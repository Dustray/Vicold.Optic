#include "core/capture/CameraEngine.h"

#include "core/device/DeviceRegistry.h"
#include "core/dng/DngWriter.h"
#include "core/ui/Ui.h"
#include "core/util/Log.h"

#include <camera/NdkCameraMetadata.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace std::chrono_literals;

namespace optic::capture {
namespace {

std::string propName(const char* key) {
    char buf[PROP_VALUE_MAX] = {0};
    __system_property_get(key, buf);
    return buf;
}

bool parseOn(const std::string& v) {
    return v == "on" || v == "1" || v == "true" || v == "yes";
}

} // namespace

bool CameraEngine::start(const std::string& dataDir) {
    if (running_.load(std::memory_order_acquire)) return true;
    stopFlag_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this, dataDir] { run(dataDir); });
    return true;
}

void CameraEngine::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    stopFlag_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
}

void CameraEngine::run(std::string dataDir) {
    dataDir_ = std::move(dataDir);
    ctl_ = std::make_unique<util::ControlFile>(dataDir_ + "/controls.txt",
                                               std::chrono::milliseconds(400));

    // 开机前先扫一遍 controls.txt：取出 cam= 强制摄像头 ID（诊断用，必须在 openFirstBack 之前）
    {
        std::ifstream f(dataDir_ + "/controls.txt");
        std::string line;
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "cam") { forcedCamId_ = v; break; }
        }
    }

    LOGI("engine start: device=%s market=%s registry=%s dataDir=%s cam=%s",
         propName("ro.product.device").c_str(), propName("ro.product.market.name").c_str(),
         device::currentDevice().name(), dataDir_.c_str(),
         forcedCamId_.empty() ? "(auto)" : forcedCamId_.c_str());

    while (!stopFlag_.load(std::memory_order_acquire)) {
        if (!cam_.opened()) {
            if (reconnects_ >= 5) {
                LOGE("reconnect limit reached, engine exits");
                break;
            }
            if (!openSession()) {
                closeSession();
                ++reconnects_;
                std::this_thread::sleep_for(1s);
                continue;
            }
        }

        pollControls();
        drainUiCmds();
        reapRetired();

        // 预览存活看门狗：repeating 被 HAL 静默丢弃（逻辑多摄切超广角时偶发，
        // setRepeatingRequest 返回成功却不再交付 result）时自动重发 setRepeating 恢复。
        if (session_ && ui_ && ui_->attached()) {
            const int64_t since = lastResultMs_ == 0 ? 0 : nowMs() - lastResultMs_;
            if (lastResultMs_ != 0 && since > 1200) {
                if (stallRetries_ < 12) {
                    LOGW("preview stall %lldms, re-issuing setRepeating (retry %d, zoom=%.2f)",
                         (long long)since, stallRetries_, settings_.zoomRatio);
                    if (sessionSig_ == "ALL") {
                        session_->setRepeatingAll(bandTargets("ALL"), effSettings(),
                                                  allPhysZooms());
                    } else {
                        const std::string phys = activePhysId();
                        session_->setRepeating(sessionSig_, bandTargets(sessionSig_),
                                               effSettings(), phys, physZoom());
                    }
                    ++stallRetries_;
                    lastResultMs_ = nowMs();   // 冷却，避免 100ms 内重复重发
                } else {
                    // 重试耗尽：与其永久黑屏（用户感知"卡死"），不如整链路重连
                    // （closeSession 含 cam_.close()，openSession 重建一切）
                    LOGE("preview dead after %d retries, reconnecting camera", stallRetries_);
                    stallRetries_ = 0;
                    lastResultMs_ = 0;
                    closeSession();
                    ++reconnects_;
                }
            } else if (stallRetries_ > 0) {
                LOGI("preview recovered after %d retries", stallRetries_);
                stallRetries_ = 0;
            }
        }

        if (deviceLost_.exchange(false, std::memory_order_acq_rel)) {
            LOGW("device lost -> reconnect");
            closeSession();
            ++reconnects_;
            continue;
        }

        // 50ms 拾取节拍：UI 命令（拖拽实时变焦）最坏延迟 = UI 节流 120ms + 本轮 50ms，
        // 平均 ~95ms；其余轮内工作（看门狗/墓地回收）都很轻，不影响功耗。
        std::this_thread::sleep_for(50ms);
    }
    closeSession();
    LOGI("engine exit");
}

