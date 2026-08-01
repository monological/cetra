#!/usr/bin/env python3
"""Generate assets/dir_shadow_fixture.gltf -- the cascade-shadow instrument (spec 10.5).

area_shadow_fixture pins the punctual projection; this one pins the DIRECTIONAL
cascade path. Its subjects were chosen by measurement, not by argument, because
the first two guesses did NOT reproduce the defect:

  - a smooth 48x96 sphere floating at h=1.5 renders a clean umbra under the
    broken far-side policy;
  - the same sphere at rock tessellation (18x24) is STILL clean;
  - an 18x24 sphere RESTING on the ground loses its shadow almost entirely
    (umbra centre reads 0.45 against a 0.57 lit ground -- ~70 percent lit).

Contact is the load-bearing ingredient, exactly as spec 10.3 measured on the
punctual path: near the tangency the caster's far side is silhouette-adjacent
sliver geometry rasterizing at grazing incidence, and what little it stores is
nearly coplanar with the receiver. So the fixture carries BOTH sphere
configurations: the resting one is the defect subject, the floating one is the
clean-geometry control whose edge position the gate measures.

The answers are closed-form. A sun at elevation E (azimuth 0, everything
axis-aligned in world) travelling along (0, -sin E, -cos E) throws a sphere of
radius r centred at (cx, h, 0) onto the ground as the ellipse

    centre  (cx, 0, -h / tan E)
    semi-axis across the sun (x):  r
    semi-axis along the sun  (z):  r / sin E

Floating sphere (0, 1.5, 0), E=40: centre z = -1.7876, axes 0.5 x 0.7779.
Resting sphere (1.6, 0.5, 0), E=40: centre z = -0.5959, same axes (the
tangency sits ON the ellipse boundary's +Z side; the closed form holds).
Points inside the eroded ellipses must be umbra; a scan through the floating
sphere's near edge must cross 50 percent at z = centre + semi_z; ground far
from every shadow must be uniformly lit. gates.py asserts all three.

The PILLAR is for the min-walk (spec 10.5 phase 3): a tall thin box whose
shadow band crosses the cascade-0/1 split at the gate camera, the geometry
that motivated the cascade union in c597ac7 (a caster clipped from a tight
inner cascade while its receiver stays covered). It sits at x=-3.5 so its
band never touches the ellipses or the acne strip at either authored sun.

Two hand-authored .cscn siblings light this file (an authored directional,
NOT an environment sky, so there is no IBL ambient and the umbra is genuinely
black): dir_shadow_fixture.cscn at 40 degrees, dir_shadow_fixture_lowsun.cscn
at 10 degrees. Low sun is the regime that reverted the last attempt at
near-side cascades (shadow.c's history) -- grazing ground is where a bias
scheme fails first, so it gets its own acne measurement.

Regenerate with:
  python3 assets/gen_dir_shadow_fixture.py
(the .cscn files are hand-authored, not generated.)
"""

import base64
import json
import math
import os
import struct

GROUND = 6.0        # ground half-extent
SPHERE_R = 0.5      # radius of both spheres          (r)
FLOAT_X = 0.0       # floating sphere: clean-edge control
FLOAT_H = 1.5
REST_X = 1.6        # resting sphere: the defect subject (tangent to ground)
REST_H = 0.5
PILLAR_X = -3.5     # pillar centre, x
PILLAR_HALF = 0.15  # pillar half-width
PILLAR_H = 3.0      # pillar height; tall enough that its 40-degree shadow band
                    # crosses the cascade-0/1 split at the gate camera with margin

# Deliberately coarse -- matching the contact rocks (18x24), whose facet-scale
# far-side slivers are what garble a far-side depth map. A 48x96 sphere renders
# a clean umbra even under the broken policy (measured), so a smooth sphere
# cannot arbitrate the fix. The silhouette chord error at this tessellation is
# r*(1-cos(pi/SECTORS)) ~ 4mm, well inside the gate's erode margin.
STACKS = 18
SECTORS = 24

positions, normals, uvs, indices = [], [], [], []


def add_quad(y, half, index_list):
    """A horizontal quad in XZ at height `y`, facing +Y.

    Index order (0,2,1)/(0,3,2), not the natural-looking (0,1,2)/(0,2,3):
    walking the listed corners is CLOCKWISE seen from above, so the natural
    order faces the quad DOWN and culling empties the frame while the declared
    NORMAL still claims +Y (the area fixture's comment, kept because the trap
    already shipped once).
    """
    base = len(positions)
    for (x, z) in ((-half, -half), (half, -half), (half, half), (-half, half)):
        positions.append((x, y, z))
        normals.append((0.0, 1.0, 0.0))
    uvs.extend(((0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)))
    index_list.extend((base + 0, base + 2, base + 1, base + 0, base + 3, base + 2))


