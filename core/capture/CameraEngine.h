#pragma once
// M1/M-UI 采集引擎：GL 预览（core/ui 渲染器）+ 拍摄/控制命令消费 + RAW（ring/once）
// + ZSL 快门回溯 + 断流重连。
// 触发/控制源（统一经 triggerBurst()/applyControl 的配额闸门）：
//   1. GL UI（core/ui/Ui 命令队列，setUi 注入）
//   2. controls.txt（过渡通道；一次性命令自消费，见 pollControls）
// 持久键：ae iso exp_us ss ev af focus_d awb zoom raw_mode save_quota uvrot fps_log disp
// 一次性键：zsl_shutter=N shot_raw=1

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/capture/CameraDevice.h"
#include "core/capture/CaptureSession.h"
#include "core/capture/CaptureSettings.h"
#include "core/capture/RawCapture.h"
#include "core/util/ControlFile.h"

namespace optic::ui {
class Ui;
}

namespace optic::capture {

class CameraEngine {
public:
    CameraEngine() = default;
    ~CameraEngine() { stop(); }

    bool start(const std::string& dataDir);   // 预览窗由 core/ui 提供
    void stop();

    bool running() const { return running_.load(std::memory_order_acquire); }

    // GL UI（optic.cpp 注入；引擎线程消费其命令队列，预览窗挂会话输出）
    void setUi(ui::Ui* u) { ui_ = u; }

private:
    void run(std::string dataDir);
    bool openSession();
    void closeSession();
    bool rebuildSession();              // 按当前模式（逻辑/超广角物理/长焦物理）重建会话
    void retireSession();               // 旧会话 close 后进墓地（延迟析构防回调 UAF）
    void reapRetired();                 // 回收超过宽限期的退役会话
    void commitSession(bool settingsChanged);  // ALL 常驻：设置变化只改 entry 重发；单流降级：模式变化→重建
    CaptureSettings effSettings() const;       // ALL 请求的逻辑流 zoom 钳在干净带内（防踩融合坏区）
    // 当前 zoom 应直连的物理 ID（空 = 逻辑多摄）：
    //   z ∈ [0.7, 1.0) → 超广角直连；z ≥ teleMinZoom_ → 长焦直连；其余 → 逻辑。
    // 本机逻辑融合管线两个坏区（sub-1.0 与高倍数字区），物理直连绕开（2026-09-29 真机确诊）。
    // ALL 常驻模式下仅用于决定 GL 显示源（请求不再随带切换）。
    std::string activePhysId() const;
    // 当前实际所处带（= uwPhysId_ / telePhysId_ / 空），滞回判定基准（activePhysId 读）。
    // 每次 commit/rebuild 后更新。滞回防边界抖动：uw 退出需 z ≥ 1.03，
    // tele 退出需 z ≥ teleMinZoom_-0.08 —— 否则导轨在阈值附近微动会触发会话重建风暴。
    std::string physBand_;
    // 直连请求应写的相对数字变焦：长焦 = z/teleNativeZoom（0 = 不写，超广角恒原生 FOV）
    float physZoom() const;
    // ALL 请求逐摄相对变焦表：uw = z/0.7、tele = z/teleMin（≤1 钳 1 = 原生 FOV，
    // 超范围由 HAL 按各摄 zoomRatioRange 钳制）。三路流恒渲染同一用户 FOV，
    // 跨带切显示源时画面内容连续；三路常流还使 AE/AWB 持续收敛（切换色彩已稳定）。
    // 同时附上与 rel 恒一致的 SCALER_CROP_REGION（per-physical ZOOM_RATIO 被 HAL
    // 忽略时的兜底，见 PhysZoom 注释）。副作用：更新 relUw_/relTele_ 缓存。
    std::vector<CaptureSession::PhysZoom> allPhysZooms();
    void refreshPhysIds();   // 按覆盖键（uw_phys/tele_phys/phys_min）+ 探测值刷新物理布局缓存
    // band 签名："L"（逻辑主摄）/ "P:"+uwPhysId_（超广角直连）/ "P:"+telePhysId_（长焦直连）
    std::vector<ANativeWindow*> bandTargets(const std::string& sig) const;  // 该带 repeating 目标
    int slotFromSig(const std::string& sig) const;                          // 该带 GL 预览源槽位
    void pollControls();
    // 注意：必须复用同一实例（成员）。此前每轮新建局部 ControlFile，内容比对状态被重置，
    // 导致 controls.txt 全量重放 —— UI 刚设置的 zoom 会在 100ms 后被文件里的旧值盖回
    // （表现为"松手瞬间预览回到 1.0，导轨不动"，2026-09-29 真机确诊）。
    std::unique_ptr<util::ControlFile> ctl_;
    void drainUiCmds();
    void triggerBurst();
    void applyControl(const std::string& k, const std::string& v, bool& changed);
    void onFrameResult(const FrameResult& r);

