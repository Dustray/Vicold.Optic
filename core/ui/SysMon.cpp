#include "core/ui/SysMon.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>

namespace optic::ui {

namespace {

// 读单个长整数；fopen "re"（close-on-exec），失败返回 false
bool readLL(const char* path, long long* out) {
    FILE* f = fopen(path, "re");
    if (!f) return false;
    const int n = fscanf(f, "%lld", out);
    fclose(f);
    return n == 1;
}

// 读一行字符串并去尾部换行
bool readStr(const char* path, char* buf, int cap) {
    FILE* f = fopen(path, "re");
    if (!f) return false;
    const bool ok = fgets(buf, cap, f) != nullptr;
    fclose(f);
    if (ok) {
        size_t l = strlen(buf);
        while (l && (buf[l - 1] == '\n' || buf[l - 1] == ' ' || buf[l - 1] == '\r'))
            buf[--l] = 0;
    }
    return ok;
}

void toLower(char* s) {
    for (; *s; ++s)
        if (*s >= 'A' && *s <= 'Z') *s += 32;
}

// 温度自适应：主流平台是 m°C（如 84000），个别平台已是 °C
float toC(long long v) {
    const float f = float(v);
    return (f > 1000.f || f < -1000.f) ? f / 1000.f : f;
}

// GPU 频率自适应：kgsl gpuclk 为 Hz；通用节点可能是 kHz 或 MHz
float gpuToMHz(long long v) {
    const float f = float(v);
    if (f > 1e8f) return f / 1e6f;  // Hz
    if (f > 1e5f) return f / 1e3f;  // kHz
    return f;                       // MHz
}

}  // namespace

void SysMon::probe() {
    probed_ = true;

    // 1) thermal zone：按 type 名匹配 cpu / gpu，缓存 temp 路径
    if (DIR* d = opendir("/sys/class/thermal")) {
        while (dirent* e = readdir(d)) {
            if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
            char base[160], tyPath[192], ty[64];
            snprintf(base, sizeof(base), "/sys/class/thermal/%s", e->d_name);
            snprintf(tyPath, sizeof(tyPath), "%s/type", base);
            if (!readStr(tyPath, ty, sizeof(ty))) continue;
            toLower(ty);
            if (strstr(ty, "trip")) continue;  // hw-trip 是降频阈值，不是实测温度
            char tmp[192];
            snprintf(tmp, sizeof(tmp), "%s/temp", base);
            if (cpuTempPath_.empty() && strstr(ty, "cpu")) cpuTempPath_ = tmp;
            if (gpuTempPath_.empty() && strstr(ty, "gpu")) gpuTempPath_ = tmp;
        }
        closedir(d);
    }

    // 2) CPU 每核频率（kHz）；在线核才可读，读不到的自然跳过
    for (int i = 0; i < 16; ++i) {
        char p[128];
        snprintf(p, sizeof(p),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        long long v;
        if (readLL(p, &v)) cpuFreqPaths_.emplace_back(p);
    }

    // 3) GPU 频率 / busy：候选路径取第一个可读者
    const char* freqCands[] = {
        "/sys/class/kgsl/kgsl-3d0/gpuclk",
        "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq",
        "/sys/kernel/gpu/gpu_clock",
    };
    for (const char* c : freqCands) {
        long long v;
        if (readLL(c, &v)) {
            gpuFreqPath_ = c;
            break;
        }
    }
    const char* busyCands[] = {
        "/sys/class/kgsl/kgsl-3d0/gpubusy",
        "/sys/kernel/gpu/gpu_busy",
    };
    for (const char* c : busyCands) {
        if (FILE* f = fopen(c, "re")) {
            fclose(f);
            gpuBusyPath_ = c;
            break;
        }
    }
}

void SysMon::sample() {
    if (!probed_) probe();

    long long v;
    s_.cpuMaxMHz = -1.f;
    for (const auto& p : cpuFreqPaths_)
        if (readLL(p.c_str(), &v))
            s_.cpuMaxMHz = std::max(s_.cpuMaxMHz, float(v) / 1000.f);

    s_.cpuTempC = cpuTempPath_.empty() ? -1.f
                  : (readLL(cpuTempPath_.c_str(), &v) ? toC(v) : -1.f);
    s_.gpuTempC = gpuTempPath_.empty() ? -1.f
                  : (readLL(gpuTempPath_.c_str(), &v) ? toC(v) : -1.f);
    s_.gpuMHz = gpuFreqPath_.empty() ? -1.f
                : (readLL(gpuFreqPath_.c_str(), &v) ? gpuToMHz(v) : -1.f);

    // gpubusy：累计计数差分 → 占用率（首轮回 -1，第二轮起有效）
    s_.gpuBusyPct = -1.f;
    if (!gpuBusyPath_.empty()) {
        long long a = -1, b = -1;
        if (FILE* f = fopen(gpuBusyPath_.c_str(), "re")) {
            const int n = fscanf(f, "%lld %lld", &a, &b);
            fclose(f);
            if (n == 2 && busyPrevA_ >= 0 && b > busyPrevB_ && a >= busyPrevA_ &&
                b >= a)
                s_.gpuBusyPct =
                    std::clamp(100.f * float(a - busyPrevA_) / float(b - busyPrevB_),
                               0.f, 100.f);
            if (n == 2) {
                busyPrevA_ = a;
                busyPrevB_ = b;
            }
        }
    }
}

}  // namespace optic::ui
