#include "core/ui/Gl.h"

// 单文件头库：实现只在本 TU 展开一次
#define STB_TRUETYPE_IMPLEMENTATION 1
#include "core/ui/stb_truetype.h"

#include "core/util/Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

namespace optic::ui {

Gl::~Gl() = default;

// ---------- 着色器 ----------

// 注意：90° 旋转必须在顶点着色器里做。
// 之前在 CPU 侧用「swap(u0,v0) + 取反」改 uUv 是错的：u0/v0 本来都是 0、u1/v1 都是 1，
// swap 等于没做，结果只翻转了一个分量 → 另一分量退化成常量 → 整幅图被拉成一条带（"一维拉丝"）。
// 旋转是各轴耦合的变换，只有在这里显式写 t→r 的映射才表达得出来。
// uCrop 是 cover 裁切（屏幕域、中心对齐），必须在旋转之前应用。
static const char* kVsPreview =
    "attribute vec2 aPos;\n"
    "uniform vec4 uRect;\n"
    "uniform vec2 uViewport;\n"
    "uniform vec4 uUv;\n"
    "uniform vec2 uCrop;\n"
    "uniform float uRot;\n"
    "varying vec2 vUv;\n"
    "void main(){\n"
    "  vec2 t = aPos*0.5 + 0.5;\n"
    "  vec2 p = uRect.xy + t*uRect.zw;\n"
    "  gl_Position = vec4(p.x/uViewport.x*2.0-1.0, 1.0-p.y/uViewport.y*2.0, 0.0, 1.0);\n"
    "  t = (t - 0.5)*uCrop + 0.5;\n"
    "  vec2 r;\n"
    "  if (uRot < 0.5)      r = t;\n"
    "  else if (uRot < 1.5) r = vec2(1.0 - t.y, t.x);\n"
    "  else if (uRot < 2.5) r = vec2(1.0 - t.x, 1.0 - t.y);\n"
    "  else                 r = vec2(t.y, 1.0 - t.x);\n"
    "  vUv = mix(uUv.xy, uUv.zw, r);\n"
    "}\n";

// 预览纹理是相机 YUV buffer 导入的 EGLImage，必须用 external OES 采样器（硬件做 YUV→RGB）。
// vUv 已经在顶点着色器里完成裁切+旋转+窗口映射，这里不能再 mix 一次。
static const char* kFsPreview =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES uTex;\n"
    "uniform float uAlpha;\n"
    "varying vec2 vUv;\n"
    "void main(){ gl_FragColor = uAlpha * texture2D(uTex, vUv); }\n";

static const char* kVsRect =
    "attribute vec2 aPos;\n"
    "uniform vec4 uQuad;\n"
    "uniform vec2 uViewport;\n"
    "uniform vec2 uCenter;\n"
    "uniform vec2 uHalf;\n"
    "varying vec2 vOff;\n"
    "void main(){\n"
    "  vec2 p = uQuad.xy + (aPos*0.5+0.5)*uQuad.zw;\n"
    "  gl_Position = vec4(p.x/uViewport.x*2.0-1.0, 1.0-p.y/uViewport.y*2.0, 0.0, 1.0);\n"
    "  vOff = p - uCenter;\n"
    "}\n";

static const char* kFsRect =
    "precision mediump float;\n"
    "varying vec2 vOff;\n"
    "uniform vec2 uHalf;\n"
    "uniform float uRadius;\n"
    "uniform vec4 uFill;\n"
    "uniform vec4 uBorder;\n"
    "uniform float uBorderW;\n"
    "float sdBox(vec2 p, vec2 b, float r){\n"
    "  vec2 d = abs(p) - b + r;\n"
    "  return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;\n"
    "}\n"
    "void main(){\n"
    "  float d = sdBox(vOff, uHalf, uRadius);\n"
    "  float aa = 1.0;\n"
    "  float fillA = uFill.a * clamp(0.5 - d / aa, 0.0, 1.0);\n"
    "  float bw = clamp(uBorderW, 0.0, 1e6);\n"
    "  float borderA = uBorder.a * (1.0 - clamp((abs(d) - bw) / aa + 0.5, 0.0, 1.0));\n"
    "  vec4 c = mix(uFill, uBorder, clamp(borderA, 0.0, 1.0));\n"
    "  float a = clamp(fillA + (1.0 - fillA) * borderA, 0.0, 1.0);\n"
    "  gl_FragColor = vec4(c.rgb, a);\n"
    "}\n";

static const char* kVsText =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUv;\n"
    "uniform vec2 uViewport;\n"
    "varying vec2 vUv;\n"
    "void main(){\n"
    "  gl_Position = vec4(aPos.x/uViewport.x*2.0-1.0, 1.0-aPos.y/uViewport.y*2.0, 0.0, 1.0);\n"
    "  vUv = aUv;\n"
    "}\n";

static const char* kFsText =
    "precision mediump float;\n"
    "uniform sampler2D uAtlas;\n"
    "uniform vec4 uColor;\n"
    "varying vec2 vUv;\n"
    "void main(){ gl_FragColor = vec4(uColor.rgb, uColor.a * texture2D(uAtlas, vUv).a); }\n";

// 纯色批：直接吃像素坐标，用于网格线/直方图柱等批量图元
static const char* kVsSolid =
    "attribute vec2 aPos;\n"
    "uniform vec2 uViewport;\n"
    "void main(){ gl_Position = vec4(aPos.x/uViewport.x*2.0-1.0, 1.0-aPos.y/uViewport.y*2.0, 0.0, 1.0); }\n";

static const char* kFsSolid =
    "precision mediump float;\n"
    "uniform vec4 uColor;\n"
    "void main(){ gl_FragColor = uColor; }\n";

// ---------- 编译工具 ----------

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint program(const char* vs, const char* fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(v);
    glDeleteShader(f);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        LOGE("program link failed: %s", log);
        return 0;
    }
    return p;
}