    CameraDevice cam_;
    std::unique_ptr<CaptureSession> session_;
    std::unique_ptr<RawCapture> raw_;
    CaptureSettings settings_;
    std::string dataDir_;

    // 每启动拍摄配额（硬盘保护；save_quota=N 可放宽，0=禁用）
    // 8 次 ≈ 8×4 张 DNG ≈ 0.8GiB 上限；真机调参走 controls.txt: save_quota=N
    int saveQuota_ = 8;
    int savesUsed_ = 0;

    ui::Ui* ui_ = nullptr;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> deviceLost_{false};
    int reconnects_ = 0;

    uint64_t frameCount_ = 0;
    int64_t firstTs_ = 0, lastTs_ = 0;
    bool forceLog_ = false;
    bool uwAllowed_ = true;   // 超广角默认开启：本机超广角已改为直连物理摄像头，不再走损坏的融合管线
    std::string forcedCamId_;   // controls.txt cam=N：强制打开指定摄像头 ID（诊断用，绕过逻辑多摄切换）
    std::string forcedUwPhys_;  // controls.txt uw_phys=N：强制指定超广角物理摄像头 ID（诊断用）
    std::string forcedTelePhys_;// controls.txt tele_phys=N：强制指定长焦物理摄像头 ID（诊断用）
    float forcedPhysMin_ = 0.f; // controls.txt phys_min=N：强制长焦直连起始倍率（0=用自动值，诊断用）
    float forcedTeleNative_ = 0.f; // controls.txt tele_native=N：强制长焦带基 az（肉眼校准用）
    std::string uwPhysId_;      // 当前生效的超广角物理 ID（forcedUwPhys_ > 自动探测 > 回退 "2"）
    std::string telePhysId_;    // 当前生效的长焦物理 ID（forcedTelePhys_ > 自动探测）
    float teleMinZoom_ = 1e9f;  // 长焦原生倍率（= teleNativeZoom，长焦带 crop 的换算带基）
    // 长焦接管点：逻辑流能健康出帧的最大倍率（本机实测 4.8 干净、5.0 起持续断流 ——
    // 2026-09-30）。接管点必须在安全区内，否则用户拖到 5x 时逻辑流已在断流：
    // 画面先冻结、再随长焦接管瞬间跳变（用户所见「5x 瞬间放大好多」）。
    static constexpr float kLogicalSafeMax = 4.85f;
    // 用户口径的长焦起点（与系统相机一致：≥5x 用长焦）；必须 ≥ 逻辑流安全上限，
    // 否则逻辑流会被请求到断流区。
    static constexpr float kTeleSwitchUser = 5.0f;
    float teleSwitch_ = 1e9f;   // = min(teleMinZoom_, kTeleSwitchUser)：实际接管点
    // 近距推迟接管（对齐系统相机行为：对焦距离过近时长焦对不上焦，1–20x 恒主摄）：
    // 依据逻辑流 result 的 LENS_FOCUS_DISTANCE 判定被摄距离，滞回 + 帧数去抖。
    static constexpr float kNearEnterM = 0.9f;   // <0.9m 进入近距（接管点 → teleNearSwitch_）
    static constexpr float kNearExitM = 1.4f;    // >1.4m 退出近距（恢复 5x 接管）
    static constexpr int kNearDebounce = 12;     // ~0.4s @30fps
    float teleNearSwitch_ = 20.f;                // controls.txt tele_near=N 可调（0=关）
    float nearEnterM_ = kNearEnterM;             // controls.txt near_m=N 可调（0=关距离判定）
    float uwNativeZoom_ = 0.7f; // 超广角光学倍率 f(uw)/f(main)（真机 0.388；探测失败回退 0.7）
                                // —— FOV 换算基准，与导轨下限 0.7 无关，混用会让超广/主摄衔接错位
    bool sessionIsPhysical_ = false;  // 当前会话是否为物理直连
    std::string sessionSig_;          // 当前 active band（"L"/"P:cameraId"，见 bandTargets）
    // 多流常驻会话（M-MC）：逻辑 + uw + tele 三路预览输出一次 configure，跨带只切换
    // repeating 请求（无 endConfigure ~290ms 冻结、纹理常驻无黑帧）。
    // HAL 拒绝组合时降级单流（multiStreamFailed_ 记忆，跨带回退为会话重建）。
    bool multiStream_ = false;
    bool multiStreamFailed_ = false;
    bool lastRawRing_ = false;        // 检测 RAW 进出 repeating（ALL 请求目标集变化 → 重建）
    // ALL 请求逐摄写入的相对变焦缓存（onFrameResult 的 appliedZoom 回传换算用）
    float relUw_ = 0.f, relTele_ = 0.f;
    int lastDispSlot_ = -1;     // 上次下发的 GL 显示 slot（变化才打日志）
    bool manualDisp_ = false;   // disp 诊断键手动锁定显示源（分带变化时才交还自动）
    // 近距状态机（onFrameResult 回调线程更新；翻转时置 bandDirty_ 让引擎线程重发请求）
    bool nearSubject_ = false;
    int nearCnt_ = 0, farCnt_ = 0;
    std::atomic<bool> bandDirty_{false};

