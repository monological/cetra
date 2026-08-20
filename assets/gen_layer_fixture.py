#!/usr/bin/env python3
"""Generate the layered-surface instrument -- assets/layer_fixture.* (spec 11.60).

A FLOOR and a WALL sharing one layered material, plus the six painted maps that
material blends between. Two plates whose normals are PERPENDICULAR, because the
projection is world-aligned: a fixture with one plate tests one axis of it and
would read exactly the same whether the other two were implemented or not.

GROUND TRUTH IS PAINTED, NOT MEASURED. Every arm reads the frame through
`--render-mode 6`, the albedo view, where the shader's output is
`linearToSRGB(albedo_factor * sRGBToLinear(blend))` and the material's factor is
white -- so a correct renderer hands back the exact code this script wrote, with
no lighting, exposure or tonemap in the path to argue about. That is the whole
reason the fixture is legible: a lit read would be a claim about the BRDF as much
as about the blend.

WHAT EACH MAP IS FOR, since none of them is decoration:

  layer 0  flat mid-grey, and mid-grey EXACTLY.  The sRGB round trip. The albedo
           of a layer is stored in un-decoded codes and decoded after the blend,
           so a hardware decode creeping back in -- or a decode applied twice --
           moves this band and nothing else. 128 is chosen because it is as far
           from both endpoints as a code can be, where sRGB's curvature is
           steepest and a spurious decode is worth ~55 codes.
  layer 1  a CHECKER at an exact period, and the only layer with a relief
           surface map. The triplanar read twice over: the same texture must
           appear at the same SIZE on the floor and on the wall (a top-down
           projection stretches it by 1/cos(slope), unbounded on a vertical
           wall), and its relief is the only thing in the fixture that can make
           triplanarBlendNormal's axis-sign flips move a pixel.
  layer 2  flat dark, height HIGH.      The height blend's winner.
  layer 3  flat light, height LOW.      Its loser. Two flat colours rather than
           two patterns, so the transition between them is the only structure in
           that band and its WIDTH is directly measurable.

The flat surface map is SHARED by layers 0, 2 and 3, deliberately:
`mask_layer_for` dedups by GL id, so one file used three times must occupy
exactly one array layer. That is the dedup path, exercised by a fixture that
would otherwise never touch it.

THE SPLAT IS THE FIXTURE'S OTHER HALF. Its lower band selects the four layers in
four hard columns -- each a single layer at full weight, so `layers-select` reads
a flat known colour rather than a blend it would have to model. Its upper band
ramps layer 2 into layer 3 across the full width, which is what gives
`layers-height` a transition to measure: under the height blend the dark layer
holds its ground until the ramp overwhelms its height advantage and then gives
way over a few texels, where a linear blend crosses over gradually across the
whole width.

Regenerate with: python3 assets/gen_layer_fixture.py
"""

import base64
import json
import os
import struct

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))

TEX = 64  # every map, so the array's canonical size is 64 and the blit is 1:1

# The plates. Four units square, meeting along the line z = -HALF, y = 0 like a
# floor and the wall behind it.
HALF = 2.0
WALL_H = 4.0

# World units per texture tile. At 2.0 a checker cell is a quarter of a world
# unit, so a scan across one splat column crosses four of them -- coarse enough
# to survive mip selection at the gate's render size, which the same pattern at
# 1.0 does not.
UV_SCALE = 2.0

STRIPE_PERIOD = 16  # texels; divides TEX exactly, so the tiling has no seam
STRIPE_DARK = 32
STRIPE_LIGHT = 224

# One checker cell, in WORLD units -- half a period, scaled out of texture space.
# Derived rather than written down because the triplanar arm predicts the size it
# should measure from exactly this, and a second literal is a second thing to
# forget when any of the three inputs moves.
CHECKER_CELL_WORLD = UV_SCALE * (STRIPE_PERIOD / 2) / TEX

GREY_CODE = 128  # layer 0, and the sRGB round trip's whole subject
DARK_CODE = 48   # layer 2
LIGHT_CODE = 208  # layer 3
DARK_HEIGHT = 230  # layer 2 stands proud
LIGHT_HEIGHT = 26  # layer 3 sits low

# A flat tangent normal (0,0,1 encoded), roughness and full ambient occlusion.
# Flat on purpose: a surface map with relief in it would move the albedo view not
# at all and the lit golden in ways that have nothing to do with the blend.
SURFACE_NORMAL_XY = 128
SURFACE_ROUGHNESS = 230
SURFACE_AO = 255

# Layer 1's relief. The tilt is large enough that the shading gradient across a
# checker cell is unmistakable; the roughness and AO are far from the shared
# map's so a layer-selection error in the surface pass has somewhere to show.
RELIEF_TILT = 0.7
RELIEF_ROUGHNESS = 90
RELIEF_AO = 200
RELIEF_NAME = "layer_fixture_relief.png"

