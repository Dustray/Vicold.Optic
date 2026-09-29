#include "core/ui/Gl.h"

// 单文件头库：实现只在本 TU 展开一次
#define STB_TRUETYPE_IMPLEMENTATION 1
#include "core/ui/stb_truetype.h"

#include "core/util/Log.h"

#include <algorithm>
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
    "varying vec2 vUv;\n"
    "void main(){ gl_FragColor = texture2D(uTex, vUv); }\n";

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
} // namespace

// ---- Gl 接口实现 ----

bool Gl::attach(ANativeWindow* win) {
    if (ready() && win_ == win) return true;
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
    if (!impl_.progPreview || !impl_.progRect || !impl_.progText || !impl_.progSolid) return false;
    impl_.aPv = glGetAttribLocation(impl_.progPreview, "aPos");
    impl_.aRect = glGetAttribLocation(impl_.progRect, "aPos");
    impl_.aText = glGetAttribLocation(impl_.progText, "aPos");
    impl_.aUv = glGetAttribLocation(impl_.progText, "aUv");
    impl_.aSolid = glGetAttribLocation(impl_.progSolid, "aPos");
    impl_.uPv.rect = glGetUniformLocation(impl_.progPreview, "uRect");
    impl_.uPv.viewport = glGetUniformLocation(impl_.progPreview, "uViewport");
    impl_.uPv.uv = glGetUniformLocation(impl_.progPreview, "uUv");
    impl_.uPv.tex = glGetUniformLocation(impl_.progPreview, "uTex");
    impl_.uPv.rot = glGetUniformLocation(impl_.progPreview, "uRot");
    impl_.uPv.crop = glGetUniformLocation(impl_.progPreview, "uCrop");
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
        {
            std::lock_guard<std::mutex> l(impl_.pendM);
            if (impl_.pending) AImage_delete(impl_.pending);
            impl_.pending = nullptr;
        }
        for (auto& t : impl_.pv) {
            if (t.tex) glDeleteTextures(1, &t.tex);
            if (t.eglImg != EGL_NO_IMAGE_KHR) eglDestroyImageKHR(impl_.dpy, t.eglImg);
            if (t.ahb) AHardwareBuffer_release(t.ahb);
        }
        if (impl_.atlasTex) glDeleteTextures(1, &impl_.atlasTex);
        if (impl_.progPreview) glDeleteProgram(impl_.progPreview);
        if (impl_.progRect) glDeleteProgram(impl_.progRect);
        if (impl_.progText) glDeleteProgram(impl_.progText);
        if (impl_.progSolid) glDeleteProgram(impl_.progSolid);
        if (impl_.reader) {
            AImageReader_setImageListener(impl_.reader, nullptr);
            AImageReader_delete(impl_.reader);
        }
        eglMakeCurrent(impl_.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (impl_.surf != EGL_NO_SURFACE) eglDestroySurface(impl_.dpy, impl_.surf);
        if (impl_.ctx != EGL_NO_CONTEXT) eglDestroyContext(impl_.dpy, impl_.ctx);
        eglTerminate(impl_.dpy);
    }
    // Impl 含 std::mutex 不可整体赋值，逐字段复位
    impl_.dpy = EGL_NO_DISPLAY;
    impl_.surf = EGL_NO_SURFACE;
    impl_.ctx = EGL_NO_CONTEXT;
    impl_.reader = nullptr;
    impl_.previewWindow = nullptr;
    impl_.pvW = impl_.pvH = 0;
    impl_.frameNo = -1;
    for (auto& t : impl_.pv) t = {};
    impl_.progPreview = impl_.progRect = impl_.progText = impl_.progSolid = 0;
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
    impl_.listener = {};
    impl_.listenerHits = 0;
    win_ = nullptr;
}

bool Gl::ready() const { return impl_.dpy != EGL_NO_DISPLAY && impl_.surf != EGL_NO_SURFACE; }
int32_t Gl::width() const { return winW_; }
int32_t Gl::height() const { return winH_; }

