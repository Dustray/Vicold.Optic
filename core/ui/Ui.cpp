#include "core/ui/Ui.h"

#include "core/util/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <vector>

namespace optic::ui {

// ISO：2026-09-30 用户指定 23 档（50–12800，含非整档 1200、跳过 5000）
const float Ui::kIsoStops[kIsoStopsN] = {
    50,   64,   80,   100,  125,  160,  200,  250,
    320,  400,  500,  640,  800,  1000, 1200, 1600,
    2000, 2500, 3200, 4000, 6400, 8000, 12800,
};
// 快门：**秒**域，1/8000 s → 30 s，1/3 EV 步进全阶梯（55 档，2026-09-30 用户指定）。
// **倒序排列**（idx0 = 最快 1/8000 s）：f=0（轨道底部）= 1/8000 s、f=1（顶部）= 30 s，
// 与 ISO 轨"向下拖看更小值"的手感一致（2026-09-30 用户反馈"刻度反了"）。
// 下标 i%3==0 为整档（滚轮带数字），倒序后恰好仍对齐标准机身份档：
// 1/8000/1/4000/…/1/60/1/30/1/15/1/8/1/4/1/2 s 与 1/2/4/8/15/30 s。
// <1 的值为 1/x s（x = 1/值）；≥1 直接秒。下发用 µs：SET_EXP_US = 1e6 × 值。
const float Ui::kSsStops[kSsStopsN] = {
    1.f / 8000, 1.f / 6400, 1.f / 5000, 1.f / 4000, 1.f / 3200, 1.f / 2500, 1.f / 2000,
    1.f / 1600, 1.f / 1250, 1.f / 1000, 1.f / 800, 1.f / 640,  1.f / 500,  1.f / 400,
    1.f / 320,  1.f / 250,  1.f / 200,  1.f / 160,  1.f / 125,  1.f / 100,  1.f / 80,
    1.f / 60,   1.f / 50,   1.f / 40,   1.f / 30,   1.f / 25,   1.f / 20,   1.f / 15,
    1.f / 13,   0.1f,       0.125f,     1.f / 6,    0.2f,       0.25f,      0.3f,
    0.4f,       0.5f,       0.6f,       0.8f,       1,          1.3f,       1.6f,
    2,          2.5f,       3.2f,       4,          5,          6,          8,
    10,         13,         15,         20,         25,         30,
};

// ---- 设计空间布局常量（camera-ui.html 移植）----
namespace layout {
constexpr Rgba kBg{0, 0, 0, 1};
constexpr Rgba kRail{10 / 255.f, 10 / 255.f, 12 / 255.f, 1};                  // #0A0A0C
constexpr Rgba kLine{1, 1, 1, 0.10f};
constexpr Rgba kAccent{1, 77 / 255.f, 0, 1};                                 // #FF4D00
constexpr Rgba kT1{1, 1, 1, 1};
constexpr Rgba kT2{1, 1, 1, 0.72f};
constexpr Rgba kT3{1, 1, 1, 0.45f};
constexpr Rgba kChipBg{0, 0, 0, 0.5f};
constexpr Rgba kTrack{1, 1, 1, 0.08f};
constexpr Rgba kTrackLine{1, 1, 1, 0.35f};
constexpr Rgba kTickMajor{1, 1, 1, 0.55f};
constexpr Rgba kTickMinor{1, 1, 1, 0.30f};
constexpr Rgba kGrid{1, 1, 1, 0.12f};
constexpr Rgba kPanel{0, 0, 0, 0.45f};
constexpr Rgba kWhite{1, 1, 1, 1};
constexpr Rgba kNone{0, 0, 0, 0};

// 左导轨（面板位置运行时自适应，见 railLX_；轨道右缘 = 面板右缘）
// 布局左移 + 全出血（2026-09-30）：挖孔真身只占设计 x<88 的窄条（y 337–380），
// 变焦轨道从 92 起完全避开；预览 198 起，右侧与右导轨间留 22 设计 px。
constexpr float kSafeW = 80;                  // 挖孔避让区（横向溢出时的裁切带宽）
constexpr float kRailREnd = 192;              // 左导轨面板/轨道公共右缘
constexpr float kZoomTrackX = 92, kZoomTrackY = 130, kZoomTrackW = 100, kZoomTrackH = 448;
constexpr float kReadoutY = 104;              // 焦距读数行顶
constexpr float kUnitBtnD = 66;               // mm/× 单位切换按钮（圆形，滚轮下方）
constexpr float kUnitBtnY = 592;              // 圆顶
// 预览区
constexpr float kPreviewX = 198, kPreviewY = 0, kPreviewW = 960, kPreviewH = 720;
// HUD
constexpr float kHudY = 16, kChipH = 26, kChipPadX = 10;
// 直方图
constexpr float kHistW = 132, kHistH = 66, kHistY = 52;
// AF（跟随预览区，保持居中：预览中心 x=678）
constexpr float kAfX = 626, kAfY = 312, kAfSize = 104, kAfTagY = 264;
// EV 面板（跟随预览区左移：面板 = 预览左缘 + 24）
constexpr float kEvPanelX = 222, kEvPanelY = 622, kEvPanelW = 360, kEvPanelH = 84;
constexpr float kEvTrackX = 234, kEvTrackW = 316, kEvTrackY = 664, kEvTrackH = 24;
// 右导轨（滑轨宽 100；整组左移拉开与快门的距离：SS 滑轨右缘 1380 ↔ 快门左缘 1410）
constexpr float kRailRX = 1156, kRailRW = 380;
constexpr float kIsoTrackX = 1166, kSsTrackX = 1280, kTrackY = 138, kTrackW = 100, kTrackH = 440;
constexpr float kDivX = 1273, kDivY = 260, kDivH = 200;
constexpr float kShutterX = 1410, kShutterY = 318, kShutterD = 106;
constexpr float kAeLockD = 54;                // 测光锁定按钮（快门上方圆形）
constexpr float kAeLockY = 238;               // 圆顶（与快门间距 26）
constexpr float kAutoBtnD = 52;               // ISO/SS 的 A 自动按钮（滚轮下方圆形）
constexpr float kAutoBtnY = 596;              // 圆顶
constexpr float kLabelY = 100;                // 标签行顶（小字）
constexpr float kValueY = 116;                // 选中值行顶（大字，滚轮上方——用户指定）
constexpr float kUiZoom = 1.5f;               // UI 整体放大系数：文字 / 按钮 / 滑块统一放大
} // namespace layout

using namespace layout;

namespace {
constexpr float kPadF = 20.f / 440.f;      // 滑轨上下内边距占比
constexpr float kUsableF = 1 - kPadF * 2;
// 刻度展开倍率：滚轮行程 = 可用高 ×2 —— 相邻档位间距加倍（2026-09-30 用户指定，
// 档位加密后太密）。拖拽换算除数同步 ×kTravelX 保持 1:1 跟手（onMove/onUp）。
constexpr float kTravelX = 2.f;
} // namespace

Ui::~Ui() = default;

// ---- 生命周期 ----

bool Ui::attach(ANativeWindow* win) {
    if (attached() && gl_.width() == ANativeWindow_getWidth(win)) return true;
    if (!gl_.attach(win)) return false;
    // 字体烘焙固定高分辨率（56px）：渲染时 px 永远 < 烘焙值 → 始终为**缩小**采样，
    // 文字边缘锐利；且与 kUiZoom 解耦，后续再调大字号也不糊。
    if (!gl_.bakeFont(56.f)) LOGE("font bake failed (text disabled)");

    // 预览源尺寸可经 controls.txt 覆盖（preview_w / preview_h），默认 1920x1440 (4:3)
    int pw = 1920, ph = 1440;
    if (!dataDir_.empty()) {
        std::ifstream f(dataDir_ + "/controls.txt");
        std::string line;
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "preview_w") pw = std::atoi(v.c_str());
            else if (k == "preview_h") ph = std::atoi(v.c_str());
            else if (k == "uvrot" || k == "uv_rot") uvRotOverride_ = std::atoi(v.c_str());
        }
    }
    if (pw <= 0 || ph <= 0) { pw = 1920; ph = 1440; }
    // 三路预览源一次建齐（0=逻辑 / 1=uw / 2=tele）：AImageReader 创建无 GL 依赖。
    // 单流降级模式下 slot1/2 空闲（未挂会话不出帧，无开销）。
    for (int i = 0; i < 3; ++i) gl_.makePreviewSource(i, pw, ph);

    // cover：等比铺满。横向溢出优先裁掉左侧摄像头避让区（80 设计 px 的留白），
    // 剩余再两侧均分——这样无论系统为挖孔保留多宽，快门都不会被裁；纵向溢出两侧均分。
    scale_ = std::max(float(gl_.width()) / kStageW, float(gl_.height()) / kStageH);
    float overflowX = kStageW * scale_ - float(gl_.width());
    if (overflowX > 0) {
        float left = std::min(overflowX, kSafeW * scale_);
        offX_ = -(left + (overflowX - left) / 2.f);
        // 有横向溢出（系统仍保留挖孔条，窗口未全出血）：面板从裁切线起，
        // 可见区即完整面板；设计 x<kSafeW 反正不可见
        railLX_ = kSafeW;
    } else {
        offX_ = -overflowX / 2.f;   // 设计窄于窗口：居中留白
        // 全出血（displayCutout inset 已隐藏，窗口铺满）：面板扩进挖孔保留区，
        // 挖孔真身只占设计 x<88 的窄条且落在近纯黑面板上，不可见
        railLX_ = 10.f;
    }
    railLW_ = kRailREnd - railLX_;
    offY_ = -(kStageH * scale_ - float(gl_.height())) / 2.f;
    LOGI("ui attached: win=%dx%d scale=%.3f off=(%.0f,%.0f)", gl_.width(), gl_.height(),
         scale_, offX_, offY_);
    return true;
}