namespace {
const GLfloat kQuad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};   // 三角带

// 直方图降采样着色器：全屏取样到小 FBO。预览纹理是 EXTERNAL_OES（EGLImage 导入）。
const char* kVsHist =
    "attribute vec2 aPos;\n"
    "varying vec2 vUv;\n"
    "void main(){ vUv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
const char* kFsHist =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES uTex;\n"
    "varying vec2 vUv;\n"
    "void main(){ gl_FragColor = vec4(texture2D(uTex, vUv).rgb, 1.0); }\n";
// 覆盖层合成：把离屏 RGBA 纹理全屏铺到窗口（1:1 像素对齐）
const char* kVsBlit =
    "attribute vec2 aPos;\n"
    "varying vec2 vUv;\n"
    "void main(){ vUv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
const char* kFsBlit =
    "precision mediump float;\n"
    "uniform sampler2D uTex;\n"
    "varying vec2 vUv;\n"
    "void main(){ gl_FragColor = texture2D(uTex, vUv); }\n";
} // namespace

// ---- Gl 接口实现 ----

bool Gl::attach(ANativeWindow* win) {
    // 同窗口但尺寸变化（如 displayCutout inset 生效后全出血）也要重建：视口必须跟上
    if (ready() && win_ == win && winW_ == ANativeWindow_getWidth(win) &&
        winH_ == ANativeWindow_getHeight(win))
        return true;
    detach();
    win_ = win;
    winW_ = ANativeWindow_getWidth(win);
    winH_ = ANativeWindow_getHeight(win);

    impl_.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (impl_.dpy == EGL_NO_DISPLAY) { LOGE("eglGetDisplay failed"); return false; }
    if (!eglInitialize(impl_.dpy, nullptr, nullptr)) { LOGE("eglInitialize failed"); return false; }
    EGLint cfgAttrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                         EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                         EGL_ALPHA_SIZE, 0, EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE};
    EGLConfig cfg = nullptr;
    EGLint n = 0;
    if (!eglChooseConfig(impl_.dpy, cfgAttrs, &cfg, 1, &n) || n < 1) {
        LOGE("eglChooseConfig failed");
        return false;
    }
    // 别试 EGL_SWAP_BEHAVIOR / EGL_BUFFER_DESTROYED：理论上能省掉每 swap 拷回上一帧的
    // 13MB 读写，但 2026-10-01 真机实测**该属性会让 eglCreateWindowSurface 直接失败**
    //（Adreno 对 window surface 不接受覆盖 swap behavior）。保持默认最小值。
    EGLint surfAttrs[] = {EGL_NONE};
    impl_.surf = eglCreateWindowSurface(impl_.dpy, cfg, win, surfAttrs);
    if (impl_.surf == EGL_NO_SURFACE) { LOGE("eglCreateWindowSurface failed"); return false; }
    EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    impl_.ctx = eglCreateContext(impl_.dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
    if (impl_.ctx == EGL_NO_CONTEXT) { LOGE("eglCreateContext failed"); return false; }
    if (!eglMakeCurrent(impl_.dpy, impl_.surf, impl_.surf, impl_.ctx)) {
        LOGE("eglMakeCurrent failed");
        return false;
    }
    eglSwapInterval(impl_.dpy, 1);

    impl_.progPreview = program(kVsPreview, kFsPreview);
    impl_.progRect = program(kVsRect, kFsRect);
    impl_.progText = program(kVsText, kFsText);
    impl_.progSolid = program(kVsSolid, kFsSolid);
    impl_.progBlit_ = program(kVsBlit, kFsBlit);
    if (!impl_.progPreview || !impl_.progRect || !impl_.progText || !impl_.progSolid ||
        !impl_.progBlit_)
        return false;
    impl_.aPv = glGetAttribLocation(impl_.progPreview, "aPos");
    impl_.aRect = glGetAttribLocation(impl_.progRect, "aPos");
    impl_.aText = glGetAttribLocation(impl_.progText, "aPos");
    impl_.aUv = glGetAttribLocation(impl_.progText, "aUv");
    impl_.aSolid = glGetAttribLocation(impl_.progSolid, "aPos");
    impl_.aBlit_ = glGetAttribLocation(impl_.progBlit_, "aPos");
    impl_.uBlitTex_ = glGetUniformLocation(impl_.progBlit_, "uTex");
    impl_.uPv.rect = glGetUniformLocation(impl_.progPreview, "uRect");
    impl_.uPv.viewport = glGetUniformLocation(impl_.progPreview, "uViewport");
    impl_.uPv.uv = glGetUniformLocation(impl_.progPreview, "uUv");
    impl_.uPv.tex = glGetUniformLocation(impl_.progPreview, "uTex");
    impl_.uPv.rot = glGetUniformLocation(impl_.progPreview, "uRot");
    impl_.uPv.crop = glGetUniformLocation(impl_.progPreview, "uCrop");
    impl_.uPv.alpha = glGetUniformLocation(impl_.progPreview, "uAlpha");
    impl_.uRect.viewport = glGetUniformLocation(impl_.progRect, "uViewport");
    impl_.uRect.quad = glGetUniformLocation(impl_.progRect, "uQuad");
    impl_.uRect.center = glGetUniformLocation(impl_.progRect, "uCenter");
    impl_.uRect.half = glGetUniformLocation(impl_.progRect, "uHalf");
    impl_.uRect.radius = glGetUniformLocation(impl_.progRect, "uRadius");
    impl_.uRect.fill = glGetUniformLocation(impl_.progRect, "uFill");
    impl_.uRect.border = glGetUniformLocation(impl_.progRect, "uBorder");
    impl_.uRect.borderW = glGetUniformLocation(impl_.progRect, "uBorderW");
    impl_.uText.viewport = glGetUniformLocation(impl_.progText, "uViewport");
    impl_.uText.color = glGetUniformLocation(impl_.progText, "uColor");
    impl_.uSolid.viewport = glGetUniformLocation(impl_.progSolid, "uViewport");
    impl_.uSolid.color = glGetUniformLocation(impl_.progSolid, "uColor");

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, winW_, winH_);
    impl_.fontOk = false;
    return true;
}

