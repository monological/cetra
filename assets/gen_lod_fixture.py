#!/usr/bin/env python3
"""Generate assets/lod_fixture.gltf -- the LOD selector's instrument (spec 11.28).

instancing_fixture is deliberately tiny, because it measures submission cost.
This one is the opposite: it exists to be DENSE enough that meshoptimizer can
actually build a chain, so the selector has something to select.

  - A smooth-shaded UV sphere, SEG_U x SEG_V quads. Smooth, i.e. vertices shared
    between faces, is load-bearing: meshopt_simplify collapses edges of the
    index buffer it is given, so a flat-shaded mesh (every face its own three
    vertices, no shared edges at all) is one it can barely touch. A flat sphere
    here would report a chain and never shrink.

  - Over the LOD_MIN_TRIANGLES floor in lod.c by a wide margin, so the fixture
    keeps working if that floor is raised a little.

  - A sphere rather than a card or a plane because meshoptimizer LOCKS MESH
    BORDERS, and a closed surface has none. Most of this repo's content is leaf
    cards and grass blades, which are nearly all border and simplify to
    approximately themselves -- correctly, and uselessly for an arm.

  - SPHERE_COUNT copies at increasing Z, so one frame contains several distances
    at once and a camera pull-back sweeps all of them through the ladder.

The arms read `triangles` out of the SUBMISSION table, which is the only counter
a level change moves: switching level leaves draws and instances exactly where
they were.

Regenerate with:
  python3 assets/gen_lod_fixture.py
"""

import base64
import json
import math
import os
import struct

SEG_U = 48        # longitude divisions
SEG_V = 24        # latitude divisions
RADIUS = 0.5
SPHERE_COUNT = 6  # copies marching away from the camera
SPACING = 2.4

# ---- sphere: shared vertices, smooth normals ------------------------------
sph_pos = []
sph_nrm = []
for v in range(SEG_V + 1):
    phi = math.pi * v / SEG_V
    for u in range(SEG_U + 1):
        theta = 2.0 * math.pi * u / SEG_U
        nx = math.sin(phi) * math.cos(theta)
        ny = math.cos(phi)
        nz = math.sin(phi) * math.sin(theta)
        sph_nrm.append((nx, ny, nz))
        sph_pos.append((nx * RADIUS, ny * RADIUS, nz * RADIUS))

sph_idx = []
row = SEG_U + 1
for v in range(SEG_V):
    for u in range(SEG_U):
        a = v * row + u
        b = a + 1
        c = a + row
        d = c + 1
        sph_idx.extend([a, c, b, b, c, d])

# ---- ground ---------------------------------------------------------------
G = 12.0
gnd_pos = [(-G, 0.0, -G), (G, 0.0, -G), (G, 0.0, G), (-G, 0.0, G)]
gnd_nrm = [(0.0, 1.0, 0.0)] * 4
gnd_idx = [0, 2, 1, 0, 3, 2]


def pack(fmt, rows):
    return b"".join(struct.pack(fmt, *r) for r in rows)


chunks = [
    pack("<3f", sph_pos),
    pack("<3f", sph_nrm),
    struct.pack("<%dI" % len(sph_idx), *sph_idx),
    pack("<3f", gnd_pos),
    pack("<3f", gnd_nrm),
    struct.pack("<%dI" % len(gnd_idx), *gnd_idx),
]

offsets = []
cursor = 0
for c in chunks:
    pad = (-cursor) % 4
    cursor += pad
    offsets.append(cursor)
    cursor += len(c)

buffer_bytes = b""
for off, c in zip(offsets, chunks):
    buffer_bytes += b"\x00" * (off - len(buffer_bytes)) + c

ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963
FLOAT = 5126
UNSIGNED_INT = 5125


def bounds(rows):
    return ([min(r[k] for r in rows) for k in range(3)],
            [max(r[k] for r in rows) for k in range(3)])


sph_min, sph_max = bounds(sph_pos)
gnd_min, gnd_max = bounds(gnd_pos)

nodes = [{"name": "Ground", "mesh": 1}]
for i in range(SPHERE_COUNT):
    nodes.append({
        "name": "Sphere_%02d" % i,
        "mesh": 0,
        "translation": [0.0, RADIUS, round(-i * SPACING, 6)],
    })

gltf = {
    "asset": {"version": "2.0", "generator": "gen_lod_fixture.py"},
    "materials": [
        {
            "name": "SphereClay",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.68, 0.60, 0.52, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.45,
            },
        },
        {
            "name": "GroundSlate",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.30, 0.31, 0.33, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.9,
            },
        },
    ],
    "scene": 0,
    "scenes": [{"nodes": list(range(len(nodes)))}],
    "nodes": nodes,
    "meshes": [
        {
            "name": "DenseSphere",
            "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}
            ],
        },
        {
            "name": "GroundQuad",
            "primitives": [
                {"attributes": {"POSITION": 3, "NORMAL": 4}, "indices": 5, "material": 1}
            ],
        },
    ],
    "accessors": [
        {"bufferView": 0, "componentType": FLOAT, "count": len(sph_pos), "type": "VEC3",
         "min": sph_min, "max": sph_max},
        {"bufferView": 1, "componentType": FLOAT, "count": len(sph_nrm), "type": "VEC3"},
        {"bufferView": 2, "componentType": UNSIGNED_INT, "count": len(sph_idx), "type": "SCALAR"},
        {"bufferView": 3, "componentType": FLOAT, "count": len(gnd_pos), "type": "VEC3",
         "min": gnd_min, "max": gnd_max},
        {"bufferView": 4, "componentType": FLOAT, "count": len(gnd_nrm), "type": "VEC3"},
        {"bufferView": 5, "componentType": UNSIGNED_INT, "count": len(gnd_idx), "type": "SCALAR"},
    ],
    "bufferViews": [
        {"buffer": 0, "byteOffset": offsets[0], "byteLength": len(chunks[0]),
         "target": ARRAY_BUFFER},
        {"buffer": 0, "byteOffset": offsets[1], "byteLength": len(chunks[1]),
         "target": ARRAY_BUFFER},
        {"buffer": 0, "byteOffset": offsets[2], "byteLength": len(chunks[2]),
         "target": ELEMENT_ARRAY_BUFFER},
        {"buffer": 0, "byteOffset": offsets[3], "byteLength": len(chunks[3]),
         "target": ARRAY_BUFFER},
        {"buffer": 0, "byteOffset": offsets[4], "byteLength": len(chunks[4]),
         "target": ARRAY_BUFFER},
        {"buffer": 0, "byteOffset": offsets[5], "byteLength": len(chunks[5]),
         "target": ELEMENT_ARRAY_BUFFER},
    ],
    "buffers": [
        {
            "byteLength": len(buffer_bytes),
            "uri": "data:application/octet-stream;base64,"
                   + base64.b64encode(buffer_bytes).decode("ascii"),
        }
    ],
}

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, "lod_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=2)
    f.write("\n")

print("wrote %s" % out)
print("  sphere: %d verts, %d triangles" % (len(sph_pos), len(sph_idx) // 3))
print("  %d spheres sharing it, plus a ground quad" % SPHERE_COUNT)
