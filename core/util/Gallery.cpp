#include "core/util/Gallery.h"

#include "core/util/Log.h"

#include <chrono>

namespace optic::util {

namespace {
// 安全查找：pending exception 不清会 abort 下一次 JNI 调用，查完立即清。
jmethodID lookupMethod(JNIEnv* env, jclass cls, const char* name, const char* sig,
                       bool isStatic) {
    if (!cls) return nullptr;
    jmethodID m = isStatic ? env->GetStaticMethodID(cls, name, sig)
                           : env->GetMethodID(cls, name, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return m;
}
jfieldID lookupField(JNIEnv* env, jclass cls, const char* name, const char* sig,
                     bool isStatic) {
    if (!cls) return nullptr;
    jfieldID f = isStatic ? env->GetStaticFieldID(cls, name, sig)
                          : env->GetFieldID(cls, name, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return f;
}
} // namespace

void GalleryWriter::init(JavaVM* vm, jobject activity) {
    if (!vm || !activity) return;
    vm_ = vm;
    JNIEnv* env = nullptr;
    if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    activity_ = env->NewGlobalRef(activity);
    ok_ = activity_ != nullptr;
    LOGI("gallery writer init: ok=%d", ok_);
}

bool GalleryWriter::saveJpeg(const std::string& displayName, const uint8_t* data,
                             size_t len) {
    if (!ok_ || !vm_ || !data || len == 0) return false;
    JNIEnv* env = nullptr;
    if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return false;

    // Java 等价：
    //   ContentValues cv = new ContentValues();
    //   cv.put(DISPLAY_NAME, name); cv.put(MIME_TYPE, "image/jpeg");
    //   cv.put(RELATIVE_PATH, "DCIM/Camera"); cv.put(DATE_TAKEN, ms);
    //   cv.put(IS_PENDING, 1);
    //   Uri uri = cr.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, cv);
    //   OutputStream os = cr.openOutputStream(uri); os.write(buf); os.close();
    //   cv.clear(); cv.put(IS_PENDING, 0); cr.update(uri, cv, null, null);

    jclass cvCls = env->FindClass("android/content/ContentValues");
    if (env->ExceptionCheck()) { env->ExceptionClear(); cvCls = nullptr; }
    jmethodID cvInit = cvCls ? env->GetMethodID(cvCls, "<init>", "()V") : nullptr;
    jmethodID cvClear = lookupMethod(env, cvCls, "clear", "()V", false);
    jmethodID cvPutS = lookupMethod(env, cvCls, "put",
                                    "(Ljava/lang/String;Ljava/lang/String;)V", false);
    jmethodID cvPutI = lookupMethod(env, cvCls, "put",
                                    "(Ljava/lang/String;Ljava/lang/Integer;)V", false);
    // DATE_TAKEN 是 Long（毫秒）
    jmethodID cvPutL = lookupMethod(env, cvCls, "put",
                                    "(Ljava/lang/String;Ljava/lang/Long;)V", false);
    jclass intCls = env->FindClass("java/lang/Integer");
    if (env->ExceptionCheck()) { env->ExceptionClear(); intCls = nullptr; }
    jmethodID intInit = intCls ? env->GetMethodID(intCls, "<init>", "(I)V") : nullptr;
    jclass longCls = env->FindClass("java/lang/Long");
    if (env->ExceptionCheck()) { env->ExceptionClear(); longCls = nullptr; }
    jmethodID longInit = longCls ? env->GetMethodID(longCls, "<init>", "(J)V") : nullptr;
    if (!cvCls || !cvInit || !cvClear || !cvPutS || !cvPutI || !cvPutL ||
        !intCls || !intInit || !longCls || !longInit) {
        LOGE("gallery: ContentValues lookup failed");
        return false;
    }

    // ---- 键名常量（MediaStore.MediaColumns / Images.Media）----
    jclass mediaCls = env->FindClass("android/provider/MediaStore$Images$Media");
    if (env->ExceptionCheck()) { env->ExceptionClear(); mediaCls = nullptr; }
    jclass colCls = env->FindClass("android/provider/MediaStore$MediaColumns");
    if (env->ExceptionCheck()) { env->ExceptionClear(); colCls = nullptr; }
    auto strField = [&](jclass c, const char* n) -> jstring {
        if (!c) return nullptr;
        jfieldID f = lookupField(env, c, n, "Ljava/lang/String;", true);
        return f ? (jstring)env->GetStaticObjectField(c, f) : nullptr;
    };
    jstring kName = strField(colCls, "DISPLAY_NAME");
    jstring kMime = strField(colCls, "MIME_TYPE");
    jstring kRel  = strField(colCls, "RELATIVE_PATH");
    jstring kPen  = strField(colCls, "IS_PENDING");
    jstring kDate = strField(colCls, "DATE_TAKEN");
    jfieldID uriField = lookupField(env, mediaCls, "EXTERNAL_CONTENT_URI",
                                    "Landroid/net/Uri;", true);
    jobject uriBase = uriField ? env->GetStaticObjectField(mediaCls, uriField) : nullptr;
    if (!kName || !kMime || !kRel || !kPen || !kDate || !uriBase) {
        LOGE("gallery: MediaStore constants missing");
        return false;
    }

    // ---- ContentValues 填充 ----
    jobject cv = env->NewObject(cvCls, cvInit);
    jstring jName = env->NewStringUTF(displayName.c_str());
    jstring jMime = env->NewStringUTF("image/jpeg");
    jstring jPath = env->NewStringUTF("DCIM/Camera");
    auto boxed = [&](jint v) { return env->NewObject(intCls, intInit, v); };
    // DATE_TAKEN = 拍摄时刻毫秒（相册排序用）
    const jlong nowMs = (jlong)std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    env->CallVoidMethod(cv, cvPutS, kName, jName);
    env->CallVoidMethod(cv, cvPutS, kMime, jMime);
    env->CallVoidMethod(cv, cvPutS, kRel, jPath);
    env->CallVoidMethod(cv, cvPutL, kDate, env->NewObject(longCls, longInit, nowMs));
    env->CallVoidMethod(cv, cvPutI, kPen, boxed(1));
    if (env->ExceptionCheck()) env->ExceptionClear();

    // ---- ContentResolver.insert ----
    jmethodID getCR = lookupMethod(env, env->GetObjectClass(activity_),
                                   "getContentResolver",
                                   "()Landroid/content/ContentResolver;", false);
    jobject cr = getCR ? env->CallObjectMethod(activity_, getCR) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); cr = nullptr; }
    jmethodID insert = cr ? lookupMethod(env, env->GetObjectClass(cr), "insert",
                                         "(Landroid/net/Uri;Landroid/content/ContentValues;)"
                                         "Landroid/net/Uri;", false)
                          : nullptr;
    if (!cr || !insert) {
        LOGE("gallery: resolver/insert lookup failed");
        return false;
    }
    jobject uri = env->CallObjectMethod(cr, insert, uriBase, cv);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("gallery: insert failed");
        return false;
    }
    if (!uri) {
        LOGE("gallery: insert returned null uri");
        return false;
    }

