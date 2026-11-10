#!/usr/bin/env python3
"""The boot splash's logo frames, as a C table (src/splash-frames.h).

    python3 -I tools/splash/gen_splash.py              regenerate the header
    python3 -I tools/splash/gen_splash.py --check      exit 1 if it is stale
    python3 -I tools/splash/gen_splash.py --import <leekwallet-zcash-web>/src/generated/logo.ts

The logo is the website's (leekwallet-zcash-web: scripts/logo.py cuts the pot,
its lid and the key out of the source pictures into src/generated/logo.ts).
--import copies the grids this needs into logo-grids.txt beside this script,
which is committed, so the firmware never depends on the other repository
being checked out. Everything after that is a pure function of that file:

  - the pose of each frame (lid open, key rise, glow) is the website's
    pose() from src/game/logo-anim.ts, sampled at fixed times;
  - each pose is drawn with that file's render(), in that file's colours:
    GREEN for the pot and lid, KEY for the key once it glows (glow > 0.5),
    SPARK for the sparkles, and under the key, while it glows at all, a soft
    halo of SPARK at alpha round(90 * glow) that the key, the pot and the
    solid lid paint over;
  - the frames are cropped to the union of what any of them lights (the
    halo never moves the crop).

Two tables come out of each frame, over the same crop:

  - SPLASH_LOGO, one bit per pixel: lit is anything but the halo. This is
    what every panel shows (the OLEDs, and the colour panel's shape), and it
    is the same picture the one-bit port drew before the colours came in.
  - SPLASH_TINT, two bits per pixel, for the colour panel only (compiled
    when splash.c defines SPLASH_WANT_TINT): 0 = the pixel as SPLASH_LOGO
    has it (GREEN when lit), 1 = KEY, 2 = SPARK, 3 = HALO. HALO marks the
    pixels only the halo lights, so they are off in SPLASH_LOGO; the panel
    gives them one dim gold, SPARK over the background at the full halo's
    alpha (SPLASH_HALO_ALPHA). A frame with no colour has no table:
    SPLASH_TINT_OF[frame] is its row in SPLASH_TINT, or -1.

No clock, no randomness: the same grids give the same header, byte for byte,
which the reproducible firmware build (docs/RELEASE.md) relies on.
"""
import math
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
GRIDS = HERE / "logo-grids.txt"
OUT = HERE.parent.parent / "src" / "splash-frames.h"

PARTS = ("LID", "KEY", "POT", "CLOSED", "CLOSED_LID")

# The animation, as (website time in ms, how long the device shows it in ms).
# The website's loop: 0-700 closed, 700-1700 the lid opens, 1500-2300 the key
# rises, 1900-2300 it starts to glow. The device plays the same arc in about
# a second and a half, then holds the open logo while the text is up.
SCHEDULE = [
    (0, 250),
    (850, 90), (1000, 90), (1150, 90), (1300, 90), (1450, 90),
    (1600, 90), (1750, 90), (1900, 90), (2050, 90), (2200, 90),
    (2400, 100), (2600, 100),
]
# The last frame, held with the text beneath it.
HOLD = (2800, 1100)


# ------------------------------------------------------------------ import

def import_ts(path: str) -> None:
    src = Path(path).read_text(encoding="utf-8")
    out = []
    for name in PARTS:
        m = re.search(r"export const LOGO_%s: readonly string\[\] = \[\n(.*?)\];" % name, src, re.S)
        if not m:
            sys.exit(f"LOGO_{name} not found in {path}")
        rows = re.findall(r'"([.#]*)"', m.group(1))
        out.append(f"[{name}]")
        out.extend(rows)
    hinge = re.search(r"LOGO_HINGE = \{ x: (-?[\d.]+), y: (-?[\d.]+) \}", src)
    fit = re.search(r"LOGO_CLOSED_FIT = \{ angle: (-?[\d.]+), dx: (-?[\d.]+), dy: (-?[\d.]+) \}", src)
    if not hinge or not fit:
        sys.exit("LOGO_HINGE / LOGO_CLOSED_FIT not found")
    out.append(f"[HINGE] {hinge.group(1)} {hinge.group(2)}")
    out.append(f"[FIT] {fit.group(1)} {fit.group(2)} {fit.group(3)}")
    GRIDS.write_text(
        "# The LeekWallet logo grids, from leekwallet-zcash-web src/generated/logo.ts\n"
        "# (made there by scripts/logo.py). Input to gen_splash.py; do not edit by hand.\n"
        + "\n".join(out) + "\n",
        encoding="utf-8",
    )
    print(f"wrote {GRIDS}")


