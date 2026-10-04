#include "core/capture/CaptureSession.h"

#include "core/util/Log.h"

#include <algorithm>
#include <string>

#include <camera/NdkCameraMetadataTags.h>

namespace optic::capture {
namespace {

// 读出该 capture result 的 ACTIVE_PHYSICAL_ID：NDK 的 onCaptureCompleted 每个 repeating 帧
// 只回调**一次**，交付的是逻辑融合结果；其 ACTIVE_PHYSICAL_ID 标识当前主源物理摄像头
//（"2"=主摄 / "3"=超广 / "4"=长焦），仅作诊断，不用于区分"哪路输出"（NDK 单帧单结果，
// 不存在按输出分发的 per-physical 回调）。注意标签名是 ACTIVE_PHYSICAL_ID（复数 PHYSICAL_IDS
// 是 characteristics 用的），单数 PHYSICAL_ID 在 NDK 中不存在、会编译失败。
// 物理 ID 以字节串（ASCII）形式存储，稳妥起见同时兼容 int 型（罕见 HAL）。
std::string parsePhysicalId(const ACameraMetadata* result) {
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(result, ACAMERA_LOGICAL_MULTI_CAMERA_ACTIVE_PHYSICAL_ID,
                                      &e) != ACAMERA_OK ||
        e.count == 0)
        return "";
    if (e.type == ACAMERA_TYPE_BYTE) {
        std::string s(reinterpret_cast<const char*>(e.data.u8), e.count);
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }
    return std::to_string(e.data.i32[0]);
}

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
    // 裁切区域（漂移诊断：HAL 变焦裁切中心是否随 zoom 偏离阵列中心）
    if (ACameraMetadata_getConstEntry(result, ACAMERA_SCALER_CROP_REGION, &e) == ACAMERA_OK &&
        e.count >= 4)
        for (int i = 0; i < 4; ++i) out.cropRegion[i] = e.data.i32[i];
    // 对焦距离（屈光度，0=无穷远）→ 近距判定（长焦最小对焦距离之内时推迟接管点）
    if (ACameraMetadata_getConstEntry(result, ACAMERA_LENS_FOCUS_DISTANCE, &e) == ACAMERA_OK &&
        e.count > 0)
        out.focusDistanceDiopters = e.data.f[0];
    if (ACameraMetadata_getConstEntry(result, ACAMERA_COLOR_CORRECTION_GAINS, &e) == ACAMERA_OK && e.count >= 4) {
        out.wbGains[0] = e.data.f[0];
        out.wbGains[1] = e.data.f[1];
        out.wbGains[2] = e.data.f[2];
        out.wbGains[3] = e.data.f[3];
    }
    // AF 回显：result 里的 CONTROL_AF_REGIONS 是 HAL「我实际用上的区域」，与下发值比对
    // 即可判定区域是被接受、被钳制还是被丢弃（丢弃 = 恒为默认/全零/无该标签）。
    if (ACameraMetadata_getConstEntry(result, ACAMERA_CONTROL_AF_REGIONS, &e) == ACAMERA_OK)
        for (int i = 0; i < 5 && i < (int)e.count; ++i) out.afRegions[i] = e.data.i32[i];
    if (ACameraMetadata_getConstEntry(result, ACAMERA_CONTROL_AE_REGIONS, &e) == ACAMERA_OK)
        for (int i = 0; i < 5 && i < (int)e.count; ++i) out.aeRegions[i] = e.data.i32[i];
    if (ACameraMetadata_getConstEntry(result, ACAMERA_CONTROL_AF_STATE, &e) == ACAMERA_OK &&
        e.count > 0)
        out.afState = e.data.u8[0];
    // AE/FLASH 状态（闪光 precapture 诊断：HAL 是否真跑了预闪序列）
    if (ACameraMetadata_getConstEntry(result, ACAMERA_CONTROL_AE_STATE, &e) == ACAMERA_OK &&
        e.count > 0)
        out.aeState = e.data.u8[0];
    if (ACameraMetadata_getConstEntry(result, ACAMERA_FLASH_STATE, &e) == ACAMERA_OK && e.count > 0)
        out.flashState = e.data.u8[0];
    out.physicalId = parsePhysicalId(result);
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
               &CaptureSession::onCaptureFailed, &CaptureSession::onSequenceCompleted,
               nullptr, nullptr};
    LOGI("capture session created (%zu outputs)", outputs_.size());
    return true;
}

void CaptureSession::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closeLocked();
}

void CaptureSession::stopRepeating() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_) ACameraCaptureSession_stopRepeating(session_);
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
    // AF 触发请求：会话关闭后其 sequenceCompleted 不会再来（且 session_ 已停止），
    // 这里必须兜底回收，否则进程生命周期内泄漏。
    for (auto& [seq, tr] : trigs_) {
        if (tr.req) ACaptureRequest_free(tr.req);
        for (auto* p : tr.tgts) ACameraOutputTarget_free(p);
    }
    trigs_.clear();
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

