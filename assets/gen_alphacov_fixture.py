#!/usr/bin/env python3
"""Generate the mip-alpha-coverage instrument -- assets/alphacov_fixture.* (spec 11.87).

A LONG PLANE RECEDING FROM THE CAMERA, wearing an alpha-tested DOT GRID.

Two things have to be true at once for this to test anything, and neither is
true of any other fixture in the corpus.

THE PLANE HAS TO REACH DEEP MIPS. Coverage preservation is a property of the
mip chain, and a fixture that never minifies never reaches it -- painting every
mip level solid black leaves all 29 goldens at 0 px, which is what sent spec
11.85 to a receding plane in the first place. This borrows that geometry.

AND ITS COVERAGE HAS TO ACTUALLY DRIFT. That is the part `texcomp_fixture`
cannot do and the leaf atlases turn out not to do either: measured on
apps/tree's leaf atlas, the fraction of texels above the cutoff runs 0.3957 at
level 0 and 0.3964 six levels down. A soft edge one texel wide is SYMMETRIC
about the threshold, so box-filtering moves as many texels up as down and the
surviving fraction barely moves. Preservation is inert on it.

SMALL ISOLATED DOTS are the shape that does drift, and they drift hard. A dot a
couple of texels across is entirely INSIDE one texel's footprint two levels
down, so the average is the dot's area over the footprint -- alpha falls toward
the background rather than toward the mean of an edge, and the whole grid drops
under the cutoff at once. Level 0 carries ~11% coverage here and an unpreserved
chain reaches 0% by level 3: the dots do not thin, they vanish.

The material is ALPHA_MASK at cutoff 0.5 -- the glTF default, and deliberately
not the 0.4 both leaf materials use, so an arm reading this cannot pass by
accident on a hardcoded 0.4.

Regenerate with: python3 assets/gen_alphacov_fixture.py
"""

import base64
import json
import math
import os
import struct

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))

TEX = 512            # power of two, so the chain halves cleanly to 1x1
DOT_PITCH = 16       # texels between dot centres
DOT_RADIUS = 3.0     # texels; see the assert on level-0 coverage below
CUTOFF = 0.5         # glTF's default, and NOT the 0.4 the leaf materials use

NEAR, FAR = 1.0, 400.0
HALF_W = 200.0
UV_PER_UNIT = 0.25

# Camera, shared with the .cscn below so the generator's reach assert and the
# scene cannot disagree.
CAM_Y, CAM_TARGET_Z, FOV = 8.0, -30.0, 50.0

# The framebuffer height a gate render actually produces. It is a CONSTANT and
# not a platform variable: gates.py scales its own request so a 1x display asks
# for double and lands on the same framebuffer as a HiDPI one, precisely so
# nothing downstream has to know which it is on. Must track CALIBRATED_FB_SCALE
# there -- every mip level below shifts by one if they disagree.
GATE_H = 300 * 2

# The three horizontal bands every arm reads, as fractions of frame height, and
# they live HERE rather than in gates.py because where they land is a fact about
# this geometry -- the asserts below check each one against the camera it is
# derived from, which a copy in the harness could not do.
#
# NEAR is level 0's reference. MID is far enough to have drifted and near enough
# that preservation still has something to hold. FAR is past the distance where
# the chain goes UNIFORM, and it is the one that reads the opposite way from the
# other two: a correct chain has nothing left to preserve out there and thins to
# nothing, where a saturating one fills in solid.
BAND_NEAR = (0.80, 0.98)
BAND_MID = (0.62, 0.72)
BAND_FAR = (0.26, 0.34)


def _alpha_dots(n):
    """A grid of small opaque dots on transparent ground, RGBA.

    RGB is WHITE everywhere including behind the transparent texels, so the
    transparent-texel dilate has nothing to change and this fixture measures the
    alpha chain alone. A coloured background here would make the dilate a second
    variable in every reading.
    """
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    cx = (np.floor(x / DOT_PITCH) + 0.5) * DOT_PITCH
    cy = (np.floor(y / DOT_PITCH) + 0.5) * DOT_PITCH
    d = np.hypot(x - cx, y - cy)
    # A hard disc, not a soft one. A soft edge is the symmetric case that does
    # not drift -- the very thing this fixture exists to be unlike.
    a = np.where(d <= DOT_RADIUS, 255, 0).astype(np.uint8)
    rgb = np.full((n, n, 3), 255, dtype=np.uint8)
    return np.dstack([rgb, a])


