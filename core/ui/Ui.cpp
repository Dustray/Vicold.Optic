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

const int Ui::kIsoStops[kIsoStopsN] = {100, 200, 400, 800, 1600, 3200, 6400, 12800};
const int Ui::kSsStops[kSsStopsN] = {8, 30, 60, 125, 250, 500, 1000, 2000, 4000};

// ---- 设计空间布局常量（camera-ui.html 移植）----
namespace layout {
constexpr Rgba kBg{0, 0, 0, 1};
constexpr Rgba kRail{10 / 255.f, 10 / 255.f, 12 / 255.f, 1};                  // #0A0A0C
constexpr Rgba kLine{1, 1, 1, 0.10f};
constexpr Rgba kAccent{245 / 255.f, 158 / 255.f, 11 / 255.f, 1};              // #F59E0B
constexpr Rgba kInk{10 / 255.f, 10 / 255.f, 12 / 255.f, 1};                   // 强调底上的字
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

// 左导轨
constexpr float kSafeW = 80, kRailLX = 80, kRailLW = 84;
constexpr float kZoomTrackX = 98, kZoomTrackY = 138, kZoomTrackW = 48, kZoomTrackH = 440;
constexpr float kReadoutY = 104;              // 焦距读数行顶
constexpr float kToggleY = 590, kToggleH = 28;
// 预览区
constexpr float kPreviewX = 232, kPreviewY = 0, kPreviewW = 960, kPreviewH = 720;
// HUD
constexpr float kHudY = 16, kChipH = 24, kChipPadX = 10;
// 直方图
constexpr float kHistW = 132, kHistH = 66, kHistY = 52;
// AF
constexpr float kAfX = 664, kAfY = 312, kAfSize = 96, kAfTagY = 264;
// EV 面板
constexpr float kEvPanelX = 256, kEvPanelY = 622, kEvPanelW = 340, kEvPanelH = 74;
constexpr float kEvTrackX = 268, kEvTrackW = 316, kEvTrackY = 664, kEvTrackH = 20;
// 右导轨
constexpr float kRailRX = 1260, kRailRW = 300;
constexpr float kIsoTrackX = 1288, kSsTrackX = 1384, kTrackY = 138, kTrackW = 48, kTrackH = 440;
constexpr float kDivX = 1360, kDivY = 260, kDivH = 200;
constexpr float kShutterX = 1456, kShutterY = 322, kShutterD = 76;
constexpr float kValueY = 592;                // 数值行顶
constexpr float kLabelY = 118;                // 标签行顶
} // namespace layout

using namespace layout;

namespace {
constexpr float kPadF = 20.f / 440.f;      // 滑轨上下内边距占比
constexpr float kThumbF = 30.f / 440.f;    // 滑块高占比
constexpr float kUsableF = 1 - kPadF * 2;

// 轨道比例 → thumb 顶部（轨道内像素）
float thumbTop(float f) { return (kPadF + (1 - f) * kUsableF - kThumbF / 2) * kTrackH; }
} // namespace

Ui::~Ui() = default;

// ---- 生命周期 ----

bool Ui::attach(ANativeWindow* win) {
    if (attached() && gl_.width() == ANativeWindow_getWidth(win)) return true;
    if (!gl_.attach(win)) return false;
    if (!gl_.bakeFont(26.f)) LOGE("font bake failed (text disabled)");

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
    } else {
        offX_ = -overflowX / 2.f;   // 设计窄于窗口：居中留白
    }
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
    // 快门（圆内判定，略放大热区）
    float scx = kShutterX + kShutterD / 2, scy = kShutterY + kShutterD / 2;
    if (std::hypot(x - scx, y - scy) <= kShutterD / 2 + 8) {
        shutterDown_ = true;
        return;
    }
    if (inR(x, y, kZoomTrackX, kZoomTrackY, kZoomTrackW, kZoomTrackH)) {
        drag_ = Drag::ZOOM; onMove(x, y); return;
    }
    if (inR(x, y, kIsoTrackX, kTrackY, kTrackW, kTrackH)) {
        drag_ = Drag::ISO; onMove(x, y); return;
    }
    if (inR(x, y, kSsTrackX, kTrackY, kTrackW, kTrackH)) {
        drag_ = Drag::SS; onMove(x, y); return;
    }
    if (inR(x, y, kEvTrackX - 6, kEvTrackY - 8, kEvTrackW + 12, kEvTrackH + 16)) {
        drag_ = Drag::EV; onMove(x, y); return;
    }
    if (inR(x, y, kZoomTrackX - 2, kToggleY, 52, kToggleH)) {
        zoomUnit_ ^= 1;   // mm / × 切换
        return;
    }
}

