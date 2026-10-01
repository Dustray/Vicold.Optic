add_library(optic_core STATIC
    device/IOpticDevice.cpp
    device/DeviceRegistry.cpp
    device/generic/GenericDevice.cpp
    device/xiaomi17pro/Xiaomi17ProDevice.cpp
    capture/CameraDevice.cpp
    capture/CaptureSettings.cpp
    capture/CaptureSession.cpp
    capture/RawCapture.cpp
    capture/StillCapture.cpp
    capture/StillProcessor.cpp
    capture/CameraEngine.cpp
    dng/DngWriter.cpp
    util/Gallery.cpp
    ui/Gl.cpp
    ui/Haptics.cpp
    ui/Battery.cpp
    ui/SysMon.cpp
    ui/Ui.cpp
)

target_include_directories(optic_core PUBLIC "${CMAKE_SOURCE_DIR}")
target_compile_options(optic_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(optic_core PUBLIC camera2ndk mediandk android log EGL GLESv2)
