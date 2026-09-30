#pragma once
// 真实电量查询：JNI 注册 null 接收器拿 ACTION_BATTERY_CHANGED 粘性广播。
// 调用方（Ui::frame，glue 线程）低频轮询（~30s 一次），JNI 附加开销可忽略。
// 查询失败（JNI 不可用/字段缺失）返回 -1，UI 保持上次值或隐藏填充。

#include <jni.h>

namespace optic::ui {

class Battery {
public:
    void init(JavaVM* vm, jobject activity);   // android_main 注入（幂等，与 Haptics 同型）
    // pct：0-100（失败 -1）；*charging = 正在充电/已充满
    int query(bool* charging);

private:
    JavaVM* vm_ = nullptr;
    jobject activity_ = nullptr;   // 全局引用
    jmethodID mReg_ = nullptr;     // Context.registerReceiver(BroadcastReceiver, IntentFilter)
    jmethodID mIfCtor_ = nullptr;  // IntentFilter.<init>(String)
    jmethodID mGetInt_ = nullptr;  // Intent.getIntExtra(String, int)
    bool ok_ = false;
};

} // namespace optic::ui
