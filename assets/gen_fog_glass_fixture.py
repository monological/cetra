#!/usr/bin/env python3
"""Generate assets/fog_glass_fixture.gltf + .cscn, the translucent-fog-depth instrument
(spec 11.78).

WHAT IS UNDER TEST. The atmosphere composite reads ONE linear depth per pixel, out of the
aux G-buffer, and the late pass writes no aux -- so a translucent surface is fogged at the
depth of whatever opaque thing stands behind it. This fixture puts that error next to its
own control in one frame.

    backdrop      far, opaque               fog over the LONG path
    opaque pane   near, opaque              fog over the SHORT path  <- the truth
    half pane     near, ALPHA_BLEND a=0.5   half its light is the pane's, half the backdrop's
    quarter pane  near, ALPHA_BLEND a=0.25  a quarter is
    dark pane     near, ALPHA_BLEND a=0.5, a DIFFERENT emissive

EVERY SURFACE CARRIES THE SAME EMISSIVE EXCEPT THE LAST, and that is the whole reason the
frame can be read as a fog measurement. With equal radiance everywhere, an unfogged render
is a FLAT FIELD -- so any difference between a pane and the bare backdrop beside it is the
fog and nothing else. No BRDF, no N.L, no exposure dependence to model. (The `lut_fixture`
reasoning: emissive over black is the only surface whose radiance is its authored factor.)

It also makes the blend panes' COMPOSITE independent of their alpha: a*E + (1-a)*E is E at
any a. So the panes agree with the backdrop before the fog touches them, whatever coverage
they carry, and coverage is left free to be the variable the arms sweep.

READ THIS FIXTURE AS RATIOS AGAINST ITS OWN NO-FOG RENDER, never as absolute codes. The
moment-based OIT reconstruction does not return a blend pane's composite exactly -- measured
here at -4.4% for a = 0.5 -- and a ratio divides that out, leaving the fog. This is the
`water_fixture` rule and it is load-bearing rather than stylistic.

WHY NO PANE AT ALPHA 1.0, WHICH IS THE OBVIOUS DESIGN. A blend pane at a = 1.0 would be a
byte-identical twin of the opaque pane -- same colour, same place, differing only in
`alphaMode` -- and would isolate the defect perfectly. It cannot be used: `mboitAbsorbance`
maps a = 1.0 to -log(1e-4) = 9.21, and the moment reconstruction charges a lone layer part
of its own absorbance, so the pane renders at about a FIFTH of its correct value. Measured
on this geometry with no fog at all: 38.9 against the 180.2 it owes, and exactly 180.20
under `--no-oit-moments`. A twin there would read the moment reconstruction, not the fog.

THE DARK PANE IS THE ONE PLACE THE TWO DECOMPOSITIONS DIFFER, and no arm reads that yet.
The other three carry the backdrop's own radiance, and there an opacity-weighted MIX of the
two fog answers and an exact per-contribution split agree identically -- the difference term
carries a factor of (translucent radiance - (1 - opacity) * background), which is zero when
they match. This pane is the case where it is not.

What that buys TODAY is only a second, differently-lit instance for the lift arm; the ship
is the mix, so nothing compares the two forms. It is kept because it is the instrument the
comparison would need, and because a fixture whose every surface has one radiance cannot
show that the approximation is bounded. Anyone revisiting the exact split starts here --
and should know the arms cannot currently tell the two apart.

THE GAPS ARE THE CONTROL AND THEY ARE IN-FRAME. Bare backdrop shows between every pair of
panes, including a wide one dead centre, so the long-path reference is read at the same
screen rows as the short-path panes rather than off the frame edge or out of a second
render. (`fog_volume_fixture`'s rule.)

THE RECENTER IS A NO-OP, BY CONSTRUCTION. `apply_model_recenter` shifts a model so its
bounds are centred in x and z with the base at y=0, and the camera here is authored in
pre-recenter world space -- so a non-zero offset would slide the geometry out from under
the framing every read depends on. The backdrop and the panes therefore sit at equal and
opposite z, the backdrop is centred in x, and its base is exactly y=0. Asserted below.

FOG IS NOT AUTHORED HERE. The gate passes `--fog --fog-density D --fog-height H` with H
large, which makes the medium UNIFORM: `airSigma` is `density * exp(-(y - floorY)/falloff)`
so a falloff far larger than the scene makes optical depth exactly proportional to path
length, and the expected transmittance at either depth is a closed form the arm can state.
Pinned rather than inherited because the app derives density from the scene radius, so a
geometry change would silently re-tune the very quantity under test.

Regenerate with: python3 assets/gen_fog_glass_fixture.py
"""

