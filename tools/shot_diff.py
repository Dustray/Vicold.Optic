#!/usr/bin/env python
# 抓两帧并对比：判断预览是否还在动（真机上的"卡死"判定）。
# 用法：python tools/shot_diff.py <tag>
# 依赖 adb 在 PATH；截图落在 build/shots/。
import subprocess
import sys
import time

import numpy as np
from PIL import Image


def cap(path: str) -> None:
    # screencap 在多屏设备上会在 stdout 前面塞一行 Warning，PNG 头不在 0 偏移，后面统一裁掉
    with open(path, "wb") as f:
        f.write(subprocess.run(["adb", "exec-out", "screencap", "-p"],
                               stdout=subprocess.PIPE, check=True).stdout)


def load(path: str) -> np.ndarray:
    d = open(path, "rb").read()
    i = d.find(b"\x89PNG")
    if i < 0:
        raise SystemExit("no PNG in " + path)
    if i > 0:
        open(path, "wb").write(d[i:])
    return np.asarray(Image.open(path).convert("L"), dtype=np.float32)


def main() -> int:
    tag = sys.argv[1] if len(sys.argv) > 1 else "t"
    a, b = f"build/shots/{tag}_a.png", f"build/shots/{tag}_b.png"
    cap(a)
    time.sleep(1.5)
    cap(b)
    x, y = load(a), load(b)
    print("frame:", x.shape[1], "x", x.shape[0])
    regions = {
        "预览区": (0, 1220, 407, 2033),
        "左侧导轨": (0, 1220, 150, 400),
        "右侧面板": (0, 1220, 2050, 2600),
    }
    for k, (y0, y1, x0, x1) in regions.items():
        d = np.abs(x[y0:y1, x0:x1] - y[y0:y1, x0:x1])
        print("%-10s 变化像素=%5.1f%%  平均差=%5.2f" % (k, (d > 6).mean() * 100, d.mean()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