# Authored rather than left to create_material's default, because the gate
# predicts the height blend's crossover position in closed form from it: a
# prediction against an unstated default is a prediction about two things.
LAYER_BLEND_SHARPNESS = 0.5
# Must match LAYER_BLEND_RANGE in layers.glsl. Only the width read uses it.
LAYER_BLEND_RANGE = 0.12


def _albedo(rgb, height):
    """One layer map: rgb in stored codes, alpha carrying the layer's height."""
    img = np.zeros((TEX, TEX, 4), dtype=np.uint8)
    img[:, :, 0:3] = rgb
    img[:, :, 3] = height
    return img


def _checker():
    """A CHECKER, not stripes, and the difference is the whole triplanar arm.

    Stripes running along one axis would be projected onto the vertical wall
    identically by the correct Z projection and by a broken one that had used Y
    for everything -- the wall sits at constant z, so a Y projection's second
    coordinate is frozen and the pattern still varies with x exactly as it should.
    A checker varies on BOTH axes, so the frozen coordinate shows up as a wall
    with no vertical structure at all.
    """
    img = np.zeros((TEX, TEX, 4), dtype=np.uint8)
    half = STRIPE_PERIOD // 2
    idx = np.arange(TEX) // half
    cell = (idx[None, :] + idx[:, None]) % 2
    img[:, :, 0:3] = np.where(cell == 0, STRIPE_DARK, STRIPE_LIGHT)[:, :, None]
    img[:, :, 3] = 128
    return img


def _surface():
    img = np.zeros((TEX, TEX, 4), dtype=np.uint8)
    img[:, :, 0] = SURFACE_NORMAL_XY
    img[:, :, 1] = SURFACE_NORMAL_XY
    img[:, :, 2] = SURFACE_ROUGHNESS
    img[:, :, 3] = SURFACE_AO
    return img


def _surface_relief():
    """Layer 1's own surface map, and the only one in the fixture with relief.

    Without it the whole normal half of the feature is untestable. A flat tangent
    normal is an exact algebraic identity through triplanarBlendNormal -- the
    three swizzled terms collapse to the geometric normal times the weights,
    which sum to one -- so the three axis-sign flips, the subtlest arithmetic in
    the include, could be deleted and the lit golden would not move by a pixel.
    Same for per-layer roughness and AO while every layer shares one map.

    A diagonal ramp rather than a bump: it varies on both axes, so a projection
    that has swapped or frozen a coordinate produces a visibly different shading
    gradient, and it is monotone, so its sign is readable rather than symmetric.
    """
    img = np.zeros((TEX, TEX, 4), dtype=np.uint8)
    t = np.linspace(-1.0, 1.0, TEX)
    nx = np.tile(t[None, :], (TEX, 1))
    nz = np.tile(t[:, None], (1, TEX))
    img[:, :, 0] = np.round((nx * RELIEF_TILT * 0.5 + 0.5) * 255).astype(np.uint8)
    img[:, :, 1] = np.round((nz * RELIEF_TILT * 0.5 + 0.5) * 255).astype(np.uint8)
    img[:, :, 2] = RELIEF_ROUGHNESS
    img[:, :, 3] = RELIEF_AO
    return img


def _splat():
    """rgb = weights for layers 1..3; layer 0 is whatever is left over."""
    img = np.zeros((TEX, TEX, 3), dtype=np.uint8)
    half = TEX // 2
    quarter = TEX // 4

    # Lower band (v < 0.5): four hard columns, one layer each at full weight.
    # Row 0 is v = 0 and the image's first row, so "lower" here means the first
    # half of the array -- which the UV1 mapping below sends to the near edge.
    for i in range(4):
        lo, hi = i * quarter, (i + 1) * quarter
        if i == 0:
            continue  # all three channels zero -> layer 0 takes the remainder
        img[0:half, lo:hi, i - 1] = 255

    # Upper band: layer 2 ramping into layer 3 across the full width. Two
    # channels moving in opposite directions rather than one rising from zero,
    # so neither endpoint is the "no weight" case that layer 0 would claim.
    t = np.linspace(0.0, 1.0, TEX)
    img[half:TEX, :, 1] = np.round((1.0 - t) * 255).astype(np.uint8)[None, :]
    img[half:TEX, :, 2] = np.round(t * 255).astype(np.uint8)[None, :]
    return img


LAYERS = [
    ("layer_fixture_grey.png", _albedo(GREY_CODE, 128)),
    ("layer_fixture_checker.png", _checker()),  # pairs with RELIEF_NAME, not SURFACE_NAME
    ("layer_fixture_dark.png", _albedo(DARK_CODE, DARK_HEIGHT)),
    ("layer_fixture_light.png", _albedo(LIGHT_CODE, LIGHT_HEIGHT)),
]
SURFACE_NAME = "layer_fixture_surface.png"
SPLAT_NAME = "layer_fixture_splat.png"


