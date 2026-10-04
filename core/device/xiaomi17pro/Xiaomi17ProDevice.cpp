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
    // 成员布局 uw=18.5 / main=23.8 / tele=119.6mm（等效比 0.78 / 1.0 / 5.02）
    p.uwRoleRatio = 0.92f;
    p.teleRoleRatio = 1.25f;
    return p;
}

NearTakeoverRule Xiaomi17ProDevice::nearTakeover() const {
    NearTakeoverRule r;
    // 2026-10-04 真机标定（focus_d 逐档扫描 + display slot 观测）：
    // 系统相机在约 20cm 内才用主摄、超过就用长焦。此前取 0.9m/1.4m 是拍脑袋的
    // 猜测值，比实测口径宽 4.5 倍 —— 1m 内的主体全被判近距，接管点被推到 20x，
    // 于是 5x 永远显示主摄（用户报告「隔一米还是主摄」的根因）。
    // 0.18m 进 / 0.30m 出：与系统相机 20cm 口径一致，且滞回带收窄到 0.12m ——
    // 旧配对 0.9/1.4 的 0.5m 记忆区会把 1m 主体永久锁死在近距态（两计数器清零，
    // 状态既不进也不退）。
    r.enabled = true;
    r.enterM = 0.18f;
    r.exitM = 0.30f;
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
    // 三路预览都用 PRIVATE：本机 YUV/RGBA ImageReader 通路被 HAL 劫持不供帧，
    // 只有 PRIVATE + GPU_SAMPLED_IMAGE → EGLImage（samplerExternalOES）稳定。
    s.previewFormat = AIMAGE_FORMAT_PRIVATE;
    // 三个物理成员都能出 1920×1440（= 0），故副摄不降级：
    // 任一带都可能成为当前显示源，降尺寸会直接牺牲该带的取景清晰度。
    s.auxPreviewW = 0;
    s.auxPreviewH = 0;
    s.targetFpsMin = 30;       // 三路常驻已在吃满 ISP 带宽，不要把自己跑到 60
    s.targetFpsMax = 30;
    // UI 渲染跟着 30fps 预览走即可（不必要只看实拍帧，单纯浪费 GPU 和电）
    s.uiRenderFps = 30;
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

// pandora /product/fonts 下的中文候选。必须**静态 TrueType(glyf)**：
//   MiSansVF.ttf = 可变字体（stb 不支持 gvar → 连笔）、
//   NotoSansCJK.ttc = CFF 轮廓（InitFont 失败）、
//   MiSansC_3.005.ttf = 仅西文子集（CJK 覆盖 0）。
// 运行时按「实际覆盖了多少个 UI 需要的汉字」择优，多列候选无害。
std::vector<std::string> Xiaomi17ProDevice::fontCandidates() const {
    std::vector<std::string> v = IOpticDevice::fontCandidates();   // 通用兜底排在后面
    v.insert(v.begin(), {
        "/product/fonts/FZFWZhuZiAYuanJWB.TTF",   // 本机实测命中（cjk 23/23）
        "/product/fonts/MiSansRoundedSC.ttf",
        "/product/fonts/BeihaibeiSC-Regular.ttf",
        "/product/fonts/MiSansC_3.005.ttf",
        "/product/fonts/MiSansVF.ttf",            // 可变字体，已知可能连笔
        "/system/fonts/MiSansVF.ttf",
    });
    return v;
}

} // namespace optic::device
