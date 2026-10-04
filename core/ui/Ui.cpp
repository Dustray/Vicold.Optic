#include "core/ui/Ui.h"

#include "core/util/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <vector>

#include <android/looper.h>
#include <android/sensor.h>

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

// log 域最近档位（自动侧联动显示：引擎回推的连续计算值吸附到滚轮档）
static int nearestStopIdx(const float* stops, int n, float v) {
    if (!(v > 0.f)) return 0;
    int best = 0;
    float bd = 1e30f;
    for (int i = 0; i < n; ++i) {
        const float d = std::fabs(std::log(stops[i] / v));
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

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
// 构图辅助线（网格 / 安全框 / 水平仪中心十字）统一走「近黑描边 + 亮白芯线」双层。
// **不要退回单层低 alpha 白**：预览画面明暗跨度极大，12% 白在亮场景（白墙/天空/
// 高光桌面）上完全不可见——用户会报「开关网格线都没反应」，实际是画了但看不见
// （2026-10-04 真机截图确诊：预览是亮桌面，kGrid 0.12 白完全隐没）。
// 近黑描边在暗背景上给对比、亮白芯线在暗背景上给亮度 ⇒ 明暗双向可读。
constexpr Rgba kGridEdge{0, 0, 0, 0.55f};
constexpr Rgba kGridCore{1, 1, 1, 0.85f};
constexpr Rgba kPanel{0, 0, 0, 0.45f};
constexpr Rgba kWhite{1, 1, 1, 1};
constexpr Rgba kNone{0, 0, 0, 0};

// 左导轨（面板位置运行时自适应，见 railLX_；轨道右缘 = 面板右缘）
// 全出血（2026-09-30）：窗口 frame 已是整屏 2656，布局不必再整块右移避让 150px 黑条。
// 2026-10-01 布局左移 20（预览/AF）：先后尝试轨道宽 64 / 左压 64，最终因真孔吃掉轨道
// 左侧而定在「X=88 压 cutout 包络边界 + 宽 80」，详见 trackX_ 注释。
// kSafeW 已移入机型层（UiLayoutPolicy.cutoutReserveW），运行时取成员 safeW_
constexpr float kRailREnd = 168;              // 左导轨面板/轨道公共右缘
// 挖孔/避让几何来自机型层 UiLayoutPolicy（成员 trackX_ / safeW_）—— 屏幕开孔是硬件
// 属性，写死在这里会让中置挖孔/无孔机型出现「轨道被吃掉」或「白留一条黑边」
//（2026-10-01 三轮实测把 pandora 定值打成了注释里的历史记录）。
constexpr float kZoomTrackY = 130, kZoomTrackW = 80, kZoomTrackH = 448;
// 三滚轮上方「实时值」的统一排版（2026-10-01：删除静态标题，只留实时预览）
// 变焦（mm/×）与 ISO、曝光时间三处共用同一基线 / 字号 / 配色规则：
//   手动 = 强调色 kAccent；自动态跟随 AE = 灰 kT3（只有 ISO / 曝光时间会自动）。
constexpr float kReadoutY = 100;              // 行顶（ascent）：实测 SS 行含下伸到底 ~130，
                                              // 与轨道顶 138 留 8px；变焦轨道顶 130 更安全
constexpr float kReadoutFs = 16.f;            // 字号（×kUiZoom×scale_）
// 滚轮下方的圆形按钮（三滚轮共用一组规格，2026-10-01 用户要求大小一致）
// 原两套：mm/× = 66@592、A = 52@596 —— 观感参差。统一为 60@594；
// 左轨道宽 80、右轨道宽 100，60 均可容纳（可点击热区 = 半径 +8 再外扩）。
constexpr float kJogBtnD = 60;                // 直径
constexpr float kJogBtnY = 594;               // 圆顶
constexpr float kJogBtnFs = 14.f;             // 按钮内文字字号（×kUiZoom×scale_）
// 预览区（2026-10-01 左移 20：随左导轨改窄；HUD/直方图/保存胶囊均自本值推导）
constexpr float kPreviewX = 178, kPreviewY = 0, kPreviewW = 960, kPreviewH = 720;
// HUD
constexpr float kHudY = 16, kChipH = 26, kChipPadX = 10;
// 直方图
constexpr float kHistW = 132, kHistH = 66, kHistY = 52;
// 点按模式按钮（预览右上角，直方图下方），右缘与直方图对齐
constexpr float kTapModeY = kHistY + kHistH + 12, kTapModeH = 34;
// EV 面板（跟随预览区左移：面板 = 预览左缘 + 24）
constexpr float kEvPanelX = 202, kEvPanelY = 622, kEvPanelW = 520, kEvPanelH = 84;
constexpr float kEvTrackX = 214, kEvTrackW = 476, kEvTrackY = 664, kEvTrackH = 24;
// 右导轨（滑轨宽 100；整组左移拉开与快门的距离：SS 滑轨右缘 1380 ↔ 快门左缘 1410）。
// 面板宽到设计右缘 1560（=404）：短 24px 会在屏幕右缘露一条 GL 清屏黑缝，
// 比面板底色 #0A0A0C 更黑，看着像遮挡条（2026-10-01 用户报告）。
constexpr float kRailRX = 1156, kRailRW = 404;
constexpr float kIsoTrackX = 1166, kSsTrackX = 1280, kTrackY = 138, kTrackW = 100, kTrackH = 440;
constexpr float kDivX = 1273, kDivY = 260, kDivH = 200;
constexpr float kShutterX = 1410, kShutterY = 318, kShutterD = 106;
constexpr float kAeLockD = 54;                // 测光锁定按钮（快门上方圆形）
constexpr float kAeLockY = 238;               // 圆顶（与快门间距 26）
constexpr float kUiZoom = 1.5f;               // UI 整体放大系数：文字 / 按钮 / 滑块统一放大

// ---- 两个入口按钮（2026-10-03：设置 / 曝光白平衡；2026-10-04 改图标）----
// 均为**纯图标**圆形按钮（无文字），直径 kIconD，靠画几何图形表意：
//   设置 = 齿轮；曝光白平衡 = 半黑半白圆（曝光/白平衡的通用符号）。
// 尺寸与位置（2026-10-04 用户要求）：
//   设置：移到**左侧变焦导轨正上方**（导轨 x 88..168，顶 y=0），圆心 x 与导轨中线对齐；
//         不再压在预览画面上（原先落在预览内 x190，被背景干扰且挡画面）。
//   曝光白平衡：右导轨顶部、快门正上方，圆形，直径与设置一致（原 246×54 太大）。
constexpr float kIconD = 60;                                    // 图标按钮直径
// 设置：左侧变焦导轨顶部（轨道 kZoomTrackY=130 / 读数行 kReadoutY=100 之上），
// 圆心 x=128 与导轨中线 (88+168)/2 对齐；cy=48 使图标占 y 18..78，与读数行留 22px。
constexpr float kSetIconX = 128, kSetIconY = 48;
// 曝光白平衡 / 闪光灯（2026-10-04 用户要求改位）：**分别放到右侧两条滑轨的正上方**，
// 圆心 x 与各自滑轨中线对齐 ——
//   闪光灯 → ISO 滑轨（kIsoTrackX=1166 宽 100 → 中线 1216）
//   曝光白平衡 → SS 滑轨（kSsTrackX=1280 宽 100 → 中线 1330）
// 原位于快门上方同排（x1376/1456）挤在导轨偏右且与快门区视觉粘连，改到滑轨上方后
// 「哪个按钮管哪条滑轨」一眼可读，也更靠近它调节的对象。
// cy=48 使图标占 y 18..78：在滑轨读数行（kReadoutY=100）之上留 22px，
// 与左侧设置图标（kSetIconY=48）同排——顶部三个入口视觉基线一致。
constexpr float kExpIconX = 1330, kExpIconY = 48;
constexpr float kFlashIconX = 1216, kFlashIconY = 48;
// 命中热区在图标外扩 8px（与快门/AE 锁同口径：视觉边缘点按常差几像素落空）
constexpr float kIconHitPad = 8.f;
// 闪光灯档位名（settings.txt 落盘用；解析时同时接受数字 0..3，便于手写调试）
constexpr const char* kFlashNames[4] = {"off", "auto", "on", "torch"};

// ---- 快速整数倍变焦按钮（2026-10-03 用户要求）----
// 竖排 4 枚圆形按钮，铺在**预览区内**、左变焦导轨右缘（kRailREnd=168）之外 ——
// 即"变焦的右侧、预览区域里"。点按直接落到指定倍率，不必拖导轨。
// 纵向占位 180..486：避开顶部 HUD 角标行（kHudY=16..42）与底部 EV 面板
// （kEvPanelY=622..706，且其左缘 202 与本列 x 区间重叠），居中于预览高度。
// 排列自上而下为 5 / 2 / 1 / 0.7（倍率递减，与左导轨同向，见 kQzVals 注释）。
constexpr float kQzX = 234, kQzD = 60, kQzY0 = 180, kQzGap = 22;


// ---- 设置 / 曝光白平衡 面板（2026-10-04 改全屏）----
// 改全屏的动因：原 760×~600 小盒里控件只有 336 宽，8 段白平衡预设每段仅 42px，
// 文字挤在一起（用户反馈"内容太紧凑"）。全屏后控件宽 720，每段 90px。
// **关闭按钮复用入口图标位**：设置面板在左上、曝光面板在右上 —— 入口在哪，
// 关闭就在哪，肌肉记忆一致。面板全屏后没有"外部"可点，故不再有"点外部关闭"。
constexpr float kPanelPadL  = 150;    // 行标签左缘
constexpr float kPanelCtlR  = 1410;   // 分段控件右缘
constexpr float kPanelCtlW  = 720;    // 分段控件宽（右对齐到 kPanelCtlR）
constexpr float kPanelCtlH  = 48;     // 控件高
constexpr float kPanelHeadH = 44;     // 分组标题行高
constexpr float kPanelRowH  = 64;     // 普通行高（控件上下各留 8px）
constexpr float kPanelTopY  = 104;    // 内容区顶（让开标题带）
constexpr float kPanelBotY  = 690;    // 内容区底


// 白平衡预设标签（下标 → 显示名）；下标→Android AWB_MODE 枚举值的映射在
// CameraEngine::drainUiCmds 的 kEnum[8] 中（避免 UI 层依赖 camera2 枚举细节）
static const char* kAwbLabels[8] = {"自动", "日光", "阴天", "白炽", "荧光", "暖荧", "暮光", "阴影"};
// 快速整数倍变焦按钮（0.7 / 1 / 2 / 5 —— 用户指定这四档）
// **自上而下按倍率递减**：5 → 2 → 1 → 0.7，与左侧变焦导轨方向一致
// （导轨是长焦在上、专业相机惯例；2026-10-04 用户指出圆钮列方向反了）。
// 绘制与命中（quickZoomHit）都按 i 递增遍历，故只需倒序这一处数组。
static const float kQzVals[4] = {5.0f, 2.0f, 1.0f, 0.7f};
static const char* kQzLabels[4] = {"5", "2", "1", "0.7"};
static const char* kFmtOpts[2] = {"RAW", "JPG"};
static const char* kOnOff[2] = {"关", "开"};
static const char* kRingOpts[2] = {"环形", "单次"};
static const char* kQuotaOpts[4] = {"1", "4", "8", "不限"};
static const int kQuotaVals[4] = {1, 4, 8, 0};
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
    // 文字边缘锐利；且与 kUiZoom 解耦，后续再调大字号也不糊。候选字体**来自机型层**
    //（不同 ROM 的中文字体名/格式差异巨大，无法探测，只能列举），运行时选 CJK 覆盖率最高者。
    if (!gl_.bakeFont(56.f, optic::device::currentDevice().fontCandidates()))
        LOGE("font bake failed (text disabled)");

    // 预览源默认尺寸来自机型层（必须落在本机 preview-size 列表内，否则 HAL 拒绝会话）；
    // controls.txt 的 preview_w/preview_h 仍可覆盖（诊断用）。
    const auto& pol = optic::device::currentDevice().sessionPolicy();
    int pw = pol.previewW, ph = pol.previewH;
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
    if (pw <= 0 || ph <= 0) { pw = pol.previewW > 0 ? pol.previewW : 1920; ph = pol.previewH > 0 ? pol.previewH : 1440; }
    // 预览源数量来自机型层（三摄 = 逻辑 + uw + tele）：AImageReader 创建无 GL 依赖。
    // 源数少于机型实际/降级模式下多余槽位空闲（未挂会话不出帧，无开销）。
    previewSlots_ = std::clamp(pol.previewSlots, 1, gl_.maxSources());
    // 格式来自机型层（多数 HAL 只有 PRIVATE 零拷贝路径稳定出帧）；副摄（uw/tele，slot≥1）
    // 预览尺寸可被机型层降到 auxPreviewW/H —— 非显示带降分辨率省 ISP/GPU 带宽与发热，
    // 显示带（slot 0 逻辑 / 切到的物理带）仍满分辨率。aux=0 表示副摄同主摄（pandora 即如此）。
    const int auxW = pol.auxPreviewW > 0 ? pol.auxPreviewW : pw;
    const int auxH = pol.auxPreviewH > 0 ? pol.auxPreviewH : ph;
    for (int i = 0; i < previewSlots_; ++i) {
        const int w = i == 0 ? pw : auxW;
        const int h = i == 0 ? ph : auxH;
        gl_.makePreviewSource(i, w, h, pol.previewFormat);
    }

    // cover：等比铺满。横向溢出优先裁掉左侧摄像头避让区（80 设计 px 的留白），
    // 剩余再两侧均分——这样无论系统为挖孔保留多宽，快门都不会被裁；纵向溢出两侧均分。
    scale_ = std::max(float(gl_.width()) / kStageW, float(gl_.height()) / kStageH);
    float overflowX = kStageW * scale_ - float(gl_.width());
    // 容差 1px：全出血时 2656/1560 的浮点乘回 ≈ +0.0002px，会把下面分支误判成
    // 「未全出血」→ railLX_=80，左侧避让区整条露黑（用户所见遮挡条，2026-10-01）。
    // 真实的挖孔保留宽度 ~150px，1px 容差不会误入 else。
    if (overflowX > 1.f) {
        float left = std::min(overflowX, safeW_ * scale_);
        offX_ = -(left + (overflowX - left) / 2.f);
        // 有横向溢出（系统仍保留挖孔条，窗口未全出血）：面板从裁切线起，
        // 可见区即完整面板；设计 x<safeW_ 反正不可见
        railLX_ = safeW_;
    } else {
        offX_ = -overflowX / 2.f;   // 设计窄于窗口：居中留白
        // 全出血（displayCutout inset 已隐藏，窗口铺满）：面板扩进挖孔保留区，
        // 挖孔真身只占设计 x<88 的窄条且落在近纯黑面板上，不可见
        railLX_ = 10.f;
    }
    railLW_ = kRailREnd - railLX_;
    offY_ = -(kStageH * scale_ - float(gl_.height())) / 2.f;
    dirty_ = true;   // 尺寸/布局变化后覆盖层必须重烤
    LOGI("ui attached: win=%dx%d scale=%.3f off=(%.0f,%.0f)", gl_.width(), gl_.height(),
         scale_, offX_, offY_);
    // 电子水平仪 + 持久化设置加载（均在 glue 线程，attach 已完成、dataDir_ 已就绪）
    initLevel();
    loadPersistedSettings();
    return true;
}

void Ui::detach() {
    gl_.detach();
    // 传感器事件队列必须成对销毁：initLevel() 在**每次 attach()** 里 createEventQueue，
    // detach 若不管，前后台/旋转反复切换会每轮泄漏一个队列——加速度计持续回调不仅
    // 泄漏句柄，还会一直唤醒 CPU 白白耗电（2026-10-04 审出）。
    if (snsQ_) {
        if (snsAcc_) ASensorEventQueue_disableSensor(snsQ_, snsAcc_);
        ASensorManager_destroyEventQueue(snsMgr_, snsQ_);
    }
    snsQ_ = nullptr;
    snsAcc_ = nullptr;
    snsMgr_ = nullptr;
}

void Ui::setExpAuto(bool isoAuto, bool ssAuto, int iso, int64_t expNs) {
    isoAuto_ = isoAuto;
    ssAuto_ = ssAuto;
    // 手动侧：把滚轮指针落到实际生效档位（否则关自动曝光后指针还停在旧下标，
    // 与真正下发的 ISO/SS 差好几档 —— 读数行显示值和实际拍出来的不一致）
    if (!isoAuto && iso > 0) isoIdx_ = nearestStopIdx(kIsoStops, kIsoStopsN, float(iso));
    if (!ssAuto && expNs > 0)
        ssIdx_ = nearestStopIdx(kSsStops, kSsStopsN, float(double(expNs) * 1e-9));
    // 自动侧的读数来源是 autoIso_/autoSsUs_，手动后不能再拿它俩当显示值
    setAutoIso(iso);
    setAutoSsUs(expNs / 1000);
    markDirty();
}

// ---- 命令队列（glue 线程生产，引擎线程消费）----

void Ui::pushCmd(Cmd::Type t, float v) {
    pushCmd(t, v, 0.f);
}

void Ui::pushCmd(Cmd::Type t, float v, float v2) {
    {
        std::lock_guard<std::mutex> l(cmdM_);
        if (cmds_.size() < 64) cmds_.push_back({t, v, v2});   // 拖拽节流：积压时丢弃最旧
    }
    // 出锁后再通知（避免持有 cmdM_ 时回调进引擎、与 popCmd 抢锁）
    if (cmdNotify_) cmdNotify_();
}

void Ui::pushCmd(Cmd::Type t, float v, float v2, float v3) {
    {
        std::lock_guard<std::mutex> l(cmdM_);
        if (cmds_.size() < 64) {
            Cmd c;
            c.type = t;
            c.v = v;
            c.v2 = v2;
            c.v3 = v3;
            cmds_.push_back(c);
        }
    }
    if (cmdNotify_) cmdNotify_();
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
    // 多指时整体忽略（含后续 move）：双指捏合的第一根手指动作会被当成一次点按，
    // 在预览区里就是莫名其妙的多余对焦。当前 UI 没有多指手势，直接放弃整次多指输入，
    // 并中断进行中的拖拽（避免第二根手指按下后前一根继续拖值）。
    if (AMotionEvent_getPointerCount(e) != 1) {
        drag_ = Drag::NONE;
        shutterDown_ = false;
        return;
    }
    float x = toDesignX(AMotionEvent_getX(e, 0));
    float y = toDesignY(AMotionEvent_getY(e, 0));
    switch (action) {
        case AMOTION_EVENT_ACTION_DOWN:
            onDown(x, y, double(AMotionEvent_getEventTime(e)) / 1e6); break;
        case AMOTION_EVENT_ACTION_MOVE: onMove(x, y); break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL: onUp(x, y); break;
        default: break;
    }
}

void Ui::onDown(float x, float y, double tMs) {
    // 休眠态：任意触摸只发 WAKE 唤醒命令，不执行任何操作（避免唤醒瞬间误触发快门/对焦）。
    // 引擎收到 WAKE 即重发 repeating 恢复预览，并把后续触摸照常处理。
    if (sleeping_) {
        pushCmd(Cmd::WAKE, 1.f);
        hap_.tick();
        return;
    }
    markDirty();     // 任何触摸都改变覆盖层（格式/快门/AE 锁/拖拽起点），重烤
    hapStop_ = -1;   // 新触摸重置落档触感跟踪
    // 面板打开：所有触摸由面板消费（命中关闭钮→关面板；命中控件→执行动作）。
    // 面板已是全屏，没有「面板外」可点，落在空白处一律忽略（不再有点击外部关闭）。
    // 必须在其它控件命中之前短路，避免面板后面藏着的快门/对焦等被误触。
    if (panel_ != Panel::NONE) {
        handlePanelTap(x, y);
        return;
    }
    // 两个入口图标按钮（无面板时可见可点）。圆形命中 = 圆心距 ≤ 半径+外扩，
    // 与快速变焦圆钮 quickZoomHit 同一口径（比方框判定更贴合圆形视觉）。
    if (std::hypot(x - kSetIconX, y - kSetIconY) <= kIconD / 2 + kIconHitPad) {
        openPanel(Panel::SETTINGS); hap_.click(); return;
    }
    if (std::hypot(x - kExpIconX, y - kExpIconY) <= kIconD / 2 + kIconHitPad) {
        openPanel(Panel::EXPOSURE); hap_.click(); return;
    }
    // 闪光灯：点按循环 关 → 自动 → 开 → 常亮 → 关（档位直接由图标符号表达，
    // 不需要进面板；相机类应用的闪光灯都是这种单击切换）。
    if (flashAvail_ &&
        std::hypot(x - kFlashIconX, y - kFlashIconY) <= kIconD / 2 + kIconHitPad) {
        applyFlash((flashMode_ + 1) % 4);
        hap_.click();
        return;
    }
    // RAW/JPG 格式角标（预览左上第一枚）：点按切换拍摄格式（引擎重建会话 ~300ms）。
    // 热区存设计坐标（onDown 的 x/y 已是 toDesign 反变换后的设计值），上下各放宽 6px 好按
    if (chipFmtL_ > 0 && x >= chipFmtL_ && x <= chipFmtR_ &&
        y >= kHudY - 6 && y <= kHudY + kChipH + 6) {
        // 必须走 applyFmt()，不能直接改成员 + pushCmd：面板路径有幂等守卫与
        // commitPersist，角标若绕开，开着持久化时从角标切的格式重启就丢（2026-10-04 审出）。
        applyFmt(!fmtJpg_);
        hap_.click();
        return;
    }
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
        const float bcy = kJogBtnY + kJogBtnD / 2;
        if (std::hypot(x - bcx, y - bcy) <= kJogBtnD / 2 + 8) {
            const bool toAuto = !(s == 0 ? isoAuto_ : ssAuto_);
            if (s == 0) isoAuto_ = toAuto; else ssAuto_ = toAuto;
            pushCmd(s == 0 ? Cmd::SET_ISO_AUTO : Cmd::SET_SS_AUTO, toAuto ? 1.f : 0.f);
            // 转手动时同步落位滚轮当前档：settings 里 iso/exp 可能还是 0（从未拖过），
            // 直接下发 0 是无效请求（HAL 拒绝）。以当前显示的自动值（引擎回推的联动
            // 计算值）吸附最近档作为手动起点 —— 值不跳变，无缝转手动。
            if (!toAuto) {
                if (s == 0) {
                    const int aIso = autoIso_.load(std::memory_order_relaxed);
                    if (aIso > 0) isoIdx_ = nearestStopIdx(kIsoStops, kIsoStopsN, float(aIso));
                    pushCmd(Cmd::SET_ISO, float(kIsoStops[isoIdx_]));
                } else {
                    const int64_t us = autoSsUs_.load(std::memory_order_relaxed);
                    if (us > 0)
                        ssIdx_ = nearestStopIdx(kSsStops, kSsStopsN, float(double(us) * 1e-6));
                    pushCmd(Cmd::SET_EXP_US, 1e6f * kSsStops[ssIdx_]);
                }
            }
            hap_.click();
            return;
        }
    }
    // 刻度盘：只记基准，不在按下时改值（否则手还没动值先跳到指针位置）。
    // 处于自动态的滚轮灰显不可拖（值由 AE 驱动，拖动 = 点下方 A 键切手动）。
    if (inR(x, y, trackX_, kZoomTrackY, kZoomTrackW, kZoomTrackH)) {
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
        // 双击归零：350ms 内、同位置（±40 设计 px）的第二击 → EV 复位 0
        if (tMs - lastEvTapMs_ < 350.0 &&
            std::hypot(x - lastEvTapX_, y - lastEvTapY_) < 40.f) {
            lastEvTapMs_ = -1e3;                  // 复位，防三击连触
            if (std::fabs(ev_) > 1e-4f) {         // 已是 0 则不重发命令
                ev_ = 0;
                pushCmd(Cmd::SET_EV, 0.f);
                LOGI("ev double-tap -> 0");
            }
            hap_.click();
            return;
        }
        lastEvTapMs_ = tMs;
        lastEvTapX_ = x;
        lastEvTapY_ = y;
        LOGI("ev drag: down design=(%.0f,%.0f) ev=%.2f", x, y, ev_);
        drag_ = Drag::EV; dragRefPos_ = x; dragRefF_ = (ev_ - evMinEv_) / (evMaxEv_ - evMinEv_);
        return;
    }
    // mm/× 单位切换（滚轮下方圆形点按按钮）
    {
        const float ux = trackX_ + kZoomTrackW / 2, uy = kJogBtnY + kJogBtnD / 2;
        if (std::hypot(x - ux, y - uy) <= kJogBtnD / 2 + 8) {
            zoomUnit_ ^= 1;   // mm / × 切换
            hap_.click();
            return;
        }
    }
    // 直方图（预览右上）：它是唯一铺设在预览区里的大块非交互 UI，点上去既会在直方
    // 图上盖一个对焦框（遮挡读数），也没有任何测光意义 —— 与模式按钮一样做热区排除。
    {
        const float hxL = kPreviewX + kPreviewW - 16 - kHistW;
        if (x >= hxL - 8 && x <= hxL + kHistW + 8 && y >= kHistY - 8 && y <= kHistY + kHistH + 8)
            return;
    }
    // 点按模式按钮（预览右上角）：点击即对焦 → 仅选位置 → 对焦并拍照，循环。
    // 必须在预览区判定之前命中（否则会被当成一次对焦点按）。热区四周放宽（按钮窄，
    // 按视觉边缘点经常差几像素落空 —— 2026-10-02 真机：视觉 1028 起，点 1023 落空）。
    if (tapModeR_ > tapModeL_ && x >= tapModeL_ - 10 && x <= tapModeR_ + 10 &&
        y >= tapModeT_ - 8 && y <= tapModeB_ + 8) {
        tapMode_ = TapMode((int(tapMode_) + 1) % 3);
        // 切模式不清除已选框：仅选位置定下的框恒常驻（统计区域仍在生效），
        // 其他模式的动画框走自己的时长自然收尾。
        shotAfterFocus_.store(false, std::memory_order_release);
        shotAfterFocusT0_.store(-1, std::memory_order_release);
        LOGI("tap mode -> %d", int(tapMode_));
        hap_.click();
        markDirty();
        return;
    }
    // 快速整数倍变焦（预览区内、变焦导轨右侧竖排圆钮）：必须在预览区触摸对焦
    // 判定之前短路，否则点按钮会在预览区盖一个对焦框。圆钮是"当前值高亮"，
    // 属静态覆盖层内容，点完靠 onDown 开头已有的 markDirty() 重烤。
    {
        const int qi = quickZoomHit(x, y);
        if (qi >= 0) {
            applyQuickZoom(qi);
            return;
        }
    }
    // 触摸对焦候选：落在预览区且上方所有控件都未命中。**按下即刻触发**（不等抬手）——
    // 抬手才处理会白吃一整个按压时长（手指抬起 80~150ms），用户直接读成"点了没反应"。
    // 预览区本身没有任何拖拽手势（变焦/ISO/SS/EV/快门都在各自热区且已 return），
    // 从预览区下滑不会引发别的操作，故不存在误触发。
    if (inR(x, y, kPreviewX, kPreviewY, kPreviewW, kPreviewH)) {
        // 双击 = 取消指定点：区域一粘到底就再也回不到默认评价测光（审查 P2-6）。
        // 第一击照常对焦（即时反馈优先），第二击整组清空 AF/AE 区域并撤掉常驻框。
        if (tMs - lastPreviewTapMs_ < 350.0 &&
            std::hypot(x - lastPreviewTapX_, y - lastPreviewTapY_) < 40.f) {
            lastPreviewTapMs_ = -1e3;   // 复位，防三击连触
            clearTapFocus();
            return;
        }
        lastPreviewTapMs_ = tMs;
        lastPreviewTapX_ = x;
        lastPreviewTapY_ = y;
        tapDownX_ = x;
        tapDownY_ = y;
        fireTapFocus(x, y);
    }
}

