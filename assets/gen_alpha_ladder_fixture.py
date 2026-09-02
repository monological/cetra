#!/usr/bin/env python3
"""Generate the alpha MIP LADDER -- assets/alpha_ladder_fixture.* (spec 11.101).

TWO ROWS OF CAMERA-FACING CARDS, EACH CARD ONE MIP LEVEL FURTHER DOWN THE CHAIN.

Every card is the same size at the same distance; what changes left to right
is how many TEXELS the card packs into the same screen pixels -- its UV range
doubles per card, so card k samples mip FIRST_MIP + k of the same seamless
texture. (Doubling distance and size together is the tempting construction
and it does nothing: the mip is a function of texels per screen pixel alone,
and that ratio cancels. This generator shipped that way first and produced six
identical cards.) One frame therefore shows every regime of an alpha-tested
texture side by side -- crisp, coverage-scaled (spec 11.88), distributed to a
binary dither (spec 11.100), deep -- under ONE anti-aliasing state, on a flat
backdrop with nothing else in it: the instrument for judging what the mip
chain does to a cutout BY EYE, which neither the coverage fixture nor the apps
offer. alphacov_fixture is a regular dot grid, the most Moire-prone content
there is, built to make the coverage MATH drift and deliberately unlike
foliage; tree and forest show the chain inside thousands of overlapping cards
where no one level is visible.

TWO ROWS BECAUSE THE CONTENT CLASS DECIDES WHERE THE CHAIN FIRES. The top row
is FOLIAGE-SHAPED: irregular soft-edged blobs at sparse coverage. On that
content the 11.88 rescale finds a scale that hits the target at every level
down to the 1x1 -- the levels keep enough variance for a scale to work --
so distribution never fires on a visible card and the row shows what real
leaves do: clump, never dither. The bottom row is HARD SPECKS: small
hard-edged discs at random positions, the irregular counterpart of the dot
grid. Isolated hard features average toward the background rather than
toward an edge, the chain goes structureless early, and distribution fires
around mip 3 -- so this row shows the dither regime and what the jittered
lookup does to it, on content with no lattice to beat against.

Regenerate with: python3 assets/gen_alpha_ladder_fixture.py
"""

import base64
import json
import math
import os
import struct

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))

TEX = 512
CUTOFF = 0.4           # the leaf materials' value; this fixture stands in for foliage
SEED = 11101

# Row 0: foliage-shaped blobs.
BLOBS = 90
BLOB_MIN, BLOB_MAX = 9.0, 22.0   # semi-axes, texels
EDGE = 1.2                       # soft-edge width, texels -- a real atlas's ~1-texel falloff
# Row 1: hard specks, the irregular twin of alphacov's dot grid.
SPECKS = 1400
SPECK_MIN, SPECK_MAX = 2.0, 3.5  # radii, texels -- hard-edged, like the dots

CARDS = 6              # mips FIRST_MIP .. FIRST_MIP + CARDS - 1, left to right
FIRST_MIP = 1
FOV = 50.0
DIST = 4.0             # every card's distance
CARD_H = 0.20          # every card's projected height, as a fraction of frame height
# Three rows, top to bottom: foliage blobs, hard specks, and the alphacov dot
# LATTICE itself -- the one content class on which distribution fires before
# the tail (level 3), so the row where the dither regime and the jittered
# lookup are actually visible, on facing geometry with no grazing anisotropy.
ROW_Y = (0.45, 0.0, -0.45)

# The framebuffer a gate render produces (gates.py CALIBRATED_FB_SCALE; the
# alphacov generator has the full note). Each card's mip is derived from it,
# so it is a constant and not a platform variable.
GATE_W = 400 * 2
GATE_H = 300 * 2

# UV range of card 0, chosen so it lands on FIRST_MIP; card k covers 2^k of it.
UV_TILE = (2.0 ** FIRST_MIP) * CARD_H * GATE_H / TEX


def _wrapped_offsets(n, cx, cy):
    """Texel offsets from (cx, cy), wrap-aware: a feature near the edge
    continues on the far side, so the texture tiles without a seam -- which
    the deeper cards depend on, since they repeat it many times over."""
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    dx = (x - cx + n / 2) % n - n / 2
    dy = (y - cy + n / 2) % n - n / 2
    return dx, dy


