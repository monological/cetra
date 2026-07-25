#!/usr/bin/env python3
"""Generate assets/aerial_fixture.gltf, the world-scale aerial-perspective asset.

Six jagged ridges receding from 400 m to 30 km, plus a ground plane. Every other
fixture in this repo is a prop shot a few metres across, where atmospheric
scattering is ~0.006% extinction and therefore invisible -- which is exactly why
spec 4.7 deferred aerial perspective. This is the world-scale scene it was
waiting for (spec 9.6).

Units are METRES, matching the engine's default world_units_per_km of 1000.
That is load-bearing: the atmosphere LUTs are parameterised in kilometres, so
the scene's own scale is what decides whether the effect exists at all.

Each ridge is a prism with a jagged crest -- front and back faces sloping to the
ground -- so there is real shading variation rather than a flat cutout, while
the crest still reads as a clean silhouette against the sky. Ridge heights grow
faster than their distances (h/d climbs 0.15 -> 0.30) so each successive ridge
sits higher in frame and the whole stack is visible at once: the classic
receding-ridgeline image, where the tell is that far ridges desaturate toward
the horizon colour rather than merely darkening.

Deterministic by construction (a fixed sum-of-sines crest, no RNG), so the file
regenerates byte-identically.

Regenerate with: python3 assets/gen_aerial_fixture.py
"""

import base64
import json
import math
import os
import struct

# Camera vantage the geometry is composed for; mirrored in aerial_fixture.cscn.
# High enough that the nearest visible ground clears the app's scene-scaled near
# plane (0.05 x scene radius, floored at a tenth of that -- render.c:1112).
CAM_Y = 400.0

# Distance out along -Z (m) and the screen elevation (degrees) each crest should
# reach from CAM_Y. Solving for height rather than authoring it is what makes the
# ridges stack as six separate silhouettes instead of the nearest one swallowing
# the rest: apparent elevation is what the eye reads, and it is not proportional
# to height.
#
# Everything starts at 20 km, which is geometry, not taste. A ridge's BASE sits
# at -atan(CAM_Y / d), so from a 400 m vantage anything nearer than ~20 km has
# its base visibly below the horizon and occludes every ridge behind it -- two
# earlier drafts of this fixture failed exactly that way. Past 20 km the bases
# converge on the horizon and the crests stack cleanly. It is also the range
# where aerial perspective is actually dramatic rather than marginal, so the
# constraint and the subject agree.
RIDGES_DEG = [
    (20000.0, 1.5),
    (28000.0, 2.5),
    (38000.0, 3.8),
    (52000.0, 5.4),
    (70000.0, 7.4),
    (95000.0, 10.0),
]
RIDGES = [(d, CAM_Y + d * math.tan(math.radians(a))) for d, a in RIDGES_DEG]

CREST_POINTS = 96  # samples across each crest
GROUND_Z = 500.0   # ground plane starts this far behind the camera


def crest(t, seed):
    """Deterministic 0..1 ridge profile. Sum of sines: no RNG, no numpy."""
    v = 0.0
    amp = 1.0
    freq = 2.3
    for k in range(4):
        v += amp * math.sin(freq * t * math.pi * 2.0 + seed * 1.7 + k * 2.399)
        amp *= 0.5
        freq *= 2.07
    # Taper the ends so a ridge does not shear off flat at the frame edge.
    taper = math.sin(min(max(t, 0.0), 1.0) * math.pi) ** 0.35
    return (0.55 + 0.45 * v * 0.5) * taper


positions = []
normals = []
indices = []


def add_quad(a, b, c, d):
    """Two CCW triangles, flat-shaded: each quad gets its own four verts."""
    base = len(positions)
    ux = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
    vx = (d[0] - a[0], d[1] - a[1], d[2] - a[2])
    nx = ux[1] * vx[2] - ux[2] * vx[1]
    ny = ux[2] * vx[0] - ux[0] * vx[2]
    nz = ux[0] * vx[1] - ux[1] * vx[0]
    ln = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
    n = (nx / ln, ny / ln, nz / ln)
    for p in (a, b, c, d):
        positions.append(p)
        normals.append(n)
    indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])


for ri, (dist, height) in enumerate(RIDGES):
    # Wide enough to fill a 45-degree frame at this distance with margin, so a
    # ridge never ends inside the view and reveal its finite width.
    half_w = dist * 0.9
    depth = dist * 0.16  # front-to-back thickness of the landform
    z_front = -(dist - depth * 0.5)
    z_back = -(dist + depth * 0.5)
    for j in range(CREST_POINTS):
        t0 = j / CREST_POINTS
        t1 = (j + 1) / CREST_POINTS
        x0 = -half_w + 2.0 * half_w * t0
        x1 = -half_w + 2.0 * half_w * t1
        y0 = height * crest(t0, ri)
        y1 = height * crest(t1, ri)
        # Front slope: ground up to the crest.
        add_quad(
            (x0, 0.0, z_front), (x1, 0.0, z_front), (x1, y1, (z_front + z_back) * 0.5),
            (x0, y0, (z_front + z_back) * 0.5),
        )
        # Back slope: crest down to the ground. Wound so it faces away.
        add_quad(
            (x0, y0, (z_front + z_back) * 0.5), (x1, y1, (z_front + z_back) * 0.5),
            (x1, 0.0, z_back), (x0, 0.0, z_back),
        )

ridge_vert_count = len(positions)

# Ground: one large quad under everything, its far edge well behind the last
# ridge so the horizon is the atmosphere rather than a visible plane edge. Kept
# no larger than that, because the scene radius it contributes to is what sets
# the app's near plane, and an over-large ground clips the foreground.
g_half = 110000.0
add_quad(
    (-g_half, 0.0, GROUND_Z), (g_half, 0.0, GROUND_Z),
    (g_half, 0.0, -130000.0), (-g_half, 0.0, -130000.0),
)

ridge_index_count = (ridge_vert_count // 4) * 6
ground_index_count = len(indices) - ridge_index_count

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
idx_bytes = b"".join(struct.pack("<I", i) for i in indices)
buffer_bytes = pos_bytes + nrm_bytes + idx_bytes

mn = [min(p[i] for p in positions) for i in range(3)]
mx = [max(p[i] for p in positions) for i in range(3)]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_aerial_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1]}],
    "nodes": [
        {"name": "aerial_ridges", "mesh": 0},
        {"name": "aerial_ground", "mesh": 1},
    ],
    "meshes": [
        {
            "name": "aerial_ridges",
            "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}
            ],
        },
        {
            "name": "aerial_ground",
            "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 3, "material": 1}
            ],
        },
    ],
    # Deliberately flat, mid-value and near-neutral. The whole point of the
    # fixture is to read the ATMOSPHERE's colour on these surfaces, so any
    # strong albedo or specular of their own would mask the thing under test.
    "materials": [
        {
            "name": "aerial_rock",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.30, 0.29, 0.27, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.95,
            },
        },
        {
            "name": "aerial_ground",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.26, 0.25, 0.23, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.95,
            },
        },
    ],
    "accessors": [
        {
            "bufferView": 0,
            "componentType": 5126,
            "count": len(positions),
            "type": "VEC3",
            "min": mn,
            "max": mx,
        },
        {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
        {
            "bufferView": 2,
            "componentType": 5125,
            "count": ridge_index_count,
            "type": "SCALAR",
        },
        {
            "bufferView": 2,
            "byteOffset": ridge_index_count * 4,
            "componentType": 5125,
            "count": ground_index_count,
            "type": "SCALAR",
        },
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

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "aerial_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote", out, "(", len(positions), "verts,", len(indices) // 3, "tris )")