bool CaptureSession::setRepeatingAll(const std::vector<ANativeWindow*>& targets,
                                     const CaptureSettings& s,
                                     const std::vector<PhysZoom>& phys) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!session_ || targets.empty()) return false;

    BandReq& b = bands_["ALL"];
    // 声明的物理成员集变化（如空 → [3,4]）必须重建请求：per-physical 键只能写在
    // withPhysicalIds 声明了该成员的请求上，仅重发 entry 会静默失败。
    std::string declaredIds;
    for (const auto& pz : phys) {
        declaredIds += pz.id;
        declaredIds += ';';
    }
    if (targets != b.wins || !b.req || declaredIds != b.declaredIds) {
        // 目标集变化（或首次）⇒ 重建请求：withPhysicalIds 声明物理流成员，
        // 逻辑流输出（RAW）照常交付 —— RAW 恒出帧，全带可拍。
        if (b.req) { ACaptureRequest_free(b.req); b.req = nullptr; }
        for (auto* t : b.tgts) ACameraOutputTarget_free(t);
        b.tgts.clear();
        b.physId.clear();
        if (!phys.empty()) {
            std::vector<const char*> ids;
            ids.reserve(phys.size());
            for (const auto& pz : phys) ids.push_back(pz.id.c_str());
            ACameraIdList plist{};
            plist.numCameras = static_cast<int>(ids.size());
            plist.cameraIds = ids.data();
            if (ACameraDevice_createCaptureRequest_withPhysicalIds(device_, TEMPLATE_PREVIEW,
                                                                   &plist, &b.req) != ACAMERA_OK ||
                !b.req) {
                LOGE("createCaptureRequest_withPhysicalIds failed (ALL, %zu phys)", ids.size());
                return false;
            }
        } else if (ACameraDevice_createCaptureRequest(device_, TEMPLATE_PREVIEW, &b.req) !=
                   ACAMERA_OK) {
            LOGE("createCaptureRequest(ALL) failed");
            return false;
        }
        for (ANativeWindow* w : targets) {
            ACameraOutputTarget* t = nullptr;
            if (ACameraOutputTarget_create(w, &t) != ACAMERA_OK || !t) {
                LOGE("output target create failed (ALL)");
                return false;
            }
            b.tgts.push_back(t);
            ACaptureRequest_addTarget(b.req, t);
        }
        b.wins = targets;
        b.declaredIds = declaredIds;
    }

    // 逻辑 zoom 由调用方（CameraEngine::effSettings）钳在干净带内；
    // 物理流逐摄覆盖相对变焦：ZOOM_RATIO + 同值的 SCALER_CROP_REGION（双保险）。
    // rel>1 才写 ZOOM：perKey=false 的机型（键被 HAL 忽略）恒为 1.0，不写防
    // 未来 ROM 部分生效时与 GL 裁切叠加成双重变焦。
    applySettings(b.req, s, false, 0.f);
    for (const auto& pz : phys) {
        if (pz.rel > 1.0f) {
            // 返回码必查：部分 HAL 对 per-physical 控制键静默失败。
            const camera_status_t rc = ACaptureRequest_setEntry_physicalCamera_float(
                b.req, pz.id.c_str(), ACAMERA_CONTROL_ZOOM_RATIO, 1, &pz.rel);
            if (rc != ACAMERA_OK)
                LOGW("per-phys ZOOM_RATIO set failed (phys=%s rel=%.2f rc=%d)", pz.id.c_str(),
                     pz.rel, (int)rc);
        }
        if (pz.crop[2] > 0) {
            const camera_status_t rc = ACaptureRequest_setEntry_physicalCamera_i32(
                b.req, pz.id.c_str(), ACAMERA_SCALER_CROP_REGION, 4, pz.crop);
            if (rc != ACAMERA_OK)
                LOGW("per-phys CROP_REGION set failed (phys=%s rc=%d)", pz.id.c_str(), (int)rc);
        }
        // 曝光/AWB 逐摄同步：这些键写在逻辑请求上只约束融合的活动物理摄，非活动
        // 物理流各自 AE → 三镜头亮度不统一（2026-09-30 真机确诊：主摄 obey、
        // 超广偏暗、长焦最暗），跨带切换的曝光跳变也是大闪烁来源之一。
        const uint8_t pAe = static_cast<uint8_t>(pz.aeOn ? ACAMERA_CONTROL_AE_MODE_ON
                                                         : ACAMERA_CONTROL_AE_MODE_OFF);
        const uint8_t pAwb = static_cast<uint8_t>(pz.awbOn ? ACAMERA_CONTROL_AWB_MODE_AUTO
                                                           : ACAMERA_CONTROL_AWB_MODE_OFF);
        camera_status_t rc = ACaptureRequest_setEntry_physicalCamera_u8(
            b.req, pz.id.c_str(), ACAMERA_CONTROL_AE_MODE, 1, &pAe);
        if (rc != ACAMERA_OK)
            LOGW("per-phys AE_MODE failed (phys=%s rc=%d)", pz.id.c_str(), (int)rc);
        rc = ACaptureRequest_setEntry_physicalCamera_u8(b.req, pz.id.c_str(),
                                                        ACAMERA_CONTROL_AWB_MODE, 1, &pAwb);
        if (rc != ACAMERA_OK)
            LOGW("per-phys AWB_MODE failed (phys=%s rc=%d)", pz.id.c_str(), (int)rc);
        {   // AE_LOCK 恒写两个状态：请求是复用的，解锁时不覆盖写会残留上次的 1
            const uint8_t pLock = static_cast<uint8_t>(pz.aeLock ? 1 : 0);
            rc = ACaptureRequest_setEntry_physicalCamera_u8(b.req, pz.id.c_str(),
                                                            ACAMERA_CONTROL_AE_LOCK, 1, &pLock);
            if (rc != ACAMERA_OK)
                LOGW("per-phys AE_LOCK failed (phys=%s rc=%d)", pz.id.c_str(), (int)rc);
        }
        if (pz.aeOn) {
            rc = ACaptureRequest_setEntry_physicalCamera_i32(
                b.req, pz.id.c_str(), ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, 1, &pz.evSteps);
            if (rc != ACAMERA_OK)
                LOGW("per-phys EV failed (phys=%s rc=%d)", pz.id.c_str(), (int)rc);
        } else {
            if (pz.iso > 0) {
                rc = ACaptureRequest_setEntry_physicalCamera_i32(
                    b.req, pz.id.c_str(), ACAMERA_SENSOR_SENSITIVITY, 1, &pz.iso);
                if (rc != ACAMERA_OK)
                    LOGW("per-phys ISO failed (phys=%s rc=%d)", pz.id.c_str(), (int)rc);
            }
            if (pz.exposureNs > 0) {
                rc = ACaptureRequest_setEntry_physicalCamera_i64(
                    b.req, pz.id.c_str(), ACAMERA_SENSOR_EXPOSURE_TIME, 1, &pz.exposureNs);
                if (rc != ACAMERA_OK)
                    LOGW("per-phys EXP failed (phys=%s rc=%d)", pz.id.c_str(), (int)rc);
            }
        }
    }
    // 拖拽中 ~8 次/s 重发，值变化才打日志（防淹没 logcat）
    if (s.zoomRatio != lastAllZoom_) {
        lastAllZoom_ = s.zoomRatio;
        LOGI("ALL re-issue: zoom=%.2f phys:", s.zoomRatio);
        for (const auto& pz : phys)
            LOGI("  %s rel=%.2f crop=[%d %d %d %d]", pz.id.c_str(), pz.rel, pz.crop[0],
                 pz.crop[1], pz.crop[2], pz.crop[3]);
    }
    int seqId = 0;
    ACaptureRequest* reqArr[1] = {b.req};
    if (ACameraCaptureSession_setRepeatingRequest(session_, &capCbs_, 1, reqArr, &seqId) !=
        ACAMERA_OK) {
        LOGE("setRepeatingRequest failed (band=ALL)");
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
    applySettings(onceReq_, s, false, 0.f, false);

    int seqId = 0;
    ACaptureRequest* reqArr[1] = {onceReq_};
    if (ACameraCaptureSession_capture(session_, &capCbs_, 1, reqArr, &seqId)
        != ACAMERA_OK) {
        LOGE("capture(once) failed");
        ACaptureRequest_free(onceReq_);
        onceReq_ = nullptr;
        for (auto* t : onceTgts_) ACameraOutputTarget_free(t);
        onceTgts_.clear();
        return false;
    }
    // 单拍完成以 sequenceId 事件为准（onCaptureCompleted 回调的 request 指针
    // 与提交指针不保证相等 —— NDK 文档明确，靠指针相等释放会泄漏 onceReq_，
    // 第二次 captureOnce 被"在途"跳过。2026-09-30 真机复现，M0 审查 P0 处方落地）
    onceSeq_ = seqId;
    return true;
}

