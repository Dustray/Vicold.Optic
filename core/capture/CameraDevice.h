#pragma once
// M1.1：ACameraManager / ACameraDevice 的 RAII 封装 + 关键 characteristics 快照。
// 所有对象生命周期明确：CameraDevice 析构 = close device + delete manager。

#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>

#include <functional>
#include <string>
#include <vector>

namespace optic::capture {

struct CameraTraits {
    std::string id;
    bool logical = false;
    std::vector<std::string> physicalIds;   // 逻辑摄的物理成员（V5）
    int hardwareLevel = -1;
    bool raw = false;
    int32_t sensorOrientation = 0;
    int32_t pixelW = 0, pixelH = 0;
    int64_t exposureMinNs = 0, exposureMaxNs = 0;
    int32_t isoMin = 0, isoMax = 0;
    float zoomMin = 1.f, zoomMax = 1.f;
    bool hasZoomRatio = false;
    int32_t focusMinD = 0;                  // 最小对焦距离（屈光度，0=定焦）
    int32_t blackLevel[4] = {0, 0, 0, 0};   // DNG 写入用（V3，运行时从 characteristics 填充）
    uint8_t cfaPattern[4] = {0, 1, 1, 2};   // CFA 图案（默认 RGGB，运行时按枚举映射）
    bool lscOn = false;                     // per-frame LSC map 可用（V4）
};

class CameraDevice {
public:
    CameraDevice() = default;
    ~CameraDevice() { close(); }
    CameraDevice(const CameraDevice&) = delete;
    CameraDevice& operator=(const CameraDevice&) = delete;

    // 枚举后置摄像头（M1.4：只取第一个后置逻辑摄；物理摄经 characteristics 读取）
    bool openFirstBack();
    void close();

    bool opened() const { return device_ != nullptr; }
    ACameraDevice* handle() const { return device_; }
    ACameraManager* manager() const { return manager_; }
    const CameraTraits& traits() const { return traits_; }
    const std::string& deviceId() const { return traits_.id; }

    // 断流/错误 → 引擎触发重连
    std::function<void()> onLost;
    std::function<void(int)> onDeviceError;

private:
    static void onDisconnected(void* ctx, ACameraDevice* dev);
    static void onError(void* ctx, ACameraDevice* dev, int error);

    bool readTraits(const char* id);

    ACameraManager* manager_ = nullptr;
    ACameraDevice* device_ = nullptr;
    ACameraDevice_stateCallbacks cbs_{};
    CameraTraits traits_;
    std::string lastError_;
};

} // namespace optic::capture
