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

// UI 注入 + 命令即时通知：<｜hy_place▁holder▁no▁813｜>命令（点按对焦/格式切换）由 UI 线程直接唤醒本线程，
// 不必等主循环的 50ms 轮询边界 —— 点按到起扫之间的固定等待能省一半。
void CameraEngine::setUi(ui::Ui* u) {
    ui_ = u;
    if (ui_)
        ui_->setCmdNotify([this] {
            cmdWake_.store(true, std::memory_order_release);
            tickCv_.notify_all();
        });
}

void CameraEngine::run(std::string dataDir) {
    dataDir_ = std::move(dataDir);
    lastInteractionMs_ = nowMs();   // 启动起算：给一整段宽限期，不立刻休眠
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
        // 休眠态跳过：停 repeating 后不再有 result 流入，否则会被误判 stall 自动重发
        //（等于白睡，相机被偷偷唤醒）。
        if (!sleeping_ && session_ && ui_ && ui_->attached()) {
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

        // 自动休眠触发：长时间无操作则停 repeating（相机停跑，降功耗/发热）。
        // 休眠中不跑看门狗（上方已用 !sleeping_ 排除），此处也只在未休眠时判定。
        if (!sleeping_ && session_ && ui_ && ui_->attached() && lastInteractionMs_ > 0 &&
            nowMs() - lastInteractionMs_ > kSleepMs) {
            sleepCamera();
        }

        // 近距状态翻转 → 分带可能变化（接管点 5x ↔ 20x），重发 repeating
        if (bandDirty_.exchange(false, std::memory_order_acq_rel)) commitSession(true);

        // AF-S 锁定窗口到点：切回 CAF 让持续跟踪恢复（见 sendAfTrigger 注释）
        if (afLockUntilMs_ > 0 && nowMs() >= afLockUntilMs_) {
            afLockUntilMs_ = 0;
            if (settings_.afMode != 0) {
                settings_.afMode = 0;
                LOGI("af: back to CAF");
                commitSession(true);
            }
        }

        // 只换 ROI（policy 0）的兜底：HAL 若完全没反应，补一次传统 AF_TRIGGER。
        // 判据是"点按之后镜头有没有动过"（fdMovedSinceTap_）—— 镜头没动 = 新 ROI 没被
        // 采纳；afFocusLogged_ 保证已经在收敛/已合焦的时候绝不打扰（打扰 = 重新甩到
        // 无穷远再全扫，实测多花一倍时间）。旧判据用 afScanned_ 是错的：CAF 常态就在
        // 1/2 之间周期跳变，点按后几百毫秒它必为真，兜底实际永不触发。
        if (afTapPolicy_ == 0 && afTapT0Ms_ > 0 && !afFocusLogged_ && !fdMovedSinceTap_ &&
            nowMs() - afTapT0Ms_ >= kAfFallbackMs) {
            LOGI("af: lens did not move in %lldms -> fallback trigger",
                 (long long)kAfFallbackMs);
            afTapT0Ms_ = nowMs();   // 重新计时（测的是补触发后的合焦耗时）
            fdMovedSinceTap_ = false;
            sendAfTrigger();
        }

        // 节拍上限 50ms，但 UI 推送命令会即时唤醒（cmdWake_）：点按对焦这类一次性命令
        // 的延迟从"最坏 50ms + pollControls"降到接近 0；无命令时退回轮询，功耗不变。
        {
            std::unique_lock<std::mutex> lk(tickMx_);
            tickCv_.wait_for(lk, 50ms, [this] { return cmdWake_.load(std::memory_order_acquire); });
            cmdWake_.store(false, std::memory_order_release);
        }
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

    // 帧率目标来自机型层（本机三摄常驻下，HAL 按 TEMPLATE 取最大档会让整条管线的
    // 带宽与发热翻倍）。用户 settings_ 的默认值在此之前可能被 controls.txt 改过，
    // 这里统一按机型口径覆盖。
    settings_.fpsMin = sessPol_.targetFpsMin;
    settings_.fpsMax = sessPol_.targetFpsMax;

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
            // 机身标识一律来自机型层（业务层写死会让第二台机型的 DNG 厂商/型号全错）
            const auto id = dev_.identity();
            sm.make = id.make;
            sm.model = id.model + " (" + propName("ro.product.device") + ")";
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
    if (ui_) ui_->setShotQuota(saveQuota_);   // 常显「已拍 n/N」角标（controls.txt 可调）
    if (ui_) ui_->setFmtJpg(jpgMode_);   // 引擎是格式真值源（冷启动读 controls.txt 后校正 UI）
    if (ui_) {
        ui_->setFlashAvail(t.flashAvailable);   // 无闪光灯单元 → UI 隐藏闪光灯入口
        if (!t.flashAvailable && settings_.flashMode != 0) {
            // 无闪光灯设备上却残留非 0 档位（典型来源：持久化 settings.txt 从别的设备
            // 拷来，或 UI 在 attach 阶段先于 openCamera 恢复出了旧值）→ 一律清零。
            // 否则每个请求都带 AE_MODE_ON_ALWAYS_FLASH，会被 HAL 拒绝整包。
            settings_.flashMode = 0;
            ui_->setFlash(0);
        }
    }
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

// 自动休眠：停 repeating（相机 ISP/传感器停跑），预览不再出帧；UI 显示"已休眠，触摸唤醒"。
// 会话/纹理/输出目标全部保留，唤醒只需重发 repeating（见 wakeCamera），瞬启且无黑帧。
void CameraEngine::sleepCamera() {
    if (sleeping_ || !session_) return;
    session_->stopRepeating();
    sleeping_ = true;
    if (ui_) ui_->setSleeping(true);
    LOGI("camera sleep: repeating stopped (idle > %llds)", (long long)(kSleepMs / 1000));
}

// 唤醒：重发 repeating 恢复预览流，并复位空闲计时（避免唤醒瞬间又立刻入睡）。
void CameraEngine::wakeCamera() {
    if (!sleeping_) return;
    sleeping_ = false;
    lastInteractionMs_ = nowMs();
    if (ui_) ui_->setSleeping(false);
    commitSession(true);   // 按当前设置/带/RAW 状态重发 ALL 或单流 repeating
    LOGI("camera wake: repeating resumed");
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
    teleSwitch_ = std::min(teleMinZoom_, zoomProf_.teleSwitchUser);
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
        // 机型层声明的会话输出上限（HAL 资源实测值）：本进程按 full → degradeSteps
        // 逐级退让，直到 HAL 接受。注意 degradeSteps 里的最小档必须 ≥ 预览槽数 + 1
        // （否则连拍摄输出都没有，不如直接走单流降级）。
        const size_t tryCount = std::min(nFull, static_cast<size_t>(sessPol_.maxStreams));
        for (size_t attempt = 0;; ++attempt) {
            const size_t want = attempt == 0
                                    ? tryCount
                                    : (attempt - 1 < sessPol_.degradeSteps.size()
                                           ? static_cast<size_t>(sessPol_.degradeSteps[attempt - 1])
                                           : 0);
            if (want == 0 || want > outs.size()) break;
            while (outs.size() > want) {
                // 被砍掉的输出若持有本进程的 reader，必须释放（否则泄漏 + 下次重试重复计数）
                if (stillUw_ && outs.back().win == stillUw_->window()) {
                    stillUw_->close();
                    stillUw_.reset();
                    uwStillOk_ = false;   // 本 ROM 拒该组合，记忆住不再尝试
                }
                outs.pop_back();
            }
            if (sess->create(cam_.handle(), outs)) {
                session_ = std::move(sess);
                multiStream_ = true;
                sessionSig_.clear();              // 强制 commitSession 发首个 repeating
                physBand_ = activePhysId();
                LOGI("multi-stream session created (%zu outputs%s)", outs.size(),
                     outs.size() < nFull ? ", uw-jpeg dropped" : "");
                commitSession(false);
                return true;
            }
            if (outs.size() == want && attempt > 0) LOGW("session rejected at %zu outputs", want);
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
    const float zmax = teleSwitch_ < 1e8f ? std::min(teleSwitch_, zoomProf_.logicalSafeMax)
                                                                  : zoomProf_.logicalFallbackMax;
    s.zoomRatio = std::clamp(s.zoomRatio <= 0.f ? 1.0f : s.zoomRatio, 1.0f, zmax);
    // 机型 quirk（见 Xiaomi17ProDevice::physQuirks）：部分 HAL 的物理流会**继承逻辑
    // ZOOM_RATIO 的裁切**。pandora 上长焦带若保持逻辑 4.85，长焦流实际 = 5.016×4.85
    // ≈ 24x（用户所见「我们的 5x = 系统 25x」）。此时长焦带逻辑流必须写 1.0，让长焦
    // 物理流回到原生 FOV；主摄流此时不显示，跳到 1.0 无副作用。
    // 不继承的机型保持逻辑倍率不变（继承式写法会让照片 FOV 塌回 1x）。
    if (physQ_.inheritsLogicalZoom && activePhysId() == telePhysId_) s.zoomRatio = 1.0f;
    return s;
}

void CameraEngine::commitSession(bool settingsChanged) {
    if (!session_) return;
    // ROI 随 zoom 走：这是**唯一**的重算入口，覆盖所有会改变 zoom 的路径
    //（UI 拖拽 / controls.txt / 近距接管点翻转后的 commit），区域变化必须重发 repeating。
    if (refreshRoi()) settingsChanged = true;
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

    // 调试键改动视为一次用户操作：休眠中直接唤醒并把新设置一起下发（避免改动被静默丢弃）。
    if (sleeping_) {
        if (changed) wakeCamera();
    } else {
        commitSession(changed);
    }

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
            if (on) {
                isoAuto_ = ssAuto_ = true;   // 回双自动：参数交还 HAL AE
            } else {
                // 关 AE = 双手动冻结当前实测值（画面参数保持不变）
                if (lastAeIso_ > 0) settings_.iso = lastAeIso_;
                if (lastAeExpNs_ > 0) settings_.exposureNs = lastAeExpNs_;
                isoAuto_ = ssAuto_ = false;
            }
            changed = true;
        }
    } else if (k == "iso") {
        int32_t iso = std::clamp(std::atoi(v.c_str()), t.isoMin, t.isoMax);
        if (iso != settings_.iso) {
            settings_.iso = iso;
            isoAuto_ = false;     // 手动 ISO：与 UI 拖滚轮同语义（同步引擎侧标志）
            recomputeMixed();     // 联动：自动 SS 反比补偿（曝光守恒）
            changed = true;
        }
    } else if (k == "exp_us") {
        long long us = std::atoll(v.c_str());
        int64_t ns = std::clamp<int64_t>(us * 1000, t.exposureMinNs, t.exposureMaxNs);
        if (ns != settings_.exposureNs) {
            settings_.exposureNs = ns;
            ssAuto_ = false;
            recomputeMixed();
            changed = true;
        }
    } else if (k == "iso_auto" || k == "ss_auto") {
        // 调试键：与 UI A 按钮同语义（切自动/手动并触发联动重算）
        bool on = parseOn(v);
        bool& flag = (k == "iso_auto") ? isoAuto_ : ssAuto_;
        if (on != flag) {
            flag = on;
            recomputeMixed();
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
    } else if (k == "afs") {
        // afs=0：点按对焦退回纯 CAF（无 AF_TRIGGER 的单次模式），用于对照合焦速度
        const bool single = parseOn(v);
        if (single != afSingleMode_) {
            afSingleMode_ = single;
            afTapPolicy_ = single ? 2 : 1;
            LOGI("controls: tap-to-focus mode -> %s", single ? "AF-S (single)" : "CAF (continuous)");
        }
    } else if (k == "aft") {
        // 点按策略对照：0=只换区域(CAF) 1=CAF+trigger 2=AF-S+trigger
        const int p = std::clamp(std::atoi(v.c_str()), 0, 2);
        if (p != afTapPolicy_) {
            afTapPolicy_ = p;
            afSingleMode_ = (p == 2);
            LOGI("controls: tap policy -> %s",
                 p == 0 ? "region only" : (p == 1 ? "CAF + trigger" : "AF-S + trigger"));
        }
    } else if (k == "awb") {
        bool on = parseOn(v);
        if (on != settings_.awbOn) {
            settings_.awbOn = on;
            changed = true;
        }
    } else if (k == "flash") {
        // 闪光灯档位：off / auto / on / torch（也接受数字 0..3）。
        // 无闪光灯单元的设备一律拒绝（写 AE_MODE_ON_ALWAYS_FLASH 会被 HAL 拒整包）。
        int m = 0;
        if (v == "auto") m = 1;
        else if (v == "on") m = 2;
        else if (v == "torch") m = 3;
        else m = std::clamp(std::atoi(v.c_str()), 0, 3);
        if (m != 0 && !t.flashAvailable) {
            LOGW("flash unavailable on %s", t.id.c_str());
            return;
        }
        if (m != settings_.flashMode) {
            settings_.flashMode = m;
            static const char* kName[4] = {"off", "auto", "on", "torch"};
            LOGI("flash -> %s", kName[m]);
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
        float z = std::clamp(static_cast<float>(std::atof(v.c_str())), zmin, zoomLimit());
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
        // 近距判定阈值（米，0 = 关闭距离判定）。改动必须**同时清掉已锁存的
        // nearSubject_**：否则诊断键「关掉判定」后状态仍留在 near，接管点继续
        // 按 teleNearSwitch_ 生效，5x 卡在主摄出不来（2026-10-04 真机实测）。
        float f = std::atof(v.c_str());
        if (f != nearEnterM_) {
            nearEnterM_ = f;
            nearCnt_ = 0;
            farCnt_ = 0;
            // 两种情况必须立即释放已锁存的近距态：① 关掉判定（f<=0，状态机
            // 不再运行，永远退不出）；② 新阈值已大于当前实测距离（状态机要重跑
            // 十几帧才翻转，期间仍按旧接管点渲染）。lastFdDiopters_ 是最近一次
            // result 回读的屈光度原子快照（0 = 无效读数，此时不猜）。
            const float lastFd = lastFdDiopters_.load(std::memory_order_acquire);
            const float lastM = lastFd > 1e-3f ? 1.f / lastFd : -1.f;
            if (nearSubject_ && (f <= 0.f || (lastM > 0.f && lastM >= f))) {
                nearSubject_ = false;
                LOGI("near-distance rule cleared (thr=%.2fm last=%.2fm) -> tele switch back at "
                     "%.2fx",
                     f, lastM, teleSwitch_);
            }
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
        // 0 = 不限制（曾误实现成"禁用所有拍摄"：savesUsed_(0) >= 0 恒真）
        int n = std::max(0, std::atoi(v.c_str()));
        if (n != saveQuota_) {
            saveQuota_ = n;
            LOGI("save quota = %d triggers per launch (0 = unlimited)", saveQuota_);
            if (ui_) ui_->setShotQuota(saveQuota_);
        }
    } else if (k == "ss") {
        // 快门分母（1/x s）→ ns。曾误写 1e9/den*1000（多乘 1000，1/125s 变 8s）。
        double den = std::atof(v.c_str());
        if (den > 0) {
            settings_.exposureNs = std::clamp<int64_t>(int64_t(1e9 / den),
                                                       t.exposureMinNs, t.exposureMaxNs);
            ssAuto_ = false;
            recomputeMixed();
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
            if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_, ui::Ui::kShotNoRawRing);
            return;
        }
        if (!activePhysId().empty() ||
            settings_.zoomRatio > zoomProf_.logicalSafeMax * 1.02f) {
            // 与 triggerBurst 同口径：物理带 + 近距长焦推迟区间（5-20x）都拿不到
            // 与预览同 FOV 的 RAW（逻辑流 zoom 钳 logicalSafeMax），诚实拒拍
            LOGW("shot_raw unavailable beyond logical RAW reach (z=%.2f phys=%s)",
                 settings_.zoomRatio, activePhysId().c_str());
            if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_, ui::Ui::kShotPhysBand);
            return;
        }
        if (saveQuota_ > 0 && savesUsed_ >= saveQuota_) {
            LOGW("save quota exhausted (%d/%d) - restart app to reset", savesUsed_, saveQuota_);
            if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_, ui::Ui::kShotQuota);
            return;
        }
        ++savesUsed_;
        raw_->requestSingle();
        session_->captureOnce({raw_->window()}, settings_);
        if (ui_) ui_->notifyShot(true, savesUsed_, saveQuota_, ui::Ui::kShotOk);
    } else if (k == "fps_log") {
        forceLog_ = true;
    }
}

// 触摸对焦（UI 点按预览回调）：把点按位置换算成 AF/AE 区域下发。
// 坐标链：UI 下发的是点按在预览 rect 内的占比 (fx,fy)。预览 rect 显示的是用户
// 可见画面（GL crop 补偿后 = zoom_ 口径的 FOV），点按占比即可见画面内占比；
// 可见宽度 = 1/zoom_（native-main 单位）→ 场景坐标 x = (fx-0.5)/zoom_。
// 逻辑 active array 恰好张成 1.0 native 单位 → 阵列坐标 = 阵列中心 + x×阵列宽。
// zoomRatio 变焦对横竖两轴等比裁切，y 轴同理用阵列高。AF/AE 同区（点按对焦同时
// 驱动测光，业界通行语义）；CamX SAT 把区域换算到当前 backing 物理摄，跨带通用。
// 区域边长取可见宽的 15%（点按精度与统计窗口的折中），钳制进阵列。区域粘滞保留
// （再次点按才更新）；手动对焦（focus_d）下点按 = 切回连续自动对焦。
// 纯换算：预览归一化位置 (fx,fy) + 当前 zoom → active array 域 metering rectangle。
// 副作用：（对不上焦 / 对框外的像素测光）—— 审查 P1-1 的根归类。
void CameraEngine::roiRectFrom(float fx, float fy, int32_t out[5]) const {
    const float z = settings_.zoomRatio > 0.01f ? settings_.zoomRatio : 1.0f;
    float side = ui::Ui::kRoiFrac * float(logAa_[2]) / z;
    side = std::clamp(side, 64.f, std::min(float(logAa_[2]), float(logAa_[3])) * 0.5f);
    const float cx = float(logAa_[0]) + float(logAa_[2]) * 0.5f +
                     (fx - 0.5f) * float(logAa_[2]) / z;
    const float cy = float(logAa_[1]) + float(logAa_[3]) * 0.5f +
                     (fy - 0.5f) * float(logAa_[3]) / z;
    // MeteringRectangle = int32×5 (xmin,ymin,xmax,ymax,weight)，左闭右开；weight∈[1,1000]。
    // 常见误写 (x,y,w,h)：xmax 比 xmin 还小 ⇒ HAL 钳成退化点，统计窗口塌掉（2026-10-02
    // 真机回显 [1740 1228 1740 1228] 确诊）。1000 = 最大权重。
    const int32_t s32 = int32_t(side);
    const int32_t rx = std::clamp(int(cx - side / 2), logAa_[0], logAa_[0] + logAa_[2] - s32);
    const int32_t ry = std::clamp(int(cy - side / 2), logAa_[1], logAa_[1] + logAa_[3] - s32);
    out[0] = rx; out[1] = ry; out[2] = rx + s32; out[3] = ry + s32; out[4] = 1000;
}

// 按当前 zoom 刷新 AF/AE 区域（每次下发前都调，便宜）：这是"区域随变焦走"的唯一入口。
// 返回 true = 区域真的变了，调用方需重发 repeating。
bool CameraEngine::refreshRoi() {
    if (!roiOn_ || logAa_[2] <= 0 || logAa_[3] <= 0) {
        if (settings_.afRegion[2] > 0 || settings_.aeRegion[2] > 0) {
            std::memset(settings_.afRegion, 0, sizeof(settings_.afRegion));
            std::memset(settings_.aeRegion, 0, sizeof(settings_.aeRegion));
            roiPendingEcho_ = false;
            return true;
        }
        return false;
    }
    int32_t reg[5];
    roiRectFrom(roiFx_, roiFy_, reg);
    // 比较基准必须是 settings_ 本身而非单独的"已下发"镜像：清空路径只清 settings_
    //（镜像若不同步清，"清空后同位置再点"会误判无变化、区域永远发不出去——
    // 2026-10-03 全量测试真机确诊 rect=[0 0 0 0]）。
    if (std::memcmp(reg, settings_.afRegion, sizeof(reg)) == 0) return false;
    std::memcpy(settings_.afRegion, reg, sizeof(reg));
    std::memcpy(settings_.aeRegion, reg, sizeof(reg));
    return true;
}

bool CameraEngine::onTapFocus(float fx, float fy, int mode) {
    if (logAa_[2] <= 0 || logAa_[3] <= 0) {
        LOGW("tap focus: active array unknown, skip");
        return false;
    }
    // mode 3 = 双击清除区域：回到默认评价测光 + 中央追焦（AF/AE 区域整组清空，
    // camera2 语义下"不写区域键"即默认全画面评价）。
    if (mode == 3) {
        roiOn_ = false;
        const bool ch = refreshRoi();
        afTapT0Ms_ = 0;
        afScanned_ = false;
        afFocusLogged_ = false;
        fdMovedSinceTap_ = false;
        roiPendingEcho_ = false;
        // 区域撤掉不一定让 3A 立刻回到画面全区評価 —— 显式扫一次，用户马上看得出来。
        tapTriggerPending_ = true;
        LOGI("tap focus: region cleared (default metering)");
        return ch;
    }
    const float z = settings_.zoomRatio > 0.01f ? settings_.zoomRatio : 1.0f;
    bool changed = false;
    // 同位置再点 = 用户明确要求重新合焦。此时区域本身没变 ⇒ policy 0 下相机侧"零动作"
    //（旧行为：除了动画和触感什么都不发生，视觉上像坏了）。视作显式重扫请求。
    const bool sameSpot = roiOn_ && std::fabs(fx - roiFx_) < 0.005f &&
                          std::fabs(fy - roiFy_) < 0.005f;
    roiOn_ = true;
    roiFx_ = fx;
    roiFy_ = fy;
    if (!settings_.afOn) {
        settings_.afOn = true;   // 点按取消手动对焦，切回连续自动
        changed = true;
    }
    if (refreshRoi()) changed = true;
    // 点按走单次 AF-S（AUTO 模式）：本机 CAF 下 trigger 会触发镜头退回无穷远后的
    // full sweep，起扫慢 1s 级；afs=0 可退回纯 CAF 行为（controls.txt）。
    const int32_t wantMode = afTapPolicy_ == 2 ? 1 : 0;
    if (settings_.afMode != wantMode) {
        settings_.afMode = wantMode;
        changed = true;
    }
    afLockUntilMs_ = 0;   // 新一轮对焦：清掉上一轮的"锁定 → 回 CAF"计时
    afSLocked_ = false;
    fdSteadyCnt_ = 0;     // 旧 tracker 值会让新点按的首帧直接判成"已稳定"
    fdAtTap_ = lastFdUi_;
    fdMovedSinceTap_ = false;
    roiPendingEcho_ = true;   // 回显匹配前，合焦判定与 FocusShot 门控都要等它落地
    // policy 0 只换统计区域，不打扰 AF 状态机（最接近系统相机的做法）；1/2 都显式触发。
    // 例外：同位置重点是明确的重扫意图，任何策略都补一次 trigger。
    tapTriggerPending_ = (afTapPolicy_ != 0) || sameSpot;
    if (sameSpot) LOGI("tap focus: same spot -> explicit re-scan (trigger)");
    // "仅选择对焦位置"：用户只要把统计区挪过去、不希望相机主动重扫（afTapT0Ms_ = 0
    // 同时关掉合焦计时与 1.5s 兜底触发，见主循环）。区域本身照常下发。
    afTapT0Ms_ = (mode == 1) ? 0 : nowMs();
    afFocusLogged_ = false;
    afScanned_ = false;    // 新一轮：重新等 HAL 真的扫一轮（扫描未开始前不许报合焦）
    LOGI("tap focus: fx=%.2f fy=%.2f z=%.2f rect=[%d %d %d %d] w=%d af=%d", fx, fy, z,
         settings_.afRegion[0], settings_.afRegion[1], settings_.afRegion[2],
         settings_.afRegion[3], settings_.afRegion[4], static_cast<int>(settings_.afOn));
    return changed;
}

// AF 触发：单独一帧请求带 AF_REGIONS + AF_TRIGGER_START。
// 不能走 repeating —— trigger 是「每个请求实例执行一次」语义，留在 repeating 上会
// 每帧重启扫描（镜头持续抽动、永远到不了 FOCUSED）。这也是本函数的唯一存在理由。
void CameraEngine::sendAfTrigger() {
    if (!session_ || !session_->valid() || !ui_) return;
    refreshRoi();   // trigger 请求自带区域：先按当前 zoom 落到最新再说（幂等）
    // 挂全量 targets（与 repeating 完全一致）：trigger 请求的目标集若与 repeating 不同，
    // 本机 CamX 会按"目标集变化"处理，实测中断预览 ~500ms（2026-10-02）。
    const std::string sig = sessionSig_.empty() ? std::string("ALL") : sessionSig_;
    const std::vector<ANativeWindow*> targets = bandTargets(sig);
    if (targets.empty()) return;
    CaptureSettings s = effSettings();
    s.afTrigger = 1;                              // START（一次性，不进入 settings_）
    if (session_->captureTrigger(targets, s)) afDbgUntilMs_ = nowMs() + 2500;   // 加密采窗
}

void CameraEngine::drainUiCmds() {
    ui::Ui::Cmd cmd;
    const auto& t = cam_.traits();
    // changed 跨整轮循环累计：拖拽时一帧可能积压多个命令，统一在循环结束后只重发一次
    // repeating（本机 HAL 对高频 setRepeating 敏感，会静默掐断预览流）。
    bool changed = false;
    while (ui_ && ui_->popCmd(&cmd)) {
        // 休眠中：任意命令视为唤醒（UI 仅在休眠时推送 WAKE，但其它命令理论上也该唤醒）。
        // 先唤醒再处理，确保后续命令（如变焦/对焦）能作用在已恢复的预览流上。
        if (sleeping_) wakeCamera();
        lastInteractionMs_ = nowMs();   // 任意 UI 交互都刷新空闲计时
        switch (cmd.type) {
            case ui::Ui::Cmd::WAKE:
                break;                  // 仅唤醒，不执行任何动作
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
                    settings_.zoomRatio = std::clamp(cmd.v, zmin, zoomLimit());
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
            case ui::Ui::Cmd::SET_AE: {
                // 面板「自动曝光」开关。**不能只翻 aeOn**：关 AE 后 CaptureSettings::apply
                // 会把 SENSITIVITY/EXPOSURE_TIME 写进请求，而双自动态下这两个值是 0
                // → HAL 按 ISO 0 / 曝光 0ns 成像，预览直接全黑（2026-10-04 真机实测）。
                //
                // 关 AE = 全手动（与滚轮拖到两端同义）：两侧都退出自动，用 AE on 期间冻结的
                // lastAeIso_/lastAeExpNs_ 补成有效值 —— 曝光守恒，关 AE 前后亮度不变。
                // 注意 recomputeMixed() 在双手动时**不填值**（它的补偿分支都以 isoAuto_/
                // ssAuto_ 为条件），故关 AE 分支显式补齐即可，不必再调它（EV 也已清零，
                // 无增益可重算）。开 AE 侧才需要调：双自动分支会清零 iso/exp 交回硬件 AE。
                const auto& tr = cam_.traits();
                if (cmd.v > 0.5f) {
                    isoAuto_ = ssAuto_ = true;
                    recomputeMixed();          // 双自动分支：清零 iso/exp，交给硬件 AE
                    if (ui_) ui_->setExpAuto(true, true, 0, 0);
                } else {
                    isoAuto_ = ssAuto_ = false;
                    if (settings_.iso <= 0)
                        settings_.iso = lastAeIso_ > 0 ? lastAeIso_ : tr.isoMin;
                    if (settings_.exposureNs <= 0)
                        settings_.exposureNs =
                            lastAeExpNs_ > 0 ? lastAeExpNs_ : tr.exposureMinNs;
                    settings_.iso = std::clamp(settings_.iso, tr.isoMin, tr.isoMax);
                    settings_.exposureNs =
                        std::clamp<int64_t>(settings_.exposureNs, tr.exposureMinNs,
                                            tr.exposureMaxNs);
                    // 双手动时 EV 无处生效（UI 灰显），清零避免残留补偿叠加到固定曝光上
                    settings_.evSteps = 0;
                    if (ui_)
                        ui_->setExpAuto(false, false, settings_.iso, settings_.exposureNs);
                }
                settings_.aeOn = cmd.v > 0.5f;
                changed = true;
                break;
            }
            case ui::Ui::Cmd::SET_AWB:
                settings_.awbOn = cmd.v > 0.5f;
                LOGI("awb -> %s (mode=%d)", settings_.awbOn ? "on" : "off", settings_.awbMode);
                changed = true;
                break;
            case ui::Ui::Cmd::SET_WB_PRESET: {
                // 预设下标 → Android AWB_MODE 枚举（见 Ui.cpp kAwbEnum）
                static const int kEnum[8] = {1, 5, 6, 2, 3, 4, 7, 8};
                int idx = int(cmd.v + 0.5f);
                settings_.awbMode = (idx >= 0 && idx < 8) ? kEnum[idx] : 1;
                settings_.awbOn = true;   // 选预设即开启白平衡
                LOGI("awb preset[%d] -> AWB_MODE=%d", idx, settings_.awbMode);
                changed = true;
                break;
            }
            case ui::Ui::Cmd::SET_RAW_MODE:
                // RAW 模式：true=环形(ZSL) false=单次。变更会改变 RAW 在 repeating 中的组成，
                // 由 commitSession 的会话签名检测统一重建（见 applyControl 的 raw_mode 分支）。
                if (raw_) {
                    raw_->setRingMode(cmd.v > 0.5f);
                    changed = true;
                } else {
                    // RAW 不可用时不能静默吞掉：UI 选中态已经变了，实际却没有对象承载，
                    // 既无日志也无回推 ⇒ 表现为「面板能点、行为没变」且无从诊断。
                    LOGW("raw unavailable, ring mode ignored (device=%s)", cam_.traits().id.c_str());
                }
                break;
            case ui::Ui::Cmd::SET_SAVE_QUOTA:
                saveQuota_ = int(cmd.v);
                if (ui_) ui_->setShotQuota(saveQuota_);
                break;
            case ui::Ui::Cmd::SET_FLASH: {
                // 无闪光灯单元的设备不下发：AE_MODE_ON_ALWAYS_FLASH 会被 HAL 拒绝整包
                //（连带同请求其它 entry 一起丢）。UI 侧入口按 traits 隐藏，这里做第二道守卫。
                int m = std::clamp(int(cmd.v + 0.5f), 0, 3);
                // 只在**相机已打开**（traits 可信）时才按能力拒绝：UI 的 attach/持久化
                // 恢复跑在 glue 线程，可能早于 openCamera，此时 traits 还是空的，
                // 误判会把启动恢复出来的档位清掉（无闪光灯设备由 openCamera 统一清零）。
                if (m != 0 && cam_.opened() && !cam_.traits().flashAvailable) {
                    LOGW("flash unavailable on %s", cam_.traits().id.c_str());
                    if (ui_) ui_->setFlash(0);   // 回推纠正，避免 UI 停在无效档
                    break;
                }
                if (m != settings_.flashMode) {
                    settings_.flashMode = m;
                    static const char* kName[4] = {"off", "auto", "on", "torch"};
                    LOGI("flash -> %s", kName[m]);
                    changed = true;
                }
                break;
            }
            case ui::Ui::Cmd::SHOT:
                triggerBurst();
                break;
            case ui::Ui::Cmd::SET_FMT:
                // 拍摄格式切换：会话输出目标变化（RAW↔JPEG），必须重建会话（~300ms）。
                // 绝对语义（v>0.5=JPEG）：UI 与持久化都按目标值下发，避免反复切换累积漂移。
                jpgMode_ = cmd.v > 0.5f;
                LOGI("still format -> %s (rebuilding session)", jpgMode_ ? "JPEG" : "RAW");
                if (ui_) ui_->setFmtJpg(jpgMode_);
                if (!rebuildSession())
                    LOGE("rebuild after fmt switch failed");
                break;
            case ui::Ui::Cmd::TAP_FOCUS:
                if (onTapFocus(cmd.v, cmd.v2, int(cmd.v3 + 0.5f))) changed = true;
                break;
        }
    }
    // 整轮命令处理完后统一提交（模式变化→重建会话，仅设置变化→重发 repeating）。
    // 避免逐命令重发导致 HAL 断流。
    // AF 触发排在 repeating 更新之前：HAL 越早拿到 START 就越早起扫（setRepeatingAll 要
    // 走一趟 CamX 参数重配，相对更耗时；trigger 请求自带新区域，不依赖 repeating 先更新）。
    if (tapTriggerPending_) {
        tapTriggerPending_ = false;
        sendAfTrigger();
    }
    if (changed) {
        commitSession(true);
        LOGI("ui applied: ae=%d iso=%d exp=%lldns ev=%d zoom=%.2f uwPhys=%s",
             static_cast<int>(settings_.aeOn), settings_.iso,
             static_cast<long long>(settings_.exposureNs), settings_.evSteps,
             settings_.zoomRatio, uwPhysId_.c_str());
    }
}

// ISO/SS 混合模式状态机（见 CameraEngine.h 注释）。
// 曝光守恒锚点：AE on 期间 onFrameResult 持续跟踪 lastAeIso_/lastAeExpNs_；
// 转 AE off（任一参数手动）后冻结值不再更新。EV 步长来自 traits（本机 1/3 EV）。
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
    // 曝光守恒模型（ISO 优先 / 快门优先）：转混合时刻的 AE 解乘积
    // P = lastAeIso_ × lastAeExpNs_（∝ AE 目标曝光量）。
    //   自动参数 = P / 手动参数 × 2^EV —— 手动 ISO/SS 动 → 自动侧反比补偿（亮度恒定），
    //   EV 动 → 自动侧整体 ×2^EV；EV=0 且手动=冻结值时恰好回到 AE 解。
    //   量程钳制后守恒被破坏属物理极限（诚实降级）。
    if (lastAeIso_ > 0 && lastAeExpNs_ > 0) {
        if (isoAuto_) {
            const double ss =
                double(settings_.exposureNs > 0 ? settings_.exposureNs : t.exposureMinNs);
            settings_.iso =
                std::clamp(int(std::lround(double(lastAeIso_) * double(lastAeExpNs_) / ss *
                                           gain)),
                           t.isoMin, t.isoMax);
        }
        if (ssAuto_) {
            const double iso = double(settings_.iso > 0 ? settings_.iso : t.isoMin);
            settings_.exposureNs = std::clamp<int64_t>(
                std::lround(double(lastAeExpNs_) * double(lastAeIso_) / iso * gain),
                t.exposureMinNs, t.exposureMaxNs);
        }
    } else {
        // 冷启动尚未收到 AE 帧：退化为单参数基准（无交叉联动）
        if (isoAuto_) {
            const int base = settings_.iso > 0 ? settings_.iso : t.isoMin;
            settings_.iso = std::clamp(int(std::lround(base * gain)), t.isoMin, t.isoMax);
        }
        if (ssAuto_) {
            const int64_t base =
                settings_.exposureNs > 0 ? settings_.exposureNs : t.exposureMinNs;
            settings_.exposureNs = std::clamp<int64_t>(int64_t(std::lround(base * gain)),
                                                       t.exposureMinNs, t.exposureMaxNs);
        }
    }
    // 计算结果回推 UI：自动侧读数/滚轮即时跟随（EV/手动参数变化的联动反馈）
    if (ui_) {
        if (isoAuto_) ui_->setAutoIso(settings_.iso);
        if (ssAuto_) ui_->setAutoSsUs(settings_.exposureNs / 1000);
    }
    LOGI("mixed: isoAuto=%d ssAuto=%d iso=%d exp=%lldns ev=%d(gain=%.2f)",
         static_cast<int>(isoAuto_), static_cast<int>(ssAuto_), settings_.iso,
         static_cast<long long>(settings_.exposureNs), settings_.evSteps, gain);
}