bool CameraEngine::openSession() {
    cam_.onLost = [this] { deviceLost_.store(true, std::memory_order_release); };
    cam_.onDeviceError = [this](int) { deviceLost_.store(true, std::memory_order_release); };

    if (!cam_.openFirstBack(forcedCamId_)) return false;
    const auto& t = cam_.traits();

    // HUD 显示真实 RAW 分辨率（由机型探测结果下发，不写死）
    if (ui_) {
        ui_->setStaticText(t.pixelW, t.pixelH);
        // 平滑拖拽变焦用：长焦直连阈值（无长焦 = 极大值，UI 永不钳制）
        ui_->setTeleMin(t.teleNativeZoom > 0.f ? t.teleNativeZoom : 1e9f);
        ui_->setSensorOrientation(t.sensorOrientation);   // 预览定向的真值来源之一
    }

    // 机型层探测挂点（quirks 注入点）
    {
        ACameraMetadata* chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(cam_.manager(), cam_.deviceId().c_str(),
                                                    &chars) == ACAMERA_OK && chars) {
            auto info = device::currentDevice().probe(cam_.manager(), cam_.deviceId(), chars);
            LOGI("device.probe: hwLevel=%d raw=%d manual=%d readSettings=%d tenBit=%d orientation=%d pixels=%dx%d",
                 info.hardwareLevel, static_cast<int>(info.rawSensor),
                 static_cast<int>(info.manualSensor), static_cast<int>(info.readSensorSettings),
                 static_cast<int>(info.tenBit), info.sensorOrientation, info.pixelArrayW,
                 info.pixelArrayH);
            ACameraMetadata_free(chars);
        }
    }

    // 预览流由 core/ui 的 GL 渲染器提供（RGBA reader，GPU 采样）；此处不再直写窗口

    // RAW 通路（能力具备时创建；会话输出恒含 RAW，repeating 是否带 RAW 由模式决定）
    if (t.raw && !raw_) {
        raw_ = std::make_unique<RawCapture>();
        if (raw_->create(t.pixelW, t.pixelH, dataDir_)) {
            // M2.1：静态元数据（DNG 写入用）来自真机 characteristics
            dng::StaticMeta sm;
            sm.width = t.pixelW;
            sm.height = t.pixelH;
            std::memcpy(sm.cfaPattern, t.cfaPattern, sizeof(sm.cfaPattern));
            std::memcpy(sm.blackLevel, t.blackLevel, sizeof(sm.blackLevel));
            sm.whiteLevel = 1023;
            sm.sensorOrientation = t.sensorOrientation;
            sm.model = "Xiaomi 17 Pro (" + propName("ro.product.device") + ")";
            raw_->setStaticMeta(sm);
        } else {
            LOGE("raw reader create failed, RAW disabled");
            raw_.reset();
        }
    }
    // 会话按当前模式（逻辑广角 / 超广角物理直连）创建
    if (!rebuildSession()) return false;

    reconnects_ = 0;
    return true;
}

void CameraEngine::retireSession() {
    if (!session_) return;
    session_->close();
    retired_.push_back({std::move(session_), nowMs()});
    session_.reset();
}

void CameraEngine::reapRetired() {
    const int64_t now = nowMs();
    constexpr int64_t kGraceMs = 1500;   // 覆盖 close 后残留在飞的框架回调
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                  [now](const RetiredSession& r) {
                                      return now - r.retiredMs > kGraceMs;
                                  }),
                   retired_.end());
}

void CameraEngine::closeSession() {
    retireSession();
    if (raw_) {
        raw_->close();
        raw_.reset();
    }
    cam_.close();
}