void Ui::detach() { gl_.detach(); }

// ---- 命令队列（glue 线程生产，引擎线程消费）----

void Ui::pushCmd(Cmd::Type t, float v) {
    std::lock_guard<std::mutex> l(cmdM_);
    if (cmds_.size() < 64) cmds_.push_back({t, v});   // 拖拽节流：积压时丢弃最旧
}

bool Ui::popCmd(Cmd* out) {
    std::lock_guard<std::mutex> l(cmdM_);
    if (cmds_.empty()) return false;
    *out = cmds_.front();
    cmds_.pop_front();
    return true;
}

// ---- 输入 ----

static bool inR(float x, float y, float rx, float ry, float rw, float rh) {
    return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh;
}

static double nowSec() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
               .count() / 1000.0;
}

// 拖拽变焦节流下发：≥120ms 才入队一次（onMove 里 1e-4 去抖之后仍会高频满足，
// 这里卡时间闸）。引擎线程每轮循环还会把积压命令合并成一次提交，
// 实际 setRepeating 频率 ≈ 8/s 上限（带内实测 0 丢帧）。
void Ui::pushZoomLive(float camTarget) {
    const double now = nowSec();
    if (now - lastZoomPush_ < 0.12) return;
    pushCmd(Cmd::SET_ZOOM, camTarget);
    lastZoomPush_ = now;
    lastCamPush_ = camTarget;
}

void Ui::onInputEvent(AInputEvent* e) {
    if (AInputEvent_getType(e) != AINPUT_EVENT_TYPE_MOTION) return;
    int32_t action = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
    // 多指时只跟第一根，避免拖拽指针跳变
    if (AMotionEvent_getPointerCount(e) < 1) return;
    float x = toDesignX(AMotionEvent_getX(e, 0));
    float y = toDesignY(AMotionEvent_getY(e, 0));
    switch (action) {
        case AMOTION_EVENT_ACTION_DOWN: onDown(x, y); break;
        case AMOTION_EVENT_ACTION_MOVE: onMove(x, y); break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL: onUp(x, y); break;
        default: break;
    }
}

void Ui::onDown(float x, float y) {
    hapStop_ = -1;   // 新触摸重置落档触感跟踪
    // 快门（圆内判定，略放大热区）
    float scx = kShutterX + kShutterD / 2, scy = kShutterY + kShutterD / 2;
    if (std::hypot(x - scx, y - scy) <= kShutterD / 2 + 8) {
        shutterDown_ = true;
        hap_.click();
        return;
    }
    // 测光锁定（快门上方圆形按钮）
    {
        const float lx = kShutterX + kShutterD / 2, ly = kAeLockY + kAeLockD / 2;
        if (std::hypot(x - lx, y - ly) <= kAeLockD / 2 + 8) {
            aeLock_ = !aeLock_;
            pushCmd(Cmd::SET_AE_LOCK, aeLock_ ? 1.f : 0.f);
            hap_.click();
            return;
        }
    }
    // ISO/SS 的 A 自动按钮（滚轮下方圆形）：点按切回自动（值交还 AE + EV 驱动）
    for (int s = 0; s < 2; ++s) {
        const float bcx = (s == 0 ? kIsoTrackX : kSsTrackX) + kTrackW / 2;
        const float bcy = kAutoBtnY + kAutoBtnD / 2;
        if (std::hypot(x - bcx, y - bcy) <= kAutoBtnD / 2 + 8) {
            const bool toAuto = !(s == 0 ? isoAuto_ : ssAuto_);
            if (s == 0) isoAuto_ = toAuto; else ssAuto_ = toAuto;
            pushCmd(s == 0 ? Cmd::SET_ISO_AUTO : Cmd::SET_SS_AUTO, toAuto ? 1.f : 0.f);
            // 转手动时同步落位滚轮当前档：settings 里 iso/exp 可能还是 0（从未拖过），
            // 直接下发 0 是无效请求（HAL 拒绝）
            if (!toAuto) {
                if (s == 0) pushCmd(Cmd::SET_ISO, float(kIsoStops[isoIdx_]));
                else pushCmd(Cmd::SET_EXP_US, 1e6f * kSsStops[ssIdx_]);
            }
            hap_.click();
            return;
        }
    }
    // 刻度盘：只记基准，不在按下时改值（否则手还没动值先跳到指针位置）。
    // 处于自动态的滚轮灰显不可拖（值由 AE 驱动，拖动 = 点下方 A 键切手动）。
    if (inR(x, y, kZoomTrackX, kZoomTrackY, kZoomTrackW, kZoomTrackH)) {
        drag_ = Drag::ZOOM; dragRefPos_ = y; dragRefF_ = zoomFrac(); return;
    }
    if (!isoAuto_ && inR(x, y, kIsoTrackX, kTrackY, kTrackW, kTrackH)) {
        drag_ = Drag::ISO; dragRefPos_ = y;
        dragRefF_ = float(isoIdx_) / (kIsoStopsN - 1); return;
    }
    if (!ssAuto_ && inR(x, y, kSsTrackX, kTrackY, kTrackW, kTrackH)) {
        drag_ = Drag::SS; dragRefPos_ = y;
        dragRefF_ = float(ssIdx_) / (kSsStopsN - 1); return;
    }
    const bool anyAuto = isoAuto_ || ssAuto_;   // EV 只作用于处于自动态的参数
    if (anyAuto &&  // 全手动时 EV 无意义（面板灰显），不可拖
        inR(x, y, kEvTrackX - 6, kEvTrackY - 8, kEvTrackW + 12, kEvTrackH + 16)) {
        LOGI("ev drag: down design=(%.0f,%.0f) ev=%.2f", x, y, ev_);
        drag_ = Drag::EV; dragRefPos_ = x; dragRefF_ = (ev_ - evMinEv_) / (evMaxEv_ - evMinEv_);
        return;
    }
    // mm/× 单位切换（滚轮下方圆形点按按钮）
    {
        const float ux = kZoomTrackX + kZoomTrackW / 2, uy = kUnitBtnY + kUnitBtnD / 2;
        if (std::hypot(x - ux, y - uy) <= kUnitBtnD / 2 + 8) {
            zoomUnit_ ^= 1;   // mm / × 切换
            hap_.click();
            return;
        }
    }
}

