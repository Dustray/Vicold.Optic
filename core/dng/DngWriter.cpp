#include "core/dng/DngWriter.h"

#include "core/util/Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

namespace optic::dng {
namespace {

// TIFF 类型码
constexpr uint16_t T_BYTE = 1, T_ASCII = 2, T_SHORT = 3, T_LONG = 4, T_RATIONAL = 5, T_SRATIONAL = 10;


struct Entry {
    uint16_t tag = 0, type = 0;
    uint32_t count = 0;
    std::vector<uint8_t> value;   // ≤4B：内联值（LE 从头填）；>4B：值体
    uint32_t offset = 0;          // >4B 的值体绝对偏移（pass A 计算）
};

struct IfdBuilder {
    std::vector<Entry> entries;

    void add(uint16_t tag, uint16_t type, uint32_t count, const void* data, uint32_t bytes) {
        Entry e;
        e.tag = tag; e.type = type; e.count = count;
        const auto* p = static_cast<const uint8_t*>(data);
        if (bytes <= 4) {
            e.value.assign(4, 0);
            if (bytes) std::memcpy(e.value.data(), p, bytes);
        } else {
            e.value.assign(p, p + bytes);
        }
        entries.push_back(std::move(e));
    }
    void addShort(uint16_t tag, uint16_t v) { add(tag, T_SHORT, 1, &v, 2); }
    void addShortVec(uint16_t tag, const uint16_t* v, uint32_t n) { add(tag, T_SHORT, n, v, n * 2); }
    void addLong(uint16_t tag, uint32_t v) { add(tag, T_LONG, 1, &v, 4); }
    void addLongVec(uint16_t tag, const uint32_t* v, uint32_t n) { add(tag, T_LONG, n, v, n * 4); }
    void addAscii(uint16_t tag, const std::string& s) {
        add(tag, T_ASCII, uint32_t(s.size() + 1), s.c_str(), uint32_t(s.size() + 1));
    }
    void addRationalVec(uint16_t tag, const std::vector<std::pair<uint32_t, uint32_t>>& v) {
        std::vector<uint8_t> b(v.size() * 8);
        for (size_t i = 0; i < v.size(); ++i) {
            uint32_t n = v[i].first, d = v[i].second;
            std::memcpy(b.data() + i * 8, &n, 4);
            std::memcpy(b.data() + i * 8 + 4, &d, 4);
        }
        add(tag, T_RATIONAL, uint32_t(v.size()), b.data(), uint32_t(b.size()));
    }
    void addSRationalVec(uint16_t tag, const std::vector<std::pair<int32_t, int32_t>>& v) {
        std::vector<uint8_t> b(v.size() * 8);
        for (size_t i = 0; i < v.size(); ++i) {
            int32_t n = v[i].first, d = v[i].second;
            std::memcpy(b.data() + i * 8, &n, 4);
            std::memcpy(b.data() + i * 8 + 4, &d, 4);
        }
        add(tag, T_SRATIONAL, uint32_t(v.size()), b.data(), uint32_t(b.size()));
    }

    // pass A：tag 排序 + 计算 >4B 值体的绝对偏移 + IFD 总大小
    // nextIfdOffset 由调用方给定（此时已可计算）；返回 nextIFD 字段在 out 中的位置
    uint32_t plan(uint32_t ifdStart, uint32_t nextIfdOffset) {
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b) { return a.tag < b.tag; });
        uint32_t table = 2 + uint32_t(entries.size()) * 12 + 4;
        uint32_t arena = ifdStart + table;
        for (auto& e : entries) {
            if (e.value.size() > 4) { e.offset = arena; arena += uint32_t(e.value.size()); }
        }
        nextIfdPos_ = ifdStart + 2 + uint32_t(entries.size()) * 12;
        nextIfdValue_ = nextIfdOffset;
        return arena - ifdStart;
    }

