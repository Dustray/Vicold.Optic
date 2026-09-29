#pragma once
// M1/M-UI 采集引擎：GL 预览（core/ui 渲染器）+ 拍摄/控制命令消费 + RAW（ring/once）
// + ZSL 快门回溯 + 断流重连。
// 触发/控制源（统一经 triggerBurst()/applyControl 的配额闸门）：
//   1. GL UI（core/ui/Ui 命令队列，setUi 注入）
//   2. controls.txt（过渡通道；一次性命令自消费，见 pollControls）
// 持久键：ae iso exp_us ss ev af focus_d awb zoom raw_mode save_quota uvrot fps_log
// 一次性键：zsl_shutter=N shot_raw=1

#include <atomic>
#include <chrono>
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
    void commitSession(bool settingsChanged);  // 模式变化→重建，仅设置变化→重发 repeating
    // 当前 zoom 应直连的物理 ID（空 = 逻辑多摄）：
    //   z ∈ [0.7, 1.0) → 超广角直连；z ≥ teleMinZoom_ → 长焦直连；其余 → 逻辑。
    // 本机逻辑融合管线两个坏区（sub-1.0 与高倍数字区），物理直连绕开（2026-09-29 真机确诊）。
    std::string activePhysId() const;
    // 直连请求应写的相对数字变焦：长焦 = z/teleNativeZoom（0 = 不写，超广角恒原生 FOV）
    float physZoom() const;
    void refreshPhysIds();   // 按覆盖键（uw_phys/tele_phys/phys_min）+ 探测值刷新物理布局缓存
    void pollControls();
    // 注意：必须复用同一实例（成员）。此前每轮新建局部 ControlFile，内容比对状态被重置，
    // 导致 controls.txt 全量重放 —— UI 刚设置的 zoom 会在 100ms 后被文件里的旧值盖回
    // （表现为"松手瞬间预览回到 1.0，导轨不动"，2026-09-29 真机确诊）。
    std::unique_ptr<util::ControlFile> ctl_;
    void drainUiCmds();
    void triggerBurst();
    void applyControl(const std::string& k, const std::string& v, bool& changed);
    std::vector<ANativeWindow*> sessionTargets() const;
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
    std::string uwPhysId_;      // 当前生效的超广角物理 ID（forcedUwPhys_ > 自动探测 > 回退 "2"）
    std::string telePhysId_;    // 当前生效的长焦物理 ID（forcedTelePhys_ > 自动探测）
    float teleMinZoom_ = 1e9f;  // 长焦直连起始倍率（默认 = teleNativeZoom，即恰在 SAT 切换点前绕开融合）
    bool sessionIsPhysical_ = false;  // 当前会话是否为物理直连
    std::string sessionSig_;          // 会话组成签名（L/P:cameraId +R），用于检测是否需重建

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
};

} // namespace optic::capture
