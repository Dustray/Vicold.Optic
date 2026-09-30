#pragma once
// 触感反馈（JNI → 系统 Vibrator）：按钮点按 EFFECT_CLICK、滚轮落档 EFFECT_TICK。
// 仅 glue 线程使用（init 与 fire 同线程，AttachCurrentThread 返回缓存 env，零开销）。
// 预定义触感是厂商校准的马达原语（比自定时长振动的手感好得多），API 29+ 可用；
// 效果对象与 Vibrator 在 init 一次性建好并持全局引用，click()/tick() 只剩一次调用。

#include <jni.h>

namespace optic::ui {

class Haptics {
public:
    void init(JavaVM* vm, jobject activity);   // android_main 注入（幂等）
    void click() { fire(effClick_, 20); }      // 按钮点按
    void tick() { fire(effTick_, 8); }         // 刻度落档（跨档一次）

private:
    void fire(jobject effect, jint msFallback);
    JavaVM* vm_ = nullptr;
    jobject activity_ = nullptr;               // 全局引用
    jobject vibrator_ = nullptr;               // 全局引用（getSystemService 一次）
    jmethodID mVibEffect_ = nullptr;           // vibrate(VibrationEffect)
    jmethodID mVibMs_ = nullptr;               // vibrate(J) 兜底（createPredefined 不可用时）
    jobject effClick_ = nullptr;               // EFFECT_CLICK 预建效果（全局引用）
    jobject effTick_ = nullptr;                // EFFECT_TICK 预建效果
    bool ok_ = false;
};

} // namespace optic::ui