// 导轨 y 位置 → 变焦值（对数刻度 + 档位吸附）。onMove/onUp 共用，
// onUp 必须用松手位置重算：快速甩动时输入管线会丢弃末尾若干 MOVE，
// 若只信最后一次 MOVE，终值会停在中途（实测 150ms 甩动停在 8.76 而非 10.0）。
float Ui::zoomFromY(float y) const {
    float f = 1 - (y - kZoomTrackY - kPadF * kZoomTrackH) /
                      (kUsableF * kZoomTrackH);
    f = std::clamp(f, 0.f, 1.f);
    float v = kZoomMin * std::pow(kZoomMax / kZoomMin, f);
    // 设计档位 / 整数档吸附
    for (float s : kZoomStops)
        if (std::fabs(v - s) < 0.13f) { v = s; break; }
    for (int s = 1; s <= 10; ++s)
        if (std::fabs(v - float(s)) < 0.13f) { v = float(s); break; }
    return std::clamp(v, kZoomMin, kZoomMax);
}

void Ui::onMove(float x, float y) {
    switch (drag_) {
        case Drag::ZOOM: {
            zoom_ = zoomFromY(y);
            // ALL 常流：相机全向实时跟随（双向节流下发，带内重发 0 间隔）；
            // 跨带由引擎切显示 slot（纯 GL 层），无请求切换、无松手落位重建。
            pushZoomLive(zoom_);
            break;
        }
        case Drag::ISO: {
            float f = std::clamp(1 - (y - kTrackY - kPadF * kTrackH) /
                                         (kUsableF * kTrackH), 0.f, 1.f);
            int idx = std::clamp(int(std::lround(f * (kIsoStopsN - 1))), 0, kIsoStopsN - 1);
            if (idx != isoIdx_) {
                isoIdx_ = idx;
                // 命令延到 onUp 下发
            }
            break;
        }
        case Drag::SS: {
            float f = std::clamp(1 - (y - kTrackY - kPadF * kTrackH) /
                                         (kUsableF * kTrackH), 0.f, 1.f);
            int idx = std::clamp(int(std::lround(f * (kSsStopsN - 1))), 0, kSsStopsN - 1);
            if (idx != ssIdx_) {
                ssIdx_ = idx;
                // 命令延到 onUp 下发
            }
            break;
        }
        case Drag::EV: {
            float f = std::clamp((x - kEvTrackX) / kEvTrackW, 0.f, 1.f);
            float v = f * 6 - 3;
            if (std::fabs(v) < 0.06f) v = 0;                 // 中心吸附
            if (std::fabs(v - ev_) > 1e-4f) {
                ev_ = v;
                aeOn_ = true;
                // 命令延到 onUp 下发
            }
            break;
        }
        default: break;
    }
}