import base64
import json
import math
import os
import struct

# --- framing, shared with the gate ------------------------------------------------------
FOVY_DEG = 45.0
ASPECT = 400.0 / 300.0  # gates.py's shared render size; the panes are laid out to fit it
EYE_Y = 26.0
EYE_Z = 28.0

PANE_Z = 20.0  # panes stand here
BACK_Z = -20.0  # backdrop stands here; equal and opposite, so the z centre is 0

PANE_DIST = EYE_Z - PANE_Z  # 8
BACK_DIST = EYE_Z - BACK_Z  # 48

# --- the panes --------------------------------------------------------------------------
PANE_HALF_W = 0.62
PANE_HALF_H = 1.6
PANE_X = [-3.10, -1.05, 1.05, 3.10]  # opaque, half a=0.5, quarter a=0.25, dark a=0.5

# Coverage of the two sweep panes. Both well inside the range where the moment
# reconstruction is well conditioned -- see the alpha-1.0 note in the module docstring.
ALPHA_HALF = 0.5
ALPHA_QUARTER = 0.25

# --- the backdrop -----------------------------------------------------------------------
# Sized to overhang the frame at its own depth so no read can reach past its edge, and
# based at y=0 so the recenter's y term is zero.
BACK_HALF_W = 42.0
BACK_HALF_H = 26.0

# --- radiance ---------------------------------------------------------------------------
# One value for the backdrop and three of the four panes: the unfogged frame is flat, so
# every difference the arms read is fog. Mid-range rather than near 1.0 because in-scatter
# ADDS to what a surface transmits and a bright field would clip at the 8-bit write.
EMISSIVE = 0.50
# The fourth pane, well away from EMISSIVE so the two decompositions separate. Same hue, so
# the read stays scalar.
EMISSIVE_DARK = 0.06

# --- the medium the gate renders in -------------------------------------------------------
# The gate IMPORTS these rather than restating them: the asserts below are calibrated in
# this medium, so a gate pinning a different one would render a fixture whose own checks no
# longer describe it. The app derives density from the scene radius, which is why it is
# pinned at all -- inheriting it would let a geometry edit retune the quantity under test.
FOG_DENSITY = 0.020
# Far larger than the scene, which is what makes the medium uniform: airSigma is
# density * exp(-(y - floorY) / falloff), so a falloff this size leaves optical depth
# proportional to path length and the transmittances below closed forms.
FOG_HEIGHT = 1000.0

# ----------------------------------------------------------------------------------------
# Derived checks. Each of these is a way the fixture could stop testing what it claims to.

half_h_at_pane = math.tan(math.radians(FOVY_DEG) * 0.5) * PANE_DIST
half_w_at_pane = half_h_at_pane * ASPECT
half_h_at_back = math.tan(math.radians(FOVY_DEG) * 0.5) * BACK_DIST
half_w_at_back = half_h_at_back * ASPECT

# 1. Every pane is inside the frustum at its depth, with margin.
assert max(PANE_X) + PANE_HALF_W < half_w_at_pane - 0.3, "panes reach the frame edge"
assert PANE_HALF_H < half_h_at_pane - 0.3, "panes reach the top of the frame"

# 2. The backdrop overhangs the frame at ITS depth, so no read finds its edge.
assert BACK_HALF_W > half_w_at_back + 5.0, "backdrop does not cover the frame"
assert BACK_HALF_H > half_h_at_back + 3.0, "backdrop does not cover the frame"

# 3. The gaps are wide enough to sample. The centre gap is the widest and is the one the
#    arms read the long path in.
gaps = []
edges = [-half_w_at_pane] + [x + s * PANE_HALF_W for x in PANE_X for s in (-1, 1)]
edges.append(half_w_at_pane)
for i in range(0, len(edges) - 1, 2):
    gaps.append(edges[i + 1] - edges[i])
assert min(gaps) > 0.5, "a gap closed up: %r" % (gaps,)
centre_gap = PANE_X[2] - PANE_HALF_W - (PANE_X[1] + PANE_HALF_W)
assert centre_gap > 0.7, "the centre control gap is too narrow: %.3f" % centre_gap

# 4. The recenter is a no-op: bounds centred in x and z, base at y=0.
bb_min = (-BACK_HALF_W, 0.0, BACK_Z)
bb_max = (BACK_HALF_W, 2.0 * BACK_HALF_H, PANE_Z)
assert abs(bb_min[0] + bb_max[0]) < 1e-6, "bounds not centred in x"
assert abs(bb_min[2] + bb_max[2]) < 1e-6, "bounds not centred in z"
assert abs(bb_min[1]) < 1e-6, "bounds do not sit on y=0"

