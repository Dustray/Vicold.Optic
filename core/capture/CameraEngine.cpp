#include "core/capture/CameraEngine.h"

#include "core/device/DeviceRegistry.h"
#include "core/dng/DngWriter.h"
#include "core/ui/Ui.h"
#include "core/util/Log.h"

#include <camera/NdkCameraMetadata.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace std::chrono_literals;

namespace optic::capture {
namespace {

std::string propName(const char* key) {
    char buf[PROP_VALUE_MAX] = {0};
    __system_property_get(key, buf);
    return buf;
}

bool parseOn(const std::string& v) {
    return v == "on" || v == "1" || v == "true" || v == "yes";
}

} // namespace

CameraEngine::~CameraEngine() {
    stop();
    // 进程退出场景：引擎线程可能还卡在 HAL close（打盹挂起），detach 交给 OS 收尾，
    // 绝不 join（卡住会让析构冻结）
    if (thread_.joinable()) thread_.detach();
}

void CameraEngine::requestStart(const std::string& dataDir) {
    pendingDataDir_ = dataDir;
    wantRun_.store(true, std::memory_order_release);
    tryStart();
}

bool CameraEngine::tryStart() {
    if (!wantRun_.load(std::memory_order_acquire)) return false;
    if (running_.load(std::memory_order_acquire)) return true;
    if (!stopped_.load(std::memory_order_acquire)) return false;   // 旧线程收摊中，下轮再试
    stopFlag_.store(false, std::memory_order_release);
    stopped_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();   // stopped_=true 保证线程已结束，立即返回
    thread_ = std::thread([this, dir = pendingDataDir_] { run(dir); });
    LOGI("engine started (dataDir=%s)", pendingDataDir_.c_str());
    return true;
}

void CameraEngine::stop() {
    wantRun_.store(false, std::memory_order_release);
    stopFlag_.store(true, std::memory_order_release);
    // 收摊（closeSession + camera close）由引擎线程在 run 尾部完成 —— 本函数不等待。
    // running_/stopped_ 由引擎线程翻转，主线程经 tryStart 观察后才能再启动。
}

void CameraEngine::run(std::string dataDir) {
    dataDir_ = std::move(dataDir);
    // 每轮运行复位看门狗/重连状态（成员跨引擎生命周期保留，残留会让新运行
    // 误报 preview stall 或开局就撞重连上限 —— 2026-09-30 真机复现）
    lastResultMs_ = 0;
    stallRetries_ = 0;
    reconnects_ = 0;
    ctl_ = std::make_unique<util::ControlFile>(dataDir_ + "/controls.txt",
                                               std::chrono::milliseconds(400));

    // 开机前先扫一遍 controls.txt：cam= 强制摄像头 ID（诊断用，必须在 openFirstBack 之前）、
    // fmt=raw 强制 RAW/DNG（默认拍摄格式是 JPEG → 系统相册）
    {
        std::ifstream f(dataDir_ + "/controls.txt");
        std::string line;
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "cam" && forcedCamId_.empty()) forcedCamId_ = v;
            if (k == "fmt") jpgMode_ = (v != "raw");
        }
    }

    LOGI("engine start: device=%s market=%s registry=%s dataDir=%s cam=%s",
         propName("ro.product.device").c_str(), propName("ro.product.market.name").c_str(),
         device::currentDevice().name(), dataDir_.c_str(),
         forcedCamId_.empty() ? "(auto)" : forcedCamId_.c_str());

    while (!stopFlag_.load(std::memory_order_acquire)) {
        if (!cam_.opened()) {
            if (reconnects_ >= 5) {
                LOGE("reconnect limit reached, engine exits");
                break;
            }
            if (!openSession()) {
                closeSession();
                ++reconnects_;
                std::this_thread::sleep_for(1s);
                continue;
            }
        }

        pollControls();
        drainUiCmds();
        reapRetired();

        // 拍照保存进度 → UI（StillCapture 原子量直读，引擎轮速足够刷新 pill）
        if (ui_ && still_)
            ui_->notifySaveProgress(still_->savePending(), still_->lastSavedMs());

        // 预览存活看门狗：repeating 被 HAL 静默丢弃（逻辑多摄切超广角时偶发，
        // setRepeatingRequest 返回成功却不再交付 result）时自动重发 setRepeating 恢复。
        if (session_ && ui_ && ui_->attached()) {
            const int64_t since = lastResultMs_ == 0 ? 0 : nowMs() - lastResultMs_;
            if (lastResultMs_ != 0 && since > 1200) {
                if (stallRetries_ < 12) {
                    LOGW("preview stall %lldms, re-issuing setRepeating (retry %d, zoom=%.2f)",
                         (long long)since, stallRetries_, settings_.zoomRatio);
                    if (sessionSig_ == "ALL") {
                        session_->setRepeatingAll(bandTargets("ALL"), effSettings(),
                                                  allPhysZooms());
                    } else {
                        const std::string phys = activePhysId();
                        session_->setRepeating(sessionSig_, bandTargets(sessionSig_),
                                               effSettings(), phys, physZoom());
                    }
                    ++stallRetries_;
                    lastResultMs_ = nowMs();   // 冷却，避免 100ms 内重复重发
                } else {
                    // 重试耗尽：与其永久黑屏（用户感知"卡死"），不如整链路重连
                    // （closeSession 含 cam_.close()，openSession 重建一切）
                    LOGE("preview dead after %d retries, reconnecting camera", stallRetries_);
                    stallRetries_ = 0;
                    lastResultMs_ = 0;
                    closeSession();
                    ++reconnects_;
                }
            } else if (stallRetries_ > 0) {
                LOGI("preview recovered after %d retries", stallRetries_);
                stallRetries_ = 0;
            }
        }

        if (deviceLost_.exchange(false, std::memory_order_acq_rel)) {
            LOGW("device lost -> reconnect");
            closeSession();
            ++reconnects_;
            continue;
        }

        // 近距状态翻转 → 分带可能变化（接管点 5x ↔ 20x），重发 repeating
        if (bandDirty_.exchange(false, std::memory_order_acq_rel)) commitSession(true);

        // 50ms 拾取节拍：UI 命令（拖拽实时变焦）最坏延迟 = UI 节流 120ms + 本轮 50ms，
        // 平均 ~95ms；其余轮内工作（看门狗/墓地回收）都很轻，不影响功耗。
        std::this_thread::sleep_for(50ms);
    }
    closeSession();
    ctl_.reset();
    running_.store(false, std::memory_order_release);
    stopped_.store(true, std::memory_order_release);   // 收摊完成，主线程可再启动
    LOGI("engine exit");
}