def _box_halve_alpha(a):
    """The engine's own 2x2 box filter on alpha, for the drift assert below."""
    h, w = a.shape
    q = a.astype(np.int32).reshape(h // 2, 2, w // 2, 2)
    return ((q.sum(axis=(1, 3)) + 2) // 4).astype(np.uint8)


def _coverage(a, cutoff):
    return float((a >= int(cutoff * 255 + 0.5)).mean())


def _plane_z_at_row(y_frac):
    """World distance of the plane point that projects to this frame row.

    The camera looks DOWN at a ground plane, so a row maps to a distance and the
    mapping runs to infinity at the horizon -- which is why every band below has
    to be checked rather than eyeballed: a band a few percent too high is not a
    band slightly further away, it is one off the end of the plane entirely.
    """
    pitch = math.atan2(CAM_Y, abs(CAM_TARGET_Z))
    ndc_y = 1.0 - 2.0 * y_frac
    above_axis = math.atan(ndc_y * math.tan(math.radians(FOV) * 0.5))
    below_horizon = pitch - above_axis
    if below_horizon <= 0.0:
        return math.inf          # at or above the horizon; the plane never gets there
    return CAM_Y / math.tan(below_horizon)


def _mip_at_z(z):
    """The mip level the plane selects at world distance z, in a gate render."""
    if not math.isfinite(z):
        return math.inf
    px = 2.0 * z * math.tan(math.radians(FOV) * 0.5) / GATE_H
    texels_per_px = px * UV_PER_UNIT * TEX * (z / CAM_Y)
    return math.log2(max(texels_per_px, 1.0))


# --- geometry: one long quad, near edge at NEAR, far edge at FAR -------------
pos = [(-HALF_W, 0.0, -NEAR), (HALF_W, 0.0, -NEAR),
       (HALF_W, 0.0, -FAR), (-HALF_W, 0.0, -FAR)]
nrm = [(0.0, 1.0, 0.0)] * 4
_u = 2.0 * HALF_W * UV_PER_UNIT
_v = (FAR - NEAR) * UV_PER_UNIT
uv = [(0.0, 0.0), (_u, 0.0), (_u, _v), (0.0, _v)]
idx = [0, 1, 2, 0, 2, 3]

_chunks = [
    (b"".join(struct.pack("<3f", *p) for p in pos), 34962),
    (b"".join(struct.pack("<3f", *n) for n in nrm), 34962),
    (b"".join(struct.pack("<2f", *t) for t in uv), 34962),
    (b"".join(struct.pack("<H", i) for i in idx), 34963),
]
buf = b"".join(c for c, _ in _chunks)


def _views(chunks):
    views, off = [], 0
    for data, target in chunks:
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(data), "target": target})
        off += len(data)
    return views


mn = [min(p[i] for p in pos) for i in range(3)]
mx = [max(p[i] for p in pos) for i in range(3)]