void Gl::detach() {
    if (impl_.dpy != EGL_NO_DISPLAY) {
        for (int i = 0; i < kSrcN; ++i) {
            Source& s = src_[i];
            {
                std::lock_guard<std::mutex> l(s.pendM);
                if (s.pending) AImage_delete(s.pending);
                s.pending = nullptr;
            }
            for (auto& t : s.pv) {
                if (t.tex) glDeleteTextures(1, &t.tex);
                if (t.eglImg != EGL_NO_IMAGE_KHR) eglDestroyImageKHR(impl_.dpy, t.eglImg);
                if (t.ahb) AHardwareBuffer_release(t.ahb);
            }
            if (s.reader) {
                AImageReader_setImageListener(s.reader, nullptr);
                AImageReader_delete(s.reader);
            }
            // Source 含 std::mutex 不可整体赋值，逐字段复位
            s.reader = nullptr;
            s.win = nullptr;
            s.w = s.h = 0;
            s.frameNo = -1;
            s.logged = false;
            for (auto& t : s.pv) t = {};
            s.lastTex = 0;
            s.listener = {};
            s.pending = nullptr;
            s.histValid = false;
            s.lastHistT = 0;
        }
        if (impl_.atlasTex) glDeleteTextures(1, &impl_.atlasTex);
        if (impl_.progPreview) glDeleteProgram(impl_.progPreview);
        if (impl_.progRect) glDeleteProgram(impl_.progRect);
        if (impl_.progText) glDeleteProgram(impl_.progText);
        if (impl_.progSolid) glDeleteProgram(impl_.progSolid);
        if (impl_.progBlit_) glDeleteProgram(impl_.progBlit_);
        if (impl_.overlayFbo_) glDeleteFramebuffers(1, &impl_.overlayFbo_);
        if (impl_.overlayTex_) glDeleteTextures(1, &impl_.overlayTex_);
        impl_.overlayFbo_ = impl_.overlayTex_ = 0;
        impl_.overlayW_ = impl_.overlayH_ = 0;
        if (histFbo_) glDeleteFramebuffers(1, &histFbo_);
        if (histTex_) glDeleteTextures(1, &histTex_);
        if (progHist_) glDeleteProgram(progHist_);
        histFbo_ = histTex_ = progHist_ = 0;
        aHist_ = uHistTex_ = -1;
        eglMakeCurrent(impl_.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (impl_.surf != EGL_NO_SURFACE) eglDestroySurface(impl_.dpy, impl_.surf);
        if (impl_.ctx != EGL_NO_CONTEXT) eglDestroyContext(impl_.dpy, impl_.ctx);
        eglTerminate(impl_.dpy);
    }
    // Impl 含 std::mutex 不可整体赋值，逐字段复位
    impl_.dpy = EGL_NO_DISPLAY;
    impl_.surf = EGL_NO_SURFACE;
    impl_.ctx = EGL_NO_CONTEXT;
    impl_.progPreview = impl_.progRect = impl_.progText = impl_.progSolid = impl_.progBlit_ = 0;
    impl_.uPv = {};
    impl_.uRect = {};
    impl_.uText = {};
    impl_.uSolid = {};
    impl_.aPv = impl_.aRect = impl_.aText = impl_.aUv = impl_.aSolid = -1;
    impl_.atlasTex = 0;
    impl_.atlasW = impl_.atlasH = 0;
    impl_.bakedPx = 0;
    impl_.glyphs.clear();
    impl_.ascentPx = 0;
    impl_.lastTex = 0;
    impl_.fontOk = false;
    win_ = nullptr;
}

bool Gl::ready() const { return impl_.dpy != EGL_NO_DISPLAY && impl_.surf != EGL_NO_SURFACE; }
int32_t Gl::width() const { return winW_; }
int32_t Gl::height() const { return winH_; }

bool Gl::makePreviewSource(int slot, int32_t w, int32_t h, int32_t fmt) {
    if (!ready()) return false;
    if (slot < 0 || slot >= kSrcN) return false;
    Source& s = src_[slot];
    if (s.reader) return true;
    // 格式由机型层决定（默认 PRIVATE）：多数 HAL 只对 PRIVATE(+GPU_SAMPLED_IMAGE)
    // → EGLImage → samplerExternalOES 这条零拷贝路径稳定供帧，申请 YUV/RGBA 有不
    // 出帧的先例。maxImages=3（acquireLatest 语义只需浅队列）；三路源 ×3 buffer 控制内存。
    uint64_t usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    if (AImageReader_newWithUsage(w, h, fmt, usage, 3, &s.reader) != AMEDIA_OK || !s.reader) {
        LOGE("preview AImageReader_new failed (slot=%d fmt=0x%x)", slot, fmt);
        return false;
    }

    // 关键：本机实测必须在回调线程里 acquire，否则轮询 acquire 恒 NO_BUFFER_AVAILABLE
    s.gl = this;
    s.listener = {&s, &Gl::onPreviewAvailable};
    if (AImageReader_setImageListener(s.reader, &s.listener) != AMEDIA_OK) {
        LOGE("preview setImageListener failed (slot=%d)", slot);
        return false;
    }
    if (AImageReader_getWindow(s.reader, &s.win) != AMEDIA_OK || !s.win) {
        LOGE("preview reader window failed (slot=%d)", slot);
        return false;
    }
    s.w = w;
    s.h = h;
    LOGI("preview source ready: slot=%d %dx%d fmt=0x%x GPU", slot, w, h, fmt);
    return true;
}

ANativeWindow* Gl::previewWindow(int slot) {
    if (slot < 0 || slot >= kSrcN) return nullptr;
    return src_[slot].win;
}
int32_t Gl::previewW(int slot) const {
    return (slot >= 0 && slot < kSrcN) ? src_[slot].w : 0;
}
int32_t Gl::previewH(int slot) const {
    return (slot >= 0 && slot < kSrcN) ? src_[slot].h : 0;
}

void Gl::onPreviewAvailable(void* ctx, AImageReader* reader) {
    auto* s = static_cast<Source*>(ctx);
    if (!s || !reader || !s->gl) return;

    // 回调线程取最新帧；旧 pending 直接丢弃（等价 acquireLatest 语义）
    AImage* img = nullptr;
    if (AImageReader_acquireLatestImage(reader, &img) != AMEDIA_OK || !img) return;
    std::lock_guard<std::mutex> l(s->pendM);
    if (s->pending) AImage_delete(s->pending);
    s->pending = img;
}

void Gl::importPreviewImage(Source& s, AImage* img) {
    AHardwareBuffer* ahb = nullptr;
    if (AImage_getHardwareBuffer(img, &ahb) != AMEDIA_OK || !ahb) return;

    // 缓存：同一 AHardwareBuffer 循环复用（maxImages=3）
    for (auto& t : s.pv) {
        if (t.ahb == ahb) {
            if (t.tex) glBindTexture(GL_TEXTURE_EXTERNAL_OES, t.tex);
            s.lastTex = t.tex;
            return;
        }
    }
    Source::PvTex* slot = nullptr;
    for (auto& t : s.pv)
        if (!t.ahb) { slot = &t; break; }
    if (!slot) slot = &s.pv[0];
    if (slot->tex) glDeleteTextures(1, &slot->tex);
    if (slot->eglImg != EGL_NO_IMAGE_KHR) eglDestroyImageKHR(impl_.dpy, slot->eglImg);
    if (slot->ahb) AHardwareBuffer_release(slot->ahb);
    *slot = {};

    // 纹理要在 AImage_delete 之后继续使用，必须自己持一份引用
    AHardwareBuffer_acquire(ahb);

    EGLClientBuffer cb = eglGetNativeClientBufferANDROID(ahb);
    if (!cb) {
        LOGE("eglGetNativeClientBuffer failed");
        AHardwareBuffer_release(ahb);
        return;
    }
    slot->eglImg = eglCreateImageKHR(impl_.dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, nullptr);
    if (slot->eglImg == EGL_NO_IMAGE_KHR) {
        LOGE("eglCreateImageKHR failed");
        AHardwareBuffer_release(ahb);
        return;
    }
    glGenTextures(1, &slot->tex);
    // YUV EGLImage 只能导入 EXTERNAL_OES 目标（导 TEXTURE_2D 未定义）
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, slot->tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, slot->eglImg);
    slot->ahb = ahb;
    s.lastTex = slot->tex;
}

