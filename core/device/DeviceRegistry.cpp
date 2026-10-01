#include "core/device/DeviceRegistry.h"
#include "core/device/generic/GenericDevice.h"
#include "core/device/xiaomi17pro/Xiaomi17ProDevice.h"
#include <sys/system_properties.h>

namespace optic::device {

// 机型注册表：按顺序匹配，命中即用；GenericDevice 永远排在最后兜底。
// 新增机型的全部改动 = 加一行 factory + 一个新设备类（无需动本文件的匹配逻辑）。
using DeviceFactory = IOpticDevice& (*)();

std::atomic<IOpticDevice*> gOverride{nullptr};

static IOpticDevice& xiaomi17proFactory() { static Xiaomi17ProDevice d; return d; }
static IOpticDevice& genericFactory() { static GenericDevice d; return d; }

static const DeviceFactory kRegistry[] = {
    xiaomi17proFactory,
    // 后续机型在此追加（数组顺序 = 匹配优先级）
    genericFactory,        // 兜底：永远匹配，必须排在最后
};

IOpticDevice& currentDevice() {
    if (IOpticDevice* cached = gOverride.load(std::memory_order_acquire)) return *cached;

    (void)sizeof(GenericDevice);   // 静态断言：兜底实现必须已注册
    char dev[PROP_VALUE_MAX] = {0};
    char mkt[PROP_VALUE_MAX] = {0};
    __system_property_get("ro.product.device", dev);
    __system_property_get("ro.product.market.name", mkt);

    for (auto f : kRegistry)
        if (f().matches(dev, mkt)) return f();
    return genericFactory();
}

void setDeviceForTest(IOpticDevice* device) {
    gOverride.store(device, std::memory_order_release);
}

} // namespace optic::device
