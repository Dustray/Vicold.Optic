#pragma once
#include "core/device/IOpticDevice.h"

namespace optic::device {

// Xiaomi 17 Pro（首发定制机型）
// codename 在 M0 getprop 落档后补充到 matches()；按 device=pandora 精确匹配为主。
//
// 本类不是「能力探测器」，而是**真机实测结论的载体**：所有不能用 CameraCharacteristics
// 问出来的 HAL 行为（融合管线坏区边界、物理流继承逻辑裁切、会话流数上限、挖孔几何）
// 都由这里的 profile 声明，业务层不得自带默认值。
class Xiaomi17ProDevice final : public IOpticDevice {
public:
    bool matches(std::string_view deviceName, std::string_view marketName) const override;
    DeviceInfo probe(ACameraManager* manager, const std::string& cameraId,
                     ACameraMetadata* characteristics) override;
    void applyQuirks(std::vector<StreamConfig>& configs) const override;
    std::string tuningFileFor(const std::string& physicalId) const override;

    DeviceIdentity identity() const override;
    ZoomProfile zoomProfile() const override;
    NearTakeoverRule nearTakeover() const override;
    PhysQuirks physQuirks() const override;
    SessionPolicy sessionPolicy() const override;
    UiLayoutPolicy uiLayout() const override;

    const char* name() const override { return "Xiaomi17Pro"; }
};

} // namespace optic::device