void CameraEngine::triggerBurst() {
    // ---- JPEG 模式：单拍请求（repeating 不带 JPEG 流，快门时才让 ISP 编码一帧）----
    // 曝光/AE/对焦沿用当前会话设置。FOV 与预览严格一致（WYSIWYG，2026-09-30）：
    //   超广带 → uw 物理流单拍，逻辑 zoom 写 1.0（物理流继承裁切，quirk 之二），
    //     照片 = uw 原生 0.774x；导轨最底 0.70–0.774 段预览也画不出更广（crop 钳 1）。
    //   其余 → 逻辑流单拍，zoom 写 min(z, logicalSafeMax)。z ≤ 安全上限时照片 FOV 即
    //     预览 FOV；超出部分（长焦带高倍 / 超广带微差）由 WysiwygCropProcessor 软件
    //     中心裁切补齐 —— 预览那部分本来就是 GL 数字裁切，照片同口径。
    if (jpgMode_) {
        if (!session_ || !session_->valid()) {
            LOGW("jpeg unavailable");
            if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_, ui::Ui::kShotNoSession);
            return;
        }
        if (saveQuota_ > 0 && savesUsed_ >= saveQuota_) {
            LOGW("save quota exhausted (%d/%d) - restart app to reset", savesUsed_, saveQuota_);
            if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_, ui::Ui::kShotQuota);
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
            // 继承式 HAL：逻辑写 1.0 → uw 物理流落在原生 FOV；非继承式保持 z 不变
            s.zoomRatio = physQ_.inheritsLogicalZoom ? 1.0f : std::max(z, 1.0f);
            p.appliedZoom = uwNativeZoom_;
            stillUw_->expectShot(p);
            if (!session_->captureOnce({stillUw_->window()}, s)) {
                stillUw_->cancelShot();
                LOGE("jpeg captureOnce failed (uw physical)");
                if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_, ui::Ui::kShotCaptureFail);
                return;
            }
        } else {
            const float applied = std::clamp(z, 1.0f, zoomProf_.logicalSafeMax);
            s.zoomRatio = applied;
            p.appliedZoom = applied;
            still_->expectShot(p);
            if (!session_->captureOnce({still_->window()}, s)) {
                still_->cancelShot();
                LOGE("jpeg captureOnce failed");
                if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_, ui::Ui::kShotCaptureFail);
                return;
            }
        }
        ++savesUsed_;
        if (ui_) ui_->notifyShot(true, savesUsed_, saveQuota_, ui::Ui::kShotOk);
        LOGI("shutter triggered [jpg single %s] z=%.2f applied=%.2f (%d/%d)",
             uwShot ? "uw" : "logical", z, p.appliedZoom, savesUsed_, saveQuota_);
        return;
    }
    if (!raw_ || !raw_->ringMode()) {
        LOGW("trigger requires raw_mode=ring");
        if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_, ui::Ui::kShotNoRawRing);
        return;
    }
    if (!activePhysId().empty() ||
        settings_.zoomRatio > zoomProf_.logicalSafeMax * 1.02f) {
        // RAW 帧来自 ALL 常驻会话的**逻辑流**输出，FOV = min(z, logicalSafeMax)；
        // 预览超出部分是 GL 数字裁切，DNG 拿不到同 FOV —— 拒拍是诚实行为。
        // 两个入口都要挡：① 物理带（超广直连/长焦带，ALL 下曾放行过 8x 预览落
        // 2.5x FOV DNG）；② 近距长焦推迟区间（teleSwitchEff()=20x，5–20x 时
        // activePhysId() 为空，仅查 ① 会漏挡 —— 真机实测 z=6 放行，DNG 4.85x
        // 比预览窄 ~24%，2026-10-01 确诊）。
        LOGW("trigger unavailable beyond logical RAW reach (z=%.2f max=%.2f phys=%s)",
             settings_.zoomRatio, zoomProf_.logicalSafeMax, activePhysId().c_str());
        if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_, ui::Ui::kShotPhysBand);
        return;
    }
    if (saveQuota_ > 0 && savesUsed_ >= saveQuota_) {
        LOGW("save quota exhausted (%d/%d) - restart app to reset", savesUsed_, saveQuota_);
        if (ui_) ui_->notifyShot(false, savesUsed_, saveQuota_, ui::Ui::kShotQuota);
        return;
    }
    ++savesUsed_;
    raw_->shutterBurst(4);
    if (ui_) ui_->notifyShot(true, savesUsed_, saveQuota_, ui::Ui::kShotOk);
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
    if (nearRuleOn_ && nearEnterM_ > 0.f && r.focusDistanceDiopters > 1e-3f) {
        const float distM = 1.f / r.focusDistanceDiopters;
        if (distM < nearEnterM_) {
            farCnt_ = 0;
            if (++nearCnt_ >= nearRule_.debounceFrames && !nearSubject_) {
                nearSubject_ = true;
                bandDirty_.store(true, std::memory_order_release);
                if (teleNearSwitch_ > 1.f)
                    LOGI("subject near (%.2fm) -> tele switch deferred to %.0fx", distM,
                         teleNearSwitch_);
            }
        } else if (distM > nearExitM_) {
            nearCnt_ = 0;
            if (++farCnt_ >= nearRule_.debounceFrames && nearSubject_) {
                nearSubject_ = false;
                bandDirty_.store(true, std::memory_order_release);
                LOGI("subject far (%.2fm) -> tele switch back at %.2fx", distM, teleSwitch_);
            }
        } else {
            nearCnt_ = 0;
            farCnt_ = 0;
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

    // AF 诊断回显：证明「HAL 收到了什么区域 + 到底扫没扫 + 焦距动没动」。
    // result 里的 AF_REGIONS 是 HAL 实际采用的区域，与下发值一致才算被接受（否则是
    // 被钳制或丢弃 —— 丢弃时用户点哪都不合焦，但 UI 方框照画，极难自查）。
    // 给 UI 的 AF 状态要"真的合上"才算数：本机 AF-S 报 FOCUSED_LOCKED 时镜头常常还在
    // 走向最终位置（实测：state=4 时 fd 还是旧位置 0.20m，之后近 1s 才走到真实的
    // 0.61m）。所以要求 [state 为合焦态] 且 [屈光度连续 3 帧稳定] 才向 UI 报绿，
    // 不做 colleges immediate green —— 这与画面清晰度无关。
    if (r.focusDistanceDiopters > 0.f) {
        // 原子快照：给引擎线程的 near_m 热更新判断「当前距离是否已在新阈值之外」
        // （见 applyControl）。回调线程写、引擎线程读，必须走原子。
        lastFdDiopters_.store(r.focusDistanceDiopters, std::memory_order_release);
        const float tol = 0.02f * std::max(1.f, r.focusDistanceDiopters);
        if (std::fabs(r.focusDistanceDiopters - lastFdUi_) <= tol) {
            if (fdSteadyCnt_ < 8) ++fdSteadyCnt_;
        } else {
            fdSteadyCnt_ = 0;
        }
        lastFdUi_ = r.focusDistanceDiopters;
    }
    // AF-S（AUTO 模式）锁定完成后 state 会掉到 0 = INACTIVE（这是规范的正常行为：未再下发
    // trigger），所以"合焦"不能只认 state==2/4 —— 那样 AF-S 合焦后就永远不绿了。
    // 判据：处于合焦态（CAF 的 PASSIVE_FOCUSED / AF-S 锁定态及其后的稳定期）**且**
    // 屈光度已连续 3 帧稳定。
    // 只有"AF 真的跑过一轮"才算合焦：[state 2/4] + [镜头稳定] + [曾进入扫描/锁定态]。
    // 第三道常被忽略却最关键：点按后的头几帧镜头还没来得及动，屈光度天然稳定，
    // 只靠前两道会在手指按下 100ms 就报绿（2026-10-02 三策略对照全部测出 ~90ms 假绿）。
    // kAfGreenMinMs 兜底的是"点按前本来就合焦、全程无需移动"的情况。
    if (r.afState == 1 || r.afState == 3 || r.afState == 4 || r.afState == 5) afScanned_ = true;
    // 点按后镜头有没有真的在走（policy 0 兜底判据 + ROI 是否被采纳的旁证）
    if (afTapT0Ms_ > 0 && fdAtTap_ > 0.f && r.focusDistanceDiopters > 0.f &&
        std::fabs(r.focusDistanceDiopters - fdAtTap_) > kFdMoveTol * std::max(1.f, fdAtTap_)) {
        fdMovedSinceTap_ = true;
    }
    // ROI 回显滞后：result 的 AF_REGIONS 与新下发值对齐之前，任何"合焦"都是旧区域的
    // 结论 —— FocusShot 会拿着它直接按快门（区域还没生效就拍了，审查 P2-8）。
    if (roiPendingEcho_ && r.afRegions[4] > 0 &&
        std::memcmp(r.afRegions, settings_.afRegion, sizeof(settings_.afRegion)) == 0) {
        roiPendingEcho_ = false;
        LOGI("af: ROI accepted by HAL in %lldms", (long long)(afTapT0Ms_ > 0 ? nowMs() - afTapT0Ms_ : 0));
    }
    const bool focusedState = (r.afState == 2 || r.afState == 4) ||
                              (afSLocked_ && r.afState == 0);
    const int64_t el = afTapT0Ms_ > 0 ? nowMs() - afTapT0Ms_ : 0;
    const bool scannedOk = afScanned_ || (afTapT0Ms_ > 0 && el >= kAfGreenMinMs);
    const bool reallyFocused = focusedState && fdSteadyCnt_ >= 3 && scannedOk;
    // roiLive = 新 ROI 已被 HAL 采纳（回显匹配）。为 false 时即便 state 报合焦也只是
    // 旧区域的结论：UI 侧据此推迟"合焦"的上报时机（FocusShot 不会提前按快门）。
    ui_->setAfState(reallyFocused ? 4 : r.afState, !roiPendingEcho_);
    // 端到端合焦耗时：点按后首次达到"合焦态 + 镜头稳定"的时刻（= 用户看到对焦框变绿）
    if (reallyFocused && !afFocusLogged_ && afTapT0Ms_ > 0) {
        afFocusLogged_ = true;
        LOGI("af: FOCUSED steady in %lld ms (policy=%d)", (long long)(nowMs() - afTapT0Ms_),
             afTapPolicy_);
    }
    if (r.afState >= 0) {
        const int64_t t = nowMs();
        const bool inWindow = t < afDbgUntilMs_;
        const bool st = r.afState != lastAfState_;
        const bool rg = std::memcmp(r.afRegions, lastAfRegions_, sizeof(r.afRegions)) != 0;
        const bool fd = std::fabs(r.focusDistanceDiopters - lastAfFd_) > 0.02f;
        // 点按后的 2.5s 加密采窗内限流到 200ms 一条（30fps 全打会淹没 logcat）
        if (st || rg || fd || (inWindow && t - lastAfLogMs_ >= 200)) {
            LOGI("af: state=%d region=[%d %d %d %d w=%d] ae=[%d %d %d %d] fd=%.3f (%.2fm)",
                 r.afState, r.afRegions[0], r.afRegions[1], r.afRegions[2], r.afRegions[3],
                 r.afRegions[4], r.aeRegions[0], r.aeRegions[1], r.aeRegions[2], r.aeRegions[3],
                 r.focusDistanceDiopters,
                 r.focusDistanceDiopters > 1e-3f ? 1.f / r.focusDistanceDiopters : -1.f);
            lastAfState_ = r.afState;
            std::memcpy(lastAfRegions_, r.afRegions, sizeof(r.afRegions));
            lastAfFd_ = r.focusDistanceDiopters;
            lastAfLogMs_ = t;
        }
    }

    // AF-S 锁定：进入 FOCUSED_LOCKED(4)/NOT_FOCUSED_LOCKED(5) 就得开始计时，到点切回 CAF。
    // 不立即切回是为了避免刚锁定就因 CAF 重评又动一次镜头（画面会"呼吸"一下）。
    if (settings_.afMode == 1 && afLockUntilMs_ == 0 && (r.afState == 4 || r.afState == 5)) {
        afLockUntilMs_ = nowMs() + kAfLockHoldMs;
        afSLocked_ = (r.afState == 4);   // UI 合焦判定用（见下方 setAfState）
        LOGI("af: AF-S %s -> hold %.1fs then back to CAF",
             r.afState == 4 ? "focused" : "failed", kAfLockHoldMs / 1000.f);
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
