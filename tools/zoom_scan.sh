#!/usr/bin/env bash
# 变焦分带 FOV 扫描：逐点设 zoom，读 crop dbg（slot/az/crop），核对实际显示 FOV = az*crop
# 判据：eff FOV 应 == zoom 且随 zoom 单调连续；偏离处即「重叠/缺失」。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/tools/env.sh"

CTL="/storage/emulated/0/Android/data/$PKG/files/controls.txt"
POINTS="${*:-0.70 0.80 0.90 1.00 1.20 1.60 2.00 2.40 2.55 2.63 2.70 3.00 4.00 6.00 10.00}"

printf '%-7s %-5s %-7s %-8s %-8s %s\n' zoom slot az crop effFOV delta
for z in $POINTS; do
    adb shell "echo 'zoom=$z' > $CTL" >/dev/null 2>&1
    sleep 2.6
    line="$(adb logcat -d -s Optic | grep 'crop dbg' | tail -1)"
    if [[ -z "$line" ]]; then
        printf '%-7s %s\n' "$z" "(no crop dbg)"
        continue
    fi
    slot="$(sed -n 's/.*slot=\([0-9]*\).*/\1/p' <<<"$line")"
    zoom="$(sed -n 's/.*zoom=\([0-9.]*\).*/\1/p' <<<"$line")"
    az="$(sed -n 's/.*az=\([0-9.]*\).*/\1/p' <<<"$line")"
    crop="$(sed -n 's/.*crop=\([0-9.]*\).*/\1/p' <<<"$line")"
    eff="$(awk -v a="$az" -v c="$crop" 'BEGIN{printf "%.3f", a*c}')"
    d="$(awk -v e="$eff" -v z="$z" 'BEGIN{printf "%+.3f", e-z}')"
    printf '%-7s %-5s %-7s %-8s %-8s %s\n' "$z" "$slot" "$az" "$crop" "$eff" "$d"
done
