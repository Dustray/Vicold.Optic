#pragma once
// Vicold.Optic 机型扩展层核心接口（doc/PLAN.md §2.2）
// 规则：所有机型差异必须收敛到 device/ 层，业务代码禁止出现机型判断。

#include <string>
#include <string_view>
#include <vector>

struct ACameraManager;      // <camera/NdkCameraManager.h>
struct ACameraMetadata;     // <camera/NdkCameraMetadata.h>

namespace optic::device {

// 会话流配置；applyQuirks 在会话创建前对它做机型修正
struct StreamConfig {
    int32_t width = 0;
    int32_t height = 0;
    int32_t format = 0;              // AIMAGE_FORMAT_*
    int32_t fpsMin = 0;
    int32_t fpsMax = 0;
};

// 机型能力快照（由 probe 从 CameraCharacteristics 提取）
struct DeviceInfo {
    std::string cameraId;
    int hardwareLevel = -1;          // ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL
    bool rawSensor = false;          // REQUEST_AVAILABLE_CAPABILITIES 含 RAW
    bool manualSensor = false;
    bool readSensorSettings = false;
    bool tenBit = false;             // DYNAMIC_RANGE_TEN_BIT
    int32_t sensorOrientation = 0;
    int32_t pixelArrayW = 0;
    int32_t pixelArrayH = 0;
};

class IOpticDevice {
public:
    virtual ~IOpticDevice() = default;

    // 是否匹配当前机型（ro.product.device / ro.product.market.name）
    virtual bool matches(std::string_view deviceName, std::string_view marketName) const = 0;

    // 能力探测：读取机型 quirks / 提炼 DeviceInfo
    virtual DeviceInfo probe(ACameraManager* manager, const std::string& cameraId,
                             ACameraMetadata* characteristics) = 0;

    // 对将要创建的会话流配置做机型修正（方向、时序、可用格式等）
    virtual void applyQuirks(std::vector<StreamConfig>& configs) const = 0;

    // per-sensor tuning 配置路径（M4 起使用）
    virtual std::string tuningFileFor(const std::string& physicalId) const { (void)physicalId; return {}; }

    virtual const char* name() const = 0;
};

} // namespace optic::device
