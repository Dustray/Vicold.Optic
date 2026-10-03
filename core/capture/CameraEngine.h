#pragma once
// M1/M-UI 采集引擎：GL 预览（core/ui 渲染器）+ 拍摄/控制命令消费 + RAW（ring/once）
// + ZSL 快门回溯 + 断流重连。
// 触发/控制源（统一经 triggerBurst()/applyControl 的配额闸门）：
//   1. GL UI（core/ui/Ui 命令队列，setUi 注入）
//   2. controls.txt（过渡通道；一次性命令自消费，见 pollControls）
// 持久键：ae iso exp_us ss ev af focus_d awb zoom raw_mode save_quota uvrot fps_log disp
// 一次性键：zsl_shutter=N shot_raw=1

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/capture/CameraDevice.h"
#include "core/capture/CaptureSession.h"
#include "core/capture/CaptureSettings.h"
#include "core/capture/RawCapture.h"
#include "core/capture/StillCapture.h"
#include "core/capture/StillPipeline.h"
#include "core/device/DeviceRegistry.h"
#include "core/device/IOpticDevice.h"
#include "core/util/ControlFile.h"

namespace optic::ui {
class Ui;
}

namespace optic::capture {

class CameraEngine {
public:
    CameraEngine() = default;
    ~CameraEngine();

    // ---- 生命周期（全异步：主线程永不阻塞）----
    // HAL 在打盹瞬间可能把 device close 挂起（2026-09-30 两次实锤：closeSession >5s
    // 不返回）——若主线程同步 join 引擎线程，surfaceDestroyed/onPause 冻结 → ANR。
    // 因此：stop 只置标志立即返回，收摊（closeSession + camera close）由引擎线程
    // 自己完成；启动若撞上收摊未完成，登记意图后由主循环 tryStart 重试。
    void requestStart(const std::string& dataDir);   // 登记 + 尝试启动
    bool tryStart();                                 // 主循环每轮调用；无阻塞
    void stop();                                     // 异步停：置标志即返回
    bool running() const { return running_.load(std::memory_order_acquire); }
    bool stopped() const { return stopped_.load(std::memory_order_acquire); }

