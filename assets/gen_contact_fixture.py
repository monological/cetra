#!/usr/bin/env python3
"""Generate assets/contact_fixture.gltf, the screen-space contact-shadow test asset (spec 9.3).

Contact shadows are a SHORT-RANGE supplement to the shadow map: they darken the
near-contact seam a cascaded shadow map's texels are too coarse to draw, and they
only read on MATTE receivers. Crucially, the "big" shadow of an object cast on the
ground is the CSM's job (a large depth delta); a canonical contact shadow keeps
only the SMALL-delta near-contact and rejects the rest. So a single primitive on a
plane shows almost nothing -- its only genuine contact is a razor-thin sliver at
the tangent line.

What the feature is actually FOR is fine near-contact DETAIL: many surfaces coming
close together at a scale below a shadow-map texel. This fixture is a mound of
matte pebbles -- dozens of overlapping rocks -- so the frame is full of crevices
(rock-to-rock and rock-to-ground), each a genuine small-delta contact the CSM
blurs over. That is where contact shadows read, and it is a curved-surface pile,
so no hard silhouette edge self-graze streaks.

The sibling assets/contact_fixture.cscn ships a shadow-casting sun and the camera,
so the fixture is self-contained -- no --sky/-e and no camera flags:

  ./out/bin/render -m assets/contact_fixture.gltf --contact-shadows

Toggle --contact-shadows (or the GUI checkbox) and watch the crevices between the
rocks deepen. --cs-strength / --cs-distance tune it.

Regenerate the geometry with:
  python3 assets/gen_contact_fixture.py
(the .cscn is hand-authored, not generated. Generation is seeded, so the mound is
deterministic and the golden reproduces.)
"""

import base64
import json
import math
import os
import random
import struct

random.seed(20260724)   # deterministic mound: same rocks every run, golden reproduces

positions, normals, uvs, indices = [], [], [], []


# ---- unit sphere template (reused, deformed per rock) ------------------------
def unit_sphere(rings, segs):
    verts, norms = [], []
    for i in range(rings + 1):
        phi = (i / rings) * math.pi
        sp, cp = math.sin(phi), math.cos(phi)
        for j in range(segs + 1):
            th = (j / segs) * 2.0 * math.pi
            verts.append((sp * math.cos(th), cp, sp * math.sin(th)))
    tris = []
    for i in range(rings):
        for j in range(segs):
            a = i * (segs + 1) + j
            b, c, d = a + 1, a + (segs + 1), a + (segs + 2)
            tris += [a, c, b, b, c, d]              # CCW seen from outside
    return verts, tris


SPHERE_V, SPHERE_TRIS = unit_sphere(12, 18)


def norm3(x, y, z):
    m = math.sqrt(x * x + y * y + z * z) or 1.0
    return (x / m, y / m, z / m)


def add_rock(cx, cz, cy, r, sx, sy, sz, yaw):
    """A sphere scaled to (r*sx, r*sy, r*sz), yawed, placed at (cx, cy, cz)."""
    base = len(positions)
    cyaw, syaw = math.cos(yaw), math.sin(yaw)
    for (vx, vy, vz) in SPHERE_V:
        # ellipsoid position; normal of a scaled sphere is (n/scale) normalized
        px, py, pz = vx * r * sx, vy * r * sy, vz * r * sz
        nx, ny, nz = norm3(vx / sx, vy / sy, vz / sz)
        # yaw about Y (keeps the lowest point, so resting height stays cy)
        px, pz = px * cyaw + pz * syaw, -px * syaw + pz * cyaw
        nx, nz = nx * cyaw + nz * syaw, -nx * syaw + nz * cyaw
        positions.append((cx + px, cy + py, cz + pz))
        normals.append((nx, ny, nz))
        uvs.append((0.0, 0.0))
    for t in SPHERE_TRIS:
        indices.append(base + t)


