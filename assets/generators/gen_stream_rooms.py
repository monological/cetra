#!/usr/bin/env python3
"""Generate stream_rooms.gltf and stream_rooms.cscn: lighting data that streams (spec 13.24).

Ten closed rooms in a row along +x, ROOM_SPACING apart, far enough that only a few are near the
camera at once. Each room has:
- its own GI volume, two reflection probes and two bodied cached point lights;
- its own colour on its back and left walls, so a wrong volume's bounce or a wrong probe's
  reflection shows as the wrong colour rather than as a subtle shading change;
- a polished floor, which is what reflects the probes.

The counts are chosen to overflow every resident cap at once:
- 10 volumes against 8 resident GI slots;
- 20 probes against 16 resident probe columns;
- 20 bodied lights, 960 cells, against the 768 the cached shadow pool holds.

Room PROP_ROOM holds two props its probes photograph: `hidden_prop`, an ordinary box a run hides
from captures by name, and `skinned_prop`, a box skinned to one joint, which captures leave out
without being asked. A thing that moves, frozen into a picture taken while the game runs, is
what both guard against.

The last room also has a light OUTSIDE it, just behind its back wall, whose reach crosses the
wall into the room. Shadowed, the wall stops it; unshadowed, it lights the room through the
wall. A GI capture taken before that light holds its shadow tiles keeps the leak for good, which
is what the capture's wait for its lights exists to prevent -- and a room lit only by its own
lights cannot show it, since inside a closed room an unshadowed light lights what a shadowed one
does.

The last two rooms are TWINS: the same colour and the same lights. The first has a grid aligned
with its interior; the second's grid is laid so its outermost probes sit inside the wall slabs,
which is what probe classification exists to switch off. The walls are slabs with thickness for
that reason: a probe inside one sees only its inner faces, all of them back faces.

Deterministic by construction. Regenerate with: python3 assets/generators/gen_stream_rooms.py
"""

import base64
import json
import math
import struct

from fixture_paths import asset_path, asset_ref

ROOMS = 10
ROOM_SPACING = 40.0
# Interior: x and z in [-HALF, HALF], y in [0, HEIGHT]. Every slab is WALL thick.
HALF = 2.0
HEIGHT = 3.0
WALL = 0.2

# The aligned grid: whole cells of 1 m over the interior, so every probe stands half a metre
# from the nearest face.
GRID_SPACING = 1.0
# The misaligned twin's grid: cells of 1.05 m centred on the room, whose outermost probes land at
# +/-2.1 on x and z, and at -0.1 and 3.05 on y -- inside the slabs, all of them.
MISALIGNED_SPACING = 1.05
MISALIGNED_HALF = 2.625
MISALIGNED_Y = (-0.625, 3.575)

WHITE = [0.72, 0.71, 0.69, 1.0]
FLOOR = [0.30, 0.29, 0.28, 1.0]
FLOOR_ROUGHNESS = 0.12
COLOURS = [
    [0.62, 0.09, 0.08, 1.0],  # red
    [0.11, 0.50, 0.13, 1.0],  # green
    [0.10, 0.16, 0.62, 1.0],  # blue
    [0.66, 0.58, 0.08, 1.0],  # yellow
    [0.58, 0.10, 0.52, 1.0],  # magenta
    [0.08, 0.52, 0.58, 1.0],  # cyan
    [0.70, 0.32, 0.06, 1.0],  # orange
    [0.34, 0.12, 0.56, 1.0],  # purple
    [0.10, 0.44, 0.36, 1.0],  # teal, the aligned twin
    [0.10, 0.44, 0.36, 1.0],  # teal, the misaligned twin
]
assert len(COLOURS) == ROOMS

PROP_ROOM = 3
PROP_HALF = 0.25
PROP_HEIGHT = 0.8

LIGHT_LUMENS = 60.0
LIGHT_RANGE = 7.0
LIGHT_BODY = 0.05


def room_x(k):
    return k * ROOM_SPACING


class Mesh:
    """Flat-shaded quads in one buffer, tagged by material group."""

    def __init__(self):
        self.positions = []
        self.normals = []
        self.indices = []
        self.groups = []  # [name, index_start, index_count]

    def begin(self, name):
        self.groups.append([name, len(self.indices), 0])

    def end(self):
        self.groups[-1][2] = len(self.indices) - self.groups[-1][1]

    def quad(self, a, b, c, d):
        """Two CCW triangles with one flat normal, wound so (b-a) x (d-a) faces out."""
        base = len(self.positions)
        ux = [b[i] - a[i] for i in range(3)]
        vx = [d[i] - a[i] for i in range(3)]
        n = [ux[1] * vx[2] - ux[2] * vx[1], ux[2] * vx[0] - ux[0] * vx[2],
             ux[0] * vx[1] - ux[1] * vx[0]]
        ln = math.sqrt(sum(x * x for x in n)) or 1.0
        n = [x / ln for x in n]
        for p in (a, b, c, d):
            self.positions.append(p)
            self.normals.append(n)
        self.indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])

    def box(self, lo, hi):
        """An axis-aligned solid with all six faces wound outward."""
        x0, y0, z0 = lo
        x1, y1, z1 = hi
        self.quad((x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0))  # +y
        self.quad((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1))  # -y
        self.quad((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1))  # +z
        self.quad((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0))  # -z
        self.quad((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1))  # +x
        self.quad((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0))  # -x


