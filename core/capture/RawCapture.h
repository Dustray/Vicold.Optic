#pragma once
// M1.5/M1.6：RAW_SENSOR AImageReader 通路 + ZSL 环形缓冲。
// 两种模式（controls.txt: raw_mode=ring|once）：
//  - ring（默认，ZSL）：repeating 请求常驻 RAW 目标，缓存最近 4 帧；快门回溯保存
//  - once：repeating 不含 RAW，单拍时临时加 RAW 目标，收到即存
// 保存线程异步落盘：raw_<ms>_<seq>.raw（16bit bayer，tight 打包）+ 同名 .txt 元数据。

#include <media/NdkImageReader.h>
#include <media/NdkImage.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/capture/CaptureSession.h"

namespace optic::capture {

class RawCapture {
public:
    bool create(int32_t w, int32_t h, const std::string& saveDir, int maxImages = 6);
    void close();

    ANativeWindow* window(); // reader 的 window，作为会话输出目标
    void onFrameResult(const FrameResult& r);

    void setRingMode(bool on);
    bool ringMode() const;
    static constexpr int kRingFrames = 4;

    void shutterBurst(int n); // ZSL 快门：回溯最近 n 帧
    void requestSingle();     // once 模式：保存下一帧

    double rawFps() const; // 实测 RAW 通路帧率（滚动 2s 窗口）
    int64_t rawCount() const;

private:
    static void onImageAvailable(void* ctx, AImageReader* reader);
    void onImage(AImage* img);
    void saverLoop();

    struct RawFrame {
        AImage* img = nullptr;
        FrameResult meta;
    };

    AImageReader* reader_ = nullptr;
    AImageReader_ImageListener listener_{};
    int32_t w_ = 0, h_ = 0;
    std::string dir_;
    int seq_ = 0;

    mutable std::mutex m_;
    std::deque<RawFrame> ring_;
    std::deque<RawFrame> saveQ_;
    std::deque<std::pair<int64_t, FrameResult>> meta_; // 时间戳 -> 结果元数据
    std::condition_variable cv_;
    bool stop_ = false;
    bool ringMode_ = true;
    int singlePending_ = 0;
    int64_t winStart_ = 0, lastTs_ = 0, total_ = 0;
    int64_t lastPairWarn_ = 0; // 配对未命中打点限频
    int winFrames_ = 0;

    std::thread saver_;
};

} // namespace optic::capture
