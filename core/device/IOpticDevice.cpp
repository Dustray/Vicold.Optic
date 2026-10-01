#include "core/device/IOpticDevice.h"
#include <sys/system_properties.h>

namespace optic::device {

// 默认机身标识：直接用系统属性，机型层可覆盖为更贴近厂商相机口径的字符串
//（例如把 internal codename 换成市场名，或修正大小写/空格）。
DeviceIdentity IOpticDevice::identity() const {
    DeviceIdentity id;
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get("ro.product.manufacturer", buf) > 0) id.make = buf;
    if (__system_property_get("ro.product.model", buf) > 0) id.model = buf;
    if (id.make.empty()) id.make = "Unknown";
    if (id.model.empty()) id.model = "Unknown";
    return id;
}

} // namespace optic::device