def _leaf_alpha(n, rng):
    """Irregular soft-edged blobs on transparent ground, plus a per-texel
    shade so the cards read as foliage rather than a chart."""
    alpha = np.zeros((n, n), dtype=np.float64)
    tint = np.zeros((n, n), dtype=np.float64)
    for _ in range(BLOBS):
        cx, cy = rng.uniform(0, n, 2)
        a, b = rng.uniform(BLOB_MIN, BLOB_MAX, 2)
        th = rng.uniform(0, math.pi)
        c, s = math.cos(th), math.sin(th)
        dx, dy = _wrapped_offsets(n, cx, cy)
        u = (c * dx + s * dy) / a
        v = (-s * dx + c * dy) / b
        # Distance to the ellipse boundary in texels along the normal --
        # close enough for a soft edge one texel wide.
        d = (np.hypot(u, v) - 1.0) * min(a, b)
        blob = np.clip(0.5 - d / EDGE, 0.0, 1.0)
        shade = rng.uniform(0.6, 1.0)
        tint = np.where(blob > alpha, shade, tint)
        alpha = np.maximum(alpha, blob)
    return alpha, tint


def _speck_alpha(n, rng):
    """Small HARD discs at random positions: the dot grid's drift behaviour
    without its lattice."""
    alpha = np.zeros((n, n), dtype=np.float64)
    for _ in range(SPECKS):
        cx, cy = rng.uniform(0, n, 2)
        r = rng.uniform(SPECK_MIN, SPECK_MAX)
        dx, dy = _wrapped_offsets(n, cx, cy)
        alpha = np.maximum(alpha, (np.hypot(dx, dy) <= r).astype(np.float64))
    return alpha, np.ones((n, n))


def _rgba(alpha, tint, base):
    """Colour, with the same mean colour BEHIND the transparent texels so the
    load-time dilate has nothing to change and the cards measure the alpha
    chain alone."""
    rgb = np.empty(alpha.shape + (3,), dtype=np.float64)
    for c in range(3):
        rgb[..., c] = np.where(alpha > 0.0, base[c] * (0.55 + 0.45 * tint), base[c] * 0.8)
    return np.dstack([np.rint(rgb * 255.0).astype(np.uint8),
                      np.rint(alpha * 255.0).astype(np.uint8)])


def _mip_of_card(k):
    """The mip card k selects in a gate render, from its texel density."""
    return math.log2(TEX * UV_TILE * (2.0 ** k) / (CARD_H * GATE_H))


def _row_geometry(ndc_y):
    """One row of CARDS facing quads, same size and distance, UV doubling."""
    t = math.tan(math.radians(FOV) * 0.5)
    aspect = GATE_W / GATE_H
    h = CARD_H * DIST * t            # half-height: projects to CARD_H of the frame
    y = ndc_y * DIST * t
    pos, nrm, uv, idx = [], [], [], []
    for k in range(CARDS):
        ndc_x = 2.0 * (k + 0.5) / CARDS - 1.0
        x = ndc_x * DIST * t * aspect
        tile = UV_TILE * (2.0 ** k)
        base = len(pos)
        pos += [(x - h, y - h, -DIST), (x + h, y - h, -DIST),
                (x + h, y + h, -DIST), (x - h, y + h, -DIST)]
        nrm += [(0.0, 0.0, 1.0)] * 4
        uv += [(0.0, 0.0), (tile, 0.0), (tile, tile), (0.0, tile)]
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return pos, nrm, uv, idx


def _mesh_chunks(pos, nrm, uv, idx):
    return [
        (b"".join(struct.pack("<3f", *p) for p in pos), 34962),
        (b"".join(struct.pack("<3f", *n) for n in nrm), 34962),
        (b"".join(struct.pack("<2f", *t) for t in uv), 34962),
        (b"".join(struct.pack("<H", i) for i in idx), 34963),
    ]


def _coverage(a8):
    return float((a8 >= int(CUTOFF * 255 + 0.5)).mean())


