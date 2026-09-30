#pragma once
// M-UI：camera-ui.html 设计的 C++ GL 实现。
// 设计空间 1560×720（横屏），等比 cover 到屏幕；UI/渲染/输入同在 glue 线程；
// 设置命令经队列传引擎线程应用（iso/ss/zoom/ev/ae/shot 全走 triggerBurst 同款配额闸门）。
// 参考原型：/camera-ui.html（布局常量 1:1 移植）。

#include <android/input.h>
#include <android/native_window.h>
#include <jni.h>

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "core/ui/Gl.h"
#include "core/ui/Haptics.h"

namespace optic::ui {

class Ui {
public:
    struct Cmd {
        enum Type { SET_ISO, SET_EXP_US, SET_ZOOM, SET_EV, SET_AE, SET_ISO_AUTO,
                    SET_SS_AUTO, SET_AE_LOCK, SHOT } type = SET_ISO;
        float v = 0;
    };

    ~Ui();                               // 析构出线（Gl::Impl 完整性在 Ui.cpp）
    bool attach(ANativeWindow* win);     // EGL + 字体（幂等）
    void detach();
    bool attached() const { return gl_.ready(); }
    int32_t winW() const { return gl_.width(); }     // 主窗口尺寸（resize 检测用）
    int32_t winH() const { return gl_.height(); }
    // slot：0=逻辑主摄 / 1=超广角直连 / 2=长焦直连（多流常驻会话，见 CameraEngine）
    ANativeWindow* previewWindow(int slot) { return gl_.previewWindow(slot); }
    // 引擎切带通知（纯 GL 显示层，不动请求）：目标源有帧即切换显示并交叉淡化 150ms
    //（遮跨镜头 AE/AWB/内容跳变），期间旧画面持续叠加淡出。
    void setPreviewSlot(int slot) { previewTarget_.store(slot, std::memory_order_release); }
    // 三路常流帧计数（诊断：确认 uw/tele 在非显示带也在持续出帧/收敛）
    int64_t slotFrames(int i) const {
        return i >= 0 && i < 3 ? slotFrames_[i].load(std::memory_order_relaxed) : -1;
    }
    int32_t previewW(int slot = 0) { return gl_.previewW(slot); }
    int32_t previewH(int slot = 0) { return gl_.previewH(slot); }

    void setStaticText(int32_t rawW, int32_t rawH) { rawW_ = rawW; rawH_ = rawH; }
    void setUvRot(int rot) { uvRotOverride_ = rot; }
    // 定向的两个真值输入（比"猜方向"可靠）：
    //   sensorDeg  = ACAMERA_SENSOR_ORIENTATION（缓冲需顺时针转多少度才在"本机自然方向"下正立）
    //   displayRot = Surface.ROTATION_*（0/1/2/3；屏幕图形相对机身已顺时针转了 rot*90°）
    // 二者齐全时 rot = (displayRot*90 - sensorDeg)/90 mod 4，横屏两个方向都能自动对。
    void setSensorOrientation(int deg) { sensorDeg_ = deg; }
    void setDisplayRot(int r) { displayRot_ = r; }
    // 注入应用数据目录：attach 时读 controls.txt 的 preview_w/preview_h（机型调参用）
    void setDataDir(const std::string& dir) { dataDir_ = dir; }
    // 注入 JNI（android_main 一次性调用）：触感反馈经系统 Vibrator
    void setJni(JavaVM* vm, jobject activity) { hap_.init(vm, activity); }

    // 引擎线程回传本次快门是否被配额放行（toast 据此给真实反馈）
    void notifyShot(bool accepted, int used, int total) {
        shotOk_.store(accepted ? 1 : 0, std::memory_order_release);
        shotUsed_.store(used, std::memory_order_release);
        shotTotal_.store(total, std::memory_order_release);
    }

