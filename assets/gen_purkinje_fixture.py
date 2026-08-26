#!/usr/bin/env python3
"""Generate the Purkinje / scotopic-shift instrument (spec 11.83).

WHY lut_fixture COULD NOT BE REUSED, since it is the obvious candidate. That
chart is the right IDIOM -- emissive patches over black, so a patch's value is a
property of emissiveFactor alone with no N.L, no shadow and no IBL in the path --
and the wrong SHAPE. Its 18 colours all sit at one set of magnitudes, so it can
show how the blue shift varies with luminance (along its grey ramp) and cannot
show how the DRAIN does, which is the axis this feature turns on.

So: COLUMNS ARE A LUMINANCE LADDER, ROWS ARE CHROMATICITY HELD CONSTANT. Each
row is one hue at eight magnitudes in exact powers of two, straddling the
mesopic band. That is the one construction a GLOBAL implementation cannot pass:
a whole-frame weight drains every rung by the same amount, so the ladder comes
out flat with no interior crossover.

THE TRICK THAT MAKES IT READABLE. The scene authors post.exposure well above 1,
so the patches' ABSOLUTE radiances are scotopic (which is what the rod ramp
reads) while their DISPLAYED codes are mid-tone (which is what an 8-bit gate can
measure). Exposure and the rod weight are independent by the feature's own
contract -- the weight divides preExposure back out -- so the fixture uses one to
make the other legible. If that division is ever dropped the whole ladder shifts
by log2(EXPOSURE) and the purkinje-absolute arm reports it.

WHAT EACH ROW IS FOR, and none of them is decoration:

  0 neutral    r == g == b exactly. THE BLUE SHIFT'S ONLY CLEAN TEST: on an
               already-achromatic input the desaturating half is an exact
               identity in every weight, so a build that only desaturates moves
               this row by exactly 0 codes. It fails at the floor, not near it.
  1 red        The row rods see worst -- V'(610)/V(610) is about 1/32 -- so it
               takes the largest drain and ends up darkest.
  2 green      Mid.
  3 blue       The row a TINT-ONLY build pushes the WRONG WAY. Multiplying by
               the rod tint without the achromatic mix RAISES blue's saturation
               where it lowers red's, which is why the drain arm asserts a sign
               per row rather than a mean over the chart.
  4 warm mid   Interior of the colour cube. 11.58's lesson: a chart of saturated
               corners leaves the region where every real look works untested.
  5 rod hue    The rod tint's own chromaticity -- a near fixed point of the whole
               operator, so it must move LEAST at every rung. A build with the
               wrong tint HUE fails this and nothing else.

Regenerate: python3 assets/gen_purkinje_fixture.py
"""

import base64
import json
import os
import struct

# ---------------------------------------------------------------------------
# The ladder
# ---------------------------------------------------------------------------

# Eight rungs, exact powers of two. Emissive radiance BEFORE the scene exposure
# below -- so the absolute value the rod ramp sees is the number here, and the
# displayed code is that times EXPOSURE through the tonemap.
RUNGS = 8
LADDER_TOP = 0.25
LADDER = [LADDER_TOP * (0.5 ** i) for i in range(RUNGS)]  # log2 -2 .. -9

# Scene exposure. Lifts the displayed codes into the readable middle of the 8-bit
# range without touching the absolute radiance the weight is computed from.
EXPOSURE = 3.2

# The shipped ramp edges, restated so the generator can assert the ladder
# straddles them. NOT shared with the shader through a header: these are a
# duplicate, and the loop is closed by purkinje-config asserting the ENGINE's own
# defaults rather than by trusting this copy.
PK_LOCAL_LO, PK_LOCAL_HI = -8.0, -3.0

# Chromaticities, one per row, each held constant down its ladder. Normalised so
# every row's photopic luminance is equal at a given rung -- otherwise a row's
# drain would be confounded with it simply being darker.
LUMA_W = (0.2126, 0.7152, 0.0722)


def _unit_luma(rgb):
    """Scale a chromaticity to photopic luminance 1, so rows are comparable."""
    y = sum(c * w for c, w in zip(rgb, LUMA_W))
    return tuple(c / y for c in rgb)


ROW_NAMES = ["neutral", "red", "green", "blue", "warm", "rodhue"]
ROW_CHROMA = [
    _unit_luma((1.00, 1.00, 1.00)),
    _unit_luma((1.00, 0.16, 0.12)),
    _unit_luma((0.16, 1.00, 0.22)),
    _unit_luma((0.16, 0.24, 1.00)),
    _unit_luma((0.85, 0.55, 0.28)),
    # The rod tint's chromaticity, from gen_scotopic_weights.py's own vector.
    _unit_luma((0.83952, 1.00942, 1.37921)),
]
ROWS, COLS = len(ROW_CHROMA), RUNGS

# ---------------------------------------------------------------------------
# Asserts. 11.58's most expensive lesson was a generator that checked three
# anti-degeneracy properties on one table and none on another, so making its
# curve the identity would have left every arm green.
# ---------------------------------------------------------------------------