bool Gl::acquirePreview(int slot, bool wantHist) {
    if (!ready() || slot < 0 || slot >= kSrcN) return false;
    Source& s = src_[slot];
    if (!s.reader) return false;

    // 取回调线程攒下的最新帧；无新帧时保持上一帧纹理与直方图不动
    AImage* img = nullptr;
    {
        std::lock_guard<std::mutex> l(s.pendM);
        img = s.pending;
        s.pending = nullptr;
    }
    if (!img) return false;

    importPreviewImage(s, img);

    // az 时间戳对齐：用**本帧**的 SENSOR_TIMESTAMP 查引擎结果环，把当时的 appliedZoom
    // 写回 UI —— UI 的 crop = zoom_/az 与显示帧严格同源，显示 FOV = az×crop ≡ zoom_，
    // 与相机 120ms 节流、管线 1-2 帧延迟全部解耦（1–5x 快拖泵动的根治）。
    if (azSrc_) {
        int64_t ts = 0;
        if (AImage_getTimestamp(img, &ts) == AMEDIA_OK && ts > 0) azSrc_(slot, ts);
    }

    AImage_delete(img);
    ++s.frameNo;

    // 直方图统计：预览软件頻 RGBA 取不到（YUV_420_888 才行 AImage_getPlaneData），
    // CPU 直读是死路 —— 走 GL 降采样（纹理 → 小 FBO → readPixels → 64-bin）。
    // 只对显示源开启 + 机型无关的固定节流（见 Gl.h 的 kHistPeriodSec 注释）：
    // readPixels 同步 stall 管线，逐帧全开会掉帧。
    // 注意在 AImage_delete 之后执行（读回期间不占用 reader 的 maxImages 配额）。
    if (wantHist) {
        const double now =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        if (now - s.lastHistT >= kHistPeriodSec) {
            sampleHistogram(s);
            s.lastHistT = now;
        }
    }
    return true;
}

