#pragma once
// M-UI：GL 渲染底座（EGL / GLES2）。
// ① 主窗口 EGL surface（预览改为 GPU 纹理合成，替代相机直写）
// ② 预览帧源：RGBA AImageReader（GPU_SAMPLED_IMAGE）→ AHardwareBuffer → EGLImage → 2D 纹理（零拷贝）
// ③ 文字图集：stb_truetype 烘焙设备字体（MiSansVF 优先），ASCII + 常用 CJK
// ④ 绘制原语：预览四边形（UV 旋转可调）、圆角矩形（SDF 填充+描边）、文字
// 线程模型：attach/draw/输入 全部在 glue 线程（UI=渲染=输入 同线程）。

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <media/NdkImageReader.h>


// eglext/gl2ext 的函数原型默认不导出，需先开启再包含头文件，
// 否则 eglCreateImageKHR / glEGLImageTargetTexture2DOES 等符号未声明。
#define EGL_EGLEXT_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace optic::ui {

struct Rgba {
    float r = 1, g = 1, b = 1, a = 1;
};

struct Glyph {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    float w = 0, h = 0;
    float bearingX = 0, bearingY = 0;
    float advance = 0;
};

class Gl {
public:
    Gl() = default;
    ~Gl();                    // Impl 完整定义在 Gl.cpp

    // 主窗口 EGL 初始化（幂等；内部含着色器与字体烘焙）
    bool attach(ANativeWindow* win);
    void detach();
    bool ready() const;
    int32_t width() const;
    int32_t height() const;

    // 预览帧源（RGBA_8888 + GPU_SAMPLED_IMAGE 用途）；window 交给引擎挂会话输出。
    // 多源：slot 0=逻辑主摄 / 1=超广角直连 / 2=长焦直连 —— 会话常驻三路输出，
    // 跨带切换 repeating 请求即可换源（不重建会话，纹理永不失效 → 无黑帧闪烁）。
    bool makePreviewSource(int slot, int32_t w, int32_t h);
    ANativeWindow* previewWindow(int slot);
    int32_t previewW(int slot) const;
    int32_t previewH(int slot) const;
    // 每帧调用：acquireLatestImage → 导入/绑定纹理；有新帧返回 true。
    // wantHist = 对该 slot 统计直方图（GL 降采样 + readPixels，内部 ~120ms 节流）。
    // 只对当前显示源开启：readPixels 会同步 stall 管线，三路全开必掉帧。
    bool acquirePreview(int slot, bool wantHist);
    // 取该 slot 最近一次直方图统计的快照（64-bin × R,G,B；从未统计过时全 0）
    void copyHistogram(int slot, int32_t outR[64], int32_t outG[64], int32_t outB[64]) const;

    // 文字图集（attach 后调用一次；bakedPx 为烘焙像素高）
    bool bakeFont(float bakedPx);
    float textWidth(const std::string& utf8, float px) const;
    // y 为「行顶」（ascent 线），不是基线：字形落在 y 下方，与 CSS line box 对齐
    void text(const std::string& utf8, float x, float yTop, float px, const Rgba& c);
    // 返回让字符串**实际墨迹**（字形位图 bbox）垂直居中于 cy 的行顶 yTop：
    // 按钮圆心等场景里按字号比例估中心会偏（ascent 含上伸部空隙），此函数遍历
    // 字形取 min(bearingY)/max(bearingY+h) 精确居中。
    float textCenterTop(const std::string& utf8, float px, float cy) const;

