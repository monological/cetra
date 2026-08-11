#!/usr/bin/env python3
"""Generate assets/instancing_fixture.gltf -- the submission-path instrument (spec 11.28).

Every other fixture in this directory pins what a pixel should be. This one pins
how many DRAWS it took to get there, so its shape is chosen for countability
rather than for closed-form radiometry.

  - ONE mesh, referenced by PROP_COUNT nodes. The glTF says outright that these
    are the same geometry, which is the fact import.c currently discards (it
    builds a fresh Mesh per node, so 130 nodes become 130 VAOs). After the
    dedup phase the importer must report 2 meshes, not 131, and the batcher
    must collapse the props to a handful of instanced draws.

  - PROP_COUNT = 130, deliberately NOT a multiple of the 64-instance UBO chunk.
    130 = 2*64 + 2, so the last chunk carries two instances and any off-by-one
    at the chunk seam renders 128 props correctly and 2 wrongly -- which a
    whole-frame comparison against a same-chunked build cannot see, and a
    comparison against a differently-chunked one can.

  - Every prop is IDENTICAL in size and material and differs only in position,
    so the batch key is the mesh pointer alone and nothing else can be blamed
    when a run fails to batch.

  - The grid is entirely inside the gate camera's frustum, so `meshes culled`
    is 0 for the camera pass and any nonzero reading is the culler, not the
    framing. The far-camera arm moves the camera instead of the content.

The ground plane is a second mesh with its own material: it receives the
shadow (so the cascade pass has work whose loss would be visible) and it makes
the prop/ground split legible in the material-switch count.

The sibling instancing_fixture.cscn is hand-authored, and its sun casts -- the
whole point is that the shadow pass, which walks the graph once per cascade
with no culling, has something to walk.

Regenerate with:
  python3 assets/gen_instancing_fixture.py
"""

import base64
import json
import os
import struct

PROP_COUNT = 130  # 2 * 64 + 2 -- straddles the UBO chunk seam on purpose
COLS = 13
ROWS = 10
SPACING = 0.62
PROP_HALF = 0.15  # half-extent of the prop box
GROUND = 5.5      # ground half-extent

assert COLS * ROWS == PROP_COUNT

# ---- prop: a unit box, 24 vertices so each face gets a flat normal ---------
# 12 triangles. Small on purpose: the fixture measures submission cost, and
# heavy geometry would bury it under vertex work.
h = PROP_HALF
FACES = [
    ((0.0, 0.0, 1.0), [(-h, -h, h), (h, -h, h), (h, h, h), (-h, h, h)]),
    ((0.0, 0.0, -1.0), [(h, -h, -h), (-h, -h, -h), (-h, h, -h), (h, h, -h)]),
    ((1.0, 0.0, 0.0), [(h, -h, h), (h, -h, -h), (h, h, -h), (h, h, h)]),
    ((-1.0, 0.0, 0.0), [(-h, -h, -h), (-h, -h, h), (-h, h, h), (-h, h, -h)]),
    ((0.0, 1.0, 0.0), [(-h, h, h), (h, h, h), (h, h, -h), (-h, h, -h)]),
    ((0.0, -1.0, 0.0), [(-h, -h, -h), (h, -h, -h), (h, -h, h), (-h, -h, h)]),
]

box_pos = []
box_nrm = []
box_idx = []
for normal, corners in FACES:
    base = len(box_pos)
    box_pos.extend(corners)
    box_nrm.extend([normal] * 4)
    box_idx.extend([base, base + 1, base + 2, base, base + 2, base + 3])

# ---- ground: one quad in the XZ plane, normal +Y --------------------------
ground_pos = [(-GROUND, 0.0, -GROUND), (GROUND, 0.0, -GROUND),
              (GROUND, 0.0, GROUND), (-GROUND, 0.0, GROUND)]
ground_nrm = [(0.0, 1.0, 0.0)] * 4
ground_idx = [0, 2, 1, 0, 3, 2]


def pack(fmt, rows):
    return b"".join(struct.pack(fmt, *r) for r in rows)


chunks = [
    pack("<3f", box_pos),
    pack("<3f", box_nrm),
    struct.pack("<%dH" % len(box_idx), *box_idx),
    pack("<3f", ground_pos),
    pack("<3f", ground_nrm),
    struct.pack("<%dH" % len(ground_idx), *ground_idx),
]

offsets = []
cursor = 0
for c in chunks:
    # 4-byte alignment: glTF requires accessor byte offsets to be a multiple of
    # their component size, and the index chunks are 2-byte shorts.
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
UNSIGNED_SHORT = 5123


def bounds(rows):
    return ([min(r[k] for r in rows) for k in range(3)],
            [max(r[k] for r in rows) for k in range(3)])


box_min, box_max = bounds(box_pos)
ground_min, ground_max = bounds(ground_pos)

# ---- nodes: one ground, then PROP_COUNT nodes ALL pointing at mesh 0 -------
nodes = [{"name": "Ground", "mesh": 1}]
for i in range(PROP_COUNT):
    col = i % COLS
    row = i // COLS
    x = (col - (COLS - 1) / 2.0) * SPACING
    z = (row - (ROWS - 1) / 2.0) * SPACING
    nodes.append({
        "name": "Prop_%03d" % i,
        "mesh": 0,
        "translation": [round(x, 6), PROP_HALF, round(z, 6)],
    })

gltf = {
    "asset": {"version": "2.0", "generator": "gen_instancing_fixture.py"},
    "materials": [
        {
            "name": "PropGrey",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.72, 0.70, 0.66, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.6,
            },
        },
        {
            "name": "GroundSlate",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.32, 0.33, 0.35, 1.0],
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
            "name": "PropBox",
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
        {"bufferView": 0, "componentType": FLOAT, "count": len(box_pos), "type": "VEC3",
         "min": box_min, "max": box_max},
        {"bufferView": 1, "componentType": FLOAT, "count": len(box_nrm), "type": "VEC3"},
        {"bufferView": 2, "componentType": UNSIGNED_SHORT, "count": len(box_idx), "type": "SCALAR"},
        {"bufferView": 3, "componentType": FLOAT, "count": len(ground_pos), "type": "VEC3",
         "min": ground_min, "max": ground_max},
        {"bufferView": 4, "componentType": FLOAT, "count": len(ground_nrm), "type": "VEC3"},
        {"bufferView": 5, "componentType": UNSIGNED_SHORT, "count": len(ground_idx),
         "type": "SCALAR"},
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
out = os.path.join(here, "instancing_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=2)
    f.write("\n")

print("wrote %s" % out)
print("  %d prop nodes -> 1 mesh, %d triangles each" % (PROP_COUNT, len(box_idx) // 3))
print("  %d mesh-bearing nodes total, 2 distinct meshes, 2 materials" % (PROP_COUNT + 1))