// 清除触摸区域：回默认（全画面评价测光 + 连续追焦），UI 上连常驻框一起撤掉。
void Ui::clearTapFocus() {
    LOGI("tap: double tap -> clear ROI (default metering)");
    pushCmd(Cmd::TAP_FOCUS, 0.f, 0.f, 3.f);   // mode 3 = 清区域（见 CameraEngine）
    afBoxT0_ = -1;
    afBoxSticky_ = false;
    afFocusedAt_ = -1;
    shotAfterFocus_.store(false, std::memory_order_release);
    shotAfterFocusT0_.store(-1, std::memory_order_release);
    hap_.click();
}

// AF 状态回显（引擎每帧回传）：合焦时若处于"对焦并拍照"模式，立刻按快门。
// roiLive = false 表示新 ROI 还没被 HAL 回显采用，此时的"合焦"是旧区域的结论，
// 不能据此触发快门（FocusShot 会拍在错误的对焦区上，审查 P2-8）。
// shotAfterFocus_ 是两个线程共写的：exchange 保证只可能有一个线程走到 pushCmd。
void Ui::setAfState(int s, bool roiLive) {
    afState_.store(s, std::memory_order_relaxed);
    if (!roiLive) return;
    if (s != 2 && s != 4) return;                   // PASSIVE_FOCUSED / FOCUSED_LOCKED
    if (!shotAfterFocus_.exchange(false, std::memory_order_acq_rel)) return;
    shotAfterFocusT0_.store(-1, std::memory_order_release);
    LOGI("tap mode: focused -> shutter");
    pushCmd(Cmd::SHOT, 0.f);
}

