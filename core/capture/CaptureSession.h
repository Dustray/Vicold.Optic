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
    float wbGains[4] = {1, 1, 1, 1};   // [r, gEven, gOdd, b]（COLOR_CORRECTION_GAINS）
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
    // physZooms = 各物理摄的相对变焦（withPhysicalIds 建请求 + 逐摄写 ZOOM_RATIO，0 = 不写；
    // 逻辑流 zoom 由 s.zoomRatio 给出，调用方钳在干净带内）。请求按 "ALL" 缓存，
    // 设置/变焦变化只改 entry 重发 —— 跨带、带内全部 0 间隔。
    bool setRepeatingAll(const std::vector<ANativeWindow*>& targets, const CaptureSettings& s,
                         const std::vector<std::pair<std::string, float>>& physZooms);
    bool captureOnce(const std::vector<ANativeWindow*>& targets, const CaptureSettings& s);

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

    // skipZoom=true 时物理直连不写用户 zoomRatio（物理镜头以自身原生 FOV 出图）；
    // physZoom>0 时改写该相对数字变焦（长焦直连的 z/teleNative，HAL 侧自行钳制）。
    void applySettings(ACaptureRequest* req, const CaptureSettings& s, bool skipZoom = false,
                       float physZoom = 0.f) const;
    void closeLocked(); // mutex_ 已持有时使用（create 复用）

    // 单个 band 的 repeating 请求（按签名缓存）
    struct BandReq {
        ACaptureRequest* req = nullptr;
        std::vector<ANativeWindow*> wins;
        std::vector<ACameraOutputTarget*> tgts;
        std::string physId;             // 非空 = withPhysicalIds 请求
    };

    ACameraDevice* device_ = nullptr;
    ACameraCaptureSession* session_ = nullptr;
    ACaptureSessionOutputContainer* container_ = nullptr;
    std::vector<ACaptureSessionOutput*> outputs_;
    std::map<std::string, BandReq> bands_;

    // 单拍：串行复用一个请求
    ACaptureRequest* onceReq_ = nullptr;
    std::vector<ANativeWindow*> onceWins_;
    std::vector<ACameraOutputTarget*> onceTgts_;

    ACameraCaptureSession_stateCallbacks sessCbs_{};
    ACameraCaptureSession_captureCallbacks capCbs_{};
    std::mutex mutex_;
};

} // namespace optic::capture
