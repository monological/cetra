#!/usr/bin/env python3
"""Generate the tiling ivy-mat textures for assets/ivy_arcade (spec 11.51).

    python3 assets/ivy_arcade/gen_ivy_mat.py

Emits textures/ivy_mat_base.png, _normal.png and _rough.png at 2048, plus
textures/ivy_leaf_base.png -- the single leaf card, cropped tight and FLIPPED.

Composited rather than baked in Cycles, for three reasons. It tiles by
construction: every leaf is stamped at its position and again at every wrapped
offset, so the seam is not something to check afterwards. It is reproducible
from a seed, where a bake depends on sampler settings and a render device. And
it needs no Blender, so it regenerates in CI like every other asset here.

The mat and the fringe cards are built from ONE source leaf, so the opaque shell
and the masked silhouette are the same plant rather than two greens that have to
be matched by eye later.

DEPTH IS FAKED, AND DELIBERATELY. Leaves are stamped back to front and each one
darkens what is already under it, so the accumulated buffer carries an occlusion
gradient a flat scatter would not. That is what stops a dense mat reading as
green noise: real ivy is mostly SHADOW with lit leaves on top.

THE LEAF IS FLIPPED VERTICALLY, and this is the one non-obvious step. Spec
11.51 measured that the shader's UV0.y = 0 end is where a mode-2 card pivots,
and textures upload unflipped (async_loader.c), so shader t = 0 is the TOP image
row. The stem must therefore sit at the top of the cell. IvyLeafReal_base.png
has it at the bottom. Flipping here means the card generator can map v = 0 to
the stem without every consumer having to remember why.
"""

import os

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(ROOT, "assets", "abandoned_window", "textures", "IvyLeafReal_base.png")
OUT = os.path.join(HERE, "textures")

RES = 2048
LEAVES = 2900
SEED = 20260818
# Leaf long-axis length as a fraction of the tile. The tile covers 2 m of wall
# (UV_SCALE 0.5 in build_arcade.py), so 0.085 is a leaf about 17 cm across --
# large for ivy, but this mat is seen from a metre away down a tunnel and a
# botanically correct 6 cm leaf turns to mush at that distance.
LEAF_FRAC = 0.085
SIZE_JITTER = 0.35
# Each layer darkens what is beneath it. Tuned so the deepest leaves sit at
# roughly a fifth of the surface ones without going black -- ivy in shade is
# still green, it is not a hole.
SHADE = 0.045
SHADE_FLOOR = 0.18


def load_leaf():
    """The source leaf, cropped to its alpha and flipped stem-to-top."""
    im = Image.open(SRC).convert("RGBA")
    a = np.array(im)[:, :, 3]
    ys, xs = np.nonzero(a > 8)
    im = im.crop((xs.min(), ys.min(), xs.max() + 1, ys.max() + 1))
    return im.transpose(Image.FLIP_TOP_BOTTOM)


def stamp(rgb, hgt, cov, leaf, rng):
    """Composite one leaf, wrapped, darkening whatever is already beneath it."""
    ang = rng.uniform(0.0, 360.0)
    scale = LEAF_FRAC * RES * (1.0 + rng.uniform(-SIZE_JITTER, SIZE_JITTER))
    w = max(8, int(scale * leaf.width / max(leaf.width, leaf.height)))
    h = max(8, int(scale * leaf.height / max(leaf.width, leaf.height)))
    im = leaf.resize((w, h), Image.LANCZOS).rotate(ang, expand=True, resample=Image.BICUBIC)
    arr = np.asarray(im).astype(np.float32) / 255.0
    la, lh, lw = arr[:, :, 3], im.height, im.width

    # Per-leaf hue and value jitter, so a mat of one source image does not read
    # as one leaf repeated.
    tint = np.array([rng.uniform(0.72, 1.12), rng.uniform(0.85, 1.15),
                     rng.uniform(0.70, 1.05)], dtype=np.float32)
    col = np.clip(arr[:, :, :3] * tint, 0.0, 1.0)

    x0 = rng.integers(0, RES)
    y0 = rng.integers(0, RES)
    # Wrapped index arrays: the stamp lands on the torus, so the tile seams
    # without a post-hoc blend. This is the whole reason for compositing rather
    # than baking.
    xi = (np.arange(lw) + x0) % RES
    yi = (np.arange(lh) + y0) % RES
    gy, gx = np.ix_(yi, xi)

    a3 = la[:, :, None]
    rgb[gy, gx] = rgb[gy, gx] * (1.0 - a3) + col * a3
    # Everything already down goes a shade darker where this leaf covers it.
    dark = np.maximum(1.0 - SHADE * la, SHADE_FLOOR)[:, :, None]
    rgb[gy, gx] *= np.where(a3 > 0.5, 1.0, dark)
    hgt[gy, gx] = np.maximum(hgt[gy, gx], hgt[gy, gx] * (1.0 - la) + la * (hgt[gy, gx] + la))
    cov[gy, gx] = np.maximum(cov[gy, gx], la)


def normal_from_height(hgt, strength=2.4):
    """Sobel on the wrapped height field -> a tangent-space normal map."""
    h = hgt / max(hgt.max(), 1e-6)
    dx = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) * strength
    dy = (np.roll(h, -1, axis=0) - np.roll(h, 1, axis=0)) * strength
    n = np.stack([-dx, -dy, np.ones_like(h)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return ((n * 0.5 + 0.5) * 255.0).astype(np.uint8)


def main():
    os.makedirs(OUT, exist_ok=True)
    leaf = load_leaf()
    leaf.save(os.path.join(OUT, "ivy_leaf_base.png"))

    rng = np.random.default_rng(SEED)
    # Seeded with deep shade rather than black: the gaps between leaves in a
    # dense mat are shadowed leaf, not void, and a black floor reads as holes.
    # At 1400 leaves this showed through as hard voids over 6% of the tile, so
    # the count is set by COVERAGE rather than by how dense it looks.
    rgb = np.zeros((RES, RES, 3), np.float32) + np.array([0.055, 0.080, 0.042], np.float32)
    hgt = np.zeros((RES, RES), np.float32)
    cov = np.zeros((RES, RES), np.float32)
    for _ in range(LEAVES):
        stamp(rgb, hgt, cov, leaf, rng)

    Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8)).save(
        os.path.join(OUT, "ivy_mat_base.png"))
    Image.fromarray(normal_from_height(hgt)).save(os.path.join(OUT, "ivy_mat_normal.png"))
    # Leaves are waxy where they catch the light and matt where they are buried,
    # so roughness tracks the height field inversely.
    h = hgt / max(hgt.max(), 1e-6)
    rough = np.clip(0.86 - 0.34 * h, 0.0, 1.0)
    Image.fromarray((rough * 255).astype(np.uint8)).save(
        os.path.join(OUT, "ivy_mat_rough.png"))

    print(f"wrote {OUT}: mat {RES}px from {LEAVES} leaves (seed {SEED}), "
          f"coverage {float((cov > 0.5).mean()) * 100:.1f}%, leaf card "
          f"{leaf.width}x{leaf.height} flipped stem-to-top")


if __name__ == "__main__":
    main()