def main():
    rng = np.random.default_rng(SEED)
    leaves = _rgba(*_leaf_alpha(TEX, rng), base=np.array([0.22, 0.48, 0.16]))
    specks = _rgba(*_speck_alpha(TEX, rng), base=np.array([0.85, 0.80, 0.55]))
    cov_leaf, cov_speck = _coverage(leaves[..., 3]), _coverage(specks[..., 3])
    # Sparse enough that the deep levels' mean alpha sits well under the
    # cutoff, dense enough to read.
    assert 0.12 < cov_leaf < 0.30, f"leaf coverage {cov_leaf:.3f} outside [0.12, 0.30]"
    assert 0.06 < cov_speck < 0.20, f"speck coverage {cov_speck:.3f} outside [0.06, 0.20]"
    Image.fromarray(leaves, "RGBA").save(os.path.join(HERE, "alpha_ladder_leaves.png"))
    Image.fromarray(specks, "RGBA").save(os.path.join(HERE, "alpha_ladder_specks.png"))

    # One row per content class; the third reuses alphacov's committed dot
    # grid rather than regenerating it, so the lattice here IS the lattice the
    # coverage gate measures.
    names = ["ladder_leaves", "ladder_specks", "ladder_dots"]
    images = ["alpha_ladder_leaves.png", "alpha_ladder_specks.png", "alphacov_dots.png"]
    cutoffs = [CUTOFF, CUTOFF, 0.5]   # the dots keep alphacov's glTF-default cutoff
    rows = [_row_geometry(y) for y in ROW_Y]
    chunks = [c for row in rows for c in _mesh_chunks(*row)]
    buf = b"".join(c for c, _ in chunks)
    views, off = [], 0
    for data, target in chunks:
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(data), "target": target})
        off += len(data)

    def accessors(first_view, pos):
        mn = [min(p[i] for p in pos) for i in range(3)]
        mx = [max(p[i] for p in pos) for i in range(3)]
        return [
            {"bufferView": first_view, "componentType": 5126, "count": len(pos),
             "type": "VEC3", "min": mn, "max": mx},
            {"bufferView": first_view + 1, "componentType": 5126, "count": len(pos),
             "type": "VEC3"},
            {"bufferView": first_view + 2, "componentType": 5126, "count": len(pos),
             "type": "VEC2"},
            {"bufferView": first_view + 3, "componentType": 5123, "count": len(pos) // 4 * 6,
             "type": "SCALAR"},
        ]

    gltf = {
        "asset": {"version": "2.0", "generator": "gen_alpha_ladder_fixture.py"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(rows)))}],
        "nodes": [{"name": n, "mesh": i} for i, n in enumerate(names)],
        "meshes": [{"name": n, "primitives": [
            {"attributes": {"POSITION": 4 * i, "NORMAL": 4 * i + 1, "TEXCOORD_0": 4 * i + 2},
             "indices": 4 * i + 3, "material": i}]} for i, n in enumerate(names)],
        "materials": [{"name": n, "alphaMode": "MASK", "alphaCutoff": cutoffs[i],
                       "doubleSided": True,
                       "pbrMetallicRoughness": {"baseColorTexture": {"index": i},
                                                "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                                                "metallicFactor": 0.0, "roughnessFactor": 1.0}}
                      for i, n in enumerate(names)],
        "textures": [{"source": i} for i in range(len(images))],
        "images": [{"uri": u} for u in images],
        "accessors": [a for i, row in enumerate(rows) for a in accessors(4 * i, row[0])],
        "bufferViews": views,
        "buffers": [{"uri": "data:application/octet-stream;base64," +
                     base64.b64encode(buf).decode("ascii"), "byteLength": len(buf)}],
    }
    cscn = {
        "version": 1,
        "models": [{"path": "alpha_ladder_fixture.gltf"}],
        # Lit from just off the camera axis, no shadows: the cards are the
        # subject and a shadow would be a second thing in the frame.
        "lights": [{"name": "LadderKey", "type": "directional",
                    "direction": [0.0, -0.3, -0.95], "color": [1.0, 1.0, 1.0],
                    "intensity": 3.0, "cast_shadows": False}],
        "camera": {"eye": [0.0, 0.0, 0.0], "target": [0.0, 0.0, -1.0], "fov": FOV},
        "post": {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False},
    }
    with open(os.path.join(HERE, "alpha_ladder_fixture.gltf"), "w") as f:
        json.dump(gltf, f, indent=1)
    with open(os.path.join(HERE, "alpha_ladder_fixture.cscn"), "w") as f:
        json.dump(cscn, f, indent=1)
    mips = ", ".join(f"{_mip_of_card(k):.1f}" for k in range(CARDS))
    print(f"wrote alpha_ladder_fixture.gltf/.cscn, leaves (coverage {cov_leaf:.3f}) and "
          f"specks (coverage {cov_speck:.3f}) at cutoff {CUTOFF}; cards at mips {mips}")


if __name__ == "__main__":
    main()