def load():
    parts, hinge, fit, cur = {}, None, None, None
    for line in GRIDS.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        if line.startswith("[HINGE]"):
            x, y = line.split()[1:]
            hinge = (float(x), float(y))
        elif line.startswith("[FIT]"):
            a, dx, dy = line.split()[1:]
            fit = (float(a), float(dx), float(dy))
        elif line.startswith("["):
            cur = line.strip("[]")
            parts[cur] = []
        else:
            parts[cur].append(line)
    return parts, hinge, fit


# ------------------------------------------------- logo-anim.ts, one bit deep

def jround(v: float) -> int:
    """JavaScript's Math.round: halves go up, not to even."""
    return math.floor(v + 0.5)


def ease(x: float) -> float:
    return 0.0 if x <= 0 else 1.0 if x >= 1 else x * x * (3 - 2 * x)


def pose(t: float):
    opn = ease((t - 700) / 1000) if t < 4200 else 1 - ease((t - 4200) / 800)
    key = ease((t - 1500) / 800) if t < 3700 else 1 - ease((t - 3700) / 600)
    glow = ease((t - 1900) / 400) if t < 2300 else 1.0 if t < 3700 else 1 - ease((t - 3700) / 300)
    return opn, key, glow, t


def cells(rows):
    return [(x, y) for y, r in enumerate(rows) for x, c in enumerate(r) if c == "#"]


def silhouette(rows):
    m = {}
    for y, r in enumerate(rows):
        a = r.find("#")
        if a >= 0:
            m[y] = (a, r.rfind("#"))
    return m


def turn(p, c, a):
    co, si = math.cos(a), math.sin(a)
    x, y = p
    cx, cy = c
    return (cx + (x - cx) * co - (y - cy) * si, cy + (x - cx) * si + (y - cy) * co)


# What render() paints a pixel with. OFF and GREEN are the one-bit picture;
# KEY and SPARK are lit too; HALO is unlit in one bit.
OFF, GREEN, KEY, SPARK, HALO = range(5)
LIT = (GREEN, KEY, SPARK)
# The two-bit tint code of each, in SPLASH_TINT.
TINT = {OFF: 0, GREEN: 0, KEY: 1, SPARK: 2, HALO: 3}
HALO_ALPHA = 90          # logo-anim.ts: round(90 * glow), at full glow
HALO_OFFSETS = ((1, 0), (-1, 0), (0, 1), (0, -1), (2, 0), (-2, 0), (0, -2))
RIM_CUT = 43