def build():
    m = Mesh()
    materials = {}
    o = HALF + WALL
    for k in range(ROOMS):
        x = room_x(k)
        # The coloured pair: the back wall the camera faces and the left wall.
        m.begin(f"room{k}_colour")
        m.box((x - o, -WALL, -o), (x + o, HEIGHT + WALL, -HALF))
        m.box((x - o, -WALL, -HALF), (x - HALF, HEIGHT + WALL, HALF))
        m.end()
        materials[f"room{k}_colour"] = (COLOURS[k], 1.0)

        m.begin(f"room{k}_white")
        m.box((x - o, -WALL, HALF), (x + o, HEIGHT + WALL, o))       # front
        m.box((x + HALF, -WALL, -HALF), (x + o, HEIGHT + WALL, HALF))  # right
        m.box((x - HALF, HEIGHT, -HALF), (x + HALF, HEIGHT + WALL, HALF))  # ceiling
        m.end()
        materials[f"room{k}_white"] = (WHITE, 1.0)

        m.begin(f"room{k}_floor")
        m.box((x - HALF, -WALL, -HALF), (x + HALF, 0.0, HALF))
        m.end()
        materials[f"room{k}_floor"] = (FLOOR, FLOOR_ROUGHNESS)

        if k == PROP_ROOM:
            for name, px in (("hidden_prop", x + 1.2), ("skinned_prop", x - 1.2)):
                m.begin(name)
                m.box((px - PROP_HALF, 0.0, -1.2 - PROP_HALF),
                      (px + PROP_HALF, PROP_HEIGHT, -1.2 + PROP_HALF))
                m.end()
                materials[name] = (WHITE, 1.0)
    return m, materials


def emit_gltf(mesh, materials):
    pos_bytes = b"".join(struct.pack("<3f", *p) for p in mesh.positions)
    nrm_bytes = b"".join(struct.pack("<3f", *n) for n in mesh.normals)
    idx_bytes = b"".join(struct.pack("<I", i) for i in mesh.indices)
    # The skin: every vertex on joint 0 at full weight, under an identity inverse bind, so the
    # skinned prop stands where it was modelled.
    joint_bytes = b"".join(struct.pack("<4H", 0, 0, 0, 0) for _ in mesh.positions)
    weight_bytes = b"".join(struct.pack("<4f", 1.0, 0.0, 0.0, 0.0) for _ in mesh.positions)
    ibm_bytes = struct.pack("<16f", *[1.0 if r == c else 0.0 for c in range(4) for r in range(4)])
    views_bytes = [pos_bytes, nrm_bytes, idx_bytes, joint_bytes, weight_bytes, ibm_bytes]
    buffer_bytes = b"".join(views_bytes)
    mn = [min(p[i] for p in mesh.positions) for i in range(3)]
    mx = [max(p[i] for p in mesh.positions) for i in range(3)]

    accessors = [
        {"bufferView": 0, "componentType": 5126, "count": len(mesh.positions), "type": "VEC3",
         "min": mn, "max": mx},
        {"bufferView": 1, "componentType": 5126, "count": len(mesh.normals), "type": "VEC3"},
    ]
    for _, start, count in mesh.groups:
        accessors.append({"bufferView": 2, "byteOffset": start * 4, "componentType": 5125,
                          "count": count, "type": "SCALAR"})
    joints_acc = len(accessors)
    accessors.append({"bufferView": 3, "componentType": 5123, "count": len(mesh.positions),
                      "type": "VEC4"})
    weights_acc = len(accessors)
    accessors.append({"bufferView": 4, "componentType": 5126, "count": len(mesh.positions),
                      "type": "VEC4"})
    ibm_acc = len(accessors)
    accessors.append({"bufferView": 5, "componentType": 5126, "count": 1, "type": "MAT4"})

    names = [g[0] for g in mesh.groups]
    joint_node = len(names)

    def primitive(i, n):
        attributes = {"POSITION": 0, "NORMAL": 1}
        if n == "skinned_prop":
            attributes.update({"JOINTS_0": joints_acc, "WEIGHTS_0": weights_acc})
        return {"attributes": attributes, "indices": 2 + i, "material": i}

    nodes = []
    for i, n in enumerate(names):
        node = {"name": n, "mesh": i}
        if n == "skinned_prop":
            node["skin"] = 0
        nodes.append(node)
    nodes.append({"name": "skinned_prop_joint"})
    offsets, at = [], 0
    for v in views_bytes:
        offsets.append(at)
        at += len(v)
    gltf = {
        "asset": {"version": "2.0", "generator": "gen_stream_rooms.py"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(names) + 1))}],
        "nodes": nodes,
        "skins": [{"joints": [joint_node], "inverseBindMatrices": ibm_acc}],
        "meshes": [{"name": n, "primitives": [primitive(i, n)]} for i, n in enumerate(names)],
        "materials": [{"name": n, "pbrMetallicRoughness": {
            "baseColorFactor": materials[n][0], "metallicFactor": 0.0,
            "roughnessFactor": materials[n][1]}} for n in names],
        "accessors": accessors,
        "bufferViews": [
            {"buffer": 0, "byteOffset": offsets[0], "byteLength": len(pos_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": offsets[1], "byteLength": len(nrm_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": offsets[2], "byteLength": len(idx_bytes), "target": 34963},
            {"buffer": 0, "byteOffset": offsets[3], "byteLength": len(joint_bytes),
             "target": 34962},
            {"buffer": 0, "byteOffset": offsets[4], "byteLength": len(weight_bytes),
             "target": 34962},
            {"buffer": 0, "byteOffset": offsets[5], "byteLength": len(ibm_bytes)},
        ],
        "buffers": [{"uri": "data:application/octet-stream;base64,"
                     + base64.b64encode(buffer_bytes).decode("ascii"),
                     "byteLength": len(buffer_bytes)}],
    }
    with open(asset_path("stream_rooms.gltf"), "w") as f:
        json.dump(gltf, f, indent=1)
        f.write("\n")