// 触摸对焦落地下发：位置换算成预览区占比，同时起对焦框动画 + 轻触感。
// 语义随 tapMode_ 变化（见 TapMode）：仅选位置不重扫、对焦并拍照等合焦后自动拍摄。
void Ui::fireTapFocus(float x, float y) {
    const float fx = std::clamp((x - kPreviewX) / kPreviewW, 0.f, 1.f);
    const float fy = std::clamp((y - kPreviewY) / kPreviewH, 0.f, 1.f);
    LOGI("tap: down fx=%.2f fy=%.2f -> TAP_FOCUS (mode=%d)", fx, fy, int(tapMode_));
    pushCmd(Cmd::TAP_FOCUS, fx, fy, float(int(tapMode_)));
    afBoxX_ = x;
    afBoxY_ = y;
    afBoxT0_ = nowSec();
    afFocusedAt_ = -1;   // 新一轮对焦：重新等待合焦回显
    shotAfterFocus_.store(tapMode_ == TapMode::FocusShot, std::memory_order_release);
    shotAfterFocusT0_.store(tapMode_ == TapMode::FocusShot ? nowSec() : -1.,
                            std::memory_order_release);
    afBoxSticky_ = (tapMode_ == TapMode::LockPos);   // 仅选位置：白框常驻不消失
    hap_.tick();
    // 宽高比断言（低频）：整套坐标换算（fx/fy → active array）假设预览流铺满预览矩形
    // 且两者宽高比一致（现均 4:3）。机型层改分辨率或改预览矩形后会静默错位 —— 点按位置
    // 整体偏移且日志看不出，故留一条低频提示。
    {
        const int pw = gl_.previewW(0), ph = gl_.previewH(0);
        if (pw > 0 && ph > 0) {
            const float src = float(pw) / float(ph);
            const float dst = kPreviewW / kPreviewH;
            static int warns = 0;
            if (std::fabs(src - dst) > 0.02f && warns++ < 3)
                LOGW("tap coords: preview %dx%d (%.3f) vs rect %.3f - aspect mismatch!",
                     pw, ph, src, dst);
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

// 运行时有效刻度表：首项 = 运行时下限（轨道底），其后取机型关键焦段中高于下限者。
// 返回项数；各刻度在轨道上均分（等距刻度，用户指定 0.7-1-2-5-10-50-120 同距）。
int Ui::zoomStopsEff(float out[8]) const {
    int n = 0;
    out[n++] = zoomMin_;
    for (float s : zoomProf_.stops)
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
    return std::clamp(v, zoomMin_, zoomMax_);
}

// 变焦值 → 轨道连续位置（zoomFromF 的严格反函数，刻度绘制与 thumb 定位用）
float Ui::zoomToF(float z) const {
    float eff[8];
    const int n = zoomStopsEff(eff);
    const float v = std::clamp(z, zoomMin_, zoomMax_);
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
            // 刻度盘手感，与 ISO/SS 竖滚轮一致：「刻度跟着手指走」——手指向右 = 刻度右移
            // = 中心对准更小的 EV（渲染 f=0 在左、f=1 在右，故取负号）。
            float f = dragRefF_ - (x - dragRefPos_) / (0.92f * kEvTrackW);
            float v = std::clamp(f, 0.f, 1.f) * span + evMinEv_;
            if (std::fabs(v) < evStepEv_ * 0.5f) v = 0;      // 中心吸附（拖动中）
            ev_ = v;
            hapticTickStop(int(std::lround((ev_ - evMinEv_) / evStepEv_)));   // 跨步触感
            break;   // 命令延到 onUp 下发
        }
        default: break;
    }
    markDirty();     // 拖拽中 zoom/iso/ss/ev 实时变化，覆盖层需重烤
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
            // 与 onMove 同号：刻度盘手感（手指向右 = EV 减小），松手吸附步长网格
            float f = dragRefF_ - (x - dragRefPos_) / (0.92f * kEvTrackW);
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
                shotMsgUntil_ = now + 1.5;   // 仅失败（配额用尽）时用于角标提示时长
            }
        }
        shutterDown_ = false;
    }
    // 触摸对焦已在 onDown 即时下发（见 fireTapFocus），此处不再处理 —— 抬手才做会让
    // 用户多等一个按压时长，且对焦框出现得比手指晚（视觉上"卡了一下"）。
    drag_ = Drag::NONE;
    markDirty();     // 松手落位改变 zoom/iso/ss/ev，覆盖层需重烤
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
    // 线宽：设计 1px 太细，在 2656 宽屏 + scale_ 放大后也仅约 1.7 物理 px，
    // 半透明叠加后几乎无对比。取 1.6 设计 px 保底 2 物理 px，双层再各加 0.8 描边。
    const float t = std::max(dim(1.6f), 2.f);
    const float e = std::max(dim(0.9f), 1.f);   // 描边单侧外扩
    std::vector<float> v, edge;
    // 一个轴对齐矩形（两三角形）
    auto quad = [&](std::vector<float>& dst, float rx, float ry, float rw, float rh) {
        const float q[12] = {rx, ry, rx + rw, ry, rx, ry + rh,
                             rx + rw, ry, rx + rw, ry + rh, rx, ry + rh};
        dst.insert(dst.end(), std::begin(q), std::end(q));
    };
    for (int i = 1; i <= 2; ++i) {                       // 竖线：x = w/3, 2w/3
        const float px = x + w * i / 3.f;
        quad(edge, px - t / 2 - e, y, t + 2 * e, h);
        quad(v, px - t / 2, y, t, h);
    }
    for (int i = 1; i <= 2; ++i) {                       // 横线：y = h/3, 2h/3
        const float py = y + h * i / 3.f;
        quad(edge, x, py - t / 2 - e, w, t + 2 * e);
        quad(v, x, py - t / 2, w, t);
    }
    if (!edge.empty()) gl_.triangles(edge.data(), int(edge.size() / 2), kGridEdge);
    gl_.triangles(v.data(), int(v.size() / 2), kGridCore);
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
        float tx; const char* value;
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

    // 去掉静态标题（2026-10-01 用户要求）：三个滚轮上方只留**实时值**，风格统一
    // 自动态滚轮指针跟到实际生效档位（联动显示：EV/手动参数变化时自动侧跟着走）
    const int64_t aSsUs = autoSsUs_.load(std::memory_order_relaxed);
    int isoIdxEff = isoIdx_;
    if (isoAuto_ && aIso > 0) isoIdxEff = nearestStopIdx(kIsoStops, kIsoStopsN, float(aIso));
    int ssIdxEff = ssIdx_;
    if (ssAuto_ && aSsUs > 0)
        ssIdxEff = nearestStopIdx(kSsStops, kSsStopsN, float(double(aSsUs) * 1e-6));
    const VSlider sliders[2] = {
        {kIsoTrackX, isoVal, isoIdxEff, kIsoStopsN, false, kIsoStops, 1, isoAuto_},
        {kSsTrackX, ssVal, ssIdxEff, kSsStopsN, true, kSsStops, 3, ssAuto_},
    };

    for (int s = 0; s < 2; ++s) {
        const VSlider& v = sliders[s];
        float cx = v.tx + kTrackW / 2;

        // 轨道 + 中心确认刻度盘（档位刻度，拖动连续滚动）
        gl_.roundedRect(screenX(v.tx), screenY(kTrackY), dim(kTrackW), dim(kTrackH),
                        dim(20), kTrack, kTrackLine, 1);
        // 渲染位置：拖动中用连续 roll（刻度逐像素滚动），静止时落位到档位
        const float curF =
            (s == 0 ? (drag_ == Drag::ISO ? isoRoll_ : float(isoIdxEff) / (kIsoStopsN - 1))
                    : (drag_ == Drag::SS ? ssRoll_ : float(ssIdxEff) / (kSsStopsN - 1)));
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

        // 实时值（滚轮上方，三滚轮统一字号/基线/配色，见 kReadoutFs）
        const float rfs = kReadoutFs * kUiZoom * scale_;
        float vw = gl_.textWidth(v.value, rfs);
        gl_.text(v.value, screenX(cx) - vw / 2, screenY(kReadoutY), rfs,
                 v.a ? kT3 : kAccent);

        // A 自动按钮（滚轮下方圆形）：自动=强调环+亮字；手动=暗环+暗字
        {
            const float bx = cx - kJogBtnD / 2, by = kJogBtnY;
            gl_.roundedRect(screenX(bx), screenY(by), dim(kJogBtnD), dim(kJogBtnD),
                            dim(kJogBtnD / 2), v.a ? Rgba{kAccent.r, kAccent.g, kAccent.b, 0.22f}
                                                    : Rgba{1, 1, 1, 0.04f},
                            v.a ? kAccent : kLine, 1.5f);
            const float fs = kJogBtnFs * kUiZoom * scale_;
            float aw = gl_.textWidth("A", fs);
            // textCenterTop：按字形实际墨迹居中（ascent 线估算会整体偏上）
            gl_.text("A", screenX(cx) - aw / 2,
                     gl_.textCenterTop("A", fs, screenY(by + kJogBtnD / 2)), fs,
                     v.a ? kT1 : kT3);
        }
    }
}