bool Gl::makePreviewSource(int32_t w, int32_t h) {
    if (!ready()) return false;
    if (impl_.reader) return true;
    // 17 Pro 实测：YUV_420_888 ImageReader 流 HAL 侧持续产帧（dumpsys Frames produced 正常增长）
    // 但 consumer 侧恒 NO_BUFFER_AVAILABLE（伴随厂商 HAL 的 FrameInsert/undistort 报错），
    // 推断小米 HAL 劫持了 YUV ImageReader 通路；RGBA_8888 同样不供帧（不在保证格式表内）。
    // 改走标准零拷贝路径：PRIV(0x22) + GPU_SAMPLED_IMAGE → EGLImage → samplerExternalOES。
    // 代价：PRIV 无法 CPU 读，直方图暂空（后续走 GL 降采样统计，见 M7.1）。
    uint64_t usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    if (AImageReader_newWithUsage(w, h, AIMAGE_FORMAT_PRIVATE, usage, 4, &impl_.reader) !=
            AMEDIA_OK ||
        !impl_.reader) {
        LOGE("preview AImageReader_new failed");
        return false;
    }

    // 关键：本机实测必须在回调线程里 acquire，否则轮询 acquire 恒 NO_BUFFER_AVAILABLE
    impl_.listenerHits = 0;
    impl_.listener = {this, &Gl::onPreviewAvailable};
    if (AImageReader_setImageListener(impl_.reader, &impl_.listener) != AMEDIA_OK) {
        LOGE("preview setImageListener failed");
        return false;
    }
    if (AImageReader_getWindow(impl_.reader, &impl_.previewWindow) != AMEDIA_OK ||
        !impl_.previewWindow) {
        LOGE("preview reader window failed");
        return false;
    }
    impl_.pvW = w;
    impl_.pvH = h;
    LOGI("preview source ready: %dx%d PRIV(GPU)", w, h);
    return true;
}

ANativeWindow* Gl::previewWindow() { return impl_.previewWindow; }
int32_t Gl::previewW() { return impl_.pvW; }
int32_t Gl::previewH() { return impl_.pvH; }

void Gl::onPreviewAvailable(void* ctx, AImageReader* reader) {
    auto* self = static_cast<Gl*>(ctx);
    if (!self || !reader) return;
    if (++self->impl_.listenerHits == 1) LOGI("preview frames flowing");

    // 回调线程取最新帧；旧 pending 直接丢弃（等价 acquireLatest 语义）
    AImage* img = nullptr;
    if (AImageReader_acquireLatestImage(reader, &img) != AMEDIA_OK || !img) return;
    std::lock_guard<std::mutex> l(self->impl_.pendM);
    if (self->impl_.pending) AImage_delete(self->impl_.pending);
    self->impl_.pending = img;
}

void Gl::importPreviewImage(AImage* img) {
    AHardwareBuffer* ahb = nullptr;
    if (AImage_getHardwareBuffer(img, &ahb) != AMEDIA_OK || !ahb) return;

    // 缓存：同一 AHardwareBuffer 循环复用（maxImages=3）
    for (auto& t : impl_.pv) {
        if (t.ahb == ahb) {
            if (t.tex) glBindTexture(GL_TEXTURE_EXTERNAL_OES, t.tex);
            impl_.lastTex = t.tex;
            return;
        }
    }
    Impl::PvTex* slot = nullptr;
    for (auto& t : impl_.pv)
        if (!t.ahb) { slot = &t; break; }
    if (!slot) slot = &impl_.pv[0];
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
    impl_.lastTex = slot->tex;
}