void CameraEngine::refreshPhysIds() {
    // 覆盖键 > 自动探测。uwPhysId_ 回退 "2" 是历史遗留（本机超广角实为 3，探测正常时不会触发）。
    uwPhysId_ = forcedUwPhys_.empty() ? cam_.traits().uwPhysicalId : forcedUwPhys_;
    if (uwPhysId_.empty()) uwPhysId_ = "2";
    telePhysId_ = forcedTelePhys_.empty() ? cam_.traits().telePhysicalId : forcedTelePhys_;
    teleMinZoom_ = forcedPhysMin_ > 0.f
                       ? forcedPhysMin_
                       : (cam_.traits().teleNativeZoom > 0.f ? cam_.traits().teleNativeZoom : 1e9f);
}

std::string CameraEngine::activePhysId() const {
    // zoomRatio==0 表示"未设置"（不写 ZOOM_RATIO），按逻辑默认处理 —— 绝不能当 <1.0
    // （否则冷启动直接进物理直连，2026-09-29 真机复现过）。
    const float z = settings_.zoomRatio;
    if (z <= 0.f) return "";
    // 带间滞回：当前在物理带内时，退出阈值低于进入阈值，防止导轨在边界微动
    // 触发会话重建风暴（真机实测：uw 边界 0.86↔1.00 摆动 3 次重建）。
    if (physBand_ == uwPhysId_ && uwAllowed_ && z < 1.03f) return uwPhysId_;
    if (physBand_ == telePhysId_ && !telePhysId_.empty() && z >= teleMinZoom_ - 0.08f)
        return telePhysId_;
    // 常规判定（进入新带）
    if (z < 1.0f - 1e-3f)
        return (uwAllowed_ && !uwPhysId_.empty()) ? uwPhysId_ : std::string();
    // 高倍区走长焦直连（teleMinZoom_ 默认 = 长焦原生倍率，恰在 SAT 融合坏区之前切换）
    if (z >= teleMinZoom_ && !telePhysId_.empty()) return telePhysId_;
    return "";
}

float CameraEngine::physZoom() const {
    // 直连请求写相对数字变焦 = 用户倍率 / 带基（rel 1.0 = 该镜头原生 FOV）：
    //   超广角带基 = 0.7（用户 0.7 = 超广角原生，0.7–1.0 → rel 1.0–1.43，段内连续变焦）
    //   长焦带基   = teleNativeZoom（用户 z ≥ 2.63 → rel z/2.63，段内连续变焦）
    // 写相对值只作用于物理直连请求自身，不进逻辑融合管线（坏区）。
    const float z = settings_.zoomRatio;
    if (z <= 0.f) return 0.f;
    const std::string phys = activePhysId();
    if (phys == uwPhysId_ && !uwPhysId_.empty()) return std::max(1.0f, z / 0.7f);
    if (phys == telePhysId_ && !telePhysId_.empty() && teleMinZoom_ < 1e8f)
        return std::max(1.0f, z / teleMinZoom_);
    return 0.f;
}

std::vector<std::pair<std::string, float>> CameraEngine::allPhysZooms() {
    const float z = settings_.zoomRatio > 0.f ? settings_.zoomRatio : 1.0f;
    relUw_ = !uwPhysId_.empty() ? std::max(1.0f, z / 0.7f) : 0.f;
    relTele_ = (!telePhysId_.empty() && teleMinZoom_ < 1e8f) ? std::max(1.0f, z / teleMinZoom_)
                                                             : 0.f;
    std::vector<std::pair<std::string, float>> v;
    if (relUw_ > 0.f) v.push_back({uwPhysId_, relUw_});
    if (relTele_ > 0.f) v.push_back({telePhysId_, relTele_});
    return v;
}

std::vector<ANativeWindow*> CameraEngine::bandTargets(const std::string& sig) const {
    std::vector<ANativeWindow*> t;
    if (sig == "ALL") {
        // 探针：全部输出一次挂上（三路预览 + RAW 环）——请求不再随带切换
        for (int s = 0; s < 3; ++s)
            if (ANativeWindow* w = ui_->previewWindow(s)) t.push_back(w);
        if (raw_ && raw_->ringMode()) t.push_back(raw_->window());
        return t;
    }
    if (sig == "L") {
        t.push_back(ui_->previewWindow(0));
        if (raw_ && raw_->ringMode()) t.push_back(raw_->window());
    } else if (!uwPhysId_.empty() && sig == "P:" + uwPhysId_) {
        t.push_back(ui_->previewWindow(1));
    } else {
        t.push_back(ui_->previewWindow(2));
    }
    return t;
}