bool CameraEngine::openSession() {
    cam_.onLost = [this] { deviceLost_.store(true, std::memory_order_release); };
    cam_.onDeviceError = [this](int) { deviceLost_.store(true, std::memory_order_release); };

    if (!cam_.openFirstBack(forcedCamId_)) return false;
    const auto& t = cam_.traits();

    // HUD 显示真实 RAW 分辨率（由机型探测结果下发，不写死）
    if (ui_) {
        ui_->setStaticText(t.pixelW, t.pixelH);
        // 平滑拖拽变焦用：长焦直连阈值（无长焦 = 极大值，UI 永不钳制）
        ui_->setTeleMin(t.teleNativeZoom > 0.f ? t.teleNativeZoom : 1e9f);
        ui_->setSensorOrientation(t.sensorOrientation);   // 预览定向的真值来源之一
    }

    // 机型层探测挂点（quirks 注入点）
    {
        ACameraMetadata* chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(cam_.manager(), cam_.deviceId().c_str(),
                                                    &chars) == ACAMERA_OK && chars) {
            auto info = device::currentDevice().probe(cam_.manager(), cam_.deviceId(), chars);
            LOGI("device.probe: hwLevel=%d raw=%d manual=%d readSettings=%d tenBit=%d orientation=%d pixels=%dx%d",
                 info.hardwareLevel, static_cast<int>(info.rawSensor),
                 static_cast<int>(info.manualSensor), static_cast<int>(info.readSensorSettings),
                 static_cast<int>(info.tenBit), info.sensorOrientation, info.pixelArrayW,
                 info.pixelArrayH);
            ACameraMetadata_free(chars);
        }
    }

    // 预览流由 core/ui 的 GL 渲染器提供（RGBA reader，GPU 采样）；此处不再直写窗口

    // RAW 通路（能力具备时创建；会话输出恒含 RAW，repeating 是否带 RAW 由模式决定）
    if (t.raw && !raw_) {
        raw_ = std::make_unique<RawCapture>();
        if (raw_->create(t.pixelW, t.pixelH, dataDir_)) {
            // M2.1：静态元数据（DNG 写入用）来自真机 characteristics
            dng::StaticMeta sm;
            sm.width = t.pixelW;
            sm.height = t.pixelH;
            std::memcpy(sm.cfaPattern, t.cfaPattern, sizeof(sm.cfaPattern));
            std::memcpy(sm.blackLevel, t.blackLevel, sizeof(sm.blackLevel));
            sm.whiteLevel = 1023;
            sm.sensorOrientation = t.sensorOrientation;
            sm.model = "Xiaomi 17 Pro (" + propName("ro.product.device") + ")";
            raw_->setStaticMeta(sm);
        } else {
            LOGE("raw reader create failed, RAW disabled");
            raw_.reset();
        }
    }
    // JPEG 通路（reader 创建无流位成本，输出随 jpgMode_ 挂会话；重建 reader 开销极低）
    if (!still_) {
        still_ = std::make_unique<StillCapture>();
        still_->setGallery(&gallery_);   // 存系统相册（DCIM/Camera）
        if (!still_->create(t.pixelW, t.pixelH, dataDir_)) {
            LOGE("jpeg reader create failed, JPEG disabled");
            still_.reset();
        }
        if (ui_) ui_->resetSaveProgress();   // 上会话的保存计数不跨会话卡「正在保存」
    }
    if (ui_) ui_->setFmtJpg(jpgMode_);   // 引擎是格式真值源（冷启动读 controls.txt 后校正 UI）
    // 会话按当前模式（逻辑广角 / 超广角物理直连）创建
    if (!rebuildSession()) return false;

    reconnects_ = 0;
    return true;
}

void CameraEngine::retireSession() {
    if (!session_) return;
    session_->close();
    retired_.push_back({std::move(session_), nowMs()});
    session_.reset();
}

void CameraEngine::reapRetired() {
    const int64_t now = nowMs();
    constexpr int64_t kGraceMs = 1500;   // 覆盖 close 后残留在飞的框架回调
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                  [now](const RetiredSession& r) {
                                      return now - r.retiredMs > kGraceMs;
                                  }),
                   retired_.end());
}

void CameraEngine::closeSession() {
    retireSession();
    if (raw_) {
        raw_->close();
        raw_.reset();
    }
    if (still_) {
        still_->close();
        still_.reset();
    }
    if (stillUw_) {
        stillUw_->close();
        stillUw_.reset();
    }
    cam_.close();
}

ANativeWindow* CameraEngine::stillWindow() const {
    if (jpgMode_) return still_ ? still_->window() : nullptr;
    return (raw_ && raw_->ringMode()) ? raw_->window() : nullptr;
}

StillParams CameraEngine::makeStillParams() const {
    StillParams p;
    p.zoom = settings_.zoomRatio;
    p.iso = settings_.iso > 0 ? settings_.iso : lastAeIso_;
    p.exposureNs = settings_.exposureNs > 0 ? settings_.exposureNs : lastAeExpNs_;
    p.evSteps = settings_.evSteps;
    p.aeOn = settings_.aeOn;
    p.awbOn = settings_.awbOn;
    return p;
}

void CameraEngine::refreshPhysIds() {
    // 覆盖键 > 自动探测。uwPhysId_ 回退 "2" 是历史遗留（本机超广角实为 3，探测正常时不会触发）。
    uwPhysId_ = forcedUwPhys_.empty() ? cam_.traits().uwPhysicalId : forcedUwPhys_;
    if (uwPhysId_.empty()) uwPhysId_ = "2";
    telePhysId_ = forcedTelePhys_.empty() ? cam_.traits().telePhysicalId : forcedTelePhys_;
    teleMinZoom_ = forcedPhysMin_ > 0.f
                       ? forcedPhysMin_
                       : (forcedTeleNative_ > 0.f
                              ? forcedTeleNative_
                              : (cam_.traits().teleNativeZoom > 0.f ? cam_.traits().teleNativeZoom
                                                                    : 1e9f));
    // 超广角光学倍率（本机 0.774，= 等效焦距比而非焦距比）；探测失败回退 1.0
    uwNativeZoom_ = cam_.traits().uwNativeZoom > 0.f ? cam_.traits().uwNativeZoom : 1.0f;
    // 长焦接管点 = 用户口径 5.0x（与系统相机一致：≥5x 走长焦），但不晚于长焦
    // 原生倍率（不可能有比原生更"广"的长焦画面）。
    teleSwitch_ = std::min(teleMinZoom_, kTeleSwitchUser);
    // 注意：接管点 5.0 与逻辑流安全上限 4.85 之间的 0.15 段不靠相机实现 ——
    // effSettings 把下发的 ZOOM_RATIO 钳在 4.85，剩余倍率由 UI 的 GL crop 补足
    //（crop = z/az ≤ 1.031），因此 FOV 仍连续到 5.0，没有死区。
    // 导轨下限按 HAL 声称值（0.70）：与系统相机口径一致；代价是最底下
    // 0.70–uwNative(0.774) 约 9% 的行程画不出更广（crop 不能 <1）。
    if (ui_) ui_->setZoomRange(cam_.traits().zoomMin);
    // EV 量程（EV 值）：traits 的 RANGE 是步数，× STEP 换算；退化（缺报告）时 UI 保留默认 ±3/0.5
    {
        const auto& t = cam_.traits();
        const float lo = float(t.evMin) * t.evStep, hi = float(t.evMax) * t.evStep;
        LOGI("EV traits: range=[%d,%d] steps step=%.3f EV -> [%.2f, %.2f] EV", t.evMin, t.evMax,
             t.evStep, lo, hi);
        ui_->setEvRange(lo, hi, t.evStep);
    }
}

std::string CameraEngine::activePhysId() const {
    // zoomRatio==0 表示"未设置"（不写 ZOOM_RATIO），按逻辑默认处理 —— 绝不能当 <1.0
    // （否则冷启动直接进物理直连，2026-09-29 真机复现过）。
    const float z = settings_.zoomRatio;
    if (z <= 0.f) return "";
    // 带间滞回：当前在物理带内时，退出阈值低于进入阈值，防止导轨在边界微动
    // 触发会话重建风暴（真机实测：uw 边界 0.86↔1.00 摆动 3 次重建）。
    if (physBand_ == uwPhysId_ && uwAllowed_ && z < 1.03f) return uwPhysId_;
    if (physBand_ == telePhysId_ && !telePhysId_.empty() && z >= teleSwitchEff() - 0.08f)
        return telePhysId_;
    // 常规判定（进入新带）
    if (z < 1.0f - 1e-3f)
        return (uwAllowed_ && !uwPhysId_.empty()) ? uwPhysId_ : std::string();
    // 高倍区走长焦直连（接管点落在逻辑流安全区内，先于断流点 5.0；
    // 近距时推迟到 teleNearSwitch_ —— 长焦对不上焦，与系统相机行为一致）
    if (z >= teleSwitchEff() && !telePhysId_.empty()) return telePhysId_;
    return "";
}

