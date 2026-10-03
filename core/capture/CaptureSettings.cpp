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

    // CAF vs 单次 AF-S：见 CaptureSettings.h 注释（点按走 AF-S 以避免 CAF 的 full sweep）
    uint8_t af = static_cast<uint8_t>(
        afOn ? (afMode == 1 ? ACAMERA_CONTROL_AF_MODE_AUTO
                            : ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE)
             : ACAMERA_CONTROL_AF_MODE_OFF);
    ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AF_MODE, 1, &af);
    if (!afOn) {
        ACaptureRequest_setEntry_float(req, ACAMERA_LENS_FOCUS_DISTANCE, 1, &focusDistance);
    }
    // 触摸对焦/测光区域（active array 域）：逻辑请求按逻辑阵列解释，CamX SAT 会把
    // 区域换算到当前 backing 物理摄（无缝变焦的核心职责之一），跨带无需分带写。
    // 区域恒在用户可见 FOV 内（引擎换算保证），变焦后超出 crop 的部分 HAL 自行钳制。
    // 每项 5 个 int32（x,y,w,h,weight）—— camera2 metering rectangle 编码，缺 weight
    // 会被 HAL 整组忽略；weight 恒最大值 1000（点按区域权重最高）。
    if (afRegion[2] > 0) {
        ACaptureRequest_setEntry_i32(req, ACAMERA_CONTROL_AF_REGIONS, 5, afRegion);
    }
    if (aeRegion[2] > 0) {
        ACaptureRequest_setEntry_i32(req, ACAMERA_CONTROL_AE_REGIONS, 5, aeRegion);
    }
    // AF 触发：仅单帧 trigger 请求携带（写进 repeating 会每帧重扫，永远合不了焦）
    if (afTrigger > 0) {
        const uint8_t trg = static_cast<uint8_t>(ACAMERA_CONTROL_AF_TRIGGER_START);
        ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AF_TRIGGER, 1, &trg);
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