// AF 触发（触摸对焦）：一次性单帧请求，只挂 win 一个输出（该 window 必须已在会话里，
// 否则 HAL 直接报错）。挂谁的无所谓 —— 我们只需要一个合法 target 让这帧能被处理。
// **绝不能复用 bands_ 里的 repeating 请求**：那会把 AF_TRIGGER_START 永久留在 repeating
// 上（camera2 的 trigger 是「每个请求实例执行一次」语义 ⇒ 每帧重启一次扫描，永不结束）。
// 请求在本次 capture 的 sequenceCompleted 事件里回收。
bool CaptureSession::captureTrigger(const std::vector<ANativeWindow*>& targets,
                                    const CaptureSettings& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!session_ || targets.empty()) return false;
    ACaptureRequest* req = nullptr;
    if (ACameraDevice_createCaptureRequest(device_, TEMPLATE_PREVIEW, &req) != ACAMERA_OK || !req) {
        LOGE("createCaptureRequest(af trigger) failed");
        return false;
    }
    TrigReq tr;
    tr.req = req;
    for (ANativeWindow* w : targets) {
        ACameraOutputTarget* t = nullptr;
        if (ACameraOutputTarget_create(w, &t) != ACAMERA_OK || !t) {
            LOGE("output target create failed (af trigger)");
            for (auto* p : tr.tgts) ACameraOutputTarget_free(p);
            ACaptureRequest_free(req);
            return false;
        }
        tr.tgts.push_back(t);
        ACaptureRequest_addTarget(req, t);
    }
    // forPreview=false：触发请求不参与帧率策略（它只跑一帧，钉 30fps 无意义）
    s.apply(req, false, false);
    int seqId = 0;
    ACaptureRequest* arr[1] = {req};
    if (ACameraCaptureSession_capture(session_, &capCbs_, 1, arr, &seqId) != ACAMERA_OK) {
        LOGE("capture(af trigger) failed");
        for (auto* p : tr.tgts) ACameraOutputTarget_free(p);
        ACaptureRequest_free(req);
        return false;
    }
    trigs_[seqId] = std::move(tr);
    // 中性措辞：本函数同时服务 AF 触发与闪光 precapture 触发（用途看 s.afTrigger/aePrecapture）
    LOGI("one-shot trigger sent (seq=%d targets=%zu af=%d aePre=%d)", seqId, targets.size(),
         s.afTrigger, s.aePrecapture);
    return true;
}

