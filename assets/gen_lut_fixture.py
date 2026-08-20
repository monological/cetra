#!/usr/bin/env python3
"""Generate the 3D-LUT colour-grading instrument (spec 11.58): one chart and four .cube files.

A LUT is a table of answers rather than a formula, so the only way to test one
is to know what the answer should be. Everything here is PAINTED ground truth:
each .cube is written from a closed form the gate can evaluate independently,
and the chart exists so those forms are exercised across the table's DOMAIN
rather than in one corner of it.

The chart is 24 emissive patches -- a grey ramp along the top row, then 18
colours spread over the RGB cube. Emissive rather than lit, and the base colour
is black, because a LUT is indexed by the pixel's own value: what this fixture
has to control is WHERE IN THE TABLE each patch lands, and an emissive patch
maps to a display value through exposure and the tonemap alone, with no N.L, no
shadow and no IBL in the path. Lit patches would work and would land wherever
the lighting put them.

The grey ramp is not decoration. It is the whole falsification of tetrahedral
interpolation: r == g == b in must give r == g == b out, and trilinear does not
guarantee that for a table with cross-channel terms.

FOUR TABLES, each answering something no other does:

  lut_identity  out = in.
                Only this one can catch the half-texel inset. Sampling a 3D LUT
                with raw [0,1] coordinates addresses texel EDGES, not centres,
                so an identity table stops being an identity -- by half a cell,
                everywhere, which reads as a slight wash rather than as an error.

  lut_swap      (r,g,b) -> (b,g,r).
                Only this one can catch the data block being read in the wrong
                axis order. .cube stores RED FASTEST; read it blue-fastest and
                you get the transpose, which is a plausible-looking image and a
                completely wrong table. Deliberately LINEAR, so trilinear and
                tetrahedral agree on it exactly -- that isolates "is the table
                right" from "is the interpolation right", which are otherwise
                two unknowns in one measurement.

  lut_steep     a hard S-curve plus a channel rotation, at 33 -- the size real
                files ship at. Curvature is what makes an interpolator's job
                hard, so this is the table lut-agree runs on: the shader is
                checked against an independent reading of the WORST case rather
                than one every interpolant reproduces exactly.
                It is NOT the table that separates the two interpolants. It was
                described that way for a spec cycle while the arm that separates
                them used lut_neutral, and the spec's own measurement says why
                -- 0.076 of a code here against 7.841 there.

  lut_neutral   identity on the grey diagonal, cross-channel off it, and COARSE.
                The arm that justifies tetrahedral existing at all, and the only
                table on which the two interpolants visibly disagree.

WHY lut_neutral IS BUILT THE WAY IT IS, because the obvious construction does
not work. The intuitive "steep curve applied identically to all three channels"
is SEPARABLE, and a separable table preserves greys under trilinear too: out.r
depends only on in.r, the g and b interpolation weights sum to 1 and factor out,
so every channel gets the same 1D lerp and a grey stays grey. Measuring that
would have reported the two interpolants as identical and concluded tetrahedral
was pointless.

What actually separates them is a table that is NON-separable while still being
exact on the diagonal. `d` below is zero iff r == g == b, so the diagonal is
untouched at every lattice point -- but off it the channels mix, and trilinear
at a grey sample blends in the six off-diagonal corners of the cell, which carry
different lifts per channel and tint the result. Tetrahedral cannot: all six
tetrahedra of the standard decomposition SHARE the main diagonal as an edge, so
a sample on it puts all its weight on the two grey corners and none on the rest.

And LUT_N_NEUTRAL is deliberately COARSE. The tint scales with the cell size, so
at Resolve's 33 it sits far under one 8-bit code and the arm would measure
nothing. A fixture's job is to make the property legible -- the same reason the
cloud arms pin --cloud-coverage 0.10 rather than the default.

Regenerate with: python3 assets/gen_lut_fixture.py
"""

import base64
import json
import os
import struct

# ---------------------------------------------------------------------------
# The tables
# ---------------------------------------------------------------------------

