#include "core/capture/CameraEngine.h"

#include <android/api-level.h>
#include <android/log.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImage.h>
#include <sys/system_properties.h>

#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>

#include "core/device/DeviceRegistry.h"

#define LOG_TAG "Optic"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace optic::capture {
namespace {

constexpr int kStatsEveryNFrames = 120;

const char* capName(int v) {
    switch (v) {
        case 0: return "BACKWARD_COMPATIBLE";
        case 1: return "MANUAL_SENSOR";
        case 2: return "MANUAL_POST_PROCESSING";
        case 3: return "RAW";
        case 4: return "PRIVATE_REPROCESSING";
        case 5: return "READ_SENSOR_SETTINGS";
        case 6: return "BURST_CAPTURE";
        case 7: return "YUV_REPROCESSING";
        case 8: return "DEPTH_OUTPUT";
        case 9: return "CONSTRAINED_HIGH_SPEED_VIDEO";
        case 10: return "MOTION_TRACKING";
        case 11: return "LOGICAL_MULTI_CAMERA";
        case 12: return "MONOCHROME";
        case 13: return "SECURE_IMAGE_DATA";
        case 14: return "SYSTEM_CAMERA";
        case 15: return "OFFLINE_PROCESSING";
        case 16: return "ULTRA_HIGH_RESOLUTION_SENSOR";
        case 17: return "REMOTE_REMOTING";
        case 18: return "DYNAMIC_RANGE_TEN_BIT";
        case 19: return "STREAM_USE_CASE";
        case 20: return "COLOR_SPACE_PROFILES";
        default: return "UNKNOWN";
    }
}

const char* hwLevelName(int v) {
    switch (v) {
        case 0: return "LIMITED";
        case 1: return "FULL";
        case 2: return "LEGACY";
        case 3: return "LEVEL_3";
        case 4: return "EXTERNAL";
        default: return "UNKNOWN";
    }
}

const char* fmtName(int f) {
    switch (f) {
        case 0x20: return "RAW_SENSOR";
        case 0x25: return "RAW10";
        case 0x26: return "RAW12";
        case 0x22: return "PRIVATE/IMPL_DEF";
        case 0x23: return "YUV_420_888";
        case 0x100: return "JPEG/BLOB";
        case 1: return "RGBA_8888";
        case 2: return "RGBX_8888";
        default: return "OTHER";
    }
}

std::string propName(const char* key) {
    char buf[PROP_VALUE_MAX] = {0};
    __system_property_get(key, buf);
    return buf;
}

} // namespace

struct CameraEngine::Impl {
    ANativeWindow* window = nullptr;
    std::string reportPath;
    std::string deviceId;

    ACameraManager* manager = nullptr;
    ACameraDevice* device = nullptr;
    ACameraCaptureSession* session = nullptr;
    ACaptureRequest* request = nullptr;
    ACaptureSessionOutput* output = nullptr;
    ACameraOutputTarget* outputTarget = nullptr;
    ACaptureSessionOutputContainer* container = nullptr;

    ACameraDevice_stateCallbacks devCb{};
    ACameraCaptureSession_stateCallbacks sessCb{};
    ACameraCaptureSession_captureCallbacks capCb{};

    std::ofstream report;
    std::mutex reportMutex;
    uint64_t frameCount = 0;
    int64_t firstTs = 0;
    int64_t lastTs = 0;

    std::atomic<bool>* stopFlag = nullptr;

    // 同时写 logcat 与报告文件
    void logp(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void probeCamera(const char* cameraId);
    bool pickBackCamera(std::string* outId);
    bool pickPreviewSize(int32_t* w, int32_t* h);
};

void CameraEngine::Impl::logp(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, "OpticProbe", fmt, ap);
    va_end(ap);

