#!/usr/bin/env python3
"""Generate assets/cornell_box.gltf, the indirect-diffuse test asset (spec 9.7).

The canonical Cornell box: a white room open at the front, with one red wall and
one green wall, two boxes, and an emissive ceiling panel. It exists because every
other fixture in this repo is a product shot on an open backdrop, and none of them
can show what A4 does -- a flat direction-only irradiance lookup produces the same
ambient everywhere, so a red wall cannot tint the floor beside it.

The red/green walls are the whole point. Colour bleed onto the white floor and the
box facing them is the single clearest indirect-diffuse signal there is: it cannot
be faked by an environment map, and its absence is equally unmistakable. That makes
this fixture readable as a before/after rather than only as an after.

Lighting is an area panel authored in the sibling .cscn (glTF punctual lights carry
no rectangle), matching the emissive quad here so the visible source and the
analytic light agree. Emissive comes from the glTF because .cscn materials express
only sss and windResponse. Direct light from that panel lights the floor and the
box tops; what only GI can supply is the colour bleed, the fill in the corners, and
the light under and behind the boxes.

Geometry is flat-shaded quads: each face owns its four vertices and one normal, so
the interior corners stay hard and no normal is shared across a colour boundary.
Deterministic by construction. Regenerate with: python3 assets/gen_cornell_box.py
"""

import base64
import json
import math
import os
import struct

# Interior spans x,z in [-1,1] and y in [0,2] -- a 2 m room at the engine's prop
# scale, so the app's scene-scaled near plane and shadow cascades behave as they
# do for every other fixture.
LO, HI = -1.0, 1.0
TOP = 2.0

positions = []
normals = []
indices = []


def add_quad(a, b, c, d):
    """Two CCW triangles with one flat normal, wound so (b-a) x (d-a) faces out."""
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


def add_box(cx, cz, w, h, d, yaw_deg):
    """An axis-aligned box of w*h*d standing on y=0, centred at (cx, cz) in the
    floor plane and yawed about its own centre. The rotation is baked into the
    vertices rather than carried on a glTF node so the whole room stays one
    buffer and one draw per material."""
    s, c = math.sin(math.radians(yaw_deg)), math.cos(math.radians(yaw_deg))
    hw, hd = w * 0.5, d * 0.5

    def v(x, y, z):
        # Rotate in the floor plane, then place.
        return (cx + x * c - z * s, y, cz + x * s + z * c)

    lo_y, hi_y = 0.0, h
    # Outward-facing, matching the winding rule in add_quad.
    add_quad(v(-hw, hi_y, hd), v(hw, hi_y, hd), v(hw, hi_y, -hd), v(-hw, hi_y, -hd))   # top
    add_quad(v(-hw, lo_y, hd), v(hw, lo_y, hd), v(hw, hi_y, hd), v(-hw, hi_y, hd))     # +z
    add_quad(v(hw, lo_y, -hd), v(-hw, lo_y, -hd), v(-hw, hi_y, -hd), v(hw, hi_y, -hd)) # -z
    add_quad(v(hw, lo_y, hd), v(hw, lo_y, -hd), v(hw, hi_y, -hd), v(hw, hi_y, hd))     # +x
    add_quad(v(-hw, lo_y, -hd), v(-hw, lo_y, hd), v(-hw, hi_y, hd), v(-hw, hi_y, -hd)) # -x
    # No bottom face: it sits on the floor and would only ever z-fight.


# Each group is a contiguous index range -> one mesh, one material, one draw.
groups = []


def begin(name):
    groups.append([name, len(indices), 0])


def end():
    groups[-1][2] = len(indices) - groups[-1][1]


# --- Room shell. Normals face INTO the room; the front (z = +1) is left open
# --- so the camera looks in, which is what makes the box readable at all.
begin("cornell_shell")
add_quad((LO, 0.0, HI), (HI, 0.0, HI), (HI, 0.0, LO), (LO, 0.0, LO))       # floor  +Y
add_quad((LO, TOP, LO), (HI, TOP, LO), (HI, TOP, HI), (LO, TOP, HI))       # ceil   -Y
add_quad((LO, 0.0, LO), (HI, 0.0, LO), (HI, TOP, LO), (LO, TOP, LO))       # back   +Z
end()