def add_sphere(cx, cy, cz, radius, index_list):
    """A UV sphere, CCW seen from OUTSIDE.

    The (a, a+1, b) order matters: under this phi/theta sweep the
    natural-looking (a, b, a+1) faces every triangle inward, and an inside-out
    sphere still reads as a plausible shaded disc (the defect the winding
    audit fixed across five generators).
    """
    base = len(positions)
    for i in range(STACKS + 1):
        phi = math.pi * i / STACKS
        for j in range(SECTORS + 1):
            theta = 2.0 * math.pi * j / SECTORS
            nx = math.sin(phi) * math.cos(theta)
            ny = math.cos(phi)
            nz = math.sin(phi) * math.sin(theta)
            positions.append((cx + radius * nx, cy + radius * ny, cz + radius * nz))
            normals.append((nx, ny, nz))
            uvs.append((j / SECTORS, i / STACKS))
    for i in range(STACKS):
        for j in range(SECTORS):
            a = base + i * (SECTORS + 1) + j
            b = a + SECTORS + 1
            index_list.extend((a, a + 1, b, a + 1, b + 1, b))


def add_box(cx, half, height, index_list):
    """An axis-aligned box from y=0 to y=height, centred at (cx, 0), CCW outside.

    Emitted as (0,2,1)/(0,3,2) for the same reason add_quad's comment gives:
    the corner lists read naturally but walk clockwise from outside, and the
    winding audit (not the eye) is what verifies the result.
    """
    x0, x1 = cx - half, cx + half
    z0, z1 = -half, half
    y0, y1 = 0.0, height
    # (corner list, outward normal) per face
    faces = [
        (((x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)), (0, 1, 0)),    # top
        (((x0, y0, z1), (x1, y0, z1), (x1, y0, z0), (x0, y0, z0)), (0, -1, 0)),   # bottom
        (((x0, y0, z1), (x0, y1, z1), (x1, y1, z1), (x1, y0, z1)), (0, 0, 1)),    # +Z
        (((x1, y0, z0), (x1, y1, z0), (x0, y1, z0), (x0, y0, z0)), (0, 0, -1)),   # -Z
        (((x1, y0, z1), (x1, y1, z1), (x1, y1, z0), (x1, y0, z0)), (1, 0, 0)),    # +X
        (((x0, y0, z0), (x0, y1, z0), (x0, y1, z1), (x0, y0, z1)), (-1, 0, 0)),   # -X
    ]
    for corners, n in faces:
        fb = len(positions)
        for c in corners:
            positions.append(c)
            normals.append(n)
        uvs.extend(((0.0, 0.0), (0.0, 1.0), (1.0, 1.0), (1.0, 0.0)))
        index_list.extend((fb + 0, fb + 2, fb + 1, fb + 0, fb + 3, fb + 2))


ground_indices = []
add_quad(0.0, GROUND, ground_indices)
float_indices = []
add_sphere(FLOAT_X, FLOAT_H, 0.0, SPHERE_R, float_indices)
rest_indices = []
add_sphere(REST_X, REST_H, 0.0, SPHERE_R, rest_indices)
pillar_indices = []
add_box(PILLAR_X, PILLAR_HALF, PILLAR_H, pillar_indices)
indices = ground_indices + float_indices + rest_indices + pillar_indices

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
uv_bytes = b"".join(struct.pack("<2f", *u) for u in uvs)
idx_bytes = b"".join(struct.pack("<I", i) for i in indices)
buffer_bytes = pos_bytes + nrm_bytes + uv_bytes + idx_bytes

all_min = [min(p[k] for p in positions) for k in range(3)]
all_max = [max(p[k] for p in positions) for k in range(3)]


def matte(name, value):
    # Fully rough and non-metallic: the measurement reads a diffuse term, and
    # any specular lobe on the ground would ride the same scanline as the edge.
    return {
        "name": name,
        "pbrMetallicRoughness": {
            "baseColorFactor": [value, value, value, 1.0],
            "metallicFactor": 0.0,
            "roughnessFactor": 1.0,
        },
    }


counts = [len(ground_indices), len(float_indices), len(rest_indices), len(pillar_indices)]
ofs = [sum(counts[:i]) for i in range(len(counts))]
NAMES = ("dir_ground", "dir_float_sphere", "dir_rest_sphere", "dir_pillar")

gltf = {
    "asset": {"version": "2.0", "generator": "gen_dir_shadow_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": list(range(len(NAMES)))}],
    "nodes": [{"name": name, "mesh": i} for i, name in enumerate(NAMES)],
    "meshes": [
        {"name": name,
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                         "indices": 3 + i, "material": i}]}
        for i, name in enumerate(NAMES)
    ],
    "materials": [matte("dir_ground", 0.55), matte("dir_float_sphere", 0.25),
                  matte("dir_rest_sphere", 0.25), matte("dir_pillar", 0.25)],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
         "min": all_min, "max": all_max},
        {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
    ] + [
        {"bufferView": 3, "byteOffset": ofs[i] * 4, "componentType": 5125,
         "count": counts[i], "type": "SCALAR"}
        for i in range(len(NAMES))
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

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dir_shadow_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote %s (floating + resting r=%.2f spheres + pillar over a %.0f-unit ground)"
      % (out, SPHERE_R, GROUND * 2))