GLTF = {
    "asset": {"version": "2.0", "generator": "gen_alphacov_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"name": "alphacov_plane", "mesh": 0}],
    "meshes": [{"name": "alphacov_plane", "primitives": [
        {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
         "indices": 3, "material": 0}]}],
    "materials": [{
        "name": "alphacov_dots",
        "alphaMode": "MASK",
        "alphaCutoff": CUTOFF,
        "doubleSided": True,
        "pbrMetallicRoughness": {
            "baseColorTexture": {"index": 0},
            "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
            "metallicFactor": 0.0, "roughnessFactor": 1.0},
    }],
    "textures": [{"source": 0}],
    "images": [{"uri": "alphacov_dots.png"}],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": mn, "max": mx},
        {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"},
    ],
    "bufferViews": _views(_chunks),
    "buffers": [{"uri": "data:application/octet-stream;base64," +
                 base64.b64encode(buf).decode("ascii"), "byteLength": len(buf)}],
}

CSCN = {
    "models": [{"path": "alphacov_fixture.gltf"}],
    # The key is "eye", not "position" -- a wrong one parses, warns nowhere and
    # leaves the auto-framed camera in place, which is the trap texcomp_fixture
    # and decal_fixture both record.
    "camera": {"eye": [0.0, CAM_Y, 0.0], "target": [0.0, 0.0, CAM_TARGET_Z], "fov": FOV},
    # Pinned and flat: every arm here counts surviving dots, so anything that
    # could move the frame for a second reason is nailed down first.
    "post": {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False},
}


def main():
    # Built and written HERE, never at module scope: gates.py imports these
    # generators for their constants, and one that writes on import rewrites a
    # committed asset mid-run. lut_fixture records walking into exactly that.
    img = _alpha_dots(TEX)
    alpha0 = img[..., 3]

    # THE TWO PROPERTIES THIS FIXTURE EXISTS FOR, asserted rather than hoped.
    #
    # First: level 0 has to carry a coverage worth losing. Too few dots and the
    # far band is empty either way; too many and they merge into a sheet whose
    # coverage is as stable as a soft edge's.
    cov0 = _coverage(alpha0, CUTOFF)
    assert 0.05 < cov0 < 0.25, \
        f"level-0 coverage {cov0:.3f} outside [0.05, 0.25]: too sparse to read or too solid to drift"

    # Second, and the one that actually distinguishes this fixture: the coverage
    # must COLLAPSE down an unpreserved chain. If it does not, preservation is
    # inert here and an arm reading this proves nothing -- which is the state the
    # leaf atlases are in, at 0.3957 against 0.3964 six levels down.
    a, drifted, uniform_level = alpha0, [], None
    for lvl in range(1, 10):
        a = _box_halve_alpha(a)
        if lvl <= 4:
            drifted.append(_coverage(a, CUTOFF))
        if uniform_level is None and a.min() == a.max():
            uniform_level = lvl
    assert drifted[-1] < cov0 * 0.25, (
        f"coverage only fell {cov0:.3f} -> {drifted[-1]:.3f} over four levels; this fixture "
        f"exists because that collapse is what preservation restores")

    # The far band reads SATURATION, and saturation needs a level with no
    # structure left in it: once every texel is equal, no scale can reproduce a
    # fractional coverage, and a rescale that tries anyway drives the whole level
    # to one side of the cutoff. Without a uniform level this fixture has nothing
    # for that band to catch.
    assert uniform_level is not None, \
        "the chain never goes uniform; the far band has no saturation to read"

    Image.fromarray(img, "RGBA").save(os.path.join(HERE, "alphacov_dots.png"))

    # And that each band lands where its arm assumes. The depth is a joint
    # function of six numbers across two files, and any one of them can quietly
    # return this to being a fixture that never leaves level 0 -- or slide a band
    # off the plane, where it reads backdrop and reports a number anyway.
    for name, (top, bottom) in (("NEAR", BAND_NEAR), ("MID", BAND_MID), ("FAR", BAND_FAR)):
        z_near, z_far = _plane_z_at_row(bottom), _plane_z_at_row(top)
        assert math.isfinite(z_far) and z_far <= FAR, \
            f"band {name} reaches {z_far:.0f} units, past the plane's far edge at {FAR:.0f}"
        assert z_near >= NEAR, \
            f"band {name} starts at {z_near:.0f} units, in front of the plane's near edge"

    # Then which LEVEL each band selects, and the bound to take is whichever row
    # of the band is the weak end of the claim -- the nearest row for a floor,
    # the farthest for a ceiling. The two below only look symmetric.
    far_mip = _mip_at_z(_plane_z_at_row(BAND_FAR[1]))
    assert far_mip >= uniform_level, (
        f"the FAR band's nearest row reaches only mip {far_mip:.1f}, but the chain does not go "
        f"uniform until level {uniform_level}")

    # NEAR is the level-0 reference, so its FARTHEST row is what must still be
    # short of uniform.
    near_mip = _mip_at_z(_plane_z_at_row(BAND_NEAR[0]))
    assert near_mip < uniform_level, \
        f"the NEAR band already reaches mip {near_mip:.1f}; it is the level-0 reference"

    # MID is the one that fails SILENTLY and in the PASSING direction: slide it
    # toward NEAR and the ratio rises toward 1 with the feature switched off
    # entirely, which is the vacuity this whole fixture argues about. So it has
    # to sit where an unpreserved chain has already collapsed -- the property
    # ALPHACOV_MIN_RATIO's 0.591 figure comes from -- and still short of uniform,
    # where there would be nothing left to preserve.
    mid_far = _mip_at_z(_plane_z_at_row(BAND_MID[0]))
    assert mid_far < uniform_level, (
        f"MID reaches mip {mid_far:.1f}, at or past the uniform level {uniform_level} where "
        f"there is nothing left to preserve")
    assert drifted[min(int(mid_far), len(drifted)) - 1] < cov0 * 0.25, (
        "an unpreserved chain has not collapsed by the level MID selects, so the arm would "
        "read a ratio near 1 with no preservation at all")

    with open(os.path.join(HERE, "alphacov_fixture.gltf"), "w") as f:
        json.dump(GLTF, f, indent=1)
    with open(os.path.join(HERE, "alphacov_fixture.cscn"), "w") as f:
        json.dump(CSCN, f, indent=1)
    print(f"wrote alphacov_fixture.gltf, alphacov_fixture.cscn and a {TEX}x{TEX} dot grid; "
          f"coverage {cov0:.3f} -> " + " -> ".join(f"{c:.3f}" for c in drifted))


if __name__ == "__main__":
    main()
