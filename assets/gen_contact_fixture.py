#!/usr/bin/env python3
"""Generate assets/contact_fixture.gltf, the screen-space contact-shadow test asset (spec 9.3).

Contact shadows are a SHORT-RANGE supplement to the shadow map: they darken the
near-contact seam a cascaded shadow map's texels are too coarse to draw, and they
only read on MATTE receivers. Crucially, the "big" shadow of an object cast on the
ground is the CSM's job (a large depth delta); a canonical contact shadow keeps
only the SMALL-delta near-contact and rejects the rest. So a single primitive on a
plane shows almost nothing -- its only genuine contact is a razor-thin sliver.

What the feature is FOR is fine near-contact DETAIL: many surfaces coming close
together at a scale below a shadow-map texel. This fixture is a pile of matte
rocks -- so the frame is full of crevices (rock-to-rock and rock-to-ground), each
a genuine small-delta contact the CSM blurs over.

Two things matter for the pile to look like rocks and not garbage:
  - rocks are DROPPED and settle ON each other (rest, touching), never
    interpenetrating -- interpenetrating spheres show hard intersection curves
    that look awful and read as "balls jammed together", not a pile;
  - each rock is a sphere DEFORMED by a few smooth lumps (normals recomputed), so
    it reads as a stone, not a billiard ball.

The sibling assets/contact_fixture.cscn ships a procedural-sky environment (the
ambient the crevices need so the term has something to darken, plus a
shadow-casting sun at an authored angle) and the camera, so the fixture is fully
self-contained -- no --sky/-e and no camera flags:

  ./out/bin/render -m assets/contact_fixture.gltf --contact-shadows

Toggle --contact-shadows (or the GUI checkbox) and watch the crevices between the
rocks deepen. --cs-strength / --cs-distance tune it.

Regenerate the geometry with:
  python3 assets/gen_contact_fixture.py
(the .cscn is hand-authored, not generated. Generation is seeded, so the pile is
deterministic and the golden reproduces.)
"""

import base64
import json
import math
import os
import random
import struct

rng = random.Random(20260724)   # deterministic pile: same rocks every run

positions, normals, uvs, indices = [], [], [], []

RINGS = 18
SEGS = 24


def _sphere_topology(rings, segs):
    tris = []
    for i in range(rings):
        for j in range(segs):
            a = i * (segs + 1) + j
            b, c, d = a + 1, a + (segs + 1), a + (segs + 2)
            tris += [(a, b, c), (b, d, c)]   # CCW seen from outside
    return tris


TOPOLOGY = _sphere_topology(RINGS, SEGS)


def _normalize(v):
    m = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) or 1.0
    return (v[0] / m, v[1] / m, v[2] / m)


def make_rock():
    """A unit sphere deformed by a few smooth lumps; returns (verts, normals).

    Displacement is a sum of positive directional lobes (dot^3, so smooth and
    C1), which pushes out rounded bumps -- lumpy stone, not spikes. Normals are
    recomputed from the deformed mesh, so the lighting follows the real surface.
    """
    lobes = [(_normalize((rng.gauss(0, 1), rng.gauss(0, 1), rng.gauss(0, 1))),
              rng.uniform(0.06, 0.16)) for _ in range(rng.randint(4, 6))]
    # gentle overall ellipsoid so silhouettes vary too
    es = (rng.uniform(0.85, 1.15), rng.uniform(0.85, 1.15), rng.uniform(0.85, 1.15))
    verts = []
    for i in range(RINGS + 1):
        phi = (i / RINGS) * math.pi
        sp, cp = math.sin(phi), math.cos(phi)
        for j in range(SEGS + 1):
            th = (j / SEGS) * 2.0 * math.pi
            n = (sp * math.cos(th), cp, sp * math.sin(th))
            disp = 1.0
            for (d, a) in lobes:
                dp = n[0] * d[0] + n[1] * d[1] + n[2] * d[2]
                if dp > 0.0:
                    disp += a * dp * dp * dp
            verts.append((n[0] * es[0] * disp, n[1] * es[1] * disp, n[2] * es[2] * disp))
    acc = [(0.0, 0.0, 0.0)] * len(verts)
    for (a, b, c) in TOPOLOGY:
        va, vb, vc = verts[a], verts[b], verts[c]
        e1 = (vb[0] - va[0], vb[1] - va[1], vb[2] - va[2])
        e2 = (vc[0] - va[0], vc[1] - va[1], vc[2] - va[2])
        fn = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
              e1[0] * e2[1] - e1[1] * e2[0])
        for k in (a, b, c):
            acc[k] = (acc[k][0] + fn[0], acc[k][1] + fn[1], acc[k][2] + fn[2])
    return verts, [_normalize(v) for v in acc]


def add_rock(cx, cy, cz, r, verts, norms):
    base = len(positions)
    for (vx, vy, vz), n in zip(verts, norms):
        positions.append((cx + vx * r, cy + vy * r, cz + vz * r))
        normals.append(n)
        uvs.append((0.0, 0.0))
    for (a, b, c) in TOPOLOGY:
        indices.extend((base + a, base + b, base + c))


# ---- drop-and-settle pile ----------------------------------------------------
# Drop each rock straight down at a random (x,z) biased toward the centre; it
# rests where its bounding sphere first touches the ground or an already-placed
# rock (never interpenetrating -- that is what kept the old fixture from looking
# like balls shoved through each other). Rocks touch, so the pile is full of
# genuine tangent crevices, and it heaps up because later rocks land on earlier.
placed = []   # (x, y, z, r)
N_ROCKS = 46
SAMPLE_R = 1.95
for _ in range(N_ROCKS):
    ang = rng.uniform(0.0, 2.0 * math.pi)
    rad = SAMPLE_R * math.sqrt(rng.random())
    x, z = rad * math.cos(ang), rad * math.sin(ang)
    r = rng.uniform(0.20, 0.34)
    y = r   # ground rest
    for (px, py, pz, pr) in placed:
        reach = (r + pr) ** 2 - ((x - px) ** 2 + (z - pz) ** 2)
        if reach > 0.0:
            y = max(y, py + math.sqrt(reach))   # rest on top of this rock
    placed.append((x, y, z, r))
    v, nrm = make_rock()
    add_rock(x, y, z, r, v, nrm)

rocks_vertex_count = len(positions)
rocks_index_count = len(indices)

# ---- ground quad in XZ, normal +Y --------------------------------------------
QUAD = 6.0
quad_positions = [(-QUAD, 0.0, -QUAD), (QUAD, 0.0, -QUAD), (QUAD, 0.0, QUAD), (-QUAD, 0.0, QUAD)]
quad_normals = [(0.0, 1.0, 0.0)] * 4
quad_uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
quad_indices = [0, 2, 1, 0, 3, 2]

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
print("wrote %s (%d settled rocks over a matte plane)" % (out, N_ROCKS))
