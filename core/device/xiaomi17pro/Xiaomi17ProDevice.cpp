#include "core/device/xiaomi17pro/Xiaomi17ProDevice.h"
#include "core/device/generic/GenericDevice.h"
#include <cctype>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>

namespace optic::device {

static std::string toLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// M0 真机确认：Xiaomi 17 Pro 的 ro.product.device = pandora，Android 16 (API 36)
bool Xiaomi17ProDevice::matches(std::string_view deviceName, std::string_view marketName) const {
    return deviceName == "pandora" || toLower(marketName).find("17 pro") != std::string::npos;
}

DeviceInfo Xiaomi17ProDevice::probe(ACameraManager* manager, const std::string& cameraId,
                                    ACameraMetadata* characteristics) {
    // 基础能力提取与通用机型一致；17 Pro 特有的怪癖在 M0 真机验证后填入
    static GenericDevice generic;
    DeviceInfo info = generic.probe(manager, cameraId, characteristics);
    return info;
}

void Xiaomi17ProDevice::applyQuirks(std::vector<StreamConfig>& configs) const {
    // TODO(M0): 真机验证清单 V6（方向/镜像 quirk）、V2（RAW stream 数上限）确认后填入
    (void)configs;
}

std::string Xiaomi17ProDevice::tuningFileFor(const std::string& physicalId) const {
    // M4：per-sensor tuning JSON，路径形如 device/xiaomi17pro/tuning_<physicalId>.json
    return "device/xiaomi17pro/tuning_" + physicalId + ".json";
}

} // namespace optic::device
