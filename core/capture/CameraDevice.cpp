#include "core/capture/CameraDevice.h"
#include "core/device/DeviceRegistry.h"
#include "core/util/Log.h"

#include <cstring>
#include <map>
#include <sys/system_properties.h>
#include <utility>
#include <vector>

namespace optic::capture {

bool CameraDevice::readTraits(const char* id) {
    traits_ = {};
    traits_.id = id;

    ACameraMetadata* chars = nullptr;
    if (ACameraManager_getCameraCharacteristics(manager_, id, &chars) != ACAMERA_OK || !chars) {
        LOGE("characteristics failed for %s", id);
        return false;
    }

    auto byte0 = [&](uint32_t tag, int32_t* out) {
        ACameraMetadata_const_entry e{};
        if (ACameraMetadata_getConstEntry(chars, tag, &e) == ACAMERA_OK && e.count > 0) {
            *out = e.data.u8[0];
            return true;
        }
        return false;
    };

    byte0(ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL, &traits_.hardwareLevel);

    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, &e) == ACAMERA_OK) {
        for (uint32_t i = 0; i < e.count; ++i) {
            if (e.data.u8[i] == ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_RAW) traits_.raw = true;
            if (e.data.u8[i] == ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA) traits_.logical = true;
        }
    }

    if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_ORIENTATION, &e) == ACAMERA_OK && e.count > 0)
        traits_.sensorOrientation = e.data.i32[0];
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_INFO_PIXEL_ARRAY_SIZE, &e) == ACAMERA_OK && e.count >= 2) {
        traits_.pixelW = e.data.i32[0];
        traits_.pixelH = e.data.i32[1];
    }
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE, &e) == ACAMERA_OK && e.count >= 2) {
        traits_.exposureMinNs = e.data.i64[0];
        traits_.exposureMaxNs = e.data.i64[1];
    }
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE, &e) == ACAMERA_OK && e.count >= 2) {
        traits_.isoMin = e.data.i32[0];
        traits_.isoMax = e.data.i32[1];
    }
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_CONTROL_ZOOM_RATIO_RANGE, &e) == ACAMERA_OK && e.count >= 2) {
        // 保留原始下限（可能 <1.0，即超广角）。是否暴露 sub-1.0 由引擎 controls.txt 的
        // uw=1 决定；默认不暴露（引擎侧钳到 1.0），因为本机超广角物理镜头在逻辑多摄切换下
        // 会周期性断流（CamX SetCurrentflushOffset NULL）。待确诊是流配置问题还是硬件缺陷。
        traits_.zoomMin = e.data.f[0];
        traits_.zoomMax = e.data.f[1];
        traits_.hasZoomRatio = true;
    }
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &e) == ACAMERA_OK && e.count > 0)
        traits_.focusMinD = static_cast<int32_t>(e.data.f[0]);

    if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_BLACK_LEVEL_PATTERN, &e) == ACAMERA_OK && e.count >= 4)
        for (int i = 0; i < 4; ++i) traits_.blackLevel[i] = e.data.i32[i];

    if (ACameraMetadata_getConstEntry(chars, ACAMERA_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES, &e) == ACAMERA_OK)
        for (uint32_t i = 0; i < e.count; ++i)
            if (e.data.u8[i] == 1) traits_.lscOn = true;

    if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &e) == ACAMERA_OK && e.count > 0) {
        // CFA 枚举 → 2×2 图案（DNG CFAPattern 值：R=0, G=1, B=2）
        static const uint8_t kCfa[4][4] = {
            {0, 1, 1, 2},   // RGGB
            {1, 0, 2, 1},   // GRBG
            {1, 2, 1, 0},   // GBRG
            {2, 1, 1, 0},   // BGGR
        };
        int idx = e.data.u8[0];
        if (idx >= 0 && idx <= 3)
            std::memcpy(traits_.cfaPattern, kCfa[idx], 4);
    }

    if (ACameraMetadata_getConstEntry(chars, ACAMERA_CONTROL_AE_COMPENSATION_RANGE, &e) == ACAMERA_OK && e.count >= 2) {
        traits_.evMin = e.data.i32[0];
        traits_.evMax = e.data.i32[1];
    }
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_CONTROL_AE_COMPENSATION_STEP, &e) == ACAMERA_OK && e.count >= 2 && e.data.r[1].denominator != 0) {
        traits_.evStep = float(e.data.r[0].numerator) / float(e.data.r[1].denominator);
    }

    // 逻辑摄的物理成员 id（byte[n]，'\0' 分隔的字符串）
    if (traits_.logical &&
        ACameraMetadata_getConstEntry(chars, ACAMERA_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS, &e) == ACAMERA_OK) {
        std::string blob(reinterpret_cast<const char*>(e.data.u8), e.count);
        size_t start = 0;
        while (start < blob.size()) {
            size_t end = blob.find('\0', start);
            if (end == std::string::npos) end = blob.size();
            if (end > start) traits_.physicalIds.emplace_back(blob, start, end - start);
            start = end + 1;
        }
    }

    // 探测物理镜头布局：逻辑摄的物理成员按焦距排序 —— 最短=超广角、最长=长焦、
    // 居中者=主摄。本机 [3 2 4] = 2.57mm(超广) / 6.62mm(主) / 17.42mm(长焦)。
    // 倍率必须用等效焦距比（含传感器宽度）：zoom_i = (f_i/w_i) / (f_main/w_main)。
    // 仅焦距比会错得离谱 —— tele 传感器(5.24mm)远小于主摄(9.99mm)，焦距比 2.63x
    // 而真值 5.02x（2026-09-30 真机+MIUI 相机口径双重确认：1x=23mm、5x=115mm、
    // 0.7x=17mm，切换点就是 1x 与 5x）。此前按 2.63x 分带导致切点错乱。
    // 逻辑多摄的 ZOOM_RATIO 融合管线在本机有两个坏区：sub-1.0（超广角融合）与
    // 高倍数字区（SAT/长焦融合），两者都要物理直连绕开（见 CameraEngine）。
    if (traits_.logical && !traits_.physicalIds.empty()) {
        float fMin = 1e9f, fMax = 0.f, fMain = 0.f;
        float swMain = 0.f;                             // 主摄传感器宽 mm
        std::string minId, maxId;   // 焦距最短/最长者（旧判据的兜底用）
        std::map<std::string, float> swOf;              // id -> sensorW
        std::map<std::string, float> fOf;               // id -> focal length (mm)
        for (auto& pid : traits_.physicalIds) {
            ACameraMetadata* pc = nullptr;
            if (ACameraManager_getCameraCharacteristics(manager_, pid.c_str(), &pc) != ACAMERA_OK || !pc)
                continue;
            ACameraMetadata_const_entry fe{};
            if (ACameraMetadata_getConstEntry(pc, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &fe) ==
                    ACAMERA_OK &&
                fe.count > 0) {
                float fl = fe.data.f[0];
                float sw = 0.f;
                ACameraMetadata_const_entry se{};
                if (ACameraMetadata_getConstEntry(pc, ACAMERA_SENSOR_INFO_PHYSICAL_SIZE, &se) ==
                        ACAMERA_OK &&
                    se.count >= 1)
                    sw = se.data.f[0];
                swOf[pid] = sw;
                fOf[pid] = fl;
                if (fl < fMin) { fMin = fl; minId = pid; }
                if (fl > fMax) { fMax = fl; maxId = pid; }
            }
            ACameraMetadata_free(pc);
        }
        auto f35 = [](float f, float w) { return w > 0.f ? f * 36.f / w : 0.f; };
        // ---- 主摄判定：35mm 等效焦距最接近 24mm 的成员（业界通用主摄视角）----
        // 原实现按「成员 ≥3 才认长焦、只有 2 个时把较短者当超广」判定 —— 在
        // 「主摄 + 长焦」双摄机型上会把**主摄误判成超广角**（minId 无条件写进
        // uwPhysicalId）。改成按等效焦距选主摄后判定与成员数无关：
        //   uw + main      → 主摄是较长者；main + tele → 主摄是较短者。两者都对。
        constexpr float kRefEquivMm = 24.f;
        std::string mainId, uwId, teleId;
        float mainEq = 0.f, uwEq = 0.f, teleEq = 0.f;
        {
            float bestD = 1e9f;
            for (auto& kv : swOf) {
                const float eq = f35(fOf[kv.first], kv.second);
                if (eq <= 0.f) continue;
                const float d = std::fabs(eq - kRefEquivMm);
                if (d < bestD) { bestD = d; mainId = kv.first; mainEq = eq; }
            }
        }
        if (mainId.empty() && swOf.size() >= 3) {
            // 传感器尺寸缺失 → 退回「既非最短也非最长」的老判据
            for (auto& kv : swOf)
                if (kv.first != minId && kv.first != maxId) { mainId = kv.first; break; }
        }
        if (mainId.empty()) mainId = maxId.empty() ? minId : maxId;   // 最后兜底
        fMain = fOf[mainId];
        swMain = swOf[mainId];

        // 副摄角色按与主摄的等效焦距比划分（阈值留足余量：本机 uw=18.5 / main=23.8
        // / tele=119.6mm，比值 0.78 / 1.0 / 5.02）。
        for (auto& kv : swOf) {
            if (kv.first == mainId) continue;
            const float eq = f35(fOf[kv.first], kv.second);
            if (eq <= 0.f) continue;
            if (eq < mainEq * 0.92f) {           // 明显更广 → 超广（取最接近主摄的）
                if (uwId.empty() || eq > uwEq) { uwId = kv.first; uwEq = eq; }
            } else if (eq > mainEq * 1.25f) {    // 明显更长 → 长焦（取最接近主摄的）
                if (teleId.empty() || eq < teleEq) { teleId = kv.first; teleEq = eq; }
            }
        }
        traits_.telePhysicalId = teleId;
        if (fMain > 0.f) {
            traits_.mainFocal = fMain;
            // 等效焦距比（传感器尺寸缺失时回退纯焦距比）
            auto zoomOf = [](float f, float w, float f0, float w0) {
                if (f <= 0.f || f0 <= 0.f) return 0.f;
                if (w <= 0.f || w0 <= 0.f) return f / f0;
                return (f / w) / (f0 / w0);
            };
            if (!uwId.empty())
                traits_.uwNativeZoom = zoomOf(fOf[uwId], swOf[uwId], fMain, swMain);
            if (!traits_.telePhysicalId.empty())
                traits_.teleNativeZoom = zoomOf(fOf[teleId], swOf[teleId], fMain, swMain);
        }
        if (mainId.empty()) {
            LOGW("physical lens detection failed (no focal length for members)");
        } else {
            traits_.uwPhysicalId = uwId;
            LOGI("phys layout: uw=%s main=%s(eq %.1fmm) tele=%s -> uw=%.3fx tele=%.3fx",
                 uwId.c_str(), mainId.c_str(), mainEq, traits_.telePhysicalId.c_str(),
                 traits_.uwNativeZoom, traits_.teleNativeZoom);
        }
    }

    ACameraMetadata_free(chars);

    // 机型 quirk：per-physical 变焦键是否真实生效（pandora = false，2026-09-29 确诊）
    traits_.physPerKeyZoom = device::currentDevice().physPerKeyZoom();
    LOGI("quirk physPerKeyZoom=%d", (int)traits_.physPerKeyZoom);
    return true;
}

