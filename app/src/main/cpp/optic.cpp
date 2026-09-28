// Vicold.Optic — NativeActivity 入口（M0）
// 纯 C++ 壳：权限走 JNI 运行时申请，相机全部逻辑在 core/capture。

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
    return r == 0; // PackageManager.PERMISSION_GRANTED
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

// 阻塞申请权限（M0 简化：轮询；dev.sh 也会预先 pm grant）
void ensurePermission(android_app* app) {
    if (permissionGranted(app)) return;
    LOGI("requesting camera permission");
    requestPermission(app);
    for (int i = 0; i < 120 && !permissionGranted(app); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    LOGI("permission state after wait: %s", permissionGranted(app) ? "granted" : "DENIED");
}

void handleCommand(android_app* app, int32_t cmd) {
    auto* engine = static_cast<optic::capture::CameraEngine*>(app->userData);
    switch (cmd) {
        case APP_CMD_START:
            LOGI("APP_CMD_START");
            break;
        case APP_CMD_RESUME:
            LOGI("APP_CMD_RESUME");
            break;
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
            break;
        default:
            break;
    }
}

} // namespace

extern "C" void android_main(android_app* app) {
    LOGI("Vicold.Optic M0 starting (native main)");
    optic::capture::CameraEngine engine;
    app->userData = &engine;
    app->onAppCmd = handleCommand;

    android_poll_source* source = nullptr;
    int events = 0;
    while (app->destroyRequested == 0) {
        int ident = ALooper_pollOnce(engine.running() ? 100 : -1, nullptr, &events,
                                     reinterpret_cast<void**>(&source));
        if (ident >= 0 && source != nullptr) source->process(app, source);
    }
    LOGI("native main exit");
}
