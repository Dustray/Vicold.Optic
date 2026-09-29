#!/usr/bin/env python
# 真机截图体检：只看数值，判断预览区是"二维画面"还是被压成一维条带。
# 用法：python tools/shot_check.py <png> <x0> <y0> <x1> <y1>
import sys

import numpy as np
from PIL import Image


def main() -> int:
    if len(sys.argv) < 6:
        print("usage: shot_check.py <png> <x0> <y0> <x1> <y1>")
        return 2
    path, x0, y0, x1, y1 = sys.argv[1], *map(int, sys.argv[2:6])
    im = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    print("screenshot:", im.shape[1], "x", im.shape[0])
    r = im[y0:y1, x0:x1]
    if r.size == 0:
        print("empty region")
        return 1
    g = r.mean(axis=2)
    row_var = g.var(axis=1).mean()     # 行内（沿 x）方差
    col_var = g.var(axis=0).mean()     # 列内（沿 y）方差
    print("region: %dx%d  mean=%.1f  min=%.0f max=%.0f" %
          (r.shape[1], r.shape[0], g.mean(), g.min(), g.max()))
    print("within-row  var (沿 x): %8.2f" % row_var)
    print("within-col  var (沿 y): %8.2f" % col_var)
    # 相邻像素梯度：两个方向都应有量级，某一方向塌成 0 就是一维条带
    dx = np.abs(np.diff(g, axis=1)).mean()
    dy = np.abs(np.diff(g, axis=0)).mean()
    print("grad x=%.2f  grad y=%.2f  ratio=%.3f" % (dx, dy, dx / max(dy, 1e-6)))
    flat = min(row_var, col_var) < 1.0 and max(row_var, col_var) > 20
    print("verdict:", "一维条带（某一方向塌成常量）" if flat else "二维画面")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
