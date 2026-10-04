#pragma once
// M1.1：会话与请求的 RAII 封装。
// 设计约束（刻意从简）：
//  - 目标集（ANativeWindow 集合）变化 ⇒ 重建 repeating 请求（NDK 无 removeTarget）
//  - 设置变化 ⇒ 复用请求改 entry 后重发 repeating
//  - 单拍串行（同一时刻至多一个 captureOnce 在途，引擎保证节奏）
// 多带（M-MC）：会话常驻多路输出（逻辑预览 + uw/tele 物理直连 + RAW），
// repeating 请求按 band 签名缓存（"L"/"P:3"/"P:4"）—— 跨带切换只换 repeating
// 请求，不重配会话（endConfigure ~290ms 冻结由此消除）。
// 线程模型：create/setRepeating/captureOnce 仅由引擎相机线程调用；
// 回调来自 binder 线程，只触达 onFrameResult/onFrameFailed。

#include <android/native_window.h>

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCaptureRequest.h>

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/capture/CaptureSettings.h"

namespace optic::capture {

struct FrameResult {
    int64_t timestampNs = 0;
    int32_t iso = 0;
    int64_t exposureNs = 0;
    float zoomRatio = 1.f;
    float focusDistanceDiopters = 0.f;  // 屈光度（0=无穷远）；AF 连续时有效
    float wbGains[4] = {1, 1, 1, 1};   // [r, gEven, gOdd, b]（COLOR_CORRECTION_GAINS）
    int32_t cropRegion[4] = {};        // SCALER_CROP_REGION（activeArray 域；w/h=0 表示未取到）
    // AF 诊断回显（触摸对焦闭环确认：HAL 到底收没收区域、扫没扫）
    int32_t afState = -1;              // CONTROL_AF_STATE（-1 = 该帧没取到）
    int32_t afRegions[5] = {};         // result 回显的 CONTROL_AF_REGIONS（== 下发值才算被接受）
    int32_t aeRegions[5] = {};         // result 回显的 CONTROL_AE_REGIONS
    std::string physicalId;             // 当前主源物理摄像头（ACTIVE_PHYSICAL_ID，诊断用）
                                        // NDK 单帧单回调只交付逻辑融合结果，其 ACTIVE_PHYSICAL_ID
                                        // 标识当前 backing 摄像头（"2"主/"3"超广/"4"长焦）；
                                        // 非"哪路输出"标识（NDK 无按输出分发回调），不用于分流。
};

class CaptureSession {
public:
    CaptureSession() = default;
    ~CaptureSession();
    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    // 会话输出描述：physId 非空 = ACaptureSessionPhysicalOutput（直连该物理摄像头）
    struct OutDesc {
        ANativeWindow* win = nullptr;
        const char* physId = nullptr;   // nullptr = 逻辑输出
    };
    // 一次性配置全部输出（跨带切换不再走这里 —— 那只换 repeating 请求）
    bool create(ACameraDevice* dev, const std::vector<OutDesc>& outputs);
    void close();

    // 按 band 签名切换 repeating：请求按签名缓存复用（设置变化只更新 entry）。
    // physId 非空 = 该带为物理直连（withPhysicalIds 建请求，skipZoom + 可选相对变焦）。
    bool setRepeating(const std::string& band, const std::vector<ANativeWindow*>& targets,
                      const CaptureSettings& s, const std::string& physId, float physZoom = 0.f);
    // 方案 A（全目标常驻）：一个 repeating 请求同时驱动全部 targets（三路预览 + RAW）。
    // 逻辑流 zoom 由 s.zoomRatio 给出（调用方钳在干净带内）。请求按 "ALL" 缓存，
    // 设置/变焦变化只改 entry 重发 —— 跨带、带内全部 0 间隔。
    struct PhysZoom {
        std::string id;
        float rel = 1.f;        // 相对变焦（该摄 zoomRatio 域，1.0 = 原生 FOV）
        int32_t crop[4] = {};   // 与 rel 一致的 SCALER_CROP_REGION（activeArray 域；
                                // crop[2]==0 = 不写）。ZOOM_RATIO 被声明但被 HAL 忽略时
                                //（2026-09-29 真机症状）由 crop region 同义兜底，二者恒一致。
        // 三镜头曝光统一（2026-09-30）：逻辑请求的 AE/SENSOR 键只约束融合的活动物理摄，
        // 非活动物理流各跑各的 AE → 超广/长焦亮度与主摄不一致，跨带切换曝光跳变。
        // 这组键恒逐摄同步下发（请求必须 withPhysicalIds 声明物理成员）。
        bool aeOn = true;
        bool aeLock = false;        // 测光锁定逐摄同步（否则非活动物理摄 AE 不锁）
        int32_t evSteps = 0;
        int32_t iso = 0;            // aeOn=false 时生效（>0 才写）
        int64_t exposureNs = 0;     // aeOn=false 时生效（>0 才写）
        bool awbOn = true;
    };
    bool setRepeatingAll(const std::vector<ANativeWindow*>& targets, const CaptureSettings& s,
                         const std::vector<PhysZoom>& phys);
    bool captureOnce(const std::vector<ANativeWindow*>& targets, const CaptureSettings& s);