float CameraEngine::physZoom() const {
    // 直连请求写相对数字变焦 = 用户倍率 / 带基（rel 1.0 = 该镜头原生 FOV）：
    //   超广角带基 = uwNativeZoom_（光学倍率 f(uw)/f(main)，本机 0.388 而非导轨下限 0.7）
    //   长焦带基   = teleNativeZoom（用户 z ≥ 2.63 → rel z/2.63，段内连续变焦）
    // 写相对值只作用于物理直连请求自身，不进逻辑融合管线（坏区）。
    const float z = settings_.zoomRatio;
    if (z <= 0.f) return 0.f;
    const std::string phys = activePhysId();
    if (phys == uwPhysId_ && !uwPhysId_.empty()) return std::max(1.0f, z / uwNativeZoom_);
    if (phys == telePhysId_ && !telePhysId_.empty() && teleMinZoom_ < 1e8f)
        return std::max(1.0f, z / teleMinZoom_);
    return 0.f;
}

// 诊断：快照各物理摄的逐键变焦能力（per-physical ZOOM_RATIO / CROP_REGION 是否在
// 该物理摄的 availableRequestKeys 里，及 zoomRatioRange / activeArray 供回退换算）。
// ALL 模式带内连续变焦完全依赖 per-physical ZOOM_RATIO 真正生效；键缺失或 HAL 忽略
// 时表现为「长焦固定在原生焦段」（2026-09-29 真机症状）。
void CameraEngine::probePhysCaps() {
    // 逻辑摄 activeArray（az meta 漂移日志的基准）
    {
        ACameraMetadata* md = nullptr;
        ACameraMetadata_const_entry e{};
        if (ACameraManager_getCameraCharacteristics(cam_.manager(), cam_.deviceId().c_str(),
                                                    &md) == ACAMERA_OK &&
            md &&
            ACameraMetadata_getConstEntry(md, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &e) ==
                ACAMERA_OK &&
            e.count >= 4) {
            for (int i = 0; i < 4; ++i) logAa_[i] = e.data.i32[i];
        }
        if (md) ACameraMetadata_free(md);
    }
    const std::string ids[2] = {uwPhysId_, telePhysId_};
    for (const std::string& pid : ids) {
        if (pid.empty()) continue;
        ACameraMetadata* md = nullptr;
        if (ACameraManager_getCameraCharacteristics(cam_.manager(), pid.c_str(), &md) !=
                ACAMERA_OK ||
            !md)
            continue;
        PhysCaps c;
        ACameraMetadata_const_entry e{};
        if (ACameraMetadata_getConstEntry(md, ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS, &e) ==
            ACAMERA_OK) {
            for (uint32_t i = 0; i < e.count; ++i) {
                if (e.data.i32[i] == ACAMERA_CONTROL_ZOOM_RATIO) c.zoomKey = true;
                if (e.data.i32[i] == ACAMERA_SCALER_CROP_REGION) c.cropKey = true;
            }
        }
        if (ACameraMetadata_getConstEntry(md, ACAMERA_CONTROL_ZOOM_RATIO_RANGE, &e) ==
                ACAMERA_OK &&
            e.count >= 2) {
            c.zmin = e.data.f[0];
            c.zmax = e.data.f[1];
        }
        if (ACameraMetadata_getConstEntry(md, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &e) ==
                ACAMERA_OK &&
            e.count >= 4) {
            for (int i = 0; i < 4; ++i) c.aa[i] = e.data.i32[i];
        }
        LOGI("phys %s caps: ZOOM_RATIO=%d CROP_REGION=%d range=[%.2f,%.2f] aa=%dx%d",
             pid.c_str(), (int)c.zoomKey, (int)c.cropKey, c.zmin, c.zmax, c.aa[2], c.aa[3]);
        physCaps_[pid] = c;
        ACameraMetadata_free(md);
    }
}

std::vector<CaptureSession::PhysZoom> CameraEngine::allPhysZooms() {
    const float z = settings_.zoomRatio > 0.f ? settings_.zoomRatio : 1.0f;
    const bool perKey = cam_.traits().physPerKeyZoom;
    // az 回传换算基准：perKey=true（HAL 执行 per-physical 变焦）→ rel = z/带基，
    // az = rel×带基 = 用户倍率；perKey=false（pandora：键被忽略，物理流恒原生 FOV）
    // → rel 恒 1，az = 带基常量（UI 用 crop = z/az 补带内变焦，带内 az 稳定无泵动）。
    relUw_ = !uwPhysId_.empty() ? (perKey ? std::max(1.0f, z / uwNativeZoom_) : 1.0f) : 0.f;
    relTele_ = (!telePhysId_.empty() && teleMinZoom_ < 1e8f)
                   ? (perKey ? std::max(1.0f, z / teleMinZoom_) : 1.0f)
                   : 0.f;
    std::vector<CaptureSession::PhysZoom> v;
    auto make = [this, perKey](const std::string& id, float rel) {
        CaptureSession::PhysZoom pz;
        pz.id = id;
        pz.rel = rel;
        if (perKey) {
            const auto it = physCaps_.find(id);
            if (it != physCaps_.end() && it->second.cropKey && it->second.aa[2] > 0) {
                const float inv = 1.f / std::max(rel, 1.0f);
                const int cw = int(it->second.aa[2] * inv) & ~1;   // 传感器 crop 常要求偶对齐
                const int ch = int(it->second.aa[3] * inv) & ~1;
                pz.crop[0] = it->second.aa[0] + (it->second.aa[2] - cw) / 2;
                pz.crop[1] = it->second.aa[1] + (it->second.aa[3] - ch) / 2;
                pz.crop[2] = cw;
                pz.crop[3] = ch;
            }
        }
        // 曝光三镜头统一：无论 perKey 与否都带上（逐摄键必须写在声明了物理成员的
        // 请求上）。perKey=false 时 rel=1/crop 空 → setRepeatingAll 不写 ZOOM/CROP。
        pz.aeOn = settings_.aeOn;
        pz.aeLock = settings_.aeLock;
        pz.evSteps = settings_.evSteps;
        pz.iso = settings_.iso;
        pz.exposureNs = settings_.exposureNs;
        pz.awbOn = settings_.awbOn;
        return pz;
    };
    // 恒返回物理成员（请求 withPhysicalIds 声明 [3,4]）：逐摄曝光同步依赖它。
    // 旧版 perKey=false 时返回空表，导致 ALL 请求退化为纯逻辑请求、逐摄键全废。
    if (relUw_ > 0.f) v.push_back(make(uwPhysId_, relUw_));
    if (relTele_ > 0.f) v.push_back(make(telePhysId_, relTele_));
    return v;
}