// 预览区叠加层：网格 / HUD / 直方图 / AF / EV
void Ui::drawPreviewOverlay() {
    const float px = screenX(kPreviewX), py = screenY(kPreviewY);
    const float pw = dim(kPreviewW), ph = dim(kPreviewH);

    drawGrid(px, py, pw, ph);
    if (levelOn_) drawLevel(px, py, pw, ph);
    if (safeFrameOn_) drawSafeFrame(px, py, pw, ph);

    // （"对焦并拍照"的超时兜底已移到 checkShotAfterFocus() —— 静态层不重烤就不执行，
    //  会出现"等合焦"吊死的情况。）

    // 触摸对焦框已移到 drawAfBox()（frame() 里逐帧直画）：常驻框泡在静态覆盖层里会
    // 让离屏缓存彻底失效，每帧重烤 ~120 draw call（审查 P1-3）。

    // HUD chips（RAW|JPG / DNG / 分辨率 / ZSL）。首枚角标是格式切换按钮：点按 RAW↔JPG
    {
        float x = screenX(kPreviewX + 16), y = screenY(kHudY);
        float dx = kPreviewX + 16;   // 同步推进的设计坐标（热区记录用，勿与屏幕 x 混用）
        std::string res = std::to_string(rawW_) + " \xc3\x97 " + std::to_string(rawH_);
        struct Chip { const char* t; Rgba c; };
        const Chip chips[4] = {{fmtJpg_ ? "JPG" : "RAW", fmtJpg_ ? kWhite : kAccent},
                               {"DNG", fmtJpg_ ? kT3 : kWhite},
                               {res.c_str(), kT2},
                               {"ZSL \xe5\xb0\xb1\xe7\xbb\xaa", kT2}};
        for (int i = 0; i < 4; ++i) {
            const auto& c = chips[i];
            float w = gl_.textWidth(c.t, 12 * kUiZoom * scale_) + 2 * dim(kChipPadX);
            gl_.roundedRect(x, y, w, dim(kChipH), dim(6), kChipBg, kNone, 0);
            gl_.text(c.t, x + dim(kChipPadX), y + dim(5), 12 * kUiZoom * scale_, c.c);
            if (i == 0) {
                // 记录格式角标热区（设计坐标；onDown 同在 glue 线程，无竞争）
                chipFmtL_ = dx;
                chipFmtR_ = dx + w / scale_;
            }
            x += w + dim(8);
            dx += w / scale_ + 8;
        }

        // 状态角标（2026-10-01 用户要求）：取代原先浮在预览下方的两个 toast。
        // 「保存中」常驻到写入完成为止（SAVING 即消失）；只有失败才短暂提示一句，
        // 且文案按**真实拒绝原因**显示（此前一律误报"配额已满"，物理带拒拍时误导）。
        const int inFlight = saveInFlight_.load(std::memory_order_acquire);
        const char* st = nullptr;
        Rgba sc = kAccent;
        char quota[48];
        if (inFlight > 0) {
            st = "SAVING";
        } else if (nowSec() < shotMsgUntil_ &&
                   shotOk_.load(std::memory_order_acquire) == 0) {
            switch (shotReason_.load(std::memory_order_acquire)) {
                case kShotQuota:
                    snprintf(quota, sizeof(quota),
                             "\xe9\x85\x8d\xe9\xa2\x9d\xe5\xb7\xb2\xe6\xbb\xa1 %d/%d",
                             shotUsed_.load(std::memory_order_acquire),
                             shotTotal_.load(std::memory_order_acquire));
                    break;
                case kShotPhysBand:
                    // DNG 模式下超广角直连/长焦带：物理流无对应 RAW 流（诚实拒拍）
                    snprintf(quota, sizeof(quota),
                             "\xe6\xad\xa4\xe7\x84\xa6\xe6\xae\xb5\xe4\xb8\x8d\xe6\x94\xaf\xe6\x8c\x81 RAW");
                    break;
                case kShotNoRawRing:
                    snprintf(quota, sizeof(quota),
                             "RAW \xe6\x9c\xaa\xe5\xb0\xb1\xe7\xbb\xaa");
                    break;
                case kShotNoSession:
                    // 启动中/跨带重建 ~300ms 窗口内按了快门
                    snprintf(quota, sizeof(quota),
                             "\xe7\x9b\xb8\xe6\x9c\xba\xe6\x9c\xaa\xe5\xb0\xb1\xe7\xbb\xaa");
                    break;
                default:
                    snprintf(quota, sizeof(quota),
                             "\xe6\x8b\x8d\xe6\x91\x84\xe5\xa4\xb1\xe8\xb4\xa5");
                    break;
            }
            st = quota;
        }
        if (st) {
            float sw = gl_.textWidth(st, 12 * kUiZoom * scale_) + 2 * dim(kChipPadX);
            gl_.roundedRect(x, y, sw, dim(kChipH), dim(6), kChipBg, kNone, 0);
            gl_.text(st, x + dim(kChipPadX), y + dim(5), 12 * kUiZoom * scale_, sc);
            x += sw + dim(8);
        }
        // 常显配额小角标「已拍 n/N」：快门次数（DNG 一次快门=4 帧，此处计次数）。
        // N<=0 不显示；每次快门（含被拒）经 notifyShot/setShotQuota 刷新。
        {
            const int used = shotUsed_.load(std::memory_order_acquire);
            const int total = shotTotal_.load(std::memory_order_acquire);
            if (total > 0) {
                snprintf(quota, sizeof(quota),
                         "\xe5\xb7\xb2\xe6\x8b\x8d %d/%d", used, total);
                const float w = gl_.textWidth(quota, 12 * kUiZoom * scale_) +
                                2 * dim(kChipPadX);
                gl_.roundedRect(x, y, w, dim(kChipH), dim(6), kChipBg, kNone, 0);
                gl_.text(quota, x + dim(kChipPadX), y + dim(5),
                         12 * kUiZoom * scale_, kT3);
            }
        }
    }

    // 预览帧率 + CPU/GPU 监控（芯片行下方，跟芯片文字同字号）。
    // SysMon 通用 sysfs 探测（机型无关）：读不到的项显示 "--"。
    if (fpsValue_ > 0.f) {
        char ftxt[16];
        snprintf(ftxt, sizeof(ftxt), "%.0f FPS", fpsValue_);
        const float fs = 12 * kUiZoom * scale_;
        const float fx = screenX(kPreviewX + 16);
        const float fy = screenY(kHudY + kChipH + 8);
        gl_.text(ftxt, fx, fy, fs, kT2);

        const auto& sm = sys_.last();
        char cF[10], cT[10], gF[10], gT[10], gB[10];
        if (sm.cpuMaxMHz > 0.f) {
            snprintf(cF, sizeof(cF), "%.2fG", sm.cpuMaxMHz / 1000.f);
        } else {
            snprintf(cF, sizeof(cF), "--");
        }
        if (sm.cpuTempC >= 0.f) {
            snprintf(cT, sizeof(cT), "%.0fC", sm.cpuTempC);
        } else {
            snprintf(cT, sizeof(cT), "--");
        }
        if (sm.gpuMHz > 0.f) {
            snprintf(gF, sizeof(gF), "%.2fG", sm.gpuMHz / 1000.f);
        } else {
            snprintf(gF, sizeof(gF), "--");
        }
        if (sm.gpuTempC >= 0.f) {
            snprintf(gT, sizeof(gT), "%.0fC", sm.gpuTempC);
        } else {
            snprintf(gT, sizeof(gT), "--");
        }
        if (sm.gpuBusyPct >= 0.f) {
            snprintf(gB, sizeof(gB), "%.0f%%", sm.gpuBusyPct);
        } else {
            snprintf(gB, sizeof(gB), "--");
        }
        char ptxt[96];
        snprintf(ptxt, sizeof(ptxt), "CPU %s %s  GPU %s %s %s", cF, cT, gF, gT, gB);
        gl_.text(ptxt, fx + gl_.textWidth(ftxt, fs) + dim(14), fy, fs, kT2);
    }

    // 时钟 + 电池（真实电量：Battery JNI 轮询，30s 刷新；失败保持上次值）
    {
        const double nowS = nowSec();
        if (nowS >= battNextT_) {
            bool chg = false;
            int p = batt_.query(&chg);
            if (p >= 0) {
                battPct_ = p;
                battCharging_ = chg;
            }
            battNextT_ = nowS + 30.0;
        }

        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_r(&t, &tm);
        char clk[8];
        snprintf(clk, sizeof(clk), "%02d:%02d", tm.tm_hour, tm.tm_min);
        float cw = gl_.textWidth(clk, 13 * kUiZoom * scale_);
        float right = screenX(kPreviewX + kPreviewW - 16);
        gl_.text(clk, right - cw - dim(34), screenY(kHudY + 4), 13 * kUiZoom * scale_, kT1);

        // 填充按真实电量比例；充电 = 绿、低电(≤20%) = 重点色、正常 = 白
        float bx = right - dim(24), by = screenY(kHudY + 2);
        gl_.roundedRect(bx, by, dim(24), dim(14), dim(3), kNone, {1, 1, 1, 0.8f}, dim(1.5f));
        Rgba fill = battCharging_ ? Rgba{74 / 255.f, 222 / 255.f, 128 / 255.f, 1}
                    : (battPct_ >= 0 && battPct_ <= 20)
                        ? kAccent
                        : Rgba{1, 1, 1, 1};
        float fw = dim(20) * (battPct_ < 0 ? 0.f : std::clamp(battPct_ / 100.f, 0.04f, 1.f));
        gl_.roundedRect(bx + dim(2), by + dim(2), fw, dim(10), dim(1.5f), fill, kNone, 0);
        // 电池正极帽
        gl_.roundedRect(bx + dim(24) + dim(1), by + dim(3), dim(2), dim(8), 0,
                        {1, 1, 1, 0.8f}, kNone, 0);
    }

    // 直方图
    float hx = screenX(kPreviewX + kPreviewW - 16 - kHistW);
    gl_.roundedRect(hx, screenY(kHistY), dim(kHistW), dim(kHistH), dim(6),
                    {0, 0, 0, 0.42f}, kNone, 0);
    drawHistogram(hx, screenY(kHistY), dim(kHistW), dim(kHistH));

    // 点按模式按钮（右上角，直方图下方）：三态循环 —— 点击即对焦 / 仅选位置 / 对焦并拍照。
    // 热区按设计坐标记录（onDown 与绘制同在 glue 线程，无竞争）。
    {
        const char* txt = tapMode_ == TapMode::Focus
                              ? "\xe7\x82\xb9\xe5\x87\xbb\xe5\x8d\xb3\xe5\xaf\xb9\xe7\x84\xa6"
                              : (tapMode_ == TapMode::LockPos
                                     ? "\xe4\xbb\x85\xe9\x80\x89\xe4\xbd\x8d\xe7\xbd\xae"
                                     : "\xe5\xaf\xb9\xe7\x84\xa6\xe5\xb9\xb6\xe6\x8b\x8d\xe7\x85\xa7");
        const float fs = 12 * kUiZoom * scale_;
        const float tw = gl_.textWidth(txt, fs);
        // 最小宽度 100 设计 px：三态文字长短不一（4/5/5 字），固定宽度让热区不随
        // 模式漂移 —— 否则"仅选位置"态按钮变窄，用户按惯性点旧位置会落空变成对焦点按。
        const float wDes = std::max(tw / scale_ + 2 * kChipPadX, 100.f);
        const float w = dim(wDes);
        const float x = screenX(kPreviewX + kPreviewW - 16 - wDes);
        const float y = screenY(kTapModeY);
        // 非默认态用强调色描边（一眼看出"当前点按会拍照"这类副作用）
        const bool hi = (tapMode_ != TapMode::Focus);
        gl_.roundedRect(x, y, w, dim(kTapModeH), dim(8), kChipBg,
                        hi ? kAccent : Rgba{1, 1, 1, 0.14f}, hi ? dim(1.4f) : 1.f);
        // textCenterTop 只返回行顶（不绘制），再交给 text 落笔：按字形墨迹精确居中
        const float yTop = gl_.textCenterTop(txt, fs, y + dim(kTapModeH) / 2);
        gl_.text(txt, x + (w - tw) / 2, yTop, fs, hi ? kAccent : kWhite);
        tapModeL_ = kPreviewX + kPreviewW - 16 - wDes;
        tapModeR_ = tapModeL_ + wDes;
        tapModeT_ = kTapModeY;
        tapModeB_ = kTapModeY + kTapModeH;
        static bool hotLogged = false;   // 一次性：核对热区与视觉是否一致（命中异常时排查）
        if (!hotLogged) {
            hotLogged = true;
            LOGI("tapmode btn hot: L=%.0f R=%.0f T=%.0f B=%.0f (scale=%.3f wDes=%.0f)",
                 tapModeL_, tapModeR_, tapModeT_, tapModeB_, scale_, wDes);
        }
    }

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
    // EV 量程/步长来自设备 traits（setEvRange 下发），此处按值映射绘制刻度。
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

// 触摸对焦框（逐帧直画，主帧缓冲）：点按位置缩放落入（1.4→1.0，ease-out）+ 中心点
//（对焦/测光区域可视化）。颜色随 AF 状态回显（白=仅选位置常驻、橙=扫描中、绿=已合焦、
// 红=合焦失败）—— 用户不看日志也知道点了之后相机有没有真的去合焦。
// 扫描慢于动画时框延长保持到 kAfBoxHold，合焦后留 0.6s 收尾淡出。
void Ui::drawAfBox() {
    if (afBoxT0_ < 0) return;
    const double now = nowSec();
    const double age = now - afBoxT0_;
    const int st = afState_.load(std::memory_order_relaxed);
    const bool focused = (st == 2 || st == 4);   // PASSIVE_FOCUSED / FOCUSED_LOCKED
    if (focused && afFocusedAt_ < 0) afFocusedAt_ = now;
    double dur = kAfBoxDur;
    if (st >= 0 && !focused) dur = kAfBoxHold;
    if (focused && afFocusedAt_ >= 0) dur = std::max(dur, (afFocusedAt_ - afBoxT0_) + 0.6);
    // "仅选位置"选中的框：白色、常驻不淡出 —— 用户要的就是"位置一直标在那"。
    const bool persist = afBoxSticky_;
    if (!persist && age >= dur) {
        afBoxT0_ = -1;   // 动画自然收尾
        return;
    }
    const float in = persist ? 1.f : std::clamp(float(age / 0.18), 0.f, 1.f);
    const float scl = persist ? 1.f : 1.4f - 0.4f * (1.f - (1.f - in) * (1.f - in));
    float a = 1.f;
    if (!persist && age > dur - 0.35) a = float((dur - age) / 0.35);
    Rgba c = kAccent;
    if (persist) {
        c = kWhite;
    } else if (st == 2 || st == 4) {
        c = Rgba{74 / 255.f, 222 / 255.f, 128 / 255.f, 1};
    } else if (st == 5 || st == 6) {
        c = Rgba{1, 77 / 255.f, 64 / 255.f, 1};
    }
    // 边长 = kAfBoxSide（= 下发 metering rectangle 的视觉口径，见 Ui.h kRoiFrac）
    const float side = dim(kAfBoxSide * scl);
    gl_.roundedRect(screenX(afBoxX_) - side / 2, screenY(afBoxY_) - side / 2, side,
                    side, dim(6 * scl), kNone, {c.r, c.g, c.b, 0.9f * a}, dim(1.6f));
    const float d = std::max(dim(2.4f), 1.5f);
    const float cx = screenX(afBoxX_) - d / 2, cy = screenY(afBoxY_) - d / 2;
    gl_.triangles((float[12]){cx, cy, cx + d, cy, cx, cy + d,
                              cx, cy + d, cx + d, cy, cx + d, cy + d},
                  6, {c.r, c.g, c.b, a});
}

// "对焦并拍照"超时兜底：HAL 迟迟不报合焦（低反差/纯色墙面）也不能把快门吊死，
// 等满 kShotAfterFocusMaxS 直接拍 —— 晚拍一张好过完全不拍。
// 每帧检查（原在静态覆盖层里：层不重烤就不执行，CLAUSE 合焦永不到来时快门永久悬挂）。
void Ui::checkShotAfterFocus() {
    if (!shotAfterFocus_.load(std::memory_order_acquire)) return;
    const double t0 = shotAfterFocusT0_.load(std::memory_order_acquire);
    if (t0 < 0 || nowSec() - t0 <= kShotAfterFocusMaxS) return;
    if (!shotAfterFocus_.exchange(false, std::memory_order_acq_rel)) return;   // 别人已接管
    shotAfterFocusT0_.store(-1, std::memory_order_release);
    LOGI("tap mode: focus timeout %.1fs -> shutter anyway", kShotAfterFocusMaxS);
    pushCmd(Cmd::SHOT, 0.f);
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
    pollLevel();      // 电子水平仪：每帧非阻塞取加速度计（无传感器则 roll_ 恒 0）
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

    // 预览帧率：活动 slot 的相机出帧速率，500ms 窗口（预览左上角显示）
    {
        // 拖拽/轨道刻度变化时才有必要重烤静态层；AF 框是逐帧直画（drawAfBox），
        // 不能再让它把覆盖层钉成永远脏 —— 那等于把缓存收益清零（审查 P1-3）。
        const double t = nowSec();
        if (previewActive_ >= 0) {
            const int64_t cnt = slotFrames_[previewActive_].load(std::memory_order_relaxed);
            if (fpsLastT_ == 0) {
                fpsLastT_ = t;
                fpsLastCnt_ = cnt;
            } else if (t - fpsLastT_ >= 0.5) {
                fpsValue_ = float(double(cnt - fpsLastCnt_) / (t - fpsLastT_));
                fpsLastT_ = t;
                fpsLastCnt_ = cnt;
                // CPU/GPU 监控与 fps 同步（2Hz）；置脏让覆盖层把新值烤进去
                sys_.sample();
                markDirty();
            }
        }
    }

    gl_.beginFrame(kBg);

    // ---- 预览（相机 → GL 纹理，主帧缓冲）----
    // 全带统一的 crop 补偿：crop = zoom_（手指目标）/ az（相机实际出图倍率）。
    //  - 逻辑带相机侧 120ms 节流：补差后 FOV 每帧连续，不表现为 8 次/s 阶梯跳。
    //  - **az 必须与显示帧时间戳对齐**（Gl::acquirePreview → 引擎结果环查询）：
    //    显示 FOV = az × crop ≡ zoom_ 恒成立，crop 直接落位、无需任何平滑。
    //  - 物理带（quirk physPerKeyZoom=false）：az = 带基常量，crop 纯由手指驱动。
    //  - az 变化时 crop 直接落位，绝不归一到 1（否则泵动闪烁）。
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
        // 淡化旧层用**自己的 az** 求 crop（跨带切换大闪烁元凶之二）。
        const float azo = az_[fadeFrom_].load(std::memory_order_acquire);
        float fadeCrop = 1.f;
        if (azo > 0.01f) fadeCrop = std::clamp(zoom_ / azo, 1.f, 32.f);
        gl_.drawPreview(int(screenX(kPreviewX)), int(screenY(kPreviewY)), int(dim(kPreviewW)),
                        int(dim(kPreviewH)), resolveUvRot(), fadeFrom_, fadeT_, fadeCrop);
    }

    // ---- 静态覆盖层（轨道/刻度/文字/HUD/直方图框）：离屏缓存 + 逐帧合成 ----
    // 仅「脏」（输入/状态变化）或直方图刷新（~4Hz）时重烤进纹理；稳态每帧只做 1 次
    // 全屏合成（≈2 draw call），把 ~120 次 draw call 从 30Hz 降到 ~4Hz —— glue 线程
    // GPU/发热的主要杠杆（机型无关，常量在 Gl.h 注释）。预览/闪光/跨带淡化不进缓存。
    if (dirty_ || gl_.consumeHistUpdated(histSlot)) {
        if (gl_.beginOverlay()) {
            paintOverlay();
            gl_.endOverlay();
            dirty_ = false;
        }
    }
    gl_.drawOverlayFull();   // 合成缓存覆盖层（透明区透出下方预览）

    // ---- 逐帧动态层（不进缓存）：对焦框 + FocusShot 超时兜底 ----
    checkShotAfterFocus();
    drawAfBox();
    // 设置 / 曝光白平衡 面板（动态层逐帧直画，常开也不摧毁静态层缓存收益）
    if (panel_ != Panel::NONE) drawPanel();

    // ---- 自动休眠遮罩：预览区压暗 + 居中提示（覆盖在预览/AF 框之上）----
    // 相机已停 repeating（见 CameraEngine::sleepCamera），预览帧冻结，只画最后一张 + 提示。
    if (sleeping_) {
        gl_.roundedRect(screenX(kPreviewX), screenY(kPreviewY), dim(kPreviewW), dim(kPreviewH),
                        0, {0, 0, 0, 0.62f}, kNone, 0);
        const float fs = 30 * kUiZoom * scale_;
        const std::string tip = "已休眠，触摸唤醒";
        const float tw = gl_.textWidth(tip, fs);
        const float cx = screenX(kPreviewX + kPreviewW / 2);
        const float cy = screenY(kPreviewY + kPreviewH / 2);
        const float yTop = gl_.textCenterTop(tip, fs, cy);
        gl_.text(tip, cx - tw / 2, yTop, fs, {1, 1, 1, 0.92f});
    }

    // ---- 白闪（最顶层，逐帧绘制）----
    double now = nowSec();
    if (now < flashUntil_) {
        Rgba f = kWhite;
        f.a = 0.85f * float((flashUntil_ - now) / 0.10);
        gl_.roundedRect(screenX(kPreviewX), screenY(kPreviewY), dim(kPreviewW),
                        dim(kPreviewH), 0, f, kNone, 0);
    }
    // 拍照反馈已全部收敛到 HUD 角标行（SAVING / 配额提示），此处不再画浮层 toast
    gl_.swap();
}

