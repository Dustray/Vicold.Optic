#include "core/ui/Battery.h"

#include "core/util/Log.h"

namespace optic::ui {

Battery::~Battery() {
    if (vm_) {
        JNIEnv* env = nullptr;
        if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK && env) {
            if (ifCls_) env->DeleteGlobalRef(ifCls_);
            if (intentCls_) env->DeleteGlobalRef(intentCls_);
            if (activity_) env->DeleteGlobalRef(activity_);
        }
    }
    ifCls_ = intentCls_ = nullptr;
    activity_ = nullptr;
}



namespace {
// 安全查找：查不到立即清 pending exception（未清会让下次 JNI 调用 abort）
jmethodID lookupMethod(JNIEnv* env, jclass cls, const char* name, const char* sig,
                       bool isStatic) {
    if (!cls) return nullptr;
    jmethodID m = isStatic ? env->GetStaticMethodID(cls, name, sig)
                           : env->GetMethodID(cls, name, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return m;
}
} // namespace

void Battery::init(JavaVM* vm, jobject activity) {
    if (!vm || !activity) return;
    vm_ = vm;
    JNIEnv* env = nullptr;
    if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;

    activity_ = env->NewGlobalRef(activity);
    jclass actCls = env->GetObjectClass(activity_);

    jclass ifCls = env->FindClass("android/content/IntentFilter");
    if (env->ExceptionCheck()) { env->ExceptionClear(); ifCls = nullptr; }
    jclass intentCls = env->FindClass("android/content/Intent");
    if (env->ExceptionCheck()) { env->ExceptionClear(); intentCls = nullptr; }

    mReg_ = lookupMethod(env, actCls, "registerReceiver",
                         "(Landroid/content/BroadcastReceiver;"
                         "Landroid/content/IntentFilter;)Landroid/content/Intent;",
                         false);
    if (ifCls) mIfCtor_ = lookupMethod(env, ifCls, "<init>", "(Ljava/lang/String;)V", false);
    if (intentCls)
        mGetInt_ = lookupMethod(env, intentCls, "getIntExtra", "(Ljava/lang/String;I)I",
                                false);
    // 缓存类为全局引用：glue 线程常驻不 detach，每次查询若 FindClass 会泄漏局部引用
    //（默认局部引用表约 512 → ~4h 周期性查询后溢出崩溃）。全局引用随 UI 生命周期存活。
    if (ifCls) ifCls_ = (jclass)env->NewGlobalRef(ifCls);
    if (intentCls) intentCls_ = (jclass)env->NewGlobalRef(intentCls);
    ok_ = mReg_ && mIfCtor_ && mGetInt_;
    LOGI("battery init: ok=%d", ok_);
}

int Battery::query(bool* charging) {
    if (charging) *charging = false;
    if (!ok_) return -1;
    JNIEnv* env = nullptr;
    if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return -1;

    // registerReceiver(null, new IntentFilter(ACTION_BATTERY_CHANGED)) → 粘性广播
    jstring action = env->NewStringUTF("android.intent.action.BATTERY_CHANGED");
    // 用 init 缓存的全局类引用构造过滤器（不再每次 FindClass，杜绝局部引用泄漏）
    jobject filter = ifCls_ ? env->NewObject(ifCls_, mIfCtor_, action) : nullptr;
    if (!filter) { env->DeleteLocalRef(action); return -1; }
    jobject intent = env->CallObjectMethod(activity_, mReg_, nullptr, filter);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return -1; }
    if (!intent) return -1;

    auto extra = [&](const char* k, jint def) -> jint {
        jstring key = env->NewStringUTF(k);
        jint v = env->CallIntMethod(intent, mGetInt_, key, def);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return def; }
        env->DeleteLocalRef(key);
        return v;
    };
    const jint level = extra("level", -1);
    const jint scale = extra("scale", 100);
    const jint status = extra("status", 0);   // 2=charging 5=full

    env->DeleteLocalRef(filter);
    env->DeleteLocalRef(intent);
    env->DeleteLocalRef(action);

    if (level < 0 || scale <= 0) return -1;
    if (charging) *charging = (status == 2 || status == 5);
    return int(100LL * level / scale);
}

} // namespace optic::ui
