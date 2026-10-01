#pragma once
// JPEG（普通照片）采集：JPEG_FORMAT AImageReader + 单拍保存。
// 与 RawCapture 平行：两者互斥挂会话输出（fmt=raw|jpg 切换），结构镜像以便维护。
//  - HAL 直出 JPEG（ISP 全处理 + EXIF 内嵌），落盘 = 码流直写（→ 系统相册 DCIM/Camera）；
//  - JPEG 编码极贵（12MP ISP 编码），**不能进 repeating**（每帧编码会把预览拖到
//    18.5fps，2026-09-30 真机确诊）——流只配置进会话，快门时引擎发单拍请求，
//    到帧即存。与 RAW 环（DMA 便宜、ZSL 回溯）机制刻意不同。
//  - 处理管线见 StillPipeline.h：PassThrough 起步，滤镜系统后续经 StillProcessor 接入。

#include <media/NdkImageReader.h>
#include <media/NdkImage.h>

#include <condition_variable>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/capture/StillPipeline.h"
#include "core/util/Gallery.h"

namespace optic::capture {

class StillCapture {
public:
    bool create(int32_t w, int32_t h, const std::string& saveDir, int maxImages = 5);
    void close();

    ANativeWindow* window(); // reader 的 window，作为会话输出目标（与 raw_->window() 互斥）

    // 快门登记：下一张到达的帧按该参数快照保存（与 captureOnce 单拍配套；
    // 未登记时到达的帧直接丢弃——repeating 不用此流，正常只有单拍出帧）
    void expectShot(const StillParams& params);
    // 撤销最后一次登记（captureOnce 提交失败时；最后一份才可撤——单拍串行）
    void cancelShot() {
        std::lock_guard<std::mutex> l(m_);
        if (!pending_.empty()) {
            pending_.pop_back();
            inFlight_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    void setProcessor(std::unique_ptr<StillProcessor> p) {
        std::lock_guard<std::mutex> l(m_);
        proc_ = std::move(p);
    }
    // 相册写入（MediaStore → DCIM/Camera，系统相册可见）；未注入或失败时
    // 回退到应用私有目录 jpg/
    void setGallery(util::GalleryWriter* g) { gallery_ = g; }

    // ---- 保存进度（引擎每轮转发给 UI；原子量，saver 线程写）----
    // 在飞帧数 = 已登记待到帧 + 已到帧待写盘
    int savePending() const { return inFlight_.load(std::memory_order_acquire); }
    // 最近一次写盘完成时刻（system_clock 毫秒；0 = 从未）。UI 用它区分"新完成"
    int64_t lastSavedMs() const { return lastSavedMs_.load(std::memory_order_acquire); }

private:
    static void onImageAvailable(void* ctx, AImageReader* reader);
    void onImage(AImage* img);
    void saverLoop();

    struct JpegFrame {
        AImage* img = nullptr;
        int64_t imgTs = 0;
        StillParams params;   // 快门时刻参数快照（随帧走）
    };

    AImageReader* reader_ = nullptr;
    AImageReader_ImageListener listener_{};
    int32_t w_ = 0, h_ = 0;
    std::string dir_;
    util::GalleryWriter* gallery_ = nullptr;

    mutable std::mutex m_;
    std::deque<JpegFrame> saveQ_;
    // 已登记待到帧的快门快照（含登记时刻；超时未到帧自动作废 —— 物理 JPEG 流
    // 单拍若被 HAL 拒绝交付，pending 会永远挂住「正在保存」进度）
    struct PendingShot {
        StillParams params;
        int64_t expectMs = 0;
    };
    std::deque<PendingShot> pending_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::unique_ptr<StillProcessor> proc_;

    // 保存进度（无锁读：UI 每帧轮询）
    std::atomic<int> inFlight_{0};      // pending_ + saveQ_ 总数
    std::atomic<int64_t> lastSavedMs_{0};

    std::thread saver_;
};

} // namespace optic::capture