    // ---- 平滑拖拽变焦（引擎线程回传）----
    // 每路预览源（0=逻辑主摄 / 1=超广角 / 2=长焦）各自维护一个 az（当前出图帧对应的
    // 用户倍率）；crop 取当前显示 slot 的 az，避免三路结果互相污染（2026-09-30 主摄带
    // 卡顿根因：超广/长焦结果曾把 az 冲成 1.0）。全部由 GL 数字裁切模拟带内变焦，
    // 收敛后 crop 恒回 1，无跳变。跨带会话重建 ~285ms 冻结只发生在松手落位时。
    void setAppliedZoom(int slot, float z) {
        if (slot >= 0 && slot < 3) az_[slot].store(z, std::memory_order_release);
    }
    // az 时间戳对齐回调注入（引擎在 rebuildSession 时调用一次）：Gl 每消费一帧
    // 预览图像，按该帧 SENSOR_TIMESTAMP 查引擎结果环 → setAppliedZoom 写回。
    void setAzSource(std::function<float(int slot, int64_t tsNs)> f) {
        gl_.setAzSource(std::move(f));
    }
    // 长焦直连阈值（引擎按机型探测下发；无长焦 = 极大值，UI 永不钳制）
    void setTeleMin(float z) { teleMin_ = z; }
    // 外部 zoom 同步（controls.txt 诊断通道）：UI 内部 zoom_ 是 crop 补偿与导轨读数的
    // 基准，诊断值不联动会让 crop 停在旧值（2026-09-30 截图验证失真的根因）。
    // 导轨把手位置不搬动（视觉跳变），仅同步数值状态。
    void setZoomExternal(float z) { zoom_ = z; }

    // 导轨下限 = 超广角光学倍率（运行时探测）：低于它画不了更广，拖到底会有一段
    // 画面不动的死区（本机 uw 原生 0.774x > HAL 声称的 0.70x，2026-09-30）。
    void setZoomRange(float zmin) { zoomMin_ = std::max(zmin, 0.5f); }
    // EV 量程（EV 值，非步数）：来自设备 traits（AE_COMPENSATION_RANGE×STEP）。
    // 全手动（ISO/SS 都非自动）时 EV 面板灰显不可拖 —— EV 只作用于处于自动态的参数。
    void setEvRange(float minEv, float maxEv, float stepEv) {
        if (maxEv > minEv + 1e-3f && stepEv > 0.01f) {
            evMinEv_ = minEv;
            evMaxEv_ = maxEv;
            evStepEv_ = stepEv;
        }   // traits 缺失/退化时保留默认 ±3.0/0.5
    }
    // AE 实时回传（自动参数的当前生效值，UI 数值行显示用；引擎每帧从结果更新）
    void setAutoIso(int iso) { autoIso_.store(iso, std::memory_order_relaxed); }
    void setAutoSsUs(int64_t us) { autoSsUs_.store(us, std::memory_order_relaxed); }
    // 导轨上限：引擎侧钳制 UI 下发的 zoom 时用（相机拿不到的高倍由 GL 裁切完成）
    static constexpr float zoomLimitMax() { return kZoomMax; }

    void onInputEvent(AInputEvent* e);   // glue 线程
    void frame();                        // 绘制一帧（glue 线程，vsync 节奏）

    Haptics hap_;                        // 触感反馈（按钮点按 / 刻度落档）
    int hapStop_ = -1;                   // 本次拖拽上次落档的档位 id（跨档变化才震）
    void hapticTickStop(int id);         // id<0 = 离开吸附带（复位，往返再入会再震）

    // 引擎线程消费
    bool popCmd(Cmd* out);

    // 屏幕像素 → 设计空间（输入命中用，坐标系原点左上）
    float toDesignX(float px) const { return (px - offX_) / scale_; }
    float toDesignY(float py) const { return (py - offY_) / scale_; }

private:
    // 设计空间常量（camera-ui.html 移植）
    static constexpr float kStageW = 1560, kStageH = 720;
    // 变焦范围 0.7–120（与系统相机口径对齐）：
    //   [0.7, 1.0) 超广角 / [1.0, 5.0) 广角主摄 / [5.0, 120] 长焦。
    // 高于相机能力的高倍段（本机长焦原生 ≈5x，HAL 逻辑流上限 5x）全部由 GL 数字裁切
    // 完成 —— 相机只收到 ≤4.85 的请求，120x 对应约 24 倍裁切（画质软，但取景可用）。
    static constexpr float kZoomMin = 0.7f, kZoomMax = 120.f, kZoomBaseMm = 23.f;
    float zoomMin_ = kZoomMin;   // 运行时实际下限（setZoomRange 覆盖，默认按 HAL 量程）
    static constexpr int kRingFrames = 4;