def light(name, pos):
    return {"name": name, "type": "point", "position": pos, "color": [1.0, 0.92, 0.82],
            "intensity": LIGHT_LUMENS, "intensity_unit": "lumens", "range": LIGHT_RANGE,
            "cast_shadows": True, "shadow_cache": True, "source_radius": LIGHT_BODY}


def emit_cscn():
    lights, probes, volumes = [], [], []
    for k in range(ROOMS):
        x = room_x(k)
        lights.append(light(f"Room{k}A", [x - 0.8, 2.6, -0.6]))
        lights.append(light(f"Room{k}B", [x + 1.0, 1.2, 0.9]))
        if k == ROOMS - 1:
            lights.append(light(f"Room{k}Behind", [x, 1.5, -HALF - WALL - 0.4]))
        # Two probes a room, its halves, overlapping at the middle.
        for side, (lo, hi) in enumerate(((x - HALF, x + 0.25), (x - 0.25, x + HALF))):
            probes.append({"position": [0.5 * (lo + hi), 1.5, 0.0],
                           "boxMin": [lo, 0.0, -HALF], "boxMax": [hi, HEIGHT, HALF],
                           "boxFade": 0.05})
        if k == ROOMS - 1:
            volumes.append({"boxMin": [x - MISALIGNED_HALF, MISALIGNED_Y[0], -MISALIGNED_HALF],
                            "boxMax": [x + MISALIGNED_HALF, MISALIGNED_Y[1], MISALIGNED_HALF],
                            "spacing": MISALIGNED_SPACING})
        else:
            volumes.append({"boxMin": [x - HALF, 0.0, -HALF], "boxMax": [x + HALF, HEIGHT, HALF],
                            "spacing": GRID_SPACING})
    cscn = {
        "version": 1,
        "models": [{"path": asset_ref("stream_rooms.gltf")}],
        "_comment_environment": "A probe cannot be created without a precomputed IBL. The sun "
                                "is below the horizon: the sky is wanted as the thing a probe "
                                "inside a wall sees through it, not as a light.",
        "environment": {"mode": "sky", "sun": {"elevation": -10.0, "azimuth": 0.0}},
        "lights": lights,
        "probes": probes,
        "giVolumes": volumes,
        "camera": {"eye": [0.0, 1.6, 1.8], "target": [0.0, 1.0, -HALF], "fov": 70},
        "post": {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False},
    }
    with open(asset_path("stream_rooms.cscn"), "w") as f:
        json.dump(cscn, f, indent=1)
        f.write("\n")


mesh, materials = build()
emit_gltf(mesh, materials)
emit_cscn()
print("wrote stream_rooms.gltf and stream_rooms.cscn:", ROOMS, "rooms,", len(mesh.positions),
      "verts,", len(mesh.indices) // 3, "tris")
