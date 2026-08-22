#!/usr/bin/env python3
"""Generate the composite-cache instrument -- assets/layer_vt_fixture.* (spec 11.66).

A FLOOR and a 45-DEGREE RAMP sharing one layered material whose splat is authored
in WORLD XZ -- which is the whole reason this fixture exists beside layer_fixture:
that one reads its splat through UV1, and the composite cache serves world-XZ
materials only, so no arm on it can ever see the cache. This is the scene that
can.

The RAMP is the anti-vacuity, and the reason it is 45 degrees exactly. On the
flat floor the cached path is byte-exact by construction -- the macro is a
composite of per-layer top-mip means, the detail ratio is map/mean, and both
collapse to the per-texel answer wherever one layer at one projection owns the
texel -- so a floor-only fixture could not tell the cache being ON from the flag
doing nothing. The ramp's normal sits exactly between the Y and Z projections:
the per-texel path blends two projections of the checker with grain-resolved
heights, the cached path composes a Y-baked macro with a runtime triplanar
detail term, and the two are legitimately different pixels. An identity arm
asserts bytes on the floor AND a difference floor on the ramp, so a build that
never arms the cache fails the second half.

MAPS ARE REUSED, NOT REGENERATED. The four layer albedos and both surface maps
are gen_layer_fixture.py's committed files, imported by name -- the constants the
arms predict from (codes, heights, checker period, uv scale) must have exactly
one source, and this generator writes only its own geometry, its own splat and
its own scene file.

THE SPLAT, in world rows (v runs z = -HALF at the ramp's crest to z = +HALF at
the floor's near edge):

  ramp   (v < 0.5)        checker layer at full weight, full width. Crossings on
                          a two-projection surface, and the only region where
                          cached and per-texel may differ.
  floor  (0.5 <= v < .75) layer 2 ramping into layer 3 along u -- the height
                          blend's crossover, which is what the invalidation arm
                          watches move when layerBlend changes mid-run.
  floor  (v >= 0.75)      four hard columns, one layer each at full weight, for
                          byte reads at column centres.

Column edges land on texel boundaries of the FORCED-coarse atlas too
(VT_MACRO_RES below), so the macro arm's selection read stays pure at a
resolution where the checker is unrepresentable.

Regenerate with: python3 assets/gen_layer_vt_fixture.py
"""

import base64
import json
import math
import os
import struct
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_layer_fixture as base  # noqa: E402  (the one source for shared constants)

HALF = base.HALF          # the domain is [-HALF, HALF] on both axes, like the parent
DOMAIN = 2.0 * HALF
TEX = base.TEX

# The atlas resolution the vt-macro arm forces. Coarse enough that a checker
# cell is under a texel (nothing grain-shaped survives in the cache), aligned so
# a splat column is a whole number of texels (the selection read stays pure).
VT_MACRO_RES = 8

SPLAT_NAME = "layer_vt_fixture_splat.png"

# The engine's derived atlas resolution: clamp(next_pow2(domain / 0.5), 256, 2048).
# Restated here because the identity arm's exactness claim depends on the checker
# being fully resolved at it, and the assert below fails loudly if that stops
# being true rather than letting the arm go soft.
VT_DERIVED_RES_MIN = 256


def _splat():
    """rgb = weights for layers 1..3; layer 0 is whatever is left over."""
    img = np.zeros((TEX, TEX, 3), dtype=np.uint8)
    half = TEX // 2
    band = 3 * TEX // 4
    quarter = TEX // 4

    # Ramp rows: the checker layer alone, full width.
    img[0:half, :, 0] = 255

    # Transition band: layer 2 giving way to layer 3 along u. Two channels
    # moving in opposite directions rather than one rising from zero, so neither
    # endpoint is the "no weight" case layer 0 would claim.
    t = np.linspace(0.0, 1.0, TEX)
    img[half:band, :, 1] = np.round((1.0 - t) * 255).astype(np.uint8)[None, :]
    img[half:band, :, 2] = np.round(t * 255).astype(np.uint8)[None, :]

    # Column band: four hard columns, one layer each.
    for i in range(4):
        lo, hi = i * quarter, (i + 1) * quarter
        if i == 0:
            continue  # all channels zero -> layer 0 takes the remainder
        img[band:TEX, lo:hi, i - 1] = 255
    return img


