#include "core/ui/Haptics.h"

#include "core/util/Log.h"

namespace optic::ui {

namespace {
// 安全查找：GetMethodID 找不到时抛 NoSuchMethodError（不是返回 null），
// 未清的 pending exception 会让下一次 JNI 调用直接 abort，所以查完立即清。
jmethodID lookupMethod(JNIEnv* env, jclass cls, const char* name, const char* sig,
                       bool isStatic) {
    if (!cls) return nullptr;
    jmethodID m = isStatic ? env->GetStaticMethodID(cls, name, sig)
                           : env->GetMethodID(cls, name, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return m;
}
} // namespace

void Haptics::init(JavaVM* vm, jobject activity) {
    if (!vm || !activity) return;
    vm_ = vm;
    JNIEnv* env = nullptr;
    if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;

    activity_ = env->NewGlobalRef(activity);

    // Vibrator = context.getSystemService(Context.VIBRATOR_SERVICE)。
    // API 31 起官方建议 VibratorManager，但 "vibrator" 服务名仍返回 Vibrator（兼容）。
    jclass ctxCls = env->FindClass("android/content/Context");
    if (env->ExceptionCheck()) { env->ExceptionClear(); ctxCls = nullptr; }
    jstring name = nullptr;
    if (jfieldID f = ctxCls ? env->GetStaticFieldID(ctxCls, "VIBRATOR_SERVICE",
                                                    "Ljava/lang/String;")
                            : nullptr)
        name = (jstring)env->GetStaticObjectField(ctxCls, f);
    jmethodID gss = lookupMethod(env, ctxCls, "getSystemService",
                                 "(Ljava/lang/String;)Ljava/lang/Object;", false);
    if (name && gss) {
        jobject v = env->CallObjectMethod(activity_, gss, name);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            v = nullptr;
        }
        if (v) vibrator_ = env->NewGlobalRef(v);
    }

    // 预定义触感：VibrationEffect.createPredefined(EFFECT_CLICK / EFFECT_TICK)
    if (vibrator_) {
        jclass vCls = env->GetObjectClass(vibrator_);
        mVibEffect_ = lookupMethod(env, vCls, "vibrate", "(Landroid/os/VibrationEffect;)V",
                                   false);
        mVibMs_ = lookupMethod(env, vCls, "vibrate", "(J)V", false);

        jclass veCls = env->FindClass("android/os/VibrationEffect");
        if (env->ExceptionCheck()) { env->ExceptionClear(); veCls = nullptr; }
        jmethodID predef = lookupMethod(env, veCls, "createPredefined",
                                        "(I)Landroid/os/VibrationEffect;", true);
        jfieldID fClick = veCls ? env->GetStaticFieldID(veCls, "EFFECT_CLICK", "I") : nullptr;
        jfieldID fTick = veCls ? env->GetStaticFieldID(veCls, "EFFECT_TICK", "I") : nullptr;
        if (env->ExceptionCheck()) env->ExceptionClear();
        auto makeEffect = [&](jfieldID f) -> jobject {
            if (!predef || !f) return nullptr;
            jobject e = env->CallStaticObjectMethod(veCls, predef,
                                                    env->GetStaticIntField(veCls, f));
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                return nullptr;
            }
            return e ? env->NewGlobalRef(e) : nullptr;
        };
        effClick_ = makeEffect(fClick);
        effTick_ = makeEffect(fTick);
        ok_ = (mVibEffect_ && (effClick_ || effTick_)) || mVibMs_;
    }
    LOGI("haptics init: ok=%d click=%d tick=%d", ok_, effClick_ != nullptr,
         effTick_ != nullptr);
}

void Haptics::fire(jobject effect, jint msFallback) {
    if (!ok_) return;
    JNIEnv* env = nullptr;   // 同线程重复 attach 返回缓存 env（廉价）
    if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    if (mVibEffect_ && effect) {
        env->CallVoidMethod(vibrator_, mVibEffect_, effect);
    } else if (mVibMs_) {
        env->CallVoidMethod(vibrator_, mVibMs_, (jlong)msFallback);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
}

} // namespace optic::ui