    // ---- 写码流（chunked，避免一次 [B 超限——6.4MB 仍远小于 2GB 上限，直接整块写）----
    jmethodID openOs = lookupMethod(env, env->GetObjectClass(cr), "openOutputStream",
                                    "(Landroid/net/Uri;)Ljava/io/OutputStream;", false);
    jobject os = openOs ? env->CallObjectMethod(cr, openOs, uri) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); os = nullptr; }
    bool saved = false;
    if (os) {
        jbyteArray arr = env->NewByteArray(static_cast<jsize>(len));
        env->SetByteArrayRegion(arr, 0, static_cast<jsize>(len),
                                reinterpret_cast<const jbyte*>(data));
        jclass osCls = env->GetObjectClass(os);
        jmethodID write = lookupMethod(env, osCls, "write", "([BII)V", false);
        jmethodID flush = lookupMethod(env, osCls, "flush", "()V", false);
        jmethodID close = lookupMethod(env, osCls, "close", "()V", false);
        if (write) {
            env->CallVoidMethod(os, write, arr, 0, static_cast<jint>(len));
            if (env->ExceptionCheck()) env->ExceptionClear();
            else if (flush) env->CallVoidMethod(os, flush);
            if (env->ExceptionCheck()) env->ExceptionClear();
            else saved = true;
        }
        if (close) env->CallVoidMethod(os, close);
        if (env->ExceptionCheck()) env->ExceptionClear();
    } else {
        LOGE("gallery: openOutputStream failed");
    }

    // ---- IS_PENDING = 0（相册可见的最终提交）----
    if (saved) {
        env->CallVoidMethod(cv, cvClear);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->CallVoidMethod(cv, cvPutI, kPen, boxed(0));
        if (env->ExceptionCheck()) env->ExceptionClear();
        jmethodID update = lookupMethod(env, env->GetObjectClass(cr), "update",
                                        "(Landroid/net/Uri;Landroid/content/ContentValues;"
                                        "Ljava/lang/String;[Ljava/lang/String;)I", false);
        if (update) {
            env->CallIntMethod(cr, update, uri, cv, nullptr, nullptr);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }
    return saved;
}

} // namespace optic::util