class Logo:
    PAD_X, PAD_TOP = 6, 14
    TILT, SWAP = 0.3, 0.45
    SPARKS = [(16, 30, 0), (33, 28, 0.25), (49, 33, 0.5), (24, 46, 0.75), (44, 45, 0.12), (12, 39, 0.62)]

    def __init__(self, parts, hinge, fit):
        self.p = parts
        self.lw, self.lh = len(parts["POT"][0]), len(parts["POT"])
        self.W = self.lw + self.PAD_X * 2
        self.H = self.lh + self.PAD_TOP + 2
        self.fit = {"angle": fit[0], "dx": fit[1], "dy": fit[2]}
        self.seat = (hinge[0] + fit[1], hinge[1] + fit[2])
        self.pot = cells(parts["POT"])
        self.shut = cells(parts["CLOSED"])
        shut_lid = set(cells(parts["CLOSED_LID"]))
        self.shut_body = [c for c in self.shut if c not in shut_lid]
        self.keyc = cells(parts["KEY"])
        self.lid_sil = silhouette(parts["LID"])
        self.shut_sil = silhouette(parts["CLOSED_LID"])
        lid_cells = cells(parts["CLOSED_LID"])
        # logo-anim.ts: the closed lid's lower right corner (largest x + y,
        # first one wins on a tie, as Array.reduce with ">" does).
        best = None
        for c in lid_cells:
            if best is None or c[0] + c[1] > best[0] + best[1]:
                best = c
        self.hinge = best if self.shut else self.seat
        h = self.unfit(turn(self.hinge, self.seat, 0))
        self.lift = (h[0] - self.hinge[0], h[1] - self.hinge[1])

    def unfit(self, q):
        x, y = turn(q, self.seat, -self.fit["angle"])
        return (x - self.fit["dx"], y - self.fit["dy"])

    def swing(self, opn):
        lift = ease((opn - 0.15) / 0.85)
        a = -self.fit["angle"] * opn + self.TILT * math.sin(math.pi * opn)
        return a, self.lift[0] * lift, self.lift[1] * lift

    def render(self, opn, key, glow, t):
        W, H = self.W, self.H
        img = [[0] * W for _ in range(H)]

        def put(x, y, c):
            x, y = jround(x), jround(y)
            if 0 <= x < W and 0 <= y < H:
                img[y][x] = c

        ox, oy = self.PAD_X, self.PAD_TOP
        if opn <= 0 and self.shut:
            for x, y in self.shut:
                put(x + ox, y + oy, GREEN)
            return img
        early = bool(self.shut) and opn < self.SWAP
        lift = -3 * key + 8 * (1 - key)
        if key > 0.02 and not early:
            # the halo first, so the key and everything after paint over it
            if glow > 0:
                for x, y in self.keyc:
                    ky = y + lift
                    if ky >= RIM_CUT:
                        continue
                    for ddx, ddy in HALO_OFFSETS:
                        put(x + ddx + ox, ky + ddy + oy, HALO)
            for x, y in self.keyc:
                ky = y + lift
                if ky < RIM_CUT:
                    put(x + ox, ky + oy, KEY if glow > 0.5 else GREEN)
        for x, y in (self.shut_body if early else self.pot):
            put(x + ox, y + oy, GREEN)
        a, dx, dy = self.swing(opn)
        rows = self.p["CLOSED_LID"] if early else self.p["LID"]
        sil = self.shut_sil if early else self.lid_sil
        for y in range(H):
            for x in range(W):
                q = turn((x - ox - dx, y - oy - dy), self.hinge, -a)
                if not early:
                    q = self.unfit(q)
                sx, sy = jround(q[0]), jround(q[1])
                span = sil.get(sy)
                if not span or sx < span[0] or sx > span[1]:
                    continue
                img[y][x] = GREEN if rows[sy][sx] == "#" else OFF   # solid
        if glow > 0.3:
            for x, y, phase in self.SPARKS:
                k = ((t / 900 + phase) % 1) * 2 * math.pi
                b = math.sin(k)
                if b < 0.2:
                    continue
                cx, cy = x + ox, y + oy
                put(cx, cy, SPARK)
                if b > 0.6:
                    for ddx, ddy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        put(cx + ddx, cy + ddy, SPARK)
        return img


# ---------------------------------------------------------------------- emit

def frames():
    parts, hinge, fit = load()
    logo = Logo(parts, hinge, fit)
    sched = SCHEDULE + [HOLD]
    imgs = [logo.render(*pose(t)) for t, _ in sched]
    # The crop is what the one-bit picture lights, so the halo cannot move it;
    # nor may it be cut off by it.
    ys = [y for im in imgs for y, r in enumerate(im) if any(v in LIT for v in r)]
    xs = [x for im in imgs for r in im for x, v in enumerate(r) if v in LIT]
    x0, x1, y0, y1 = min(xs), max(xs) + 1, min(ys), max(ys) + 1
    for im in imgs:
        for y, r in enumerate(im):
            for x, v in enumerate(r):
                if v == HALO and not (x0 <= x < x1 and y0 <= y < y1):
                    sys.exit(f"halo pixel ({x}, {y}) falls outside the crop")
    crop = [[r[x0:x1] for r in im[y0:y1]] for im in imgs]
    return crop, sched