void Ui::onUp(float x, float y) {
    // 松手补发当前值兜底：拖拽中已节流实时下发（pushZoomLive），此处保证终值精确落位
    // （节流窗口内最后一次移动可能还没推给引擎）。
    switch (drag_) {
        case Drag::ZOOM: {
            // 用松手位置重算终值（不依赖最后一次 MOVE，见 zoomFromY 注释），
            // 推真实值精确落位（拖拽中已实时下发，此处只补节流窗口内的末段差值）
            float v = zoomFromY(y);
            if (std::fabs(v - zoom_) > 1e-4f) zoom_ = v;
            pushCmd(Cmd::SET_ZOOM, zoom_);
            lastZoomPush_ = nowSec();
            lastCamPush_ = zoom_;
            break;
        }
        case Drag::ISO:  pushCmd(Cmd::SET_ISO, float(kIsoStops[isoIdx_])); break;
        case Drag::SS:   pushCmd(Cmd::SET_EXP_US, 1e6f / float(kSsStops[ssIdx_])); break;   // µs
        case Drag::EV:   pushCmd(Cmd::SET_EV, ev_); pushCmd(Cmd::SET_AE, aeOn_ ? 1.f : 0.f); break;
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
    const Rgba cols[3] = {{79 / 255.f, 142 / 255.f, 247 / 255.f, 0.55f},
                          {74 / 255.f, 222 / 255.f, 128 / 255.f, 0.55f},
                          {245 / 255.f, 158 / 255.f, 11 / 255.f, 0.55f}};
    const int32_t* hists[3] = {histR_, histG_, histB_};
    const float pad = dim(4);
    const float innerW = w - pad * 2, innerH = h - pad * 2;
    const float binW = innerW / 64;
    const float bw = std::max(binW / 3, dim(1));

    for (int c = 0; c < 3; ++c) {
        int32_t mx = 1;
        for (int i = 0; i < 64; ++i) mx = std::max(mx, hists[c][i]);
        std::vector<float> v;
        v.reserve(64 * 12);
        for (int i = 0; i < 64; ++i) {
            float bh = float(hists[c][i]) / float(mx) * innerH;
            if (bh < 0.5f) continue;
            float bx = x + pad + i * binW + c * (binW / 3);
            float by = y + pad + innerH - bh;
            float q[12] = {bx, by, bx + bw, by, bx, by + bh,
                           bx, by + bh, bx + bw, by, bx + bw, by + bh};
            v.insert(v.end(), std::begin(q), std::end(q));
        }
        if (!v.empty()) gl_.triangles(v.data(), int(v.size() / 2), cols[c]);
    }
}

// 右侧 ISO / 曝光时间 双滑轨
void Ui::drawTracks() {
    struct VSlider { float tx; const char* label; const char* thumb; const char* value; int idx; int n; };
    const float fracs[2] = {float(isoIdx_) / (kIsoStopsN - 1), float(ssIdx_) / (kSsStopsN - 1)};
    char isoBuf[16], ssBuf[16], isoVal[16], ssVal[16];
    snprintf(isoBuf, sizeof(isoBuf), "%d", kIsoStops[isoIdx_]);
    snprintf(ssBuf, sizeof(ssBuf), "1/%d", kSsStops[ssIdx_]);
    snprintf(isoVal, sizeof(isoVal), "ISO %d", kIsoStops[isoIdx_]);
    snprintf(ssVal, sizeof(ssVal), "1/%d s", kSsStops[ssIdx_]);

    const VSlider sliders[2] = {
        {kIsoTrackX, "ISO", isoBuf, isoVal, isoIdx_, kIsoStopsN},
        {kSsTrackX, "\xe6\x9b\x9d\xe5\x85\x89\xe6\x97\xb6\xe9\x97\xb4", ssBuf, ssVal, ssIdx_, kSsStopsN},
    };

    for (int s = 0; s < 2; ++s) {
        const VSlider& v = sliders[s];
        float cx = v.tx + kTrackW / 2;
        // 标签（轨道上方居中）
        float lw = gl_.textWidth(v.label, 12 * scale_);
        gl_.text(v.label, screenX(cx) - lw / 2, screenY(kLabelY), 12 * scale_, kT2);

        // 轨道 + 中线 + 刻度
        gl_.roundedRect(screenX(v.tx), screenY(kTrackY), dim(kTrackW), dim(kTrackH),
                        dim(20), kTrack, kTrackLine, 1);
        gl_.roundedRect(screenX(v.tx + 23), screenY(kTrackY + 20), dim(2),
                        dim(kTrackH - 40), 0, kTrackLine, kNone, 0);
        std::vector<float> fs;
        std::vector<char> mj;
        for (int i = 0; i < v.n; ++i) {
            float f = float(i) / (v.n - 1);
            fs.push_back(f);
            mj.push_back(1);
            if (i < v.n - 1) {                      // 档位之间加短刻度
                fs.push_back(f + 0.5f / (v.n - 1));
                mj.push_back(0);
            }
        }
        drawVTicks(screenX(v.tx), screenY(kTrackY), dim(kTrackW), dim(kTrackH), fs, mj);

        // thumb
        float ty = thumbTop(fracs[s]);
        gl_.roundedRect(screenX(v.tx + 2), screenY(kTrackY + ty), dim(44), dim(30),
                        dim(15), kAccent, kNone, 0);
        float tw2 = gl_.textWidth(v.thumb, 11 * scale_);
        gl_.text(v.thumb, screenX(v.tx + 24) - tw2 / 2, screenY(kTrackY + ty + 9),
                 11 * scale_, kInk);

        // 数值（轨道下方居中）
        float vw = gl_.textWidth(v.value, 12 * scale_);
        gl_.text(v.value, screenX(cx) - vw / 2, screenY(kValueY), 12 * scale_, kAccent);
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
    gl_.text("AF-S \xc2\xb7 f/1.65", screenX(kAfX), screenY(kAfTagY), 10 * scale_, kT2);

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
            float w = gl_.textWidth(c.t, 12 * scale_) + 2 * dim(kChipPadX);
            gl_.roundedRect(x, y, w, dim(kChipH), dim(6), kChipBg, kNone, 0);
            gl_.text(c.t, x + dim(kChipPadX), y + dim(5), 12 * scale_, c.c);
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
        float cw = gl_.textWidth(clk, 13 * scale_);
        float right = screenX(kPreviewX + kPreviewW - 16);
        gl_.text(clk, right - cw - dim(34), screenY(kHudY + 4), 13 * scale_, kT1);

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

    // EV 面板
    gl_.roundedRect(screenX(kEvPanelX), screenY(kEvPanelY), dim(kEvPanelW), dim(kEvPanelH),
                    dim(12), kPanel, {1, 1, 1, 0.08f}, 1);
    gl_.text("\xe6\x9b\x9d\xe5\x85\x89\xe8\xa1\xa5\xe5\x81\xbf", screenX(kEvPanelX + 12),
             screenY(kEvPanelY + 12), 11 * scale_, kT2);
    {
        char v[16];
        snprintf(v, sizeof(v), "%c%.1f EV", ev_ < 0 ? '-' : (ev_ > 0 ? '+' : ' '),
                 std::fabs(ev_));
        float w = gl_.textWidth(v, 12 * scale_);
        gl_.text(v, screenX(kEvPanelX + kEvPanelW - 12) - w, screenY(kEvPanelY + 12),
                 12 * scale_, kAccent);
    }
    // EV 轨：中线 + 13 档刻度 + thumb
    gl_.roundedRect(screenX(kEvTrackX), screenY(kEvTrackY + 9), dim(kEvTrackW), dim(2), 0,
                    kTrackLine, kNone, 0);
    {
        std::vector<float> maj, min;
        float t = std::max(dim(1), 1.f);
        for (int i = 0; i <= 12; ++i) {
            bool m = i % 2 == 0;
            float tx = screenX(kEvTrackX + kEvTrackW * i / 12) - t / 2;
            float ty = screenY(kEvTrackY + (m ? 3 : 6));
            float th = dim(m ? 14 : 8);
            float q[12] = {tx, ty, tx + t, ty, tx, ty + th,
                           tx, ty + th, tx + t, ty, tx + t, ty + th};
            auto& v = m ? maj : min;
            v.insert(v.end(), std::begin(q), std::end(q));
        }
        if (!min.empty()) gl_.triangles(min.data(), int(min.size() / 2), {1, 1, 1, 0.35f});
        if (!maj.empty()) gl_.triangles(maj.data(), int(maj.size() / 2), {1, 1, 1, 0.55f});
    }
    {
        float f = (ev_ + 3) / 6;
        float tx = screenX(kEvTrackX + kEvTrackW * f);
        gl_.roundedRect(tx - dim(9), screenY(kEvTrackY), dim(18), dim(20), dim(9),
                        kAccent, {0, 0, 0, 0.55f}, dim(1.5f));
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
    // HAL 会拖慢/掐断整条 repeating）；直方图只统计当前显示源。
    const int tgt = previewTarget_.load(std::memory_order_acquire);
    {
        int32_t dr[64], dg[64], db[64];
        bool tgtGot = false;
        for (int s = 0; s < 3; ++s) {
            const bool main = (s == tgt);
            if (gl_.acquirePreview(s, main ? histR_ : dr, main ? histG_ : dg,
                                   main ? histB_ : db)) {
                slotFrames_[s].fetch_add(1, std::memory_order_relaxed);
                if (main) tgtGot = true;
            }
        }
        // 切换显示源：目标源有帧才切（旧画面保持），并启动 150ms 交叉淡化
        if (tgtGot && tgt != previewActive_) {
            if (previewActive_ >= 0) {
                fadeFrom_ = previewActive_;
                fadeT_ = 1.f;
            }
            previewActive_ = tgt;
        }
    }

    gl_.beginFrame(kBg);

    // ---- 预览（相机 → GL 纹理）----
    // 全带统一的 crop 补偿：crop = zoom_（手指目标）/ az（相机实际出图倍率）。
    //  - 为什么逻辑带也要补：相机侧下发有 120ms 节流，画面若全靠相机跟随就是
    //    8 次/s 的阶梯跳 —— 2026-09-30 真机症状「广角变焦不丝滑会卡顿」的根因。
    //    补差后相机阶梯跳一步、crop 同步收一点，FOV 每帧连续。
    //  - 物理带（quirk physPerKeyZoom=false，pandora 的 CamX 忽略 per-physical
    //    变焦键，物理流恒原生 FOV）：az = 带基常量，crop 平滑跟随手指。
    //  - az 变化（相机真实变焦生效，纹理同步更新）时必须**落位到新 target**，
    //    绝不能归一到 1：归一会让 FOV 先退回 az 再爬升，与纹理过渡叠加 =
    //    泵动闪烁（旧双模式的 bug，2026-09-29 全程闪烁的元凶）。
    {
        const float az = appliedZoom_.load(std::memory_order_acquire);
        float target = 1.f;
        if (az > 0.01f) target = std::clamp(zoom_ / az, 1.f, 16.f);
        const double t = nowSec();
        const float dt = lastCropT_ > 0 ? float(t - lastCropT_) : 0.016f;
        lastCropT_ = t;
        const bool azChanged = std::fabs(az - lastAz_) > 1e-3f;
        const bool slotChanged = previewActive_ != lastCropSlot_;
        if (azChanged || slotChanged) {
            cropSmooth_ = target;       // 元数据与像素同帧：瞬时落位，无爬升过程
            lastAz_ = az;
            lastCropSlot_ = previewActive_;
        } else {
            cropSmooth_ += (target - cropSmooth_) * (1.f - std::exp(-dt * 20.f));
        }
        gl_.setPreviewZoom(cropSmooth_);
        if (fadeT_ > 0.f) fadeT_ = std::max(0.f, fadeT_ - dt / 0.15f);
        // 诊断（低频）：crop 链路四要素实况（2026-09-30 排查物理带 crop 未生效）
        static int dbgN = 0;
        if (++dbgN % 150 == 0)
            LOGI("crop dbg: slot=%d zoom=%.2f az=%.2f crop=%.3f", previewActive_, zoom_, az,
                 cropSmooth_);
    }
    gl_.drawPreview(int(screenX(kPreviewX)), int(screenY(kPreviewY)), int(dim(kPreviewW)),
                    int(dim(kPreviewH)), resolveUvRot(), previewActive_);
    if (fadeT_ > 0.f && fadeFrom_ >= 0 && fadeFrom_ != previewActive_)
        gl_.drawPreview(int(screenX(kPreviewX)), int(screenY(kPreviewY)), int(dim(kPreviewW)),
                        int(dim(kPreviewH)), resolveUvRot(), fadeFrom_, fadeT_);
    drawPreviewOverlay();

    // ---- 左侧：摄像头避让区（真机只留黑，虚线为设计标注不绘制）----
    gl_.roundedRect(screenX(0), screenY(0), dim(kSafeW), dim(kStageH), 0, kBg, kNone, 0);

    // ---- 左导轨：变焦 ----
    gl_.roundedRect(screenX(kRailLX), screenY(0), dim(kRailLW), dim(kStageH), 0, kRail,
                    kLine, 1);
    {
        char buf[16];
        if (zoomUnit_ == 0) snprintf(buf, sizeof(buf), "%dmm", int(std::lround(kZoomBaseMm * zoom_)));
        else snprintf(buf, sizeof(buf), "%.1f\xc3\x97", zoom_);
        float w = gl_.textWidth(buf, 18 * scale_);
        gl_.text(buf, screenX(kZoomTrackX + kZoomTrackW / 2) - w / 2, screenY(kReadoutY),
                 18 * scale_, kT1);
    }

    // 变焦轨道 + 刻度 + thumb
    gl_.roundedRect(screenX(kZoomTrackX), screenY(kZoomTrackY), dim(kZoomTrackW),
                    dim(kZoomTrackH), dim(20), kTrack, kTrackLine, 1);
    gl_.roundedRect(screenX(kZoomTrackX + 23), screenY(kZoomTrackY + 20), dim(2),
                    dim(kZoomTrackH - 40), 0, kTrackLine, kNone, 0);
    {
        float logMax = std::log(kZoomMax / kZoomMin);
        std::vector<float> fs;
        std::vector<char> mj;
        for (float s : kZoomStops) {
            fs.push_back(std::log(s / kZoomMin) / logMax);
            mj.push_back(1);
        }
        for (int k = 1; k < 14; ++k) {
            fs.push_back(float(k) / 14);
            mj.push_back(0);
        }
        drawVTicks(screenX(kZoomTrackX), screenY(kZoomTrackY), dim(kZoomTrackW),
                   dim(kZoomTrackH), fs, mj);
    }
    {
        float logf = std::log(zoom_ / kZoomMin) / std::log(kZoomMax / kZoomMin);
        float zt = thumbTop(logf);
        gl_.roundedRect(screenX(kZoomTrackX + 2), screenY(kZoomTrackY + zt), dim(44),
                        dim(30), dim(15), kAccent, kNone, 0);
        char buf[12];
        if (zoomUnit_ == 0) snprintf(buf, sizeof(buf), "%d", int(std::lround(kZoomBaseMm * zoom_)));
        else snprintf(buf, sizeof(buf), "%.1f", zoom_);
        float w = gl_.textWidth(buf, 11 * scale_);
        gl_.text(buf, screenX(kZoomTrackX + 24) - w / 2, screenY(kZoomTrackY + zt + 9),
                 11 * scale_, kInk);
    }

    // 单位切换（mm / ×）
    {
        float px = screenX(kZoomTrackX - 2), py = screenY(kToggleY);
        gl_.roundedRect(px, py, dim(52), dim(kToggleH), dim(14), {1, 1, 1, 0.06f}, kLine, 1);
        struct Opt { const char* t; float x; bool on; };
        const Opt opts[2] = {{"mm", 3, zoomUnit_ == 0}, {"\xc3\x97", 27, zoomUnit_ == 1}};
        for (const auto& o : opts) {
            float ox = px + dim(o.x), oy = py + dim(3);
            float ow = dim(22), oh = dim(kToggleH - 6);
            if (o.on) gl_.roundedRect(ox, oy, ow, oh, dim(11), kAccent, kNone, 0);
            float w = gl_.textWidth(o.t, 9 * scale_);
            gl_.text(o.t, ox + ow / 2 - w / 2, oy + dim(6), 9 * scale_,
                     o.on ? kInk : kT3);
        }
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
                        dim(inner / 2), kWhite, kNone, 0);
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
        float w = gl_.textWidth(t, 11 * scale_) + dim(28);
        float tx = screenX(kPreviewX + kPreviewW / 2) - w / 2;
        float ty = screenY(kStageH - 56);
        gl_.roundedRect(tx, ty, w, dim(30), dim(15), {0, 0, 0, 0.6f}, {1, 1, 1, 0.12f}, 1);
        gl_.text(t, tx + dim(14), ty + dim(9), 11 * scale_, kWhite);
    }

    gl_.swap();
}

} // namespace optic::ui
