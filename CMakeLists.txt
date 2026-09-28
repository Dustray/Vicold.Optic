cmake_minimum_required(VERSION 3.22)
project(VicoldOptic LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# NativeActivity 胶水层直接引用 NDK 源码，稳定后 vendor 进仓库
set(NDK_GLUE_DIR "${ANDROID_NDK}/sources/android/native_app_glue")
if(NOT EXISTS "${NDK_GLUE_DIR}/android_native_app_glue.c")
    message(FATAL_ERROR "native_app_glue not found under ${ANDROID_NDK}")
endif()

add_subdirectory(core)

add_library(optic SHARED
    app/src/main/cpp/optic.cpp
    "${NDK_GLUE_DIR}/android_native_app_glue.c"
)
target_include_directories(optic PRIVATE "${NDK_GLUE_DIR}")
target_compile_options(optic PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(optic PRIVATE optic_core camera2ndk android log)