std::vector<ANativeWindow*> CameraEngine::bandTargets(const std::string& sig) const {
    std::vector<ANativeWindow*> t;
    // repeating 的拍摄流：RAW 环常驻（DMA 直出便宜，ZSL 回溯需要连续出帧）；
    // JPEG 流**绝不进 repeating**（每帧 12MP ISP 编码把预览拖到 18.5fps，
    // 2026-09-30 真机确诊）——流只配置进会话，快门走 captureOnce 单拍。
    if (sig == "ALL") {
        // 全部输出一次挂上（三路预览 + RAW 环）——请求不再随带切换
        for (int s = 0; s < 3; ++s)
            if (ANativeWindow* w = ui_->previewWindow(s)) t.push_back(w);
        if (!jpgMode_)
            if (ANativeWindow* w = stillWindow()) t.push_back(w);
        return t;
    }
    if (sig == "L") {
        t.push_back(ui_->previewWindow(0));
        if (!jpgMode_)
            if (ANativeWindow* w = stillWindow()) t.push_back(w);
    } else if (!uwPhysId_.empty() && sig == "P:" + uwPhysId_) {
        t.push_back(ui_->previewWindow(1));
    } else {
        t.push_back(ui_->previewWindow(2));
    }
    return t;
}

int CameraEngine::slotFromSig(const std::string& sig) const {
    if (sig == "L" || sig == "ALL") return 0;
    if (!uwPhysId_.empty() && sig == "P:" + uwPhysId_) return 1;
    return 2;
}

// 重建会话：优先多流常驻（一次 configure 全部带），HAL 拒绝组合则降级单流。
// 多流模式下本函数只在启动/设备重连/致命错误时调用（跨带走 commitSession 换请求）。
bool CameraEngine::rebuildSession() {
    if (!cam_.opened() || !ui_ || !ui_->attached()) return false;

    refreshPhysIds();
    probePhysCaps();

    // az 时间戳对齐回调：显示线程消费纹理时查环取该帧的 zoomRatio 写回 UI
    ui_->setAzSource([this](int slot, int64_t tsNs) {
        const float az = azForSlot(slot, tsNs);
        if (az > 0.f) ui_->setAppliedZoom(slot, az);
        return az;
    });

    auto sess = std::make_unique<CaptureSession>();
    sess->onFrameResult = [this](const FrameResult& r) { onFrameResult(r); };
    sess->onFrameFailed = [](int reason) { LOGW("capture failed reason=%d", reason); };

    // 旧会话 close 后进墓地延迟析构（防止 in-flight 回调 UAF），再建新会话
    retireSession();

    // ---- 多流常驻：L + uw + tele (+拍摄流) 一次 configure ----
    if (!multiStreamFailed_) {
        const auto& t = cam_.traits();
        std::vector<CaptureSession::OutDesc> outs = {{ui_->previewWindow(0), nullptr}};
        if (!uwPhysId_.empty()) outs.push_back({ui_->previewWindow(1), uwPhysId_.c_str()});
        if (!telePhysId_.empty()) outs.push_back({ui_->previewWindow(2), telePhysId_.c_str()});
        // 拍摄输出互斥：JPEG 模式挂 JPEG，否则挂 RAW（防 HAL 拒 5 流）
        if (jpgMode_) {
            if (still_) outs.push_back({still_->window(), nullptr});
            // 超广物理 JPEG：超广带单拍直连物理 3，照片 FOV = 预览（WYSIWYG）。
            // reader 开销极低（不进 repeating），会话多一路输出而已。
            if (still_ && uwStillOk_ && !uwPhysId_.empty()) {
                if (!stillUw_) {
                    stillUw_ = std::make_unique<StillCapture>();
                    stillUw_->setGallery(&gallery_);
                    if (!stillUw_->create(t.pixelW, t.pixelH, dataDir_)) {
                        LOGE("uw jpeg reader create failed");
                        stillUw_.reset();
                    }
                }
                if (stillUw_) outs.push_back({stillUw_->window(), uwPhysId_.c_str()});
            }
        } else if (raw_) {
            outs.push_back({raw_->window(), nullptr});
        }
        const size_t nFull = outs.size();
        if (sess->create(cam_.handle(), outs)) {
            session_ = std::move(sess);
            multiStream_ = true;
            sessionSig_.clear();              // 强制 commitSession 发首个 repeating
            physBand_ = activePhysId();
            LOGI("multi-stream session created (L+uw:%s+tele:%s%s)", uwPhysId_.c_str(),
                 telePhysId_.c_str(), raw_ ? "+RAW" : "");
            commitSession(false);
            return true;
        }
        // HAL 拒绝组合：先怀疑 5 流（uw still 是唯一"多余"输出）—— 拆掉重试 4 流，
        // 不能直接砸进单流降级（那会丢掉整个多摄架构）。4 流也拒才走降级。
        if (stillUw_ && outs.size() == nFull) {
            LOGW("session rejected, retrying without uw-jpeg stream");
            stillUw_->close();
            stillUw_.reset();
            uwStillOk_ = false;   // 本 ROM 拒 5 流，记忆住不再尝试
            outs.pop_back();
            if (sess->create(cam_.handle(), outs)) {
                session_ = std::move(sess);
                multiStream_ = true;
                sessionSig_.clear();
                physBand_ = activePhysId();
                LOGI("multi-stream session created (4 outputs, uw-jpeg dropped)");
                commitSession(false);
                return true;
            }
        }
        LOGE("multi-stream session rejected by HAL, falling back to single-stream rebuilds");
        multiStreamFailed_ = true;
        sess = std::make_unique<CaptureSession>();
        sess->onFrameResult = [this](const FrameResult& r) { onFrameResult(r); };
        sess->onFrameFailed = [](int reason) { LOGW("capture failed reason=%d", reason); };
    }

    // ---- 单流降级：按当前带重建（跨带 = 会话重建，旧行为）----
    multiStream_ = false;
    const std::string phys = activePhysId();
    std::vector<ANativeWindow*> outs;
    outs.push_back(ui_->previewWindow(0));
    if (phys.empty())
        if (ANativeWindow* sw = stillWindow()) outs.push_back(sw);

    CaptureSession::OutDesc od{outs[0], phys.empty() ? nullptr : phys.c_str()};
    if (!sess->create(cam_.handle(), {od})) {
        LOGE("rebuildSession: create failed (phys=%s)", phys.c_str());
        return false;
    }
    lastRawRing_ = raw_ && raw_->ringMode();
    sessionSig_ = phys.empty() ? std::string("L") : "P:" + phys;
    if (!sess->setRepeating(sessionSig_, outs, settings_, phys, physZoom())) {
        LOGE("rebuildSession: setRepeating failed");
        return false;
    }
    session_ = std::move(sess);
    sessionIsPhysical_ = !phys.empty();
    physBand_ = phys;
    if (ui_) ui_->setPreviewSlot(slotFromSig(sessionSig_));
    LOGI("single-stream session rebuilt: %s (zoom=%.2f physZoom=%.2f)", sessionSig_.c_str(),
         settings_.zoomRatio, physZoom());
    return true;
}