int64_t Gl::acquirePreview(int32_t histR[64], int32_t histG[64], int32_t histB[64]) {
    if (!ready() || !impl_.reader) return impl_.frameNo;

    // 取回调线程攒下的最新帧；无新帧时保持上一帧纹理与直方图不动
    AImage* img = nullptr;
    {
        std::lock_guard<std::mutex> l(impl_.pendM);
        img = impl_.pending;
        impl_.pending = nullptr;
    }
    if (!img) return impl_.frameNo;

    importPreviewImage(img);

    std::memset(histR, 0, 64 * sizeof(int32_t));
    std::memset(histG, 0, 64 * sizeof(int32_t));
    std::memset(histB, 0, 64 * sizeof(int32_t));

    // PRIV 流无法 CPU 读，直方图暂空；后续走 GL 降采样统计（M7.1）
    uint8_t *yd = nullptr, *ud = nullptr, *vd = nullptr;
    int yl = 0, ul = 0, vl = 0, ys = 0, us = 0, vs = 0, ypx = 0, upx = 0, vpx = 0;
    bool ok = AImage_getPlaneData(img, 0, &yd, &yl) == AMEDIA_OK && yd && yl > 0 &&
              AImage_getPlaneData(img, 1, &ud, &ul) == AMEDIA_OK && ud && ul > 0 &&
              AImage_getPlaneData(img, 2, &vd, &vl) == AMEDIA_OK && vd && vl > 0;
    if (ok) {
        AImage_getPlaneRowStride(img, 0, &ys);
        AImage_getPlaneRowStride(img, 1, &us);
        AImage_getPlaneRowStride(img, 2, &vs);
        AImage_getPlanePixelStride(img, 0, &ypx);
        AImage_getPlanePixelStride(img, 1, &upx);
        AImage_getPlanePixelStride(img, 2, &vpx);
        int px = std::max(1, impl_.pvW / 16), py = std::max(1, impl_.pvH / 16);
        for (int32_t y = 0; y < impl_.pvH; y += py) {
            const uint8_t* yrow = yd + size_t(y) * ys;
            const uint8_t* urow = ud + size_t(y / 2) * us;
            const uint8_t* vrow = vd + size_t(y / 2) * vs;
            for (int32_t x = 0; x < impl_.pvW; x += px) {
                int Y = yrow[size_t(x) * ypx];
                int U = urow[size_t(x / 2) * upx] - 128;
                int V = vrow[size_t(x / 2) * vpx] - 128;
                int r = Y + (1402 * V) / 1000;
                int g = Y - (344 * U) / 1000 - (714 * V) / 1000;
                int b = Y + (1772 * U) / 1000;
                ++histR[std::clamp(r >> 2, 0, 63)];
                ++histG[std::clamp(g >> 2, 0, 63)];
                ++histB[std::clamp(b >> 2, 0, 63)];
            }
        }
    }

    AImage_delete(img);
    return ++impl_.frameNo;
}