def _assert_fixture_still_tests_something():
    """Every claim the arms rest on, checked here so a bad edit fails loudly.

    A fixture is a claim, and one that has stopped discriminating still renders a
    perfectly plausible picture.
    """
    flats = [GREY_CODE, DARK_CODE, LIGHT_CODE]
    for a in range(len(flats)):
        for b in range(a + 1, len(flats)):
            assert abs(flats[a] - flats[b]) >= 60, (
                f"layers {flats[a]} and {flats[b]} are too close to tell apart; "
                "layers-select would pass against a renderer that picked either")

    # The height blend moves the crossover, it does NOT narrow the transition --
    # the 10-90 width is 0.889 x LAYER_BLEND_RANGE and is algebraically
    # independent of the heights. The first version of this assert claimed the
    # opposite and was the reason layers-height could not see the height term at
    # all. What must be guaranteed is that the two crossovers are far apart.
    _shift = (DARK_HEIGHT - LIGHT_HEIGHT) / 255.0 * LAYER_BLEND_SHARPNESS * 0.5
    assert _shift >= 0.15, (
        f"the height blend shifts the crossover by only {_shift:.3f} of the ramp; "
        "layers-height cannot separate that from the linear leg's 0.5")

    assert GREY_CODE == 128, (
        "layer 0 is no longer mid-code; layers-srgb's expected value is the whole "
        "point of that number")

    assert STRIPE_LIGHT - STRIPE_DARK >= 150, "the checker is too faint to count reliably"
    assert TEX % STRIPE_PERIOD == 0, (
        "the checker period does not divide the texture, so the pattern has a seam "
        "at every tile boundary and the count is off by one per tile")

    # The property the triplanar arm rests on: the pattern must vary along BOTH
    # texture axes. One-axis stripes read identically on the wall under the
    # correct projection and under one that had frozen a coordinate.
    checker = _checker()[:, :, 0].astype(np.int16)
    assert np.abs(np.diff(checker, axis=0)).max() >= 150, (
        "layer 1 does not vary along v; the triplanar arm cannot tell the Z "
        "projection from a Y projection frozen at the wall's own z")
    assert np.abs(np.diff(checker, axis=1)).max() >= 150, (
        "layer 1 does not vary along u")

    # The two plates must genuinely exercise different axes of the projection.
    floor_n = np.array([0.0, 1.0, 0.0])
    wall_n = np.array([0.0, 0.0, 1.0])
    assert abs(float(np.dot(floor_n, wall_n))) < 1e-6, (
        "the plates are no longer perpendicular; the triplanar arm would compare "
        "one axis against itself")

    splat = _splat()
    quarter = TEX // 4
    half = TEX // 2
    for i in range(4):
        col = splat[0:half, i * quarter + quarter // 2, :]
        weights = col[0].astype(np.float32) / 255.0
        expected = np.zeros(3, dtype=np.float32)
        if i > 0:
            expected[i - 1] = 1.0
        assert np.allclose(weights, expected), (
            f"splat column {i} does not select layer {i} alone; layers-select "
            "would be reading a blend it does not model")


def _write_maps():
    for name, arr in LAYERS:
        Image.fromarray(arr, "RGBA").save(os.path.join(HERE, name))
    Image.fromarray(_surface(), "RGBA").save(os.path.join(HERE, SURFACE_NAME))
    Image.fromarray(_surface_relief(), "RGBA").save(os.path.join(HERE, RELIEF_NAME))
    Image.fromarray(_splat(), "RGB").save(os.path.join(HERE, SPLAT_NAME))


# --- geometry ---------------------------------------------------------------
# UV1 is the splat coordinate and spans [0,1] over each plate exactly once. UV0
# is required as well: glTF wants contiguous TEXCOORD_n and assimp drops
# TEXCOORD_1 outright when TEXCOORD_0 is absent -- the same trap gen_wind_uv
# records. Nothing reads UV0 here, because the layers are world-aligned.

floor_pos = [(-HALF, 0.0, HALF), (HALF, 0.0, HALF), (HALF, 0.0, -HALF), (-HALF, 0.0, -HALF)]
floor_nrm = [(0.0, 1.0, 0.0)] * 4
floor_uv1 = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]

wall_pos = [(-HALF, 0.0, -HALF), (HALF, 0.0, -HALF), (HALF, WALL_H, -HALF), (-HALF, WALL_H, -HALF)]
wall_nrm = [(0.0, 0.0, 1.0)] * 4
wall_uv1 = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]

