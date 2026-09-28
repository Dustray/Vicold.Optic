#pragma once
#include "core/device/IOpticDevice.h"

namespace optic::device {

// 全局机型实例：按 ro.product.device / market name 匹配，未命中回退 GenericDevice
IOpticDevice& currentDevice();

} // namespace optic::device