# 33 is Resolve's export default and what most vendor LUTs carry. The roadmap
# row said 32, which is not a size the format produces; the loader reads the
# size from the file and these fixtures are what prove it does.
LUT_N = 33
# 17 is the other size small look LUTs ship at, and a 33-cubed table of six
# decimals is a megabyte of committed text -- so the two tables whose property
# is size-independent take this one.
LUT_N_SMALL = 17
# Coarse on purpose -- see the module docstring. 9 puts the cell at 1/8, where
# the neutral-axis tint is worth several 8-bit codes instead of a fraction of one.
LUT_N_NEUTRAL = 9
# Scales the cross-channel term. Fixed by measurement in phase 0 and bounded at
# the bottom of this file against the tint the arm actually reads -- the first
# version claimed that and asserted only that the term was NON-ZERO, which left
# a 20x window (amp 0.07 to 1.5) where the generator was happy and lut-neutral
# was red.
NEUTRAL_AMP = 4.0


def _clamp01(v):
    return 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)


def lut_identity(r, g, b):
    return (r, g, b)


def lut_swap(r, g, b):
    """Channel exchange. Linear, so it is exact under any interpolation."""
    return (b, g, r)


def lut_steep(r, g, b):
    """A hard S-curve per channel plus a rotation, so the table is both curved
    and non-separable -- curvature is what the two interpolants disagree about."""
    def s_curve(v):
        # smoothstep applied twice: steep enough that a cell's chord departs
        # visibly from the surface it is approximating.
        t = v * v * (3.0 - 2.0 * v)
        return t * t * (3.0 - 2.0 * t)
    rr = s_curve(_clamp01(0.70 * r + 0.30 * g))
    gg = s_curve(_clamp01(0.70 * g + 0.30 * b))
    bb = s_curve(_clamp01(0.70 * b + 0.30 * r))
    return (rr, gg, bb)


def lut_neutral(r, g, b):
    """Exactly identity where r == g == b; lifts RED ONLY everywhere else.

    `(g - b)**2` is zero iff those two channels are equal, so every lattice point
    ON the grey diagonal maps to itself and the table makes no claim there for
    the interpolation to get right -- which is what makes a tint measured on
    greys attributable to the interpolant and to nothing else.

    ONE CHANNEL, and that asymmetry is the whole construction. The symmetric
    form -- lift each channel by the same function of all three differences --
    was written first and MEASURED ZERO TINT under both interpolants. At a grey
    sample the trilinear weights are themselves symmetric under permuting the
    axes, so corner (dr,dg,db) and corner (dg,dr,db) carry equal weight and
    equal lift while contributing opposite channel differences, and the eight
    terms cancel in pairs exactly. A table symmetric in its channels cannot
    show a channel-asymmetric artifact, which is obvious afterwards and was not
    before.

    So this is the second trap in a row on the same fixture: a SEPARABLE table
    preserves greys because the weights factor out, and a SYMMETRIC one
    preserves them because the weights cancel. The table has to be neither.
    """
    return (_clamp01(r + NEUTRAL_AMP * ((g - b) ** 2) * (1.0 - r)), g, b)


# FOUR TABLES, THREE DIFFERENT SIZES, and the sizes are chosen rather than
# uniform. A table's size is the one header field the loader must actually act
# on, so a corpus that is all 33 would let a hardcoded 33 pass everything -- and
# the inset arithmetic divides by it, which is exactly the sort of constant that
# gets inlined. 17 also makes the inset error LARGER (it is half a texel, so it
# scales as 1/N: 7.5 codes at 17 against 3.9 at 33), which is the arm that has
# to see it. Steep stays at 33 because its job is the opposite -- to report what
# the interpolants do at the size real files actually ship at.
TABLES = [
    ("lut_identity.cube", "cetra identity", LUT_N_SMALL, lut_identity),
    ("lut_swap.cube", "cetra channel swap", LUT_N_SMALL, lut_swap),
    ("lut_steep.cube", "cetra steep s-curve", LUT_N, lut_steep),
    ("lut_neutral.cube", "cetra neutral-axis probe", LUT_N_NEUTRAL, lut_neutral),
]