    std::string line(512, '\0');
    va_start(ap, fmt);
    int n = vsnprintf(line.data(), line.size(), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    line.resize(static_cast<size_t>(n));
    std::lock_guard<std::mutex> lock(reportMutex);
    if (report.is_open()) report << line << '\n';
}

// V1–V7：单颗摄像头能力逐项打点
void CameraEngine::Impl::probeCamera(const char* cameraId) {
    ACameraMetadata* chars = nullptr;
    camera_status_t st = ACameraManager_getCameraCharacteristics(manager, cameraId, &chars);
    if (st != ACAMERA_OK || chars == nullptr) {
        logp("[ERR] characteristics failed for id=%s status=%d", cameraId, static_cast<int>(st));
        return;
    }

    logp("=== camera id=%s ===", cameraId);

    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(chars,
            ACAMERA_LENS_FACING, &e) == ACAMERA_OK && e.count > 0) {
        static const char* facing[] = {"FRONT", "BACK", "EXTERNAL"};
        int v = e.data.u8[0];
        logp("lens.facing = %d (%s)", v, v <= 2 ? facing[v] : "?");
    }

    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(chars,
            ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL, &e) == ACAMERA_OK && e.count > 0) {
        logp("[V2] info.supportedHardwareLevel = %d (%s)", e.data.u8[0],
             hwLevelName(e.data.u8[0]));
    }

    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(chars,
            ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, &e) == ACAMERA_OK) {
        std::ostringstream caps;
        for (uint32_t i = 0; i < e.count; ++i) {
            // 必须转 int：uint8_t 会被按字符写入，值 0 产生 NUL 截断 %s
            caps << static_cast<int>(e.data.u8[i]) << "(" << capName(e.data.u8[i]) << ")";
            if (i + 1 < e.count) caps << ",";
        }
        logp("[V1] request.availableCapabilities = [%s]", caps.str().c_str());
    }

    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(chars,
            ACAMERA_REQUEST_MAX_NUM_OUTPUT_STREAMS, &e) == ACAMERA_OK && e.count >= 3) {
        logp("[V2] request.maxNumOutputStreams = [RAW=%d, PREVIEW=%d, PROC=%d]",
             e.data.i32[0], e.data.i32[1], e.data.i32[2]);
    }

    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(chars,
            ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE, &e) == ACAMERA_OK && e.count >= 2) {
        logp("[V7] sensor.exposureTimeRange = [%lld, %lld] ns",
             static_cast<long long>(e.data.i64[0]), static_cast<long long>(e.data.i64[1]));
    }
    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(chars,
            ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE, &e) == ACAMERA_OK && e.count >= 2) {
        logp("[V7] sensor.sensitivityRange = [%d, %d] ISO", e.data.i32[0], e.data.i32[1]);
    }
    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(chars,
            ACAMERA_SENSOR_ORIENTATION, &e) == ACAMERA_OK && e.count > 0) {
        logp("[V6] sensor.orientation = %d", e.data.i32[0]);
    }
    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(chars,
            ACAMERA_SENSOR_INFO_PIXEL_ARRAY_SIZE, &e) == ACAMERA_OK && e.count >= 2) {
        logp("sensor.pixelArraySize = %dx%d", e.data.i32[0], e.data.i32[1]);
    }
    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(chars,
            ACAMERA_SENSOR_BLACK_LEVEL_PATTERN, &e) == ACAMERA_OK && e.count >= 4) {
        logp("[V3] sensor.blackLevelPattern = [%d %d %d %d]",
             e.data.i32[0], e.data.i32[1], e.data.i32[2], e.data.i32[3]);
    }
    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(chars,
            ACAMERA_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES, &e) == ACAMERA_OK) {
        std::ostringstream modes;
        for (uint32_t i = 0; i < e.count; ++i) modes << static_cast<int>(e.data.u8[i]) << ",";
        logp("[V4] statistics.lensShadingMapModes = [%s] (1=ON 可用)", modes.str().c_str());
    }
    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(chars,
            ACAMERA_SYNC_MAX_LATENCY, &e) == ACAMERA_OK && e.count > 0) {
        const char* s = e.data.i32[0] == 0 ? "PER_FRAME" : e.data.i32[0] == 1 ? "PARTIAL" : "UNKNOWN";
        logp("sync.maxLatency = %d (%s)", e.data.i32[0], s);
    }
    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(chars,
            ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &e) == ACAMERA_OK && e.count > 0) {
        logp("lens.minimumFocusDistance = %.2f D", static_cast<double>(e.data.f[0]));
    }

    // 流配置：统计各格式 OUTPUT 流数量与 RAW/预览可用尺寸
    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(chars,
            ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) == ACAMERA_OK) {
        int rawOut = 0, yuvOut = 0, privOut = 0, blobOut = 0;
        int32_t bestW = 0, bestH = 0; // 预览候选：YUV/PRIV 中 ≤1920 宽的最大者
        for (uint32_t i = 0; i + 3 < e.count; i += 4) {
            int32_t fmt = e.data.i32[i], w = e.data.i32[i + 1],
                    h = e.data.i32[i + 2], isInput = e.data.i32[i + 3];
            if (isInput != 0) continue;
            switch (fmt) {
                case 0x20: case 0x25: case 0x26: ++rawOut; break;
                case 0x23: ++yuvOut; break;
                case 0x22: ++privOut; break;
                case 0x100: ++blobOut; break;
                default: break;
            }
            if ((fmt == 0x23 || fmt == 0x22) && w <= 1920 && w * h > bestW * bestH) {
                bestW = w; bestH = h;
            }
        }
        logp("[V1/V2] streamConfigs: RAW_out=%d YUV_out=%d PRIV_out=%d JPEG_out=%d (preview candidate %dx%d)",
             rawOut, yuvOut, privOut, blobOut, bestW, bestH);

        // 列出 RAW 输出尺寸
        std::ostringstream raws;
        for (uint32_t i = 0; i + 3 < e.count; i += 4) {
            int32_t fmt = e.data.i32[i], w = e.data.i32[i + 1],
                    h = e.data.i32[i + 2], isInput = e.data.i32[i + 3];
            if (isInput == 0 && (fmt == 0x20 || fmt == 0x25 || fmt == 0x26)) {
                raws << fmtName(fmt) << ":" << w << "x" << h << " ";
            }
        }
        if (!raws.str().empty()) logp("[V1] RAW output sizes: %s", raws.str().c_str());
    }

    // 物理摄像头（逻辑多摄）：NDK 不暴露枚举接口，V5 由 dumpsys 存档覆盖
    ACameraMetadata_free(chars);
}

