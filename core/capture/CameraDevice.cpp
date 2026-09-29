#include "core/capture/CameraDevice.h"
#include "core/util/Log.h"

#include <cstring>
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
    // 居中者=主摄。本机 [3 2 4] = 2.57mm(超广) / 6.62mm(主) / 17.42mm(长焦≈2.63x)。
    // 逻辑多摄的 ZOOM_RATIO 融合管线在本机有两个坏区：sub-1.0（超广角融合）与
    // 高倍数字区（≥~4，SAT/长焦融合），两者都要物理直连绕开（见 CameraEngine）。
    if (traits_.logical && !traits_.physicalIds.empty()) {
        float fMin = 1e9f, fMax = 0.f, fMain = 0.f;
        std::string minId, maxId;
        std::vector<std::pair<std::string, float>> fmap;   // (id, focal)
        for (auto& pid : traits_.physicalIds) {
            ACameraMetadata* pc = nullptr;
            if (ACameraManager_getCameraCharacteristics(manager_, pid.c_str(), &pc) != ACAMERA_OK || !pc)
                continue;
            ACameraMetadata_const_entry fe{};
            if (ACameraMetadata_getConstEntry(pc, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &fe) ==
                    ACAMERA_OK &&
                fe.count > 0) {
                float fl = fe.data.f[0];
                fmap.emplace_back(pid, fl);
                if (fl < fMin) { fMin = fl; minId = pid; }
                if (fl > fMax) { fMax = fl; maxId = pid; }
            }
            ACameraMetadata_free(pc);
        }
        // 主摄 = 非（最短/最长）的那一个；成员只有 2 个时主摄 = 较长焦者（无长焦）
        if (fmap.size() >= 3) {
            for (auto& [pid, fl] : fmap) {
                if (pid != minId && pid != maxId) { fMain = fl; break; }
            }
            if (fMain > 0.f && maxId != minId) {
                traits_.telePhysicalId = maxId;
                traits_.teleNativeZoom = fMax / fMain;
            }
        } else if (fmap.size() == 2) {
            fMain = fMax;
        }
        if (fMain > 0.f) traits_.mainFocal = fMain;
        if (minId.empty()) {
            LOGW("physical lens detection failed (no focal length for members)");
        } else {
            traits_.uwPhysicalId = minId;
            if (traits_.telePhysicalId.empty())
                LOGI("phys layout: uw=%s(%.2fmm) main=%.2fmm (no tele)", minId.c_str(), fMin, fMain);
            else
                LOGI("phys layout: uw=%s(%.2fmm) main=%.2fmm tele=%s(%.2fmm, %.2fx)",
                     minId.c_str(), fMin, fMain, traits_.telePhysicalId.c_str(), fMax,
                     traits_.teleNativeZoom);
        }
    }

    ACameraMetadata_free(chars);
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
