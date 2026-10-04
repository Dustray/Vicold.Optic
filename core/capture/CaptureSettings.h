#pragma once
// 手动采集参数（M1.3）：应用到 ACaptureRequest 上，逐项可独立覆盖。
// 语义：ae=on 时 iso/expNs 被硬件 AE 覆盖（spec 如此，无需清项）。

#include <camera/NdkCaptureRequest.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace optic::capture {

struct CaptureSettings {
    // AE
    bool aeOn = true;
    bool aeLock = false;          // 测光锁定（AE_LOCK，aeOn=true 时语义完整）
    int32_t iso = 0;              // aeOn=false 时生效
    int64_t exposureNs = 0;       // aeOn=false 时生效
    int32_t evSteps = 0;          // AE 曝光补偿步数（aeOn=true 时生效，范围来自 characteristics）
    // AF
    bool afOn = true;
    float focusDistance = 0.f;    // 屈光度；afOn=false 时生效
    // 0 = CONTINUOUS_PICTURE（CAF，恒定 track）；1 = AUTO（单次 AF-S：trigger 后合焦并锁定）。
    // 点按对焦走 AF-S：本机 CAF 下 trigger 会退化成「镜头退回无穷远 → 从 10m 一路扫到
    // 近处」的 full sweep，起扫就要 1.1s（2026-10-02 实测）；AUTO 模式让 HAL 走
    // 单次 course+fine 流程，通常显著更快。合焦/失败后由引擎定时切回 CAF。
    int32_t afMode = 0;
    // 触摸对焦区域（active array 域，MeteringRectangle 编码）
    // AF/AE 同区：点按既驱动对焦也驱动测光（业界通行语义）。区域恒由引擎按
    // 用户变焦换算到逻辑 active array 坐标（见 CameraEngine::onTapFocus）。
    // **必须 5 元素** [xmin, ymin, xmax, ymax, weight]：camera2 的 metering rectangle 就长这样
    //（NDK 头文件明写 every five elements = (xmin,ymin,xmax,ymax,weight)，矩形左闭右开），
    // weight∈[1,1000] 且 ≠0。**写成 (x,y,w,h) 是致命的**：xmax(=w) < xmin(=x) 时 HAL 会把
    // 矩形钳成退化点（pandora 实测回显 [1740 1228 1740 1228]），等于把统计窗口压成左上角
    // 一个像素 —— 这正是 2026-10-02 用户报「点了不对焦」的真根因。
    int32_t afRegion[5] = {};
    int32_t aeRegion[5] = {};
    // 一次性 AF 触发（CONTROL_AF_TRIGGER，写入 = START）。**只能出现在单帧请求上**：
    // trigger 落在 repeating 请求里会被每个请求实例各执行一次（每帧重启扫描 ⇒ 永不结束），
    // 因此该字段恒由点按后的 captureTrigger 临时请求携带，settings_ 自身保持 0。
    int32_t afTrigger = 0;
    // 一次性 AE 预捕获触发（CONTROL_AE_PRECAPTURE_TRIGGER，写入 = START）。与 afTrigger
    // 同一约束：**只能出现在单帧请求上**。闪光/低光单拍前必须先走这个序列 —— HAL 靠它
    // 启动「闪光预闪测光 → AE 收敛」流程，没有它 AE_MODE=ON_ALWAYS_FLASH 的单拍请求
    // 会被 HAL 直接出帧、灯不亮（pandora 真机实测：亮度对照无差异，flash.state 不进 FIRED）。
    int32_t aePrecapture = 0;
    // AWB
    bool awbOn = true;            // 自动白平衡开关（与 wbManual 互斥：手动偏移开启时恒为 false）
    int awbMode = 1;              // 白平衡预设（Android AWB_MODE 枚举值；1=AUTO）；awbOn=false 时下发 OFF
    // 手动白平衡偏移（2D 坐标板）：进入手动后接管色彩校正，与 AWB 预设互斥。
    // wbTemp = 色温（X 轴）：+ = 暖/琥珀(增 R 减 B)，- = 冷/蓝(增 B 减 R)，范围 [-1,1]
    // wbTint = 色调（Y 轴）：+ = 品红(减 G 增 R/B)，- = 绿(增 G 减 R/B)，范围 [-1,1]
    // 默认 (0,0) = 中性。映射为 COLOR_CORRECTION 增益（见 CaptureSettings.cpp::apply）。
    bool wbManual = false;
    float wbTemp = 0.f;
    float wbTint = 0.f;
    // 闪光灯（自定义档位，不是 camera2 枚举）：0=关 1=自动 2=开（强制） 3=常亮手电筒。
    // **语义落在 CONTROL_AE_MODE 上**（camera2 规矩）：自动=ON_AUTO_FLASH、开=ON_ALWAYS_FLASH，
    // 直接写 FLASH_MODE=SINGLE 而不改 AE_MODE 在多数 HAL 上不会闪。只有「常亮」需要
    // 额外写 FLASH_MODE=TORCH（AE_MODE 没有对应枚举）。详见 CaptureSettings.cpp。
    // 引擎侧会按 traits.flashAvailable 守卫：无闪光灯单元时不该出现非 0 值。
    int flashMode = 0;
    // 变焦（逻辑摄 zoomRatio，HAL 自动做物理摄切换）
    float zoomRatio = 0.f;        // 0 = 不设置（保持默认）

    // 帧率目标（机型层 SessionPolicy 提供）：不写 AE_TARGET_FPS_RANGE 时 HAL 往往按
    // TEMPLATE 取到最大档，三摄常驻下等于把 ISP/DRAM/GPU 负载无条件翻倍。
    int32_t fpsMin = 30, fpsMax = 30;

    // 应用到请求；forPreview=false 用于单拍请求（不限制其帧率策略）
    void apply(ACaptureRequest* req, bool skipZoom = false, bool forPreview = true) const;
};

// 两个设置是否要求重发 repeating（粗粒度：任一字段变化即重发）
inline bool operator!=(const CaptureSettings& a, const CaptureSettings& b) {
    return a.aeOn != b.aeOn || a.aeLock != b.aeLock || a.iso != b.iso ||
           a.exposureNs != b.exposureNs ||
           a.evSteps != b.evSteps || a.afOn != b.afOn || a.focusDistance != b.focusDistance ||
           a.afMode != b.afMode || a.awbOn != b.awbOn || a.awbMode != b.awbMode ||
           a.wbManual != b.wbManual || a.wbTemp != b.wbTemp || a.wbTint != b.wbTint ||
           a.flashMode != b.flashMode || a.zoomRatio != b.zoomRatio ||
           std::memcmp(a.afRegion, b.afRegion, sizeof(a.afRegion)) != 0 ||
           std::memcmp(a.aeRegion, b.aeRegion, sizeof(a.aeRegion)) != 0;
}

} // namespace optic::capture