    void beginFrame(const Rgba& c);   // viewport + 清屏
    void clear(const Rgba& c);
    // alpha < 1 用于跨带显示切换的交叉淡化（新源为底、旧源叠画淡出）。
    // zoom < 0 = 沿用全局 previewZoom_；淡化旧层必须传自己的 crop（zoom_/az_旧带），
    // 否则旧纹理会被新带的 crop 放大到错误倍率（跨带大闪烁元凶之二）。
    void drawPreview(int32_t x, int32_t y, int32_t w, int32_t h, int uvRot, int srcSlot = 0,
                     float alpha = 1.f, float zoom = -1.f);
    // 圆角矩形：填充 + 描边（borderA.a<=0 跳过描边）
    void roundedRect(float cx, float cy, float w, float h, float radius,
                     const Rgba& fill, const Rgba& border, float borderW);
    // 批量纯色三角形（像素坐标）：网格线/直方图柱等，单次 draw 出一批，避免逐图元开销
    void triangles(const float* xy, int vertexCount, const Rgba& c);
    // 预览数字变焦（≥1）：与 cover 裁切同域相乘，中心放大取样。拖拽平滑变焦用 ——
    // 相机侧追赶期间由 GL 补齐 FOV 差值，收敛后恒回 1（无跳变）。
    void setPreviewZoom(float z) { previewZoom_ = z > 1.f ? z : 1.f; }
    // az 时间戳对齐回调（引擎注入）：每消费一帧新预览图像，按该帧 SENSOR_TIMESTAMP
    // 查询当时的 appliedZoom 并写回 UI —— crop 与显示帧严格同源，消除快拖泵动。
    // 返回值 ≤0 表示无可用结果，调用方保持旧 az。
    void setAzSource(std::function<float(int slot, int64_t tsNs)> f) { azSrc_ = std::move(f); }
    void swap();

    // 预览源上限（硬件/实现上限）。实际启用几路由机型层 SessionPolicy.previewSlots
    // 决定：双摄机型不必常开三路只读蒸发器 updates（每路都是一整条 ISP 流水线）。
    static constexpr int kMaxSources = 3;
    int maxSources() const { return kMaxSources; }

private:
    static constexpr int kSrcN = kMaxSources;   // 预览源数上限
    struct Source {
        Gl* gl = nullptr;                // 反向指针（listener 回调定位）
        AImageReader* reader = nullptr;
        ANativeWindow* win = nullptr;
        int32_t w = 0, h = 0;
        int64_t frameNo = -1;
        bool logged = false;
        struct PvTex {
            AHardwareBuffer* ahb = nullptr;
            EGLImageKHR eglImg = EGL_NO_IMAGE_KHR;
            uint32_t tex = 0;
        };
        PvTex pv[4] = {};
        uint32_t lastTex = 0;
        AImageReader_ImageListener listener = {};
        std::mutex pendM;
        AImage* pending = nullptr;
        // 直方图缓存：GL 降采样统计（kHistW×kHistH FBO readPixels → 64-bin RGB）
        int32_t hist[3][64] = {};
        bool histValid = false;
        double lastHistT = 0;            // 上次统计时刻（steady_clock 秒，节流用）
    };
    Source src_[kSrcN];

    struct Impl {
        EGLDisplay dpy = EGL_NO_DISPLAY;
        EGLSurface surf = EGL_NO_SURFACE;
        EGLContext ctx = EGL_NO_CONTEXT;

        GLuint progPreview = 0, progRect = 0, progText = 0, progSolid = 0;
        struct U {
            GLint rect = -1, viewport = -1, uv = -1, tex = -1, center = -1, half = -1,
                  radius = -1, fill = -1, border = -1, borderW = -1, color = -1, quad = -1,
                  rot = -1, crop = -1, alpha = -1;
        } uPv, uRect, uText, uSolid;
        // 属性 location 缓存（避免每次 draw 走 glGetAttribLocation 字符串查找）
        GLint aPv = -1, aRect = -1, aText = -1, aUv = -1, aSolid = -1;

        GLuint atlasTex = 0;
        int32_t atlasW = 0, atlasH = 0;
        float bakedPx = 0;
        std::map<uint32_t, struct Glyph> glyphs;
        float ascentPx = 0;
        GLuint lastTex = 0;
        bool fontOk = false;
    };
    Impl impl_;
    static void onPreviewAvailable(void* ctx, AImageReader* reader);
    void importPreviewImage(Source& s, AImage* img);
    // 直方图降采样管线：预览纹理 → 小 FBO（LINEAR）→ readPixels → 64-bin RGB。
    // RGBA_8888 流无法 AImage_getPlaneData（仅支持 YUV_420_888），CPU 直读死路。
    static constexpr int kHistW = 128, kHistH = 96;
    bool ensureHistRt();
    void sampleHistogram(Source& s);
    GLuint histFbo_ = 0, histTex_ = 0, progHist_ = 0;
    GLint aHist_ = -1, uHistTex_ = -1;
    ANativeWindow* win_ = nullptr;
    int32_t winW_ = 0, winH_ = 0;
    float previewZoom_ = 1.f;            // 预览数字变焦（setPreviewZoom；默认 1 = 不裁切）
    std::function<float(int, int64_t)> azSrc_;
};

} // namespace optic::ui