    // GL UI（optic.cpp 注入；引擎线程消费其命令队列，预览窗挂会话输出）
    // 实现在 .cpp（此处只有 Ui 的前向声明，不能 inline 调其成员）
    void setUi(ui::Ui* u);
    // JNI 注入（android_main）：相册写入用（MediaStore → DCIM/Camera）
    void setJni(JavaVM* vm, jobject activity) { gallery_.init(vm, activity); }

private:
    void run(std::string dataDir);
    bool openSession();
    void closeSession();
    bool rebuildSession();              // 按当前模式（逻辑/超广角物理/长焦物理）重建会话
    void retireSession();               // 旧会话 close 后进墓地（延迟析构防回调 UAF）
    void reapRetired();                 // 回收超过宽限期的退役会话
    void commitSession(bool settingsChanged);  // ALL 常驻：设置变化只改 entry 重发；单流降级：模式变化→重建
    CaptureSettings effSettings() const;       // ALL 请求的逻辑流 zoom 钳在干净带内（防踩融合坏区）
    // 当前 zoom 应直连的物理 ID（空 = 逻辑多摄）：
    //   z ∈ [0.7, 1.0) → 超广角直连；z ≥ teleMinZoom_ → 长焦直连；其余 → 逻辑。
    // 本机逻辑融合管线两个坏区（sub-1.0 与高倍数字区），物理直连绕开（2026-09-29 真机确诊）。
    // ALL 常驻模式下仅用于决定 GL 显示源（请求不再随带切换）。
    std::string activePhysId() const;
    // 当前实际所处带（= uwPhysId_ / telePhysId_ / 空），滞回判定基准（activePhysId 读）。
    // 每次 commit/rebuild 后更新。滞回防边界抖动：uw 退出需 z ≥ 1.03，
    // tele 退出需 z ≥ teleMinZoom_-0.08 —— 否则导轨在阈值附近微动会触发会话重建风暴。
    std::string physBand_;
    // 直连请求应写的相对数字变焦：长焦 = z/teleNativeZoom（0 = 不写，超广角恒原生 FOV）
    float physZoom() const;
    // ALL 请求逐摄相对变焦表：uw = z/0.7、tele = z/teleMin（≤1 钳 1 = 原生 FOV，
    // 超范围由 HAL 按各摄 zoomRatioRange 钳制）。三路流恒渲染同一用户 FOV，
    // 跨带切显示源时画面内容连续；三路常流还使 AE/AWB 持续收敛（切换色彩已稳定）。
    // 同时附上与 rel 恒一致的 SCALER_CROP_REGION（per-physical ZOOM_RATIO 被 HAL
    // 忽略时的兜底，见 PhysZoom 注释）。副作用：更新 relUw_/relTele_ 缓存。
    std::vector<CaptureSession::PhysZoom> allPhysZooms();
    void refreshPhysIds();   // 按覆盖键（uw_phys/tele_phys/phys_min）+ 探测值刷新物理布局缓存
    // band 签名："L"（逻辑主摄）/ "P:"+uwPhysId_（超广角直连）/ "P:"+telePhysId_（长焦直连）
    std::vector<ANativeWindow*> bandTargets(const std::string& sig) const;  // 该带 repeating 目标
    int slotFromSig(const std::string& sig) const;                          // 该带 GL 预览源槽位
    void pollControls();
    // 触摸对焦：UI 点按预览（预览区内占比 fx/fy）→ 用户 FOV → 逻辑 active array
    // 坐标 → settings 的 AF/AE 区域。返回 true = 设置有变化需重发 repeating。
    // mode：0=点击即对焦 1=仅选择对焦位置（不重扫、不兜底） 2=点击即对焦并拍照
    bool onTapFocus(float fx, float fy, int mode);
    // 点按后发一次 AF_TRIGGER_START（单帧请求，与 repeating 分离）。改区域本身在多数
    // 3A 实现里只更新统计窗口，未必立刻重扫 —— 必须显式触发才能"点哪合哪"。
    void sendAfTrigger();
    bool tapTriggerPending_ = false;
    // 点按对焦用单次 AF-S（见 CaptureSettings::afMode）。合焦/失败锁定后维持该时长再切回
    // CAF —— 一直锁着会让用户以为"相机不对焦了"，立刻切回又会因为 CAF 重评而动一次镜头。
    // 点按对焦三策略（controls.txt: aft=0/1/2），用于对比" HAL 侧的合焦开销 "：
    //   0 = 只换统计区域（保持 CAF，不下发 trigger）——依赖 3A 自行按新 ROI 重扫
    //   1 = 换区域 + CAF 下 trigger（典型第三方 App 做法）
    //   2 = 换区域 + 切 AF-S(AUTO) + trigger（最慢，保留做对照）
    // 实测（2026-10-02 大行程：手动推到无穷远后点近距目标，两次取均值）：
    //   0 ≈ 0.8s / 1 ≈ 1.7s / 2 ≈ 1.6s —— 显式 trigger 会让 CamX 重启 AF 状态机
    //   （先甩到无穷远再全程扫描），白跑一大段；只换 ROI 则由已在跑的 CAF 用新统计
    //   窗口自然收敛，这也是系统相机快的做法。
    int afTapPolicy_ = 0;
    bool afSingleMode_ = false;             // controls.txt: afs=1 切回 AF-S + trigger（对照用）
    // 兜底：只换 ROI 之后若 HAL 长时间没有任何 AF 动作，补一次传统 trigger（防个别固件
    // 不按新 ROI 重扫）。短于它就能正常合焦的场景不会走到这里。
    static constexpr int64_t kAfFallbackMs = 1500;
    // 点按到"用户可以认为合上了"的端到端耗时：首次满足 UI 合焦判据时打一条日志，
    // 用来横向比较三种策略（只看 afState 变化会被镜头仍在移动误导）。
    int64_t afTapT0Ms_ = 0;
    bool afFocusLogged_ = false;
    bool afScanned_ = false;             // 点按后 AF 是否真的跑过一轮（scan/lock 状态出现过）
    // ~的下限：本机 CamX 从下发 trigger 到镜头真正起扫实测可达 670ms，在此之前若镜头
    // 被"还没来得及动"骗到，会被误判成"已合焦"（2026-10-02 三策略对照全测出 90ms 的
    // 假绿）。只有真正无法移动的场合（点按前本来就在焦上）才用这个超时兜底。
    static constexpr int64_t kAfGreenMinMs = 800;
    static constexpr int64_t kAfLockHoldMs = 2500;
    int64_t afLockUntilMs_ = 0;             // >0 = 处于 AF-S 锁定窗口，到期由主循环切回 CAF
    bool afSLocked_ = false;                // 本轮 AF-S 已进入 FOCUSED_LOCKED（UI 合焦判定用）
    // 注意：必须复用同一实例（成员）。此前每轮新建局部 ControlFile，内容比对状态被重置，
    // 导致 controls.txt 全量重放 —— UI 刚设置的 zoom 会在 100ms 后被文件里的旧值盖回
    // （表现为"松手瞬间预览回到 1.0，导轨不动"，2026-09-29 真机确诊）。
    std::unique_ptr<util::ControlFile> ctl_;
    void drainUiCmds();
    void triggerBurst();
    void applyControl(const std::string& k, const std::string& v, bool& changed);
    void onFrameResult(const FrameResult& r);
    // ISO/SS 自动-手动混合模式状态机：双自动 = 真 AE（EV 走 HAL 补偿）；一自动一手动 =
    // AE off，曝光守恒模型：自动参数 = 冻结 AE 乘积 P(=iso×exp) / 手动参数 × 2^EV ——
    // 手动 ISO/SS 动 → 自动侧反比补偿（亮度恒定，ISO 优先/快门优先语义），EV 动 →
    // 自动侧整体 ×2^EV；P 在 AE off 后不再更新；双手动 = AE off，EV 无效（UI 灰显）。
    // 混合计算结果回推 UI（setAutoIso/setAutoSsUs），自动侧读数/滚轮即时联动。
    void recomputeMixed();

