#include "core/capture/CameraEngine.h"

#include "core/device/DeviceRegistry.h"
#include "core/dng/DngWriter.h"
#include "core/util/Log.h"

#include <camera/NdkCameraMetadata.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

bool CameraEngine::start(ANativeWindow* window, const std::string& dataDir) {
    if (running_.load(std::memory_order_acquire)) return true;
    stopFlag_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this, window, dataDir] { run(window, dataDir); });
    return true;
}

void CameraEngine::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    stopFlag_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
}

void CameraEngine::run(ANativeWindow* window, std::string dataDir) {
    dataDir_ = std::move(dataDir);
    preview_ = window;

    LOGI("engine start: device=%s market=%s registry=%s dataDir=%s",
         propName("ro.product.device").c_str(), propName("ro.product.market.name").c_str(),
         device::currentDevice().name(), dataDir_.c_str());

    while (!stopFlag_.load(std::memory_order_acquire)) {
        if (!cam_.opened()) {
            if (reconnects_ >= 5) {
                LOGE("reconnect limit reached, engine exits");
                break;
            }
            if (!openSession(window)) {
                closeSession();
                ++reconnects_;
                std::this_thread::sleep_for(1s);
                continue;
            }
        }

        pollControls();
        pollShutterButton();

        if (deviceLost_.exchange(false, std::memory_order_acq_rel)) {
            LOGW("device lost -> reconnect");
            closeSession();
            ++reconnects_;
            continue;
        }

        std::this_thread::sleep_for(100ms);
    }
    closeSession();
    LOGI("engine exit");
}

bool CameraEngine::openSession(ANativeWindow* window) {
    preview_ = window;

    cam_.onLost = [this] { deviceLost_.store(true, std::memory_order_release); };
    cam_.onDeviceError = [this](int) { deviceLost_.store(true, std::memory_order_release); };

    if (!cam_.openFirstBack()) return false;
    const auto& t = cam_.traits();

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

    if (pickPreviewSize(&previewW_, &previewH_)) {
        LOGI("preview geometry: %dx%d", previewW_, previewH_);
        ANativeWindow_setBuffersGeometry(window, previewW_, previewH_, 0);
    }

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
    rawInRepeating_ = raw_ ? raw_->ringMode() : false;

    std::vector<ANativeWindow*> outs{window};
    if (raw_) outs.push_back(raw_->window());

    session_ = std::make_unique<CaptureSession>();
    session_->onFrameResult = [this](const FrameResult& r) { onFrameResult(r); };
    session_->onFrameFailed = [](int reason) { LOGW("capture failed reason=%d", reason); };
    if (!session_->create(cam_.handle(), outs)) return false;

    if (!session_->setRepeating(sessionTargets(), settings_)) return false;

    LOGI("session running: preview=%dx%d raw=%s", previewW_, previewH_,
         raw_ ? (rawInRepeating_ ? "ring" : "once") : "off");
    reconnects_ = 0;
    return true;
}

void CameraEngine::closeSession() {
    if (session_) {
        session_->close();
        session_.reset();
    }
    if (raw_) {
        raw_->close();
        raw_.reset();
    }
    cam_.close();
}

