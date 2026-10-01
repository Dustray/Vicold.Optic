// WysiwygCropProcessor 实现：stb 解码 → 中心裁切 → 重编码。
// 仅在照片 FOV 比预览广（appliedZoom < zoom）超过 0.2% 时启用，其余帧直通
// （保留 HAL 直出码流的 EXIF 与画质，零重编码损失）。
// 重编码的代价：EXIF 丢失（MediaStore 的 DATE_TAKEN 仍由 GalleryWriter 写入），
// 且此路径只发生在长焦带高倍（z > 4.85）——全画质关键段不受影响。

#include "core/capture/StillPipeline.h"
#include "core/util/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#pragma GCC diagnostic pop

namespace optic::capture {

bool WysiwygCropProcessor::process(StillFrame& f) {
    if (f.fmt != StillFrame::Fmt::JpegBlob || f.blob.empty()) return true;
    const float az = f.params.appliedZoom > 0.f ? f.params.appliedZoom : 1.f;
    const float factor = f.params.zoom / az;   // >1 = 照片比预览广，需裁切
    if (!(factor > 1.002f)) return true;       // 差异可忽略（含 az≥zoom 的所有情形）

    int w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load_from_memory(f.blob.data(), static_cast<int>(f.blob.size()),
                                        &w, &h, &ch, 3);
    if (!px) {
        LOGE("wysiwyg: jpeg decode failed (%zu bytes), saving uncropped", f.blob.size());
        return true;   // 解码失败宁可保存未裁切版，也不丢照片
    }

    // 裁切目标尺寸：向下取偶（YUV/JPEG 编码器惯例），并保底防 0
    const int cw = std::max(2, int(w / factor) & ~1);
    const int chh = std::max(2, int(h / factor) & ~1);
    const int ox = (w - cw) / 2;
    const int oy = (h - chh) / 2;

    std::vector<stbi_uc> crop(size_t(cw) * chh * 3);
    for (int y = 0; y < chh; ++y)
        std::memcpy(crop.data() + size_t(y) * cw * 3,
                    px + (size_t(oy + y) * w + ox) * 3, size_t(cw) * 3);
    stbi_image_free(px);

    // 重编码（质量 92：与 ISP 直出的观感差可忽略；12MP 软编码 saver 线程 ~1s）。
    // stb 的 JPEG 无 png_to_mem 直出，走 to_func 回调汇入内存。
    std::vector<uint8_t> out;
    auto sink = [](void* ctx, void* data, int len) {
        auto* v = static_cast<std::vector<uint8_t>*>(ctx);
        const auto* p = static_cast<const uint8_t*>(data);
        v->insert(v->end(), p, p + len);
    };
    if (!stbi_write_jpg_to_func(sink, &out, cw, chh, 3, crop.data(), 92) || out.empty()) {
        LOGE("wysiwyg: jpeg encode failed, saving uncropped");
        return true;
    }
    f.blob = std::move(out);
    f.w = cw;
    f.h = chh;
    LOGI("wysiwyg crop: zoom=%.2f applied=%.2f -> %dx%d (%d KB)", f.params.zoom, az, cw, chh,
         int(f.blob.size() / 1024));
    return true;
}

} // namespace optic::capture