bool CameraEngine::Impl::pickBackCamera(std::string* outId) {
    ACameraIdList* ids = nullptr;
    if (ACameraManager_getCameraIdList(manager, &ids) != ACAMERA_OK || ids == nullptr) return false;
    bool found = false;
    for (int i = 0; i < ids->numCameras && !found; ++i) {
        ACameraMetadata* chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(manager, ids->cameraIds[i], &chars) != ACAMERA_OK)
            continue;
        if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(chars,
                ACAMERA_LENS_FACING, &e) == ACAMERA_OK && e.count > 0 &&
                e.data.u8[0] == ACAMERA_LENS_FACING_BACK) {
            *outId = ids->cameraIds[i];
            found = true;
        }
        ACameraMetadata_free(chars);
    }
    ACameraManager_deleteCameraIdList(ids);
    return found;
}

// 从 YUV/PRIVATE 输出流里挑 ≤1920 宽的最大尺寸作预览
bool CameraEngine::Impl::pickPreviewSize(int32_t* w, int32_t* h) {
    if (deviceId.empty()) return false;
    ACameraMetadata* chars = nullptr;
    if (ACameraManager_getCameraCharacteristics(manager, deviceId.c_str(), &chars) != ACAMERA_OK)
        return false;
    bool ok = false;
    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(chars,
            ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) == ACAMERA_OK) {
        int32_t bestW = 0, bestH = 0;
        for (uint32_t i = 0; i + 3 < e.count; i += 4) {
            int32_t fmt = e.data.i32[i], sw = e.data.i32[i + 1],
                    sh = e.data.i32[i + 2], isInput = e.data.i32[i + 3];
            if (isInput == 0 && (fmt == 0x23 || fmt == 0x22) &&
                sw <= 1920 && sw * sh > bestW * bestH) {
                bestW = sw; bestH = sh;
            }
        }
        if (bestW > 0) { *w = bestW; *h = bestH; ok = true; }
    }
    ACameraMetadata_free(chars);
    return ok;
}

// ---- 静态回调 ----

static void onDeviceDisconnected(void*, ACameraDevice*) {
    LOGE("camera device disconnected");
}

