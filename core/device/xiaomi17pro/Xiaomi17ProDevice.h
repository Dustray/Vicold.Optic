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
    // 2026-09-29 真机确诊：phys 3/4 的 availableRequestKeys 声明 ZOOM_RATIO+CROP_REGION
    //（range [1,10]），但 per-physical 写入两键均被忽略（长焦带 3.0↔8.0 预览零差异，
    // 而逻辑带 1.2↔2.4 同场景 mean diff 42）。物理带内变焦走 GL 裁切兜底。
    bool physPerKeyZoom() const override { return false; }
    std::string tuningFileFor(const std::string& physicalId) const override;
    const char* name() const override { return "Xiaomi17Pro"; }
};

} // namespace optic::device