def write_cube(path, title, n, fn):
    """Adobe .cube, RED VARYING FASTEST.

    The loop order IS the format: b outermost, r innermost. Writing it the other
    way round produces a syntactically perfect file that transposes the table,
    which is the failure lut_swap exists to catch on the reading side and this
    comment exists to prevent on the writing side.
    """
    lines = [f"TITLE \"{title}\"", f"LUT_3D_SIZE {n}", "DOMAIN_MIN 0.0 0.0 0.0",
             "DOMAIN_MAX 1.0 1.0 1.0", ""]
    d = n - 1
    for ib in range(n):
        for ig in range(n):
            for ir in range(n):
                out = fn(ir / d, ig / d, ib / d)
                lines.append(" ".join(f"{v:.6f}" for v in out))
    with open(path, "w") as f:
        f.write("\n".join(lines))
        f.write("\n")
    return len(lines)


# ---------------------------------------------------------------------------
# The chart
# ---------------------------------------------------------------------------

COLS, ROWS = 6, 4
PATCH = 0.42        # half-width of a patch quad
PITCH = 1.02        # centre-to-centre, so there is bare backdrop between patches

# Row 0 is the grey ramp. The values are EMISSIVE radiance, not display values --
# they land wherever the tonemap puts them, and the gate reads that rather than
# predicting it. Spread from well under to well over mid-grey so the ramp covers
# the diagonal rather than clustering.
GREYS = [0.04, 0.12, 0.28, 0.50, 0.78, 1.15]

# 18 colours over the cube: saturated primaries and secondaries, then
# desaturated and dark variants, so the table is sampled off its edges as well
# as on them. A chart of saturated corners only would leave the interior of the
# table -- where every real grade does its work -- untested.
COLOURS = [
    (0.90, 0.10, 0.10), (0.10, 0.85, 0.15), (0.12, 0.20, 0.95),
    (0.90, 0.80, 0.10), (0.85, 0.15, 0.80), (0.10, 0.80, 0.85),
    (0.62, 0.30, 0.18), (0.28, 0.55, 0.24), (0.22, 0.30, 0.62),
    (0.70, 0.55, 0.38), (0.55, 0.32, 0.52), (0.34, 0.58, 0.60),
    (0.30, 0.08, 0.06), (0.06, 0.26, 0.09), (0.05, 0.09, 0.32),
    (0.46, 0.44, 0.20), (0.40, 0.18, 0.36), (0.16, 0.38, 0.42),
]

patches = [(g, g, g) for g in GREYS] + COLOURS
assert len(patches) == COLS * ROWS, f"chart is {COLS}x{ROWS}, got {len(patches)} patches"

# Every patch must be distinguishable from every other one, or two of them
# compare equal and the arm cannot tell which it read. Checked here rather than
# trusted, because the lists above are hand-authored and easy to edit into a
# collision.
for i in range(len(patches)):
    for j in range(i + 1, len(patches)):
        sep = max(abs(a - b) for a, b in zip(patches[i], patches[j]))
        assert sep > 0.03, f"patches {i} and {j} differ by only {sep:.4f}"

# The ramp has to actually be a ramp, monotone and spread, or lut-neutral reads
# a handful of nearly identical greys.
assert GREYS == sorted(GREYS), "the grey ramp must be monotone"
assert GREYS[-1] - GREYS[0] > 0.9, "the grey ramp is too narrow to cover the diagonal"

# ---------------------------------------------------------------------------
# Geometry: one quad, reused. Patches differ ONLY in material and translation.
# ---------------------------------------------------------------------------

positions = [(-PATCH, -PATCH, 0.0), (PATCH, -PATCH, 0.0), (PATCH, PATCH, 0.0),
             (-PATCH, PATCH, 0.0)]
