#pragma once
// M1.1：会话与请求的 RAII 封装。
// 设计约束（刻意从简）：
//  - 目标集（ANativeWindow 集合）变化 ⇒ 重建 repeating 请求（NDK 无 removeTarget）
//  - 设置变化 ⇒ 复用请求改 entry 后重发 repeating
//  - 单拍串行（同一时刻至多一个 captureOnce 在途，引擎保证节奏）
// 线程模型：create/setRepeating/captureOnce 仅由引擎相机线程调用；
// 回调来自 binder 线程，只触达 onFrameResult/onFrameFailed。

#include <android/native_window.h>

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCaptureRequest.h>

#include <functional>
#include <mutex>
#include <vector>

#include "core/capture/CaptureSettings.h"

namespace optic::capture {

struct FrameResult {
    int64_t timestampNs = 0;
    int32_t iso = 0;
    int64_t exposureNs = 0;
    float zoomRatio = 1.f;
};

class CaptureSession {
public:
    CaptureSession() = default;
    ~CaptureSession();
    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    bool create(ACameraDevice* dev, const std::vector<ANativeWindow*>& outputs);
    void close();

    bool setRepeating(const std::vector<ANativeWindow*>& targets, const CaptureSettings& s);
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

    void applySettings(ACaptureRequest* req, const CaptureSettings& s) const;
    void closeLocked(); // mutex_ 已持有时使用（create 复用）

    ACameraDevice* device_ = nullptr;
    ACameraCaptureSession* session_ = nullptr;
    ACaptureSessionOutputContainer* container_ = nullptr;
    std::vector<ACaptureSessionOutput*> outputs_;

    // repeating：目标集不变时复用请求，仅更新 entry
    ACaptureRequest* repeating_ = nullptr;
    std::vector<ANativeWindow*> repeatingWins_;
    std::vector<ACameraOutputTarget*> repeatingTgts_;

    // 单拍：串行复用一个请求
    ACaptureRequest* onceReq_ = nullptr;
    std::vector<ANativeWindow*> onceWins_;
    std::vector<ACameraOutputTarget*> onceTgts_;

    ACameraCaptureSession_stateCallbacks sessCbs_{};
    ACameraCaptureSession_captureCallbacks capCbs_{};
    std::mutex mutex_;
};

} // namespace optic::capture