// 刻度落档触感：拖动跨到新档位时 tick 一次。id<0 = 离开吸附带（先复位，
// 往返再入同档会再次触发 —— 与机械滚轮的段落感一致）。
void Ui::hapticTickStop(int id) {
    if (id < 0) {
        hapStop_ = -1;
        return;
    }
    if (id != hapStop_) {
        hapStop_ = id;
        hap_.tick();
    }
}

// 运行时有效刻度表：首项 = 运行时下限（轨道底），其后取 kZoomStops 中高于下限者。
// 返回项数；各刻度在轨道上均分（等距刻度，用户指定 0.7-1-2-5-10-50-120 同距）。
int Ui::zoomStopsEff(float out[8]) const {
    int n = 0;
    out[n++] = zoomMin_;
    for (float s : kZoomStops)
        if (s > zoomMin_ * 1.001f && n < 8) out[n++] = s;
    return n;
}

// 轨道连续位置 f ∈[0,1] → 变焦值（关键焦段等距分段，段内对数插值 + 刻度吸附）。
// 刻度盘用相对位移驱动，onMove/onUp 都从"按下基准"重算：快速甩动时输入管线
// 会丢弃末尾若干 MOVE，只信最后一次 MOVE 会让终值停在中途。
float Ui::zoomFromF(float f) const {
    f = std::clamp(f, 0.f, 1.f);
    float eff[8];
    const int n = zoomStopsEff(eff);
    // 分段对数插值：等距刻度下每段内仍是等比滚动手感（段内线性会有值域突兀跳变）
    const float seg = f * (n - 1);
    int j = int(seg);
    if (j >= n - 1) j = n - 2;
    float v = eff[j] * std::pow(eff[j + 1] / eff[j], seg - j);
    // 刻度吸附：等距刻度 → 直接按轨道距离吸附（段位差 <0.13 段 ≈ 轨道 2.2%），各档手感一致
    for (int i = 0; i < n; ++i)
        if (std::fabs(seg - i) < 0.13f) { v = eff[i]; break; }
    return std::clamp(v, zoomMin_, kZoomMax);
}

// 变焦值 → 轨道连续位置（zoomFromF 的严格反函数，刻度绘制与 thumb 定位用）
float Ui::zoomToF(float z) const {
    float eff[8];
    const int n = zoomStopsEff(eff);
    const float v = std::clamp(z, zoomMin_, kZoomMax);
    for (int i = 0; i + 1 < n; ++i) {
        if (v <= eff[i + 1]) {
            const float t = std::log(std::max(v, eff[i]) / eff[i]) /
                            std::log(eff[i + 1] / eff[i]);
            return (i + t) / (n - 1);
        }
    }
    return 1.f;
}

void Ui::onMove(float x, float y) {
    switch (drag_) {
        case Drag::ZOOM: {
            // 搓刻度盘：刻度跟着手指走（手指向下 → 刻度向下滚 → 中心对准更大的值）
            float f = dragRefF_ + (y - dragRefPos_) / (kUsableF * kZoomTrackH * kTravelX);
            zoom_ = zoomFromF(f);
            // 落档触感：与 zoomFromF 同判据（|seg-i|<0.13 吸附带），命中段位变化才 tick
            float eff[8];
            const int n = zoomStopsEff(eff);
            const float seg = std::clamp(f, 0.f, 1.f) * (n - 1);
            int hit = -1;
            for (int i = 0; i < n; ++i)
                if (std::fabs(seg - i) < 0.13f) { hit = i; break; }
            hapticTickStop(hit);
            // ALL 常流：相机全向实时跟随（双向节流下发，带内重发 0 间隔）；
            // 跨带由引擎切显示 slot（纯 GL 层），无请求切换、无松手落位重建。
            pushZoomLive(zoom_);
            break;
        }
        case Drag::ISO: {
            float f = dragRefF_ + (y - dragRefPos_) / (kUsableF * kTrackH * kTravelX);
            isoRoll_ = std::clamp(f, 0.f, 1.f);           // 连续滚动位置（渲染用）
            isoIdx_ = std::clamp(int(std::lround(isoRoll_ * (kIsoStopsN - 1))), 0,
                                 kIsoStopsN - 1);
            hapticTickStop(isoIdx_);                      // 跨档触感
            break;   // 命令延到 onUp 下发
        }
        case Drag::SS: {
            float f = dragRefF_ + (y - dragRefPos_) / (kUsableF * kTrackH * kTravelX);
            ssRoll_ = std::clamp(f, 0.f, 1.f);
            ssIdx_ = std::clamp(int(std::lround(ssRoll_ * (kSsStopsN - 1))), 0,
                                kSsStopsN - 1);
            hapticTickStop(ssIdx_);                       // 跨档触感
            break;   // 命令延到 onUp 下发
        }
        case Drag::EV: {
            const float span = evMaxEv_ - evMinEv_;
            // 符号与渲染一致：f=0(evMin) 在左、f=1(evMax) 在右 → 手指向右 = EV 增大
            float f = dragRefF_ + (x - dragRefPos_) / (0.92f * kEvTrackW);
            float v = std::clamp(f, 0.f, 1.f) * span + evMinEv_;
            if (std::fabs(v) < evStepEv_ * 0.5f) v = 0;      // 中心吸附（拖动中）
            ev_ = v;
            hapticTickStop(int(std::lround((ev_ - evMinEv_) / evStepEv_)));   // 跨步触感
            break;   // 命令延到 onUp 下发
        }
        default: break;
    }
}

void Ui::onUp(float x, float y) {
    // 松手补发当前值兜底：拖拽中已节流实时下发（pushZoomLive），此处保证终值精确落位
    // （节流窗口内最后一次移动可能还没推给引擎）。
    switch (drag_) {
        case Drag::ZOOM: {
            // 用松手坐标从基准重算终值（末尾 MOVE 可能被输入管线丢弃），
            // 补推真实值精确落位（拖拽中已实时下发，此处只补节流窗口内的末段差值）
            float f = dragRefF_ + (y - dragRefPos_) / (kUsableF * kZoomTrackH * kTravelX);
            zoom_ = zoomFromF(f);
            pushCmd(Cmd::SET_ZOOM, zoom_);
            lastZoomPush_ = nowSec();
            lastCamPush_ = zoom_;
            break;
        }
        case Drag::ISO: {
            float f = dragRefF_ + (y - dragRefPos_) / (kUsableF * kTrackH * kTravelX);
            isoIdx_ = std::clamp(int(std::lround(std::clamp(f, 0.f, 1.f) *
                                                 (kIsoStopsN - 1))), 0, kIsoStopsN - 1);
            // 连续滚动位置回写到落位值：否则下次触摸渲染切回 isoRoll_（仍是松手前的
            // 非整数位置）→ 触摸/松手来回跳（2026-09-30 真机确诊）。
            isoRoll_ = float(isoIdx_) / (kIsoStopsN - 1);
            isoAuto_ = false;                             // 手动 ISO（引擎 SET_ISO 同步关 auto）
            pushCmd(Cmd::SET_ISO, float(kIsoStops[isoIdx_]));
            break;
        }
        case Drag::SS: {
            float f = dragRefF_ + (y - dragRefPos_) / (kUsableF * kTrackH * kTravelX);
            ssIdx_ = std::clamp(int(std::lround(std::clamp(f, 0.f, 1.f) *
                                                (kSsStopsN - 1))), 0, kSsStopsN - 1);
            ssRoll_ = float(ssIdx_) / (kSsStopsN - 1);   // 同上，防下次触摸回跳
            ssAuto_ = false;                              // 手动快门（引擎 SET_EXP_US 同步）
            pushCmd(Cmd::SET_EXP_US, 1e6f * kSsStops[ssIdx_]);   // µs（表为秒域）
            break;
        }
        case Drag::EV: {
            const float span = evMaxEv_ - evMinEv_;
            float f = dragRefF_ + (x - dragRefPos_) / (0.92f * kEvTrackW);
            ev_ = std::clamp(f, 0.f, 1.f) * span + evMinEv_;
            // 松手吸附到步长网格（traits evStep，默认 0.5）：显示值 = 相机实际生效值
            const float st = evStepEv_;
            ev_ = std::clamp(std::round(ev_ / st) * st, evMinEv_, evMaxEv_);
            if (std::fabs(ev_) < st * 0.5f) ev_ = 0;
            pushCmd(Cmd::SET_EV, ev_);
            break;
        }
        default: break;
    }
    if (shutterDown_) {
        float scx = kShutterX + kShutterD / 2, scy = kShutterY + kShutterD / 2;
        if (std::hypot(x - scx, y - scy) <= kShutterD / 2 + 12) {
            double now = nowSec();
            if (now - lastShotAt_ > 0.7) {
                lastShotAt_ = now;
                shotOk_.store(-1, std::memory_order_release);   // 等引擎回执
                pushCmd(Cmd::SHOT, 0);
                flashUntil_ = now + 0.10;
                toastUntil_ = now + 1.5;
                savedCount_ = kRingFrames;
            }
        }
        shutterDown_ = false;
    }
    drag_ = Drag::NONE;
}

