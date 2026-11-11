#!/usr/bin/env python3
"""Camera-like QVGA grayscale frames of UR-shaped QR codes, for bench.c.

Unlike ../qr-spike/gen-frames.py (clean renders, a floor), these try to look
like what the OV5640 hands quirc at 2x zoom: a code filling 170-200 px of the
240 px height, a little rotation and perspective, a Gaussian blur, sensor
noise, a screen's limited contrast and a lighting fall-off. They are still
synthetic: no rolling-shutter tear, no moire, no glare. Use them for relative
stage costs and for A/B result checks, not for absolute decode rates.

    python3 gen-frames.py v8 v8.gray 24      # also v5, v6, none
"""
import random
import sys

import numpy as np
import qrcode
from PIL import Image, ImageFilter

W, H = 320, 240


def matrix(payload):
    q = qrcode.QRCode(border=0, error_correction=qrcode.constants.ERROR_CORRECT_L)
    q.add_data(payload)
    q.make(fit=True)
    return np.array(q.get_matrix(), dtype=np.uint8), q.version


def frame(payload, side, rnd, blur=1.0, noise=6.0):
    m, ver = matrix(payload)
    n = m.shape[0]
    sc = 16
    big = np.kron(1 - m, np.ones((sc, sc), dtype=np.uint8)) * 255
    big = np.pad(big, 4 * sc, constant_values=255)
    img = Image.fromarray(big)
    px = side * (n + 8) / n
    cx, cy = W / 2 + rnd.uniform(-15, 15), H / 2 + rnd.uniform(-10, 10)
    a = rnd.uniform(-0.15, 0.15)
    ca, sa = np.cos(a), np.sin(a)
    h = px / 2
    quad = []
    for dx, dy in [(-h, -h), (h, -h), (h, h), (-h, h)]:
        dx *= 1 + rnd.uniform(-0.04, 0.04)
        dy *= 1 + rnd.uniform(-0.04, 0.04)
        quad.append((cx + ca * dx - sa * dy, cy + sa * dx + ca * dy))
    s = big.shape[0]
    src = [(0, 0), (s, 0), (s, s), (0, s)]
    rows, rhs = [], []
    for (x, y), (u, v) in zip(quad, src):
        rows.append([x, y, 1, 0, 0, 0, -u * x, -u * y])
        rhs.append(u)
        rows.append([0, 0, 0, x, y, 1, -v * x, -v * y])
        rhs.append(v)
    coef = np.linalg.solve(np.array(rows, float), np.array(rhs, float))
    out = img.transform((W, H), Image.PERSPECTIVE, tuple(coef), Image.BILINEAR,
                        fillcolor=90)
    out = out.filter(ImageFilter.GaussianBlur(blur))
    f = np.asarray(out, dtype=np.float32)
    lo, hi = rnd.uniform(25, 50), rnd.uniform(170, 220)
    f = lo + (hi - lo) * f / 255.0
    yy, xx = np.mgrid[0:H, 0:W]
    f *= 1.0 - 0.25 * ((xx - cx) ** 2 + (yy - cy) ** 2) / (W * W / 4)
    f += np.random.default_rng(rnd.randrange(1 << 30)).normal(0, noise, f.shape)
    return np.clip(f, 0, 255).astype(np.uint8), ver


def ur_part(i, chars):
    alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    r = random.Random(i)
    return "UR:LEEK-PCZT/%d-28/" % (i + 1) + "".join(r.choice(alpha) for _ in range(chars))


def no_code(i):
    f = np.random.default_rng(i).normal(100, 30, (H // 8, W // 8))
    img = Image.fromarray(np.clip(f, 0, 255).astype(np.uint8)).resize((W, H), Image.BILINEAR)
    f = np.asarray(img, dtype=np.float32)
    f += np.random.default_rng(i + 99).normal(0, 6, f.shape)
    return np.clip(f, 0, 255).astype(np.uint8)


def main():
    kind, out, count = sys.argv[1], sys.argv[2], int(sys.argv[3])
    rnd = random.Random(1234)
    versions = set()
    with open(out, "wb") as fo:
        for i in range(count):
            if kind == "none":
                fo.write(no_code(i).tobytes())
                continue
            # (bytewords characters, code side in px at 2x zoom)
            chars, side = {"v5": (130, 170), "v6": (170, 185), "v8": (250, 200)}[kind]
            f, v = frame(ur_part(i, chars), side, rnd)
            versions.add(v)
            fo.write(f.tobytes())
    print(kind, out, count, "QR versions", sorted(versions), file=sys.stderr)


if __name__ == "__main__":
    main()