void CameraEngine::pollControls() {
    util::ControlFile ctl(dataDir_ + "/controls.txt", std::chrono::milliseconds(400));
    auto kv = ctl.poll();
    if (!kv) return;

    bool changed = false, rebuild = false;
    for (auto& [k, v] : *kv) applyControl(k, v, changed, rebuild);

    if (raw_ && raw_->ringMode() != rawInRepeating_) {
        rawInRepeating_ = raw_->ringMode();
        rebuild = true;
    }

    if ((changed || rebuild) && session_) {
        if (session_->setRepeating(sessionTargets(), settings_)) {
            LOGI("settings applied: ae=%d iso=%d exp=%lldns af=%d focus=%.2f awb=%d zoom=%.2f",
                 static_cast<int>(settings_.aeOn), settings_.iso,
                 static_cast<long long>(settings_.exposureNs), static_cast<int>(settings_.afOn),
                 settings_.focusDistance, static_cast<int>(settings_.awbOn),
                 settings_.zoomRatio);
        }
    }

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

void CameraEngine::applyControl(const std::string& k, const std::string& v, bool& changed,
                                bool& rebuild) {
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
    } else if (k == "zoom") {
        if (!t.hasZoomRatio) {
            LOGW("zoom not supported on %s", t.id.c_str());
            return;
        }
        float z = std::clamp(static_cast<float>(std::atof(v.c_str())), t.zoomMin, t.zoomMax);
        if (z != settings_.zoomRatio) {
            settings_.zoomRatio = z;
            changed = true;
        }
    } else if (k == "raw_mode") {
        if (!raw_) {
            LOGW("raw unavailable on %s", t.id.c_str());
            return;
        }
        bool ring = (v == "ring");
        if (ring != raw_->ringMode()) {
            raw_->setRingMode(ring);
            rebuild = true;
        }
    } else if (k == "zsl_shutter") {
        triggerBurst();
    } else if (k == "save_quota") {
        saveQuota_ = std::max(0, std::atoi(v.c_str()));
        LOGI("save quota = %d triggers per launch", saveQuota_);
    } else if (k == "shot_raw") {
        if (!raw_ || raw_->ringMode()) {
            LOGW("shot_raw requires raw_mode=once");
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

void CameraEngine::pollShutterButton() {
    bool pressed = shutterPressed_ && shutterPressed_();
    if (pressed && !lastShutterPressed_ &&
        std::chrono::steady_clock::now() - lastTriggerAt_ > std::chrono::milliseconds(700)) {
        lastTriggerAt_ = std::chrono::steady_clock::now();
        triggerBurst();
    }
    lastShutterPressed_ = pressed;
}

void CameraEngine::triggerBurst() {
    if (!raw_ || !raw_->ringMode()) {
        LOGW("trigger requires raw_mode=ring");
        return;
    }
    if (savesUsed_ >= saveQuota_) {
        LOGW("save quota exhausted (%d/%d) - restart app to reset", savesUsed_, saveQuota_);
        return;
    }
    ++savesUsed_;
    raw_->shutterBurst(4);   // 想单帧验证就改成 shutterBurst(1)
    LOGI("shutter triggered (%d/%d)", savesUsed_, saveQuota_);
}

std::vector<ANativeWindow*> CameraEngine::sessionTargets() const {
    std::vector<ANativeWindow*> outs{preview_};
    if (raw_ && raw_->ringMode()) outs.push_back(raw_->window());
    return outs;
}

void CameraEngine::onFrameResult(const FrameResult& r) {
    frameCount_++;
    if (firstTs_ == 0) firstTs_ = r.timestampNs;
    lastTs_ = r.timestampNs;

    if (raw_) raw_->onFrameResult(r);

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
    }
}

bool CameraEngine::pickPreviewSize(int32_t* w, int32_t* h) {
    ACameraMetadata* chars = nullptr;
    if (ACameraManager_getCameraCharacteristics(cam_.manager(), cam_.deviceId().c_str(), &chars) !=
        ACAMERA_OK)
        return false;
    bool ok = false;
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                                      &e) == ACAMERA_OK) {
        int32_t bestW = 0, bestH = 0;
        for (uint32_t i = 0; i + 3 < e.count; i += 4) {
            int32_t fmt = e.data.i32[i], sw = e.data.i32[i + 1], sh = e.data.i32[i + 2],
                    isInput = e.data.i32[i + 3];
            if (isInput == 0 && (fmt == 0x23 || fmt == 0x22) && sw <= 1920 && sw * sh > bestW * bestH) {
                bestW = sw;
                bestH = sh;
            }
        }
        if (bestW > 0) {
            *w = bestW;
            *h = bestH;
            ok = true;
        }
    }
    ACameraMetadata_free(chars);
    return ok;
}

} // namespace optic::capture