// ---- 绘制 ----

// 竖直轨道刻度：fracs 为轨道比例（0=底 1=顶）
void Ui::drawVTicks(float tx, float ty, float tw, float th,
                    const std::vector<float>& fracs, const std::vector<char>& major) {
    std::vector<float> vMaj, vMin;
    for (size_t i = 0; i < fracs.size(); ++i) {
        float cy = ty + (kPadF + (1 - fracs[i]) * kUsableF) * th;
        bool m = i < major.size() ? major[i] != 0 : false;
        float x0 = tx + (m ? 16.f : 20.f) * (tw / 48.f);
        float w = (m ? 16.f : 8.f) * (tw / 48.f);
        float h = (m ? 2.f : 1.5f);
        auto& v = m ? vMaj : vMin;
        float x1 = x0 + w, y1 = cy + h * std::max(scale_, 0.5f);
        float q[12] = {x0, cy, x1, cy, x0, y1, x0, y1, x1, cy, x1, y1};
        v.insert(v.end(), std::begin(q), std::end(q));
    }
    if (!vMin.empty()) gl_.triangles(vMin.data(), int(vMin.size() / 2), kTickMinor);
    if (!vMaj.empty()) gl_.triangles(vMaj.data(), int(vMaj.size() / 2), kTickMajor);
}

// ---- 中心确认点刻度盘 ----
// 轨道正中是固定的确认线，刻度整体随当前值滚动：中心对准的刻度即当前值。
// 越靠近中心越亮（GL 批只有单色，故按 近/远 × 主/次 分四批），出轨道的刻度裁掉。
void Ui::drawRollerV(float tx, float ty, float tw, float th, float curF,
                     const RollItem* items, int n, bool grey) {
    // 行程 ×kTravelX：档位间距加倍（2026-09-30 用户澄清："刻度×2" 指**档位间距**，
    // 刻度线粗细长短不变、数字在线右侧）。拖拽换算除数在 onMove/onUp 同步，保持 1:1 跟手。
    const float travel = kUsableF * th * kTravelX;
    const float yMid = ty + th * 0.5f;
    const float sx = tw / 48.f;              // 设计宽 48 → 屏幕比例
    std::vector<float> b[4];
    struct Lbl { const char* t; float x, y, fs; Rgba c; };
    std::vector<Lbl> lbl;
    for (int i = 0; i < n; ++i) {
        const RollItem& it = items[i];
        const float cy = yMid - (it.f - curF) * travel;
        if (cy < ty + 5.f * scale_ || cy > ty + th - 5.f * scale_) continue;
        const float d = std::fabs(cy - yMid) / (th * 0.5f);
        const float x0 = tx + 4.f * sx;      // 刻度线原始尺寸：主 13 / 副 7，厚 2 / 1.5
        const float w = (it.major ? 13.f : 7.f) * sx;
        const float h = std::max(dim(it.major ? 2.f : 1.5f), 1.f);
        const float x1 = x0 + w, y1 = cy + h;
        const float q[12] = {x0, cy, x1, cy, x0, y1, x0, y1, x1, cy, x1, y1};
        auto& v = b[(d < 0.38f ? 0 : 2) + (it.major ? 0 : 1)];
        v.insert(v.end(), std::begin(q), std::end(q));
        if (it.major && it.label && d < 0.72f)
            lbl.push_back({it.label, tx + 19.f * sx, cy - 5.5f * scale_, 9.f * kUiZoom * scale_,
                           d < 0.38f ? kT1 : kT3});
    }
    static const Rgba cols[4] = {{1, 1, 1, 0.85f}, {1, 1, 1, 0.45f},
                                 {1, 1, 1, 0.30f}, {1, 1, 1, 0.14f}};
    const float ga = grey ? 0.30f : 1.f;     // 自动态灰显（值由 AE 驱动）
    for (int i = 0; i < 4; ++i)
        if (!b[i].empty())
            gl_.triangles(b[i].data(), int(b[i].size() / 2),
                          {cols[i].r, cols[i].g, cols[i].b, cols[i].a * ga});
    for (const auto& l : lbl)
        gl_.text(l.t, l.x, l.y, l.fs, {l.c.r, l.c.g, l.c.b, l.c.a * ga});
    // 中心确认区（替代原滑块）：选择带 + 确认线
    gl_.roundedRect(tx + dim(2), yMid - dim(15), tw - dim(4), dim(30), dim(8),
                    {1, 1, 1, 0.07f * ga}, kNone, 0);
    gl_.roundedRect(tx + dim(4), yMid - dim(1), tw - dim(8), dim(2), 0,
                    grey ? Rgba{1, 1, 1, 0.25f} : kAccent, kNone, 0);
}

void Ui::drawRollerH(float tx, float ty, float tw, float th, float curF,
                     const RollItem* items, int n, bool grey) {
    const float travel = 0.92f * tw;
    const float xMid = tx + tw * 0.5f;
    std::vector<float> b[4];
    struct Lbl { const char* t; float x, y, fs; Rgba c; };
    std::vector<Lbl> lbl;
    for (int i = 0; i < n; ++i) {
        const RollItem& it = items[i];
        const float cx = xMid + (it.f - curF) * travel;
        if (cx < tx + 4.f * scale_ || cx > tx + tw - 4.f * scale_) continue;
        const float d = std::fabs(cx - xMid) / (tw * 0.5f);
        const float w = std::max(dim(it.major ? 1.6f : 1.f), 1.f);
        const float y0 = ty + 1.f * scale_;
        const float h = dim(it.major ? 12.f : 7.f);
        const float x1 = cx + w, y1 = y0 + h;
        const float q[12] = {cx, y0, x1, y0, cx, y1, cx, y1, x1, y0, x1, y1};
        auto& v = b[(d < 0.38f ? 0 : 2) + (it.major ? 0 : 1)];
        v.insert(v.end(), std::begin(q), std::end(q));
        if (it.major && it.label && d < 0.8f) {
            const float fs = 9.f * kUiZoom * scale_;
            const float lw = gl_.textWidth(it.label, fs);
            lbl.push_back({it.label, cx - lw / 2, ty + dim(15), fs,
                           d < 0.38f ? kT1 : kT3});
        }
    }
    static const Rgba cols[4] = {{1, 1, 1, 0.85f}, {1, 1, 1, 0.45f},
                                 {1, 1, 1, 0.30f}, {1, 1, 1, 0.14f}};
    const float ga = grey ? 0.32f : 1.f;   // 灰显（手动曝光下 EV 不可用）
    for (int i = 0; i < 4; ++i)
        if (!b[i].empty())
            gl_.triangles(b[i].data(), int(b[i].size() / 2),
                          {cols[i].r, cols[i].g, cols[i].b, cols[i].a * ga});
    for (const auto& l : lbl)
        gl_.text(l.t, l.x, l.y, l.fs, {l.c.r, l.c.g, l.c.b, l.c.a * ga});
    gl_.roundedRect(xMid - dim(15), ty - dim(6), dim(30), th + dim(12), dim(8),
                    {1, 1, 1, 0.07f * ga}, kNone, 0);
    gl_.roundedRect(xMid - dim(1), ty - dim(4), dim(2), th + dim(8), 0,
                    grey ? Rgba{1, 1, 1, 0.25f} : kAccent, kNone, 0);
}