static void onDeviceError(void*, ACameraDevice*, int error) {
    LOGE("camera device error=%d", error);
}

static void onSessionClosed(void*, ACameraCaptureSession*) { LOGI("session closed"); }
static void onSessionReady(void*, ACameraCaptureSession*) { LOGI("session ready"); }
static void onSessionActive(void*, ACameraCaptureSession*) { LOGI("session active (streaming)"); }

static void onCaptureCompleted(void* ctx, ACameraCaptureSession*, ACaptureRequest*,
                               const ACameraMetadata* result) {
    auto* s = static_cast<CameraEngine::Impl*>(ctx);

    int64_t ts = 0;
    if (ACameraMetadata_const_entry e{}; ACameraMetadata_getConstEntry(result,
            ACAMERA_SENSOR_TIMESTAMP, &e) == ACAMERA_OK && e.count > 0) {
        ts = e.data.i64[0];
    }
    ACameraMetadata_free(const_cast<ACameraMetadata*>(result));

    s->frameCount++;
    if (s->firstTs == 0) s->firstTs = ts;
    s->lastTs = ts;

    if (s->frameCount % kStatsEveryNFrames == 0) {
        double span = static_cast<double>(s->lastTs - s->firstTs) / 1e9;
        double fps = span > 0 ? static_cast<double>(s->frameCount - 1) / span : 0.0;
        s->logp("stats: frames=%llu fps=%.1f", static_cast<unsigned long long>(s->frameCount), fps);
    }
}

static void onCaptureFailed(void*, ACameraCaptureSession*, ACaptureRequest*,
                            ACameraCaptureFailure* failure) {
    LOGE("capture failed reason=%d wasImageCaptured=%d", failure->reason,
         failure->wasImageCaptured ? 1 : 0);
}

// ---- 主流程 ----

