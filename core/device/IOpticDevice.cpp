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

// 默认字体候选：AOSP 常见路径。厂商机型请覆盖（自家 ROM 的中文字体名/格式各异）——
// 每个候选都会被实际 InitFont 探测，失败自动跳过，因此多列无害、漏列才会没字。
std::vector<std::string> IOpticDevice::fontCandidates() const {
    return {
        "/system/fonts/NotoSansCJK-Regular.ttc",   // CFF 轮廓，多数 ROM 会被 stb 拒绝
        "/system/fonts/NotoSansSC-Regular.otf",
        "/system/fonts/NotoSans-Regular.ttf",
        "/system/fonts/Roboto-Regular.ttf",
        "/system/fonts/DroidSans.ttf",
        "/system/fonts/DroidSansFallback.ttf",     // 历史中文备选
    };
}

} // namespace optic::device