// 序列结束：sequenceId 匹配的单拍请求在此释放（成功/失败都保证回调）
void CaptureSession::onSequenceCompleted(void* ctx, ACameraCaptureSession*, int sequenceId,
                                         int64_t) {
    auto* self = static_cast<CaptureSession*>(ctx);
    std::lock_guard<std::mutex> lock(self->mutex_);
    if (self->onceSeq_ == sequenceId && self->onceReq_) {
        ACaptureRequest_free(self->onceReq_);
        self->onceReq_ = nullptr;
        self->onceSeq_ = -1;
        for (auto* t : self->onceTgts_) ACameraOutputTarget_free(t);
        self->onceTgts_.clear();
    }
    // AF 触发请求同样以 sequenceId 为准回收（必须等这次 capture 真正完成才能释放）
    auto it = self->trigs_.find(sequenceId);
    if (it != self->trigs_.end()) {
        if (it->second.req) ACaptureRequest_free(it->second.req);
        for (auto* p : it->second.tgts) ACameraOutputTarget_free(p);
        self->trigs_.erase(it);
    }
}

void CaptureSession::applySettings(ACaptureRequest* req, const CaptureSettings& s, bool skipZoom,
                                   float physZoom, bool forPreview) const {
    s.apply(req, skipZoom, forPreview);
    // 物理直连 + 相对数字变焦（>0 才写；值域按该物理镜头自身 zoomRatioRange，HAL 钳制）
    if (skipZoom && physZoom > 0.f) {
        ACaptureRequest_setEntry_float(req, ACAMERA_CONTROL_ZOOM_RATIO, 1, &physZoom);
    }
}

void CaptureSession::onSessionClosed(void*, ACameraCaptureSession*) {}
void CaptureSession::onSessionReady(void*, ACameraCaptureSession*) {}
void CaptureSession::onSessionActive(void*, ACameraCaptureSession*) {}

void CaptureSession::onCaptureCompleted(void* ctx, ACameraCaptureSession*, ACaptureRequest*,
                                        const ACameraMetadata* result) {
    auto* self = static_cast<CaptureSession*>(ctx);
    // 单拍请求的释放走 onSequenceCompleted（sequenceId 匹配，指针相等不可靠）
    if (self->onFrameResult) self->onFrameResult(parseResult(result));
}

void CaptureSession::onCaptureFailed(void* ctx, ACameraCaptureSession*, ACaptureRequest*,
                                     ACameraCaptureFailure* failure) {
    auto* self = static_cast<CaptureSession*>(ctx);
    if (self->onFrameFailed) self->onFrameFailed(failure->reason);
}

} // namespace optic::capture