bool Gl::ensureHistRt() {
    if (histFbo_) return true;
    glGenTextures(1, &histTex_);
    glBindTexture(GL_TEXTURE_2D, histTex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kHistW, kHistH, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
    glGenFramebuffers(1, &histFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, histFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, histTex_, 0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!complete) {
        LOGW("hist FBO incomplete");
        glDeleteFramebuffers(1, &histFbo_);
        glDeleteTextures(1, &histTex_);
        histFbo_ = histTex_ = 0;
        return false;
    }
    progHist_ = program(kVsHist, kFsHist);
    if (!progHist_) return false;
    aHist_ = glGetAttribLocation(progHist_, "aPos");
    uHistTex_ = glGetUniformLocation(progHist_, "uTex");
    return aHist_ >= 0 && uHistTex_ >= 0;
}

void Gl::sampleHistogram(Source& s) {
    if (!s.lastTex || !ensureHistRt()) return;

    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, histFbo_);
    glViewport(0, 0, kHistW, kHistH);
    glUseProgram(progHist_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, s.lastTex);
    glUniform1i(uHistTex_, 0);
    glEnableVertexAttribArray(aHist_);
    glVertexAttribPointer(aHist_, 2, GL_FLOAT, GL_FALSE, 0, kQuad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(aHist_);

    static uint8_t buf[kHistW * kHistH * 4];     // 渲染线程独占，静态省栈
    glReadPixels(0, 0, kHistW, kHistH, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glViewport(0, 0, winW_, winH_);              // 恢复（beginFrame 每帧也会设）

    std::memset(s.hist, 0, sizeof(s.hist));
    for (int i = 0; i < kHistW * kHistH; ++i) {
        const uint8_t* p = buf + i * 4;
        ++s.hist[0][p[0] >> 2];
        ++s.hist[1][p[1] >> 2];
        ++s.hist[2][p[2] >> 2];
    }
    s.histValid = true;
    s.histNew = true;                                // 标记新样本（UI 据此重烤覆盖层）
}

void Gl::copyHistogram(int slot, int32_t outR[64], int32_t outG[64], int32_t outB[64]) const {
    if (slot < 0 || slot >= kSrcN) return;
    const Source& s = src_[slot];
    if (!s.histValid) return;
    std::memcpy(outR, s.hist[0], 64 * sizeof(int32_t));
    std::memcpy(outG, s.hist[1], 64 * sizeof(int32_t));
    std::memcpy(outB, s.hist[2], 64 * sizeof(int32_t));
}

bool Gl::bakeFont(float bakedPx, const std::vector<std::string>& fonts) {
    if (!ready()) return false;
    if (fonts.empty()) { LOGE("font: no candidates supplied"); return false; }

    // 先构建需要光栅化的字符集（不依赖字体，可提前用于覆盖率统计）
    std::set<uint32_t> cps;
    for (uint32_t c = 0x20; c <= 0x7E; ++c) cps.insert(c);
    // UI 实际用到的全部非 ASCII 字符（字形按需增删，见 Ui.cpp 文案）。
    // 2026-10-01 补快门拒绝文案用字：配额满此段不支持相机未失败
    //（此前"配额已满"的 配/额/满 三字就一直缺失渲染成空位，因角标罕见未被发现）。
    const std::string extra =
        "曝光时间补偿就绪已保存环形缓冲摄像头区域对焦拍摄配额满此段不支持相机未失败×·";
    for (size_t i = 0; i < extra.size();) {
        uint32_t cp = uint8_t(extra[i]);
        int n = 1;
        if ((cp & 0xE0) == 0xC0) { cp &= 0x1F; n = 2; }
        else if ((cp & 0xF0) == 0xE0) { cp &= 0x0F; n = 3; }
        else if ((cp & 0xF8) == 0xF0) { cp &= 0x07; n = 4; }
        for (int k = 1; k < n && i + k < extra.size(); ++k) cp = (cp << 6) | (uint8_t(extra[i + k]) & 0x3F);
        cps.insert(cp);
        i += n;
    }
    int cjkNeed = 0;
    for (uint32_t c : cps) if (c >= 0x4E00 && c <= 0x9FFF) ++cjkNeed;

    // 候选字体来自机型层（fonts 参数）：不同 ROM 的中文字体名/格式差异巨大，无法探测，
    // 只能按机型列举。逐个探测，选**静态 TrueType(glyf)** 且中文覆盖率最高的那一个
    //（MiSansVF 可变字体 stb 不支持 gvar→连笔；NotoSansCJK ttc 是 CFF 轮廓 InitFont 失败）。
    std::vector<uint8_t> font;
    const char* used = nullptr;
    int bestCov = -1;
    for (const std::string& p : fonts) {
        std::ifstream f(p, std::ios::binary);
        if (!f) { LOGI("font skip (missing): %s", p.c_str()); continue; }
        f.seekg(0, std::ios::end); size_t n = size_t(f.tellg()); if (!n) continue;
        f.seekg(0); font.resize(n); f.read(reinterpret_cast<char*>(font.data()), n);
        stbtt_fontinfo t{};
        int off = stbtt_GetFontOffsetForIndex(font.data(), 0);
        if (!stbtt_InitFont(&t, font.data(), off)) { LOGI("font skip (init fail): %s", p.c_str()); continue; }
        int got = 0;
        for (uint32_t c : cps) if (c >= 0x4E00 && c <= 0x9FFF)
            if (stbtt_FindGlyphIndex(&t, int(c)) != 0) ++got;
        LOGI("font candidate: %s init=ok cjk=%d/%d", p.c_str(), got, cjkNeed);
        if (got > bestCov) { bestCov = got; used = p.c_str(); }  // 记下覆盖率最高的
    }
    if (!used) { LOGE("no usable font found"); return false; }
    // 重新读入选中字体（上一轮 font 可能属于另一个候选）
    {
        std::ifstream f(used, std::ios::binary);
        f.seekg(0, std::ios::end); size_t n = size_t(f.tellg()); f.seekg(0);
        font.resize(n); f.read(reinterpret_cast<char*>(font.data()), n);
    }
    LOGI("font: using %s (cjk=%d/%d)", used, bestCov, cjkNeed);

    stbtt_fontinfo info{};
    stbtt_InitFont(&info, font.data(), stbtt_GetFontOffsetForIndex(font.data(), 0));

    const float baked = bakedPx;
    float scale = stbtt_ScaleForPixelHeight(&info, baked);
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
    impl_.ascentPx = float(ascent) * scale;

    impl_.atlasW = 1024;
    // 行高留足：字形高度 ≈ baked，再留上下余量，避免 LINEAR 采样把上一行的像素
    // 拉成细线（大字号下尤其明显 = "乱的线条" 的元凶之一）。
    int rowH = int(baked * 1.6f) + 4;
    // 按最宽字形（CJK ≈ 1em）估行，宁可多留一行，避免静默丢字形
    int perRow = std::max(1, impl_.atlasW / int(baked + 4));
    int rows = int(cps.size()) / perRow + 3;
    impl_.atlasH = rows * rowH;
    std::vector<uint8_t> atlas(size_t(impl_.atlasW) * impl_.atlasH, 0);

    impl_.glyphs.clear();
    float penX = 1, penY = 1;
    for (uint32_t cp : cps) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBox(&info, int(cp), scale, scale, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (penX + gw + 1 >= impl_.atlasW) { penX = 2; penY += rowH; }
        if (penY + rowH >= impl_.atlasH) break;
        if (gw > 0 && gh > 0)
            stbtt_MakeCodepointBitmap(&info, atlas.data() + size_t(penY) * impl_.atlasW + int(penX),
                                      gw, gh, impl_.atlasW, scale, scale, int(cp));
        Glyph g;
        g.u0 = penX / float(impl_.atlasW);          g.v0 = penY / float(impl_.atlasH);
        g.u1 = (penX + gw) / float(impl_.atlasW);   g.v1 = (penY + gh) / float(impl_.atlasH);
        g.w = float(gw); g.h = float(gh);
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&info, int(cp), &adv, &lsb);
        g.advance = float(adv) * scale;
        g.bearingX = float(x0);
        g.bearingY = float(y0) + impl_.ascentPx;
        impl_.glyphs[cp] = g;
        penX += gw + 6;   // 相邻字形留 6px 透明间隙，杜绝 LINEAR 采样串扰成细线
    }

    if (impl_.atlasTex) glDeleteTextures(1, &impl_.atlasTex);
    glGenTextures(1, &impl_.atlasTex);
    glBindTexture(GL_TEXTURE_2D, impl_.atlasTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, impl_.atlasW, impl_.atlasH, 0, GL_ALPHA, GL_UNSIGNED_BYTE,
                 atlas.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    impl_.bakedPx = baked;
    impl_.fontOk = true;
    LOGI("font atlas baked: %dx%d glyphs=%zu", impl_.atlasW, impl_.atlasH, impl_.glyphs.size());
    return true;
}

float Gl::textWidth(const std::string& utf8, float px) const {
    if (!impl_.fontOk) return 0;
    float s = px / impl_.bakedPx;
    float w = 0;
    for (size_t i = 0; i < utf8.size();) {
        uint32_t cp = uint8_t(utf8[i]);
        int n = 1;
        if ((cp & 0xE0) == 0xC0) { cp &= 0x1F; n = 2; }
        else if ((cp & 0xF0) == 0xE0) { cp &= 0x0F; n = 3; }
        else if ((cp & 0xF8) == 0xF0) { cp &= 0x07; n = 4; }
        for (int k = 1; k < n && i + k < utf8.size(); ++k) cp = (cp << 6) | (uint8_t(utf8[i + k]) & 0x3F);
        auto it = impl_.glyphs.find(cp);
        if (it != impl_.glyphs.end()) w += it->second.advance;
        else w += impl_.bakedPx * 0.5f;
        i += n;
    }
    return w * s;
}

// 让字符串的**实际墨迹**垂直居中于 cy：字形绘制起点 = yTop + bearingY*s（bearingY =
// 字形位图顶相对行顶的偏移，Gl::text 同源），故墨迹顶 = yTop + min(bearingY)*s、
// 墨迹底 = yTop + max(bearingY+h)*s。令 (顶+底)/2 = cy 反解 yTop。
float Gl::textCenterTop(const std::string& utf8, float px, float cy) const {
    if (!impl_.fontOk || impl_.bakedPx <= 0) return cy - px * 0.5f;
    const float s = px / impl_.bakedPx;
    float top = 1e9f, bottom = -1e9f;
    for (size_t i = 0; i < utf8.size();) {
        uint32_t cp = uint8_t(utf8[i]);
        int n = 1;
        if ((cp & 0xE0) == 0xC0) { cp &= 0x1F; n = 2; }
        else if ((cp & 0xF0) == 0xE0) { cp &= 0x0F; n = 3; }
        else if ((cp & 0xF8) == 0xF0) { cp &= 0x07; n = 4; }
        for (int k = 1; k < n && i + k < utf8.size(); ++k)
            cp = (cp << 6) | (uint8_t(utf8[i + k]) & 0x3F);
        i += n;
        auto it = impl_.glyphs.find(cp);
        if (it == impl_.glyphs.end()) continue;
        const Glyph& g = it->second;
        top = std::min(top, g.bearingY);
        bottom = std::max(bottom, g.bearingY + g.h);
    }
    if (top > bottom) return cy - px * 0.5f;   // 无有效字形（图集未覆盖），退回估算法
    return cy - (top + (bottom - top) * 0.5f) * s;
}

void Gl::text(const std::string& utf8, float x, float yBaseline, float px, const Rgba& c) {
    if (!impl_.fontOk) return;
    float s = px / impl_.bakedPx;

    std::vector<float> vtx, uv;
    float pen = x;
    for (size_t i = 0; i < utf8.size();) {
        uint32_t cp = uint8_t(utf8[i]);
        int n = 1;
        if ((cp & 0xE0) == 0xC0) { cp &= 0x1F; n = 2; }
        else if ((cp & 0xF0) == 0xE0) { cp &= 0x0F; n = 3; }
        else if ((cp & 0xF8) == 0xF0) { cp &= 0x07; n = 4; }
        for (int k = 1; k < n && i + k < utf8.size(); ++k) cp = (cp << 6) | (uint8_t(utf8[i + k]) & 0x3F);
        i += n;
        auto it = impl_.glyphs.find(cp);
        if (it == impl_.glyphs.end()) { pen += impl_.bakedPx * 0.5f * s; continue; }
        const Glyph& g = it->second;
        float gx = pen + g.bearingX * s;
        float gy = yBaseline + g.bearingY * s;
        float gw = g.w * s, gh = g.h * s;
        if (gw > 0 && gh > 0) {
            // 必须用 GL_TRIANGLES 逐字形独立双三角形。若把多字形串进一条
            // TRIANGLE_STRIP，相邻字形四边形之间会自动生成"对角连接三角形"
            //（前字形右下角 → 后字形左上角），把字形间空隙用错误采样填满，
            // 呈现"字形之间斜坡状连线"伪影（2026-09-30 截图确诊）。
            vtx.insert(vtx.end(), {gx, gy,     gx + gw, gy,     gx, gy + gh,
                                   gx + gw, gy, gx + gw, gy + gh, gx, gy + gh});
            uv.insert(uv.end(), {g.u0, g.v0, g.u1, g.v0, g.u0, g.v1,
                                 g.u1, g.v0, g.u1, g.v1, g.u0, g.v1});
        }
        pen += g.advance * s;
    }
    if (vtx.empty()) return;

    glUseProgram(impl_.progText);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, impl_.atlasTex);
    glUniform2f(impl_.uText.viewport, float(winW_), float(winH_));
    glUniform4f(impl_.uText.color, c.r, c.g, c.b, c.a);
    // vtx / uv 是两条独立的紧密数组，逐顶点 2 float，stride = 8 字节
    glEnableVertexAttribArray(impl_.aText);
    glEnableVertexAttribArray(impl_.aUv);
    glVertexAttribPointer(impl_.aText, 2, GL_FLOAT, GL_FALSE, 8, vtx.data());
    glVertexAttribPointer(impl_.aUv, 2, GL_FLOAT, GL_FALSE, 8, uv.data());
    glDrawArrays(GL_TRIANGLES, 0, int(vtx.size() / 2));
    glDisableVertexAttribArray(impl_.aText);
    glDisableVertexAttribArray(impl_.aUv);
}

void Gl::clear(const Rgba& c) {
    glClearColor(c.r, c.g, c.b, c.a);
    glClear(GL_COLOR_BUFFER_BIT);
}

// uvRot < 0 = 自动：源/目标同为横（或同为竖）就不转，否则转 90°。
// 这是纯几何判断，只保证画面不侧躺；若整机是反的（差 180°），用 controls.txt 的 uvrot=2 覆盖。
// srcSlot：预览源（0=逻辑 / 1=uw / 2=tele）——多流常驻会话下纹理永不失效。
void Gl::drawPreview(int32_t x, int32_t y, int32_t w, int32_t h, int uvRot, int srcSlot,
                     float alpha, float zoom) {
    if (srcSlot < 0 || srcSlot >= kSrcN) return;
    Source& s = src_[srcSlot];
    GLuint tex = s.lastTex;
    if (!tex) return;

    if (uvRot < 0 && s.w > 0 && s.h > 0 && w > 0 && h > 0) {
        bool srcLand = s.w >= s.h;
        bool dstLand = w >= h;
        uvRot = (srcLand == dstLand) ? 0 : 1;
    }
    if (uvRot < 0) uvRot = 0;

    // cover 裁切：源与目标的"有效宽高比"不等时，按长边方向的中心子矩形取样，避免拉伸变形。
    // 旋转 90/270 后画面有效宽高比是 pvH/pvW。
    float cx = 1.f, cy = 1.f;
    if (s.w > 0 && s.h > 0 && w > 0 && h > 0) {
        float sa = (uvRot & 1) ? float(s.h) / float(s.w) : float(s.w) / float(s.h);
        float da = float(w) / float(h);
        if (sa > da) cx = da / sa;
        else cy = sa / da;
    }
    // 本层生效的数字变焦：全局 previewZoom_ 或调用方覆盖（淡化旧层用自己的 crop）
    const float pz = zoom > 0.f ? zoom : previewZoom_;
    if (!s.logged) {
        s.logged = true;
        LOGI("preview orient: slot=%d src=%dx%d dst=%dx%d rot=%d crop=(%.3f,%.3f) "
             "uCropLoc=%d pvZoom=%.3f",
             srcSlot, s.w, s.h, w, h, uvRot & 3, cx, cy, impl_.uPv.crop, pz);
    }
    // 数字变焦：与 cover 裁切同域（uCrop < 1 = 取中心子矩形 = 放大），等比缩小取样窗
    if (pz > 1.f) {
        cx /= pz;
        cy /= pz;
    }

    glUseProgram(impl_.progPreview);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glUniform1i(impl_.uPv.tex, 0);
    glUniform2f(impl_.uPv.viewport, float(winW_), float(winH_));
    glUniform4f(impl_.uPv.rect, float(x), float(y), float(w), float(h));
    glUniform4f(impl_.uPv.uv, 0.f, 0.f, 1.f, 1.f);
    glUniform1f(impl_.uPv.rot, float(uvRot & 3));
    glUniform2f(impl_.uPv.crop, cx, cy);
    glUniform1f(impl_.uPv.alpha, alpha);
    glEnableVertexAttribArray(impl_.aPv);
    glVertexAttribPointer(impl_.aPv, 2, GL_FLOAT, GL_FALSE, 0, kQuad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(impl_.aPv);
}

void Gl::roundedRect(float x, float y, float w, float h, float radius,
                     const Rgba& fill, const Rgba& border, float borderW) {
    float bw = border.a > 0 ? borderW : 0.f;
    float expand = bw + 1.5f;
    glUseProgram(impl_.progRect);
    glUniform2f(impl_.uRect.viewport, float(winW_), float(winH_));
    glUniform2f(impl_.uRect.center, x + w / 2, y + h / 2);
    glUniform2f(impl_.uRect.half, w / 2, h / 2);
    glUniform1f(impl_.uRect.radius, std::max(radius, 0.f));
    glUniform4f(impl_.uRect.fill, fill.r, fill.g, fill.b, fill.a);
    glUniform4f(impl_.uRect.border, border.r, border.g, border.b, border.a);
    glUniform1f(impl_.uRect.borderW, bw);
    glUniform4f(impl_.uRect.quad, x - expand, y - expand, w + expand * 2, h + expand * 2);
    glEnableVertexAttribArray(impl_.aRect);
    glVertexAttribPointer(impl_.aRect, 2, GL_FLOAT, GL_FALSE, 0, kQuad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(impl_.aRect);
}

void Gl::triangles(const float* xy, int vertexCount, const Rgba& c) {
    if (!ready() || !xy || vertexCount < 3) return;
    glUseProgram(impl_.progSolid);
    glUniform2f(impl_.uSolid.viewport, float(winW_), float(winH_));
    glUniform4f(impl_.uSolid.color, c.r, c.g, c.b, c.a);
    glEnableVertexAttribArray(impl_.aSolid);
    glVertexAttribPointer(impl_.aSolid, 2, GL_FLOAT, GL_FALSE, 0, xy);
    glDrawArrays(GL_TRIANGLES, 0, vertexCount);
    glDisableVertexAttribArray(impl_.aSolid);
}

void Gl::swap() {
    if (impl_.dpy != EGL_NO_DISPLAY && impl_.surf != EGL_NO_SURFACE)
        eglSwapBuffers(impl_.dpy, impl_.surf);
}

void Gl::beginFrame(const Rgba& c) {
    if (!ready()) return;
    glViewport(0, 0, winW_, winH_);
    clear(c);
}

bool Gl::ensureOverlayFbo(int w, int h) {
    if (impl_.overlayFbo_ && impl_.overlayW_ == w && impl_.overlayH_ == h) return true;
    if (impl_.overlayFbo_) glDeleteFramebuffers(1, &impl_.overlayFbo_);
    if (impl_.overlayTex_) glDeleteTextures(1, &impl_.overlayTex_);
    impl_.overlayFbo_ = impl_.overlayTex_ = 0;
    glGenTextures(1, &impl_.overlayTex_);
    glBindTexture(GL_TEXTURE_2D, impl_.overlayTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &impl_.overlayFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, impl_.overlayFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, impl_.overlayTex_, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) {
        LOGE("overlay FBO incomplete %dx%d", w, h);
        glDeleteFramebuffers(1, &impl_.overlayFbo_);
        glDeleteTextures(1, &impl_.overlayTex_);
        impl_.overlayFbo_ = impl_.overlayTex_ = 0;
        return false;
    }
    impl_.overlayW_ = w;
    impl_.overlayH_ = h;
    return true;
}

bool Gl::beginOverlay() {
    if (!ready()) return false;
    if (!ensureOverlayFbo(winW_, winH_)) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, impl_.overlayFbo_);
    glViewport(0, 0, impl_.overlayW_, impl_.overlayH_);
    glClearColor(0, 0, 0, 0);            // 透明底：合成时预览透过透明区显示
    glClear(GL_COLOR_BUFFER_BIT);
    return true;
}

void Gl::endOverlay() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, winW_, winH_);
}

void Gl::drawOverlayFull() {
    if (!impl_.overlayTex_) return;
    glUseProgram(impl_.progBlit_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, impl_.overlayTex_);
    glUniform1i(impl_.uBlitTex_, 0);
    glEnableVertexAttribArray(impl_.aBlit_);
    glVertexAttribPointer(impl_.aBlit_, 2, GL_FLOAT, GL_FALSE, 0, kQuad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(impl_.aBlit_);
}

bool Gl::consumeHistUpdated(int slot) {
    if (slot < 0 || slot >= kSrcN) return false;
    bool v = src_[slot].histNew;
    src_[slot].histNew = false;
    return v;
}

} // namespace optic::ui