void Ui::paintOverlay() {
    drawPreviewOverlay();

    // ---- 左侧：摄像头避让区（真机只留黑，虚线为设计标注不绘制）----
    gl_.roundedRect(screenX(0), screenY(0), dim(safeW_), dim(kStageH), 0, kBg, kNone, 0);

    // ---- 左导轨：变焦（面板位置 attach 自适应，见 railLX_）----
    gl_.roundedRect(screenX(railLX_), screenY(0), dim(railLW_), dim(kStageH), 0, kRail,
                    kLine, 1);
    {
        // 实时焦距 / 倍率：与右侧 ISO、曝光时间共用 kReadoutY / kReadoutFs / 配色
        char buf[16];
        if (zoomUnit_ == 0) snprintf(buf, sizeof(buf), "%dmm", int(std::lround(zoomBaseMm_ * zoom_)));
        else snprintf(buf, sizeof(buf), "%.1f\xc3\x97", zoom_);
        const float rfs = kReadoutFs * kUiZoom * scale_;
        float w = gl_.textWidth(buf, rfs);
        gl_.text(buf, screenX(trackX_ + kZoomTrackW / 2) - w / 2, screenY(kReadoutY), rfs,
                 kAccent);
    }

    // 变焦轨道 + 刻度 + thumb
    gl_.roundedRect(screenX(trackX_), screenY(kZoomTrackY), dim(kZoomTrackW),
                    dim(kZoomTrackH), dim(20), kTrack, kTrackLine, 1);
    // 分隔线偏移按轨道宽等比（原宽 80 时 = 26），否则收窄后会压到刻度标签上
    gl_.roundedRect(screenX(trackX_ + kZoomTrackW * 0.325f), screenY(kZoomTrackY + 20),
                    dim(2), dim(kZoomTrackH - 40), 0, kTrackLine, kNone, 0);
    {
        // 中心确认刻度盘：关键焦段为主刻度（带标签），段间按对数等分插短刻度
        RollItem items[48];
        char labels[16][8];
        int n = 0;
        float prev = -1.f;
        for (size_t i = 0; i < zoomProf_.stops.size(); ++i) {
            const float s = zoomProf_.stops[i];
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
        drawRollerV(screenX(trackX_), screenY(kZoomTrackY), dim(kZoomTrackW),
                    dim(kZoomTrackH), zoomFrac(), items, n);
    }

    // 单位切换（mm / ×）：圆形点按按钮，显示当前单位，点按切换
    {
        const float cx = trackX_ + kZoomTrackW / 2, cy = kJogBtnY + kJogBtnD / 2;
        gl_.roundedRect(screenX(cx - kJogBtnD / 2), screenY(cy - kJogBtnD / 2),
                        dim(kJogBtnD), dim(kJogBtnD), dim(kJogBtnD / 2),
                        {1, 1, 1, 0.06f}, kLine, 1.5f);
        const char* t = zoomUnit_ == 0 ? "mm" : "\xc3\x97";
        const float fs = kJogBtnFs * kUiZoom * scale_;
        const float w = gl_.textWidth(t, fs);
        // textCenterTop：mm（x 高字形）与 ×（数学符号）高度不同，按墨迹精确居中
        gl_.text(t, screenX(cx) - w / 2, gl_.textCenterTop(t, fs, screenY(cy)), fs, kT1);
    }

    // 快速整数倍变焦圆钮（预览区内、导轨右侧）—— 必须画在预览区之后（预览在
    // paintOverlay 开头已画），否则会被预览纹理盖住。
    drawQuickZoom();

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

    // ---- 两个入口按钮：必须画在最后 ----
    // 两侧导轨（railLX_=88 宽 80 / kRailRX=1156 宽 404）都是整块不透明面板，且都在
    // 预览之后绘制；入口按钮若提前画会被导轨整块盖住 —— 表现为「命中区在、按钮看不见」
    // （真机验证踩过：点 150,72 能开面板，但屏幕上找不到按钮）。
    drawSettingsButton();
    drawExposureButton();
    drawFlashButton();
}

// =================== 设置 / 曝光白平衡 面板（2026-10-03）===================

// ---- 快速整数倍变焦（0.7 / 1 / 2 / 5）----
// 圆钮中心 y：kQzY0 + kQzD/2 + i*(kQzD+kQzGap)。命中用「圆 + 8px 外扩」，
// 与快门/AE 锁同一热区口径（视觉边缘点按经常差几像素落空）。
int Ui::quickZoomHit(float x, float y) const {
    for (int i = 0; i < 4; ++i) {
        const float cy = kQzY0 + kQzD / 2 + float(i) * (kQzD + kQzGap);
        if (std::hypot(x - kQzX, y - cy) <= kQzD / 2 + 8) return i;
    }
    return -1;
}

void Ui::applyQuickZoom(int idx) {
    if (idx < 0 || idx >= 4) return;
    const float z = kQzVals[idx];
    // 落在当前导轨量程之外（低端机 uw 原生 > 0.7 等）→ 钳到量程，不下发越界值。
    const float target = std::clamp(z, zoomMin_, zoomMax_);
    zoom_ = target;
    // 绝对语义直推：与拖拽松手（onUp ZOOM）同一条路径，绕开 120ms 实时节流。
    // lastZoomPush_/lastCamPush_ 同步落位，避免紧随其后的拖拽被节流窗口吞掉首帧。
    pushCmd(Cmd::SET_ZOOM, target);
    lastZoomPush_ = nowSec();
    lastCamPush_ = target;
    LOGI("quick zoom tap -> %.2f (target %.2f)", target, z);
    hap_.click();
    markDirty();
}

void Ui::drawQuickZoom() {
    const float fs = kJogBtnFs * kUiZoom * scale_;
    for (int i = 0; i < 4; ++i) {
        const float cy = kQzY0 + kQzD / 2 + float(i) * (kQzD + kQzGap);
        // 当前值高亮：与目标档的相对误差 <12% 即视为命中该档。
        // 用相对判据而非绝对差，0.7 档（带宽窄）与 5 档（带宽宽）手感一致。
        const bool on = std::fabs(zoom_ - kQzVals[i]) / kQzVals[i] < 0.12f;
        gl_.roundedRect(screenX(kQzX - kQzD / 2), screenY(cy - kQzD / 2),
                        dim(kQzD), dim(kQzD), dim(kQzD / 2),
                        on ? Rgba{kAccent.r, kAccent.g, kAccent.b, 0.32f}
                           : Rgba{1, 1, 1, 0.06f},
                        on ? kAccent : kLine, 1.5f);
        const char* t = kQzLabels[i];
        const float w = gl_.textWidth(t, fs);
        gl_.text(t, screenX(kQzX) - w / 2, gl_.textCenterTop(t, fs, screenY(cy)), fs,
                 on ? kAccent : kT1);
    }
}

// ---- 入口图标按钮（2026-10-04）----
// 圆形按钮 + 纯图标几何图形。图标用 Gl::roundedRect（圆环=大圆角矩形）与
// Gl::triangles（半圆）拼出，不依赖字体 —— 中文字形进静态图集，图标不占那预算。
// 约定：本段自由函数只收**已换算好的屏幕像素**（cx/cy/r），不碰 Ui 的私有缩放成员；
// 设计→屏幕的换算由 Ui::drawSettingsButton / drawExposureButton 完成。
namespace {

// 齿轮：外圈圆环 + 8 个齿 + 中心孔。r 为齿轮外半径（屏幕像素）。
void gearIcon(Gl& gl, float cx, float cy, float r, float scale, const Rgba& c) {
    const float bw = 1.8f * scale;               // 描边宽
    const float ring = r * 0.62f;                // 齿根圆半径
    gl.roundedRect(cx - ring, cy - ring, ring * 2, ring * 2, ring, kNone, c, bw);
    // 8 齿：沿 45° 均布的小方块
    for (int i = 0; i < 8; ++i) {
        const float a = float(i) * 3.14159265f / 4.f + 3.14159265f / 8.f;
        const float tx = cx + std::cos(a) * r * 0.80f - r * 0.17f;
        const float ty = cy + std::sin(a) * r * 0.80f - r * 0.17f;
        gl.roundedRect(tx, ty, r * 0.34f, r * 0.34f, r * 0.10f, c, kNone, 0.f);
    }
    // 中心孔（用按钮底色挖空 → 直接画深色圆）
    gl.roundedRect(cx - r * 0.20f, cy - r * 0.20f, r * 0.40f, r * 0.40f, r * 0.20f,
                   {0, 0, 0, 0.9f}, kNone, 0.f);
}

// 曝光/白平衡：上下半黑半白的圆（业界通用符号）。r 为圆半径（屏幕像素）。
void exposureIcon(Gl& gl, float cx, float cy, float r, float scale, const Rgba& c) {
    const float bw = 1.8f * scale;
    gl.roundedRect(cx - r, cy - r, r * 2, r * 2, r, kNone, c, bw);
    // 上半：亮色填充（用小扇形三角形近似半圆，避免依赖圆弧 shader）
    const float R = r - bw * 0.5f;               // 内缩到描边内侧
    const int N = 14;
    float xy[14 * 2 * 3];
    int n = 0;
    for (int i = 0; i < N; ++i) {
        const float a0 = 3.14159265f + float(i) * 3.14159265f / float(N);
        const float a1 = 3.14159265f + float(i + 1) * 3.14159265f / float(N);
        // 每个小扇形一个三角形：(中心, a0, a1)
        const float p[6] = {cx, cy,
                            cx + std::cos(a0) * R, cy + std::sin(a0) * R,
                            cx + std::cos(a1) * R, cy + std::sin(a1) * R};
        std::memcpy(xy + n, p, sizeof(p));
        n += 6;
    }
    gl.triangles(xy, n, c);
}

// 关闭图标：圆底 + 两根 ±45° 交叉条（画笔无法旋转，斜条用细长三角形拼）。
// 面板全屏后没有"面板外"可点，关闭按钮就落在原入口图标位（设置左上 / 曝光右上）。
void closeIcon(Gl& gl, float cx, float cy, float r, float scale, const Rgba& c) {
    const float bw = 1.8f * scale;
    gl.roundedRect(cx - r, cy - r, r * 2, r * 2, r, kNone, c, bw);
    const float L = r * 0.60f;          // 斜条半长
    const float t = r * 0.15f;          // 斜条半宽
    for (int k = 0; k < 2; ++k) {
        const float a = (k ? -1.f : 1.f) * 0.7853982f;   // ±45°
        const float ca = std::cos(a), sa = std::sin(a);
        const float p[12] = {
            cx - ca * L + sa * t, cy - sa * L - ca * t,      // A
            cx + ca * L + sa * t, cy + sa * L - ca * t,      // B
            cx + ca * L - sa * t, cy + sa * L + ca * t,      // C
            cx - ca * L + sa * t, cy - sa * L - ca * t,      // A
            cx + ca * L - sa * t, cy + sa * L + ca * t,      // C
            cx - ca * L - sa * t, cy - sa * L + ca * t,      // D
        };
        gl.triangles(p, 12, c);
    }
}

// 粗线段 → 两个三角形（笔画基元；本文件多个图标共用）。w 为线宽（屏幕像素）。
void strokeSeg(std::vector<float>& v, float x0, float y0, float x1, float y1, float w) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = std::hypot(dx, dy);
    if (len < 1e-3f) return;
    const float nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
    const float p[12] = {
        x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny,
        x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny,
    };
    v.insert(v.end(), std::begin(p), std::end(p));
}

// 闪光灯：Z 形闪电（两段斜笔 + 中间横笔，接缝用小方块缝合）+ 档位附加符号。
// 档位（0 关 / 1 自动 / 2 开 / 3 常亮）靠**附加符号**区分，不靠颜色单独表意
// —— 单靠颜色在强光下读不出，且色弱用户分不开：
//   关 = 闪电被深色斜杠划断；自动 = 右下角 "A"；开 = 纯闪电；常亮 = 闪电 + 四向短芒。
void flashIcon(Gl& gl, float cx, float cy, float r, float scale, int mode, const Rgba& c) {
    const float w = r * 0.34f;                 // 笔画宽
    // Z 形闪电的四个骨架点（y 向上为正，与 roundedRect 同一坐标系）
    const float xTop = cx + 0.16f * r, yTop = cy + 0.55f * r;
    const float xML = cx - 0.20f * r, yMid = cy + 0.02f * r;
    const float xMR = cx + 0.20f * r;
    const float xBot = cx - 0.16f * r, yBot = cy - 0.55f * r;

    std::vector<float> v;
    strokeSeg(v, xTop, yTop, xML, yMid, w);      // 上斜笔
    strokeSeg(v, xML, yMid, xMR, yMid, w);      // 中横笔
    strokeSeg(v, xMR, yMid, xBot, yBot, w);     // 下斜笔
    gl.triangles(v.data(), int(v.size() / 2), c);
    // 接缝补齐：斜笔端点落在横笔中线，笔宽方向垂直 ⇒ 转折处会缺一个小三角。
    // 用同宽方块盖住两个转折点（roundedRect 不能旋转，方块无需旋转）。
    gl.roundedRect(xML - w / 2, yMid - w / 2, w, w, 0, c, kNone, 0);
    gl.roundedRect(xMR - w / 2, yMid - w / 2, w, w, 0, c, kNone, 0);

    if (mode == 0) {
        // 关：一道深色斜杠划断闪电（比"灰掉"更易读，且与自动档的 A 明确区分）
        std::vector<float> s;
        strokeSeg(s, cx - r * 0.80f, cy + r * 0.80f, cx + r * 0.80f, cy - r * 0.80f,
                  w * 0.85f);
        gl.triangles(s.data(), int(s.size() / 2), {0, 0, 0, 0.92f});
    } else if (mode == 1) {
        // 自动：右下角小 "A"（相机界通用：闪电 + A = 自动闪光）
        const float fs = r * 0.78f;
        const std::string t = "A";
        const float tw = gl.textWidth(t, fs);
        gl.text(t, cx + r * 0.42f - tw / 2, cy - r * 0.92f, fs, c);
    } else if (mode == 3) {
        // 常亮：四向短芒（手电筒持续发光的表意符号）
        std::vector<float> s;
        const float r0 = r * 0.72f, r1 = r * 1.00f;
        for (int i = 0; i < 4; ++i) {
            const float a = 0.7853982f + float(i) * 1.5707963f;   // 45°/135°/225°/315°
            const float ca = std::cos(a), sa = std::sin(a);
            strokeSeg(s, cx + ca * r0, cy + sa * r0, cx + ca * r1, cy + sa * r1, w * 0.42f);
        }
        gl.triangles(s.data(), int(s.size() / 2), c);
    }
    (void)scale;
}

}  // namespace