int CameraEngine::slotFromSig(const std::string& sig) const {
    if (sig == "L" || sig == "ALL") return 0;
    if (!uwPhysId_.empty() && sig == "P:" + uwPhysId_) return 1;
    return 2;
}

// 重建会话：优先多流常驻（一次 configure 全部带），HAL 拒绝组合则降级单流。
// 多流模式下本函数只在启动/设备重连/致命错误时调用（跨带走 commitSession 换请求）。
bool CameraEngine::rebuildSession() {
    if (!cam_.opened() || !ui_ || !ui_->attached()) return false;

    refreshPhysIds();

    auto sess = std::make_unique<CaptureSession>();
    sess->onFrameResult = [this](const FrameResult& r) { onFrameResult(r); };
    sess->onFrameFailed = [](int reason) { LOGW("capture failed reason=%d", reason); };

    // 旧会话 close 后进墓地延迟析构（防止 in-flight 回调 UAF），再建新会话
    retireSession();

    // ---- 多流常驻：L + uw + tele (+RAW) 一次 configure ----
    if (!multiStreamFailed_) {
        std::vector<CaptureSession::OutDesc> outs = {{ui_->previewWindow(0), nullptr}};
        if (!uwPhysId_.empty()) outs.push_back({ui_->previewWindow(1), uwPhysId_.c_str()});
        if (!telePhysId_.empty()) outs.push_back({ui_->previewWindow(2), telePhysId_.c_str()});
        if (raw_) outs.push_back({raw_->window(), nullptr});
        if (sess->create(cam_.handle(), outs)) {
            session_ = std::move(sess);
            multiStream_ = true;
            sessionSig_.clear();              // 强制 commitSession 发首个 repeating
            physBand_ = activePhysId();
            LOGI("multi-stream session created (L+uw:%s+tele:%s%s)", uwPhysId_.c_str(),
                 telePhysId_.c_str(), raw_ ? "+RAW" : "");
            commitSession(false);
            return true;
        }
        LOGE("multi-stream session rejected by HAL, falling back to single-stream rebuilds");
        multiStreamFailed_ = true;
        sess = std::make_unique<CaptureSession>();
        sess->onFrameResult = [this](const FrameResult& r) { onFrameResult(r); };
        sess->onFrameFailed = [](int reason) { LOGW("capture failed reason=%d", reason); };
    }

    // ---- 单流降级：按当前带重建（跨带 = 会话重建，旧行为）----
    multiStream_ = false;
    const std::string phys = activePhysId();
    std::vector<ANativeWindow*> outs;
    outs.push_back(ui_->previewWindow(0));
    if (phys.empty() && raw_ && raw_->ringMode()) outs.push_back(raw_->window());

    CaptureSession::OutDesc od{outs[0], phys.empty() ? nullptr : phys.c_str()};
    if (!sess->create(cam_.handle(), {od})) {
        LOGE("rebuildSession: create failed (phys=%s)", phys.c_str());
        return false;
    }
    lastRawRing_ = raw_ && raw_->ringMode();
    sessionSig_ = phys.empty() ? std::string("L") : "P:" + phys;
    if (!sess->setRepeating(sessionSig_, outs, settings_, phys, physZoom())) {
        LOGE("rebuildSession: setRepeating failed");
        return false;
    }
    session_ = std::move(sess);
    sessionIsPhysical_ = !phys.empty();
    physBand_ = phys;
    if (ui_) ui_->setPreviewSlot(slotFromSig(sessionSig_));
    LOGI("single-stream session rebuilt: %s (zoom=%.2f physZoom=%.2f)", sessionSig_.c_str(),
         settings_.zoomRatio, physZoom());
    return true;
}