// ALL 请求的逻辑流 zoom 钳在干净带内（防踩融合坏区）；物理流由逐摄键独立变焦，
// 不受此钳制影响（allPhysZooms 用原始 settings_.zoomRatio 换算）。
CaptureSettings CameraEngine::effSettings() const {
    if (!multiStream_) return settings_;
    CaptureSettings s = settings_;
    // 钳制只为"逻辑流不进断流区"（本机实测 4.8 干净、5.0 起持续断流 —— 2026-09-30）。
    // 上界取安全上限而非接管点（5.0）：两者之间那 0.15 段由 UI 的 GL crop 补足
    //（见 refreshPhysIds 注释），相机侧绝不越进断流区。
    const float zmax = teleSwitch_ < 1e8f ? std::min(teleSwitch_, kLogicalSafeMax) : 2.5f;
    s.zoomRatio = std::clamp(s.zoomRatio <= 0.f ? 1.0f : s.zoomRatio, 1.0f, zmax);
    // pandora quirk（真机 2026-09-30 确诊）：逐摄变焦键被 CamX 忽略，且物理流会
    // **继承逻辑 ZOOM_RATIO 的裁切**。长焦带若保持逻辑 4.85，长焦流实际 =
    // 5.016 × 4.85 ≈ 24x（用户所见「我们的 5x = 系统 25x」）。长焦带逻辑流写 1.0，
    // 让长焦物理流回到原生 FOV；主摄流此时不显示，跳到 1.0 无副作用。
    if (activePhysId() == telePhysId_) s.zoomRatio = 1.0f;
    return s;
}

void CameraEngine::commitSession(bool settingsChanged) {
    if (!session_) return;
    refreshPhysIds();
    const std::string phys = activePhysId();
    const bool rawRing = raw_ && raw_->ringMode();
    const bool rawRingChanged = rawRing != lastRawRing_;
    lastRawRing_ = rawRing;

    if (multiStream_) {
        // ---- 方案 A 全目标常驻：repeating 恒为 ALL（三路预览+RAW），跨带零请求切换 ----
        // 只有 RAW 进出会改目标集（重建请求）；其余变化复用请求只改 entry 重发（0 间隔）。
        if (sessionSig_ != "ALL" || rawRingChanged) {
            LOGI("ALL repeating (re)build: rawRing=%d zoom=%.2f", (int)rawRing,
                 settings_.zoomRatio);
            if (session_->setRepeatingAll(bandTargets("ALL"), effSettings(), allPhysZooms())) {
                sessionSig_ = "ALL";
                sessionIsPhysical_ = false;   // RAW 恒出帧，全带可拍
            } else {
                LOGE("ALL repeating failed, rebuilding session");
                if (!rebuildSession()) {
                    LOGE("rebuild failed, reconnecting");
                    closeSession();
                    ++reconnects_;
                }
                return;
            }
        } else if (settingsChanged) {
            session_->setRepeatingAll(bandTargets("ALL"), effSettings(), allPhysZooms());
        }
        // 显示源切换：纯 GL 层（不动请求），按滞回带判定（activePhysId 内含滞回状态机）。
        // slot 变化才写（拖拽期间 commitSession 每 120ms 一次；此前每轮无条件写会
        // 把 controls.txt 的 disp 诊断键立即盖回，2026-09-30 标定时确诊）。
        physBand_ = phys;
        const int dispSlot = slotFromSig(phys.empty() ? std::string("L") : "P:" + phys);
        if (ui_ && dispSlot != lastDispSlot_) {
            LOGI("display slot -> %d (z=%.2f phys=%s)", dispSlot, settings_.zoomRatio,
                 phys.c_str());
            lastDispSlot_ = dispSlot;
            ui_->setPreviewSlot(dispSlot);
            manualDisp_ = false;
        }
        return;
    }

    // ---- 单流降级：跨带 / RAW 进出 → 会话重建 ----
    const std::string sig = phys.empty() ? std::string("L") : "P:" + phys;
    if (sig != sessionSig_ || rawRingChanged) {
        LOGI("session mode change: %s -> %s (zoom=%.2f rawRing=%d)", sessionSig_.c_str(),
             sig.c_str(), settings_.zoomRatio, (int)rawRing);
        if (!rebuildSession()) {
            LOGE("rebuild failed, reconnecting");
            closeSession();
            ++reconnects_;
        }
    } else if (settingsChanged) {
        std::vector<ANativeWindow*> outs;
        outs.push_back(ui_->previewWindow(0));
        if (phys.empty())
            if (ANativeWindow* sw = stillWindow()) outs.push_back(sw);
        session_->setRepeating(sig, outs, settings_, phys, physZoom());
    }
    if (session_) physBand_ = phys;
}

void CameraEngine::pollControls() {
    if (!ctl_) return;
    auto kv = ctl_->poll();
    if (!kv) return;

    bool changed = false;
    for (auto& [k, v] : *kv) applyControl(k, v, changed);

    commitSession(changed);

    // 一次性命令自消费：处理后从文件剔除（防 FUSE mtime 抖动导致的重触发）
    bool hadOneShot = false;
    for (const char* k : {"zsl_shutter", "shot_raw"}) {
        if (kv->count(k)) {
            kv->erase(k);
            hadOneShot = true;
        }
    }
    if (hadOneShot) {
        std::ofstream out(dataDir_ + "/controls.txt", std::ios::trunc);
        for (auto& [k, v] : *kv) out << k << "=" << v << "\n";
    }
}

