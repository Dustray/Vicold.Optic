#!/usr/bin/env bash
# 开发流程一键脚本: ./tools/dev.sh <build|package|install|run|log|shot|report|all>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/tools/env.sh"

cmd="${1:-all}"

do_build() {
    cmake --preset android-arm64 -S "$ROOT"
    cmake --build "$ROOT/build/android-arm64" -j
}

do_package() { "$ROOT/tools/package.sh"; }

do_install() {
    adb install -r "$ROOT/dist/Optic-debug.apk"
    # 运行时权限预先授予（若失败，app 内会走 JNI 申请兜底）
    adb shell pm grant "$PKG" android.permission.CAMERA 2>/dev/null || true
}

do_run() {
    adb shell am force-stop "$PKG" 2>/dev/null || true
    adb shell am start -n "$PKG/android.app.NativeActivity"
}

do_log() {
    adb logcat -d -s Optic OpticProbe AndroidRuntime | tail -120
}

do_shot() {
    adb exec-out screencap -p > "$ROOT/build/screen.png" 2>/dev/null || adb shell screencap -p /sdcard/optic_screen.png
    adb pull /sdcard/optic_screen.png "$ROOT/build/screen.png" 2>/dev/null || true
    echo "screenshot: build/screen.png"
}

do_report() {
    mkdir -p "$ROOT/doc/devices/xiaomi17pro"
    # 先停 app：CameraEngine 在 stop() 里 flush 并关闭报告文件
    adb shell am force-stop "$PKG" 2>/dev/null || true
    sleep 1
    adb pull "/storage/emulated/0/Android/data/$PKG/files/capabilities.txt" \
        "$ROOT/doc/devices/xiaomi17pro/capabilities_app.txt" 2>/dev/null \
        || echo "capabilities.txt not ready"
    adb shell getprop > "$ROOT/doc/devices/xiaomi17pro/getprop.txt" || true
    adb shell dumpsys media.camera > "$ROOT/doc/devices/xiaomi17pro/camera_dumpsys.txt" 2>/dev/null || true
    echo "device intel saved to doc/devices/xiaomi17pro/"
}

case "$cmd" in
    build)   do_build ;;
    package) do_package ;;
    install) do_install ;;
    run)     do_run ;;
    log)     do_log ;;
    shot)    do_shot ;;
    report)  do_report ;;
    all)     do_build && do_package && do_install && do_run ;;
    *) echo "usage: $0 <build|package|install|run|log|shot|report|all>"; exit 1 ;;
esac