bool CameraDevice::openFirstBack(const std::string& forcedId) {
    close();
    manager_ = ACameraManager_create();
    if (!manager_) return false;

    ACameraIdList* ids = nullptr;
    if (ACameraManager_getCameraIdList(manager_, &ids) != ACAMERA_OK || !ids) return false;

    std::string chosen;
    if (!forcedId.empty()) {
        // 诊断用：直接打开指定 ID（如物理超广角 2），绕过逻辑多摄 ZOOM_RATIO 切换管线
        for (int i = 0; i < ids->numCameras; ++i) {
            if (forcedId == ids->cameraIds[i]) { chosen = forcedId; break; }
        }
        if (chosen.empty()) LOGW("forced camera %s not in id list", forcedId.c_str());
    } else {
        for (int i = 0; i < ids->numCameras && chosen.empty(); ++i) {
            ACameraMetadata* chars = nullptr;
            if (ACameraManager_getCameraCharacteristics(manager_, ids->cameraIds[i], &chars) != ACAMERA_OK) continue;
            ACameraMetadata_const_entry e{};
            if (ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_FACING, &e) == ACAMERA_OK && e.count > 0 &&
                e.data.u8[0] == ACAMERA_LENS_FACING_BACK) {
                chosen = ids->cameraIds[i];
            }
            ACameraMetadata_free(chars);
        }
    }
    ACameraManager_deleteCameraIdList(ids);
    if (chosen.empty()) {
        LOGE("no back camera");
        return false;
    }

    if (!readTraits(chosen.c_str())) return false;

    cbs_ = {this, &CameraDevice::onDisconnected, &CameraDevice::onError, nullptr};
    if (ACameraManager_openCamera(manager_, chosen.c_str(), &cbs_, &device_) != ACAMERA_OK || !device_) {
        LOGE("openCamera failed");
        return false;
    }

    LOGI("camera %s opened: hw=%d raw=%d logical=%d phys=[%s] zoom=[%.2f,%.2f] exp=[%lld,%lld]ns iso=[%d,%d]",
         chosen.c_str(), traits_.hardwareLevel, (int)traits_.raw, (int)traits_.logical,
         [this] { std::string s; for (auto& p : traits_.physicalIds) s += p + " "; return s; }().c_str(),
         traits_.zoomMin, traits_.zoomMax, (long long)traits_.exposureMinNs, (long long)traits_.exposureMaxNs,
         traits_.isoMin, traits_.isoMax);
    return true;
}

void CameraDevice::close() {
    if (device_) { ACameraDevice_close(device_); device_ = nullptr; }
    if (manager_) { ACameraManager_delete(manager_); manager_ = nullptr; }
}

void CameraDevice::onDisconnected(void* ctx, ACameraDevice*) {
    LOGW("camera disconnected");
    auto* self = static_cast<CameraDevice*>(ctx);
    if (self && self->onLost) self->onLost();
}

void CameraDevice::onError(void* ctx, ACameraDevice*, int error) {
    LOGE("camera device error=%d", error);
    auto* self = static_cast<CameraDevice*>(ctx);
    if (self && self->onDeviceError) self->onDeviceError(error);
}

} // namespace optic::capture
