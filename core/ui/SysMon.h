#pragma once
// SysMon —— CPU/GPU 频率与温度的 sysfs 采样，供预览 HUD 调试显示（与 fps 同行）。
// 通用 Android 探测，无机型硬编码：
//  - CPU/GPU 温度：首次枚举 /sys/class/thermal/thermal_zone*/type，按名字含
//    "cpu"/"gpu" 匹配并缓存 temp 路径（跳过 *trip* 阈值节点）；
//  - CPU 频率：cpuN/cpufreq/scaling_cur_freq（kHz）取最大核；
//  - GPU 频率/占用：候选路径探测（高通 kgsl 优先，通用节点兜底），占用率用
//    gpubusy 计数差分；读不到的项保持负值，UI 显示 "--"。
// 后续 Mali 等其它 SoC 只需在 SysMon.cpp 候选表追加路径即可。
#include <string>
#include <vector>

namespace optic::ui {

class SysMon {
public:
    struct Sample {
        float cpuMaxMHz = -1.f;   // 最大 CPU 核频 (MHz)
        float cpuTempC = -1.f;    // CPU 温度 (°C)
        float gpuMHz = -1.f;      // GPU 频率 (MHz)
        float gpuTempC = -1.f;    // GPU 温度 (°C)
        float gpuBusyPct = -1.f;  // GPU 占用率 (%)，需两轮差分
    };

    // 读一轮 sysfs（首次调用探测并缓存路径；单轮 ~10 个小文件读，2Hz 无压力）
    void sample();
    const Sample& last() const { return s_; }

private:
    void probe();

    Sample s_;
    bool probed_ = false;

    // 探测产物（缓存，避免每轮枚举上百个 thermal zone）
    std::vector<std::string> cpuFreqPaths_;  // 每核 scaling_cur_freq
    std::string cpuTempPath_;                // thermal_zoneN/temp（type 含 cpu）
    std::string gpuTempPath_;                // thermal_zoneN/temp（type 含 gpu）
    std::string gpuFreqPath_;                // 频率候选中第一个可读者
    std::string gpuBusyPath_;                // gpubusy（差分求占用率）
    long long busyPrevA_ = -1, busyPrevB_ = -1;
};

}  // namespace optic::ui
