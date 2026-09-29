#include "core/capture/RawCapture.h"

#include "core/util/Log.h"

#include <android/native_window.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace optic::capture {

bool RawCapture::create(int32_t w, int32_t h, const std::string& saveDir, int maxImages) {
    w_ = w;
    h_ = h;
    dir_ = saveDir;
    std::error_code ec;
    std::filesystem::create_directories(dir_ + "/dng", ec);

    // 0x20 = RAW_SENSOR（NDK 未导出该枚举名，值同 ImageFormat.RAW_SENSOR）
    if (AImageReader_new(w, h, 0x20, maxImages, &reader_) != AMEDIA_OK || !reader_) {
        LOGE("AImageReader_new(RAW %dx%d) failed", w, h);
        return false;
    }
    listener_ = {this, &RawCapture::onImageAvailable};
    if (AImageReader_setImageListener(reader_, &listener_) != AMEDIA_OK) {
        LOGE("setImageListener failed");
        return false;
    }
    saver_ = std::thread([this] { saverLoop(); });
    LOGI("raw reader ready: %dx%d maxImages=%d dir=%s", w, h, maxImages, saveDir.c_str());
    return true;
}

void RawCapture::close() {
    {
        std::lock_guard<std::mutex> l(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (saver_.joinable()) saver_.join();

    std::lock_guard<std::mutex> l(m_);
    if (reader_) {
        AImageReader_setImageListener(reader_, nullptr);
        for (auto& f : ring_)
            if (f.img) AImage_delete(f.img);
        ring_.clear();
        for (auto& f : saveQ_)
            if (f.img) AImage_delete(f.img);
        saveQ_.clear();
        AImageReader_delete(reader_);
        reader_ = nullptr;
    }
}

ANativeWindow* RawCapture::window() {
    ANativeWindow* win = nullptr;
    if (reader_) AImageReader_getWindow(reader_, &win);
    return win;
}

void RawCapture::onFrameResult(const FrameResult& r) {
    std::lock_guard<std::mutex> l(m_);
    meta_.emplace_back(r.timestampNs, r);
    while (meta_.size() > 64) meta_.pop_front();
}

void RawCapture::setRingMode(bool on) {
    std::lock_guard<std::mutex> l(m_);
    ringMode_ = on;
}

bool RawCapture::ringMode() const {
    std::lock_guard<std::mutex> l(m_);
    return ringMode_;
}

void RawCapture::shutterBurst(int n) {
    std::lock_guard<std::mutex> l(m_);
    int cnt = std::min<int>(n, static_cast<int>(ring_.size()));
    for (int i = 0; i < cnt; ++i) {
        saveQ_.push_back(ring_.back());
        ring_.pop_back();
    }
    LOGI("ZSL shutter: %d frames queued for save", cnt);
    cv_.notify_one();
}

void RawCapture::requestSingle() {
    std::lock_guard<std::mutex> l(m_);
    singlePending_ = 1;
}

double RawCapture::rawFps() const {
    std::lock_guard<std::mutex> l(m_);
    if (winStart_ <= 0 || lastTs_ <= winStart_) return 0.0;
    return static_cast<double>(winFrames_) * 1e9 / static_cast<double>(lastTs_ - winStart_);
}

int64_t RawCapture::rawCount() const {
    std::lock_guard<std::mutex> l(m_);
    return total_;
}

void RawCapture::onImageAvailable(void* ctx, AImageReader* reader) {
    auto* self = static_cast<RawCapture*>(ctx);
    AImage* img = nullptr;
    if (AImageReader_acquireNextImage(reader, &img) != AMEDIA_OK || !img) return;
    self->onImage(img);
}

void RawCapture::onImage(AImage* img) {
    int64_t ts = 0;
    AImage_getTimestamp(img, &ts);

    std::lock_guard<std::mutex> l(m_);
    total_++;

    // 滚动 2s 窗口统计 RAW 通路实际帧率
    if (winStart_ == 0) {
        winStart_ = ts;
        winFrames_ = 1;
    } else {
        winFrames_++;
        if (ts > lastTs_) lastTs_ = ts;
        if (lastTs_ - winStart_ >= static_cast<int64_t>(2e9)) {
            winStart_ = lastTs_;
            winFrames_ = 1;
        }
    }

    if (!ringMode_) {
        if (singlePending_ > 0) {
            singlePending_ = 0;
            saveQ_.push_back({img, ts});
            cv_.notify_one();
            return;
        }
        AImage_delete(img); // once 模式下的多余帧直接回收
        return;
    }

    ring_.push_back({img, ts});
    while (ring_.size() > kRingFrames) {
        if (ring_.front().img) AImage_delete(ring_.front().img);
        ring_.pop_front();
    }
}

void RawCapture::saverLoop() {
    for (;;) {
        RawFrame f;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [this] { return stop_ || !saveQ_.empty(); });
            if (stop_ && saveQ_.empty()) return;
            f = std::move(saveQ_.front());
            saveQ_.pop_front();
        }

        // ★ 元数据配对在落盘时进行：此刻该帧的结果元数据早已到达（晚约 2 帧）
        FrameResult meta;
        {
            std::lock_guard<std::mutex> l(m_);
            bool paired = false;
            for (auto it = meta_.rbegin(); it != meta_.rend(); ++it) {
                if (std::llabs(it->first - f.imgTs) < 20000000LL) {
                    meta = it->second;
                    paired = true;
                    break;
                }
            }
            if (!paired && !meta_.empty()) {
                meta = meta_.back().second;
                if (f.imgTs - lastPairWarn_ > 3000000000LL) {
                    lastPairWarn_ = f.imgTs;
                    LOGW("meta pairing miss: imgTs=%lld nearestMetaTs=%lld delta=%lldms",
                         static_cast<long long>(f.imgTs),
                         static_cast<long long>(meta_.back().first),
                         static_cast<long long>(std::llabs(meta_.back().first - f.imgTs)) / 1000000);
                }
            }
        }

        uint8_t* data = nullptr;
        int len = 0, rowStride = 0, pixStride = 0;
        AImage_getPlaneData(f.img, 0, &data, &len);
        AImage_getPlaneRowStride(f.img, 0, &rowStride);
        AImage_getPlanePixelStride(f.img, 0, &pixStride);

        std::vector<uint8_t> buf;
        const size_t tight = static_cast<size_t>(w_) * h_ * pixStride;
        if (data && len > 0) {
            if (rowStride == w_ * pixStride && static_cast<size_t>(len) >= tight) {
                buf.assign(data, data + tight);
            } else {
                // 行有 padding：逐行拷贝
                buf.reserve(tight);
                for (int r = 0; r < h_; ++r) {
                    const uint8_t* row = data + static_cast<size_t>(r) * rowStride;
                    buf.insert(buf.end(), row, row + static_cast<size_t>(w_) * pixStride);
                }
            }
        }

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
        char name[64];
        snprintf(name, sizeof(name), "dng_%lld_%03d", static_cast<long long>(ms), seq_++);
        std::string base = dir_ + "/dng/" + name;

        dng::FrameMeta fm;
        fm.timestampNs = f.imgTs;
        fm.iso = meta.iso;
        fm.exposureNs = meta.exposureNs;
        std::memcpy(fm.wbGains, meta.wbGains, sizeof(fm.wbGains));

        if (!dng::write(base + ".dng", buf.data(), buf.size(), sm_, fm)) {
            LOGE("dng write failed: %s", base.c_str());
        }
        if (f.img) AImage_delete(f.img);
    }
}

} // namespace optic::capture
