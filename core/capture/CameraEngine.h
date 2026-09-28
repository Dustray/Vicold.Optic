#pragma once
// M0 采集引擎：打开相机 → 直出预览（相机帧直接写入 Surface，零拷贝路径）
// 并把 CameraCharacteristics 逐项打点（logcat + 外部文件），供《17 Pro 能力报告》使用。

#include <android/native_window.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace optic::capture {

class CameraEngine {
public:
    CameraEngine();
    ~CameraEngine();

    // 在相机线程执行完整流程：探测 → 打开 → 会话 → 预览循环。
    // reportDir 为 app 外部文件目录（ANativeActivity::externalDataPath）。
    bool start(ANativeWindow* window, const std::string& reportDir);
    void stop();

    bool running() const { return running_.load(std::memory_order_acquire); }

    // 回调需要访问内部状态；对外不透明
    struct Impl;

private:
    void run(ANativeWindow* window, std::string reportDir);

    std::unique_ptr<Impl> impl_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
};

} // namespace optic::capture