void Ui::drawGrid(float x, float y, float w, float h) {
    if (!gridOn_) return;
    std::vector<float> v;
    const float t = std::max(dim(1), 1.f);
    for (int i = 1; i <= 2; ++i) {                       // 竖线：x = w/3, 2w/3
        float px = x + w * i / 3.f - t / 2;
        float q[12] = {px, y, px + t, y, px, y + h, px, y + h, px + t, y, px + t, y + h};
        v.insert(v.end(), std::begin(q), std::end(q));
    }
    for (int i = 1; i <= 2; ++i) {                       // 横线：y = h/3, 2h/3
        float py = y + h * i / 3.f - t / 2;
        float q[12] = {x, py, x + w, py, x, py + t, x, py + t, x + w, py, x + w, py + t};
        v.insert(v.end(), std::begin(q), std::end(q));
    }
    gl_.triangles(v.data(), int(v.size() / 2), kGrid);
}

void Ui::drawHistogram(float x, float y, float w, float h) {
    // R/G/B 三通道同位叠加（alpha 混合成洋红/青等复合色，主流相机 UI 风格）。
    // 64-bin；3-bin 平滑抹平单 bin 噪声；gamma 0.6 压缩 —— 线性归一化下单色场景的
    // 尖峰会把其余 bins 压成贴地，视觉上"直方图坏了"。
    const Rgba cols[3] = {{239 / 255.f, 68 / 255.f, 68 / 255.f, 0.45f},    // R 红
                          {74 / 255.f, 222 / 255.f, 128 / 255.f, 0.45f},   // G 绿
                          {79 / 255.f, 142 / 255.f, 247 / 255.f, 0.45f}};  // B 蓝
    const int32_t* hists[3] = {histR_, histG_, histB_};
    const float pad = dim(4);
    const float innerW = w - pad * 2, innerH = h - pad * 2;
    const float binW = innerW / 64;
    const float bw = std::max(binW * 0.9f, dim(1));

    long long total = 0;
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 64; ++i) total += hists[c][i];
    if (total <= 0) return;                              // 尚无统计（首帧前）

    float sm[3][64];
    for (int c = 0; c < 3; ++c) {
        for (int i = 0; i < 64; ++i) {
            const int a = std::max(i - 1, 0), b = std::min(i + 1, 63);
            sm[c][i] = (float(hists[c][a]) + 2.f * float(hists[c][i]) +
                        float(hists[c][b])) / 4.f;
        }
    }

    for (int c = 0; c < 3; ++c) {
        float mx = 1.f;
        for (int i = 0; i < 64; ++i) mx = std::max(mx, sm[c][i]);
        std::vector<float> v;
        v.reserve(64 * 12);
        for (int i = 0; i < 64; ++i) {
            const float bh =
                std::pow(std::clamp(sm[c][i] / mx, 0.f, 1.f), 0.6f) * innerH;
            if (bh < 0.5f) continue;
            const float bx = x + pad + i * binW;
            const float by = y + pad + innerH - bh;
            const float q[12] = {bx, by, bx + bw, by, bx, by + bh,
                                 bx, by + bh, bx + bw, by, bx + bw, by + bh};
            v.insert(v.end(), std::begin(q), std::end(q));
        }
        if (!v.empty()) gl_.triangles(v.data(), int(v.size() / 2), cols[c]);
    }

    // 裁切警示：任一通道端点 bin 占比 > 1.5% → 对应边缘红色窄条（过曝/欠曝提示）
    const double clipN = double(total) * 0.015;
    const bool clipLo = hists[0][0] > clipN || hists[1][0] > clipN || hists[2][0] > clipN;
    const bool clipHi = hists[0][63] > clipN || hists[1][63] > clipN || hists[2][63] > clipN;
    if (clipLo || clipHi) {
        const Rgba clipC{1.f, 0.22f, 0.22f, 0.9f};
        const float cw = dim(2.5f), cy0 = y + pad, ch = innerH;
        std::vector<float> cv;
        cv.reserve(24);
        if (clipLo) {
            const float cx0 = x + pad;
            const float q[12] = {cx0, cy0, cx0 + cw, cy0, cx0, cy0 + ch,
                                 cx0, cy0 + ch, cx0 + cw, cy0, cx0 + cw, cy0 + ch};
            cv.insert(cv.end(), std::begin(q), std::end(q));
        }
        if (clipHi) {
            const float cx0 = x + w - pad - cw;
            const float q[12] = {cx0, cy0, cx0 + cw, cy0, cx0, cy0 + ch,
                                 cx0, cy0 + ch, cx0 + cw, cy0, cx0 + cw, cy0 + ch};
            cv.insert(cv.end(), std::begin(q), std::end(q));
        }
        gl_.triangles(cv.data(), int(cv.size() / 2), clipC);
    }
}

