#!/usr/bin/env python3
"""Generate assets/conifer_fixture.{gltf,cscn} -- the tree generator's fixture (spec 13.35).

A ground quad and a `trees` block the render app grows with tree_gen. The conifer gate reads the
render app's --tree-probe over it: a digest of every stream of each tree's meshes, and for a
conifer its shape and its levels of detail. So each tree here is in the file for an arm:

  - broadleaf seed 42 and dead seed 4049, the recursive form, which the conifer form shares a
    generator with: their digests are held to the ones they had before it existed;
  - spruce, fir and snag at seeds 1 to 3, for the triangle budget, the card thinning and the
    shared height range of bark and needles;
  - spruce seeds 7 and 8, the one grown twice across two runs and the other a different tree;
  - spruce seed 1 again at irregularity 0, the tidy cone whose crown has to narrow.

The trees stand in rows a little apart so the scene also renders as a look at all of them.

Regenerate with:
  python3 assets/generators/gen_conifer_fixture.py
"""

import base64
import json
import struct
from fixture_paths import asset_path, asset_ref

# World units per tree unit: the presets' trunks are 125 units, so a tree here is about 10 m.
SCALE = 0.08
SPACING = 12.0

TREES = [
    ("broadleaf", 42, None),
    ("dead", 4049, None),
    ("spruce", 1, None),
    ("spruce", 2, None),
    ("spruce", 3, None),
    ("fir", 1, None),
    ("fir", 2, None),
    ("fir", 3, None),
    ("snag", 1, None),
    ("snag", 2, None),
    ("snag", 3, None),
    ("spruce", 7, None),
    ("spruce", 8, None),
    ("spruce", 1, 0.0),
]
PER_ROW = 7

trees = []
for i, (preset, seed, irregularity) in enumerate(TREES):
    row, col = divmod(i, PER_ROW)
    tree = {
        "preset": preset,
        "seed": seed,
        "position": [round((col - (PER_ROW - 1) / 2) * SPACING, 4), 0.0, round(-row * SPACING, 4)],
        "scale": SCALE,
    }
    if irregularity is not None:
        tree["irregularity"] = irregularity
    trees.append(tree)

# ---- ground ---------------------------------------------------------------
G = 60.0
gnd_pos = [(-G, 0.0, -G), (G, 0.0, -G), (G, 0.0, G), (-G, 0.0, G)]
gnd_nrm = [(0.0, 1.0, 0.0)] * 4
gnd_idx = [0, 2, 1, 0, 3, 2]


def pack(fmt, rows):
    return b"".join(struct.pack(fmt, *r) for r in rows)


chunks = [pack("<3f", gnd_pos), pack("<3f", gnd_nrm), struct.pack("<6I", *gnd_idx)]
offsets, cursor = [], 0
for c in chunks:
    cursor += (-cursor) % 4
    offsets.append(cursor)
    cursor += len(c)
buffer_bytes = b""
for off, c in zip(offsets, chunks):
    buffer_bytes += b"\x00" * (off - len(buffer_bytes)) + c

ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963
FLOAT = 5126
UNSIGNED_INT = 5125

gltf = {
    "asset": {"version": "2.0", "generator": "gen_conifer_fixture.py"},
    "materials": [{
        "name": "ConiferGround",
        "pbrMetallicRoughness": {"baseColorFactor": [0.22, 0.20, 0.17, 1.0],
                                 "metallicFactor": 0.0, "roughnessFactor": 0.95},
    }],
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"name": "Ground", "mesh": 0}],
    "meshes": [{"name": "GroundQuad", "primitives": [
        {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}]}],
    "accessors": [
        {"bufferView": 0, "componentType": FLOAT, "count": 4, "type": "VEC3",
         "min": [-G, 0.0, -G], "max": [G, 0.0, G]},
        {"bufferView": 1, "componentType": FLOAT, "count": 4, "type": "VEC3"},
        {"bufferView": 2, "componentType": UNSIGNED_INT, "count": 6, "type": "SCALAR"},
    ],
    "bufferViews": [
        {"buffer": 0, "byteOffset": offsets[0], "byteLength": len(chunks[0]),
         "target": ARRAY_BUFFER},
        {"buffer": 0, "byteOffset": offsets[1], "byteLength": len(chunks[1]),
         "target": ARRAY_BUFFER},
        {"buffer": 0, "byteOffset": offsets[2], "byteLength": len(chunks[2]),
         "target": ELEMENT_ARRAY_BUFFER},
    ],
    "buffers": [{"byteLength": len(buffer_bytes),
                 "uri": "data:application/octet-stream;base64,"
                        + base64.b64encode(buffer_bytes).decode("ascii")}],
}

cscn = {
    "version": 1,
    "models": [{"path": asset_ref("conifer_fixture.gltf")}],
    # A sun from behind the camera, so the trees are seen lit and not as silhouettes.
    "lights": [{"name": "ConiferSun", "type": "directional", "direction": [0.25, -0.6, -0.75],
                "color": [1.0, 0.97, 0.92], "intensity": 3.0, "cast_shadows": True}],
    "camera": {"eye": [0.0, 9.0, 34.0], "target": [0.0, 5.0, -6.0], "fov": 50},
    "post": {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False},
    "trees": trees,
}

with open(asset_path("conifer_fixture.gltf"), "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
with open(asset_path("conifer_fixture.cscn"), "w") as f:
    json.dump(cscn, f, indent=1)
    f.write("\n")

print("wrote %s and %s" % (asset_path("conifer_fixture.gltf"), asset_path("conifer_fixture.cscn")))
print("  %d trees on a %.0f m ground" % (len(trees), 2 * G))
