#pragma once
// M-UI：camera-ui.html 设计的 C++ GL 实现。
// 设计空间 1560×720（横屏），等比 cover 到屏幕；UI/渲染/输入同在 glue 线程；
// 设置命令经队列传引擎线程应用（iso/ss/zoom/ev/ae/shot 全走 triggerBurst 同款配额闸门）。
// 参考原型：/camera-ui.html（布局常量 1:1 移植）。

#include <android/input.h>
#include <android/native_window.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "core/ui/Gl.h"

namespace optic::ui {

class Ui {
public:
    struct Cmd {
        enum Type { SET_ISO, SET_EXP_US, SET_ZOOM, SET_EV, SET_AE, SHOT } type = SET_ISO;
        float v = 0;
    };

    ~Ui();                               // 析构出线（Gl::Impl 完整性在 Ui.cpp）
    bool attach(ANativeWindow* win);     // EGL + 字体（幂等）
    void detach();
    bool attached() const { return gl_.ready(); }
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

    // 引擎线程回传本次快门是否被配额放行（toast 据此给真实反馈）
    void notifyShot(bool accepted, int used, int total) {
        shotOk_.store(accepted ? 1 : 0, std::memory_order_release);
        shotUsed_.store(used, std::memory_order_release);
        shotTotal_.store(total, std::memory_order_release);
    }

    // ---- 平滑拖拽变焦（引擎线程回传）----
    // appliedZoom = 当前出图帧对应的用户倍率；拖拽中相机侧钳在带内实时跟随，
    // 越带部分由 GL 数字裁切模拟（crop = zoom_/applied），收敛后恒回 1，无跳变。
    // 跨带会话重建 ~285ms 冻结（真机实测），只发生在松手落位时，拖动中不再出现。
    void setAppliedZoom(float z) { appliedZoom_.store(z, std::memory_order_release); }
    // 长焦直连阈值（引擎按机型探测下发；无长焦 = 极大值，UI 永不钳制）
    void setTeleMin(float z) { teleMin_ = z; }
    // 外部 zoom 同步（controls.txt 诊断通道）：UI 内部 zoom_ 是 crop 补偿与导轨读数的
    // 基准，诊断值不联动会让 crop 停在旧值（2026-09-30 截图验证失真的根因）。
    // 导轨把手位置不搬动（视觉跳变），仅同步数值状态。
    void setZoomExternal(float z) { zoom_ = z; }

    void onInputEvent(AInputEvent* e);   // glue 线程
    void frame();                        // 绘制一帧（glue 线程，vsync 节奏）

    // 引擎线程消费
    bool popCmd(Cmd* out);

    // 屏幕像素 → 设计空间（输入命中用，坐标系原点左上）
    float toDesignX(float px) const { return (px - offX_) / scale_; }
    float toDesignY(float py) const { return (py - offY_) / scale_; }

private:
    // 设计空间常量（camera-ui.html 移植）
    static constexpr float kStageW = 1560, kStageH = 720;
    // 变焦范围 0.7–10：sub-1.0 走超广角物理直连（引擎侧 uwActive 判定，见 CameraEngine）。
    // 0.7–1.0 区间预览为超广角原生 FOV（物理直连不写 ZOOM_RATIO，段内无数字变焦）。
    static constexpr float kZoomMin = 0.7f, kZoomMax = 10.f, kZoomBaseMm = 23.f;
    static constexpr int kRingFrames = 4;

    void onDown(float dx, float dy);
    void onMove(float dx, float dy);
    void onUp(float dx, float dy);
    float zoomFromY(float y) const;   // 导轨 y 位置 → 变焦值（含档位吸附）
    void draw();
    void drawTracks();
    void drawPreviewOverlay();
    void drawHistogram(float x, float y, float w, float h);
    void drawGrid(float x, float y, float w, float h);
    void drawVTicks(float tx, float ty, float tw, float th,
                    const std::vector<float>& fracs, const std::vector<char>& major);
    // cover 缩放：长度只乘 scale_；位置再加居中偏移（cover 下 offX/offY 恒 ≤ 0）
    float dim(float designLen) const { return designLen * scale_; }
    float screenX(float designX) const { return designX * scale_ + offX_; }
    float screenY(float designY) const { return designY * scale_ + offY_; }

    Gl gl_;
    float scale_ = 1.f;
    float offX_ = 0.f, offY_ = 0.f;
    int uvRotOverride_ = -1;                            // controls.txt 的 uvrot 强制值
    int sensorDeg_ = 90;                                // 缺省 90（手机常规挂载）
    int displayRot_ = -1;                               // -1 表示还没取到
    int resolveUvRot() const;                           // -1=交给 Gl 做几何自动判断
    std::string dataDir_;

    // UI 状态
    static constexpr int kIsoStopsN = 8;
    static constexpr int kSsStopsN = 9;
    static const int kIsoStops[kIsoStopsN];
    static const int kSsStops[kSsStopsN];               // 分母（1/x s）
    static constexpr float kZoomStops[6] = {0.7f, 1, 2, 3, 5, 10};
    int isoIdx_ = 2;
    int ssIdx_ = 3;
    float zoom_ = 1.0f;   // 与引擎初始状态一致（引擎 zoomRatio=0 未设置 ≈ 原生 1.0）
    float ev_ = -0.3f;
    bool aeOn_ = true;
    int zoomUnit_ = 0;                                  // 0=mm 1=×
    bool gridOn_ = true;

    // 触摸
    enum class Drag { NONE, ZOOM, ISO, SS, EV } drag_ = Drag::NONE;
    bool shutterDown_ = false;
    double lastShotAt_ = 0;
    double lastZoomPush_ = 0;   // 上次实时变焦下发时刻（拖拽节流，见 pushZoomLive）
    float lastCamPush_ = 0.f;   // 上次下发给相机的目标值
    std::atomic<float> appliedZoom_{1.0f};  // 引擎回传：当前显示源出图的用户倍率
    // ALL 常流：逻辑带相机实时跟随（crop=1）；物理带（quirk physPerKeyZoom=false）
    // 带内变焦由 GL 裁切补足 —— crop = zoom_/az，az = 带基常量（引擎回传），
    // 带内 az 稳定 → crop 单调连续无泵动；slot 切换瞬间 crop 直接落位（lastCropSlot_）。
    float cropSmooth_ = 1.f;    // GL 裁切平滑值（指数趋近，帧率无关）
    int lastCropSlot_ = 0;      // 上次计算 crop 时的显示 slot（切换 → 直接落位）
    double lastCropT_ = 0;      // 上一帧时刻（淡化计时 dt）
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