// ALL 请求的逻辑流 zoom 钳在干净带内（防踩融合坏区）；物理流由逐摄键独立变焦，
// 不受此钳制影响（allPhysZooms 用原始 settings_.zoomRatio 换算）。
CaptureSettings CameraEngine::effSettings() const {
    if (!multiStream_) return settings_;
    CaptureSettings s = settings_;
    const float zmax = teleMinZoom_ < 1e8f ? std::min(2.5f, teleMinZoom_ - 0.05f) : 2.5f;
    s.zoomRatio = std::clamp(s.zoomRatio <= 0.f ? 1.0f : s.zoomRatio, 1.0f, zmax);
    return s;
}

void CameraEngine::commitSession(bool settingsChanged) {
    if (!session_) return;
    refreshPhysIds();
    const std::string phys = activePhysId();
    const bool rawRing = raw_ && raw_->ringMode();
    const bool rawRingChanged = rawRing != lastRawRing_;
    lastRawRing_ = rawRing;

    if (multiStream_) {
        // ---- 方案 A 全目标常驻：repeating 恒为 ALL（三路预览+RAW），跨带零请求切换 ----
        // 只有 RAW 进出会改目标集（重建请求）；其余变化复用请求只改 entry 重发（0 间隔）。
        if (sessionSig_ != "ALL" || rawRingChanged) {
            LOGI("ALL repeating (re)build: rawRing=%d zoom=%.2f", (int)rawRing,
                 settings_.zoomRatio);
            if (session_->setRepeatingAll(bandTargets("ALL"), effSettings(), allPhysZooms())) {
                sessionSig_ = "ALL";
                sessionIsPhysical_ = false;   // RAW 恒出帧，全带可拍
            } else {
                LOGE("ALL repeating failed, rebuilding session");
                if (!rebuildSession()) {
                    LOGE("rebuild failed, reconnecting");
                    closeSession();
                    ++reconnects_;
                }
                return;
            }
        } else if (settingsChanged) {
            session_->setRepeatingAll(bandTargets("ALL"), effSettings(), allPhysZooms());
        }
        // 显示源切换：纯 GL 层（不动请求），按滞回带判定（activePhysId 内含滞回状态机）
        physBand_ = phys;
        if (ui_)
            ui_->setPreviewSlot(slotFromSig(phys.empty() ? std::string("L") : "P:" + phys));
        return;
    }

    // ---- 单流降级：跨带 / RAW 进出 → 会话重建 ----
    const std::string sig = phys.empty() ? std::string("L") : "P:" + phys;
    if (sig != sessionSig_ || rawRingChanged) {
        LOGI("session mode change: %s -> %s (zoom=%.2f rawRing=%d)", sessionSig_.c_str(),
             sig.c_str(), settings_.zoomRatio, (int)rawRing);
        if (!rebuildSession()) {
            LOGE("rebuild failed, reconnecting");
            closeSession();
            ++reconnects_;
        }
    } else if (settingsChanged) {
        std::vector<ANativeWindow*> outs;
        outs.push_back(ui_->previewWindow(0));
        if (phys.empty() && rawRing) outs.push_back(raw_->window());
        session_->setRepeating(sig, outs, settings_, phys, physZoom());
    }
    if (session_) physBand_ = phys;
}

void CameraEngine::pollControls() {
    if (!ctl_) return;
    auto kv = ctl_->poll();
    if (!kv) return;

    bool changed = false;
    for (auto& [k, v] : *kv) applyControl(k, v, changed);

    commitSession(changed);

    // 一次性命令自消费：处理后从文件剔除（防 FUSE mtime 抖动导致的重触发）
    bool hadOneShot = false;
    for (const char* k : {"zsl_shutter", "shot_raw"}) {
        if (kv->count(k)) {
            kv->erase(k);
            hadOneShot = true;
        }
    }
    if (hadOneShot) {
        std::ofstream out(dataDir_ + "/controls.txt", std::ios::trunc);
        for (auto& [k, v] : *kv) out << k << "=" << v << "\n";
    }
}