normals = [(0.0, 0.0, 1.0)] * 4
indices = [0, 1, 2, 0, 2, 3]

# Wide enough to sit behind the whole chart, so a patch always has something
# behind it and a hole reads as a hole rather than as the clear colour.
BACK_W = COLS * PITCH * 0.5 + 0.6
BACK_H = ROWS * PITCH * 0.5 + 0.6
back_positions = [(-BACK_W, -BACK_H, 0.0), (BACK_W, -BACK_H, 0.0),
                  (BACK_W, BACK_H, 0.0), (-BACK_W, BACK_H, 0.0)]

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
back_bytes = b"".join(struct.pack("<3f", *p) for p in back_positions)
idx_bytes = b"".join(struct.pack("<H", i) for i in indices)
_chunks = [(pos_bytes, 34962), (nrm_bytes, 34962), (back_bytes, 34962), (idx_bytes, 34963)]
buffer_bytes = b"".join(c for c, _ in _chunks)


def _views(chunks):
    """One bufferView per chunk, offsets accumulated rather than re-summed --
    gen_mask_fixture.py's note applies verbatim: a wrong offset here is not a
    build error, it is geometry that renders subtly wrong."""
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


def patch_centre(idx):
    """Grid position, centred on the origin. Column 0 is the LEFT of frame and
    row 0 the TOP, so the grey ramp reads across the top the way a chart does."""
    col, row = idx % COLS, idx // COLS
    x = (col - (COLS - 1) * 0.5) * PITCH
    y = ((ROWS - 1) * 0.5 - row) * PITCH
    return [x, y, 0.0]


nodes = [{"name": "backdrop", "mesh": len(patches), "translation": [0.0, 0.0, -0.8]}]
meshes = []
materials = []
for i, rgb in enumerate(patches):
    kind = "grey" if i < COLS else "colour"
    nodes.append({"name": f"patch_{i:02d}_{kind}", "mesh": i, "translation": patch_centre(i)})
    meshes.append({"name": f"patch_{i:02d}",
                   "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 3,
                                   "material": i}]})
    # Black base with an emissive face: the unlit-flat-colour idiom. What the
    # patch is worth is then a property of emissiveFactor alone, which is what
    # lets the chart decide where in the table it lands.
    materials.append({
        "name": f"lut_patch_{i:02d}",
        "pbrMetallicRoughness": {"baseColorFactor": [0.0, 0.0, 0.0, 1.0],
                                 "metallicFactor": 0.0, "roughnessFactor": 1.0},
        "emissiveFactor": [round(c, 6) for c in rgb],
    })

meshes.append({"name": "backdrop",
               "primitives": [{"attributes": {"POSITION": 2, "NORMAL": 1}, "indices": 3,
                               "material": len(patches)}]})
materials.append({"name": "lut_backdrop",
                  "pbrMetallicRoughness": {"baseColorFactor": [0.02, 0.02, 0.025, 1.0],
                                           "metallicFactor": 0.0, "roughnessFactor": 1.0}})

