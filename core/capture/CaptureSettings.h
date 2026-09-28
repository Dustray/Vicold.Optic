#pragma once
// 手动采集参数（M1.3）：应用到 ACaptureRequest 上，逐项可独立覆盖。
// 语义：ae=on 时 iso/expNs 被硬件 AE 覆盖（spec 如此，无需清项）。

#include <camera/NdkCaptureRequest.h>
#include <cstdint>
#include <string>

namespace optic::capture {

struct CaptureSettings {
    // AE
    bool aeOn = true;
    int32_t iso = 0;              // aeOn=false 时生效
    int64_t exposureNs = 0;       // aeOn=false 时生效
    // AF
    bool afOn = true;
    float focusDistance = 0.f;    // 屈光度；afOn=false 时生效
    // AWB
    bool awbOn = true;            // TODO(M2): CCT 手动（colorTemperature tag 需真机验证）
    // 变焦（逻辑摄 zoomRatio，HAL 自动做物理摄切换）
    float zoomRatio = 0.f;        // 0 = 不设置（保持默认）

    // 应用到请求；返回是否有改动（调用方决定是否重发 repeating）
    void apply(ACaptureRequest* req) const;
};

// 两个设置是否要求重发 repeating（粗粒度：任一字段变化即重发）
inline bool operator!=(const CaptureSettings& a, const CaptureSettings& b) {
    return a.aeOn != b.aeOn || a.iso != b.iso || a.exposureNs != b.exposureNs ||
           a.afOn != b.afOn || a.focusDistance != b.focusDistance || a.awbOn != b.awbOn ||
           a.zoomRatio != b.zoomRatio;
}

} // namespace optic::capture