begin("cornell_left_red")
add_quad((LO, 0.0, HI), (LO, 0.0, LO), (LO, TOP, LO), (LO, TOP, HI))       # +X
end()

begin("cornell_right_green")
add_quad((HI, 0.0, LO), (HI, 0.0, HI), (HI, TOP, HI), (HI, TOP, LO))       # -X
end()

# --- Contents. The tall box sits toward the red wall and the short one toward
# --- the green, so each picks up a different bounce colour on its inward face.
begin("cornell_tall_box")
add_box(-0.36, -0.32, 0.6, 1.2, 0.6, 17.0)
end()

begin("cornell_short_box")
add_box(0.38, 0.34, 0.6, 0.6, 0.6, -18.0)
end()

# --- Emissive panel, just under the ceiling so it reads as a source rather than
# --- z-fighting with it. The .cscn area light mirrors its position and size.
begin("cornell_light")
add_quad((-0.35, TOP - 0.02, -0.35), (0.35, TOP - 0.02, -0.35),
         (0.35, TOP - 0.02, 0.35), (-0.35, TOP - 0.02, 0.35))
end()

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
idx_bytes = b"".join(struct.pack("<I", i) for i in indices)
buffer_bytes = pos_bytes + nrm_bytes + idx_bytes

mn = [min(p[i] for p in positions) for i in range(3)]
mx = [max(p[i] for p in positions) for i in range(3)]

# Mid-grey rather than white: a true-white Cornell box clips the moment any bounce
# is added, and a clipped floor cannot show a colour tint.
WHITE = [0.72, 0.71, 0.69, 1.0]
RED = [0.62, 0.09, 0.08, 1.0]
GREEN = [0.11, 0.50, 0.13, 1.0]

mat_by_group = {
    "cornell_shell": WHITE,
    "cornell_left_red": RED,
    "cornell_right_green": GREEN,
    "cornell_tall_box": WHITE,
    "cornell_short_box": WHITE,
    "cornell_light": WHITE,
}

materials = []
for name, _, _ in groups:
    m = {
        "name": name,
        "pbrMetallicRoughness": {
            "baseColorFactor": mat_by_group[name],
            "metallicFactor": 0.0,
            # Fully rough: a Cornell box is a diffuse test, and any specular
            # response would confound the thing being measured.
            "roughnessFactor": 1.0,
        },
    }
    if name == "cornell_light":
        m["emissiveFactor"] = [1.0, 0.95, 0.88]
    materials.append(m)

# Index accessors share one bufferView, offset per group.
accessors = [
    {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
     "min": mn, "max": mx},
    {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
]
for _, start, count in groups:
    accessors.append({"bufferView": 2, "byteOffset": start * 4, "componentType": 5125,
                      "count": count, "type": "SCALAR"})

gltf = {
    "asset": {"version": "2.0", "generator": "gen_cornell_box.py"},
    "scene": 0,
    "scenes": [{"nodes": list(range(len(groups)))}],
    "nodes": [{"name": n, "mesh": i} for i, (n, _, _) in enumerate(groups)],
    "meshes": [
        {"name": n, "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1},
                                    "indices": 2 + i, "material": i}]}
        for i, (n, _, _) in enumerate(groups)
    ],
    "materials": materials,
    "accessors": accessors,
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(nrm_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes) + len(nrm_bytes), "byteLength": len(idx_bytes),
         "target": 34963},
    ],
    "buffers": [
        {"uri": "data:application/octet-stream;base64,"
         + base64.b64encode(buffer_bytes).decode("ascii"),
         "byteLength": len(buffer_bytes)},
    ],
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cornell_box.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote", out, "(", len(positions), "verts,", len(indices) // 3, "tris,",
      len(groups), "draws )")
