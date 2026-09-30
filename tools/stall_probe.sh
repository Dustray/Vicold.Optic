#!/usr/bin/env bash
# 逐点统计预览 stall：每个 zoom 点静置 $HOLD 秒，数 "preview stall" 条数。
# stall = HAL 静默停止交付帧（看门狗兜底重发）；持续 stall 的点即不可用区间。
# 用法: bash tools/stall_probe.sh [HOLD] [点...]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/tools/env.sh"

CTL="/storage/emulated/0/Android/data/$PKG/files/controls.txt"
HOLD="${1:-6}"
shift || true
POINTS="${*:-1.0 2.0 2.5 2.63 3.0 3.5 4.0 4.5 4.9 5.5 7.0 10.0}"
export MSYS_NO_PATHCONV=1
cd "$ROOT" || exit 1

printf '%-7s %-7s %s\n' zoom stalls verdict
for z in $POINTS; do
    printf 'zoom=%s\n' "$z" > build/ctl.txt
    adb push build/ctl.txt "$CTL" >/dev/null
    sleep 2
    adb logcat -c
    sleep "$HOLD"
    n=$(adb logcat -d -s Optic | grep -c "preview stall" || true)
    slot="$(adb logcat -d -s Optic | grep 'crop dbg' | tail -1 | sed -n 's/.*slot=\([0-9]*\).*/\1/p')"
    if (( n == 0 )); then v="ok"; elif (( n <= 2 )); then v="flaky"; else v="BROKEN"; fi
    printf '%-7s %-7s %s (slot=%s)\n' "$z" "$n" "$v" "$slot"
done