    CameraDevice cam_;
    std::unique_ptr<CaptureSession> session_;
    std::unique_ptr<RawCapture> raw_;
    std::unique_ptr<StillCapture> still_;   // JPEG 通路（与 raw_ 互斥挂会话输出）
    // 超广角物理 JPEG 通路：超广带（z<1.0）单拍直连物理 3，照片 = 超广原生 FOV
    //（0.774x），与预览一致。没有它，超广带照片只能走逻辑流下限 1.0x（主摄），
    // 拍出来比预览广角差一截（2026-09-30 用户报告「照片焦距与预览不一致」）。
    // HAL 拒绝 5 流会话时自动拆除并记忆（uwStillOk_=false），退回逻辑流照片。
    std::unique_ptr<StillCapture> stillUw_;
    bool uwStillOk_ = true;
    // 拍摄格式：true=JPEG（默认，存系统相册） false=RAW/DNG（fmt=raw 切换）。
    // UI 角标点按切换（Cmd::SET_FMT），会话输出目标随之替换（stillWindow），
    // 需重建会话（~300ms）。
    bool jpgMode_ = true;
    // 拍摄输出的窗口：JPEG 模式返回 still_，否则按 raw 环模式返回 raw_
    ANativeWindow* stillWindow() const;
    // 快门时刻参数快照（滤镜/自定义算法管线的用户设置来源）
    StillParams makeStillParams() const;
    CaptureSettings settings_;      // fpsMin/fpsMax 在 openSession 时从 sessPol_ 取
    // 曝光参数自动状态（UI A 键 / 拖滚轮切换）与 AE 冻结值（AE on 时从结果跟踪）
    bool isoAuto_ = true, ssAuto_ = true;
    int lastAeIso_ = 0;
    int64_t lastAeExpNs_ = 0;
    std::string dataDir_;
    util::GalleryWriter gallery_;   // JPEG → 系统相册（DCIM/Camera）

    // 异步生命周期状态（stop() 只置标志；running_/stopped_ 由引擎线程在收摊尾部翻转）
    std::atomic<bool> stopped_{true};
    std::atomic<bool> wantRun_{false};
    std::string pendingDataDir_;    // 主线程写、tryStart 读（同线程），无竞争

    // 每启动拍摄配额（硬盘保护；save_quota=N 可调，0=不限制——曾误实现为"禁用所有拍摄"）
    // 8 次 ≈ 8×4 张 DNG ≈ 0.8GiB 上限；真机调参走 controls.txt: save_quota=N
    int saveQuota_ = 8;
    int savesUsed_ = 0;

    ui::Ui* ui_ = nullptr;

    // 主循环节拍：命令通知可即时唤醒（见 setUi），无命令时退回 50ms 轮询。
    std::mutex tickMx_;
    std::condition_variable tickCv_;
    std::atomic<bool> cmdWake_{false};

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> deviceLost_{false};
    int reconnects_ = 0;

