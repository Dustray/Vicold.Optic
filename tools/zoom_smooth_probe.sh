#!/usr/bin/env bash
# 拖动流畅度探针：对比「相机 zoom 静止」与「相机 zoom 连续变化」两种状态下的
# 掉帧（帧间隔 >70ms）与帧率，定位主摄带 1-5x 拖动卡顿是掉帧还是 FOV 阶梯。
# 用法: zoom_smooth_probe.sh [起始] [结束] [步长] [每步间隔秒]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tools/env.sh" >/dev/null 2>&1
export MSYS_NO_PATHCONV=1
CTL="/storage/emulated/0/Android/data/com.vicold.optic/files/controls.txt"
# Git Bash/MSYS 下 adb.exe 无法解析 /d/... 形式的本地路径；转成 Windows 路径。
# 真实 Linux/WSL 环境无 cygpath，回退到原始路径（那里本就正确）。
LOCAL_CTL="$(cygpath -w "$ROOT/build/ctl.txt" 2>/dev/null || echo "$ROOT/build/ctl.txt")"
Z0="${1:-1.0}"; Z1="${2:-5.0}"; STEP="${3:-0.1}"; DT="${4:-0.12}"

setzoom() { printf 'zoom=%.3f\n' "$1" > "$ROOT/build/ctl.txt"; adb push "$LOCAL_CTL" "$CTL" >/dev/null; }
pacing() { adb logcat -d -s Optic | grep "pacing:" | tail -1 | \
           sed -n 's/.*gaps>70ms=\([0-9]*\).*max=\([0-9]*\)ms.*(frames=\([0-9]*\))/\1 \2 \3/p'; }

echo "== 基线（相机 zoom 静止 ${Z0}） =="
setzoom "$Z0"; sleep 2
adb logcat -c; sleep 5
read -r g0 m0 f0 <<<"$(pacing)"
echo "gaps=$g0 max=${m0}ms frames=$f0  (~ $(awk "BEGIN{printf \"%.1f\", $f0/5}") fps)"

echo "== 拖动（zoom $Z0 -> $Z1 -> $Z0 步长$STEP 间隔${DT}s） =="
adb logcat -c
T0=$(date +%s)
z=$Z0
while (( $(awk "BEGIN{print ($z < $Z1 - 0.001) ? 1 : 0}") )); do
    z=$(awk "BEGIN{printf \"%.3f\", $z + $STEP}"); setzoom "$z"; sleep "$DT"
done
while (( $(awk "BEGIN{print ($z > $Z0 + 0.001) ? 1 : 0}") )); do
    z=$(awk "BEGIN{printf \"%.3f\", $z - $STEP}"); setzoom "$z"; sleep "$DT"
done
T1=$(date +%s); DUR=$((T1 - T0))
read -r g1 m1 f1 <<<"$(pacing)"
echo "gaps=$g1 max=${m1}ms frames=$f1  (~ $(awk "BEGIN{printf \"%.1f\", $f1/$DUR}") fps / ${DUR}s)"
