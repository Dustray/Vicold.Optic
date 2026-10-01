#pragma once
// 拍摄输出处理管线（可插拔）：
//   StillCapture（采集）→ StillProcessor（处理阶段，可替换/串联）→ 落盘
//
// 当前阶段：PassThrough —— HAL JPEG blob 直通保存（ISP 已完成 AE/AWB/降噪/锐化，
// EXIF 由 HAL 内嵌）。后续扩展点：
//   - 滤镜系统：实现 StillProcessor，在 process() 里对 YUV 平面做处理；
//     届时 StillCapture 的 reader 换 YUV_420_888 通路 + 软件编码器（或复用 GL
//     shader 做实时预览滤镜，快门时同一算法走离屏渲染）。
//   - 自定义算法 / 实时用户设置：StillParams 携带快门时刻的全部参数快照，
//     处理阶段据此生效（用户在 UI 拖的每个设置都会实时反映在 params 里）。
//
// 帧的所有权与生命周期：StillFrame 持有像素数据拷贝，处理/保存阶段可安全跨线程。

#include <cstdint>
#include <vector>

namespace optic::capture {

// 快门时刻的拍摄参数快照（引擎在 triggerBurst 时填充）
struct StillParams {
    float zoom = 1.f;          // 用户倍率（导轨值，= 预览显示 FOV）
    float appliedZoom = 1.f;   // 实际出图的等效倍率（照片 FOV 的真值）：
                               //   逻辑流单拍 = 钳制后写进 HAL 的 zoom（≤4.85）；
                               //   超广物理流单拍 = uwNativeZoom（原生 0.774）。
                               // WysiwygCropProcessor 据 zoom/appliedZoom 补软件裁切，
                               // 使照片 FOV 与预览严格一致（长焦带高倍曾是
                               // 「预览 10x、照片 4.85x」的根因，2026-09-30）。
    int iso = 0;               // 实际生效 ISO（自动时 = AE 冻结值）
    int64_t exposureNs = 0;    // 实际生效曝光时间（自动时 = AE 冻结值）
    int evSteps = 0;           // EV 补偿步数
    bool aeOn = true;          // 双自动（真 AE）
    bool awbOn = true;         // AWB on
    // 后续：滤镜 id / 强度 / LUT 文件 / 水印设置 / 构图线 …
};

// 一张待处理/待保存的照片
struct StillFrame {
    enum class Fmt { JpegBlob, Yuv420 };
    Fmt fmt = Fmt::JpegBlob;
    std::vector<uint8_t> blob;   // JpegBlob：完整 JPEG 码流（EXIF 内嵌）；Yuv420 预留
    int w = 0, h = 0;
    int64_t tsNs = 0;
    StillParams params;          // 快门时刻参数（处理阶段的用户设置来源）
};

// 可插拔处理阶段：返回 false = 丢弃该帧（算法判定不保存等）
class StillProcessor {
public:
    virtual ~StillProcessor() = default;
    virtual bool process(StillFrame& f) = 0;
};

// 直通：原样保存（当前默认阶段）
class PassThroughProcessor final : public StillProcessor {
public:
    bool process(StillFrame&) override { return true; }
};

// WYSIWYG 裁切：照片 FOV 对齐预览显示。逻辑流单拍的 zoom 被钳在安全带内
//（≤kLogicalSafeMax=4.85，融合管线坏区之上），长焦带高倍预览靠 GL 数字裁切显示
// zoom_，照片若不补裁切就会比预览广（z=10 时照片只有 4.85x）。差异 >0.2% 时
// 中心裁切 zoom/appliedZoom 并重编码；否则直通（保留 HAL EXIF 与原画质）。
// 实现（stb 解码/编码）在 StillProcessor.cpp —— 头文件保持无依赖。
class WysiwygCropProcessor final : public StillProcessor {
public:
    bool process(StillFrame& f) override;
};

} // namespace optic::capture