void CameraEngine::applyControl(const std::string& k, const std::string& v, bool& changed) {
    const auto& t = cam_.traits();
    if (k == "ae") {
        bool on = parseOn(v);
        if (on != settings_.aeOn) {
            settings_.aeOn = on;
            changed = true;
        }
    } else if (k == "iso") {
        int32_t iso = std::clamp(std::atoi(v.c_str()), t.isoMin, t.isoMax);
        if (iso != settings_.iso) {
            settings_.iso = iso;
            settings_.aeOn = false; // 手动 ISO 隐含 AE off
            changed = true;
        }
    } else if (k == "exp_us") {
        long long us = std::atoll(v.c_str());
        int64_t ns = std::clamp<int64_t>(us * 1000, t.exposureMinNs, t.exposureMaxNs);
        if (ns != settings_.exposureNs) {
            settings_.exposureNs = ns;
            settings_.aeOn = false;
            changed = true;
        }
    } else if (k == "af") {
        bool on = parseOn(v);
        if (on != settings_.afOn) {
            settings_.afOn = on;
            changed = true;
        }
    } else if (k == "focus_d") {
        float d = std::atof(v.c_str());
        if (d != settings_.focusDistance) {
            settings_.focusDistance = d;
            settings_.afOn = false;
            changed = true;
        }
    } else if (k == "awb") {
        bool on = parseOn(v);
        if (on != settings_.awbOn) {
            settings_.awbOn = on;
            changed = true;
        }
    } else if (k == "disp") {
        // 诊断：手动切显示源（0=逻辑 1=超广 2=长焦；纯 GL 层，不动请求）。
        // manualDisp_ 期间自动判定不覆盖手动值，直到分带真的变化才交还自动。
        if (ui_) {
            ui_->setPreviewSlot(std::clamp(std::atoi(v.c_str()), 0, 2));
            manualDisp_ = true;
        }
    } else if (k == "zoom") {
        if (!t.hasZoomRatio) {
            LOGW("zoom not supported on %s", t.id.c_str());
            return;
        }
        // 上界按 UI 导轨 120x（相机拿不到的高倍由 GL 裁切完成，下发时各自钳制）
        float zmin = uwAllowed_ ? t.zoomMin : std::max(1.0f, t.zoomMin);
        float z = std::clamp(static_cast<float>(std::atof(v.c_str())), zmin,
                             ui::Ui::zoomLimitMax());
        if (z != settings_.zoomRatio) {
            settings_.zoomRatio = z;
            if (ui_) ui_->setZoomExternal(z);   // UI crop 基准 / 导轨读数联动
            changed = true;
        }
    } else if (k == "uw") {
        bool on = parseOn(v);
        if (on != uwAllowed_) {
            uwAllowed_ = on;
            LOGI("ultrawide opt-in: %s (raw zoomMin=%.2f max=%.2f)",
                 on ? "ON" : "OFF", t.zoomMin, t.zoomMax);
        }
    } else if (k == "uw_phys") {
        // 强制指定超广角物理摄像头 ID（诊断用）。空串 = 恢复自动探测。
        if (v != forcedUwPhys_) {
            forcedUwPhys_ = v;
            LOGI("ultrawide physical override: '%s'", v.c_str());
        }
    } else if (k == "tele_phys") {
        // 强制指定长焦物理摄像头 ID（诊断用）。空串 = 恢复自动探测。
        if (v != forcedTelePhys_) {
            forcedTelePhys_ = v;
            LOGI("tele physical override: '%s'", v.c_str());
        }
    } else if (k == "tele_native") {
        // 诊断：强制长焦带基（az，即 crop 换算的分母）。本机等效焦距算得 5.02x，
        // 但物理直连流可能被 HAL 额外裁切；像素标定在本场景不可靠（视差 + 近景），
        // 用肉眼校准：切到长焦的瞬间画面不跳变，该值即真值（试 5.0 / 7.0 / 9.6）。
        float f = std::atof(v.c_str());
        if (f > 0.f && std::fabs(f - forcedTeleNative_) > 1e-3f) {
            forcedTeleNative_ = f;
            LOGI("tele native override: %.2fx", f);
        }
    } else if (k == "phys_min") {
        // 强制长焦直连起始倍率（诊断用；0 = 恢复自动值 teleNativeZoom）
        float f = std::atof(v.c_str());
        if (f != forcedPhysMin_) {
            forcedPhysMin_ = f;
            LOGI("tele direct threshold override: %.2f", f);
        }
    } else if (k == "tele_near") {
        // 近距接管点（0 = 关闭近距推迟，恒按 5x 切长焦；诊断/校准用）
        float f = std::atof(v.c_str());
        if (f != teleNearSwitch_) {
            teleNearSwitch_ = f;
            bandDirty_.store(true, std::memory_order_release);
            LOGI("tele near-distance switch: %.1fx", f);
        }
    } else if (k == "near_m") {
        // 近距判定阈值（米，0 = 关闭距离判定；退出阈值 = 1.5 倍）
        float f = std::atof(v.c_str());
        if (f != nearEnterM_) {
            nearEnterM_ = f;
            nearCnt_ = farCnt_ = 0;
            bandDirty_.store(true, std::memory_order_release);
            LOGI("near-distance threshold: %.2fm", f);
        }
    } else if (k == "raw_mode") {
        if (!raw_) {
            LOGW("raw unavailable on %s", t.id.c_str());
            return;
        }
        bool ring = (v == "ring");
        if (ring != raw_->ringMode()) {
            raw_->setRingMode(ring);
            // 会话组成变化（RAW 进出 repeating）由 commitSession 的签名检测统一重建
        }
    } else if (k == "zsl_shutter") {
        triggerBurst();
    } else if (k == "save_quota") {
        // controls.txt 每轮都会被重放，值没变就别刷日志（之前每 400ms 一条，把 logcat 淹了）
        int n = std::max(0, std::atoi(v.c_str()));
        if (n != saveQuota_) {
            saveQuota_ = n;
            LOGI("save quota = %d triggers per launch", saveQuota_);
        }
    } else if (k == "ss") {
        // 快门分母（1/x s）→ µs
        double den = std::atof(v.c_str());
        if (den > 0) {
            settings_.exposureNs = std::clamp<int64_t>(int64_t(1e9 / den * 1000),
                                                       t.exposureMinNs, t.exposureMaxNs);
            settings_.aeOn = false;
            changed = true;
        }
    } else if (k == "ev") {
        float ev = std::atof(v.c_str());
        settings_.evSteps = std::clamp(int(std::lround(ev / std::max(t.evStep, 0.01f))),
                                       t.evMin, t.evMax);
        recomputeMixed();   // 双自动→AE on（HAL 补偿）；混合→模拟增益；双手动→EV 无效
        changed = true;
    } else if (k == "uvrot") {
        if (ui_) ui_->setUvRot(std::atoi(v.c_str()));
    } else if (k == "shot_raw") {
        if (!raw_ || raw_->ringMode()) {
            LOGW("shot_raw requires raw_mode=once");
            return;
        }
        if (!activePhysId().empty()) {
            // 物理带 DNG = 主摄逻辑流（zoom 钳 ≤2.5），与预览 FOV 不一致 —— 拒拍是
            // 诚实行为（ALL 常驻下曾放行过，实测 8x 预览落盘 2.5x FOV DNG，已撤销）
            LOGW("shot_raw unavailable in physical band (no matching RAW)");
            return;
        }
        if (savesUsed_ >= saveQuota_) {
            LOGW("save quota exhausted (%d/%d) - restart app to reset", savesUsed_, saveQuota_);
            return;
        }
        ++savesUsed_;
        raw_->requestSingle();
        session_->captureOnce({raw_->window()}, settings_);
    } else if (k == "fps_log") {
        forceLog_ = true;
    }
}

void CameraEngine::drainUiCmds() {
    ui::Ui::Cmd cmd;
    const auto& t = cam_.traits();
    // changed 跨整轮循环累计：拖拽时一帧可能积压多个命令，统一在循环结束后只重发一次
    // repeating（本机 HAL 对高频 setRepeating 敏感，会静默掐断预览流）。
    bool changed = false;
    while (ui_ && ui_->popCmd(&cmd)) {
        switch (cmd.type) {
            case ui::Ui::Cmd::SET_ISO:
                isoAuto_ = false;                       // 拖 ISO 滚轮 = 该参数转手动
                settings_.iso = std::clamp(int(cmd.v), t.isoMin, t.isoMax);
                recomputeMixed();
                changed = true;
                break;
            case ui::Ui::Cmd::SET_EXP_US:
                ssAuto_ = false;                        // 拖 SS 滚轮 = 该参数转手动
                settings_.exposureNs = std::clamp<int64_t>(
                    int64_t(cmd.v * 1000), t.exposureMinNs, t.exposureMaxNs);
                recomputeMixed();
                changed = true;
                break;
            case ui::Ui::Cmd::SET_ISO_AUTO:
                isoAuto_ = cmd.v > 0.5f;
                recomputeMixed();
                changed = true;
                break;
            case ui::Ui::Cmd::SET_SS_AUTO:
                ssAuto_ = cmd.v > 0.5f;
                recomputeMixed();
                changed = true;
                break;
            case ui::Ui::Cmd::SET_AE_LOCK:
                settings_.aeLock = cmd.v > 0.5f;
                changed = true;
                break;
            case ui::Ui::Cmd::SET_ZOOM:
                if (t.hasZoomRatio) {
                    // 下限按 HAL 声称值（用户口径 0.7x）；上限按 UI 导轨 120x —— 相机
                    // 拿不到的高倍由 GL 数字裁切完成（下发时 effSettings/physZoom 各自钳制）。
                    float zmin = uwAllowed_ ? t.zoomMin : std::max(1.0f, t.zoomMin);
                    settings_.zoomRatio = std::clamp(cmd.v, zmin, ui::Ui::zoomLimitMax());
                    changed = true;
                }
                break;
            case ui::Ui::Cmd::SET_EV:
                settings_.evSteps = std::clamp(int(std::lround(cmd.v / std::max(t.evStep, 0.01f))),
                                               t.evMin, t.evMax);
                // EV 生效方式按模式：双自动 = HAL 补偿（AE on）；混合 = 模拟增益
                //（冻结 AE 值 × 2^EV，recomputeMixed 内处理）；全手动 = UI 已灰显不可达
                recomputeMixed();
                changed = true;
                break;
            case ui::Ui::Cmd::SET_AE:
                settings_.aeOn = cmd.v > 0.5f;
                changed = true;
                break;
            case ui::Ui::Cmd::SHOT:
                triggerBurst();
                break;
            case ui::Ui::Cmd::SET_FMT:
                // 拍摄格式切换：会话输出目标变化（RAW↔JPEG），必须重建会话（~300ms）
                jpgMode_ = !jpgMode_;
                LOGI("still format -> %s (rebuilding session)", jpgMode_ ? "JPEG" : "RAW");
                if (!rebuildSession())
                    LOGE("rebuild after fmt switch failed");
                break;
        }
    }
    // 整轮命令处理完后统一提交（模式变化→重建会话，仅设置变化→重发 repeating）。
    // 避免逐命令重发导致 HAL 断流。
    if (changed) {
        commitSession(true);
        LOGI("ui applied: ae=%d iso=%d exp=%lldns ev=%d zoom=%.2f uwPhys=%s",
             static_cast<int>(settings_.aeOn), settings_.iso,
             static_cast<long long>(settings_.exposureNs), settings_.evSteps,
             settings_.zoomRatio, uwPhysId_.c_str());
    }
}

