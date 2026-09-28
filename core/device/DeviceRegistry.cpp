#include "core/device/DeviceRegistry.h"
#include "core/device/generic/GenericDevice.h"
#include "core/device/xiaomi17pro/Xiaomi17ProDevice.h"
#include <sys/system_properties.h>

namespace optic::device {

IOpticDevice& currentDevice() {
    static Xiaomi17ProDevice xiaomi17pro;
    static GenericDevice generic;

    char dev[PROP_VALUE_MAX] = {0};
    char mkt[PROP_VALUE_MAX] = {0};
    __system_property_get("ro.product.device", dev);
    __system_property_get("ro.product.market.name", mkt);

    if (xiaomi17pro.matches(dev, mkt)) {
        return xiaomi17pro;
    }
    return generic;
}

} // namespace optic::device