void Ui::drawFlashButton() {
    if (!flashAvail_) return;      // 无闪光灯单元：干脆不显示入口（下发会被 HAL 拒整包）
    const float d = kIconD;
    const float x = screenX(kFlashIconX - d / 2), y = screenY(kFlashIconY - d / 2);
    gl_.roundedRect(x, y, dim(d), dim(d), dim(d / 2),
                    {0, 0, 0, 0.80f}, {1, 1, 1, 0.55f}, dim(1.5f));
    // 颜色只做辅助：关=灰、自动=白、开/常亮=强调橙。档位本身靠图标附加符号区分，
    // 单靠颜色在强光下读不出，色弱用户也分不开。
    const Rgba c = (flashMode_ == 0) ? kT3 : (flashMode_ == 1) ? kT1 : kAccent;
    flashIcon(gl_, screenX(kFlashIconX), screenY(kFlashIconY), dim(d) * 0.30f, scale_,
              flashMode_, c);
}

void Ui::drawSettingsButton() {
    const float d = kIconD;
    const float x = screenX(kSetIconX - d / 2), y = screenY(kSetIconY - d / 2);
    const bool active = (panel_ == Panel::SETTINGS);
    // 底衬用近黑实底 + 亮描边（浮层铁律：浅色半透明会随预览背景一起消失）
    gl_.roundedRect(x, y, dim(d), dim(d), dim(d / 2),
                    {0, 0, 0, 0.80f}, {1, 1, 1, 0.55f}, dim(1.5f));
    if (active)
        gl_.roundedRect(x, y, dim(d), dim(d), dim(d / 2),
                        Rgba{kAccent.r, kAccent.g, kAccent.b, 0.30f}, kAccent, dim(2));
    gearIcon(gl_, screenX(kSetIconX), screenY(kSetIconY), dim(d) * 0.30f, scale_, kT1);
}