// 右侧 ISO / 曝光时间 双滑轨（自动态：滚轮灰显、数值行实时显示 AE 实测值；
// 滚轮下方 A 键切回自动，自动参数由 EV 补偿驱动）
void Ui::drawTracks() {
    struct VSlider {
        float tx; const char* label; const char* value;
        int idx, n; bool slash; const float* stops;
        int stride;   // 带数字的整档间隔：ISO=1；SS=3（1/3 EV 细分，中间档只画短刻度）
        bool a;       // 自动态
    };
    char isoVal[16], ssVal[16];
    // 自动态：显示 AE 实测值（引擎每帧回传）；手动：显示用户选中值
    const int aIso = autoIso_.load(std::memory_order_relaxed);
    if (isoAuto_)
        snprintf(isoVal, sizeof(isoVal), "ISO %d", aIso > 0 ? aIso : 100);
    else
        snprintf(isoVal, sizeof(isoVal), "ISO %.0f", (double)kIsoStops[isoIdx_]);
    if (ssAuto_) {
        const int64_t us = autoSsUs_.load(std::memory_order_relaxed);
        if (us > 0) {
            const double sec = double(us) / 1e6;
            if (sec >= 1.0) snprintf(ssVal, sizeof(ssVal), "%.4g s", sec);
            else snprintf(ssVal, sizeof(ssVal), "1/%.4g s", 1e6 / double(us));
        } else snprintf(ssVal, sizeof(ssVal), "1/8 s");
    } else if (kSsStops[ssIdx_] >= 1.f)
        snprintf(ssVal, sizeof(ssVal), "%.4g s", (double)kSsStops[ssIdx_]);
    else
        snprintf(ssVal, sizeof(ssVal), "1/%.4g s", (double)(1.f / kSsStops[ssIdx_]));

    const VSlider sliders[2] = {
        {kIsoTrackX, "ISO", isoVal, isoIdx_, kIsoStopsN, false, kIsoStops, 1, isoAuto_},
        {kSsTrackX, "\xe6\x9b\x9d\xe5\x85\x89\xe6\x97\xb6\xe9\x97\xb4", ssVal, ssIdx_,
         kSsStopsN, true, kSsStops, 3, ssAuto_},
    };

    for (int s = 0; s < 2; ++s) {
        const VSlider& v = sliders[s];
        float cx = v.tx + kTrackW / 2;
        // 标签（小字，最上）
        float lw = gl_.textWidth(v.label, 11 * kUiZoom * scale_);
        gl_.text(v.label, screenX(cx) - lw / 2, screenY(kLabelY), 11 * kUiZoom * scale_, kT3);

        // 轨道 + 中心确认刻度盘（档位刻度，拖动连续滚动）
        gl_.roundedRect(screenX(v.tx), screenY(kTrackY), dim(kTrackW), dim(kTrackH),
                        dim(20), kTrack, kTrackLine, 1);
        // 渲染位置：拖动中用连续 roll（刻度逐像素滚动），静止时落位到档位
        const float curF =
            (s == 0 ? (drag_ == Drag::ISO ? isoRoll_ : float(isoIdx_) / (kIsoStopsN - 1))
                    : (drag_ == Drag::SS ? ssRoll_ : float(ssIdx_) / (kSsStopsN - 1)));
        RollItem items[64];
        char labels[56][8];
        for (int i = 0; i < v.n; ++i) {
            // ISO 整数直显；SS 秒域：<1 带完整 "1/" 前缀（"1/8000"），≥1 带 " s" 后缀
            // （"30 s"）—— 与 1/30 s 严格区分，不再只写分母。
            if (!v.slash) snprintf(labels[i], sizeof(labels[i]), "%.0f", (double)v.stops[i]);
            else if (v.stops[i] >= 1.f)
                snprintf(labels[i], sizeof(labels[i]), "%.4g s", (double)v.stops[i]);
            else
                snprintf(labels[i], sizeof(labels[i]), "1/%.4g", (double)(1.f / v.stops[i]));
        }
        int n = 0;
        for (int i = 0; i < v.n && n < 64; ++i) {
            const bool full = (i % v.stride) == 0;        // 整档（带数字）
            // 两表自身粒度已合适（ISO 全整档、SS 1/3 EV 中间档即短刻度），不再插档间刻度
            items[n++] = {float(i) / (v.n - 1), full, full ? labels[i] : nullptr};
        }
        drawRollerV(screenX(v.tx), screenY(kTrackY), dim(kTrackW), dim(kTrackH),
                    curF, items, n, v.a);

        // 选中值（滚轮上方大字，用户指定）：自动=灰显 AE 实测值；手动=强调色用户值
        float vw = gl_.textWidth(v.value, 13 * kUiZoom * scale_);
        gl_.text(v.value, screenX(cx) - vw / 2, screenY(kValueY), 13 * kUiZoom * scale_,
                 v.a ? kT3 : kAccent);

        // A 自动按钮（滚轮下方圆形）：自动=强调环+亮字；手动=暗环+暗字
        {
            const float bx = cx - kAutoBtnD / 2, by = kAutoBtnY;
            gl_.roundedRect(screenX(bx), screenY(by), dim(kAutoBtnD), dim(kAutoBtnD),
                            dim(kAutoBtnD / 2), v.a ? Rgba{kAccent.r, kAccent.g, kAccent.b, 0.22f}
                                                    : Rgba{1, 1, 1, 0.04f},
                            v.a ? kAccent : kLine, 1.5f);
            const float fs = 13.f * kUiZoom * scale_;
            float aw = gl_.textWidth("A", fs);
            // textCenterTop：按字形实际墨迹居中（ascent 线估算会整体偏上）
            gl_.text("A", screenX(cx) - aw / 2,
                     gl_.textCenterTop("A", fs, screenY(by + kAutoBtnD / 2)), fs,
                     v.a ? kT1 : kT3);
        }
    }
}

// 预览区叠加层：网格 / HUD / 直方图 / AF / EV
void Ui::drawPreviewOverlay() {
    const float px = screenX(kPreviewX), py = screenY(kPreviewY);
    const float pw = dim(kPreviewW), ph = dim(kPreviewH);

    drawGrid(px, py, pw, ph);

    // AF 框 + 十字标记 + 标注
    gl_.roundedRect(screenX(kAfX), screenY(kAfY), dim(kAfSize), dim(kAfSize), dim(4),
                    kNone, {1, 1, 1, 0.7f}, dim(1));
    {
        std::vector<float> v;
        float cxp = screenX(kAfX + kAfSize / 2), cyp = screenY(kAfY + kAfSize / 2);
        float t = std::max(dim(1), 1.f), arm = dim(12);
        // 上边中点竖线
        float x0 = cxp - t / 2, y0 = screenY(kAfY) - arm / 2;
        float q1[12] = {x0, y0, x0 + t, y0, x0, y0 + arm,
                        x0, y0 + arm, x0 + t, y0, x0 + t, y0 + arm};
        v.insert(v.end(), std::begin(q1), std::end(q1));
        // 左边中点横线
        float x1 = screenX(kAfX) - arm / 2, y1 = cyp - t / 2;
        float q2[12] = {x1, y1, x1 + arm, y1, x1, y1 + t,
                        x1, y1 + t, x1 + arm, y1, x1 + arm, y1 + t};
        v.insert(v.end(), std::begin(q2), std::end(q2));
        gl_.triangles(v.data(), int(v.size() / 2), {1, 1, 1, 0.85f});
    }
    gl_.text("AF-S \xc2\xb7 f/1.65", screenX(kAfX), screenY(kAfTagY), 10 * kUiZoom * scale_, kT2);

    // HUD chips（RAW / DNG / 分辨率 / ZSL）
    {
        float x = screenX(kPreviewX + 16), y = screenY(kHudY);
        std::string res = std::to_string(rawW_) + " \xc3\x97 " + std::to_string(rawH_);
        struct Chip { const char* t; Rgba c; };
        const Chip chips[4] = {{"RAW", kAccent},
                               {"DNG", kWhite},
                               {res.c_str(), kT2},
                               {"ZSL \xe5\xb0\xb1\xe7\xbb\xaa", kT2}};
        for (const auto& c : chips) {
            float w = gl_.textWidth(c.t, 12 * kUiZoom * scale_) + 2 * dim(kChipPadX);
            gl_.roundedRect(x, y, w, dim(kChipH), dim(6), kChipBg, kNone, 0);
            gl_.text(c.t, x + dim(kChipPadX), y + dim(5), 12 * kUiZoom * scale_, c.c);
            x += w + dim(8);
        }
    }

    // 时钟 + 电池
    {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_r(&t, &tm);
        char clk[8];
        snprintf(clk, sizeof(clk), "%02d:%02d", tm.tm_hour, tm.tm_min);
        float cw = gl_.textWidth(clk, 13 * kUiZoom * scale_);
        float right = screenX(kPreviewX + kPreviewW - 16);
        gl_.text(clk, right - cw - dim(34), screenY(kHudY + 4), 13 * kUiZoom * scale_, kT1);

        float bx = right - dim(24), by = screenY(kHudY + 2);
        gl_.roundedRect(bx, by, dim(24), dim(14), dim(3), kNone, {1, 1, 1, 0.8f}, dim(1.5f));
        gl_.roundedRect(bx + dim(2), by + dim(2), dim(12), dim(10), dim(1.5f), kAccent, kNone, 0);
        // 电池正极帽
        gl_.roundedRect(bx + dim(24) + dim(1), by + dim(3), dim(2), dim(8), 0,
                        {1, 1, 1, 0.8f}, kNone, 0);
    }

    // 直方图
    float hx = screenX(kPreviewX + kPreviewW - 16 - kHistW);
    gl_.roundedRect(hx, screenY(kHistY), dim(kHistW), dim(kHistH), dim(6),
                    {0, 0, 0, 0.42f}, kNone, 0);
    drawHistogram(hx, screenY(kHistY), dim(kHistW), dim(kHistH));

    // EV 面板（ISO/SS 全手动时灰显：EV 只作用于处于自动态的参数，全手动下无意义）
    const bool evGrey = !(isoAuto_ || ssAuto_);
    const Rgba evT2 = evGrey ? Rgba{kT2.r, kT2.g, kT2.b, kT2.a * 0.32f} : kT2;
    const Rgba evAcc = evGrey ? Rgba{1, 1, 1, 0.25f} : kAccent;
    gl_.roundedRect(screenX(kEvPanelX), screenY(kEvPanelY), dim(kEvPanelW), dim(kEvPanelH),
                    dim(12), kPanel, {1, 1, 1, 0.08f}, 1);
    gl_.text("\xe6\x9b\x9d\xe5\x85\x89\xe8\xa1\xa5\xe5\x81\xbf", screenX(kEvPanelX + 12),
             screenY(kEvPanelY + 12), 11 * kUiZoom * scale_, evT2);
    {
        // 步长网格取整后再格式化（避免 0.30000004 之类浮点噪声），步长 <0.5 显示两位小数
        const float st = evStepEv_;
        const float v = std::round(ev_ / st) * st;
        char val[16], fmt[12];
        snprintf(fmt, sizeof(fmt), "%c%%.%df EV", v < 0 ? '-' : (v > 0 ? '+' : ' '),
                 st < 0.5f ? 2 : 1);
        snprintf(val, sizeof(val), fmt, std::fabs(v));
        const float w = gl_.textWidth(val, 12 * kUiZoom * scale_);
        gl_.text(val, screenX(kEvPanelX + kEvPanelW - 12) - w, screenY(kEvPanelY + 12),
                 12 * kUiZoom * scale_, evAcc);
    }
    // EV 轨：水平中心确认刻度盘，量程/步长按设备 traits（引擎 setEvRange 下发）。
    // 本机 pandora：±8 EV、1/3 步长（49 档）。
    {
        const float span = evMaxEv_ - evMinEv_;
        const int nSteps = std::clamp(int(std::lround(span / evStepEv_)) + 1, 2, 241);
        const int stride = std::max(1, (nSteps + 62) / 63);   // 抽稀：绘制项 ≤ ~63+整档
        RollItem items[96];
        char labels[96][8];
        int n = 0;
        for (int i = 0; i < nSteps && n < 95; ++i) {
            const float v = evMinEv_ + float(i) * evStepEv_;
            const bool major = std::fabs(v - std::round(v)) < 0.01f;   // 整数 EV 带标签
            // 抽稀时保留整数 EV 大刻度，保证 -8…+8 标签齐全
            if (i % stride != 0 && !major) continue;
            const float f = float(i) / float(nSteps - 1);              // 按值映射，非按序号
            if (major) snprintf(labels[n], sizeof(labels[n]), "%+d", int(std::lround(v)));
            items[n++] = {f, major, major ? labels[n] : nullptr};
        }
        drawRollerH(screenX(kEvTrackX), screenY(kEvTrackY), dim(kEvTrackW), dim(kEvTrackH),
                    (ev_ - evMinEv_) / span, items, n, evGrey);
    }
}