    // pass B：在 out[ifdStart] 写 table + 值区
    void write(std::vector<uint8_t>& out, uint32_t ifdStart) {
        auto u16 = [&](size_t pos, uint16_t v) {
            out[pos] = uint8_t(v & 0xFF); out[pos + 1] = uint8_t(v >> 8);
        };
        auto u32 = [&](size_t pos, uint32_t v) {
            out[pos] = uint8_t(v & 0xFF); out[pos + 1] = uint8_t((v >> 8) & 0xFF);
            out[pos + 2] = uint8_t((v >> 16) & 0xFF); out[pos + 3] = uint8_t((v >> 24) & 0xFF);
        };
        uint32_t pos = ifdStart;
        u16(pos, uint16_t(entries.size())); pos += 2;
        for (const auto& e : entries) {
            u16(pos, e.tag); u16(pos + 2, e.type); u32(pos + 4, e.count);
            if (e.value.size() <= 4) {
                std::memcpy(out.data() + pos + 8, e.value.data(), e.value.size());
            } else {
                u32(pos + 8, e.offset);
            }
            pos += 12;
        }
        u32(pos, nextIfdValue_);
        for (const auto& e : entries) {
            if (e.value.size() > 4) std::memcpy(out.data() + e.offset, e.value.data(), e.value.size());
        }
    }

    // 内联 LONG 条目的值改写（SubIFDs/StripOffsets 等）
    bool setInlineLong(uint16_t tag, uint32_t v) {
        for (auto& e : entries) {
            if (e.tag == tag && e.value.size() == 4) {
                e.value[0] = uint8_t(v & 0xFF); e.value[1] = uint8_t((v >> 8) & 0xFF);
                e.value[2] = uint8_t((v >> 16) & 0xFF); e.value[3] = uint8_t((v >> 24) & 0xFF);
                return true;
            }
        }
        return false;
    }

    uint32_t nextIfdValue_ = 0;
    uint32_t nextIfdPos_ = 0;
};

std::string nowAscii() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d:%02d:%02d %02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

uint32_t tiffOrientation(int32_t sensorOrientation) {
    switch (sensorOrientation) {
        case 90: return 6;
        case 180: return 3;
        case 270: return 8;
        default: return 1;
    }
}

// 快速缩略图：bayer 2×2 块聚合 → RGB，再 decimate 到 ~512 宽 + gamma
std::vector<uint8_t> makeThumbnail(const uint8_t* bayer, int32_t w, int32_t h,
                                   int32_t black, int32_t* tw, int32_t* th) {
    const int32_t targetW = 512;
    int32_t step = (w + targetW - 1) / targetW;
    if (step < 2) step = 2;
    int32_t ow = w / step, oh = h / step;
    std::vector<float> acc(ow * oh * 3, 0.f);
    std::vector<uint32_t> cnt(ow * oh, 0);
    // 2×2 CFA 块 → RGB（bayer 为 16-bit LE，按 uint16_t 取样）
    const auto* p16 = reinterpret_cast<const uint16_t*>(bayer);
    for (int32_t y = 0; y + 1 < h; y += 2) {
        for (int32_t x = 0; x + 1 < w; x += 2) {
            auto px = [&](int32_t yy, int32_t xx) {
                int v = p16[size_t(yy) * w + xx];
                return std::max(0, v - black);
            };
            float r = px(y, x), g1 = px(y, x + 1), g2 = px(y + 1, x), b = px(y + 1, x + 1);
            int32_t oy = y / step, ox = x / step;
            size_t idx = size_t(oy) * ow + ox;
            acc[idx * 3 + 0] += r;
            acc[idx * 3 + 1] += (g1 + g2) * 0.5f;
            acc[idx * 3 + 2] += b;
            cnt[idx]++;
        }
    }
    std::vector<uint8_t> rgb(ow * oh * 3);
    for (size_t i = 0; i < size_t(ow) * oh; ++i) {
        if (cnt[i] == 0) continue;
        for (int c = 0; c < 3; ++c) {
            float v = acc[i * 3 + c] / cnt[i] / 511.f;   // 10-bit → 0..1
            v = std::pow(std::clamp(v, 0.f, 1.f), 1.0f / 2.2f);
            rgb[i * 3 + c] = uint8_t(std::clamp(v, 0.f, 1.f) * 255.f);
        }
    }
    *tw = ow; *th = oh;
    return rgb;
}

} // namespace

