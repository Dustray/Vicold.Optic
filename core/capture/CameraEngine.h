#pragma once
// M1 采集引擎：预览（直写 Surface）+ 拍摄按钮/控制文件触发 + RAW（ring/once）
// + ZSL 快门回溯 + 断流重连。
// 触发源（两路合一，均过 triggerBurst() 的每启动配额闸门）：
//   1. 拍摄按钮（optic.cpp 经 setTriggerPoll 注入轮询）
//   2. controls.txt（过渡通道；一次性命令自消费，见 pollControls）
// 持久键：ae iso exp_us af focus_d awb zoom raw_mode save_quota fps_log
// 一次性键：zsl_shutter=N shot_raw=1

#include <android/native_window.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/capture/CameraDevice.h"
#include "core/capture/CaptureSession.h"
#include "core/capture/CaptureSettings.h"
#include "core/capture/RawCapture.h"
#include "core/util/ControlFile.h"

namespace optic::capture {

class CameraEngine {
public:
    CameraEngine() = default;
    ~CameraEngine() { stop(); }

    bool start(ANativeWindow* window, const std::string& dataDir);
    void stop();

    bool running() const { return running_.load(std::memory_order_acquire); }

    // 拍摄按钮状态轮询（optic.cpp 注入）
    void setTriggerPoll(std::function<bool()> f) { shutterPressed_ = std::move(f); }

private:
    void run(ANativeWindow* window, std::string dataDir);
    bool openSession(ANativeWindow* window);
    void closeSession();
    void pollControls();
    void pollShutterButton();
    void triggerBurst();
    void applyControl(const std::string& k, const std::string& v, bool& changed, bool& rebuild);
    std::vector<ANativeWindow*> sessionTargets() const;
    void onFrameResult(const FrameResult& r);
    bool pickPreviewSize(int32_t* w, int32_t* h);

    CameraDevice cam_;
    std::unique_ptr<CaptureSession> session_;
    std::unique_ptr<RawCapture> raw_;
    CaptureSettings settings_;
    std::string dataDir_;
    ANativeWindow* preview_ = nullptr;
    int32_t previewW_ = 0, previewH_ = 0;
    bool rawInRepeating_ = true;

    // 每启动拍摄配额（硬盘保护；save_quota=N 可放宽，0=禁用）
    int saveQuota_ = 1;
    int savesUsed_ = 0;

    std::function<bool()> shutterPressed_;
    bool lastShutterPressed_ = false;
    std::chrono::steady_clock::time_point lastTriggerAt_{};

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> deviceLost_{false};
    int reconnects_ = 0;

    uint64_t frameCount_ = 0;
    int64_t firstTs_ = 0, lastTs_ = 0;
    bool forceLog_ = false;
};

} // namespace optic::capture