// ISO/SS 混合模式状态机（见 CameraEngine.h 注释）。
// 冻结基准：AE on 期间 onFrameResult 持续跟踪 lastAeIso_/lastAeExpNs_；
// 转 AE off（任一参数手动）后冻结值不再更新，混合模式下自动参数 = 冻结值 × 2^EV。
// EV 步长来自 traits（本机 1/3 EV）。
void CameraEngine::recomputeMixed() {
    const auto& t = cam_.traits();
    if (isoAuto_ && ssAuto_) {                       // 双自动：整块交给硬件 AE
        settings_.aeOn = true;
        settings_.iso = 0;
        settings_.exposureNs = 0;
        return;
    }
    settings_.aeOn = false;
    const double gain = std::pow(2.0, double(settings_.evSteps) * t.evStep);
    if (isoAuto_) {
        const int base = lastAeIso_ > 0 ? lastAeIso_
                                        : (settings_.iso > 0 ? settings_.iso : t.isoMin);
        settings_.iso = std::clamp(int(std::lround(base * gain)), t.isoMin, t.isoMax);
    }
    if (ssAuto_) {
        const int64_t base = lastAeExpNs_ > 0 ? lastAeExpNs_
                            : (settings_.exposureNs > 0 ? settings_.exposureNs : t.exposureMinNs);
        settings_.exposureNs = std::clamp<int64_t>(int64_t(std::lround(base * gain)),
                                                   t.exposureMinNs, t.exposureMaxNs);
    }
}

void CameraEngine::triggerBurst() {
    // ---- JPEG 模式：单拍请求（repeating 不带 JPEG 流，快门时才让 ISP 编码一帧）----
    // 曝光/AE/对焦沿用当前会话设置。FOV 与预览严格一致（WYSIWYG，2026-09-30）：
    //   超广带 → uw 物理流单拍，逻辑 zoom 写 1.0（物理流继承裁切，quirk 之二），
    //     照片 = uw 原生 0.774x；导轨最底 0.70–0.774 段预览也画不出更广（crop 钳 1）。
    //   其余 → 逻辑流单拍，zoom 写 min(z, kLogicalSafeMax)。z ≤ 4.85 时照片 FOV 即
    //     预览 FOV；超出部分（长焦带高倍 / 超广带微差）由 WysiwygCropProcessor 软件
    //     中心裁切补齐 —— 预览那部分本来就是 GL 数字裁切，照片同口径。
    if (jpgMode_) {
        if (!session_ || !session_->valid()) {
            LOGW("jpeg unavailable");
            if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_);
            return;
        }
        if (savesUsed_ >= saveQuota_) {
            LOGW("save quota exhausted (%d/%d) - restart app to reset", savesUsed_, saveQuota_);
            if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_);
            return;
        }
        const float z = settings_.zoomRatio <= 0.f ? 1.0f : settings_.zoomRatio;
        // 超广带判定复用显示源状态机（含滞回：z∈[1.0,1.03) 预览仍在超广带）。
        // 该段照片 = uw 原生 0.774，比预览（GL crop 到 z）略广，超出部分同样由
        // WysiwygCropProcessor 中心裁切补齐 —— 全程统一 WYSIWYG。
        const bool uwShot = uwAllowed_ && stillUw_ && !uwPhysId_.empty() &&
                            activePhysId() == uwPhysId_;
        StillParams p = makeStillParams();
        CaptureSettings s = settings_;
        if (uwShot) {
            s.zoomRatio = 1.0f;   // 逻辑键 1.0 → uw 物理流继承 = 原生 FOV（0.774）
            p.appliedZoom = uwNativeZoom_;
            stillUw_->expectShot(p);
            if (!session_->captureOnce({stillUw_->window()}, s)) {
                stillUw_->cancelShot();
                LOGE("jpeg captureOnce failed (uw physical)");
                if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_);
                return;
            }
        } else {
            const float applied = std::clamp(z, 1.0f, kLogicalSafeMax);
            s.zoomRatio = applied;
            p.appliedZoom = applied;
            still_->expectShot(p);
            if (!session_->captureOnce({still_->window()}, s)) {
                still_->cancelShot();
                LOGE("jpeg captureOnce failed");
                if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_);
                return;
            }
        }
        ++savesUsed_;
        if (ui_) ui_->notifyShot(true, savesUsed_, saveQuota_);
        LOGI("shutter triggered [jpg single %s] z=%.2f applied=%.2f (%d/%d)",
             uwShot ? "uw" : "logical", z, p.appliedZoom, savesUsed_, saveQuota_);
        return;
    }
    if (!raw_ || !raw_->ringMode()) {
        LOGW("trigger requires raw_mode=ring");
        if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_);
        return;
    }
    if (!activePhysId().empty()) {
        // 物理带 DNG = 主摄逻辑流（zoom 钳 ≤2.5），与预览 FOV 不一致 —— 拒拍是
        // 诚实行为（ALL 常驻下曾放行过，实测 8x 预览落盘 2.5x FOV DNG，已撤销）
        LOGW("trigger unavailable in physical band (no matching RAW)");
        if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_);
        return;
    }
    if (savesUsed_ >= saveQuota_) {
        LOGW("save quota exhausted (%d/%d) - restart app to reset", savesUsed_, saveQuota_);
        if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_);
        return;
    }
    ++savesUsed_;
    raw_->shutterBurst(4);
    if (ui_) ui_->notifyShot(true, savesUsed_, saveQuota_);
    LOGI("shutter triggered (%d/%d)", savesUsed_, saveQuota_);
}

