#!/usr/bin/env bash
# 逻辑（主摄）流 zoom 安全上限测试：逐点设 zoom，看开 slot=0 是否在出帧。
# 用法: bash tools/logical_limit.sh [点...]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/tools/env.sh"

CTL="/storage/emulated/0/Android/data/$PKG/files/controls.txt"
POINTS="${*:-2.0 3.0 3.5 4.0 4.5 5.0}"
export MSYS_NO_PATHCONV=1
cd "$ROOT" || exit 1   # adb 是原生 Windows 程序：只认相对路径 / D:\ 路径

# phys_min=5.0 抬高长焦接管点，迫使逻辑流自己走到 5x（否则被 effSettings 钳住）
printf 'phys_min=5.0\nzoom=1.0\n' > build/ctl.txt
adb push build/ctl.txt "$CTL" >/dev/null
sleep 2

printf '%-7s %-6s %-10s %s\n' zoom slot L_frames verdict
for z in $POINTS; do
    printf 'zoom=%s\nphys_min=5.0\n' "$z" > build/ctl.txt
    adb push build/ctl.txt "$CTL" >/dev/null
    sleep 2.5
    a="$(adb logcat -d -s Optic | grep 'slot frames' | tail -1 | sed -n 's/.*L=\([0-9]*\).*/\1/p')"
    sleep 2.5
    b="$(adb logcat -d -s Optic | grep 'slot frames' | tail -1 | sed -n 's/.*L=\([0-9]*\).*/\1/p')"
    sl="$(adb logcat -d -s Optic | grep 'crop dbg' | tail -1 | sed -n 's/.*slot=\([0-9]*\).*/\1/p')"
    cz="$(adb logcat -d -s Optic | grep 'crop dbg' | tail -1 | sed -n 's/.*zoom=\([0-9.]*\).*/\1/p')"
    if [[ -z "$a" || -z "$b" ]]; then
        printf '%-7s %-6s %-10s %s\n' "$z" "-" "-" "no stats"
        continue
    fi
    d=$((b - a))
    if (( d > 30 )); then v="flowing (+$d/2.5s, ui z=$cz)"; else v="STALLED (+$d/2.5s, ui z=$cz)"; fi
    printf '%-7s %-6s %-10s %s\n' "$z" "$sl" "$b" "$v"
done