// 定向求解优先级：controls.txt 强制值 > (屏幕旋转角 − 传感器朝向) > 交给 Gl 几何自动判断。
// 取模要做正余数：C++ 里 (-1) % 4 == -1。
int Ui::resolveUvRot() const {
    if (uvRotOverride_ >= 0) return uvRotOverride_ & 3;
    if (displayRot_ >= 0) {
        int k = (displayRot_ * 90 - sensorDeg_) / 90;
        return ((k % 4) + 4) % 4;
    }
    return -1;
}

void Ui::frame() {
    if (!attached()) return;
    // 三路常流：每帧必须 drain 全部 reader（不消费会撑满 maxImages=3 队列，
    // HAL 会拖慢/掐断整条 repeating）；直方图只统计当前显示源（读回会 stall，三路全开必掉帧）。
    const int tgt = previewTarget_.load(std::memory_order_acquire);
    const int histSlot = previewActive_ >= 0 ? previewActive_ : tgt;
    {
        bool tgtGot = false;
        for (int s = 0; s < 3; ++s) {
            if (gl_.acquirePreview(s, s == histSlot)) {
                slotFrames_[s].fetch_add(1, std::memory_order_relaxed);
                if (s == tgt) tgtGot = true;
            }
        }
        gl_.copyHistogram(histSlot, histR_, histG_, histB_);
        // 切换显示源：目标源有帧**且纹理已是原生 FOV** 才切（旧画面保持 + GL crop
        // 继续补偿）。pandora quirk 之二：物理流继承逻辑 ZOOM_RATIO 裁切 —— 主摄带
        // 逻辑 4.85 时长焦纹理实际 ≈24.3x，立即切换会闪一帧错误倍率（真机 20x 切换
        // 确诊）。az 已按帧时间戳匹配（azForSlot）：az ≈ 带基 = 纹理已原生。
        // 等待期间旧 slot 继续 GL 补偿（crop 上界 32 覆盖近距 20x），逻辑 zoom=1.0
        // 落地后（~1-2 请求节流）首条原生长焦帧即翻转。
        if (tgtGot && tgt != previewActive_) {
            const float azT = az_[tgt].load(std::memory_order_acquire);
            const float base = tgt == 2 ? teleMin_ : (tgt == 1 ? zoomMin_ : 1.0f);
            if (azT > 0.01f && azT <= base * 1.5f + 0.05f) {
                if (previewActive_ >= 0) {
                    fadeFrom_ = previewActive_;
                    fadeT_ = 1.f;
                }
                LOGI("preview slot %d -> %d (az=%.2f base=%.2f)", previewActive_, tgt, azT,
                     base);
                previewActive_ = tgt;
            }
        }
    }

    gl_.beginFrame(kBg);

    // ---- 预览（相机 → GL 纹理）----
    // 全带统一的 crop 补偿：crop = zoom_（手指目标）/ az（相机实际出图倍率）。
    //  - 为什么逻辑带也要补：相机侧下发有 120ms 节流，画面若全靠相机跟随就是
    //    8 次/s 的阶梯跳。补差后相机阶梯跳一步、crop 同步收一点，FOV 每帧连续。
    //  - **az 必须与显示帧时间戳对齐**（Gl::acquirePreview → 引擎结果环查询）：
    //    旧做法用「最新结果」的 az，领先显示纹理 1-2 帧，快拖时 crop 与纹理错位，
    //    显示 FOV 在两个值间泵动（1–5x「不丝滑/来回闪」根因）。对齐后
    //    显示 FOV = az × crop ≡ zoom_ 恒成立，crop 直接落位、无需任何平滑。
    //  - 物理带（quirk physPerKeyZoom=false）：az = 带基常量，crop 纯由手指驱动。
    //  - az 变化（相机真实变焦生效）时 crop 直接落位，绝不能归一到 1：归一会让
    //    FOV 先退回 az 再爬升 = 泵动闪烁（2026-09-29 全程闪烁的元凶）。
    {
        const float az = az_[previewActive_].load(std::memory_order_acquire);
        float target = 1.f;
        // 上界 32 覆盖导轨 120x：长焦原生 ≈5x，120/5.016 ≈ 24 倍数字裁切。
        if (az > 0.01f) target = std::clamp(zoom_ / az, 1.f, 32.f);
        const double t = nowSec();
        const float dt = lastCropT_ > 0 ? float(t - lastCropT_) : 0.016f;
        lastCropT_ = t;
        gl_.setPreviewZoom(target);
        if (fadeT_ > 0.f) fadeT_ = std::max(0.f, fadeT_ - dt / 0.15f);
        // 诊断（低频）：crop 链路四要素实况
        static int dbgN = 0;
        if (++dbgN % 30 == 0)  // 诊断：每 30 帧输出 crop 链路实况（~1s/次，便于真机拖拽自测）
            LOGI("crop dbg: slot=%d zoom=%.2f az=%.2f crop=%.3f", previewActive_, zoom_, az,
                 target);
    }
    gl_.drawPreview(int(screenX(kPreviewX)), int(screenY(kPreviewY)), int(dim(kPreviewW)),
                    int(dim(kPreviewH)), resolveUvRot(), previewActive_);
    if (fadeT_ > 0.f && fadeFrom_ >= 0 && fadeFrom_ != previewActive_) {
        // 淡化旧层用**自己的 az** 求 crop：新带 crop 已按新带落位，旧带纹理若沿用会被
        // 放大 (zoom_/az_旧带) 倍 —— 跨带切换大闪烁的元凶之二（元凶之一是带基 az 陈旧，
        // 见 CameraEngine::onFrameResult）。
        const float azo = az_[fadeFrom_].load(std::memory_order_acquire);
        float fadeCrop = 1.f;
        if (azo > 0.01f) fadeCrop = std::clamp(zoom_ / azo, 1.f, 32.f);
        gl_.drawPreview(int(screenX(kPreviewX)), int(screenY(kPreviewY)), int(dim(kPreviewW)),
                        int(dim(kPreviewH)), resolveUvRot(), fadeFrom_, fadeT_, fadeCrop);
    }
    drawPreviewOverlay();

    // ---- 左侧：摄像头避让区（真机只留黑，虚线为设计标注不绘制）----
    gl_.roundedRect(screenX(0), screenY(0), dim(kSafeW), dim(kStageH), 0, kBg, kNone, 0);

    // ---- 左导轨：变焦（面板位置 attach 自适应，见 railLX_）----
    gl_.roundedRect(screenX(railLX_), screenY(0), dim(railLW_), dim(kStageH), 0, kRail,
                    kLine, 1);
    {
        char buf[16];
        if (zoomUnit_ == 0) snprintf(buf, sizeof(buf), "%dmm", int(std::lround(kZoomBaseMm * zoom_)));
        else snprintf(buf, sizeof(buf), "%.1f\xc3\x97", zoom_);
        float w = gl_.textWidth(buf, 18 * kUiZoom * scale_);
        gl_.text(buf, screenX(kZoomTrackX + kZoomTrackW / 2) - w / 2, screenY(kReadoutY),
                 18 * kUiZoom * scale_, kT1);
    }

    // 变焦轨道 + 刻度 + thumb
    gl_.roundedRect(screenX(kZoomTrackX), screenY(kZoomTrackY), dim(kZoomTrackW),
                    dim(kZoomTrackH), dim(20), kTrack, kTrackLine, 1);
    gl_.roundedRect(screenX(kZoomTrackX + 26), screenY(kZoomTrackY + 20), dim(2),
                    dim(kZoomTrackH - 40), 0, kTrackLine, kNone, 0);
    {
        // 中心确认刻度盘：关键焦段为主刻度（带标签），段间按对数等分插短刻度
        RollItem items[48];
        char labels[16][8];
        int n = 0;
        float prev = -1.f;
        for (size_t i = 0; i < sizeof(kZoomStops) / sizeof(kZoomStops[0]); ++i) {
            const float s = kZoomStops[i];
            if (s < zoomMin_ - 1e-3f) continue;          // 低于光学下限的档位不画
            const float f = zoomToF(s);
            if (prev >= 0.f) {                           // 段间短刻度（对数等分 3 份）
                for (int k = 1; k <= 3 && n < 48; ++k)
                    items[n++] = {prev + (f - prev) * float(k) / 4.f, false, nullptr};
            }
            if (n >= 48) break;
            snprintf(labels[i], sizeof(labels[i]),
                     s < 1.f ? "%.1f" : "%.0f", s);
            items[n++] = {f, true, labels[i]};
            prev = f;
        }
        drawRollerV(screenX(kZoomTrackX), screenY(kZoomTrackY), dim(kZoomTrackW),
                    dim(kZoomTrackH), zoomFrac(), items, n);
    }

    // 单位切换（mm / ×）：圆形点按按钮，显示当前单位，点按切换
    {
        const float cx = kZoomTrackX + kZoomTrackW / 2, cy = kUnitBtnY + kUnitBtnD / 2;
        gl_.roundedRect(screenX(cx - kUnitBtnD / 2), screenY(cy - kUnitBtnD / 2),
                        dim(kUnitBtnD), dim(kUnitBtnD), dim(kUnitBtnD / 2),
                        {1, 1, 1, 0.06f}, kLine, 1.5f);
        const char* t = zoomUnit_ == 0 ? "mm" : "\xc3\x97";
        const float fs = 15.f * kUiZoom * scale_;
        const float w = gl_.textWidth(t, fs);
        // textCenterTop：mm（x 高字形）与 ×（数学符号）高度不同，按墨迹精确居中
        gl_.text(t, screenX(cx) - w / 2, gl_.textCenterTop(t, fs, screenY(cy)), fs, kT1);
    }

    // ---- 右导轨 ----
    gl_.roundedRect(screenX(kRailRX), screenY(0), dim(kRailRW), dim(kStageH), 0, kRail,
                    kLine, 1);
    gl_.roundedRect(screenX(kDivX), screenY(kDivY), dim(1), dim(kDivH), 0, kLine, kNone, 0);
    drawTracks();

    // ---- 快门 ----
    {
        float s = shutterDown_ ? 0.94f : 1.0f;
        float d = kShutterD * s;
        float sx = kShutterX + (kShutterD - d) / 2, sy = kShutterY + (kShutterD - d) / 2;
        gl_.roundedRect(screenX(sx), screenY(sy), dim(d), dim(d), dim(d / 2),
                        {1, 1, 1, 0.15f}, {1, 1, 1, 0.85f}, dim(2));
        float inner = d * (58.f / 76.f);
        float off = (d - inner) / 2;
        gl_.roundedRect(screenX(sx + off), screenY(sy + off), dim(inner), dim(inner),
                        dim(inner / 2), kAccent, kNone, 0);   // 内圆 = 重点色（用户指定）
    }

    // ---- 测光锁定（快门上方圆形）：锁定时强调色实心环 + 亮字 ----
    {
        const float lx = kShutterX + kShutterD / 2 - kAeLockD / 2;
        gl_.roundedRect(screenX(lx), screenY(kAeLockY), dim(kAeLockD), dim(kAeLockD),
                        dim(kAeLockD / 2), aeLock_ ? Rgba{kAccent.r, kAccent.g, kAccent.b, 0.30f}
                                                   : Rgba{1, 1, 1, 0.06f},
                        aeLock_ ? kAccent : kLine, 1.5f);
        const float fs = 11.f * kUiZoom * scale_;
        float w = gl_.textWidth("AE", fs);
        gl_.text("AE", screenX(lx + kAeLockD / 2) - w / 2,
                 gl_.textCenterTop("AE", fs, screenY(kAeLockY + kAeLockD / 2)), fs,
                 aeLock_ ? kT1 : kT3);
    }

    // ---- 白闪 + toast ----
    double now = nowSec();
    if (now < flashUntil_) {
        Rgba f = kWhite;
        f.a = 0.85f * float((flashUntil_ - now) / 0.10);
        gl_.roundedRect(screenX(kPreviewX), screenY(kPreviewY), dim(kPreviewW),
                        dim(kPreviewH), 0, f, kNone, 0);
    }
    if (now < toastUntil_) {
        char t[64];
        int ok = shotOk_.load(std::memory_order_acquire);
        if (ok == 0) {
            // 配额用尽：如实反馈，不假装已保存
            snprintf(t, sizeof(t),
                     "\xe9\x85\x8d\xe9\xa2\x9d\xe5\xb7\xb2\xe7\x94\xa8\xe5\xb0\xbd %d/%d",
                     shotUsed_.load(std::memory_order_acquire),
                     shotTotal_.load(std::memory_order_acquire));
        } else {
            snprintf(t, sizeof(t),
                     "\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98 \xc2\xb7 DNG "
                     "\xe7\x8e\xaf\xe5\xbd\xa2\xe7\xbc\x93\xe5\x86\xb2 %d/%d",
                     savedCount_, kRingFrames);
        }
        float w = gl_.textWidth(t, 11 * kUiZoom * scale_) + dim(28);
        float tx = screenX(kPreviewX + kPreviewW / 2) - w / 2;
        float ty = screenY(kStageH - 56);
        gl_.roundedRect(tx, ty, w, dim(30), dim(15), {0, 0, 0, 0.6f}, {1, 1, 1, 0.12f}, 1);
        gl_.text(t, tx + dim(14), ty + dim(9), 11 * kUiZoom * scale_, kWhite);
    }

    gl_.swap();
}

} // namespace optic::ui
