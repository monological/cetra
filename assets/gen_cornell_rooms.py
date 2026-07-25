#!/usr/bin/env python3
"""Generate the Cornell test assets: cornell_box.gltf and cornell_leak.gltf for
indirect diffuse (spec 9.7), cornell_point.gltf for punctual shadows (spec 9.8).

One room, three variants, one generator -- and each ships as a `.gltf` with a
sibling `.cscn` of the same stem, which is what lets every one of them run as
`render -m assets/<name>.gltf` with no second path to remember. Between them
they cover a different light type each: area panel, spot, point.

There is deliberately no directional variant. Directional shadows are the one
kind already pinned by committed goldens -- contact_debug, froxel_fog and
aerial_fixture all run on a cascaded sun -- so a fourth room would repeat that
coverage rather than add any, and a sun inside a closed box is a poor test of a
cascade fit that is sized from the whole scene.

**cornell_box** is the canonical Cornell box: a white room open at the front, with
one red wall and one green wall, two boxes, and an emissive ceiling panel. It
exists because every other fixture in this repo is a product shot on an open
backdrop, and none of them can show what A4 does -- a flat direction-only
irradiance lookup produces the same ambient everywhere, so a red wall cannot tint
the floor beside it.

The red/green walls are the whole point. Colour bleed onto the white floor and the
box facing them is the single clearest indirect-diffuse signal there is: it cannot
be faked by an environment map, and its absence is equally unmistakable. That makes
this fixture readable as a before/after rather than only as an after.

**cornell_leak** is the same room cut in half by a thin partition, lit on one side
only. It tests the opposite property: that light does NOT arrive where it has no
path. A probe grid has no idea walls exist -- trilinear weights alone will happily
blend a probe from the lit half into a surface in the dark half, and the partition
is deliberately thinner than a grid cell so probes straddle it. Only the visibility
moments (the Chebyshev test) can reject that, so this fixture is the one that fails
loudly if they are wrong, missing, or mis-encoded. Its sibling .cscn lights it with a
shadow-casting SPOT, not the area panel the Cornell box uses: a leak test has to start
from direct lighting that is beyond question, and the LTC panel currently lights
surfaces whose normal points away from it (an A2 defect, reproducible with
--area-light and nothing else, and enough on its own to make the dark half brighter
than the lit one). That also makes this the only fixture in the repo exercising a
punctual shadow map, so it doubles as the spot-occlusion test.

**cornell_point** is the same room with the emissive quad removed, lit by a point
light standing in the open. A point light's shadow is six 2D faces picked by
dominant axis, so its failure mode is a seam along a 45-degree face boundary; the
light sits low enough that those boundaries cross open floor, where a seam would
be visible, rather than hiding in the wall/floor corners.

Lighting for the Cornell box is an area panel authored in its sibling .cscn (glTF
punctual lights carry no rectangle), matching the emissive quad here so the visible
source and the analytic light agree. Emissive comes from the glTF because .cscn materials express
only sss and windResponse. Direct light from that panel lights the floor and the
box tops; what only GI can supply is the colour bleed, the fill in the corners, and
the light under and behind the boxes.

Geometry is flat-shaded quads: each face owns its four vertices and one normal, so
the interior corners stay hard and no normal is shared across a colour boundary.
Deterministic by construction. Regenerate with: python3 assets/gen_cornell_rooms.py
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

# Mid-grey rather than white: a true-white Cornell box clips the moment any bounce
# is added, and a clipped floor cannot show a colour tint.
WHITE = [0.72, 0.71, 0.69, 1.0]
RED = [0.62, 0.09, 0.08, 1.0]
GREEN = [0.11, 0.50, 0.13, 1.0]


class Room:
    """Accumulates flat-shaded quads into one buffer, tagged by material group."""

    def __init__(self):
        self.positions = []
        self.normals = []
        self.indices = []
        self.groups = []  # [name, index_start, index_count]

    def quad(self, a, b, c, d):
        """Two CCW triangles with one flat normal, wound so (b-a) x (d-a) faces out."""
        base = len(self.positions)
        ux = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        vx = (d[0] - a[0], d[1] - a[1], d[2] - a[2])
        nx = ux[1] * vx[2] - ux[2] * vx[1]
        ny = ux[2] * vx[0] - ux[0] * vx[2]
        nz = ux[0] * vx[1] - ux[1] * vx[0]
        ln = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
        n = (nx / ln, ny / ln, nz / ln)
        for p in (a, b, c, d):
            self.positions.append(p)
            self.normals.append(n)
        self.indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])

    def box(self, cx, cz, w, h, d, yaw_deg):
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
        # Outward-facing, matching the winding rule in quad().
        self.quad(v(-hw, hi_y, hd), v(hw, hi_y, hd), v(hw, hi_y, -hd), v(-hw, hi_y, -hd))   # top
        self.quad(v(-hw, lo_y, hd), v(hw, lo_y, hd), v(hw, hi_y, hd), v(-hw, hi_y, hd))     # +z
        self.quad(v(hw, lo_y, -hd), v(-hw, lo_y, -hd), v(-hw, hi_y, -hd), v(hw, hi_y, -hd)) # -z
        self.quad(v(hw, lo_y, hd), v(hw, lo_y, -hd), v(hw, hi_y, -hd), v(hw, hi_y, hd))     # +x
        self.quad(v(-hw, lo_y, -hd), v(-hw, lo_y, hd), v(-hw, hi_y, hd), v(-hw, hi_y, -hd)) # -x
        # No bottom face: it sits on the floor and would only ever z-fight.

    def begin(self, name):
        self.groups.append([name, len(self.indices), 0])

    def end(self):
        self.groups[-1][2] = len(self.indices) - self.groups[-1][1]

    def shell(self):
        """Floor, ceiling and back wall. Normals face INTO the room; the front
        (z = +1) is left open so the camera looks in, which is what makes the
        interior readable at all."""
        self.begin("cornell_shell")
        self.quad((LO, 0.0, HI), (HI, 0.0, HI), (HI, 0.0, LO), (LO, 0.0, LO))  # floor  +Y
        self.quad((LO, TOP, LO), (HI, TOP, LO), (HI, TOP, HI), (LO, TOP, HI))  # ceil   -Y
        self.quad((LO, 0.0, LO), (HI, 0.0, LO), (HI, TOP, LO), (LO, TOP, LO))  # back   +Z
        self.end()

    def panel(self, cx):
        """The emissive source, just under the ceiling so it reads as a source
        rather than z-fighting with it. The .cscn area light mirrors it."""
        self.begin("cornell_light")
        self.quad((cx - 0.35, TOP - 0.02, -0.35), (cx + 0.35, TOP - 0.02, -0.35),
                  (cx + 0.35, TOP - 0.02, 0.35), (cx - 0.35, TOP - 0.02, 0.35))
        self.end()


def build_box_room(with_panel=True):
    r = Room()
    r.shell()

    r.begin("cornell_left_red")
    r.quad((LO, 0.0, HI), (LO, 0.0, LO), (LO, TOP, LO), (LO, TOP, HI))  # +X
    r.end()

    r.begin("cornell_right_green")
    r.quad((HI, 0.0, LO), (HI, 0.0, HI), (HI, TOP, HI), (HI, TOP, LO))  # -X
    r.end()

    # The tall box sits toward the red wall and the short one toward the green,
    # so each picks up a different bounce colour on its inward face.
    r.begin("cornell_tall_box")
    r.box(-0.36, -0.32, 0.6, 1.2, 0.6, 17.0)
    r.end()

    r.begin("cornell_short_box")
    r.box(0.38, 0.34, 0.6, 0.6, 0.6, -18.0)
    r.end()

    if with_panel:
        r.panel(0.0)
    return r, {"cornell_shell": WHITE, "cornell_left_red": RED,
               "cornell_right_green": GREEN, "cornell_tall_box": WHITE,
               "cornell_short_box": WHITE, "cornell_light": WHITE}


def build_point_room():
    """The box room with no emissive quad. Its .cscn lights it with a POINT
    light standing in the open, low enough that the 45-degree planes dividing
    the six shadow faces fall across open floor rather than hiding in the
    wall/floor corners -- those junctions are what a dominant-axis face
    selection gets wrong, so the fixture has to show them."""
    return build_box_room(with_panel=False)


# Thinner than one cell of the default 8x4x8 grid (0.25 m), so probes straddle it
# and the trilinear weights alone cannot keep the halves apart. That is the point:
# a thicker wall would pass even with no visibility test at all.
PARTITION_HALF = 0.03

# How far the partition pokes through the surfaces it meets, so no silhouette
# edge of the room's only shadow caster is coincident with a receiver junction.
# Large enough to clear a shadow texel at this scale, small enough to stay
# hidden inside the wall it passes through.
EMBED = 0.05


def build_leak_room():
    r = Room()
    r.shell()

    # Plain side walls -- no red/green here. A colour cast would make it ambiguous
    # whether the dark half is glowing or just tinted.
    r.begin("cornell_left_red")
    r.quad((LO, 0.0, HI), (LO, 0.0, LO), (LO, TOP, LO), (LO, TOP, HI))
    r.quad((HI, 0.0, LO), (HI, 0.0, HI), (HI, TOP, HI), (HI, TOP, LO))
    r.end()

    # The partition: a slab at x = 0 spanning the full height and depth. Both
    # faces are modelled (it is a wall, not a plane) so a probe inside the dark
    # half sees a near surface in the direction of the lit half, which is exactly
    # what the visibility moments have to encode.
    r.begin("cornell_tall_box")
    p = PARTITION_HALF
    # EMBEDDED, not merely touching. The shadow pass culls front faces, so from a
    # light inside the room every wall is culled and the partition is the only
    # occluder in the map. Ending it exactly at the floor, ceiling and back wall
    # would put its silhouette precisely on those junctions, and one texel of
    # shadow-map quantization there is one texel of light leaking along the
    # corner -- a bright staircase no filter width or map resolution can remove,
    # because the error is pinned to the edge rather than spread around it.
    # Poking through by a margin moves the silhouette inside solid geometry.
    lo_y, hi_y, back = -EMBED, TOP + EMBED, LO - EMBED
    # Wound like the room's own side walls but mirrored: the face at x = -p must
    # look OUT into the left half and the one at x = +p into the right. Wound the
    # other way both faces point into the slab, get backface-culled, and the
    # partition stops existing for the renderer -- which does not merely weaken
    # the test, it inverts it, since each half's probes then see the other half.
    r.quad((-p, lo_y, back), (-p, lo_y, HI), (-p, hi_y, HI), (-p, hi_y, back))  # -X
    r.quad((p, lo_y, HI), (p, lo_y, back), (p, hi_y, back), (p, hi_y, HI))      # +X
    r.quad((-p, lo_y, HI), (p, lo_y, HI), (p, hi_y, HI), (-p, hi_y, HI))        # front edge, +Z
    r.end()

    # Light over the LEFT half only. The right half has no path to it except
    # around the open front, which the camera sees past.
    r.panel(-0.5)
    return r, {"cornell_shell": WHITE, "cornell_left_red": WHITE,
               "cornell_tall_box": WHITE, "cornell_light": WHITE}


def emit(filename, room, mat_by_group):
    pos_bytes = b"".join(struct.pack("<3f", *p) for p in room.positions)
    nrm_bytes = b"".join(struct.pack("<3f", *n) for n in room.normals)
    idx_bytes = b"".join(struct.pack("<I", i) for i in room.indices)
    buffer_bytes = pos_bytes + nrm_bytes + idx_bytes

    mn = [min(p[i] for p in room.positions) for i in range(3)]
    mx = [max(p[i] for p in room.positions) for i in range(3)]

    materials = []
    for name, _, _ in room.groups:
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
        {"bufferView": 0, "componentType": 5126, "count": len(room.positions), "type": "VEC3",
         "min": mn, "max": mx},
        {"bufferView": 1, "componentType": 5126, "count": len(room.normals), "type": "VEC3"},
    ]
    for _, start, count in room.groups:
        accessors.append({"bufferView": 2, "byteOffset": start * 4, "componentType": 5125,
                          "count": count, "type": "SCALAR"})

    gltf = {
        "asset": {"version": "2.0", "generator": "gen_cornell_rooms.py"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(room.groups)))}],
        "nodes": [{"name": n, "mesh": i} for i, (n, _, _) in enumerate(room.groups)],
        "meshes": [
            {"name": n, "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1},
                                        "indices": 2 + i, "material": i}]}
            for i, (n, _, _) in enumerate(room.groups)
        ],
        "materials": materials,
        "accessors": accessors,
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(nrm_bytes),
             "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bytes) + len(nrm_bytes),
             "byteLength": len(idx_bytes), "target": 34963},
        ],
        "buffers": [
            {"uri": "data:application/octet-stream;base64,"
             + base64.b64encode(buffer_bytes).decode("ascii"),
             "byteLength": len(buffer_bytes)},
        ],
    }

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), filename)
    with open(out, "w") as f:
        json.dump(gltf, f, indent=1)
        f.write("\n")
    print("wrote", out, "(", len(room.positions), "verts,", len(room.indices) // 3, "tris,",
          len(room.groups), "draws )")


emit("cornell_box.gltf", *build_box_room())
emit("cornell_leak.gltf", *build_leak_room())
emit("cornell_point.gltf", *build_point_room())