    // 当前生效的长焦接管点：近距时推迟（长焦模组最小对焦距离之外，与系统相机一致）
    float teleSwitchEff() const {
        return (nearSubject_ && teleNearSwitch_ > 1.f) ? teleNearSwitch_ : teleSwitch_;
    }

    // 物理摄逐键变焦能力快照（probePhysCaps，rebuildSession 时刷新）：ALL 带内连续变焦
    // 依赖 per-physical ZOOM_RATIO 生效；部分 HAL 不支持/忽略该键（表现为镜头固定在
    // 原生焦段，2026-09-29 真机症状），届时按 cropKey 决定回退方案。
    struct PhysCaps {
        bool zoomKey = false, cropKey = false;
        float zmin = 1.f, zmax = 1.f;   // CONTROL_ZOOM_RATIO_RANGE
        int32_t aa[4] = {};             // SENSOR_INFO_ACTIVE_ARRAY_SIZE (x, y, w, h)
    };
    std::map<std::string, PhysCaps> physCaps_;
    void probePhysCaps();

    // 退役会话墓地：重建时旧会话立即 close（停止回调流），但对象延迟 1.5s 才析构。
    // 框架回调线程（C2N-dev-looper）在 close 返回后仍可能携 in-flight 回调访问
    // capCbs_/mutex_；立即析构会 UAF（2026-09-29 真机 SIGABRT：
    // "pthread_mutex_lock called on a destroyed mutex"）。
    struct RetiredSession {
        std::unique_ptr<CaptureSession> sess;
        int64_t retiredMs = 0;
    };
    std::vector<RetiredSession> retired_;

    // 预览存活看门狗：repeating 被 HAL 静默丢弃（本机逻辑多摄切超广角时偶发）时
    // 自动重发 setRepeating 尝试恢复。lastResultMs_ 记录最近一次 capture result 时刻。
    int64_t lastResultMs_ = 0;
    int64_t nowMs() const;
    int stallRetries_ = 0;

    // 帧节奏探针：相邻 result 间隔 >70ms（30fps 标称 33ms）计一次"节奏断裂"。
    // 用于区分卡顿来源：带内 setRepeating 重发（间歇性小断裂）vs 跨带会话重建
    // （一次大断裂数百 ms）。
    int frameGaps_ = 0;
    int64_t maxGapMs_ = 0;
    int64_t lastFrameMs_ = 0;
};

} // namespace optic::capture
