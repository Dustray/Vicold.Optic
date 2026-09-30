// Vicold.Optic — NativeActivity 入口（GL UI + 权限 + 引擎驱动）
// UI 全部由 core/ui 的 GL 渲染器绘制（camera-ui.html 设计的 C++ 实现），
// 本文件只做生命周期/权限/输入接线，无 .java/.kt，无 View 层。
// 线程：glue 线程 = UI/渲染/输入；引擎线程 = 会话/拍摄（经队列通信）。

#include <android/log.h>
#include <android_native_app_glue.h>

#include <chrono>

#include "core/capture/CameraEngine.h"
#include "core/ui/Ui.h"

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

// 安全查找：GetMethodID/GetStaticMethodID 找不到时会抛 NoSuchMethodError（不是返回 null），
// 未清的 pending exception 会让下一次 JNI 调用直接 abort，所以每次查找后立即清理。
static jmethodID lookupMethod(JNIEnv* env, jclass cls, const char* name, const char* sig,
                              bool isStatic) {
    if (!cls) return nullptr;
    jmethodID m = isStatic ? env->GetStaticMethodID(cls, name, sig)
                           : env->GetMethodID(cls, name, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return m;
}

// 全屏沉浸：隐藏状态栏/导航栏 + 不让 decor 避让 insets。
// 否则 MIUI 在横屏下让窗口避让系统栏（实测一侧留 150px），cover 缩放会把快门裁掉一条。
// 注意：必须在 UI 线程调用（见 android_main 里的 onWindowFocusChanged 钩子）。
// statusBars()/navigationBars() 在嵌套类 android.view.WindowInsets$Type 上。
void hideSystemBars(android_app* app) {
    JNIEnv* env = nullptr;
    if (app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;

    jobject activity = app->activity->clazz;
    jclass actCls = env->GetObjectClass(activity);
    jmethodID getWindow = lookupMethod(env, actCls, "getWindow", "()Landroid/view/Window;", false);
    jobject window = getWindow ? env->CallObjectMethod(activity, getWindow) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(actCls);
    if (!window) return;

    jclass winCls = env->GetObjectClass(window);
    jmethodID setDecorFits =
        lookupMethod(env, winCls, "setDecorFitsSystemWindows", "(Z)V", false);
    LOGI("hideBars: window=%d setDecorFits=%d", window != nullptr, setDecorFits != nullptr);
    if (setDecorFits) {
        env->CallVoidMethod(window, setDecorFits, JNI_FALSE);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // 全出血双保险：主题里已声明 always，但部分 ROM（MIUI）解析主题时机不透明，
    // 再在运行时直接改 WindowManager.LayoutParams.layoutInDisplayCutoutMode = 3(ALWAYS)。
    {
        jmethodID getAttrs =
            lookupMethod(env, winCls, "getAttributes", "()Landroid/view/WindowManager$LayoutParams;", false);
        if (getAttrs) {
            jobject lp = env->CallObjectMethod(window, getAttrs);
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (lp) {
                jclass lpCls = env->GetObjectClass(lp);
                if (jfieldID fid = env->GetFieldID(lpCls, "layoutInDisplayCutoutMode", "I")) {
                    env->SetIntField(lp, fid, 3);   // LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                    if (jmethodID setAttrs = lookupMethod(env, winCls, "setAttributes",
                                                          "(Landroid/view/WindowManager$LayoutParams;)V", false)) {
                        env->CallVoidMethod(window, setAttrs, lp);
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                } else {
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                env->DeleteLocalRef(lpCls);
                env->DeleteLocalRef(lp);
            }
        }
    }

    jmethodID getCtrl =
        lookupMethod(env, winCls, "getInsetsController",
                     "()Landroid/view/WindowInsetsController;", false);
    jobject ctrl = getCtrl ? env->CallObjectMethod(window, getCtrl) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    LOGI("hideBars: ctrl=%d", ctrl != nullptr);
    env->DeleteLocalRef(winCls);
    if (ctrl) {
        jclass ctrlCls = env->GetObjectClass(ctrl);
        jclass typeCls = env->FindClass("android/view/WindowInsets$Type");
        if (env->ExceptionCheck()) env->ExceptionClear();
        jint types = 0;
        if (jmethodID m = lookupMethod(env, typeCls, "statusBars", "()I", true))
            types |= env->CallStaticIntMethod(typeCls, m);
        if (jmethodID m = lookupMethod(env, typeCls, "navigationBars", "()I", true))
            types |= env->CallStaticIntMethod(typeCls, m);
        // displayCutout：MIUI 横屏下为挖孔保留整条 150px（真机实测挖孔真身仅 74px 宽），
        // 不隐藏它窗口永远从 x=150 起，设计空间的左导轨无法左移
        if (jmethodID m = lookupMethod(env, typeCls, "displayCutout", "()I", true))
            types |= env->CallStaticIntMethod(typeCls, m);
        if (env->ExceptionCheck()) env->ExceptionClear();

        LOGI("hideBars: typeCls=%d types=%d", typeCls != nullptr, types);
        if (jmethodID hide = lookupMethod(env, ctrlCls, "hide", "(I)V", false); hide && types) {
            env->CallVoidMethod(ctrl, hide, types);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        // BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE：滑动时临时显示，不改变窗口布局
        if (jmethodID setBeh = lookupMethod(env, ctrlCls, "setSystemBarsBehavior", "(I)V", false)) {
            env->CallVoidMethod(ctrl, setBeh, 2);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (ctrlCls) env->DeleteLocalRef(ctrlCls);
        if (typeCls) env->DeleteLocalRef(typeCls);
        env->DeleteLocalRef(ctrl);
    }
    env->DeleteLocalRef(window);
}

// 屏幕旋转角：Surface.ROTATION_*（0/1/2/3），即"绘制图形相对机身自然方向顺时针转了 rot*90°"。
// 预览定向 = (displayRot*90 - sensorOrientation)/90 mod 4；取不到就退回几何自动判断。
// 优先 Activity.getDisplay()（API30+），失败再退 getWindowManager().getDefaultDisplay()。
int displayRotation(android_app* app) {
    JNIEnv* env = nullptr;
    if (!app || !app->activity || app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
        return -1;
    jobject activity = app->activity->clazz;
    jclass actCls = env->GetObjectClass(activity);
    jobject disp = nullptr;

    if (jmethodID m = lookupMethod(env, actCls, "getDisplay", "()Landroid/view/Display;", false)) {
        disp = env->CallObjectMethod(activity, m);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (!disp) {
        jmethodID gw =
            lookupMethod(env, actCls, "getWindowManager", "()Landroid/view/WindowManager;", false);
        if (gw) {
            jobject wm = env->CallObjectMethod(activity, gw);
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (wm) {
                jclass wmCls = env->GetObjectClass(wm);
                jmethodID gd = lookupMethod(env, wmCls, "getDefaultDisplay",
                                            "()Landroid/view/Display;", false);
                if (gd) {
                    disp = env->CallObjectMethod(wm, gd);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                env->DeleteLocalRef(wmCls);
                env->DeleteLocalRef(wm);
            }
        }
    }
    int rot = -1;
    if (disp) {
        jclass dCls = env->GetObjectClass(disp);
        if (jmethodID gr = lookupMethod(env, dCls, "getRotation", "()I", false)) {
            rot = env->CallIntMethod(disp, gr);
            if (env->ExceptionCheck()) { env->ExceptionClear(); rot = -1; }
        }
        env->DeleteLocalRef(dCls);
        env->DeleteLocalRef(disp);
    }
    env->DeleteLocalRef(actCls);
    LOGI("display rotation = %d", rot);
    return rot;
}

// 框架在 UI 线程回调 onWindowFocusChanged —— 在这里做系统栏隐藏最安全。
// 先挂系统栏钩子再转调 glue 原实现，保持 glue 的 GAINED/LOST_FOCUS 命令投递。
using FocusChangedFn = void (*)(ANativeActivity*, int);
FocusChangedFn g_glueFocusChanged = nullptr;

void hookWindowFocusChanged(ANativeActivity* activity, int focused) {
    if (focused) {
        android_app* app = static_cast<android_app*>(activity->instance);
        if (app) {
            LOGI("focus gained -> hide system bars");
            hideSystemBars(app);
        }
    }
    if (g_glueFocusChanged) g_glueFocusChanged(activity, focused);
}

struct AppState {
    optic::capture::CameraEngine engine;
    optic::ui::Ui ui;
};

int onInputEvent(android_app* app, AInputEvent* e) {
    auto* st = static_cast<AppState*>(app->userData);
    if (st) {
        st->ui.onInputEvent(e);
        return 1;
    }
    return 0;
}

void handleCommand(android_app* app, int32_t cmd) {
    auto* st = static_cast<AppState*>(app->userData);
    auto* engine = &st->engine;
    auto* ui = &st->ui;
    switch (cmd) {
        case APP_CMD_START: LOGI("APP_CMD_START"); break;
        case APP_CMD_RESUME: LOGI("APP_CMD_RESUME"); break;
        // 横屏两个方向（ROTATION_90/270）之间切换只发 CONFIG_CHANGED 时也要重取旋转角
        case APP_CMD_CONFIG_CHANGED:
            LOGI("APP_CMD_CONFIG_CHANGED");
            if (ui) ui->setDisplayRot(displayRotation(app));
            break;
        case APP_CMD_INIT_WINDOW:
            LOGI("APP_CMD_INIT_WINDOW surface=%p", (void*)app->window);
            ensurePermission(app);
            ui->setDataDir(app->activity->externalDataPath);   // attach 时要读 controls.txt
            ui->setDisplayRot(displayRotation(app));           // 预览定向（横屏两个方向都要对）
            if (ui && ui->attach(app->window)) {
                engine->setUi(ui);
                engine->start(app->activity->externalDataPath);
            }
            break;
        case APP_CMD_WINDOW_RESIZED: {
            // 全出血生效（displayCutout inset 隐藏后 MIUI 放开 150px 保留条）等场景：
            // 尺寸变了才重建 —— attach 会销毁旧预览 reader，引擎必须重启会话重挂新窗口
            const int w = ANativeWindow_getWidth(app->window);
            const int h = ANativeWindow_getHeight(app->window);
            LOGI("APP_CMD_WINDOW_RESIZED %dx%d (ui=%dx%d)", w, h,
                 ui ? ui->winW() : -1, ui ? ui->winH() : -1);
            if (ui && ui->attached() && (w != ui->winW() || h != ui->winH())) {
                engine->stop();
                if (ui->attach(app->window))
                    engine->start(app->activity->externalDataPath);
            }
            break;
        }
        case APP_CMD_TERM_WINDOW:
            LOGI("APP_CMD_TERM_WINDOW");
            engine->stop();
            if (ui) ui->detach();
            break;
        case APP_CMD_PAUSE:
            LOGI("APP_CMD_PAUSE");
            engine->stop();
            if (ui) ui->detach();
            break;
        case APP_CMD_DESTROY:
            LOGI("APP_CMD_DESTROY");
            engine->stop();
            if (ui) ui->detach();
            break;
        default: break;
    }
}

} // namespace

extern "C" void android_main(android_app* app) {
    LOGI("Vicold.Optic starting (native main)");
    static AppState state;
    app->userData = &state;
    app->onAppCmd = handleCommand;
    app->onInputEvent = onInputEvent;
    state.engine.setUi(&state.ui);
    state.ui.setJni(app->activity->vm, app->activity->clazz);   // 触感反馈（Vibrator）

    // 挂焦点钩子：框架在 UI 线程回调，正好用来隐藏系统栏（ glue 原实现转调保留）
    g_glueFocusChanged = app->activity->callbacks->onWindowFocusChanged;
    app->activity->callbacks->onWindowFocusChanged = hookWindowFocusChanged;

    // glue 主循环：UI 渲染 + 事件处理（vsync 由 eglSwapInterval 节奏控制）
    // 必须调用 source->process()：glue 以 NULL 回调注册 fd，pollOnce 只「报告」事件，
    // 真正的读管道 + 派发命令/输入在 process() 里。漏掉它 → 命令永不消费、fd 一直可读，
    // 表现为主线程永久卡在 NativeActivity.onStart*（glue 写完命令要等本线程消费）+ 100% CPU 自旋。
    while (app->destroyRequested == 0) {
        int events = 0;
        android_poll_source* source = nullptr;
        // 有窗口时非阻塞轮询（由 eglSwapBuffers 跟 vsync 限速）；无窗口时阻塞等事件
        int timeout = state.ui.attached() ? 0 : -1;
        int ident = ALooper_pollOnce(timeout, nullptr, &events,
                                     reinterpret_cast<void**>(&source));
        if (ident >= 0 && source != nullptr) source->process(app, source);
        if (state.ui.attached()) state.ui.frame();
    }
    LOGI("native main exit");
}
