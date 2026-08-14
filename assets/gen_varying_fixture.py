#!/usr/bin/env python3
"""Generate assets/varying_fixture.gltf -- varyings read outside their own triangle (spec 11.38).

Under MSAA the fragment stage runs once per PIXEL while coverage is per SAMPLE, so every
varying is evaluated at one point: the pixel centre, unless it is qualified `centroid`. On a
partly covered pixel that centre can lie outside the triangle, and interpolating outside a
triangle is extrapolation -- the barycentric weights go negative and the value leaves the range
its three vertices bound. `RENDER_MODE_EXTRAPOLATION` reports exactly that, per channel.

Every element below exists because something measured on real content would otherwise be
unreadable:

  fins       thin AND SLANTED, so pixel centres genuinely fall outside. assets/parallax_
             fixture.gltf was tried first and reads exactly ZERO in all three channels: one
             large near-axis-aligned quad lands almost entirely on whole pixels -- 478,677 px
             of surface produced ~423 partly covered ones. Area is not the quantity; perimeter
             at an angle is.
  taper      base to tip spans several pixels down to a fraction of one, so one frame carries a
             range of coverage fractions rather than one.
  fanned     the normals and the vertex colours VARY across each triangle. This is the
             requirement that looks like decoration and is not: extrapolating a CONSTANT gives
             the constant back, so a flat quad with one normal cannot show normal extrapolation
             at any sample count. Real foliage fans its normals over a flat card for shading
             reasons; that is the case being reproduced.
  backdrop   attachment 0 clears to a 0.1 grey and that is not scene-overridable, so an
             uncovered pixel reads 25/255 in a debug frame -- above any small excursion. A
             large fully covered flat quad reads exactly 0 in all three channels, which makes
             it the background, the in-frame negative control, and the proof the mode is not
             stuck on, at once.
  uv_control UVs authored PAST 1.0, at full coverage. The UV channel is the one that cannot be
             sound in general -- nothing bounds a UV but what the mesh authored, and on
             apps/tree's tiling grass it reads 1,170,351 px at ONE sample, where extrapolation
             cannot happen. So this quad is the arm's liveness: it must fire always, at both
             sample counts, and it needs no shader lever to arm it.

OPAQUE throughout, and that is load-bearing rather than incidental. `a2c_capable` is
`msaa_samples > 1` (render.c), so alpha-to-coverage switches on and off WITH the sample count --
a MASK material would make every MSAA-against-1-sample reading a measurement of two changes at
once. It is also why spec 11.37's two-profile SSS fixture cannot share this file.

Read at --msaa 4 against --msaa 1. The single-sample render is ground truth: with coverage
reduced to one centre test there is no partial coverage to extrapolate from, so every channel
is zero on geometry by construction.

Deliberately NOT a golden. Its whole purpose is to differ between two sample counts, and the
arms compare within one build.

Regenerate with: python3 assets/gen_varying_fixture.py
"""

import base64
import json
import math
import os
import struct

# --- the fins -----------------------------------------------------------------------------

FINS = 24
FIN_HEIGHT = 1.0
FIN_BASE_W = 0.005  # ~4.6 px at the authored framing on a 1200 px-tall framebuffer
FIN_TIP_W = 0.0008  # ~0.7 px, i.e. under one sample at the top
FIN_SPAN = 0.30     # bases spread over [-FIN_SPAN, FIN_SPAN]
# Fanned in the SCREEN plane so no fin ever presents a backface -- the fins stay facing +Z and
# only their edges slant, which is what produces partly covered pixels without dragging
# backface culling or the gl_FrontFacing normal flip into the measurement.
#
# Splayed OUTWARD, tips away from the centre. Leaning them inward instead converges every tip
# on one point, and the pile of overlapping sub-pixel geometry there measures depth complexity
# rather than interpolation.
FIN_LEAN_DEG = 22.0
# Half-angle of the normal fan across a fin's width. Wide on purpose: the excursion from
# extrapolating between two unit vectors grows with the angle between them, and 50 degrees
# either side of facing still leaves both normals pointing toward the camera.
NORMAL_FAN_DEG = 50.0

# Colour varies along the fin for the same reason the normal fans across it. Base dark, tip
# near white: extrapolation past the tip overshoots ABOVE the tip value, and a value above 1
# is what printed as specks on apps/tree's grass, which colours itself entirely per vertex.
COLOR_BASE = (0.30, 0.55, 0.20, 1.0)
COLOR_TIP = (0.95, 0.98, 0.75, 1.0)