assert LADDER == sorted(LADDER, reverse=True), "the ladder must be monotone"
_span = __import__("math").log2(LADDER[0] / LADDER[-1])
assert _span >= 5.5, f"the ladder spans only {_span:.2f} stops"

# It must STRADDLE the shipped local ramp, or every rung sits on one side of the
# transition and the ladder arm reads a constant.
_lo = __import__("math").log2(LADDER[-1])
_hi = __import__("math").log2(LADDER[0])
assert _lo < PK_LOCAL_LO + 1.0, f"dimmest rung log2 {_lo:.2f} is not below the ramp floor"
assert _hi > PK_LOCAL_HI - 1.0, f"brightest rung log2 {_hi:.2f} is not above the ramp ceiling"

# Row 0 must be EXACTLY achromatic, or purkinje-blue's baseline is not zero and
# its floor test becomes a tolerance.
assert ROW_CHROMA[0][0] == ROW_CHROMA[0][1] == ROW_CHROMA[0][2], "row 0 must be exactly grey"

# Every chromatic row must be genuinely saturated at every rung, or the drain
# arm measures a property of the fixture.
for name, c in zip(ROW_NAMES[1:], ROW_CHROMA[1:]):
    sat = (max(c) - min(c)) / max(c)
    assert sat > 0.25, f"row {name} is only {sat:.3f} saturated"

# No two chromatic rows may be channel PERMUTATIONS of each other: a build that
# swapped the rod tint's R and B would answer identically on all of them and the
# whole group would be blind to it.
for i in range(1, ROWS):
    for j in range(i + 1, ROWS):
        a = tuple(round(v, 4) for v in ROW_CHROMA[i])
        b = tuple(round(v, 4) for v in ROW_CHROMA[j])
        assert sorted(a) != sorted(b), f"rows {ROW_NAMES[i]} and {ROW_NAMES[j]} are permutations"

# The brightest patch, once exposed, must stay under the working-space ceiling --
# and so must purkinje-absolute's second leg, which renders the same file at 4x.
# Otherwise the sanitize clamp binds on one leg only and its exact-0 claim dies
# for a reason that has nothing to do with the feature.
WS_SCENE_MAX = 60000.0
assert LADDER[0] * EXPOSURE * 4.0 < WS_SCENE_MAX * 0.5, "the 4x leg would reach the WS ceiling"

# ---------------------------------------------------------------------------
# Geometry: one quad, reused. Patches differ ONLY in material and translation.
# ---------------------------------------------------------------------------

PATCH = 0.44   # half-width
PITCH = 1.06   # centre-to-centre
# Far enough back that the chart is a MINORITY of the frame, which is a
# requirement rather than framing taste -- see the coverage assert below.
CAM_Z = 19.4
FOV_DEG = 40.0
ASPECT = 4.0 / 3.0

# The pitch must clear the default bloom radius, or a bright rung's halo raises
# its dim neighbour's absolute radiance and the ladder arm reads the bloom
# instead of the ramp. Every arm also passes --no-bloom; this is the belt.
assert PITCH - 2 * PATCH > 0.12, "patches are too close for a bloom-free read"

positions = [(-PATCH, -PATCH, 0.0), (PATCH, -PATCH, 0.0), (PATCH, PATCH, 0.0),
             (-PATCH, PATCH, 0.0)]
normals = [(0.0, 0.0, 1.0)] * 4
indices = [0, 1, 2, 0, 2, 3]

# The backdrop must fill the frame, not merely sit behind the chart: the black
# around the patches is what keeps the meter's kept population dark, and a gap
# would show the clear colour instead.
BACK_W = 13.0
BACK_H = 10.0

# THE CHART MUST BE A MINORITY OF THE FRAME, and this is the least obvious
# constraint here. The meter keeps the brightest 30% of the POPULATION, so if the
# patches filled that tail the frame's metered mean would be the chart's own
# bright rungs -- around log2 -3, which is past PK_GLOBAL_HI, so the global gate
# would close and the fixture would render its own subject unshifted. Kept
# black-dominated, the mean sits near the measure shader's -26.57 floor and the
# global gate saturates at exactly 1, which is what lets every arm read the LOCAL
# ramp cleanly.
_half_h = CAM_Z * __import__("math").tan(__import__("math").radians(FOV_DEG) * 0.5)
_frame = (2 * _half_h * ASPECT) * (2 * _half_h)
_coverage = (ROWS * COLS * (2 * PATCH) ** 2) / _frame
assert _coverage < 0.20, f"patches cover {_coverage:.3f} of frame; the meter would read the chart"
assert BACK_W >= _half_h * ASPECT and BACK_H >= _half_h, "the backdrop does not fill the frame"