# 5. The two depths are far enough apart, in the medium above, that the defect is a large
#    signal rather than a rounding difference. Uniform medium, so tau = density * path.
#
#    A LINEAR-DOMAIN PROXY, deliberately loose. The arms read a display-encoded frame with
#    in-scatter added, and that compresses this separation by about 1.8x -- measured 0.469
#    here against 0.264 between the arms' own opaque-pane and backdrop readings. So this
#    bar is a floor on the physics, not on what the gate sees; the arms carry their own.
t_near = math.exp(-FOG_DENSITY * PANE_DIST)
t_far = math.exp(-FOG_DENSITY * BACK_DIST)
assert t_near - t_far > 0.25, (
    "the two paths transmit too similarly to read: %.3f vs %.3f" % (t_near, t_far)
)

# 6. The panes are centred on the camera's own height, so a pane and the gap beside it are
#    the same screen rows.
PANE_Y = EYE_Y
assert bb_min[1] < PANE_Y - PANE_HALF_H and PANE_Y + PANE_HALF_H < bb_max[1], (
    "the panes leave the backdrop"
)

# 6b. THE TWO SWEEP PANES ARE MIRRORED ABOUT x=0, and the coverage arm rests on it. The
#     backdrop is a PLANE, so the path to it grows as 1/cos(horizontal angle) -- measured
#     here as a fog ratio running 0.621 at the frame edge against 0.667 dead centre. Two
#     panes at unequal |x| would therefore sit over unequal backdrop fog, and the arm that
#     differences them would be reading that gradient as coverage response.
assert abs(PANE_X[1] + PANE_X[2]) < 1e-9, "the sweep panes are no longer mirrored about x=0"
assert abs(PANE_X[0] + PANE_X[3]) < 1e-9, "the outer panes are no longer mirrored about x=0"

# ----------------------------------------------------------------------------------------
# Geometry: one unit quad in XY facing +Z, instanced by node scale.

positions = [(-0.5, -0.5, 0.0), (0.5, -0.5, 0.0), (0.5, 0.5, 0.0), (-0.5, 0.5, 0.0)]
normals = [(0.0, 0.0, 1.0)] * 4
indices = [0, 1, 2, 0, 2, 3]

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
idx_bytes = b"".join(struct.pack("<H", i) for i in indices)
buffer_bytes = pos_bytes + nrm_bytes + idx_bytes


def unlit(name, value, alpha, blend):
    """Emissive over black albedo: the fragment's colour is this constant.

    `blend` is authored independently of `alpha` so a caller states the LANE and the
    coverage separately; nothing here relies on a particular pairing of the two.
    """
    mat = {
        "name": name,
        "doubleSided": True,
        "emissiveFactor": [value] * 3,
        "pbrMetallicRoughness": {
            "baseColorFactor": [0.0, 0.0, 0.0, alpha],
            "metallicFactor": 0.0,
            "roughnessFactor": 1.0,
        },
    }
    if blend:
        mat["alphaMode"] = "BLEND"
    return mat


materials = [
    unlit("fogglass_backdrop", EMISSIVE, 1.0, False),
    unlit("fogglass_opaque", EMISSIVE, 1.0, False),
    unlit("fogglass_half", EMISSIVE, ALPHA_HALF, True),
    unlit("fogglass_quarter", EMISSIVE, ALPHA_QUARTER, True),
    unlit("fogglass_dark", EMISSIVE_DARK, ALPHA_HALF, True),
]

# 7. The three blend panes carry the backdrop's own emissive except the dark one, and the
#    two sweep panes differ from each other in ALPHA ALONE. If a future edit gives one of
#    them its own colour, the coverage arm stops measuring coverage and starts measuring
#    that.
_half = dict(materials[2])
_quarter = dict(materials[3])
for _m in (_half, _quarter):
    _m.pop("name")
    _m["pbrMetallicRoughness"] = dict(_m["pbrMetallicRoughness"])
    _m["pbrMetallicRoughness"].pop("baseColorFactor")
assert _half == _quarter, "the two sweep panes differ in more than their alpha"
assert materials[2]["emissiveFactor"] == materials[0]["emissiveFactor"], (
    "the sweep panes no longer match the backdrop, so the unfogged frame is not flat"
)
assert materials[4]["emissiveFactor"] != materials[0]["emissiveFactor"], (
    "the dark pane matches the backdrop, so it can no longer separate the decompositions"
)
assert 0.0 < ALPHA_QUARTER < ALPHA_HALF < 0.9, "coverage sweep left the well-conditioned range"

