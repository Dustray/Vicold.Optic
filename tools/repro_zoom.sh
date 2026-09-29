#!/usr/bin/env bash
# 复现「拉到最广预览卡死」：保持亮屏 → 启动 → 采帧率 → 下发 zoom=0.7 → 再采帧率。
# 用法：bash tools/repro_zoom.sh [zoom值]
set -u
export PATH="/d/Android/sdk/platform-tools:$PATH"
CTL=/storage/emulated/0/Android/data/com.vicold.optic/files/controls.txt
Z="${1:-0.7}"

adb shell svc power stayon true >/dev/null 2>&1
adb logcat -c
adb shell am start -n com.vicold.optic/android.app.NativeActivity >/dev/null 2>&1

echo "--- 阶段1：默认（zoom=1.4，raw=ring）运行 10s ---"
sleep 10
adb logcat -d -s Optic:V | grep -E "stats:|preview frames|session running|PAUSE" | tail -6

echo "--- 阶段2：下发 zoom=$Z ---"
adb shell "printf 'zoom=%s\n' '$Z' > $CTL"
sleep 10
adb logcat -d -s Optic:V | grep -E "stats:|ui applied" | tail -8

echo "--- 阶段3：再等 8s 看是否还在出帧 ---"
adb logcat -c
sleep 8
adb logcat -d -s Optic:V | grep -cE "stats:" | sed 's/^/stats 条数: /'
adb logcat -d -s Optic:V | grep -E "stats:|ui applied|PAUSE|error" | tail -6

adb shell "rm -f $CTL"
adb shell svc power stayon false >/dev/null 2>&1
