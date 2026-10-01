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
    const char* physicalId = nullptr; // 物理直连输出的目标物理摄像头 ID（nullptr = 逻辑输出）
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

// 机身标识：写进 DNG 的 Make / Model / UniqueCameraModel
struct DeviceIdentity {
    std::string make;      // 厂商（DNG tag 0x010F）
    std::string model;     // 机型名（0x0110 / 0xC614）
};

// ---- 变焦口径 ----
// 逻辑多摄的融合管线通常有「坏区」（见 doc/devices/xiaomi17pro/CAM_PATHS.md §2），
// 超过某倍率就持续断流；越过 bad 区就不再是 repressed 由应用层选择，而是要在这里
// 给出「本机实测的安全上限 / 用户口径接管点 / 导轨关键焦段 / mm 读数基准」。
struct ZoomProfile {
    float rangeMin = 0.7f;            // 导轨量程下限（= HAL zoomRatioRange 下限）
    float rangeMax = 120.f;           // 导轨量程上限（数字裁切口径，非 HAL zoomMax）
    float logicalSafeMax = 2.5f;      // 逻辑融合流能健康出帧的最大倍率
    float teleSwitchUser = 5.0f;      // 用户口径长焦接管点（应与本机系统相机一致）
    float logicalFallbackMax = 2.5f;  // 无长焦机型时的兜底上限（:606 曾用魔法数 2.5）
    float baseEquivMm = 24.f;         // 1x 对应的 35mm 等效焦距（mm 读数换算基准）
    std::vector<float> stops{0.7f, 1, 2, 5, 10, 50, 120};   // 导轨关键焦段（等距刻度）
};

// ---- 近距接管规则 ----
// 长焦模组的最小对焦距离通常远大于主摄，距离过近时接管会拉风箱。厂商相机的
// 做法是「近距时推迟接管」，阈值随机型而异 —— 必须可覆盖。
struct NearTakeoverRule {
    bool enabled = false;             // 默认关闭：只有确认真机行为才启用
    float enterM = 0.9f;              // < 此距离进入近距
    float exitM = 1.4f;               // > 此距离退出（滞回）
    float deferredSwitch = 20.f;      // 近距时接管点推迟到的倍率（0 = 不接管）
    int debounceFrames = 12;          // 去抖帧数（@30fps）
};

// ---- 物理直连流的语义 quirk ----
// 物理流不是独立摄像头帧：HAL 常常让它们**继承逻辑请求的某些键**，pandora 就继承
// ZOOM_RATIO 的裁切（长焦带逻辑写 4.85 → 长焦实际 ≈24x）。这个行为无法从
// CameraCharacteristics 探测出来，只能由机型层声明。
struct PhysQuirks {
    bool inheritsLogicalZoom = false;   // 物理流是否继承逻辑 ZOOM_RATIO 的裁切
    bool inheritsLogicalCropRegion = false;
    bool perKeyZoom = true;             // per-physical ZOOM_RATIO/CROP 是否被真实执行
    bool perKeyExposure = true;         // per-physical 曝光/ISO/AWB 键是否被真实执行
};

// ---- 会话 / 多流策略 ----
struct SessionPolicy {
    int previewSlots = 1;           // 常驻预览输出数（三摄机型 = 3）
    int maxStreams = 4;             // HAL 实测可接受的会话输出总数上限
    std::vector<int> degradeSteps{4};  // 降级阶梯：被拒时依次尝试的输出数
    bool allowSingleStreamFallback = true;
    int previewW = 1920, previewH = 1440;   // 预览尺寸（须在本机 preview-size 列表内）
    // 帧率必须显式钉死：不写 AE_TARGET_FPS_RANGE 时 HAL 通常按 TEMPLATE_PREVIEW 取到
    // 最大档（本机三摄常驻，跑满箇 = ISP/GPU 带宽与发热直接翻倍）。
    int targetFpsMin = 30, targetFpsMax = 30;
};

// ---- UI 布局约束（挖孔等物理几何）----
// 屏幕开孔位置/大小是硬件属性。UI 布局只能从机型层拿到「禁区边界」，
// 否则同一套常量在刘海屏/居中挖孔/无孔机型上必然出错。
struct UiLayoutPolicy {
    float cutoutSafeX = 0.f;      // 设计坐标下左侧不可用区的右边界（0 = 无左侧禁区）
    float cutoutReserveW = 0.f;   // 横向 overflow 时的避让带宽
    bool hasLeftCutout = false;   // 是否在左缘有开孔（决定面板是否要避让）
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

    // 机身标识（写进 DNG Make/Model）。默认由 ro.product.* 推导，机型层可覆盖为
    // 更贴近厂商相机口径的字符串。
    virtual DeviceIdentity identity() const;

    // 变焦口径：坏区安全上限、接管点、关键焦段、mm 读数基准（见 ZoomProfile 注释）
    virtual ZoomProfile zoomProfile() const { return ZoomProfile{}; }

    // 近距推迟接管规则（长焦最小对焦距离的限制）
    virtual NearTakeoverRule nearTakeover() const { return NearTakeoverRule{}; }

    // 物理直连流的语义 quirk（无法从 characteristics 探测，只能声明）
    virtual PhysQuirks physQuirks() const { return PhysQuirks{}; }

    // 会话输出数与降级阶梯（HAL 资源上限实测值）
    virtual SessionPolicy sessionPolicy() const { return SessionPolicy{}; }

    // UI 布局约束：挖孔禁区等屏幕物理几何
    virtual UiLayoutPolicy uiLayout() const { return UiLayoutPolicy{}; }

    // per-sensor tuning 配置路径（M4 起使用）
    virtual std::string tuningFileFor(const std::string& physicalId) const { (void)physicalId; return {}; }

    // per-physical 请求键（ZOOM_RATIO / SCALER_CROP_REGION）是否被 HAL 真实执行。
    // 部分机型（pandora/CamX）在 availableRequestKeys 里声明支持但实际忽略
    //（2026-09-29 真机确诊：长焦带 3.0↔8.0 预览逐像素零差异），此时物理带内
    // 变焦回退 GL 数字裁切（见 CameraEngine/Ui）。
    // 注：语义并入 PhysQuirks::perKeyZoom，保留此方法仅为兼容既有调用点。
    bool physPerKeyZoom() const { return physQuirks().perKeyZoom; }

    virtual const char* name() const = 0;
};

} // namespace optic::device