void Ui::drawExposureButton() {
    const float d = kIconD;
    const float x = screenX(kExpIconX - d / 2), y = screenY(kExpIconY - d / 2);
    const bool active = (panel_ == Panel::EXPOSURE);
    gl_.roundedRect(x, y, dim(d), dim(d), dim(d / 2),
                    {0, 0, 0, 0.80f}, {1, 1, 1, 0.55f}, dim(1.5f));
    if (active)
        gl_.roundedRect(x, y, dim(d), dim(d), dim(d / 2),
                        Rgba{kAccent.r, kAccent.g, kAccent.b, 0.30f}, kAccent, dim(2));
    exposureIcon(gl_, screenX(kExpIconX), screenY(kExpIconY), dim(d) * 0.30f, scale_, kT1);
}

void Ui::openPanel(Panel p) {
    if (panel_ == p) return;
    panel_ = p;
    buildCtlRects();   // 提前填充命中矩形（首帧绘制前若有触摸也不落空）
    markDirty();       // 按钮高亮变化
    hap_.click();
}

void Ui::closePanel() {
    if (panel_ == Panel::NONE) return;
    panel_ = Panel::NONE;
    ctlRects_.clear();
    markDirty();
}

// 面板内容块高：由行数推导（buildCtlRects 须已 build）。drawPanel 与
// handlePanelTap 共用同一份 ctlRects_，天然一致。
void Ui::buildCtlRects() {
    ctlRects_.clear();
    if (panel_ == Panel::NONE) return;
    // 内容块在全屏内容区里垂直居中：设置面板 9 行（3 标题+6 控件）占 516px，
    // 曝光面板 5 行只占 280px —— 顶对齐会让后者下半屏空着。
    const int nHead = (panel_ == Panel::SETTINGS) ? 3 : 2;
    const int nRow  = (panel_ == Panel::SETTINGS) ? 6 : 3;
    const float totalH = nHead * kPanelHeadH + nRow * kPanelRowH;
    float y = kPanelTopY + (kPanelBotY - kPanelTopY - totalH) / 2;
    const float ctlX = kPanelCtlR - kPanelCtlW;
    auto header = [&](const char* t) {
        ctlRects_.push_back({kPanelPadL, y, 0, kPanelHeadH, 0, 0, int(A_HEADER), t, nullptr});
        y += kPanelHeadH;
    };
    auto row = [&](const char* label, const char** opts, int n, int sel, PAct act) {
        ctlRects_.push_back({ctlX, y + (kPanelRowH - kPanelCtlH) / 2, kPanelCtlW, kPanelCtlH,
                             n, sel, int(act), label, opts});
        y += kPanelRowH;
    };
    if (panel_ == Panel::SETTINGS) {
        header("照片质量");
        row("格式", kFmtOpts, 2, fmtJpg_ ? 1 : 0, A_FMT);
        row("RAW模式", kRingOpts, 2, rawRing_ ? 0 : 1, A_RAW);
        row("连拍配额", kQuotaOpts, 4, saveQuotaSel_, A_QUOTA);
        header("辅助构图");
        row("网格线", kOnOff, 2, gridOn_ ? 1 : 0, A_GRID);
        row("水平仪", kOnOff, 2, levelOn_ ? 1 : 0, A_LEVEL);
        row("安全框", kOnOff, 2, safeFrameOn_ ? 1 : 0, A_SAFE);
        header("其他");
        row("持久化", kOnOff, 2, persist_ ? 1 : 0, A_PERSIST);
    } else {
        header("曝光");
        row("自动曝光", kOnOff, 2, aeOn_ ? 1 : 0, A_AE);
        header("白平衡");
        row("自动白平衡", kOnOff, 2, awbOn_ ? 1 : 0, A_AWB);
        row("白平衡预设", kAwbLabels, 8, awbPreset_, A_AWBPRESET);
    }
}

void Ui::drawPanel() {
    buildCtlRects();
    // 全屏实底（面板本身就是全屏，不需要再叠一层压暗）
    gl_.roundedRect(screenX(0), screenY(0), dim(kStageW), dim(kStageH), 0,
                    {0.055f, 0.055f, 0.065f, 1.0f}, kNone, 0);
    const bool set = (panel_ == Panel::SETTINGS);
    // 标题居中（关闭按钮在角落，标题不与之争位）
    const char* title = set ? "设置" : "曝光白平衡";
    const float tfs = 26 * kUiZoom * scale_;
    const float tW = gl_.textWidth(title, tfs);
    gl_.text(title, screenX(kStageW / 2) - tW / 2, screenY(46), tfs, kT1);

    // 行
    const float fs = 19 * kUiZoom * scale_;
    for (auto& c : ctlRects_) {
        if (c.act == int(A_HEADER)) {
            const float hfs = 15 * kUiZoom * scale_;
            gl_.text(c.label, screenX(c.x),
                     gl_.textCenterTop(c.label, hfs, screenY(c.y + c.h / 2)), hfs, kAccent);
            // 标题下细线，强化分组
            const float ly = c.y + c.h - 2;
            gl_.roundedRect(screenX(kPanelPadL), screenY(ly), dim(kStageW - 2 * kPanelPadL),
                            std::max(1.f, dim(1)), 0, {1, 1, 1, 0.10f}, kNone, 0);
            continue;
        }
        gl_.text(c.label, screenX(c.x - 24) - gl_.textWidth(c.label, fs),
                 gl_.textCenterTop(c.label, fs, screenY(c.y + c.h / 2)), fs, kT2);
        // 分段控件底
        gl_.roundedRect(screenX(c.x), screenY(c.y), dim(c.w), dim(c.h), dim(10),
                        {1, 1, 1, 0.08f}, kLine, 1);
        const float sw = c.w / c.n;
        for (int i = 0; i < c.n; ++i) {
            const float sx = screenX(c.x + i * sw);
            if (i == c.sel) {
                gl_.roundedRect(sx + dim(3), screenY(c.y + 3), dim(sw - 6), dim(c.h - 6),
                                dim(8), kAccent, kNone, 0);
            } else if (i > 0) {
                // 段分隔线
                gl_.roundedRect(sx, screenY(c.y + 8), std::max(1.f, dim(1)),
                                dim(c.h - 16), 0, {1, 1, 1, 0.12f}, kNone, 0);
            }
            const char* o = c.opts[i];
            const float ofs = 17 * kUiZoom * scale_;
            const float ow = gl_.textWidth(o, ofs);
            gl_.text(o, sx + (dim(sw) - ow) / 2,
                     gl_.textCenterTop(o, ofs, screenY(c.y + c.h / 2)), ofs,
                     i == c.sel ? kWhite : kT2);
        }
    }

    // 关闭按钮：落在原入口图标位（设置左上 / 曝光右上），点它即关闭
    const float ccx = set ? kSetIconX : kExpIconX;
    const float ccy = set ? kSetIconY : kExpIconY;
    const float scx = screenX(ccx - kIconD / 2), scy = screenY(ccy - kIconD / 2);
    gl_.roundedRect(scx, scy, dim(kIconD), dim(kIconD), dim(kIconD / 2),
                    {0, 0, 0, 0.80f}, {1, 1, 1, 0.55f}, dim(1.5f));
    closeIcon(gl_, screenX(ccx), screenY(ccy), dim(kIconD) * 0.30f, scale_, kT1);
}