# ---- a mound of matte rocks --------------------------------------------------
# Scatter overlapping rocks in a disc, taller toward the centre so the pile heaps
# up: rocks interpenetrate and rest in each other's valleys, filling the frame
# with rock-to-rock and rock-to-ground crevices -- the near-contacts the feature
# draws. Overlap (not physical rest) is fine: a crevice where two rock surfaces
# come close IS the small-delta contact.
CLUSTER_R = 1.7
MOUND_H = 0.9
N_ROCKS = 70
for _ in range(N_ROCKS):
    # sample (x,z) biased toward the centre (sqrt for area-uniform, then squared
    # to concentrate) so the mound is denser and taller in the middle
    ang = random.uniform(0.0, 2.0 * math.pi)
    rad = CLUSTER_R * (random.random() ** 0.7)
    cx, cz = rad * math.cos(ang), rad * math.sin(ang)
    r = random.uniform(0.16, 0.30)
    sx, sy, sz = (random.uniform(0.8, 1.2) for _ in range(3))
    # mound profile: centre rocks sit higher (on top of others), rim rocks on the
    # ground; jitter so they nestle at varied heights.
    heap = MOUND_H * max(0.0, 1.0 - (rad / CLUSTER_R) ** 2)
    cy = r * sy + heap * random.uniform(0.0, 1.0)
    add_rock(cx, cz, cy, r, sx, sy, sz, random.uniform(0.0, 2.0 * math.pi))

rocks_vertex_count = len(positions)
rocks_index_count = len(indices)

# ---- ground quad in XZ, normal +Y --------------------------------------------
QUAD = 6.0
quad_positions = [(-QUAD, 0.0, -QUAD), (QUAD, 0.0, -QUAD), (QUAD, 0.0, QUAD), (-QUAD, 0.0, QUAD)]
quad_normals = [(0.0, 1.0, 0.0)] * 4
quad_uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
quad_indices = [0, 1, 2, 0, 2, 3]

positions += quad_positions
normals += quad_normals
uvs += quad_uvs
indices += [i + rocks_vertex_count for i in quad_indices]

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
uv_bytes = b"".join(struct.pack("<2f", *u) for u in uvs)
idx_bytes = b"".join(struct.pack("<I", i) for i in indices)
buffer_bytes = pos_bytes + nrm_bytes + uv_bytes + idx_bytes


def bounds(pts):
    return ([min(p[k] for p in pts) for k in range(3)], [max(p[k] for p in pts) for k in range(3)])


all_min, all_max = bounds(positions)


def matte(name, color):
    return {
        "name": name,
        "pbrMetallicRoughness": {
            "baseColorFactor": list(color) + [1.0],
            "metallicFactor": 0.0,
            "roughnessFactor": 0.9,
        },
    }


materials = [matte("contact_ground", (0.55, 0.55, 0.55)), matte("contact_rock", (0.58, 0.56, 0.53))]

nodes = [
    {"name": "rock_pile", "mesh": 0, "translation": [0.0, 0.0, 0.0]},
    {"name": "contact_ground", "mesh": 1, "translation": [0.0, 0.0, 0.0]},
]

meshes = [
    {"name": "contact_rocks",
     "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                     "indices": 3, "material": 1}]},
    {"name": "contact_ground",
     "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                     "indices": 4, "material": 0}]},
]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_contact_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": list(range(len(nodes)))}],
    "nodes": nodes,
    "meshes": meshes,
    "materials": materials,
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
         "min": all_min, "max": all_max},
        {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
        {"bufferView": 3, "componentType": 5125, "count": rocks_index_count, "type": "SCALAR"},
        {"bufferView": 3, "byteOffset": rocks_index_count * 4, "componentType": 5125,
         "count": len(quad_indices), "type": "SCALAR"},
    ],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(nrm_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes) + len(nrm_bytes), "byteLength": len(uv_bytes),
         "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes) + len(nrm_bytes) + len(uv_bytes),
         "byteLength": len(idx_bytes), "target": 34963},
    ],
    "buffers": [
        {"uri": "data:application/octet-stream;base64,"
         + base64.b64encode(buffer_bytes).decode("ascii"),
         "byteLength": len(buffer_bytes)}
    ],
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "contact_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote %s (%d rocks over a matte plane)" % (out, N_ROCKS))
