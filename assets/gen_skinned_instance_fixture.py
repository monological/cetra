#!/usr/bin/env python3
"""Generate assets/skinned_instance_fixture.gltf -- the batching-eligibility arm (spec 11.28).

instancing_fixture proves that repeated meshes DO batch. This one proves that a
repeated mesh whose program cannot read InstanceBlock does NOT, which is the
opposite failure and the one that deletes geometry rather than merely costing
draws.

  - ONE SKINNED mesh referenced by TWO nodes at different translations. Skinned
    meshes compile to pbr_skinned, whose vertex stage takes the object
    transform from a plain `model` uniform and has no instance block at all.
    Batch the two and glDrawElementsInstanced draws both copies at the FIRST
    node's transform: the second quad does not move, it disappears.

  - The two nodes are adjacent in the scene and carry the same material, so
    every other batching precondition is met. The program is the only thing
    standing between this fixture and a wrong frame -- which is exactly what
    the arms need to be measuring.

  - Deliberately NOT given a golden. The arms are `draws == 2` (an integer with
    no noise floor) and 0 px against --no-instancing (an inverse arm that fails
    at ~29% of the frame when eligibility is not enforced). A golden would add
    a re-bake surface without adding a way for this to fail.

The joints are left at the origin with identity inverse-bind matrices and every
vertex fully weighted to joint 0, so skinning resolves to the bind pose. The
fixture is about which PROGRAM the mesh takes, not about what skinning computes,
and a pose that cannot drift is one less reason for the frame to move.

Regenerate with:
  python3 assets/gen_skinned_instance_fixture.py
"""

import base64
import json
import os
import struct

HALF = 0.4      # panel half-extent
OFFSET = 0.55   # each panel's distance from the origin along X
SEG = 32        # subdivisions per side

# ---- one subdivided panel in the XY plane, normal +Z -----------------------
# Subdivided, not a single quad, so it clears lod.c's triangle floor by a wide
# margin. That is what makes the lod-skinned arm mean something: a mesh this
# size WOULD be given a chain, and the only reason it is not is that it is
# skinned. A four-vertex quad would be refused for being small and the arm would
# pass without ever exercising the rule it names.
quad_pos = []
quad_nrm = []
for v in range(SEG + 1):
    for u in range(SEG + 1):
        x = -HALF + 2.0 * HALF * u / SEG
        y = -HALF + 2.0 * HALF * v / SEG
        quad_pos.append((x, y, 0.0))
        quad_nrm.append((0.0, 0.0, 1.0))

quad_idx = []
row = SEG + 1
for v in range(SEG):
    for u in range(SEG):
        a = v * row + u
        quad_idx.extend([a, a + 1, a + row, a + 1, a + row + 1, a + row])

# Every vertex rigid to joint 0: the bind pose, and nothing that can drift.
quad_joints = [(0, 0, 0, 0)] * len(quad_pos)
quad_weights = [(1.0, 0.0, 0.0, 0.0)] * len(quad_pos)

IDENTITY = (1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0)


def pack(fmt, rows):
    return b"".join(struct.pack(fmt, *r) for r in rows)


chunks = [
    pack("<3f", quad_pos),
    pack("<3f", quad_nrm),
    struct.pack("<%dI" % len(quad_idx), *quad_idx),
    pack("<4H", quad_joints),
    pack("<4f", quad_weights),
    struct.pack("<16f", *IDENTITY),
]

offsets = []
cursor = 0
for c in chunks:
    # glTF requires accessor byte offsets to be a multiple of the component
    # size; the index and joint chunks are 2-byte shorts.
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
UNSIGNED_INT = 5125

quad_min = [min(r[k] for r in quad_pos) for k in range(3)]
quad_max = [max(r[k] for r in quad_pos) for k in range(3)]

# Node 0 is the joint; 1 and 2 are the two mesh instances that must not batch.
nodes = [
    {"name": "Joint_Root"},
    {"name": "SkinnedQuad_A", "mesh": 0, "skin": 0, "translation": [-OFFSET, 0.0, 0.0]},
    {"name": "SkinnedQuad_B", "mesh": 0, "skin": 0, "translation": [OFFSET, 0.0, 0.0]},
]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_skinned_instance_fixture.py"},
    "materials": [
        {
            "name": "QuadWarm",
            "doubleSided": True,
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.78, 0.62, 0.44, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.55,
            },
        },
    ],
    "scene": 0,
    "scenes": [{"nodes": [0, 1, 2]}],
    "nodes": nodes,
    "skins": [{"joints": [0], "inverseBindMatrices": 5}],
    "meshes": [
        {
            "name": "SkinnedQuad",
            "primitives": [
                {
                    "attributes": {"POSITION": 0, "NORMAL": 1, "JOINTS_0": 3, "WEIGHTS_0": 4},
                    "indices": 2,
                    "material": 0,
                }
            ],
        },
    ],
    "accessors": [
        {"bufferView": 0, "componentType": FLOAT, "count": len(quad_pos), "type": "VEC3",
         "min": quad_min, "max": quad_max},
        {"bufferView": 1, "componentType": FLOAT, "count": len(quad_nrm), "type": "VEC3"},
        {"bufferView": 2, "componentType": UNSIGNED_INT, "count": len(quad_idx),
         "type": "SCALAR"},
        {"bufferView": 3, "componentType": UNSIGNED_SHORT, "count": len(quad_joints),
         "type": "VEC4"},
        {"bufferView": 4, "componentType": FLOAT, "count": len(quad_weights), "type": "VEC4"},
        {"bufferView": 5, "componentType": FLOAT, "count": 1, "type": "MAT4"},
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
        {"buffer": 0, "byteOffset": offsets[5], "byteLength": len(chunks[5])},
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
out = os.path.join(here, "skinned_instance_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=2)
    f.write("\n")

print("wrote %s" % out)
print("  2 mesh-bearing nodes -> 1 skinned mesh, %d triangles (over lod.c's floor)"
      % (len(quad_idx) // 3))
print("  both nodes share the mesh; only the program stops them batching")