    uint64_t frameCount_ = 0;
    int64_t firstTs_ = 0, lastTs_ = 0;
    bool forceLog_ = false;
    bool uwAllowed_ = true;   // 超广角默认开启：本机超广角已改为直连物理摄像头，不再走损坏的融合管线
    std::string forcedCamId_;   // controls.txt cam=N：强制打开指定摄像头 ID（诊断用，绕过逻辑多摄切换）
    std::string forcedUwPhys_;  // controls.txt uw_phys=N：强制指定超广角物理摄像头 ID（诊断用）
    std::string forcedTelePhys_;// controls.txt tele_phys=N：强制指定长焦物理摄像头 ID（诊断用）
    float forcedPhysMin_ = 0.f; // controls.txt phys_min=N：强制长焦直连起始倍率（0=用自动值，诊断用）
    float forcedTeleNative_ = 0.f; // controls.txt tele_native=N：强制长焦带基 az（肉眼校准用）
    std::string uwPhysId_;      // 当前生效的超广角物理 ID（forcedUwPhys_ > 自动探测 > 回退 "2"）
    std::string telePhysId_;    // 当前生效的长焦物理 ID（forcedTelePhys_ > 自动探测）
    float teleMinZoom_ = 1e9f;  // 长焦原生倍率（= teleNativeZoom，长焦带 crop 的换算带基）
    // ---- 机型差异一律来自 device 层 ----
    // 这些数值（融合管线坏区边界、接管点、物理流继承裁切、会话流数上限…）是**真机
    // 实测结论**，无法从 CameraCharacteristics 问出来。业务层不得自带默认值 ——
    // 否则第二台机型会带着 pandora 的参数跑（会把 EOS/画质/近距规则全搞错）。
    // 仍可用 controls.txt 覆盖（near_m / tele_near / phys_min 等诊断键）。
    optic::device::IOpticDevice& dev_ = optic::device::currentDevice();
    optic::device::ZoomProfile zoomProf_{dev_.zoomProfile()};
    optic::device::NearTakeoverRule nearRule_{dev_.nearTakeover()};
    optic::device::PhysQuirks physQ_{dev_.physQuirks()};
    optic::device::SessionPolicy sessPol_{dev_.sessionPolicy()};

    // 长焦接管点：逻辑流能健康出帧的最大倍率（本机实测 4.8 干净、5.0 起持续断流 ——
    // 2026-09-30）。接管点必须在安全区内，否则用户拖到 5x 时逻辑流已在断流：
    // 画面先冻结、再随长焦接管瞬间跳变（用户所见「5x 瞬间放大好多」）。
    // 用户口径的长焦起点（与系统相机一致：≥5x 用长焦）必须 ≥ 逻辑流安全上限。
    // 变焦上界：UI 已注入时以 UI 为准（会被 setZoomRange 按 HAL 量程收紧），
    // 否则退回机型层口径（避免 Engine/Ui 各拿一套上界导致请求被拒）。
    float zoomLimit() const { return zoomProf_.rangeMax; }
    float teleSwitch_ = 1e9f;   // = min(teleMinZoom_, zoomProf_.teleSwitchUser)：实际接管点
    // 近距推迟接管（对齐系统相机行为：对焦距离过近时长焦对不上焦，1–20x 恒主摄）：
    // 依据逻辑流 result 的 LENS_FOCUS_DISTANCE 判定被摄距离，滞回 + 帧数去抖。
    float teleNearSwitch_ = nearRule_.deferredSwitch;   // controls.txt tele_near=N 可调（0=关）
    float nearEnterM_ = nearRule_.enterM;               // controls.txt near_m=N 可调（0=关距离判定）
    float nearExitM_ = nearRule_.exitM;                 // 滞回退出阈值（> enter）
    bool nearRuleOn_ = nearRule_.enabled;               // 机型是否启用近距推迟接管
    float uwNativeZoom_ = 0.7f; // 超广角光学倍率 f(uw)/f(main)（真机 0.388；探测失败回退 0.7）
                                // —— FOV 换算基准，与导轨下限 0.7 无关，混用会让超广/主摄衔接错位
    bool sessionIsPhysical_ = false;  // 当前会话是否为物理直连
    std::string sessionSig_;          // 当前 active band（"L"/"P:cameraId"，见 bandTargets）
    // 多流常驻会话（M-MC）：逻辑 + uw + tele 三路预览输出一次 configure，跨带只切换
    // repeating 请求（无 endConfigure ~290ms 冻结、纹理常驻无黑帧）。
    // HAL 拒绝组合时降级单流（multiStreamFailed_ 记忆，跨带回退为会话重建）。
    bool multiStream_ = false;
    bool multiStreamFailed_ = false;
    bool lastRawRing_ = false;        // 检测 RAW 进出 repeating（ALL 请求目标集变化 → 重建）
    // ALL 请求逐摄写入的相对变焦缓存（onFrameResult 的 appliedZoom 回传换算用）
    float relUw_ = 0.f, relTele_ = 0.f;
    int lastDispSlot_ = -1;     // 上次下发的 GL 显示 slot（变化才打日志）
    bool manualDisp_ = false;   // disp 诊断键手动锁定显示源（分带变化时才交还自动）
    // 近距状态机（onFrameResult 回调线程更新；翻转时置 bandDirty_ 让引擎线程重发请求）
    bool nearSubject_ = false;
    int nearCnt_ = 0, farCnt_ = 0;
    std::atomic<bool> bandDirty_{false};