void CameraEngine::applyControl(const std::string& k, const std::string& v, bool& changed) {
    const auto& t = cam_.traits();
    if (k == "ae") {
        bool on = parseOn(v);
        if (on != settings_.aeOn) {
            settings_.aeOn = on;
            changed = true;
        }
    } else if (k == "iso") {
        int32_t iso = std::clamp(std::atoi(v.c_str()), t.isoMin, t.isoMax);
        if (iso != settings_.iso) {
            settings_.iso = iso;
            settings_.aeOn = false; // 手动 ISO 隐含 AE off
            changed = true;
        }
    } else if (k == "exp_us") {
        long long us = std::atoll(v.c_str());
        int64_t ns = std::clamp<int64_t>(us * 1000, t.exposureMinNs, t.exposureMaxNs);
        if (ns != settings_.exposureNs) {
            settings_.exposureNs = ns;
            settings_.aeOn = false;
            changed = true;
        }
    } else if (k == "af") {
        bool on = parseOn(v);
        if (on != settings_.afOn) {
            settings_.afOn = on;
            changed = true;
        }
    } else if (k == "focus_d") {
        float d = std::atof(v.c_str());
        if (d != settings_.focusDistance) {
            settings_.focusDistance = d;
            settings_.afOn = false;
            changed = true;
        }
    } else if (k == "awb") {
        bool on = parseOn(v);
        if (on != settings_.awbOn) {
            settings_.awbOn = on;
            changed = true;
        }
    } else if (k == "disp") {
        // 诊断：手动切显示源（0=逻辑 1=超广 2=长焦；纯 GL 层，不动请求）
        if (ui_) ui_->setPreviewSlot(std::clamp(std::atoi(v.c_str()), 0, 2));
    } else if (k == "zoom") {
        if (!t.hasZoomRatio) {
            LOGW("zoom not supported on %s", t.id.c_str());
            return;
        }
        float zmin = uwAllowed_ ? t.zoomMin : std::max(1.0f, t.zoomMin);
        float z = std::clamp(static_cast<float>(std::atof(v.c_str())), zmin, t.zoomMax);
        if (z != settings_.zoomRatio) {
            settings_.zoomRatio = z;
            changed = true;
        }
    } else if (k == "uw") {
        bool on = parseOn(v);
        if (on != uwAllowed_) {
            uwAllowed_ = on;
            LOGI("ultrawide opt-in: %s (raw zoomMin=%.2f max=%.2f)",
                 on ? "ON" : "OFF", t.zoomMin, t.zoomMax);
        }
    } else if (k == "uw_phys") {
        // 强制指定超广角物理摄像头 ID（诊断用）。空串 = 恢复自动探测。
        if (v != forcedUwPhys_) {
            forcedUwPhys_ = v;
            LOGI("ultrawide physical override: '%s'", v.c_str());
        }
    } else if (k == "tele_phys") {
        // 强制指定长焦物理摄像头 ID（诊断用）。空串 = 恢复自动探测。
        if (v != forcedTelePhys_) {
            forcedTelePhys_ = v;
            LOGI("tele physical override: '%s'", v.c_str());
        }
    } else if (k == "phys_min") {
        // 强制长焦直连起始倍率（诊断用；0 = 恢复自动值 teleNativeZoom）
        float f = std::atof(v.c_str());
        if (f != forcedPhysMin_) {
            forcedPhysMin_ = f;
            LOGI("tele direct threshold override: %.2f", f);
        }
    } else if (k == "raw_mode") {
        if (!raw_) {
            LOGW("raw unavailable on %s", t.id.c_str());
            return;
        }
        bool ring = (v == "ring");
        if (ring != raw_->ringMode()) {
            raw_->setRingMode(ring);
            // 会话组成变化（RAW 进出 repeating）由 commitSession 的签名检测统一重建
        }
    } else if (k == "zsl_shutter") {
        triggerBurst();
    } else if (k == "save_quota") {
        // controls.txt 每轮都会被重放，值没变就别刷日志（之前每 400ms 一条，把 logcat 淹了）
        int n = std::max(0, std::atoi(v.c_str()));
        if (n != saveQuota_) {
            saveQuota_ = n;
            LOGI("save quota = %d triggers per launch", saveQuota_);
        }
    } else if (k == "ss") {
        // 快门分母（1/x s）→ µs
        double den = std::atof(v.c_str());
        if (den > 0) {
            settings_.exposureNs = std::clamp<int64_t>(int64_t(1e9 / den * 1000),
                                                       t.exposureMinNs, t.exposureMaxNs);
            settings_.aeOn = false;
            changed = true;
        }
    } else if (k == "ev") {
        float ev = std::atof(v.c_str());
        settings_.evSteps = std::clamp(int(std::lround(ev / std::max(t.evStep, 0.01f))),
                                       t.evMin, t.evMax);
        settings_.aeOn = true;
        changed = true;
    } else if (k == "uvrot") {
        if (ui_) ui_->setUvRot(std::atoi(v.c_str()));
    } else if (k == "shot_raw") {
        if (!raw_ || raw_->ringMode()) {
            LOGW("shot_raw requires raw_mode=once");
            return;
        }
        if (!multiStream_ && !activePhysId().empty()) {
            // 单流降级模式下物理带 repeating 无 RAW 流；ALL 常驻下 RAW 恒出帧，全带可拍
            LOGW("shot_raw unavailable in physical direct mode (no RAW on uw/tele)");
            return;
        }
        if (savesUsed_ >= saveQuota_) {
            LOGW("save quota exhausted (%d/%d) - restart app to reset", savesUsed_, saveQuota_);
            return;
        }
        ++savesUsed_;
        raw_->requestSingle();
        session_->captureOnce({raw_->window()}, settings_);
    } else if (k == "fps_log") {
        forceLog_ = true;
    }
}

