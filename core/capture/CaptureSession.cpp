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
    if (ACameraMetadata_getConstEntry(result, ACAMERA_COLOR_CORRECTION_GAINS, &e) == ACAMERA_OK && e.count >= 4) {
        out.wbGains[0] = e.data.f[0];
        out.wbGains[1] = e.data.f[1];
        out.wbGains[2] = e.data.f[2];
        out.wbGains[3] = e.data.f[3];
    }
    return out;
}

} // namespace

CaptureSession::~CaptureSession() { close(); }

bool CaptureSession::create(ACameraDevice* dev, const std::vector<OutDesc>& outputs) {
    std::lock_guard<std::mutex> lock(mutex_);
    closeLocked();
    device_ = dev;
    if (!dev || outputs.empty()) return false;

    if (ACaptureSessionOutputContainer_create(&container_) != ACAMERA_OK) return false;
    for (const OutDesc& od : outputs) {
        if (!od.win) continue;
        ACaptureSessionOutput* out = nullptr;
        if (od.physId) {
            // 物理直连输出：绑定到指定物理摄像头，绕过逻辑多摄融合管线
            //（本机 uw/tele 融合管线损坏，跨带也无需重配会话）
            if (ACaptureSessionPhysicalOutput_create(od.win, od.physId, &out) != ACAMERA_OK ||
                !out) {
                LOGE("physical session output create failed (phys=%s)", od.physId);
                return false;
            }
        } else {
            if (ACaptureSessionOutput_create(od.win, &out) != ACAMERA_OK || !out) {
                LOGE("session output create failed");
                return false;
            }
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
    LOGI("capture session created (%zu outputs)", outputs_.size());
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
    for (auto& [sig, b] : bands_) {
        if (b.req) ACaptureRequest_free(b.req);
        for (auto* t : b.tgts) ACameraOutputTarget_free(t);
    }
    bands_.clear();
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

bool CaptureSession::setRepeating(const std::string& band, const std::vector<ANativeWindow*>& targets,
                                  const CaptureSettings& s, const std::string& physId,
                                  float physZoom) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!session_ || targets.empty() || band.empty()) return false;

    BandReq& b = bands_[band];
    if (targets != b.wins || !b.req) {
        // 目标集变化（或首次）⇒ 重建该 band 请求（NDK 无 removeTarget）
        if (b.req) { ACaptureRequest_free(b.req); b.req = nullptr; }
        for (auto* t : b.tgts) ACameraOutputTarget_free(t);
        b.tgts.clear();
        b.physId = physId;
        if (!physId.empty()) {
            // 直连物理摄像头：用 withPhysicalIds 建请求，使 HAL 直接交付该物理镜头画面，
            // 不进入逻辑多摄融合（本机 uw/tele 融合管线已损坏）。
            ACameraIdList plist{};
            const char* pid = physId.c_str();
            plist.numCameras = 1;
            plist.cameraIds = &pid;
            if (ACameraDevice_createCaptureRequest_withPhysicalIds(device_, TEMPLATE_PREVIEW,
                                                                   &plist, &b.req) != ACAMERA_OK ||
                !b.req) {
                LOGE("createCaptureRequest_withPhysicalIds failed (band=%s phys=%s)", band.c_str(),
                     physId.c_str());
                return false;
            }
        } else {
            if (ACameraDevice_createCaptureRequest(device_, TEMPLATE_PREVIEW, &b.req) != ACAMERA_OK) {
                LOGE("createCaptureRequest(repeating) failed (band=%s)", band.c_str());
                return false;
            }
        }
        for (ANativeWindow* w : targets) {
            ACameraOutputTarget* t = nullptr;
            if (ACameraOutputTarget_create(w, &t) != ACAMERA_OK || !t) {
                LOGE("output target create failed");
                return false;
            }
            b.tgts.push_back(t);
            ACaptureRequest_addTarget(b.req, t);
        }
        b.wins = targets;
    }

    applySettings(b.req, s, !b.physId.empty(), physZoom);
    int seqId = 0;
    ACaptureRequest* reqArr[1] = {b.req};
    if (ACameraCaptureSession_setRepeatingRequest(session_, &capCbs_, 1, reqArr, &seqId) !=
        ACAMERA_OK) {
        LOGE("setRepeatingRequest failed (band=%s)", band.c_str());
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

void CaptureSession::applySettings(ACaptureRequest* req, const CaptureSettings& s, bool skipZoom,
                                   float physZoom) const {
    s.apply(req, skipZoom);
    // 物理直连 + 相对数字变焦（>0 才写；值域按该物理镜头自身 zoomRatioRange，HAL 钳制）
    if (skipZoom && physZoom > 0.f) {
        ACaptureRequest_setEntry_float(req, ACAMERA_CONTROL_ZOOM_RATIO, 1, &physZoom);
    }
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
