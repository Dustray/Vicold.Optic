#pragma once
#include "core/device/IOpticDevice.h"

namespace optic::device {

// Xiaomi 17 Pro（首发定制机型）
// codename 在 M0 getprop 落档后补充到 matches()；当前按市场名匹配。
class Xiaomi17ProDevice final : public IOpticDevice {
public:
    bool matches(std::string_view deviceName, std::string_view marketName) const override;
    DeviceInfo probe(ACameraManager* manager, const std::string& cameraId,
                     ACameraMetadata* characteristics) override;
    void applyQuirks(std::vector<StreamConfig>& configs) const override;
    std::string tuningFileFor(const std::string& physicalId) const override;
    const char* name() const override { return "Xiaomi17Pro"; }
};

} // namespace optic::device
