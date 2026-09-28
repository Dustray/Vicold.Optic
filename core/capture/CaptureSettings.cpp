#include "core/capture/CaptureSettings.h"

namespace optic::capture {

void CaptureSettings::apply(ACaptureRequest* req) const {
    int32_t ae = aeOn ? ACAMERA_CONTROL_AE_MODE_ON : ACAMERA_CONTROL_AE_MODE_OFF;
    ACaptureRequest_setEntry_i32(req, ACAMERA_CONTROL_AE_MODE, 1, &ae);

    if (!aeOn) {
        ACaptureRequest_setEntry_i32(req, ACAMERA_SENSOR_SENSITIVITY, 1, &iso);
        ACaptureRequest_setEntry_i64(req, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &exposureNs);
    }

    int32_t af = afOn ? ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE : ACAMERA_CONTROL_AF_MODE_OFF;
    ACaptureRequest_setEntry_i32(req, ACAMERA_CONTROL_AF_MODE, 1, &af);
    if (!afOn) {
        ACaptureRequest_setEntry_float(req, ACAMERA_LENS_FOCUS_DISTANCE, 1, &focusDistance);
    }

    int32_t awb = awbOn ? ACAMERA_CONTROL_AWB_MODE_AUTO : ACAMERA_CONTROL_AWB_MODE_OFF;
    ACaptureRequest_setEntry_i32(req, ACAMERA_CONTROL_AWB_MODE, 1, &awb);

    if (zoomRatio > 0.f) {
        ACaptureRequest_setEntry_float(req, ACAMERA_CONTROL_ZOOM_RATIO, 1, &zoomRatio);
    }
}

} // namespace optic::capture
