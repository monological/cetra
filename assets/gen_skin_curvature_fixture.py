#!/usr/bin/env python3
"""Generate assets/skin_curvature_fixture.gltf, the pre-integrated-skin subject.

Curvature is the axis this feature varies along, so a single sphere cannot test
it: a sphere has ONE curvature everywhere. This fixture is a curvature ladder.

Three nodes share ONE unit-sphere mesh at uniform scales 0.5 / 1.0 / 2.0, so
their curvatures are exactly 2.0 / 1.0 / 0.5 per unit -- a 4x spread in 2x steps,
giving two independent ordering inequalities with no geometry difference to
confound them. Reusing one mesh is what keeps that exact; scaling a node is the
only difference between the three.

A fourth node is a prolate ellipsoid, and it is here for a different reason: the
spheres each have uniform curvature, so they show the effect varying BETWEEN
objects but never ACROSS one surface. The ellipsoid varies continuously from
sharp at the tips to flat at the waist, which is what a cheekbone or a nose
actually does. Its normals are the analytic gradient of the implicit surface,
(x/a^2, y/b^2, z/c^2) normalised -- NOT the sphere normal, which a non-uniform
node scale would have given.

Everything is smooth-shaded and convex on purpose. Curvature is read from the
rate of change of the interpolated vertex normal, so a flat-shaded mesh reads
zero across each facet and spikes at the seams; and Penner's model assumes a
convex surface, so a saddle would be outside what the fit describes.

Lighting, camera and the skin material live in the sibling .cscn.
Regenerate with: python3 assets/gen_skin_curvature_fixture.py
"""

import base64
import json
import math
import os
import struct

RINGS = 48    # latitude divisions, matching the other sphere fixtures
SECTORS = 96  # longitude divisions

# Ellipsoid semi-axes. Prolate along X so the curvature sweep runs left-to-right
# across the silhouette, the same direction the sphere ladder runs.
ELL = (2.2, 0.85, 0.85)


def sphere_dirs():
    """Unit directions over a phi/theta sweep, plus the shared index buffer."""
    dirs = []
    for r in range(RINGS + 1):
        phi = (r / RINGS) * math.pi  # 0 (top) .. pi (bottom)
        for s in range(SECTORS + 1):
            theta = (s / SECTORS) * 2.0 * math.pi
            dirs.append(
                (
                    math.sin(phi) * math.cos(theta),
                    math.cos(phi),
                    math.sin(phi) * math.sin(theta),
                )
            )
    idx = []
    for r in range(RINGS):
        for s in range(SECTORS):
            a = r * (SECTORS + 1) + s
            b = a + SECTORS + 1
            # CCW seen from OUTSIDE (commit 7ae16d0). The natural-looking
            # (a, b, a+1) order faces every triangle inward under this sweep.
            idx.extend([a, a + 1, b, a + 1, b + 1, b])
    return dirs, idx


DIRS, INDICES = sphere_dirs()

# Unit sphere: position IS the normal, so the ladder's normals are exact at
# every tessellation level.
sphere_pos = DIRS
sphere_nrm = DIRS

# Ellipsoid: scale the direction for position, but take the normal from the
# implicit gradient. Substituting the scaled direction into (x/a^2, ...) leaves
# (nx/a, ny/b, nz/c), which is only parallel to the direction when a == b == c.
a, b, c = ELL
ell_pos = [(a * d[0], b * d[1], c * d[2]) for d in DIRS]
ell_nrm = []
for d in DIRS:
    g = (d[0] / a, d[1] / b, d[2] / c)
    inv = 1.0 / math.sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2])
    ell_nrm.append((g[0] * inv, g[1] * inv, g[2] * inv))


def pack_vec3(v):
    return b"".join(struct.pack("<3f", *p) for p in v)


# uint32 indices (5125): correct at any tessellation, matching the other sphere
# generators that outgrew uint16.
idx_bytes = b"".join(struct.pack("<I", i) for i in INDICES)
blobs = [
    pack_vec3(sphere_pos),
    pack_vec3(sphere_nrm),
    pack_vec3(ell_pos),
    pack_vec3(ell_nrm),
    idx_bytes,
]
offsets = []
cursor = 0
for blob in blobs:
    offsets.append(cursor)
    cursor += len(blob)
