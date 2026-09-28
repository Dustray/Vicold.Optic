#include "core/capture/CaptureSession.h"

#include "core/util/Log.h"

#include <algorithm>

namespace optic::capture {
namespace {

FrameResult parseResult(const ACameraMetadata* result) {
    FrameResult out;
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_TIMESTAMP, &e) == ACAMERA_OK && e.count > 0)
        out.timestampNs = e.data.i64[0];
    if (ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_SENSITIVITY, &e) == ACAMERA_OK && e.count > 0)
        out.iso = e.data.i32[0];
    if (ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_EXPOSURE_TIME, &e) == ACAMERA_OK && e.count > 0)
        out.exposureNs = e.data.i64[0];
    if (ACameraMetadata_getConstEntry(result, ACAMERA_CONTROL_ZOOM_RATIO, &e) == ACAMERA_OK && e.count > 0)
        out.zoomRatio = e.data.f[0];
    return out;
}

} // namespace

CaptureSession::~CaptureSession() { close(); }

bool CaptureSession::create(ACameraDevice* dev, const std::vector<ANativeWindow*>& outputs) {
    std::lock_guard<std::mutex> lock(mutex_);
    closeLocked();
    device_ = dev;
    if (!dev || outputs.empty()) return false;

    if (ACaptureSessionOutputContainer_create(&container_) != ACAMERA_OK) return false;
    for (ANativeWindow* w : outputs) {
        ACaptureSessionOutput* out = nullptr;
        if (ACaptureSessionOutput_create(w, &out) != ACAMERA_OK || !out) {
            LOGE("session output create failed");
            return false;
        }
        if (ACaptureSessionOutputContainer_add(container_, out) != ACAMERA_OK) {
            ACaptureSessionOutput_free(out);
            LOGE("session output add failed");
            return false;
        }
        outputs_.push_back(out);
    }

    sessCbs_ = {this, &CaptureSession::onSessionClosed, &CaptureSession::onSessionReady,
                &CaptureSession::onSessionActive};
    if (ACameraDevice_createCaptureSession(dev, container_, &sessCbs_, &session_) != ACAMERA_OK ||
        !session_) {
        LOGE("createCaptureSession failed");
        return false;
    }

    capCbs_ = {this, nullptr, nullptr, &CaptureSession::onCaptureCompleted,
               &CaptureSession::onCaptureFailed, nullptr, nullptr, nullptr};
    LOGI("capture session created (%zu outputs)", outputs.size());
    return true;
}

void CaptureSession::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closeLocked();
}

void CaptureSession::closeLocked() {
    if (session_) {
        ACameraCaptureSession_stopRepeating(session_);
        ACameraCaptureSession_close(session_);
        session_ = nullptr;
    }
    if (repeating_) { ACaptureRequest_free(repeating_); repeating_ = nullptr; }
    for (auto* t : repeatingTgts_) ACameraOutputTarget_free(t);
    repeatingTgts_.clear();
    repeatingWins_.clear();
    if (onceReq_) { ACaptureRequest_free(onceReq_); onceReq_ = nullptr; }
    for (auto* t : onceTgts_) ACameraOutputTarget_free(t);
    onceTgts_.clear();
    for (auto* o : outputs_) ACaptureSessionOutput_free(o);
    outputs_.clear();
    if (container_) {
        ACaptureSessionOutputContainer_free(container_);
        container_ = nullptr;
    }
}

bool CaptureSession::setRepeating(const std::vector<ANativeWindow*>& targets,
                                  const CaptureSettings& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!session_ || targets.empty()) return false;

    if (targets != repeatingWins_ || !repeating_) {
        // 目标集变化 ⇒ 重建请求（NDK 无 removeTarget）
        if (repeating_) { ACaptureRequest_free(repeating_); repeating_ = nullptr; }
        for (auto* t : repeatingTgts_) ACameraOutputTarget_free(t);
        repeatingTgts_.clear();
        if (ACameraDevice_createCaptureRequest(device_, TEMPLATE_PREVIEW, &repeating_) != ACAMERA_OK) {
            LOGE("createCaptureRequest(repeating) failed");
            return false;
        }
        for (ANativeWindow* w : targets) {
            ACameraOutputTarget* t = nullptr;
            if (ACameraOutputTarget_create(w, &t) != ACAMERA_OK || !t) {
                LOGE("output target create failed");
                return false;
            }
            repeatingTgts_.push_back(t);
            ACaptureRequest_addTarget(repeating_, t);
        }
        repeatingWins_ = targets;
    }

    applySettings(repeating_, s);
    int seqId = 0;
    ACaptureRequest* reqArr[1] = {repeating_};
    if (ACameraCaptureSession_setRepeatingRequest(session_, &capCbs_, 1, reqArr, &seqId) !=
        ACAMERA_OK) {
        LOGE("setRepeatingRequest failed");
        return false;
    }
    return true;
}

bool CaptureSession::captureOnce(const std::vector<ANativeWindow*>& targets,
                                 const CaptureSettings& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!session_ || targets.empty()) return false;
    if (onceReq_) {
        LOGW("captureOnce skipped: previous request in flight");
        return false;
    }
    if (ACameraDevice_createCaptureRequest(device_, TEMPLATE_STILL_CAPTURE, &onceReq_) != ACAMERA_OK)
        return false;
    for (ANativeWindow* w : targets) {
        ACameraOutputTarget* t = nullptr;
        if (ACameraOutputTarget_create(w, &t) != ACAMERA_OK || !t) return false;
        onceTgts_.push_back(t);
        ACaptureRequest_addTarget(onceReq_, t);
    }
    applySettings(onceReq_, s);

    int seqId = 0;
    ACaptureRequest* reqArr[1] = {onceReq_};
    if (ACameraCaptureSession_capture(session_, &capCbs_, 1, reqArr, &seqId) != ACAMERA_OK) {
        LOGE("capture(once) failed");
        return false;
    }
    return true;
}

void CaptureSession::applySettings(ACaptureRequest* req, const CaptureSettings& s) const {
    s.apply(req);
}

void CaptureSession::onSessionClosed(void*, ACameraCaptureSession*) {}
void CaptureSession::onSessionReady(void*, ACameraCaptureSession*) {}
void CaptureSession::onSessionActive(void*, ACameraCaptureSession*) {}

void CaptureSession::onCaptureCompleted(void* ctx, ACameraCaptureSession*, ACaptureRequest* req,
                                        const ACameraMetadata* result) {
    auto* self = static_cast<CaptureSession*>(ctx);
    FrameResult fr = parseResult(result);
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        if (self->onceReq_ && req == self->onceReq_) {
            // 单拍请求完成，释放给下一次 captureOnce
            ACaptureRequest_free(self->onceReq_);
            self->onceReq_ = nullptr;
            for (auto* t : self->onceTgts_) ACameraOutputTarget_free(t);
            self->onceTgts_.clear();
        }
    }
    if (self->onFrameResult) self->onFrameResult(fr);
}

void CaptureSession::onCaptureFailed(void* ctx, ACameraCaptureSession*, ACaptureRequest*,
                                     ACameraCaptureFailure* failure) {
    auto* self = static_cast<CaptureSession*>(ctx);
    if (self->onFrameFailed) self->onFrameFailed(failure->reason);
}

} // namespace optic::capture