void Ui::handlePanelTap(float x, float y) {
    // 关闭按钮优先：落在入口图标位（左上 / 右上）即关闭当前面板
    const bool set = (panel_ == Panel::SETTINGS);
    const float ccx = set ? kSetIconX : kExpIconX;
    const float ccy = set ? kSetIconY : kExpIconY;
    if (std::hypot(x - ccx, y - ccy) <= kIconD / 2 + kIconHitPad) {
        closePanel(); hap_.click(); return;
    }
    for (auto& c : ctlRects_) {
        if (c.act == int(A_HEADER)) continue;
        if (inR(x, y, c.x, c.y, c.w, c.h)) {
            const int seg = int(std::clamp((x - c.x) / (c.w / c.n), 0.f, float(c.n - 1)));
            doAction(PAct(c.act), seg);
            return;
        }
    }
    // 其余区域（面板已全屏，无"外部"概念）：忽略
}

void Ui::doAction(PAct act, int seg) {
    switch (act) {
        case A_FMT:       applyFmt(seg == 1); break;
        case A_RAW:       applyRawMode(seg == 0); break;
        case A_QUOTA:     applyQuota(kQuotaVals[seg]); break;
        case A_GRID:      gridOn_ = (seg == 1); markDirty(); commitPersist(); break;
        case A_LEVEL:     levelOn_ = (seg == 1); markDirty(); commitPersist(); break;
        case A_SAFE:      safeFrameOn_ = (seg == 1); markDirty(); commitPersist(); break;
        case A_PERSIST:   persist_ = (seg == 1); markDirty(); commitPersist(); break;
        case A_AE:        applyAe(seg == 1); break;
        case A_AWB:       applyAwb(seg == 1); break;
        case A_AWBPRESET: applyAwbPreset(seg); break;
        default: break;
    }
    hap_.click();
    buildCtlRects();   // 刷新选中态显示
}

void Ui::applyFmt(bool jpg) {
    if (jpg == fmtJpg_) return;       // SET_FMT 绝对语义：仅值变化时下发
    fmtJpg_ = jpg;
    markDirty();   // HUD 的 RAW/JPG 角标画在静态覆盖层里，不置脏就停在旧字样
    pushCmd(Cmd::SET_FMT, jpg ? 1.f : 0.f);   // 绝对语义（v>0.5=JPEG）
    commitPersist();
}

void Ui::applyRawMode(bool ring) {
    if (ring == rawRing_) return;
    rawRing_ = ring;
    pushCmd(Cmd::SET_RAW_MODE, ring ? 1.f : 0.f);
    commitPersist();
}

void Ui::applyQuota(int n) {
    int sel = 3;
    for (int i = 0; i < 4; ++i) if (kQuotaVals[i] == n) sel = i;
    if (sel == saveQuotaSel_) return;
    saveQuotaSel_ = sel;
    pushCmd(Cmd::SET_SAVE_QUOTA, float(n));
    commitPersist();
}

void Ui::applyAe(bool on) {
    if (on == aeOn_) return;
    aeOn_ = on;
    markDirty();     // 面板选中态在逐帧动态层，状态变必须显式置脏重画
    pushCmd(Cmd::SET_AE, on ? 1.f : 0.f);
    commitPersist();
}

void Ui::applyAwb(bool on) {
    if (on == awbOn_) return;
    awbOn_ = on;
    markDirty();
    pushCmd(Cmd::SET_AWB, on ? 1.f : 0.f);
    commitPersist();
}

void Ui::applyAwbPreset(int idx) {
    if (idx < 0 || idx > 7) idx = 0;
    // markDirty 不可省：面板画在逐帧动态层，选中态不回写就不会重画 ⇒ 高亮停在旧档
    // （2026-10-03 真机：点「阴天」无反应，日志证明命中与分段都对，就是缺重画）。
    // 幂等守卫：**必须带上 awbOn_**。本函数有副作用（awbOn_ = true），守卫若只看档位，
    // 关掉白平衡后再点**当前高亮那档**会被直接 return，白平衡仍是关的 ⇒ 用户视角
    // 「点了没反应」（2026-10-04 审出）。档位没变但白平衡关着时，必须放行以重开。
    // 通用教训：有副作用的 apply*，幂等守卫不能放在副作用之前。
    if (idx == awbPreset_ && awbOn_) return;
    awbPreset_ = idx;
    awbOn_ = true;   // 选预设即打开白平衡
    pushCmd(Cmd::SET_WB_PRESET, float(idx));
    markDirty();
    commitPersist();
}

void Ui::applyFlash(int mode) {
    if (mode < 0 || mode > 3) mode = 0;
    if (mode == flashMode_) return;
    flashMode_ = mode;
    markDirty();          // 入口图标画在静态覆盖层，不置脏就停在旧档位符号
    pushCmd(Cmd::SET_FLASH, float(mode));
    commitPersist();
}

void Ui::commitPersist() {
    if (dataDir_.empty()) return;
    const std::string path = dataDir_ + "/settings.txt";
    if (!persist_) {
        // 关持久化 = 不记住任何设置，必须**删掉已落盘的文件**。只 "return 不写" 的话，
        // 旧文件里的 persist=1 和全部旧值会留在盘上，下次启动原样恢复 ⇒ 开关形同虚设
        // （2026-10-04 真机实测：关持久化后改网格，文件里 grid 照样被改写）。
        std::remove(path.c_str());
        return;
    }
    std::ofstream f(path);
    if (!f) return;
    f << "persist=1\n";
    f << "fmt=" << (fmtJpg_ ? "jpg" : "raw") << "\n";
    f << "rawmode=" << (rawRing_ ? "ring" : "once") << "\n";
    f << "quota=" << kQuotaVals[saveQuotaSel_] << "\n";
    f << "ae=" << (aeOn_ ? "on" : "off") << "\n";
    f << "awb=" << (awbOn_ ? "on" : "off") << "\n";
    f << "awbpreset=" << awbPreset_ << "\n";
    f << "flash=" << kFlashNames[flashMode_] << "\n";
    f << "grid=" << (gridOn_ ? 1 : 0) << "\n";
    f << "level=" << (levelOn_ ? 1 : 0) << "\n";
    f << "safe=" << (safeFrameOn_ ? 1 : 0) << "\n";
}

void Ui::loadPersistedSettings() {
    if (dataDir_.empty()) return;
    std::ifstream f(dataDir_ + "/settings.txt");
    if (!f) return;
    std::string line;
    bool persist = false;
    // **先解析再应用**：applyAwbPreset() 会强制 awbOn_=true（选预设即开白平衡），
    // 若边解析边应用，文件行序就决定最终 AWB 状态（awb=off 行在 awbpreset 行之前/之后
    // 结果相反）——加载同一份配置得到两种状态。改为全量读完再按固定顺序应用。
    bool hasAwb = false, awbOn = true;
    int awbPresetIdx = 0;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "persist") persist = (v == "1");
        else if (k == "fmt") { if ((v == "jpg") != fmtJpg_) applyFmt(v == "jpg"); }
        else if (k == "rawmode") { bool ring = (v == "ring"); if (ring != rawRing_) applyRawMode(ring); }
        else if (k == "quota") { int n = std::atoi(v.c_str()); if (n != kQuotaVals[saveQuotaSel_]) applyQuota(n); }
        else if (k == "ae") { bool on = (v == "on"); if (on != aeOn_) applyAe(on); }
        else if (k == "awb") { hasAwb = true; awbOn = (v == "on"); }
        else if (k == "awbpreset") awbPresetIdx = std::atoi(v.c_str());
        else if (k == "flash") {
            int m = 0;
            if (v == "auto") m = 1;
            else if (v == "on") m = 2;
            else if (v == "torch") m = 3;
            else m = std::clamp(std::atoi(v.c_str()), 0, 3);
            if (m != flashMode_) applyFlash(m);
        }
        else if (k == "grid") { bool on = (v == "1"); if (on != gridOn_) { gridOn_ = on; markDirty(); } }
        else if (k == "level") { bool on = (v == "1"); if (on != levelOn_) { levelOn_ = on; markDirty(); } }
        else if (k == "safe") { bool on = (v == "1"); if (on != safeFrameOn_) { safeFrameOn_ = on; markDirty(); } }
    }
    // AWB：预设先落（可能顺带打开白平衡），再按文件里的 awb 开关收敛到最终态。
    // 顺序固定 ⇒ 结果与文件行序无关。
    applyAwbPreset(std::clamp(awbPresetIdx, 0, 7));
    if (hasAwb && awbOn != awbOn_) applyAwb(awbOn);
    // 持久化开关本身：仅当文件标记为持久化时才恢复开启（否则默认关闭、不恢复旧值）
    persist_ = persist;
    markDirty();
}

void Ui::drawLevel(float x, float y, float w, float h) {
    const float cx = x + w / 2, cy = y + h / 2;
    // 取景中心十字：与网格同口径双层（近黑描边 + 亮白芯），否则亮场景下不可见
    const float t = std::max(dim(1.6f), 2.f), e = std::max(dim(0.9f), 1.f);
    gl_.roundedRect(cx - dim(40) - e, cy - t / 2 - e, dim(80) + 2 * e, t + 2 * e, 0,
                    kGridEdge, kNone, 0);
    gl_.roundedRect(cx - t / 2 - e, cy - dim(40) - e, t + 2 * e, dim(80) + 2 * e, 0,
                    kGridEdge, kNone, 0);
    gl_.roundedRect(cx - dim(40), cy - t / 2, dim(80), t, 0, kGridCore, kNone, 0);
    gl_.roundedRect(cx - t / 2, cy - dim(40), t, dim(80), 0, kGridCore, kNone, 0);
    // 水平仪气泡：随 roll 横向偏移，居中 = 水平（无传感器时 roll_=0，气泡居中）
    const float off = std::clamp(roll_ * (w * 0.35f), -w * 0.45f, w * 0.45f);
    const float br = dim(10);
    gl_.roundedRect(cx + off - br, cy - br, br * 2, br * 2, br,
                    {1, 1, 1, 0.5f}, kAccent, 1.5f);
    // 中央参考刻度
    gl_.roundedRect(cx - dim(1), cy - dim(6), dim(2), dim(12), 0, kAccent, kNone, 0);
}

void Ui::drawSafeFrame(float x, float y, float w, float h) {
    const float m = 0.05f;   // 5% 安全边距
    const float sw = std::max(dim(1.6f), 2.f), e = std::max(dim(0.9f), 1.f);
    // 双层描边（近黑 + 亮白）：单层低 alpha 白在亮预览上会整条消失，同 drawGrid
    gl_.roundedRect(x + w * m, y + h * m, w * (1 - 2 * m), h * (1 - 2 * m), 0,
                    kNone, kGridEdge, sw + 2 * e);
    gl_.roundedRect(x + w * m, y + h * m, w * (1 - 2 * m), h * (1 - 2 * m), 0,
                    kNone, kGridCore, sw);
}

// ---- 电子水平仪：加速度计（无传感器则 roll_ 恒 0，气泡居中，优雅降级）----
void Ui::initLevel() {
    // ASensorManager_getInstance 自 API26 起废弃；等价且非废弃的入口是
    // getInstanceForPackage(nullptr)，minSdk=31 可直接用，避免 -Wdeprecated-declarations。
    snsMgr_ = ASensorManager_getInstanceForPackage(nullptr);
    if (!snsMgr_) return;
    snsAcc_ = ASensorManager_getDefaultSensor(snsMgr_, ASENSOR_TYPE_ACCELEROMETER);
    if (!snsAcc_) return;
    ALooper* looper = ALooper_forThread();
    if (!looper) return;
    snsQ_ = ASensorManager_createEventQueue(snsMgr_, looper, 1, nullptr, nullptr);
    if (snsQ_)
        ASensorEventQueue_enableSensor(snsQ_, snsAcc_);
}

void Ui::pollLevel() {
    if (!snsQ_) return;
    ASensorEvent e;
    // 非阻塞：每帧最多消费若干事件，取最新倾角
    int got = 0;
    while (ASensorEventQueue_getEvents((ASensorEventQueue*)snsQ_, &e, 1) > 0 && got < 4) {
        // 横滚（左右倾）≈ atan2(gx, gz)：手机平放时 gz≈+9.8，左右倾使 gx 增大
        roll_ = std::atan2(e.acceleration.x, e.acceleration.z);
        ++got;
    }
}

} // namespace optic::ui