back_positions = [(-BACK_W, -BACK_H, 0.0), (BACK_W, -BACK_H, 0.0),
                  (BACK_W, BACK_H, 0.0), (-BACK_W, BACK_H, 0.0)]

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
back_bytes = b"".join(struct.pack("<3f", *p) for p in back_positions)
idx_bytes = b"".join(struct.pack("<H", i) for i in indices)
_chunks = [(pos_bytes, 34962), (nrm_bytes, 34962), (back_bytes, 34962), (idx_bytes, 34963)]
buffer_bytes = b"".join(c for c, _ in _chunks)


def _views(chunks):
    """One bufferView per chunk, offsets accumulated rather than re-summed: a
    wrong offset is not a build error, it is geometry that renders subtly
    wrong."""
    views, offset = [], 0
    for data, target in chunks:
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data),
                      "target": target})
        offset += len(data)
    return views


mn = [min(p[i] for p in positions) for i in range(3)]
mx = [max(p[i] for p in positions) for i in range(3)]
back_mn = [min(p[i] for p in back_positions) for i in range(3)]
back_mx = [max(p[i] for p in back_positions) for i in range(3)]


def patch_centre(row, col):
    """Column 0 is the LEFT of frame (the BRIGHTEST rung) and row 0 the TOP."""
    x = (col - (COLS - 1) * 0.5) * PITCH
    y = ((ROWS - 1) * 0.5 - row) * PITCH
    return [x, y, 0.0]


nodes = [{"name": "backdrop", "mesh": ROWS * COLS, "translation": [0.0, 0.0, -0.8]}]
meshes = []
materials = []
for r in range(ROWS):
    for c in range(COLS):
        idx = r * COLS + c
        rgb = tuple(ch * LADDER[c] for ch in ROW_CHROMA[r])
        nodes.append({"name": f"pk_{ROW_NAMES[r]}_{c}", "mesh": idx,
                      "translation": patch_centre(r, c)})
        meshes.append({"name": f"pk_{ROW_NAMES[r]}_{c}",
                       "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1},
                                       "indices": 3, "material": idx}]})
        # Black base with an emissive face: the unlit-flat-colour idiom, which is
        # what makes a patch's absolute radiance a property of one authored
        # number rather than of the lighting.
        materials.append({
            "name": f"pk_{ROW_NAMES[r]}_{c}",
            "pbrMetallicRoughness": {"baseColorFactor": [0.0, 0.0, 0.0, 1.0],
                                     "metallicFactor": 0.0, "roughnessFactor": 1.0},
            "emissiveFactor": [round(v, 6) for v in rgb],
        })

meshes.append({"name": "backdrop",
               "primitives": [{"attributes": {"POSITION": 2, "NORMAL": 1}, "indices": 3,
                               "material": ROWS * COLS}]})
materials.append({"name": "pk_backdrop",
                  "pbrMetallicRoughness": {"baseColorFactor": [0.0, 0.0, 0.0, 1.0],
                                           "metallicFactor": 0.0, "roughnessFactor": 1.0}})

gltf = {
    "asset": {"version": "2.0", "generator": "gen_purkinje_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": list(range(len(nodes)))}],
    "nodes": nodes,
    "meshes": meshes,
    "materials": materials,
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": mn, "max": mx},
        {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": back_mn, "max": back_mx},
        {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"},
    ],
    "bufferViews": _views(_chunks),
    "buffers": [
        {"uri": "data:application/octet-stream;base64," +
                base64.b64encode(buffer_bytes).decode("ascii"),
         "byteLength": len(buffer_bytes)},
    ],
}

cscn = {
    "version": 1,
    "models": [{"path": "purkinje_fixture.gltf"}],
    # One dim white directional, for gen_lut_fixture's reason: a .cscn with no
    # lights gets the render app's automatic three-point rig, which is three
    # lights this fixture did not choose. White and dim, so whatever it
    # contributes is achromatic and equal across the chart.
    "lights": [{"name": "PkFill", "type": "directional", "direction": [0.0, 0.0, -1.0],
                "color": [1.0, 1.0, 1.0], "intensity": 0.02, "cast_shadows": False}],
    "camera": {"eye": [0.0, 0.0, CAM_Z], "target": [0.0, 0.0, 0.0], "fov": FOV_DEG},
    # PINNED, and high. Pinned because an adapting meter would move the whole
    # ladder between runs; high because it is what lifts scotopic radiances into
    # readable 8-bit codes without touching the absolute values the rod weight is
    # computed from. "neutral" for 11.58's reason: the least operator in the path.
    "post": {"tonemap": "neutral", "exposure": EXPOSURE},
}

here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(here, "purkinje_fixture.gltf"), "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
with open(os.path.join(here, "purkinje_fixture.cscn"), "w") as f:
    json.dump(cscn, f, indent=1)
    f.write("\n")

print(f"wrote purkinje_fixture.gltf + .cscn ({ROWS} rows x {COLS} rungs, "
      f"{_span:.1f} stops, exposure {EXPOSURE}, coverage {_coverage:.3f})")
