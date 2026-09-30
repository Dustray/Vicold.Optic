#pragma once
// 系统相册写入：经 MediaStore.Images 把 JPEG 贡献到 DCIM/Camera（与系统相机同目录），
// 打开系统相册即可见。API 29+ 应用向媒体库贡献自己的媒体无需任何存储权限；
// 直接文件路径写 DCIM 在 scoped storage 下行不通，必须走 MediaStore。
// init 在 android_main 注入 (JavaVM, Activity)；saveJpeg 在保存线程阻塞调用
// （内部按需 AttachCurrentThread，重复 attach 返回缓存 env）。

#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace optic::util {

class GalleryWriter {
public:
    void init(JavaVM* vm, jobject activity);   // android_main 线程调用一次

    bool ok() const { return ok_; }

    // displayName 需带 ".jpg"；重复名由 MediaStore 自动去重（追加 " (n)"）。
    // 失败返回 false（调用方回退到应用私有目录落盘）。
    bool saveJpeg(const std::string& displayName, const uint8_t* data, size_t len);

private:
    JavaVM* vm_ = nullptr;
    jobject activity_ = nullptr;   // 全局引用（NativeActivity）
    bool ok_ = false;
};

} // namespace optic::util