fin_pos, fin_nrm, fin_uv, fin_col, fin_idx = [], [], [], [], []
for i in range(FINS):
    # Bases evenly spread; lean fanned across the tuft so every fin presents a different edge
    # slope, and no two long edges share a pixel-grid relationship.
    t = i / (FINS - 1)
    x0 = -FIN_SPAN + 2.0 * FIN_SPAN * t
    lean = math.radians(FIN_LEAN_DEG - 2.0 * FIN_LEAN_DEG * t)
    # A distinct z per fin, so where the bases crowd together they cannot tie in depth and
    # leave draw order deciding which one a pixel gets.
    z0 = -0.001 * i

    ca, sa = math.cos(lean), math.sin(lean)

    def place(lx, ly):
        """Fin-local (x, y) through the lean, into world."""
        return (x0 + lx * ca - ly * sa, lx * sa + ly * ca, z0)

    fan = math.radians(NORMAL_FAN_DEG)
    # Normal at the left and right edge of the width, fanned about the fin's own long axis and
    # then carried through the same lean as the positions.
    def fanned(sign):
        nx, nz = sign * math.sin(fan), math.cos(fan)
        return (nx * ca, nx * sa, nz)

    hb, ht = 0.5 * FIN_BASE_W, 0.5 * FIN_TIP_W
    fin_pos += [place(-hb, 0.0), place(hb, 0.0), place(ht, FIN_HEIGHT), place(-ht, FIN_HEIGHT)]
    fin_nrm += [fanned(-1.0), fanned(1.0), fanned(1.0), fanned(-1.0)]
    # The full 0..1 range on every triangle: a quad split 0-1-2 / 0-2-3 gives each triangle
    # vertices spanning both axes end to end, so an excursion leaves [0,1] rather than merely
    # moving inside it. Authored v-up; the importer flips UVs, which the [0,1] test does not
    # care about.
    fin_uv += [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    fin_col += [COLOR_BASE, COLOR_BASE, COLOR_TIP, COLOR_TIP]
    b = 4 * i
    fin_idx += [b, b + 1, b + 2, b, b + 2, b + 3]

# --- the backdrop and the UV control quad -----------------------------------------------

# Larger than the frustum at its depth, so its own silhouette never enters the frame and every
# one of its pixels is fully covered.
BACK_HALF_W, BACK_HALF_H, BACK_Z = 2.6, 2.0, -1.0
back_pos = [(-BACK_HALF_W, 0.5 - BACK_HALF_H, BACK_Z), (BACK_HALF_W, 0.5 - BACK_HALF_H, BACK_Z),
            (BACK_HALF_W, 0.5 + BACK_HALF_H, BACK_Z), (-BACK_HALF_W, 0.5 + BACK_HALF_H, BACK_Z)]

# Clear of the fins in x, and fully covered, so its excursion is the authored one rather than
# anything coverage did to it.
CTRL_X0, CTRL_X1, CTRL_Y0, CTRL_Y1, CTRL_Z = -1.05, -0.78, 0.03, 0.30, -0.30
ctrl_pos = [(CTRL_X0, CTRL_Y0, CTRL_Z), (CTRL_X1, CTRL_Y0, CTRL_Z),
            (CTRL_X1, CTRL_Y1, CTRL_Z), (CTRL_X0, CTRL_Y1, CTRL_Z)]
# ENTIRELY outside [0,1], not spanning across it. Spanning 0 -> 1.4 was tried and makes the
# quad read as an L: only the strips where u or v exceed 1 excurse, so most of the surface
# reports nothing and a box placed on it can legitimately read zero. Wholly outside, every
# pixel of the quad carries the same excursion and any box on it is valid.
#
# The margin is wide enough that the flip the importer applies cannot bring it back inside:
# v -> 1 - v maps [1.2, 1.6] to [-0.6, -0.2], still out, still at least 0.2 clear.
CTRL_UV_LO, CTRL_UV_HI = 1.2, 1.6
ctrl_uv = [(CTRL_UV_LO, CTRL_UV_LO), (CTRL_UV_HI, CTRL_UV_LO), (CTRL_UV_HI, CTRL_UV_HI),
           (CTRL_UV_LO, CTRL_UV_HI)]

flat_nrm = [(0.0, 0.0, 1.0)] * 4
quad_idx = [0, 1, 2, 0, 2, 3]
flat_uv = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]

# --- buffer -------------------------------------------------------------------------------

ARRAY, ELEMENT = 34962, 34963
_chunks = [
    (b"".join(struct.pack("<3f", *p) for p in fin_pos), ARRAY),
    (b"".join(struct.pack("<3f", *n) for n in fin_nrm), ARRAY),
    (b"".join(struct.pack("<2f", *u) for u in fin_uv), ARRAY),
    (b"".join(struct.pack("<4f", *c) for c in fin_col), ARRAY),
    (b"".join(struct.pack("<H", i) for i in fin_idx), ELEMENT),
    (b"".join(struct.pack("<3f", *p) for p in back_pos), ARRAY),
    (b"".join(struct.pack("<3f", *n) for n in flat_nrm), ARRAY),
    (b"".join(struct.pack("<2f", *u) for u in flat_uv), ARRAY),
    (b"".join(struct.pack("<3f", *p) for p in ctrl_pos), ARRAY),
    (b"".join(struct.pack("<2f", *u) for u in ctrl_uv), ARRAY),
    (b"".join(struct.pack("<H", i) for i in quad_idx), ELEMENT),
]
buffer_bytes = b"".join(c for c, _ in _chunks)


