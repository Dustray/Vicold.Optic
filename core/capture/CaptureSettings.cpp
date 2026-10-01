#include "core/capture/CaptureSettings.h"

namespace optic::capture {

void CaptureSettings::apply(ACaptureRequest* req, bool skipZoom, bool forPreview) const {
    // 钉死帧率（机型层口径）：三路常驻会话只要不写这个 entry，HAL 就可能跑在最大档。
    // 这是纯收益 —— 预览本来就只要 30fps，多出来的帧全是白烧的带宽和电。
    if (forPreview && fpsMin > 0 && fpsMax > 0) {
        const int32_t fps[2] = {fpsMin, fpsMax};
        ACaptureRequest_setEntry_i32(req, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, fps);
    }

    // 注意：AE_MODE / AF_MODE / AWB_MODE 在 camera2 里是 TYPE_BYTE，必须用 setEntry_u8，
    // 用 i32 写会被框架报 "Mismatched tag type" 并丢弃该 entry（切到超广角物理镜头时尤其致命）。
    uint8_t ae = static_cast<uint8_t>(aeOn ? ACAMERA_CONTROL_AE_MODE_ON : ACAMERA_CONTROL_AE_MODE_OFF);
    ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AE_MODE, 1, &ae);
    uint8_t ael = static_cast<uint8_t>(aeLock ? 1 : 0);
    ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AE_LOCK, 1, &ael);
    ACaptureRequest_setEntry_i32(req, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, 1, &evSteps);

    if (!aeOn) {
        ACaptureRequest_setEntry_i32(req, ACAMERA_SENSOR_SENSITIVITY, 1, &iso);
        ACaptureRequest_setEntry_i64(req, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &exposureNs);
    }

    uint8_t af = static_cast<uint8_t>(afOn ? ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE : ACAMERA_CONTROL_AF_MODE_OFF);
    ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AF_MODE, 1, &af);
    if (!afOn) {
        ACaptureRequest_setEntry_float(req, ACAMERA_LENS_FOCUS_DISTANCE, 1, &focusDistance);
    }

    uint8_t awb = static_cast<uint8_t>(awbOn ? ACAMERA_CONTROL_AWB_MODE_AUTO : ACAMERA_CONTROL_AWB_MODE_OFF);
    ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AWB_MODE, 1, &awb);

    // 超广角直连物理摄像头时不写 ZOOM_RATIO：物理镜头本身就是最宽 FOV，写 <1.0 反而会让 HAL
    // 试图在物理请求上套用逻辑缩放、重新触发已损坏的融合管线；裁剪区默认取物理传感器全幅即可。
    if (zoomRatio > 0.f && !skipZoom) {
        ACaptureRequest_setEntry_float(req, ACAMERA_CONTROL_ZOOM_RATIO, 1, &zoomRatio);
    }
}

} // namespace optic::capture
