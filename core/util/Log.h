#pragma once
// 统一日志宏（core 层通用）
#include <android/log.h>

#define OPTIC_LOG_TAG "Optic"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, OPTIC_LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, OPTIC_LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, OPTIC_LOG_TAG, __VA_ARGS__)
