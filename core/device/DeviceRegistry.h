#pragma once
#include <atomic>
#include "core/device/IOpticDevice.h"

namespace optic::device {

// 全局机型实例：按 ro.product.device / market name 顺序匹配注册表，未命中回退 GenericDevice。
// 业务层要拿任何机型差异都从这里取，**禁止**在 capture/ ui/ dng/ 里写死机型常量。
IOpticDevice& currentDevice();

// 测试/诊断用：强制指定机型实例（nullptr 恢复自动匹配）
void setDeviceForTest(IOpticDevice* device);
extern std::atomic<IOpticDevice*> gOverride;

} // namespace optic::device
