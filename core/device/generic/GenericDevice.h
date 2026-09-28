#pragma once
#include "core/device/IOpticDevice.h"

namespace optic::device {

// 未识别机型的兜底实现：原样透传能力，不做任何修正
class GenericDevice final : public IOpticDevice {
public:
    bool matches(std::string_view deviceName, std::string_view marketName) const override;
    DeviceInfo probe(ACameraManager* manager, const std::string& cameraId,
                     ACameraMetadata* characteristics) override;
    void applyQuirks(std::vector<StreamConfig>& configs) const override;
    const char* name() const override { return "Generic"; }
};

} // namespace optic::device