    // 当前生效的长焦接管点：近距时推迟（长焦模组最小对焦距离之外，与系统相机一致）
    float teleSwitchEff() const {
        return (nearSubject_ && teleNearSwitch_ > 1.f) ? teleNearSwitch_ : teleSwitch_;
    }

    // 物理摄逐键变焦能力快照（probePhysCaps，rebuildSession 时刷新）：ALL 带内连续变焦
    // 依赖 per-physical ZOOM_RATIO 生效；部分 HAL 不支持/忽略该键（表现为镜头固定在
    // 原生焦段，2026-09-29 真机症状），届时按 cropKey 决定回退方案。
    struct PhysCaps {
        bool zoomKey = false, cropKey = false;
        float zmin = 1.f, zmax = 1.f;   // CONTROL_ZOOM_RATIO_RANGE
        int32_t aa[4] = {};             // SENSOR_INFO_ACTIVE_ARRAY_SIZE (x, y, w, h)
    };
    std::map<std::string, PhysCaps> physCaps_;
    void probePhysCaps();

    // ---- 时间戳对齐的 az 回传（消除「结果 az 与显示帧错位」的泵动闪烁） ----
    // 结果回调线程只把 (timestamp, zoomRatio) 入环；显示线程消费纹理时按该帧
    // SENSOR_TIMESTAMP 查环取当时的 zoomRatio 作为 az —— crop 与显示帧严格同源，
    // 显示 FOV = az × crop = zoom_ 恒成立，快拖时不再出现两焦距来回闪。
    struct ResTs {
        int64_t ts = 0;
        float zoom = 1.f;
    };
    std::mutex resMx_;
    std::vector<ResTs> resRing_;        // 按 ts 递增，容量 64
    float azForSlot(int slot, int64_t tsNs);
    int32_t logAa_[4] = {};             // 逻辑摄 ACTIVE_ARRAY（漂移诊断基准）
    float lastAzLogZoom_ = 0.f;         // az meta 日志去重

    // AF 回显诊断（触摸对焦闭环）：只有状态/回显区域/屈光度变化才打，避免 30fps 刷屏；
    // afDbgUntilMs_ 是点按后的加密采窗。
    int32_t lastAfState_ = -1;
    int32_t lastAfRegions_[5] = {};
    float lastAfFd_ = -1.f;
    int64_t afDbgUntilMs_ = 0;
    int64_t lastAfLogMs_ = 0;
    // UI 合焦判定的稳态跟踪（AF 报锁定时镜头常仍在移动，见 onFrameResult）
    float lastFdUi_ = -1.f;
    int fdSteadyCnt_ = 0;

    // 退役会话墓地：重建时旧会话立即 close（停止回调流），但对象延迟 1.5s 才析构。
    // 框架回调线程（C2N-dev-looper）在 close 返回后仍可能携 in-flight 回调访问
    // capCbs_/mutex_；立即析构会 UAF（2026-09-29 真机 SIGABRT：
    // "pthread_mutex_lock called on a destroyed mutex"）。
    struct RetiredSession {
        std::unique_ptr<CaptureSession> sess;
        int64_t retiredMs = 0;
    };
    std::vector<RetiredSession> retired_;

    // 预览存活看门狗：repeating 被 HAL 静默丢弃（本机逻辑多摄切超广角时偶发）时
    // 自动重发 setRepeating 尝试恢复。lastResultMs_ 记录最近一次 capture result 时刻。
    int64_t lastResultMs_ = 0;
    int64_t nowMs() const;
    int stallRetries_ = 0;

    // 帧节奏探针：相邻 result 间隔 >70ms（30fps 标称 33ms）计一次"节奏断裂"。
    // 用于区分卡顿来源：带内 setRepeating 重发（间歇性小断裂）vs 跨带会话重建
    // （一次大断裂数百 ms）。
    int frameGaps_ = 0;
    int64_t maxGapMs_ = 0;
    int64_t lastFrameMs_ = 0;
};

} // namespace optic::capture