def header() -> str:
    crop, sched = frames()
    w, h = len(crop[0][0]), len(crop[0])
    stride = (w + 7) // 8
    lines = [
        "/* Generated by tools/splash/gen_splash.py from tools/splash/logo-grids.txt.",
        " * Do not edit by hand: change the script and run it again. */",
        "#ifndef LEEK_SPLASH_FRAMES_H",
        "#define LEEK_SPLASH_FRAMES_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define SPLASH_LOGO_W      {w}",
        f"#define SPLASH_LOGO_H      {h}",
        f"#define SPLASH_LOGO_STRIDE {stride}   /* bytes per row, MSB is the leftmost pixel */",
        f"#define SPLASH_FRAMES      {len(crop)}",
        "",
        "/* How long each frame stays up, in ms. The last one is held with the text. */",
        "static const uint16_t SPLASH_FRAME_MS[SPLASH_FRAMES] = {",
        "    " + ", ".join(str(ms) for _, ms in sched) + ",",
        "};",
        "",
        "static const uint8_t SPLASH_LOGO[SPLASH_FRAMES][SPLASH_LOGO_H * SPLASH_LOGO_STRIDE] = {",
    ]
    for i, (im, (t, _)) in enumerate(zip(crop, sched)):
        lines.append(f"    /* frame {i}: the website's logo at t = {t} ms */")
        lines.append("    {")
        for row in im:
            bs = []
            for b in range(stride):
                v = 0
                for k in range(8):
                    x = b * 8 + k
                    if x < w and row[x] in LIT:
                        v |= 0x80 >> k
                bs.append(f"0x{v:02x}")
            art = "".join("#" if v in LIT else "." for v in row)
            lines.append("        " + ",".join(bs) + f",  /* {art} */")
        lines.append("    },")
    lines.append("};")
    lines += tint_lines(crop, sched, w)
    lines += ["", "#endif /* LEEK_SPLASH_FRAMES_H */", ""]
    return "\n".join(lines)


def tint_lines(crop, sched, w):
    """The colour layer, SPLASH_TINT, for the frames that have any colour."""
    tstride = (w * 2 + 7) // 8
    tinted = [i for i, im in enumerate(crop) if any(TINT[v] for r in im for v in r)]
    index = [tinted.index(i) if i in tinted else -1 for i in range(len(crop))]
    lines = [
        "",
        "/* The colour layer (see gen_splash.py), for the colour panel only. */",
        "#ifdef SPLASH_WANT_TINT",
        "",
        f"#define SPLASH_TINT_STRIDE {tstride}   /* bytes per row, 2 bits a pixel, MSB pair leftmost */",
        f"#define SPLASH_TINTS       {len(tinted)}",
        "#define SPLASH_TINT_MONO   0   /* the pixel as SPLASH_LOGO has it */",
        "#define SPLASH_TINT_KEY    1",
        "#define SPLASH_TINT_SPARK  2",
        "#define SPLASH_TINT_HALO   3   /* lit by the halo alone: unlit in SPLASH_LOGO */",
        f"#define SPLASH_HALO_ALPHA  {HALO_ALPHA}  /* of 255: SPARK over the background */",
        "",
        "/* Each frame's row in SPLASH_TINT, or -1 when it has no colour. */",
        "static const int8_t SPLASH_TINT_OF[SPLASH_FRAMES] = {",
        "    " + ", ".join(str(i) for i in index) + ",",
        "};",
        "",
        "static const uint8_t SPLASH_TINT[SPLASH_TINTS][SPLASH_LOGO_H * SPLASH_TINT_STRIDE] = {",
    ]
    glyph = {0: ".", 1: "K", 2: "*", 3: "o"}
    for i in tinted:
        lines.append(f"    /* frame {i}: the website's logo at t = {sched[i][0]} ms */")
        lines.append("    {")
        for row in crop[i]:
            bs = []
            for b in range(tstride):
                v = 0
                for k in range(4):
                    x = b * 4 + k
                    if x < w:
                        v |= TINT[row[x]] << (6 - 2 * k)
                bs.append(f"0x{v:02x}")
            art = "".join(glyph[TINT[v]] for v in row)
            lines.append("        " + ",".join(bs) + f",  /* {art} */")
        lines.append("    },")
    lines += ["};", "", "#endif /* SPLASH_WANT_TINT */"]
    return lines


def main() -> None:
    args = sys.argv[1:]
    if args[:1] == ["--import"] and len(args) == 2:
        import_ts(args[1])
        args = []
    if args == ["--check"]:
        if OUT.read_text(encoding="utf-8") != header():
            sys.exit(f"{OUT} is stale: run tools/splash/gen_splash.py")
        print("splash frames up to date")
        return
    if args:
        sys.exit(__doc__)
    OUT.write_text(header(), encoding="utf-8")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