bool write(const std::string& path, const uint8_t* bayer, size_t len,
           const StaticMeta& sm, const FrameMeta& fm) {
    const size_t expect = size_t(sm.width) * sm.height * 2;
    if (!bayer || len != expect) {
        LOGE("dng: bayer size mismatch (%zu vs %zu)", len, expect);
        return false;
    }

    IfdBuilder ifd0, ifd1;

    // ---- IFD0：元数据 + 缩略图 + SubIFDs → raw IFD ----
    ifd0.addLong(254, 1);                             // NewSubFileType = 1
    int32_t tw = 0, th = 0;
    std::vector<uint8_t> thumb = makeThumbnail(bayer, sm.width, sm.height,
                                               sm.blackLevel[0], &tw, &th);
    ifd0.addLong(0x100, uint32_t(tw));                // ImageWidth
    ifd0.addLong(0x101, uint32_t(th));                // ImageLength
    {
        uint16_t bps[3] = {8, 8, 8};
        ifd0.addShortVec(0x102, bps, 3);              // BitsPerSample
    }
    ifd0.addShort(0x103, 1);                          // Compression = 无压缩
    ifd0.addShort(0x106, 2);                          // Photometric = RGB
    ifd0.addAscii(0x10F, sm.make);
    ifd0.addAscii(0x110, sm.model);
    ifd0.addLong(0x111, 0);                           // StripOffsets（pass A 回填）
    ifd0.addShort(0x115, 3);                          // SamplesPerPixel
    ifd0.addLong(0x116, uint32_t(th));                // RowsPerStrip
    ifd0.addLong(0x117, uint32_t(thumb.size()));      // StripByteCounts
    ifd0.addShort(0x11C, 1);                          // PlanarConfiguration = 1
    ifd0.addShort(0x112, 1);                          // Orientation（缩略图按 1 输出）
    ifd0.addRationalVec(0x11A, {{72, 1}});            // XResolution（0x11A）
    ifd0.addRationalVec(0x11B, {{72, 1}});            // YResolution（0x11B）
    ifd0.addLong(0x14A, 0);                           // SubIFDs（pass A 回填 ifd1Offset）
    ifd0.addAscii(0x131, sm.software);
    ifd0.addAscii(0x132, nowAscii());
    {
        uint8_t v[4] = {1, 4, 0, 0};
        ifd0.add(0xC612, T_BYTE, 4, v, 4);            // DNGVersion 1.4.0.0
        uint8_t b[4] = {1, 1, 0, 0};
        ifd0.add(0xC613, T_BYTE, 4, b, 4);            // DNGBackwardVersion 1.1.0.0
    }
    ifd0.addAscii(0xC614, sm.model);                  // UniqueCameraModel

    // ---- IFD1：raw CFA 数据 ----
    ifd1.addLong(254, 0);                             // NewSubFileType = 0（raw）
    ifd1.addLong(0x100, uint32_t(sm.width));
    ifd1.addLong(0x101, uint32_t(sm.height));
    ifd1.addShort(0x102, 16);                         // BitsPerSample
    ifd1.addShort(0x103, 1);                          // Compression = 无压缩
    ifd1.addShort(0x106, 32803);                      // Photometric = CFA
    ifd1.addAscii(0x10F, sm.make);
    ifd1.addAscii(0x110, sm.model);
    ifd1.addLong(0x111, 0);                           // StripOffsets（pass A 回填）
    ifd1.addShort(0x112, uint16_t(tiffOrientation(sm.sensorOrientation)));
    ifd1.addShort(0x115, 1);                          // SamplesPerPixel
    ifd1.addLong(0x116, uint32_t(sm.height));         // RowsPerStrip（单条带）
    ifd1.addLong(0x117, uint32_t(len));               // StripByteCounts
    ifd1.addShort(0x11C, 1);                          // PlanarConfiguration
    ifd1.addAscii(0x131, sm.software);
    ifd1.addAscii(0x132, nowAscii());
    {
        uint16_t iso = uint16_t(std::clamp(fm.iso, 0, 65535));
        ifd1.addShort(0x8827, iso);                   // ISOSpeedRatings
        ifd1.addRationalVec(0x829A, {{uint32_t(std::max<int64_t>(fm.exposureNs, 1)),
                                       1000000000u}});  // ExposureTime
    }
    {
        uint16_t dim[2] = {2, 2};
        ifd1.addShortVec(0x828D, dim, 2);             // CFARepeatPatternDim (33421)
        uint16_t pat[4] = {sm.cfaPattern[0], sm.cfaPattern[1], sm.cfaPattern[2], sm.cfaPattern[3]};
        ifd1.addShortVec(0x828E, pat, 4);             // CFAPattern (33422)
    }
    ifd1.addShort(0xC611, 1);                         // CFALayout = 方形
    {
        std::vector<std::pair<int32_t, int32_t>> cm;
        for (int i = 0; i < 9; ++i)
            cm.push_back({int32_t(std::lround(sm.colorMatrix1[i] * 10000.0f)), 10000});
        ifd1.addSRationalVec(0xC621, cm);             // ColorMatrix1（占位，M2.4 校准）
    }
    {
        std::vector<std::pair<uint32_t, uint32_t>> neutral;
        const float g[3] = {fm.wbGains[0], fm.wbGains[1], fm.wbGains[3]};
        for (int i = 0; i < 3; ++i) {
            float v = g[i] > 1e-6f ? 1.0f / g[i] : 1.0f;
            neutral.push_back({uint32_t(std::lround(v * 1000000.0f)), 1000000});
        }
        ifd1.addRationalVec(0xC628, neutral);         // AsShotNeutral
    }
    ifd1.addShort(0xC65A, 21);                        // CalibrationIlluminant1 = D65
    {
        std::vector<std::pair<uint32_t, uint32_t>> bl;
        for (int i = 0; i < 4; ++i) bl.push_back({uint32_t(sm.blackLevel[i]), 1});
        ifd1.addRationalVec(0xC61A, bl);              // BlackLevel（RGGB 四通道）
    }
    ifd1.addLong(0xC61D, uint32_t(sm.whiteLevel));    // WhiteLevel
    {
        uint32_t origin[2] = {0, 0};
        ifd1.addLongVec(0xC61F, origin, 2);           // DefaultCropOrigin
        uint32_t size[2] = {uint32_t(sm.width), uint32_t(sm.height)};
        ifd1.addLongVec(0xC620, size, 2);             // DefaultCropSize
        uint32_t area[4] = {0, 0, uint32_t(sm.height), uint32_t(sm.width)};
        ifd1.addLongVec(0xC68D, area, 4);             // ActiveArea
    }

    // ---- pass A：偏移计算（此时 ifd1Offset/dataOffset 已知并回填内联值）----
    uint32_t ifd0Start = 8;
    uint32_t ifd0Size = ifd0.plan(ifd0Start, 0);
    ifd0.setInlineLong(0x14A, ifd0Start + ifd0Size);  // SubIFDs → IFD1
    uint32_t ifd1Start = ifd0Start + ifd0Size;
    uint32_t ifd1Size = ifd1.plan(ifd1Start, 0);
    ifd1.setInlineLong(0x111, ifd1Start + ifd1Size);  // StripOffsets → 像素数据
    uint32_t thumbOffset = ifd1Start + ifd1Size;
    ifd0.setInlineLong(0x111, thumbOffset);           // 缩略图 StripOffsets
    uint32_t dataStart = thumbOffset + uint32_t(thumb.size());

    // ---- pass B：序列化 ----
    std::vector<uint8_t> out;
    out.resize(dataStart + len);   // ★ 含像素数据区（此前漏加 len 导致 memcpy 越界）
    out[0] = 'I'; out[1] = 'I';
    out[2] = 0x2A; out[3] = 0x00;
    out[4] = 0x08; out[5] = 0x00; out[6] = 0x00; out[7] = 0x00;
    ifd0.write(out, ifd0Start);
    ifd1.write(out, ifd1Start);
    std::memcpy(out.data() + thumbOffset, thumb.data(), thumb.size());
    std::memcpy(out.data() + dataStart, bayer, len);

    std::ofstream f(path, std::ios::binary);
    if (!f) { LOGE("dng: cannot open %s", path.c_str()); return false; }
    f.write(reinterpret_cast<const char*>(out.data()), out.size());
    if (!f) { LOGE("dng: write failed %s", path.c_str()); return false; }
    LOGI("dng saved: %s (%zu bytes, thumb %dx%d)", path.c_str(), out.size(), tw, th);
    return true;
}

} // namespace optic::dng