    void onDown(float dx, float dy);
    void onMove(float dx, float dy);
    void onUp(float dx, float dy);
    float zoomFromF(float f) const;   // 轨道连续位置 → 变焦值（等距分段 + 刻度吸附）
    float zoomFrac() const { return zoomToF(zoom_); }         // 当前变焦 → 轨道位置
    // 变焦轨道映射：关键焦段在轨道上**等距**（6 段均分），段内对数插值 ——
    // 刻度间距相同、手感一致；首项用运行时下限 zoomMin_（0.774 光学极限时
    // 0.7 档被跳过，轨道底 = 0.774）。
    float zoomToF(float z) const;
    int zoomStopsEff(float out[8]) const;
    void draw();
    void drawTracks();
    void drawPreviewOverlay();
    void drawHistogram(float x, float y, float w, float h);
    void drawGrid(float x, float y, float w, float h);
    void drawVTicks(float tx, float ty, float tw, float th,
                    const std::vector<float>& fracs, const std::vector<char>& major);
    // ---- 中心确认点刻度盘（替代滑块）----
    // 轨道正中是固定的确认线，刻度整体随当前值滚动：中心对准的刻度即当前值。
    // f ∈ [0,1] 是该轨道的连续位置（0=底/左，1=顶/右），curF 为当前值对应的位置。
    struct RollItem {
        float f;             // 该刻度的位置
        bool major;          // 主刻度（长线 + 标签）
        const char* label;   // 主刻度标签（nullptr = 不画）
    };
    void drawRollerV(float tx, float ty, float tw, float th, float curF,
                     const RollItem* items, int n, bool grey = false);
    void drawRollerH(float tx, float ty, float tw, float th, float curF,
                     const RollItem* items, int n, bool grey = false);
    // 拖拽基准：按下瞬间的指针位置与轨道位置。刻度盘用**相对位移**驱动（手指移动
    // 多少像素，刻度滚多少像素），按下即取值会造成"手还没动值先跳"。
    float dragRefPos_ = 0.f;
    float dragRefF_ = 0.f;
    // 离散档位轨（ISO/SS）的**连续**滚动位置：拖动中按像素连续滚（刻度有滚动感），
    // 松手落位后渲染直接用 idx 派生值 —— 不加这两个成员，拖动时刻度只能整档跳变。
    // 松手时必须把 roll_ 回写成落位 idx 的位置，否则下次触摸渲染切回 roll_ 会回跳到
    // 松手前的非整数位置（触摸/松手来回跳，2026-09-30 真机确诊）。
    float isoRoll_ = 0.f, ssRoll_ = 0.f;
    // cover 缩放：长度只乘 scale_；位置再加居中偏移（cover 下 offX/offY 恒 ≤ 0）
    float dim(float designLen) const { return designLen * scale_; }
    float screenX(float designX) const { return designX * scale_ + offX_; }
    float screenY(float designY) const { return designY * scale_ + offY_; }

    Gl gl_;
    float scale_ = 1.f;
    float offX_ = 0.f, offY_ = 0.f;
    // 左导轨面板位置（attach 按窗口几何自适应）：全出血（无横向溢出）时面板扩进
    // 挖孔保留区（面板近纯黑，挖孔落在上面不可见）；有溢出（MIUI 保留 150px）
    // 时面板从溢出裁切线（kSafeW）起。右缘恒贴轨道右缘（kRailREnd）。
    float railLX_ = 10.f, railLW_ = 182.f;
    int uvRotOverride_ = -1;                            // controls.txt 的 uvrot 强制值
    int sensorDeg_ = 90;                                // 缺省 90（手机常规挂载）
    int displayRot_ = -1;                               // -1 表示还没取到
    int resolveUvRot() const;                           // -1=交给 Gl 做几何自动判断
    std::string dataDir_;