void CameraEngine::drainUiCmds() {
    ui::Ui::Cmd cmd;
    const auto& t = cam_.traits();
    // changed 跨整轮循环累计：拖拽时一帧可能积压多个命令，统一在循环结束后只重发一次
    // repeating（本机 HAL 对高频 setRepeating 敏感，会静默掐断预览流）。
    bool changed = false;
    while (ui_ && ui_->popCmd(&cmd)) {
        switch (cmd.type) {
            case ui::Ui::Cmd::SET_ISO:
                settings_.iso = std::clamp(int(cmd.v), t.isoMin, t.isoMax);
                settings_.aeOn = false;
                changed = true;
                break;
            case ui::Ui::Cmd::SET_EXP_US:
                settings_.exposureNs = std::clamp<int64_t>(
                    int64_t(cmd.v * 1000), t.exposureMinNs, t.exposureMaxNs);
                settings_.aeOn = false;
                changed = true;
                break;
            case ui::Ui::Cmd::SET_ZOOM:
                if (t.hasZoomRatio) {
                    float zmin = uwAllowed_ ? t.zoomMin : std::max(1.0f, t.zoomMin);
                    settings_.zoomRatio = std::clamp(cmd.v, zmin, t.zoomMax);
                    changed = true;
                }
                break;
            case ui::Ui::Cmd::SET_EV:
                settings_.evSteps = std::clamp(int(std::lround(cmd.v / std::max(t.evStep, 0.01f))),
                                               t.evMin, t.evMax);
                settings_.aeOn = true;
                changed = true;
                break;
            case ui::Ui::Cmd::SET_AE:
                settings_.aeOn = cmd.v > 0.5f;
                changed = true;
                break;
            case ui::Ui::Cmd::SHOT:
                triggerBurst();
                break;
        }
    }
    // 整轮命令处理完后统一提交（模式变化→重建会话，仅设置变化→重发 repeating）。
    // 避免逐命令重发导致 HAL 断流。
    if (changed) {
        commitSession(true);
        LOGI("ui applied: ae=%d iso=%d exp=%lldns ev=%d zoom=%.2f uwPhys=%s",
             static_cast<int>(settings_.aeOn), settings_.iso,
             static_cast<long long>(settings_.exposureNs), settings_.evSteps,
             settings_.zoomRatio, uwPhysId_.c_str());
    }
}