indices = [0, 1, 2, 0, 2, 3]
# DELIBERATELY not the same as UV1. Authored identically, "the splat reads UV1"
# is untestable -- swapping the shader to TexCoords moves zero pixels. Doubled
# and v-flipped, so a slip to UV0 shows as four splat tiles upside down.
uv0 = [(0.0, 2.0), (2.0, 2.0), (2.0, 0.0), (0.0, 0.0)]

_chunks = [
    (b"".join(struct.pack("<3f", *p) for p in floor_pos), 34962),
    (b"".join(struct.pack("<3f", *n) for n in floor_nrm), 34962),
    (b"".join(struct.pack("<2f", *t) for t in floor_uv1), 34962),
    (b"".join(struct.pack("<3f", *p) for p in wall_pos), 34962),
    (b"".join(struct.pack("<3f", *n) for n in wall_nrm), 34962),
    (b"".join(struct.pack("<2f", *t) for t in wall_uv1), 34962),
    (b"".join(struct.pack("<2f", *t) for t in uv0), 34962),
    (b"".join(struct.pack("<H", i) for i in indices), 34963),
]
buffer_bytes = b"".join(c for c, _ in _chunks)


def _views(chunks):
    views, offset = [], 0
    for data, target in chunks:
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data),
                      "target": target})
        offset += len(data)
    return views


def _bounds(pts):
    return ([min(p[i] for p in pts) for i in range(3)],
            [max(p[i] for p in pts) for i in range(3)])


floor_mn, floor_mx = _bounds(floor_pos)
wall_mn, wall_mx = _bounds(wall_pos)

GLTF = {
    "asset": {"version": "2.0", "generator": "gen_layer_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1]}],
    "nodes": [{"name": "layer_floor", "mesh": 0}, {"name": "layer_wall", "mesh": 1}],
    "meshes": [
        {"name": "layer_floor",
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 6,
                                        "TEXCOORD_1": 2},
                         "indices": 7, "material": 0}]},
        {"name": "layer_wall",
         "primitives": [{"attributes": {"POSITION": 3, "NORMAL": 4, "TEXCOORD_0": 6,
                                        "TEXCOORD_1": 5},
                         "indices": 7, "material": 0}]},
    ],
    # ONE material for both plates: the projection is what must differ between
    # them, not the authoring. Two materials would let a change touch one plate.
    "materials": [
        {"name": "layered_surface",
         "pbrMetallicRoughness": {"baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": floor_mn, "max": floor_mx},
        {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": wall_mn, "max": wall_mx},
        {"bufferView": 4, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 5, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": 6, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": 7, "componentType": 5123, "count": 6, "type": "SCALAR"},
    ],
    "bufferViews": _views(_chunks),
    "buffers": [{"uri": "data:application/octet-stream;base64," +
                        base64.b64encode(buffer_bytes).decode("ascii"),
                 "byteLength": len(buffer_bytes)}],
}

# The camera sees the near edge of the floor and the whole wall, with the seam
# between them across the middle of frame -- both plates in one image, because an
# arm comparing their texture SIZE has to read them from one render or it is
# comparing two framings as much as two projections.
CSCN = {
    "version": 1,
    "models": [{"path": "layer_fixture.gltf"}],
    "materials": {
        "layered_surface": {
            "splat": SPLAT_NAME,
            "layerBlend": LAYER_BLEND_SHARPNESS,
            # Layer 1 takes the relief map; the rest share the flat one, which is
            # also what exercises mask_layer_for's dedup (one file, three uses,
            # one array layer).
            "layers": [
                {"albedo": name,
                 "surface": RELIEF_NAME if i == 1 else SURFACE_NAME,
                 "uvScale": UV_SCALE}
                for i, (name, _) in enumerate(LAYERS)
            ],
        },
    },
    # Head-on and untinted. The arms read the albedo view, which has no lighting
    # in it at all, but the golden is the lit frame and wants both plates evenly
    # exposed rather than one of them in shadow.
    "lights": [{"name": "LayerSun", "type": "directional",
                "direction": [-0.2, -0.6, -0.77], "color": [1.0, 1.0, 1.0],
                "intensity": 3.0, "cast_shadows": False}],
    "camera": {"eye": [0.0, 4.2, 9.0], "target": [0.0, 1.3, -1.0], "fov": 45},
    "post": {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False},
}


def main():
    _assert_fixture_still_tests_something()
    _write_maps()
    with open(os.path.join(HERE, "layer_fixture.gltf"), "w") as f:
        json.dump(GLTF, f, indent=1)
        f.write("\n")
    with open(os.path.join(HERE, "layer_fixture.cscn"), "w") as f:
        json.dump(CSCN, f, indent=1)
        f.write("\n")
    print(f"wrote layer_fixture.gltf, layer_fixture.cscn and {len(LAYERS) + 3} maps at {TEX}x{TEX}")


if __name__ == "__main__":
    main()