    // UI 状态
    static constexpr int kIsoStopsN = 23;                // 50–12800（2026-09-30 用户指定表）
    static constexpr int kSsStopsN = 55;                 // 1/8000 s–30 s，1/3 EV 步进全阶梯
    static const float kIsoStops[kIsoStopsN];
    static const float kSsStops[kSsStopsN];              // 秒（≥1 直接秒；<1 为 1/x s 的倒数域）
    // 关键焦段（与系统相机一致）：0.7 超广 / 1 广角 / 2 / 5 长焦起点 / 10 / 50 / 120。
    // 刻度盘上**等距**分布（见 zoomToF/zoomFromF 的分段映射）。
    static constexpr int kZoomStopsN = 7;
    static constexpr float kZoomStops[kZoomStopsN] = {0.7f, 1, 2, 5, 10, 50, 120};
    int isoIdx_ = 9;      // ISO 400（新 23 档表中 400 的下标）
    int ssIdx_ = 30;      // 1/8 s（1/8000→30s 阶梯中 1/8 s 的下标）
    float zoom_ = 1.0f;   // 与引擎初始状态一致（引擎 zoomRatio=0 未设置 ≈ 原生 1.0）
    float ev_ = 0.f;      // 与引擎初始 evSteps=0 一致（曾误置 -0.3：启动显示与实际不符）
    bool isoAuto_ = true, ssAuto_ = true;   // 各参数自动态（拖滚轮→手动；A 键切回）
    bool aeLock_ = false;                   // 测光锁定（AE_LOCK，快门上方按钮）
    float evMinEv_ = -3.f, evMaxEv_ = 3.f, evStepEv_ = 0.5f;   // EV 量程（引擎按 traits 下发）
    std::atomic<int> autoIso_{0};           // AE 实测 ISO（自动参数数值行显示）
    std::atomic<int> autoSsUs_{0};          // AE 实测曝光时间 µs
    int zoomUnit_ = 0;                                  // 0=mm 1=×
    bool gridOn_ = true;

    // 触摸
    enum class Drag { NONE, ZOOM, ISO, SS, EV } drag_ = Drag::NONE;
    bool shutterDown_ = false;
    double lastShotAt_ = 0;
    double lastZoomPush_ = 0;   // 上次实时变焦下发时刻（拖拽节流，见 pushZoomLive）
    float lastCamPush_ = 0.f;   // 上次下发给相机的目标值
    std::atomic<float> az_[3] = {1.f, 1.f, 1.f};  // 三路预览源各自的应用倍率（引擎按 slot 回传）
    // 全带 crop 补偿：crop = zoom_（手指目标）/ az（相机实际出图倍率），相机阶梯下发
    // 的缺口由 GL 每帧补齐 → FOV 连续（逻辑带 120ms 节流不再表现为 8 次/s 跳变）。
    // az 变化或 slot 切换时 crop **落位到新 target**（绝不归一到 1 —— 那会让 FOV
    // 退回后再爬升，与纹理过渡叠加成泵动闪烁）。
    double lastCropT_ = 0;      // 上一帧时刻（淡化计时 dt；crop 已时间戳对齐直接落位）
    float teleMin_ = 2.63f;                 // 长焦直连阈值（setTeleMin 下发）
    void pushZoomLive(float camTarget);

    // 动效
    float flashA_ = 0;
    double flashUntil_ = 0;
    double toastUntil_ = 0;
    int savedCount_ = 4;                                // toast 文案 n/4
    std::atomic<int> shotOk_{-1};                       // -1 未定 / 0 被拒 / 1 已接受
    std::atomic<int> shotUsed_{0}, shotTotal_{0};

    // 预览源切换（ALL 常流）：target = 引擎按带要求的显示源；active = 实际显示源。
    std::atomic<int> previewTarget_{0};
    int previewActive_ = 0;
    // 跨带显示切换的交叉淡化（150ms）：旧源纹理叠画淡出，遮跨镜头 AE/AWB/内容跳变
    int fadeFrom_ = -1;
    float fadeT_ = 0.f;

    // 三路常流帧计数（frame() 每帧 drain 全部源时累加）
    std::atomic<int64_t> slotFrames_[3] = {};

    // 直方图
    int32_t histR_[64] = {}, histG_[64] = {}, histB_[64] = {};
    int32_t rawW_ = 4096, rawH_ = 3072;

    std::mutex cmdM_;
    std::deque<Cmd> cmds_;
    void pushCmd(Cmd::Type t, float v);
};

} // namespace optic::ui