    // 自动休眠：仅停止 repeating 请求（保留会话、纹理、输出目标），不 close。
    // 唤醒时由调用方重新 setRepeating 即可恢复预览流；比 close 重建（~290ms 冻结 +
    // 纹理失效）省电且瞬启。stopRepeating 后不再有 capture result 流入（相机 ISP 停跑）。
    void stopRepeating();

    // 一次性 AF 触发请求（触摸对焦专用）：带 AF_REGIONS + AF_TRIGGER_START。
    // 必须与 repeating 分开提交 —— camera2 里 trigger 是"每个请求实例执行一次"的语义，
    // 写在 repeating 上会让 HAL 每帧重启一次扫描，镜头永远合不上焦。
    // **targets 必须挂成与 repeating 完全一致**：本机 CamX 对目标集合变化极敏感
    //（2026-10-02 实测：只挂一个 target 的 trigger 请求会让流断 ~500ms，点按后
    // 画面先卡半秒才开始起扫 —— 用户感知的"延迟"就来自这里）。
    // 请求由 sequenceId 事件回收（与 captureOnce 互不干扰，可并发在途）。
    bool captureTrigger(const std::vector<ANativeWindow*>& targets, const CaptureSettings& s);

    std::function<void(const FrameResult&)> onFrameResult;
    std::function<void(int reason)> onFrameFailed;

    bool valid() const { return session_ != nullptr; }

private:
    // NDK 回调（静态）
    static void onSessionClosed(void*, ACameraCaptureSession*);
    static void onSessionReady(void*, ACameraCaptureSession*);
    static void onSessionActive(void*, ACameraCaptureSession*);
    static void onCaptureCompleted(void* ctx, ACameraCaptureSession*, ACaptureRequest*,
                                   const ACameraMetadata* result);
    static void onCaptureFailed(void* ctx, ACameraCaptureSession*, ACaptureRequest*,
                                ACameraCaptureFailure* failure);
    static void onSequenceCompleted(void* ctx, ACameraCaptureSession*, int sequenceId,
                                    int64_t frameNumber);

    // skipZoom=true 时物理直连不写用户 zoomRatio（物理镜头以自身原生 FOV 出图）；
    // physZoom>0 时改写该相对数字变焦（长焦直连的 z/teleNative，HAL 侧自行钳制）。
    void applySettings(ACaptureRequest* req, const CaptureSettings& s, bool skipZoom = false,
                       float physZoom = 0.f, bool forPreview = true) const;
    void closeLocked(); // mutex_ 已持有时使用（create 复用）

    // 单个 band 的 repeating 请求（按签名缓存）
    struct BandReq {
        ACaptureRequest* req = nullptr;
        std::vector<ANativeWindow*> wins;
        std::vector<ACameraOutputTarget*> tgts;
        std::string physId;             // 非空 = withPhysicalIds 请求
        std::string declaredIds;        // ALL 请求声明的物理成员集（变化 → 强制重建请求）
    };

    ACameraDevice* device_ = nullptr;
    ACameraCaptureSession* session_ = nullptr;
    float lastAllZoom_ = 0.f;           // 上次 ALL 重发的逻辑 zoom（日志去抖）
    ACaptureSessionOutputContainer* container_ = nullptr;
    std::vector<ACaptureSessionOutput*> outputs_;
    std::map<std::string, BandReq> bands_;

    // 单拍：串行复用一个请求（释放以 sequenceId 匹配的 onSequenceCompleted 为准）
    ACaptureRequest* onceReq_ = nullptr;
    int onceSeq_ = -1;
    std::vector<ANativeWindow*> onceWins_;
    std::vector<ACameraOutputTarget*> onceTgts_;

    // AF 触发请求：seqId → (请求, 输出目标)，等 onSequenceCompleted 回收（请求复用会
    // 残留 AF_TRIGGER_START，导致后续 repeating 每帧重扫）
    struct TrigReq {
        ACaptureRequest* req = nullptr;
        std::vector<ACameraOutputTarget*> tgts;
    };
    std::map<int, TrigReq> trigs_;

    ACameraCaptureSession_stateCallbacks sessCbs_{};
    ACameraCaptureSession_captureCallbacks capCbs_{};
    std::mutex mutex_;
};

} // namespace optic::capture