bool Gl::bakeFont(float bakedPx) {
    if (!ready()) return false;
    static const char* kFonts[] = {
        "/system/fonts/MiSansVF.ttf",
        "/system/fonts/MiSansNormal.ttf",
        "/system/fonts/NotoSansCJK-Regular.ttc",
        "/system/fonts/Roboto-Regular.ttf",
        "/system/fonts/DroidSans.ttf",
    };
    std::vector<uint8_t> font;
    for (const char* p : kFonts) {
        std::ifstream f(p, std::ios::binary);
        if (!f) continue;
        f.seekg(0, std::ios::end);
        size_t n = size_t(f.tellg());
        if (!n) continue;
        f.seekg(0);
        font.resize(n);
        f.read(reinterpret_cast<char*>(font.data()), n);
        LOGI("font: %s (%zu bytes)", p, n);
        break;
    }
    if (font.empty()) { LOGE("no font found"); return false; }

    stbtt_fontinfo info{};
    int offset = stbtt_GetFontOffsetForIndex(font.data(), 0);
    if (!stbtt_InitFont(&info, font.data(), offset)) { LOGE("stbtt_InitFont failed"); return false; }

    const float baked = bakedPx;
    float scale = stbtt_ScaleForPixelHeight(&info, baked);
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
    impl_.ascentPx = float(ascent) * scale;

    std::set<uint32_t> cps;
    for (uint32_t c = 0x20; c <= 0x7E; ++c) cps.insert(c);
    // UI 实际用到的全部非 ASCII 字符（字形按需增删，见 Ui.cpp 文案）
    const std::string extra = "曝光时间补偿就绪已保存环形缓冲摄像头区域对焦拍摄×·";
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

    impl_.atlasW = 1024;
    int rowH = int(baked * 1.35f) + 2;
    // 按最宽字形（CJK ≈ 1em）估行，宁可多留一行，避免静默丢字形
    int perRow = std::max(1, impl_.atlasW / int(baked + 2));
    int rows = int(cps.size()) / perRow + 3;
    impl_.atlasH = rows * rowH;
    std::vector<uint8_t> atlas(size_t(impl_.atlasW) * impl_.atlasH, 0);

    impl_.glyphs.clear();
    float penX = 1, penY = 1;
    for (uint32_t cp : cps) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBox(&info, int(cp), scale, scale, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (penX + gw + 1 >= impl_.atlasW) { penX = 1; penY += rowH; }
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
        penX += gw + 2;
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
            vtx.insert(vtx.end(), {gx, gy, gx + gw, gy, gx, gy + gh, gx + gw, gy + gh});
            uv.insert(uv.end(), {g.u0, g.v0, g.u1, g.v0, g.u0, g.v1, g.u1, g.v1});
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
    glDrawArrays(GL_TRIANGLE_STRIP, 0, int(vtx.size() / 2));
    glDisableVertexAttribArray(impl_.aText);
    glDisableVertexAttribArray(impl_.aUv);
}

void Gl::clear(const Rgba& c) {
    glClearColor(c.r, c.g, c.b, c.a);
    glClear(GL_COLOR_BUFFER_BIT);
}

// uvRot < 0 = 自动：源/目标同为横（或同为竖）就不转，否则转 90°。
// 这是纯几何判断，只保证画面不侧躺；若整机是反的（差 180°），用 controls.txt 的 uvrot=2 覆盖。
void Gl::drawPreview(int32_t x, int32_t y, int32_t w, int32_t h, int uvRot) {
    GLuint tex = impl_.lastTex;
    if (!tex) return;

    if (uvRot < 0 && impl_.pvW > 0 && impl_.pvH > 0 && w > 0 && h > 0) {
        bool srcLand = impl_.pvW >= impl_.pvH;
        bool dstLand = w >= h;
        uvRot = (srcLand == dstLand) ? 0 : 1;
    }
    if (uvRot < 0) uvRot = 0;

    // cover 裁切：源与目标的"有效宽高比"不等时，按长边方向的中心子矩形取样，避免拉伸变形。
    // 旋转 90/270 后画面有效宽高比是 pvH/pvW。
    float cx = 1.f, cy = 1.f;
    if (impl_.pvW > 0 && impl_.pvH > 0 && w > 0 && h > 0) {
        float sa = (uvRot & 1) ? float(impl_.pvH) / float(impl_.pvW)
                               : float(impl_.pvW) / float(impl_.pvH);
        float da = float(w) / float(h);
        if (sa > da) cx = da / sa;
        else cy = sa / da;
    }
    if (!impl_.rotLogged) {
        impl_.rotLogged = true;
        LOGI("preview orient: src=%dx%d dst=%dx%d rot=%d crop=(%.3f,%.3f)",
             impl_.pvW, impl_.pvH, w, h, uvRot & 3, cx, cy);
    }
    // 数字变焦：与 cover 裁切同域（uCrop < 1 = 取中心子矩形 = 放大），等比缩小取样窗
    if (previewZoom_ > 1.f) {
        cx /= previewZoom_;
        cy /= previewZoom_;
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

} // namespace optic::ui
