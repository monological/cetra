"""Draw the reeds that stand in apps/silent's lake shallows (spec 13.41).

A scan would need an opacity channel, and the fetcher's sets have none, so the clump is drawn:
a few dozen blades rising from a common foot, tapering, bending over as they go up, some broken
off, among them a few stems carrying a cattail's dark head. Late in the year, so straw and dead
olive rather than green. One picture is one clump, seen side on; the lake stands each clump as
two or three cards crossed through its foot.

Writes reeds_{albedo,normal,rough}.png into assets/textures/silent/, the albedo RGBA with the
blades in alpha, and like every map there BOTTOM ROW FIRST (see fetch_textures.py).

    python3 apps/silent/tools/make_reeds.py
"""

import os
import sys

import numpy as np
from PIL import Image

from fetch_textures import OUT_DIR

SIZE = 256
SEED = 1341
BLADES = 46
CATTAILS = 5

STRAW = np.array([0.60, 0.53, 0.38])  # sRGB, dry reed
OLIVE = np.array([0.36, 0.36, 0.24])  # and one not quite dead
ROOT = np.array([0.20, 0.17, 0.12])  # the wet foot of the clump
HEAD = np.array([0.24, 0.15, 0.09])  # a cattail's seed head


def blade_path(rng, foot_x):
    """A blade's centre line, foot to tip, in texels with y up: rising and leaning out, bending
    over toward its tip, and now and then snapped off part way."""
    height = rng.uniform(0.55, 0.97) * SIZE
    lean = rng.normal(0.0, 0.18)
    droop = rng.uniform(0.0, 0.35) * np.sign(lean if lean != 0 else 1.0)
    t = np.linspace(0.0, 1.0, 64)
    x = foot_x + SIZE * (lean * t + droop * t ** 3)
    y = height * (t - 0.18 * abs(droop) * t ** 4)
    if rng.random() < 0.18:
        keep = int(rng.uniform(0.35, 0.8) * len(t))
        x, y, t = x[:keep], y[:keep], t[:keep]
    return x, y, t


def stroke(cov, shade, x, y, t, width0, colour, rng):
    """Paint one tapering stroke into the coverage and colour buffers."""
    yy, xx = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64) + 0.5
    for i in range(len(x) - 1):
        w = width0 * (1.0 - 0.85 * t[i]) + 0.4
        x0, y0, x1, y1 = x[i], y[i], x[i + 1], y[i + 1]
        lo_x, hi_x = int(max(min(x0, x1) - w - 2, 0)), int(min(max(x0, x1) + w + 2, SIZE))
        lo_y, hi_y = int(max(min(y0, y1) - w - 2, 0)), int(min(max(y0, y1) + w + 2, SIZE))
        if lo_x >= hi_x or lo_y >= hi_y:
            continue
        px, py = xx[lo_y:hi_y, lo_x:hi_x], yy[lo_y:hi_y, lo_x:hi_x]
        dx, dy = x1 - x0, y1 - y0
        seg = max(dx * dx + dy * dy, 1e-6)
        s = np.clip(((px - x0) * dx + (py - y0) * dy) / seg, 0.0, 1.0)
        d = np.hypot(px - (x0 + s * dx), py - (y0 + s * dy))
        a = np.clip(w - d + 0.5, 0.0, 1.0)
        # Lit down one edge of the blade and dark down the other.
        across = ((px - x0) * dy - (py - y0) * dx) / np.sqrt(seg) / max(w, 0.5)
        tone = 0.85 + 0.25 * np.clip(across, -1.0, 1.0)
        region = cov[lo_y:hi_y, lo_x:hi_x]
        over = a > region
        region[over] = a[over]
        shade[lo_y:hi_y, lo_x:hi_x][over] = (colour * tone[..., None])[over]


def main():
    rng = np.random.default_rng(SEED)
    cov = np.zeros((SIZE, SIZE))
    shade = np.zeros((SIZE, SIZE, 3))
    for _ in range(BLADES):
        foot = SIZE * 0.5 + rng.normal(0.0, SIZE * 0.07)
        x, y, t = blade_path(rng, foot)
        mix = rng.random()
        colour = STRAW * mix + OLIVE * (1.0 - mix)
        # Darker toward the wet foot.
        stroke(cov, shade, x, y, t, rng.uniform(1.4, 2.4), colour * rng.uniform(0.75, 1.05), rng)
    for _ in range(CATTAILS):
        foot = SIZE * 0.5 + rng.normal(0.0, SIZE * 0.05)
        top = rng.uniform(0.7, 0.92) * SIZE
        lean = rng.normal(0.0, 0.06)
        t = np.linspace(0.0, 1.0, 48)
        x = foot + SIZE * lean * t
        y = top * t
        stroke(cov, shade, x, y, t * 0.3, 1.0, STRAW * 0.8, rng)
        # The head: a dark sausage near the top of the stem.
        h0, h1 = int(len(t) * 0.78), int(len(t) * 0.92)
        stroke(cov, shade, x[h0:h1], y[h0:h1], np.zeros(h1 - h0), 3.6, HEAD, rng)
    # The foot of the clump in the mud.
    yy = np.arange(SIZE)[:, None] + 0.5
    foot_dark = np.clip(1.0 - yy / (SIZE * 0.25), 0.0, 1.0)
    shade = shade * (1.0 - 0.6 * foot_dark[..., None]) + ROOT * 0.6 * foot_dark[..., None]

    # Rows were painted with y up; the file is stored bottom row first, which is y up already.
    albedo = np.concatenate([shade, cov[..., None]], axis=-1)
    normal = np.zeros((SIZE, SIZE, 3))
    normal[...] = [0.5, 0.5, 1.0]
    rough = np.full((SIZE, SIZE, 3), 0.85)

    def save(array, name, mode):
        img = Image.fromarray(np.clip(array * 255.0 + 0.5, 0, 255).astype(np.uint8), mode)
        img.save(os.path.join(OUT_DIR, "reeds_%s.png" % name))

    save(albedo, "albedo", "RGBA")
    save(normal, "normal", "RGB")
    save(rough, "rough", "RGB")
    print("reeds: %d px, %d blades and %d cattails" % (SIZE, BLADES, CATTAILS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
