#include "core/device/xiaomi17pro/Xiaomi17ProDevice.h"
#include "core/device/generic/GenericDevice.h"
#include <cctype>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>

namespace optic::device {

static std::string toLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// M0 真机确认：Xiaomi 17 Pro 的 ro.product.device = pandora，Android 16 (API 36)。
// 市场名必须带品牌前缀 —— 只判 "17 pro" 裸子串会误命中其它品牌的同名机型。
bool Xiaomi17ProDevice::matches(std::string_view deviceName, std::string_view marketName) const {
    if (deviceName == "pandora") return true;
    const std::string m = toLower(marketName);
    if (m.find("xiaomi") == std::string::npos) return false;
    return m.find("17 pro") != std::string::npos;
}

DeviceInfo Xiaomi17ProDevice::probe(ACameraManager* manager, const std::string& cameraId,
                                    ACameraMetadata* characteristics) {
    // 基础能力提取与通用机型一致；17 Pro 特有的怪癖见下方各 profile
    static GenericDevice generic;
    DeviceInfo info = generic.probe(manager, cameraId, characteristics);
    return info;
}

void Xiaomi17ProDevice::applyQuirks(std::vector<StreamConfig>& configs) const {
    // 本机无需在此修正流配置：方向/镜像由 HAL 报告且实测正确；RAW 仅 1 条逻辑流的
    // 限制体现在 SessionPolicy::maxStreams。保留 Hook 供后续机型差异使用。
    (void)configs;
}

std::string Xiaomi17ProDevice::tuningFileFor(const std::string& physicalId) const {
    // M4：per-sensor tuning JSON，路径形如 device/xiaomi17pro/tuning_<physicalId>.json
    return "device/xiaomi17pro/tuning_" + physicalId + ".json";
}

// ---- 以下是 pandora 在真机上实测到的全部机型差异。此前它们硬编码在 CameraEngine /
// Ui 的常量与内联分支里（连 Jupyter 「悬挂的判断」都没写），机型层反而无法表达 ----

DeviceIdentity Xiaomi17ProDevice::identity() const {
    DeviceIdentity id = IOpticDevice::identity();
    if (id.make.empty() || id.make == "Unknown") id.make = "Xiaomi";
    id.model = "Xiaomi 17 Pro";
    return id;
}

ZoomProfile Xiaomi17ProDevice::zoomProfile() const {
    ZoomProfile p;
    p.rangeMin = 0.7f;             // HAL zoomRatioRange 下限
    p.rangeMax = 120.f;            // 数字裁切口径（与 MIUI 相机一致）
    p.logicalSafeMax = 4.85f;      // 2026-09-30 stall_probe：4.8 干净、5.0 起持续断流
    p.teleSwitchUser = 5.0f;       // 用户口径接管点（系统相机同口径）
    p.logicalFallbackMax = 2.5f;   // 无长焦时的兜底
    p.baseEquivMm = 23.f;          // 本机主摄 35mm 等效焦距（1x = 23mm）
    p.stops = {0.7f, 1, 2, 5, 10, 50, 120};
    return p;
}

NearTakeoverRule Xiaomi17ProDevice::nearTakeover() const {
    NearTakeoverRule r;
    r.enabled = true;              // 对齐系统相机：近距时 1–20x 恒主摄
    r.enterM = 0.9f;
    r.exitM = 1.4f;
    r.deferredSwitch = 20.f;
    r.debounceFrames = 12;         // ~0.4s @30fps
    return r;
}

PhysQuirks Xiaomi17ProDevice::physQuirks() const {
    PhysQuirks q;
    // quirk 之一（2026-09-29）：per-physical ZOOM_RATIO / SCALER_CROP_REGION 被 CamX
    // 声明支持但实际忽略（长焦带 3.0↔8.0 预览逐像素零差异）→ 物理带内变焦走 GL crop。
    q.perKeyZoom = false;
    // quirk 之二（2026-09-30）：物理流**继承逻辑请求的 ZOOM_RATIO 裁切** ——
    // 长焦带若逻辑写 4.85，长焦流实际 = 5.016×4.85 ≈ 24x（用户所见「5x 变 25x」）。
    // 故进入物理带时必须把逻辑 zoom 写回 1.0。
    q.inheritsLogicalZoom = true;
    // quirk 之三（2026-09-30 实证）：与 ZOOM/CROP 相反，**per-physical 曝光键
    // CamX 照常执行**（AE_MODE/AWB_MODE/EV/SENSITIVITY/EXPOSURE_TIME 逐摄下发后
    // 三摄曝光统一），因此 ALL 请求仍逐摄下发曝光键。
    q.perKeyExposure = true;
    return q;
}

SessionPolicy Xiaomi17ProDevice::sessionPolicy() const {
    SessionPolicy s;
    s.previewSlots = 3;            // 逻辑 + 物理 3(uw) + 物理 4(tele) 三路常驻
    s.maxStreams = 5;              // 真机实测：3 预览 + JPEG + uw JPEG 一次 configure 通过
    s.degradeSteps = {4};          // 5 流被拒时先拆掉 uw JPEG 重试 4 流
    s.allowSingleStreamFallback = true;
    s.previewW = 1920;
    s.previewH = 1440;
    s.targetFpsMin = 30;       // 三路常驻已在吃满 ISP 带宽，不要把自己跑到 60
    s.targetFpsMax = 30;
    return s;
}

UiLayoutPolicy Xiaomi17ProDevice::uiLayout() const {
    UiLayoutPolicy u;
    // dumpsys display：cutout boundingRect.Top = Rect(573,0-647,150)（竖屏顶中）、
    // cutoutSpec "M 0,0 H -37 V 150 H 37 V 0 H 0 Z" → 横屏紧贴左缘、设计坐标 x 0–88。
    // 2026-10-01 三轮实测：X=64 时轨道左侧被真孔吃掉 ⇒ 88 是硬下限。
    u.cutoutSafeX = 88.f;
    u.cutoutReserveW = 80.f;       // 150 设备px 保留带 ÷ 1.7026（设计 px）
    u.hasLeftCutout = true;
    return u;
}

} // namespace optic::device