int64_t CameraEngine::nowMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 显示线程按**显示帧**的 SENSOR_TIMESTAMP 查结果环取该时刻的 zoomRatio 作为 az。
// 这样 crop = zoom_/az 与显示帧严格同源：显示 FOV = az × crop ≡ zoom_，与相机
// 120ms 节流/管线延迟无关 —— 变焦全程 GL 侧无泵动。
float CameraEngine::azForSlot(int slot, int64_t tsNs) {
    // 物理流（quirk 之二：继承逻辑 ZOOM_RATIO 裁切）：纹理真实 FOV = 带基 × 逻辑zoom(t)。
    // 常量带基（旧做法）在跨带瞬间必错：主摄带逻辑写 4.85 时长焦纹理实际 ≈24.3x，
    // 却按 5.016 配 crop → 切换闪帧；按帧时间戳取逻辑 zoom 后严格一致。
    float base = 1.f;
    if (slot == 1) base = uwNativeZoom_;
    else if (slot == 2 && teleMinZoom_ < 1e8f) base = teleMinZoom_;
    std::lock_guard<std::mutex> lk(resMx_);
    if (resRing_.empty()) return 0.f;               // 无结果：调用方保持旧值
    // 取 ts ≤ tsNs 的最新结果；图像早于最旧结果（环已卷绕）时用最旧值兜底
    const ResTs* best = &resRing_.front();
    for (const auto& e : resRing_) {
        if (e.ts <= tsNs) best = &e;
        else break;
    }
    return base * best->zoom;
}

void CameraEngine::onFrameResult(const FrameResult& r) {
    // 心跳：任意结果都算存活（看门狗用）。NDK 单帧单回调（交付逻辑融合结果），
    // 该结果即预览帧代表，直接计帧率（无需按物理 ID 区分，也不存在三路同频虚高）。
    lastResultMs_ = nowMs();
    {
        frameCount_++;
        if (lastFrameMs_ != 0) {
            const int64_t gap = lastResultMs_ - lastFrameMs_;
            if (gap > 70) ++frameGaps_;
            if (gap > maxGapMs_) maxGapMs_ = gap;
        }
        lastFrameMs_ = lastResultMs_;
    }

    if (firstTs_ == 0) firstTs_ = r.timestampNs;
    lastTs_ = r.timestampNs;

    if (raw_) raw_->onFrameResult(r);

    // 近距判定取逻辑融合结果（其焦距即当前主源物理摄像头的对焦距离：主摄带=主摄、
    // 长焦带=长焦，正是接管点推迟判断所需）。NDK 单帧单结果，无需按物理 ID 区分。
    if (nearEnterM_ > 0.f && r.focusDistanceDiopters > 1e-3f) {
        const float distM = 1.f / r.focusDistanceDiopters;
        if (distM < nearEnterM_) {
            farCnt_ = 0;
            if (++nearCnt_ >= kNearDebounce && !nearSubject_) {
                nearSubject_ = true;
                bandDirty_.store(true, std::memory_order_release);
                if (teleNearSwitch_ > 1.f)
                    LOGI("subject near (%.2fm) -> tele switch deferred to %.0fx", distM,
                         teleNearSwitch_);
            }
        } else if (distM > nearEnterM_ * 1.5f) {
            nearCnt_ = 0;
            if (++farCnt_ >= kNearDebounce && nearSubject_) {
                nearSubject_ = false;
                bandDirty_.store(true, std::memory_order_release);
                LOGI("subject far (%.2fm) -> tele switch back at %.2fx", distM, teleSwitch_);
            }
        } else {
            nearCnt_ = farCnt_ = 0;
        }
    }

    // 平滑变焦锚点 az：NDK 单帧单回调，result 即逻辑融合结果，其 ZOOM_RATIO 就是相机
    // 实际出图的倍率 —— 直接作为主摄带 az[0] 锚点，每帧刷新（不再按物理 ID 判定"逻辑/物理
    // 流"，那套假设在 NDK 不成立）。超广/长焦带 az 为恒定带基（与 result 内容无关、幂等），
    // 任意结果均可触发 —— 无论 HAL 是否下发 per-physical 结果都能正确工作。
    // 主摄带 az[0] 由逻辑结果 ZOOM_RATIO 驱动，彻底杜绝被超广/长焦流污染（旧 bug：az 被
    // 冲成 1.0 → 预览周期性被放大到 zoom_² 倍 → 1–5x 卡顿）。
    if (ui_) {
        // 结果线程只把 (ts, zoomRatio) 入环；az 回传改由显示线程按**显示帧时间戳**
        // 查环（Gl::acquirePreview → azForSlot）。旧做法「结果一到就写 az[0]」会让
        // az 领先显示纹理 1-2 帧：快拖时 crop 与纹理错位，显示 FOV 在两个值间泵动
        //（1–5x「不丝滑/来回闪」的根因）。时间戳对齐后 crop 与显示帧严格同源。
        {
            std::lock_guard<std::mutex> lk(resMx_);
            if (resRing_.empty() || resRing_.back().ts < r.timestampNs)
                resRing_.push_back({r.timestampNs, r.zoomRatio});
            if (resRing_.size() > 64) resRing_.erase(resRing_.begin());
        }
        // 漂移诊断（zoom 变化时）：HAL 变焦裁切中心是否随 zoom 偏离阵列中心
        //（用户报告 2–5x 拖动中画面中心慢慢右移；静止 6s 互相关零漂移）。
        if (std::fabs(r.zoomRatio - lastAzLogZoom_) > 0.03f && r.cropRegion[2] > 0 &&
            logAa_[2] > 0) {
            lastAzLogZoom_ = r.zoomRatio;
            const float ccx = r.cropRegion[0] + r.cropRegion[2] * 0.5f;
            const float ccy = r.cropRegion[1] + r.cropRegion[3] * 0.5f;
            const float acx = logAa_[0] + logAa_[2] * 0.5f;
            const float acy = logAa_[1] + logAa_[3] * 0.5f;
            LOGI("az meta: z=%.2f crop=[%d %d %d %d] off=(%.1f,%.1f)px", r.zoomRatio,
                 r.cropRegion[0], r.cropRegion[1], r.cropRegion[2], r.cropRegion[3],
                 ccx - acx, ccy - acy);
        }
    }

    // AE 实测值跟踪（混合模式冻结基准 + UI 自动参数数值行显示）
    if (settings_.aeOn && r.iso > 0 && r.exposureNs > 0) {
        lastAeIso_ = r.iso;
        lastAeExpNs_ = r.exposureNs;
        if (ui_) {
            ui_->setAutoIso(r.iso);
            ui_->setAutoSsUs(r.exposureNs / 1000);
        }
    }

    if (forceLog_ || frameCount_ % 120 == 0) {
        forceLog_ = false;
        double fps = lastTs_ > firstTs_
                         ? static_cast<double>(frameCount_ - 1) * 1e9 /
                               static_cast<double>(lastTs_ - firstTs_)
                         : 0.0;
        if (raw_) {
            LOGI("stats: frames=%llu fps=%.1f eff(iso=%d exp=%lldns zoom=%.2f phys=%s) raw: %.1ffps total=%lld",
                 static_cast<unsigned long long>(frameCount_), fps, r.iso,
                 static_cast<long long>(r.exposureNs), r.zoomRatio, r.physicalId.c_str(),
                 raw_->rawFps(),
                 static_cast<long long>(raw_->rawCount()));
        } else {
            LOGI("stats: frames=%llu fps=%.1f eff(iso=%d exp=%lldns zoom=%.2f phys=%s)",
                 static_cast<unsigned long long>(frameCount_), fps, r.iso,
                 static_cast<long long>(r.exposureNs), r.zoomRatio, r.physicalId.c_str());
        }
        LOGI("pacing: gaps>70ms=%d max=%lldms (frames=%llu)", frameGaps_,
             (long long)maxGapMs_, static_cast<unsigned long long>(frameCount_));
        if (multiStream_ && sessionSig_ == "ALL" && ui_)
            LOGI("slot frames: L=%lld uw=%lld tele=%lld",
                 (long long)ui_->slotFrames(0), (long long)ui_->slotFrames(1),
                 (long long)ui_->slotFrames(2));
    }
}

} // namespace optic::capture