gltf = {
    "asset": {"version": "2.0", "generator": "gen_lut_fixture.py"},
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

# One dim white directional. Not for the patches -- their base colour is black,
# so it delivers them almost nothing -- but so the scene declares lights at all:
# a .cscn with none gets the render app's automatic three-point rig, which is
# three lights this fixture did not choose and cannot see the colour of.
# White and dim, so whatever it does contribute is achromatic and equal across
# the chart, which is what keeps the grey ramp grey.
CAM_Z = 7.4
cscn = {
    "version": 1,
    "models": [{"path": "lut_fixture.gltf"}],
    "lights": [{"name": "LutFill", "type": "directional", "direction": [0.0, 0.0, -1.0],
                "color": [1.0, 1.0, 1.0], "intensity": 0.05, "cast_shadows": False}],
    "camera": {"eye": [0.0, 0.0, CAM_Z], "target": [0.0, 0.0, 0.0], "fov": 40},
    # Pinned, because a LUT is indexed by the pixel's own value and an adapting
    # meter would move every patch to a different place in the table between runs.
    "post": {"tonemap": "neutral", "exposure": 1.0},
}

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, "lut_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
scn = os.path.join(here, "lut_fixture.cscn")
with open(scn, "w") as f:
    json.dump(cscn, f, indent=1)
    f.write("\n")

written = []
for name, title, n, fn in TABLES:
    p = os.path.join(here, name)
    write_cube(p, title, n, fn)
    written.append(f"{name} ({n}^3)")

# The tables must have the properties the docstring claims, checked against the
# closed forms rather than asserted in prose. Each of these has a matching gate
# arm; failing here means the fixture stopped testing what the arm believes.
for v in (0.0, 0.25, 0.5, 0.75, 1.0):
    assert lut_identity(v, v, v) == (v, v, v)
    r, g, b = lut_neutral(v, v, v)
    assert r == g == b == v, f"lut_neutral must be identity on the diagonal, got {(r, g, b)} at {v}"
# OFF the diagonal too: asserted only on it, `lambda r, g, b: (r, r, r)` passes.
assert lut_identity(0.2, 0.5, 0.9) == (0.2, 0.5, 0.9)
assert lut_swap(0.2, 0.5, 0.9) == (0.9, 0.5, 0.2)
# ...and non-separable off it, or it degenerates to the separable construction
# that measures the two interpolants as identical.
off = lut_neutral(0.8, 0.2, 0.5)
assert abs(off[0] - 0.8) > 1e-3, "lut_neutral has no cross-channel term; see the docstring"
# ...and ASYMMETRIC in its channels, which is the second and less obvious of the
# two ways this table can silently stop discriminating. Swapping the inputs of
# two channels must not simply swap the outputs.
a = lut_neutral(0.7, 0.3, 0.1)
sw = lut_neutral(0.3, 0.7, 0.1)
assert abs(a[0] - sw[1]) > 1e-3, "lut_neutral is channel-symmetric; the tint will cancel"

# THE BOUND THE ARM READS, not merely a non-zero check. The trilinear grey tint
# scales as amp / (N-1)^2, so both constants above steer it and either can take
# lut-neutral under its floor with every other assert here still passing. 3.0
# against the arm's LUT_TINT_MIN of 2 leaves the margin measurement showed (the
# rendered value is 5).
_tint_codes = 0.5 * NEUTRAL_AMP / (LUT_N_NEUTRAL - 1) ** 2 * 0.5 * 255.0
assert _tint_codes >= 3.0, (
    f"the neutral probe would tint greys by only {_tint_codes:.1f} codes; lut-neutral wants >= 2")

# lut_steep must be CURVED, or lut-agree runs on a table every interpolant
# reproduces exactly and the arm stops being about interpolation at all. Nothing
# guarded this: making s_curve the identity leaves the table a linear channel
# rotation and every arm still passes.
_mid = lut_steep(0.35, 0.35, 0.35)[0]
_chord = 0.5 * (lut_steep(0.20, 0.20, 0.20)[0] + lut_steep(0.50, 0.50, 0.50)[0])
assert abs(_mid - _chord) > 0.02, "lut_steep has no curvature; see the docstring"

# ...and lut_swap must be LINEAR, which is what lets lut-interp use it as the
# leg where the two interpolants are required to agree.
_a, _b = (0.2, 0.5, 0.9), (0.8, 0.1, 0.3)
_half = tuple(0.5 * (x + y) for x, y in zip(_a, _b))
_lerp = tuple(0.5 * (x + y) for x, y in zip(lut_swap(*_a), lut_swap(*_b)))
assert all(abs(x - y) < 1e-6 for x, y in zip(lut_swap(*_half), _lerp)), \
    "lut_swap is not linear; lut-interp's agreement leg depends on it"

print("wrote", out, "and", scn, f"({COLS}x{ROWS} chart: {COLS} greys + {len(COLOURS)} colours)")
print("wrote", ", ".join(written))