def _views(chunks):
    """One bufferView per chunk, offsets accumulated rather than re-summed.

    A running total because the alternative is a chain of len(a) + len(b) + ... that grows a
    term with every attribute -- and a wrong offset there is not a build error, it is geometry
    that renders subtly wrong.
    """
    views, offset = [], 0
    for data, target in chunks:
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data),
                      "target": target})
        offset += len(data)
    return views


def _bounds(points):
    n = len(points[0])
    return ([min(p[i] for p in points) for i in range(n)],
            [max(p[i] for p in points) for i in range(n)])


fin_mn, fin_mx = _bounds(fin_pos)
back_mn, back_mx = _bounds(back_pos)
ctrl_mn, ctrl_mx = _bounds(ctrl_pos)

# Mid-grey rather than white: white clips against the tonemap, and two clipped surfaces compare
# equal however wrong the shading behind them is.
FIN_ALBEDO = [0.62, 0.62, 0.62, 1.0]
# Dark, so the fins read against it and a stray bright pixel is unmistakable.
BACK_ALBEDO = [0.05, 0.06, 0.09, 1.0]
CTRL_ALBEDO = [0.40, 0.40, 0.45, 1.0]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_varying_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1, 2]}],
    "nodes": [
        {"name": "backdrop", "mesh": 1},
        {"name": "uv_control", "mesh": 2},
        {"name": "fins", "mesh": 0},
    ],
    "meshes": [
        # One primitive for all 24 fins rather than 24 nodes: the measurement is about
        # rasterization, and a single draw keeps instancing, batching and per-node transforms
        # out of it entirely.
        {"name": "fins",
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2,
                                        "COLOR_0": 3},
                         "indices": 4, "material": 0}]},
        {"name": "backdrop",
         "primitives": [{"attributes": {"POSITION": 5, "NORMAL": 6, "TEXCOORD_0": 7},
                         "indices": 10, "material": 1}]},
        {"name": "uv_control",
         "primitives": [{"attributes": {"POSITION": 8, "NORMAL": 6, "TEXCOORD_0": 9},
                         "indices": 10, "material": 2}]},
    ],
    "materials": [
        # No alphaMode: OPAQUE is glTF's default and is the point (see the module docstring).
        {"name": "vary_fin",
         "pbrMetallicRoughness": {"baseColorFactor": FIN_ALBEDO, "metallicFactor": 0.0,
                                  "roughnessFactor": 0.45}},
        {"name": "vary_backdrop",
         "pbrMetallicRoughness": {"baseColorFactor": BACK_ALBEDO, "metallicFactor": 0.0,
                                  "roughnessFactor": 0.9}},
        {"name": "vary_uv_control",
         "pbrMetallicRoughness": {"baseColorFactor": CTRL_ALBEDO, "metallicFactor": 0.0,
                                  "roughnessFactor": 0.6}},
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": len(fin_pos), "type": "VEC3",
         "min": fin_mn, "max": fin_mx},
        {"bufferView": 1, "componentType": 5126, "count": len(fin_nrm), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(fin_uv), "type": "VEC2"},
        {"bufferView": 3, "componentType": 5126, "count": len(fin_col), "type": "VEC4"},
        {"bufferView": 4, "componentType": 5123, "count": len(fin_idx), "type": "SCALAR"},
        {"bufferView": 5, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": back_mn, "max": back_mx},
        {"bufferView": 6, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 7, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": 8, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": ctrl_mn, "max": ctrl_mx},
        {"bufferView": 9, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": 10, "componentType": 5123, "count": 6, "type": "SCALAR"},
    ],
    "bufferViews": _views(_chunks),
    "buffers": [
        {"uri": "data:application/octet-stream;base64," +
                base64.b64encode(buffer_bytes).decode("ascii"),
         "byteLength": len(buffer_bytes)},
    ],
}

# Directional and head-on: the fins fan their normals about their own axes, so a light that
# grazed them would make the shaded arm a measurement of the fan rather than of the
# interpolation. Nothing casts -- a shadow map's own filtering at these widths would be a
# second source of edge behaviour.
cscn = {
    "version": 1,
    "models": [{"path": "varying_fixture.gltf"}],
    "lights": [{"name": "VarySun", "type": "directional", "direction": [0.0, -0.15, -0.99],
                "color": [1.0, 1.0, 1.0], "intensity": 3.0, "cast_shadows": False}],
    # Frames the fins at about 60% of the frame height, with the backdrop filling the rest and
    # the UV control clear of both in x.
    "camera": {"eye": [0.0, 0.5, 2.0], "target": [0.0, 0.5, 0.0], "fov": 45},
    "post": {"tonemap": "neutral", "exposure": 1.0},
}

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, "varying_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
scn = os.path.join(here, "varying_fixture.cscn")
with open(scn, "w") as f:
    json.dump(cscn, f, indent=1)
    f.write("\n")
print("wrote", out, "and", scn,
      f"({FINS} fins {FIN_BASE_W}->{FIN_TIP_W} wide, splayed +/-{FIN_LEAN_DEG:.0f} deg, "
      f"normals fanned +/-{NORMAL_FAN_DEG:.0f} deg, backdrop, "
      f"UV control at {CTRL_UV_LO}-{CTRL_UV_HI})")
