#include "core/capture/StillCapture.h"

#include "core/util/Log.h"

#include <android/native_window.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace optic::capture {

bool StillCapture::create(int32_t w, int32_t h, const std::string& saveDir, int maxImages) {
    w_ = w;
    h_ = h;
    dir_ = saveDir;
    std::error_code ec;
    std::filesystem::create_directories(dir_ + "/jpg", ec);

    // AIMAGE_FORMAT_JPEG = 0x100：HAL ISP 直出压缩码流（YUV420 也是后续滤镜通路的备选）
    if (AImageReader_new(w, h, AIMAGE_FORMAT_JPEG, maxImages, &reader_) != AMEDIA_OK || !reader_) {
        LOGE("AImageReader_new(JPEG %dx%d) failed", w, h);
        return false;
    }
    listener_ = {this, &StillCapture::onImageAvailable};
    if (AImageReader_setImageListener(reader_, &listener_) != AMEDIA_OK) {
        LOGE("StillCapture setImageListener failed");
        return false;
    }
    setProcessor(std::make_unique<PassThroughProcessor>());
    saver_ = std::thread([this] { saverLoop(); });
    LOGI("jpeg reader ready: %dx%d maxImages=%d dir=%s", w, h, maxImages, saveDir.c_str());
    return true;
}

void StillCapture::close() {
    {
        std::lock_guard<std::mutex> l(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (saver_.joinable()) saver_.join();

    std::lock_guard<std::mutex> l(m_);
    if (reader_) {
        AImageReader_setImageListener(reader_, nullptr);
        for (auto& f : saveQ_)
            if (f.img) AImage_delete(f.img);
        saveQ_.clear();
        AImageReader_delete(reader_);
        reader_ = nullptr;
    }
}

ANativeWindow* StillCapture::window() {
    ANativeWindow* win = nullptr;
    if (reader_) AImageReader_getWindow(reader_, &win);
    return win;
}

void StillCapture::expectShot(const StillParams& params) {
    std::lock_guard<std::mutex> l(m_);
    pending_.push_back(params);
    inFlight_.fetch_add(1, std::memory_order_acq_rel);
    LOGI("jpeg shot pending: %d in flight", inFlight_.load(std::memory_order_relaxed));
}

void StillCapture::onImageAvailable(void* ctx, AImageReader* reader) {
    auto* self = static_cast<StillCapture*>(ctx);
    AImage* img = nullptr;
    if (AImageReader_acquireNextImage(reader, &img) != AMEDIA_OK || !img) return;
    self->onImage(img);
}

void StillCapture::onImage(AImage* img) {
    // repeating 不用本流：到帧必是单拍请求的产物。未登记的帧（会话残留/时序差）
    // 直接丢弃。pending 队列与 captureOnce 的串行约束（引擎保证节奏）一一对应。
    std::lock_guard<std::mutex> l(m_);
    if (pending_.empty()) {
        AImage_delete(img);
        return;
    }
    int64_t ts = 0;
    AImage_getTimestamp(img, &ts);
    saveQ_.push_back({img, ts, pending_.front()});
    pending_.pop_front();
    cv_.notify_one();
}

void StillCapture::saverLoop() {
    for (;;) {
        JpegFrame f;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [this] { return stop_ || !saveQ_.empty(); });
            if (stop_ && saveQ_.empty()) return;
            f = std::move(saveQ_.front());
            saveQ_.pop_front();
        }
        // 出队进入写盘：进度仍计 1（在飞），写完（含失败/丢弃）才减
        auto finish = [&](bool saved) {
            if (saved)
                lastSavedMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count(),
                                   std::memory_order_release);
            inFlight_.fetch_sub(1, std::memory_order_acq_rel);
        };

        StillFrame sf;
        sf.fmt = StillFrame::Fmt::JpegBlob;
        sf.w = w_;
        sf.h = h_;
        sf.tsNs = f.imgTs;
        sf.params = f.params;

        uint8_t* data = nullptr;
        int len = 0;
        AImage_getPlaneData(f.img, 0, &data, &len);
        if (data && len > 0) sf.blob.assign(data, data + len);
        if (f.img) AImage_delete(f.img);

        // 处理管线（当前 PassThrough；滤镜系统 = 实现 StillProcessor 替换/串联）
        if (proc_ && !proc_->process(sf)) {
            LOGI("still frame dropped by processor");
            finish(false);
            continue;
        }
        if (sf.blob.empty()) {
            LOGE("still frame empty blob, skipped");
            finish(false);
            continue;
        }

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
        char name[80];
        snprintf(name, sizeof(name), "IMG_%lld_%03d.jpg", static_cast<long long>(ms),
                 seq_++);

        // 首选：MediaStore 贡献到 DCIM/Camera（与系统相机同目录，相册可见）
        if (gallery_ && gallery_->ok() &&
            gallery_->saveJpeg(name, sf.blob.data(), sf.blob.size())) {
            LOGI("jpg saved to gallery/DCIM: %s (%d KB, %dx%d)", name,
                 int(sf.blob.size() / 1024), w_, h_);
            finish(true);
            continue;
        }
        // 回退：应用私有目录（无 JNI 注入 / MediaStore 写入失败）
        const std::string path = dir_ + "/jpg/" + name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(reinterpret_cast<const char*>(sf.blob.data()),
                               static_cast<std::streamsize>(sf.blob.size()))) {
            LOGE("jpg write failed: %s", path.c_str());
            finish(false);
            continue;
        }
        LOGI("jpg saved: %s (%d KB, %dx%d)", path.c_str(), int(sf.blob.size() / 1024),
             w_, h_);
        finish(true);
    }
}

} // namespace optic::capture