def _assert_fixture_still_tests_something():
    """Every claim the vt arms rest on, checked here so a bad edit fails loudly."""
    # The ramp must sit exactly between the Y and Z projections, or the
    # "cached and per-texel legitimately differ here" claim weakens to taste.
    # Derived from the GEOMETRY, not restated beside it: the first version of
    # this assert compared a local literal against itself and could not fire.
    e1 = np.array(ramp_pos[1]) - np.array(ramp_pos[0])
    e2 = np.array(ramp_pos[3]) - np.array(ramp_pos[0])
    face_n = np.cross(e1, e2)
    face_n = face_n / np.linalg.norm(face_n)
    assert abs(face_n[1] - face_n[2]) < 1e-9, (
        "the ramp is no longer 45 degrees; the identity arm's difference floor "
        "rests on both projections being equally live there")
    assert np.allclose(face_n, np.array(ramp_nrm[0])), (
        "the ramp's authored normal disagrees with its own faces")

    # The derived atlas must OUT-RESOLVE the splat, which is where the identity
    # arm's residual edges come from -- the checker never lives in the atlas at
    # any resolution (the bake freezes grain at per-layer means), so the splat's
    # resampling is the only density the exactness claim depends on.
    assert VT_DERIVED_RES_MIN >= 2 * TEX, (
        f"the derived atlas ({VT_DERIVED_RES_MIN}) no longer out-resolves the "
        f"{TEX}-texel splat; the identity arm's byte-exact column reads go soft")

    cell = base.CHECKER_CELL_WORLD

    coarse_texel = DOMAIN / VT_MACRO_RES
    assert coarse_texel >= 2.0 * cell, (
        f"the forced-coarse texel ({coarse_texel}) no longer exceeds a checker "
        f"period ({2.0 * cell}); vt-macro would be reading a cache that can "
        "still hold the grain it claims cannot be there")

    column_w = DOMAIN / 4.0
    assert column_w % coarse_texel == 0, (
        "splat columns no longer align to the forced-coarse texel grid; the "
        "macro arm's centre read would straddle a bilinear seam")

    splat = _splat()
    quarter = TEX // 4
    band = 3 * TEX // 4
    for i in range(4):
        col = splat[band:TEX, i * quarter + quarter // 2, :]
        weights = col[0].astype(np.float32) / 255.0
        expected = np.zeros(3, dtype=np.float32)
        if i > 0:
            expected[i - 1] = 1.0
        assert np.allclose(weights, expected), (
            f"splat column {i} does not select layer {i} alone")

    # The transition band's endpoints must be pure so the crossover scan has
    # anchors, and the ramp rows must be the checker alone.
    assert splat[TEX // 2, 0, 1] == 255 and splat[TEX // 2, TEX - 1, 2] == 255, (
        "the transition band no longer runs pure layer 2 to pure layer 3")
    assert np.all(splat[0:TEX // 2, :, 0] == 255), "the ramp rows are not pure checker"
    assert np.all(splat[0:TEX // 2, :, 1:] == 0), "the ramp rows carry stray weight"


# --- geometry ---------------------------------------------------------------
# The splat is world-XZ, so no UV1 exists at all -- which is itself part of what
# the fixture proves: the world path must not depend on a vertex stream. UV0 is
# authored anyway (nothing here reads it) because assimp is happier with one.

floor_pos = [(-HALF, 0.0, DOMAIN / 2), (HALF, 0.0, DOMAIN / 2), (HALF, 0.0, 0.0), (-HALF, 0.0, 0.0)]
floor_nrm = [(0.0, 1.0, 0.0)] * 4

_s = 1.0 / math.sqrt(2.0)
ramp_pos = [(-HALF, 0.0, 0.0), (HALF, 0.0, 0.0), (HALF, HALF, -HALF), (-HALF, HALF, -HALF)]
ramp_nrm = [(0.0, _s, _s)] * 4

uv0 = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
indices = [0, 1, 2, 0, 2, 3]

_chunks = [
    (b"".join(struct.pack("<3f", *p) for p in floor_pos), 34962),
    (b"".join(struct.pack("<3f", *n) for n in floor_nrm), 34962),
    (b"".join(struct.pack("<3f", *p) for p in ramp_pos), 34962),
    (b"".join(struct.pack("<3f", *n) for n in ramp_nrm), 34962),
    (b"".join(struct.pack("<2f", *t) for t in uv0), 34962),
    (b"".join(struct.pack("<H", i) for i in indices), 34963),
]
buffer_bytes = b"".join(c for c, _ in _chunks)


# The parent's glTF plumbing, not a copy of it -- an offset or bounds fix
# there must reach this file without a second edit.
floor_mn, floor_mx = base._bounds(floor_pos)
ramp_mn, ramp_mx = base._bounds(ramp_pos)

GLTF = {
    "asset": {"version": "2.0", "generator": "gen_layer_vt_fixture.py"},
    "scene": 0,
    # RAMP FIRST, and the order is load-bearing for the feedback-occlusion arm:
    # the vote pass disables culling and its occlusion claim rests on the depth
    # test, but a hidden surface drawn BEFORE its occluder is also hidden by
    # painter's order -- so with the occluder last, deleting the depth test
    # changes nothing and the arm cannot see the deletion. Occluder first is
    # what makes depth the only thing rejecting the hidden floor.
    "scenes": [{"nodes": [1, 0]}],
    "nodes": [{"name": "vt_floor", "mesh": 0}, {"name": "vt_ramp", "mesh": 1}],
    "meshes": [
        {"name": "vt_floor",
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 4},
                         "indices": 5, "material": 0}]},
        {"name": "vt_ramp",
         "primitives": [{"attributes": {"POSITION": 2, "NORMAL": 3, "TEXCOORD_0": 4},
                         "indices": 5, "material": 0}]},
    ],
    # ONE material for both plates, as in the parent fixture: what differs
    # between them must be geometry, never authoring.
    "materials": [
        {"name": "layered_vt_surface",
         "pbrMetallicRoughness": {"baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": floor_mn, "max": floor_mx},
        {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": ramp_mn, "max": ramp_mx},
        {"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 4, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": 5, "componentType": 5123, "count": 6, "type": "SCALAR"},
    ],
    "bufferViews": base._views(_chunks),
    "buffers": [{"uri": "data:application/octet-stream;base64," +
                        base64.b64encode(buffer_bytes).decode("ascii"),
                 "byteLength": len(buffer_bytes)}],
}

CSCN = {
    "version": 1,
    "models": [{"path": "layer_vt_fixture.gltf"}],
    "materials": {
        "layered_vt_surface": {
            "splat": SPLAT_NAME,
            # Present = world-XZ addressing over this rectangle; the key IS the
            # space (spec 11.66).
            "splatDomain": [-HALF, -HALF, DOMAIN, DOMAIN],
            "layerBlend": base.LAYER_BLEND_SHARPNESS,
            "layers": [
                {"albedo": name,
                 "surface": base.RELIEF_NAME if i == 1 else base.SURFACE_NAME,
                 "uvScale": base.UV_SCALE}
                for i, (name, _) in enumerate(base.LAYERS)
            ],
        },
    },
    "lights": [{"name": "VtSun", "type": "directional",
                "direction": [-0.2, -0.6, -0.77], "color": [1.0, 1.0, 1.0],
                "intensity": 3.0, "cast_shadows": False}],
    # Both plates in one frame: the near columns fill the lower half, the ramp
    # rises across the upper half.
    "camera": {"eye": [0.0, 4.0, 7.0], "target": [0.0, 0.7, -0.5], "fov": 45},
    "post": {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False},
}


def main():
    _assert_fixture_still_tests_something()
    Image.fromarray(_splat(), "RGB").save(os.path.join(HERE, SPLAT_NAME))
    with open(os.path.join(HERE, "layer_vt_fixture.gltf"), "w") as f:
        json.dump(GLTF, f, indent=1)
        f.write("\n")
    with open(os.path.join(HERE, "layer_vt_fixture.cscn"), "w") as f:
        json.dump(CSCN, f, indent=1)
        f.write("\n")
    print(f"wrote layer_vt_fixture.gltf, layer_vt_fixture.cscn and {SPLAT_NAME} at {TEX}x{TEX}")


if __name__ == "__main__":
    main()
