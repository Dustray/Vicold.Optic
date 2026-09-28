#pragma once
// 手动控制下发的过渡方案（M7 换触控 UI）：轮询 app 外部目录里的 controls.txt。
// 每行 key=value；内容与上次相同则不重复上报（对 sdcard/FUSE 的 mtime 抖动免疫——
// 曾因 mtime 比较失效导致 zsl_shutter 每 100ms 重触发、20 分钟写掉 155GB RAW）。
// 一次性命令（zsl_shutter/shot_raw/fps_log）由引擎处理后从文件中移除（自消费）。
//
// adb 用法：
//   adb shell "echo 'zsl_shutter=4' > /sdcard/Android/data/com.vicold.optic/files/controls.txt"

#include <chrono>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>

namespace optic::util {

class ControlFile {
public:
    explicit ControlFile(std::string path,
                         std::chrono::milliseconds pollInterval = std::chrono::milliseconds(400))
        : path_(std::move(path)), interval_(pollInterval) {}

    // 到期时读取；内容相对上次有变化才返回新键值表，否则 nullopt
    std::optional<std::unordered_map<std::string, std::string>> poll() {
        auto now = std::chrono::steady_clock::now();
        if (now - lastPoll_ < interval_) return std::nullopt;
        lastPoll_ = now;

        std::ifstream in(path_);
        if (!in) return std::nullopt;

        std::unordered_map<std::string, std::string> kv;
        std::string line;
        while (std::getline(in, line)) {
            auto pos = line.find('=');
            if (pos == std::string::npos) continue;
            auto key = trim(line.substr(0, pos));
            auto val = trim(line.substr(pos + 1));
            if (!key.empty()) kv[key] = val;
        }
        if (hasLast_ && kv == lastKv_) return std::nullopt; // 内容未变
        lastKv_ = kv;
        hasLast_ = true;
        return kv;
    }

    static std::string trim(const std::string& s) {
        auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return {};
        auto e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

private:
    std::string path_;
    std::chrono::milliseconds interval_;
    std::chrono::steady_clock::time_point lastPoll_{};
    std::unordered_map<std::string, std::string> lastKv_;
    bool hasLast_ = false;
};

} // namespace optic::util