void CameraEngine::triggerBurst() {
    if (!raw_ || !raw_->ringMode()) {
        LOGW("trigger requires raw_mode=ring");
        if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_);
        return;
    }
    if (!multiStream_ && !activePhysId().empty()) {
        // 单流降级模式下物理带 repeating 无 RAW 流；ALL 常驻下 RAW 恒出帧，全带可拍
        LOGW("trigger unavailable in physical direct mode (no RAW on uw/tele)");
        if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_);
        return;
    }
    if (savesUsed_ >= saveQuota_) {
        LOGW("save quota exhausted (%d/%d) - restart app to reset", savesUsed_, saveQuota_);
        if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_);
        return;
    }
    ++savesUsed_;
    raw_->shutterBurst(4);
    if (ui_) ui_->notifyShot(true, savesUsed_, saveQuota_);
    LOGI("shutter triggered (%d/%d)", savesUsed_, saveQuota_);
}

int64_t CameraEngine::nowMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void CameraEngine::onFrameResult(const FrameResult& r) {
    frameCount_++;
    lastResultMs_ = nowMs();

    // 帧节奏探针
    if (lastFrameMs_ != 0) {
        const int64_t gap = lastResultMs_ - lastFrameMs_;
        if (gap > 70) ++frameGaps_;
        if (gap > maxGapMs_) maxGapMs_ = gap;
    }
    lastFrameMs_ = lastResultMs_;

    if (firstTs_ == 0) firstTs_ = r.timestampNs;
    lastTs_ = r.timestampNs;

    if (raw_) raw_->onFrameResult(r);

    // 回传当前出图帧对应的"用户倍率"（UI 平滑变焦的锚点）：
    // result 里的 zoomRatio 在物理直连下是相对值，需按带基换算回用户域。
    float userZoom = r.zoomRatio;
    if (multiStream_ && sessionSig_ == "ALL") {
        // ALL 模式：result 元数据是逻辑流的（被钳在干净带内），显示源的应用倍率
        // 按当前显示带换算 —— 物理流 applied = 写入的相对值 × 带基。
        if (physBand_ == uwPhysId_ && relUw_ > 0.f) {
            userZoom = relUw_ * 0.7f;
        } else if (physBand_ == telePhysId_ && relTele_ > 0.f && teleMinZoom_ < 1e8f) {
            userZoom = relTele_ * teleMinZoom_;
        }
        // 逻辑带：r.zoomRatio 即真实应用值
    } else if (sessionIsPhysical_) {
        if (sessionSig_ == "P:" + uwPhysId_)
            userZoom = r.zoomRatio * 0.7f;
        else if (teleMinZoom_ < 1e8f)
            userZoom = r.zoomRatio * teleMinZoom_;
    }
    if (ui_) ui_->setAppliedZoom(userZoom);

    if (forceLog_ || frameCount_ % 120 == 0) {
        forceLog_ = false;
        double fps = lastTs_ > firstTs_
                         ? static_cast<double>(frameCount_ - 1) * 1e9 /
                               static_cast<double>(lastTs_ - firstTs_)
                         : 0.0;
        if (raw_) {
            LOGI("stats: frames=%llu fps=%.1f eff(iso=%d exp=%lldns zoom=%.2f) raw: %.1ffps total=%lld",
                 static_cast<unsigned long long>(frameCount_), fps, r.iso,
                 static_cast<long long>(r.exposureNs), r.zoomRatio, raw_->rawFps(),
                 static_cast<long long>(raw_->rawCount()));
        } else {
            LOGI("stats: frames=%llu fps=%.1f eff(iso=%d exp=%lldns zoom=%.2f)",
                 static_cast<unsigned long long>(frameCount_), fps, r.iso,
                 static_cast<long long>(r.exposureNs), r.zoomRatio);
        }
        LOGI("pacing: gaps>70ms=%d max=%lldms (frames=%llu)", frameGaps_,
             (long long)maxGapMs_, static_cast<unsigned long long>(frameCount_));
        if (multiStream_ && sessionSig_ == "ALL" && ui_)
            LOGI("slot frames: L=%lld uw=%lld tele=%lld",
                 (long long)ui_->slotFrames(0), (long long)ui_->slotFrames(1),
                 (long long)ui_->slotFrames(2));
    }
}

} // namespace optic::capture