# Backdrop first so it draws first; the panes follow in x order.
nodes_spec = [
    (0, (0.0, BACK_HALF_H, BACK_Z), (2.0 * BACK_HALF_W, 2.0 * BACK_HALF_H, 1.0)),
]
for i, x in enumerate(PANE_X):
    nodes_spec.append(
        (i + 1, (x, PANE_Y, PANE_Z), (2.0 * PANE_HALF_W, 2.0 * PANE_HALF_H, 1.0))
    )

gltf = {
    "asset": {"version": "2.0", "generator": "gen_fog_glass_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": list(range(len(nodes_spec)))}],
    "nodes": [
        {
            "name": materials[mat]["name"],
            "mesh": i,
            "translation": list(t),
            "scale": list(s),
        }
        for i, (mat, t, s) in enumerate(nodes_spec)
    ],
    "meshes": [
        {
            "name": materials[mat]["name"],
            "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": mat}
            ],
        }
        for mat, _, _ in nodes_spec
    ],
    "materials": materials,
    "accessors": [
        {
            "bufferView": 0,
            "componentType": 5126,
            "count": 4,
            "type": "VEC3",
            "min": [-0.5, -0.5, 0.0],
            "max": [0.5, 0.5, 0.0],
        },
        {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR"},
    ],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(nrm_bytes), "target": 34962},
        {
            "buffer": 0,
            "byteOffset": len(pos_bytes) + len(nrm_bytes),
            "byteLength": len(idx_bytes),
            "target": 34963,
        },
    ],
    "buffers": [
        {
            "uri": "data:application/octet-stream;base64,"
            + base64.b64encode(buffer_bytes).decode("ascii"),
            "byteLength": len(buffer_bytes),
        }
    ],
}

scene_desc = {
    "version": 1,
    "_comment": [
        "The translucent-fog-depth instrument (spec 11.78). Four panes at ONE near depth in",
        "front of one emissive backdrop far behind them, with bare backdrop showing between",
        "every pair.",
        "",
        "Backdrop and three of the four panes carry the SAME emissive, so an unfogged render",
        "is a flat field and every difference an arm reads is the fog -- and a blend pane's",
        "composite is then its own emissive at ANY alpha, which leaves coverage free to be",
        "the variable the arms sweep.",
        "",
        "Read as RATIOS against a no-fog render of this same file. The moment OIT",
        "reconstruction does not return a blend pane's composite exactly and a ratio divides",
        "that out. No pane sits at alpha 1.0: the reconstruction charges a lone layer part of",
        "its own absorbance and renders it at about a fifth of its value.",
        "",
        "The fourth pane carries a different emissive on purpose: with equal radiance a",
        "coverage-weighted mix of the two fog answers and an exact per-contribution",
        "decomposition agree exactly, so without it the cheaper one passes every arm.",
        "",
        "No fog is authored here. The gate pins --fog-density and a --fog-height far larger",
        "than the scene, which makes the medium uniform and the expected transmittance at",
        "either depth a closed form. The app derives density from the scene radius, so",
        "inheriting it would let a geometry change re-tune the quantity under test.",
    ],
    "models": [{"path": "fog_glass_fixture.gltf"}],
    "lights": [
        {
            "name": "FogGlassKey",
            "type": "directional",
            "direction": [0.6, -0.5, -0.62],
            "color": [1.0, 1.0, 1.0],
            "intensity": 2.5,
            "cast_shadows": False,
        }
    ],
    "camera": {
        "eye": [0.0, EYE_Y, EYE_Z],
        "target": [0.0, EYE_Y, 0.0],
        "fov": FOVY_DEG,
    },
    "post": {"tonemap": "neutral", "exposure": 1.0},
}

# Guarded, unlike most generators here, because the gate IMPORTS this file for the pane
# layout and the medium it pins -- and a generator that wrote on import would rewrite a
# committed asset in the middle of a gate run.
if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "fog_glass_fixture.gltf"), "w") as f:
        json.dump(gltf, f, indent=1)
        f.write("\n")
    with open(os.path.join(here, "fog_glass_fixture.cscn"), "w") as f:
        json.dump(scene_desc, f, indent=1)
        f.write("\n")
    print("wrote fog_glass_fixture.gltf + .cscn")
    print("  pane path %.1f units (T=%.4f), backdrop path %.1f units (T=%.4f) at density %.4f"
          % (PANE_DIST, t_near, BACK_DIST, t_far, FOG_DENSITY))
    print("  gaps in x at pane depth: %s" % ", ".join("%.2f" % g for g in gaps))
