#pragma once
// M2.2：最小 DNG writer（TIFF little-endian，未压缩 CFA raw）。
// 结构：IFD0(NewSubFileType=1 + SubIFDs) → IFD1(NewSubFileType=0, raw CFA 数据)。
// 覆盖 DNG 必需 tag：DNGVersion/DNGBackwardVersion/UniqueCameraModel/ColorMatrix1/
//   AsShotNeutral/BlackLevel/WhiteLevel/DefaultCrop*/ActiveArea/CFAPattern/BlackLevelRepeatDim。
// 已知限制：①无缩略图位图（IFD0 仅元数据，Lightroom 自行生成）②ColorMatrix1 为占位待校准（M2.4）
//   ③EXIF IFD/GPS 未做（M2.3）④Orientation 由 sensor.orientation 映射。

#include <cstdint>
#include <string>

namespace optic::dng {

struct StaticMeta {
    int32_t width = 0;
    int32_t height = 0;
    uint8_t cfaPattern[4] = {0, 1, 1, 2};        // RGGB（colorFilterArrangement=0）
    int32_t blackLevel[4] = {64, 64, 64, 64};    // 实测 V3：四通道 64
    int32_t whiteLevel = 1023;                   // 10-bit 传感器
    float colorMatrix1[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // TODO(M2.4)：传感器校准
    int32_t sensorOrientation = 90;              // 映射为 TIFF Orientation
    std::string make = "Xiaomi";
    std::string model = "Xiaomi 17 Pro";
    std::string software = "Vicold.Optic";
};

struct FrameMeta {
    int64_t timestampNs = 0;
    int32_t iso = 0;
    int64_t exposureNs = 0;
    float wbGains[4] = {1, 1, 1, 1};             // [r, gEven, gOdd, b]（COLOR_CORRECTION_GAINS）
};

// bayer：紧密打包 16-bit LE bayer，width*height*2 字节（即 saverLoop 的 buf）
bool write(const std::string& path, const uint8_t* bayer, size_t len,
           const StaticMeta& sm, const FrameMeta& fm);

} // namespace optic::dng
