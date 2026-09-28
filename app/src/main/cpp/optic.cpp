// Vicold.Optic — NativeActivity 入口（M1：JNI 拍摄按钮 + 权限 + 引擎驱动）
// 无 .java/.kt 源码；按钮经 JNI 叠加在 content FrameLayout 上。
// ★ View 操作必须在主线程：android_main 跑在 glue 工作线程（非 Looper），
//   直接 addView 会抛 "Animators may only be run on Looper threads"。
//   修法：覆盖 ANativeActivity 的 onWindowFocusChanged 回调（主线程执行），
//   hasFocus=true 时创建按钮，并链回 glue 原回调。
// 引擎线程轮询 isPressed() 取上升沿触发一次 burst（受每启动配额保护）。
 
#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>
 
#include <chrono>
#include <thread>
 
#include "core/capture/CameraEngine.h"
 
#define LOG_TAG "Optic"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
 
namespace {
 
constexpr const char* kPerm = "android.permission.CAMERA";
constexpr int kAndroidRIdContent = 16908290;   // android.R.id.content
constexpr int kGravityBottomCenterH = 0x51;    // BOTTOM | CENTER_HORIZONTAL
constexpr int kWrapContent = -2;
 
JavaVM* g_vm = nullptr;
jobject g_shutterBtn = nullptr;                // GlobalRef
ANativeActivityCallbacks g_origCallbacks{};    // glue 原回调（链回用）
 
JNIEnv* jniEnv() {
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    g_vm->AttachCurrentThread(&env, nullptr);
    return env;
}
 
// 取当前挂起异常的可读消息，用我们的 LOGE 打印（不依赖 stderr）
void logPendingException(JNIEnv* env, const char* where) {
    if (!env->ExceptionCheck()) return;
    jthrowable ex = env->ExceptionOccurred();
    env->ExceptionClear();
    const char* what = "unknown";
    jstring msg = nullptr;
    jclass throwableCls = env->FindClass("java/lang/Throwable");
    if (throwableCls) {
        jmethodID getMsg = env->GetMethodID(throwableCls, "getMessage", "()Ljava/lang/String;");
        if (getMsg) msg = static_cast<jstring>(env->CallObjectMethod(ex, getMsg));
    }
    if (msg) what = env->GetStringUTFChars(msg, nullptr);
    LOGE("btn: %s threw: %s", where, what);
    if (msg) { env->ReleaseStringUTFChars(msg, what); env->DeleteLocalRef(msg); }
    env->DeleteLocalRef(ex);
    if (throwableCls) env->DeleteLocalRef(throwableCls);
}
 
bool checkClear(JNIEnv* env, const char* where) {
    if (!env->ExceptionCheck()) return true;
    logPendingException(env, where);
    return false;
}
 
bool permissionGranted(android_app* app) {
    JNIEnv* env = nullptr;
    if (app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
    jobject activity = app->activity->clazz;
    jclass cls = env->GetObjectClass(activity);
    jmethodID check = env->GetMethodID(cls, "checkSelfPermission", "(Ljava/lang/String;)I");
    jstring perm = env->NewStringUTF(kPerm);
    jint r = env->CallIntMethod(activity, check, perm);
    env->DeleteLocalRef(perm);
    env->DeleteLocalRef(cls);
    return r == 0;
}
 
void requestPermission(android_app* app) {
    JNIEnv* env = nullptr;
    if (app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    jobject activity = app->activity->clazz;
    jclass cls = env->GetObjectClass(activity);
    jmethodID req = env->GetMethodID(cls, "requestPermissions", "([Ljava/lang/String;I)V");
    jstring perm = env->NewStringUTF(kPerm);
    jclass strCls = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray(1, strCls, perm);
    env->CallVoidMethod(activity, req, arr, 1);
    env->DeleteLocalRef(arr);
    env->DeleteLocalRef(strCls);
    env->DeleteLocalRef(perm);
    env->DeleteLocalRef(cls);
}
 
void ensurePermission(android_app* app) {
    if (permissionGranted(app)) return;
    LOGI("requesting camera permission");
    requestPermission(app);
    for (int i = 0; i < 120 && !permissionGranted(app); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    LOGI("permission after wait: %s", permissionGranted(app) ? "granted" : "DENIED");
}
 
// ★ 主线程执行（经 onWindowFocusChanged 回调）：创建按钮
void createShutterButton(ANativeActivity* activity) {
    if (g_shutterBtn) return;
    JNIEnv* env = jniEnv();
    env->ExceptionClear();
 
    jclass btnCls = env->FindClass("android/widget/Button");
    if (!checkClear(env, "FindClass Button")) return;
    jmethodID ctor = env->GetMethodID(btnCls, "<init>", "(Landroid/content/Context;)V");
    jobject btn = env->NewObject(btnCls, ctor, activity->clazz);
    if (!checkClear(env, "NewObject Button")) return;
 
    jmethodID setText = env->GetMethodID(btnCls, "setText", "(Ljava/lang/CharSequence;)V");
    jstring label = env->NewStringUTF("拍摄 RAW");
    env->CallVoidMethod(btn, setText, label);
    env->DeleteLocalRef(label);
    if (!checkClear(env, "setText")) return;
 
    jclass viewCls = env->FindClass("android/view/View");
    jmethodID setBg = env->GetMethodID(viewCls, "setBackgroundColor", "(I)V");
    env->CallVoidMethod(btn, setBg, static_cast<jint>(0xFFFF0000));   // 调试红底
    if (!checkClear(env, "setBackgroundColor")) return;
 
    jmethodID getWindow = env->GetMethodID(env->GetObjectClass(activity->clazz),
                                           "getWindow", "()Landroid/view/Window;");
    jobject window = env->CallObjectMethod(activity->clazz, getWindow);
    jmethodID getDecor = env->GetMethodID(env->GetObjectClass(window),
                                          "getDecorView", "()Landroid/view/View;");
    jobject decor = env->CallObjectMethod(window, getDecor);
    if (!checkClear(env, "getWindow/getDecorView")) return;
 
    // 子窗口方案：按钮挂独立 surface，合成在相机预览之上（主窗口 surface 被相机直写覆盖）
    jmethodID getToken = env->GetMethodID(viewCls, "getWindowToken", "()Landroid/os/IBinder;");
    jobject token = env->CallObjectMethod(decor, getToken);
    if (!checkClear(env, "getWindowToken")) return;
    if (!token) { LOGE("btn: window token not ready"); return; }

    jclass ctxCls = env->FindClass("android/content/Context");
    jfieldID fWs = env->GetStaticFieldID(ctxCls, "WINDOW_SERVICE", "Ljava/lang/String;");
    jstring wsName = static_cast<jstring>(env->GetStaticObjectField(ctxCls, fWs));
    jmethodID getSysSvc = env->GetMethodID(env->GetObjectClass(activity->clazz),
        "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject wm = env->CallObjectMethod(activity->clazz, getSysSvc, wsName);
    if (!checkClear(env, "getSystemService(WINDOW_SERVICE)")) return;
    if (!wm) { LOGE("btn: window manager not found"); return; }

    jclass lpCls = env->FindClass("android/view/WindowManager$LayoutParams");
    if (!checkClear(env, "FindClass WM$LayoutParams") || !lpCls) { logPendingException(env, "FindClass WM$LayoutParams"); return; }
    jmethodID lpCtor = env->GetMethodID(lpCls, "<init>", "(IIIII)V");   // (w, h, type, flags, format)
    if (!lpCtor || !checkClear(env, "GetMethodID LayoutParams<init>")) { logPendingException(env, "lpCtor lookup"); return; }
    // w=WRAP_CONTENT, h=WRAP_CONTENT, type=TYPE_APPLICATION_PANEL(1000), flags=0, format=TRANSLUCENT(-3)
    jobject lp = env->NewObject(lpCls, lpCtor, kWrapContent, kWrapContent, 1000, 0, -3);
    if (!checkClear(env, "NewObject LayoutParams")) return;

    jfieldID gravityField = env->GetFieldID(lpCls, "gravity", "I");
    if (!gravityField || !checkClear(env, "gravity field")) { logPendingException(env, "gravity field"); return; }
    env->SetIntField(lp, gravityField, kGravityBottomCenterH);
    jfieldID yField = env->GetFieldID(lpCls, "y", "I");
    if (!yField || !checkClear(env, "y field")) { logPendingException(env, "y field"); return; }
    env->SetIntField(lp, yField, 200);   // gravity=BOTTOM 时 y 为自底向上的偏移

    jfieldID tokenField = env->GetFieldID(lpCls, "token", "Landroid/os/IBinder;");
    if (!tokenField || !checkClear(env, "GetFieldID token")) { logPendingException(env, "token field"); return; }
    env->SetObjectField(lp, tokenField, token);
    if (!checkClear(env, "LayoutParams.token")) return;

    jmethodID wmAddView = env->GetMethodID(env->GetObjectClass(wm), "addView",
        "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V");
    env->CallVoidMethod(wm, wmAddView, btn, lp);
    if (!checkClear(env, "wm.addView")) return;
 
    jmethodID bringFront = env->GetMethodID(viewCls, "bringToFront", "()V");
    env->CallVoidMethod(btn, bringFront);
    jmethodID setElev = env->GetMethodID(viewCls, "setElevation", "(F)V");
    env->CallVoidMethod(btn, setElev, 24.0f);
    if (!checkClear(env, "bringToFront/setElevation")) return;
 
    g_shutterBtn = env->NewGlobalRef(btn);
    LOGI("shutter button created");
}
 
void destroyShutterButton(android_app* app) {
    if (!g_shutterBtn) return;
    JNIEnv* env = jniEnv();
    jclass viewCls = env->FindClass("android/view/View");
    jmethodID getParent = env->GetMethodID(viewCls, "getParent", "()Landroid/view/ViewParent;");
    jobject parent = env->CallObjectMethod(g_shutterBtn, getParent);
    if (parent) {
        jmethodID removeView = env->GetMethodID(env->GetObjectClass(parent), "removeView", "(Landroid/view/View;)V");
        env->CallVoidMethod(parent, removeView, g_shutterBtn);
        env->DeleteLocalRef(parent);
    }
    checkClear(env, "removeView");   // 非 Iooper 线程可能抛，容忍（进程即将退出）
    env->DeleteGlobalRef(g_shutterBtn);
    g_shutterBtn = nullptr;
    LOGI("shutter button removed");
}
 
// 引擎线程轮询：按钮是否被按住（上升沿判定在引擎内）
bool shutterPressed() {
    if (!g_shutterBtn) return false;
    JNIEnv* env = jniEnv();
    jclass cls = env->GetObjectClass(g_shutterBtn);
    jmethodID isPressed = env->GetMethodID(cls, "isPressed", "()Z");
    return env->CallBooleanMethod(g_shutterBtn, isPressed);
}
 
// ★ 主线程回调（覆盖 glue 的 onWindowFocusChanged）：窗口聚焦时创建按钮
void onWindowFocusChanged(ANativeActivity* activity, int hasFocus) {
    if (g_origCallbacks.onWindowFocusChanged)
        g_origCallbacks.onWindowFocusChanged(activity, hasFocus);   // 链回 glue
    if (g_vm && hasFocus && !g_shutterBtn) createShutterButton(activity);
}
 
void handleCommand(android_app* app, int32_t cmd) {
    auto* engine = static_cast<optic::capture::CameraEngine*>(app->userData);
    switch (cmd) {
        case APP_CMD_START: LOGI("APP_CMD_START"); break;
        case APP_CMD_RESUME: LOGI("APP_CMD_RESUME"); break;
        case APP_CMD_INIT_WINDOW:
            LOGI("APP_CMD_INIT_WINDOW surface=%p", (void*)app->window);
            ensurePermission(app);
            engine->start(app->window, app->activity->externalDataPath);
            break;
        case APP_CMD_TERM_WINDOW:
            LOGI("APP_CMD_TERM_WINDOW");
            engine->stop();
            break;
        case APP_CMD_PAUSE:
            LOGI("APP_CMD_PAUSE");
            engine->stop();
            break;
        case APP_CMD_DESTROY:
            LOGI("APP_CMD_DESTROY");
            engine->stop();
            destroyShutterButton(app);
            break;
        default: break;
    }
}
 
} // namespace
 
extern "C" void android_main(android_app* app) {
    LOGI("Vicold.Optic starting (native main)");
    g_vm = app->activity->vm;                     // JNI 句柄必须最先设
    optic::capture::CameraEngine engine;
    engine.setTriggerPoll(shutterPressed);
    app->userData = &engine;
    app->onAppCmd = handleCommand;
 
    // 覆盖主线程回调（glue 已在 onCreate 设置过自己的，先存后覆盖以链回）
    g_origCallbacks = *app->activity->callbacks;
    app->activity->callbacks->onWindowFocusChanged = onWindowFocusChanged;
 
    android_poll_source* source = nullptr;
    int events = 0;
    while (app->destroyRequested == 0) {
        int ident = ALooper_pollOnce(engine.running() ? 100 : -1, nullptr, &events,
                                     reinterpret_cast<void**>(&source));
        if (ident >= 0 && source != nullptr) source->process(app, source);
    }
    LOGI("native main exit");
}