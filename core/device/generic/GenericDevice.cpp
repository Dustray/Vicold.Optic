#include "core/device/generic/GenericDevice.h"
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>

namespace optic::device {

bool GenericDevice::matches(std::string_view, std::string_view) const {
    return true; // 兜底，永远匹配（排在注册表最后）
}

DeviceInfo GenericDevice::probe(ACameraManager* manager, const std::string& cameraId,
                                ACameraMetadata* characteristics) {
    DeviceInfo info;
    info.cameraId = cameraId;
    (void)manager;

    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(characteristics,
            ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL, &e) == ACAMERA_OK && e.count > 0) {
        info.hardwareLevel = e.data.u8[0];
    }
    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(characteristics,
            ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, &e) == ACAMERA_OK) {
        for (uint32_t i = 0; i < e.count; ++i) {
            switch (e.data.u8[i]) {
                case ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_RAW:                info.rawSensor = true; break;
                case ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR:      info.manualSensor = true; break;
                case ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_READ_SENSOR_SETTINGS: info.readSensorSettings = true; break;
                case 18: // REQUEST_AVAILABLE_CAPABILITIES_DYNAMIC_RANGE_TEN_BIT（NDK 未导出枚举名）
                    info.tenBit = true; break;
                default: break;
            }
        }
    }
    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(characteristics,
            ACAMERA_SENSOR_ORIENTATION, &e) == ACAMERA_OK && e.count > 0) {
        info.sensorOrientation = e.data.i32[0];
    }
    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(characteristics,
            ACAMERA_SENSOR_INFO_PIXEL_ARRAY_SIZE, &e) == ACAMERA_OK && e.count >= 2) {
        info.pixelArrayW = e.data.i32[0];
        info.pixelArrayH = e.data.i32[1];
    }
    return info;
}

void GenericDevice::applyQuirks(std::vector<StreamConfig>&) const {
    // 兜底机型不修正
}

} // namespace optic::device
