#!/usr/bin/env python
"""物理摄等效倍率标定：比较两帧画面，估计"把 test 缩放 k 倍后与 base 对齐"的 k。

用法: calib_fov.py base.png test.png [test2.png ...]
输出: 每个 test 相对 base 的等效倍率 z = 1/k（base 视为 1.0）。
      画面中心对齐、ZNCC 去均值抗亮度差；视差/畸变会让峰值略钝，但可分辨。
"""
import sys
import numpy as np
from PIL import Image

N = 220           # 比对分辨率
FRAC = 0.45       # 取画面中心比例（避开 UI 与边缘畸变）


def prep(path, cx=None, cy=None):
    im = Image.open(path).convert("L")
    w, h = im.size
    s = int(min(w, h) * FRAC)
    if cx is None:
        cx = w // 2
    if cy is None:
        cy = h // 2
    im = im.crop((cx - s // 2, cy - s // 2, cx + s // 2, cy + s // 2))
    a = np.asarray(im.resize((N, N), Image.BILINEAR), dtype=np.float64)
    a -= a.mean()
    n = np.linalg.norm(a)
    return a / (n if n > 0 else 1.0)


def sample(src, k):
    """把 src 画面中心 1/k 区域放大到全画幅（等价于视野缩到 1/k）"""
    s = max(4, int(round(N / k)))
    x0 = (N - s) // 2
    crop = src[x0:x0 + s, x0:x0 + s]
    im = Image.fromarray(crop.astype(np.uint8) if crop.max() > 255 else crop)
    a = np.asarray(im.resize((N, N), Image.BILINEAR), dtype=np.float64)
    a -= a.mean()
    n = np.linalg.norm(a)
    return a / (n if n > 0 else 1.0)


def best_k(base, src):
    """k = 让 src 与 base 对齐所需的中心缩放倍率；等效倍率 z = 1/k"""
    lo, hi = 0.08, 8.0
    grid = np.exp(np.linspace(np.log(lo), np.log(hi), 260))
    scores = [ncc(base, *warp(src, k)) for k in grid]
    k0 = float(grid[int(np.argmax(scores))])
    for step in (0.03, 0.008, 0.002):
        cand = np.arange(k0 * (1 - step * 5), k0 * (1 + step * 5) + 1e-9, k0 * step)
        sc = [ncc(base, *warp(src, k)) for k in cand]
        k0 = float(cand[int(np.argmax(sc))])
    return k0, ncc(base, *warp(src, k0))


def warp(src, k):
    """把 src 按中心缩放 k 倍（k>1 视野变窄 / k<1 变宽），铺在 N*N 画布上。

    返回 (画布, 有效边长)。k<1 时 src 缩小后只占中心 valid 区域，四周为零，
    比对时只取中心 valid 区域（否则零填充会污染 NCC）。"""
    if k >= 1.0:
        s = max(4, int(round(N / k)))
        x0 = (N - s) // 2
        im = Image.fromarray(src[x0:x0 + s, x0:x0 + s], mode="F").resize((N, N), Image.BILINEAR)
        return np.asarray(im, dtype=np.float64), N
    s = max(4, int(round(N * k)))
    small = np.asarray(Image.fromarray(src, mode="F").resize((s, s), Image.BILINEAR),
                       dtype=np.float64)
    canvas = np.zeros((N, N), dtype=np.float64)
    x0 = (N - s) // 2
    canvas[x0:x0 + s, x0:x0 + s] = small
    return canvas, s


def ncc(a, b, valid):
    """只比中心 valid 区域，各自去均值归一"""
    if valid < N:
        x0 = (N - valid) // 2
        a = a[x0:x0 + valid, x0:x0 + valid]
        b = b[x0:x0 + valid, x0:x0 + valid]
    a = a - a.mean()
    b = b - b.mean()
    na, nb = np.linalg.norm(a), np.linalg.norm(b)
    if na == 0 or nb == 0:
        return 0.0
    return float(np.sum(a * b) / (na * nb))


def sample01(src, k):
    a, v = warp(src, k)
    return a, v


def main():
    # 可选 --cx/--cy：比对中心（默认画面中心）。本机主屏预览区中心 ≈ (1478,610)。
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    cx = cy = None
    for a in sys.argv[1:]:
        if a.startswith("--cx="):
            cx = int(a[5:])
        elif a.startswith("--cy="):
            cy = int(a[5:])
    base = prep(args[0], cx, cy)
    print(f"base: {args[0]}  (z=1.0)")
    for p in args[1:]:
        src = prep(p)
        k, score = best_k(base, src)
        print(f"  {p}: k={k:.3f}  ->  z={1.0 / k:.3f}   (ncc={score:.3f})")


if __name__ == "__main__":
    main()