buffer_bytes = b"".join(blobs)


def bounds(v):
    return (
        [min(p[i] for p in v) for i in range(3)],
        [max(p[i] for p in v) for i in range(3)],
    )


sphere_min, sphere_max = bounds(sphere_pos)
ell_min, ell_max = bounds(ell_pos)

# Sphere centres sit on one line (y = 0) rather than resting on a common plane,
# so a single grazing key meets all three at the same incidence and any
# difference between them is curvature, not framing. Gaps are ~0.4 at the
# closest approach, so no sphere occludes or bounces into its neighbour.
LADDER = [
    ("curv_r050", 0.5, -3.6),
    ("curv_r100", 1.0, -1.7),
    ("curv_r200", 2.0, 1.7),
]

nodes = [
    {
        "name": name,
        "mesh": 0,
        "translation": [x, 0.0, 0.0],
        "scale": [s, s, s],
    }
    for name, s, x in LADDER
]
nodes.append({"name": "curv_ellipsoid", "mesh": 1, "translation": [0.0, 3.0, 0.0]})

gltf = {
    "asset": {"version": "2.0", "generator": "gen_skin_curvature_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": list(range(len(nodes)))}],
    "nodes": nodes,
    "meshes": [
        {
            "name": "curv_sphere",
            "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 4, "material": 0}
            ],
        },
        {
            "name": "curv_ellipsoid",
            "primitives": [
                {"attributes": {"POSITION": 2, "NORMAL": 3}, "indices": 4, "material": 0}
            ],
        },
    ],
    # ONE material across every node, so the whole ladder shares a single
    # scatter profile and nothing in a comparison varies but curvature.
    #
    # Deliberately a plain grey dielectric here: the skin tone, roughness and
    # scatter are all authored in the sibling .cscn, so the fixture exercises
    # the scene file's material overrides rather than duplicating them. glTF
    # cannot express subsurface at all, which is why that half has to live
    # there regardless.
    "materials": [
        {
            "name": "skin",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.5, 0.5, 0.5, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.5,
            },
        }
    ],
    "accessors": [
        {
            "bufferView": 0,
            "componentType": 5126,
            "count": len(sphere_pos),
            "type": "VEC3",
            "min": sphere_min,
            "max": sphere_max,
        },
        {"bufferView": 1, "componentType": 5126, "count": len(sphere_nrm), "type": "VEC3"},
        {
            "bufferView": 2,
            "componentType": 5126,
            "count": len(ell_pos),
            "type": "VEC3",
            "min": ell_min,
            "max": ell_max,
        },
        {"bufferView": 3, "componentType": 5126, "count": len(ell_nrm), "type": "VEC3"},
        {"bufferView": 4, "componentType": 5125, "count": len(INDICES), "type": "SCALAR"},
    ],
    "bufferViews": [
        {"buffer": 0, "byteOffset": offsets[0], "byteLength": len(blobs[0]), "target": 34962},
        {"buffer": 0, "byteOffset": offsets[1], "byteLength": len(blobs[1]), "target": 34962},
        {"buffer": 0, "byteOffset": offsets[2], "byteLength": len(blobs[2]), "target": 34962},
        {"buffer": 0, "byteOffset": offsets[3], "byteLength": len(blobs[3]), "target": 34962},
        {"buffer": 0, "byteOffset": offsets[4], "byteLength": len(blobs[4]), "target": 34963},
    ],
    "buffers": [
        {
            "uri": "data:application/octet-stream;base64,"
            + base64.b64encode(buffer_bytes).decode("ascii"),
            "byteLength": len(buffer_bytes),
        }
    ],
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "skin_curvature_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print(
    "wrote", out,
    "(", len(sphere_pos), "verts/mesh,", len(INDICES) // 3, "tris/mesh, 4 nodes )",
)