void CameraEngine::run(ANativeWindow* window, std::string reportDir) {
    auto* s = impl_.get();
    s->window = window;
    s->reportPath = reportDir + "/capabilities.txt";
    s->report.open(s->reportPath, std::ios::trunc);
    if (!s->report.is_open()) LOGE("cannot open report file: %s", s->reportPath.c_str());
    s->stopFlag = &stopRequested_;

    s->logp("=== Vicold.Optic M0 probe ===");
    s->logp("device=%s market=%s registry=%s", propName("ro.product.device").c_str(),
            propName("ro.product.market.name").c_str(),
            device::currentDevice().name());
    s->logp("apiLevel=%d", android_get_device_api_level());

    s->manager = ACameraManager_create();
    if (!s->manager) { LOGE("ACameraManager_create failed"); return; }

    // 1. 枚举并探测所有摄像头
    ACameraIdList* ids = nullptr;
    if (ACameraManager_getCameraIdList(s->manager, &ids) == ACAMERA_OK && ids) {
        for (int i = 0; i < ids->numCameras; ++i) s->probeCamera(ids->cameraIds[i]);
        ACameraManager_deleteCameraIdList(ids);
    }

    // 2. 选后置
    std::string camId;
    if (!s->pickBackCamera(&camId)) {
        LOGE("no back camera found");
        s->logp("[ERR] no back camera");
        return;
    }
    s->deviceId = camId;
    s->logp("selected back camera id=%s", camId.c_str());

    // 3. 机型层探测（DeviceInfo 由 device/ 层提炼）
    {
        ACameraMetadata* chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(s->manager, camId.c_str(), &chars) == ACAMERA_OK) {
            auto info = device::currentDevice().probe(s->manager, camId, chars);
            s->logp("device.probe: hwLevel=%d raw=%d manual=%d readSettings=%d tenBit=%d orientation=%d pixels=%dx%d",
                    info.hardwareLevel, static_cast<int>(info.rawSensor),
                    static_cast<int>(info.manualSensor), static_cast<int>(info.readSensorSettings),
                    static_cast<int>(info.tenBit), info.sensorOrientation,
                    info.pixelArrayW, info.pixelArrayH);
            ACameraMetadata_free(chars);
        }
    }

    // 4. 预览尺寸 + 窗口几何
    int32_t pw = 0, ph = 0;
    if (s->pickPreviewSize(&pw, &ph)) {
        s->logp("preview geometry: %dx%d", pw, ph);
        ANativeWindow_setBuffersGeometry(window, pw, ph, 0);
    }
    ANativeWindow_acquire(window);

    // 5. 打开相机
    {
        auto& cb = s->devCb;
        cb.context = s;
        cb.onDisconnected = onDeviceDisconnected;
        cb.onError = onDeviceError;
        cb.onClientSharedAccessPriorityChanged = nullptr;
    }
    if (ACameraManager_openCamera(s->manager, camId.c_str(), &s->devCb, &s->device) != ACAMERA_OK ||
        !s->device) {
        LOGE("openCamera failed (permission?)");
        s->logp("[ERR] openCamera failed");
        return;
    }

    // 6. 会话：单个 Surface 输出（直出预览）
    ACaptureSessionOutputContainer_create(&s->container);
    ACaptureSessionOutput_create(window, &s->output);
    ACaptureSessionOutputContainer_add(s->container, s->output);
    ACameraOutputTarget_create(window, &s->outputTarget);

    {
        auto& cb = s->sessCb;
        cb.context = s;
        cb.onClosed = onSessionClosed;
        cb.onReady = onSessionReady;
        cb.onActive = onSessionActive;
    }
    if (ACameraDevice_createCaptureSession(s->device, s->container, &s->sessCb, &s->session) !=
        ACAMERA_OK) {
        LOGE("createCaptureSession failed");
        s->logp("[ERR] createCaptureSession failed");
        return;
    }

    // 7. 重复请求
    ACameraDevice_createCaptureRequest(s->device, TEMPLATE_PREVIEW, &s->request);
    ACaptureRequest_addTarget(s->request, s->outputTarget);

    {
        auto& cb = s->capCb;
        cb.context = s;
        cb.onCaptureCompleted = onCaptureCompleted;
        cb.onCaptureFailed = onCaptureFailed;
    }
    int seqId = 0;
    ACaptureRequest* reqArr[1] = {s->request};
    if (ACameraCaptureSession_setRepeatingRequest(s->session, &s->capCb, 1, reqArr, &seqId) !=
        ACAMERA_OK) {
        LOGE("setRepeatingRequest failed");
        s->logp("[ERR] setRepeatingRequest failed");
        return;
    }
    s->logp("preview: repeating request started (seq=%d)", seqId);
    LOGI("preview streaming");

    // 8. 预览循环直到窗口销毁
    while (!s->stopFlag->load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    LOGI("preview loop exit");
}

CameraEngine::CameraEngine() : impl_(std::make_unique<Impl>()) {}
CameraEngine::~CameraEngine() { stop(); }

bool CameraEngine::start(ANativeWindow* window, const std::string& reportDir) {
    if (running_.load(std::memory_order_acquire)) return true;
    stopRequested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this, window, reportDir] { run(window, reportDir); });
    return true;
}

void CameraEngine::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    stopRequested_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();

    auto* s = impl_.get();
    if (s->session) {
        ACameraCaptureSession_stopRepeating(s->session);
        ACameraCaptureSession_close(s->session);
        s->session = nullptr;
    }
    if (s->request) { ACaptureRequest_free(s->request); s->request = nullptr; }
    if (s->outputTarget) { ACameraOutputTarget_free(s->outputTarget); s->outputTarget = nullptr; }
    if (s->output) { ACaptureSessionOutput_free(s->output); s->output = nullptr; }
    if (s->container) { ACaptureSessionOutputContainer_free(s->container); s->container = nullptr; }
    if (s->device) { ACameraDevice_close(s->device); s->device = nullptr; }
    if (s->window) { ANativeWindow_release(s->window); s->window = nullptr; }
    if (s->manager) { ACameraManager_delete(s->manager); s->manager = nullptr; }

    {
        std::lock_guard<std::mutex> lock(s->reportMutex);
        if (s->report.is_open()) {
            s->report << "=== end: frames=" << s->frameCount << " ===" << std::endl;
            s->report.close();
        }
    }
    running_.store(false, std::memory_order_release);
}

} // namespace optic::capture
