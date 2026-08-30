#!/usr/bin/env python3
"""Generate the occlusion-culling instruments for spec 11.98.

Nothing in the corpus stands behind anything: every fixture is either fully
visible or frustum-culled, so a culler that hides on-screen geometry -- or
hides nothing at all -- is green everywhere. These two scenes are the first
with real occlusion in them.

  occlusion_fixture   A portal room: four wall slabs framing a window, ~300
                      distinct heavy meshes behind the wall (occl_hidden_*),
                      three visible through the window (occl_door_*), and one
                      marginal mesh whose projection pokes into the window by a
                      couple of mask texels (occl_door_marginal). Camera outside.
                      Five .cscn variants from one .gltf:
                        occlusion_fixture.cscn        occluders:[] = 4 interior boxes
                        occlusion_fixture_bare.cscn   no occluders key at all --
                                                      the empty-context path no
                                                      flag can reach
                        occlusion_fixture_inside.cscn the same boxes with the
                                                      camera inside the room
                        occlusion_fixture_near.cscn   the same boxes with the
                                                      camera almost touching a
                                                      slab, so the near plane
                                                      clips the wall open
                        occlusion_fixture_mat.cscn    no boxes; the occl_wall
                                                      material carries occluder=on

  occlusion_scatter   The measured LOSS: one shared mesh instanced 24x in a
                      grid (one long instanced run), a sparse pillar fence
                      hiding every third column. Occlusion ON removes a third
                      of the triangles but splits the run -- draws go UP while
                      triangles go DOWN.

Arms served (the `occlusion` gate group):

  occl-hidden    meshes culled == the occl_hidden_* count, exactly
  occl-material  the _mat variant culls the same count through the material row
  occl-doorway   0 px vs --no-occlusion-cull with the marginal mesh in frame
  occl-inside    the _inside variant culls nothing and moves nothing
  occl-near      the _near variant culls nothing and moves nothing -- the
                 near-clipped wall shows the room, so a face that should not
                 stand (a back face, before classification) deletes it
  occl-bare      the _bare variant culls nothing and moves nothing
  occl-sum       seen == instances + culled on the profiled run
  occl-shadow    cascade submit rows identical on vs off
  occl-probe     the brute-force twin over the name classes this file defines
                 (occl_hidden_* / occl_door* hier-vs-ref, occl_wall proxies)
  occl-fragment  scatter: exactly the pillared instances culled, draws up,
                 triangles down
  occl-crossover the portal room's opaque-clock win

EVERY geometric property an arm depends on is asserted below rather than
trusted -- the shadow-lag fixture shipped with three of its six caster faces
wound inward and stayed green, which is why this file checks winding, margins,
containment and the recenter no-op instead of stating them in comments.

The camera is DERIVED and the scene is authored so the render app's model
recenter is a provable NO-OP (x/z bounds symmetric via the floor strip, min y
exactly 0): the authored occluders:[] boxes are world-space on the Scene and
would NOT follow a recenter shift, so the generator replicates the app's
bounds math and asserts the offset is zero.

Regenerate with:
  python3 assets/gen_occlusion_fixture.py
"""

import base64
import json
import math
import os
import struct

# The occlusion buffer's shape, restated rather than imported (the terrain
# gate's third-copy idiom): the gate must be able to fail when this file and
# the module disagree, and importing one into the other would make the margin
# a function of the thing it bounds. Must match occlusion.h.
OCC_BUF_W = 256
OCC_BUF_H = 144
OCC_TILE_W = 8
OCC_TILE_H = 4
TEXEL_X = 2.0 / OCC_BUF_W  # one mask texel, in NDC
TEXEL_Y = 2.0 / OCC_BUF_H
TILE_X = TEXEL_X * OCC_TILE_W  # one tile, in NDC
TILE_Y = TEXEL_Y * OCC_TILE_H

FOV_DEG = 45.0
ASPECT = 4.0 / 3.0  # the gate renders 400x300 and 800x600

# --- the portal room, all in "wall plane" coordinates -----------------------
# The camera sits at (0, CY, EYE_Z) looking at (0, CY, 0); the wall's front
# face is the z = 0 plane. A point's NDC is constant along its eye ray, so
# containment is asserted where it is legible: on the wall plane.
EYE_Z = 10.0
CY = 10.0  # eye height; high enough that the deepest mesh's bottom stays above y=0
HALF_H = EYE_Z * math.tan(math.radians(FOV_DEG) * 0.5)  # frustum half-height at the wall
HALF_W = HALF_H * ASPECT

WALL_THICK = 0.6
WALL_X = 6.5  # slab outer edge; past the frustum so band edges stay covered
WALL_Y = 4.6
WIN_X = 1.2  # the window half-extents
WIN_Y = 1.2
BOX_EPS = 0.02  # authored boxes are inset by this: strictly interior

LAYER_Z = (-8.0, -12.0, -16.0)  # the hidden meshes' depths, round-robin
BAND_COLS = 6
BAND_ROWS = 25
BAND_MARGIN = 0.75      # wall units from the window jamb and the band's start
NDC_LIMIT = 0.888       # hidden meshes stay inside this much of the frustum
MESH_HX = 0.18          # a hidden mesh's projected half-size on the wall plane
MESH_HY = 0.11

DOOR_XS = (-0.55, 0.0, 0.55)
DOOR_HALF = 0.22
DOOR_MARGIN = 0.30      # wall units from the jamb, both axes

MARGINAL_POKE = 0.10    # how far the marginal mesh's projection enters the window
MARGINAL_HX = 0.40
MARGINAL_HY = 0.35

GRID_N = 48             # the shared plane: GRID_N^2 quads, 2*GRID_N^2 triangles

# --- the scatter twin -------------------------------------------------------
SC_EYE_Z = 12.0
SC_CY = 2.2
SC_INST_Z = -6.0
SC_PILLAR_Z = -2.0
SC_PILLAR_THICK = 0.3
SC_COLS = 8
SC_ROWS = 3
SC_SPACING = 2.4        # world units between instance columns at SC_INST_Z
SC_HALF = 0.5           # instance half-size
SC_ROW_YS = (1.2, 2.2, 3.2)
SC_HIDDEN_STRIDE = 3    # every third column stands behind a pillar
SC_PILLAR_MARGIN_TILES = 2.0  # pillar coverage past the hidden projection
SC_VISIBLE_MIN_TEXELS = 2.0   # a visible neighbour pokes out by at least this


def ndc_x(wall_x):
    return wall_x / HALF_W


def ndc_y(wall_y):
    return (wall_y - CY) / HALF_H


def unproject(wall_c, z, eye_z, cy):
    """Wall-plane (x', y') to world at depth z, through the eye."""
    s = (eye_z - z) / eye_z
    return wall_c[0] * s, cy + (wall_c[1] - cy) * s, s


def plane_mesh_data(n):
    """A [-1,1]^2 grid facing +Z: positions, normals, uint16 indices."""
    pos, nrm = b"", b""
    for j in range(n + 1):
        for i in range(n + 1):
            x = -1.0 + 2.0 * i / n
            y = -1.0 + 2.0 * j / n
            pos += struct.pack("<3f", x, y, 0.0)
            nrm += struct.pack("<3f", 0.0, 0.0, 1.0)
    idx = b""
    for j in range(n):
        for i in range(n):
            a = j * (n + 1) + i
            b = a + 1
            c = a + (n + 1)
            d = c + 1
            # Counter-clockwise seen from +Z, the side the camera is on.
            idx += struct.pack("<6H", a, b, d, a, d, c)
    return pos, nrm, idx, (n + 1) * (n + 1), 6 * n * n


def box_mesh_data(hx, hy, hz):
    """An axis-aligned box with per-face normals, all faces wound OUTWARD."""
    faces = [
        ((+1, 0, 0), [(+1, -1, -1), (+1, +1, -1), (+1, +1, +1), (+1, -1, +1)]),
        ((-1, 0, 0), [(-1, -1, +1), (-1, +1, +1), (-1, +1, -1), (-1, -1, -1)]),
        ((0, +1, 0), [(-1, +1, -1), (-1, +1, +1), (+1, +1, +1), (+1, +1, -1)]),
        ((0, -1, 0), [(-1, -1, +1), (-1, -1, -1), (+1, -1, -1), (+1, -1, +1)]),
        ((0, 0, +1), [(-1, -1, +1), (+1, -1, +1), (+1, +1, +1), (-1, +1, +1)]),
        ((0, 0, -1), [(+1, -1, -1), (-1, -1, -1), (-1, +1, -1), (+1, +1, -1)]),
    ]
    pos, nrm, idx = b"", b"", b""
    for f, (normal, corners) in enumerate(faces):
        base = f * 4
        world = [(cx * hx, cy_ * hy, cz * hz) for cx, cy_, cz in corners]
        for c in world:
            pos += struct.pack("<3f", *c)
            nrm += struct.pack("<3f", *normal)
        idx += struct.pack("<6H", base, base + 1, base + 2, base, base + 2, base + 3)
        # occl-hidden depends on the slabs actually writing depth: every face
        # normal must point away from the box centre, or half the box is culled
        # and the wall is a pair of lines (the shadow-lag lesson, verbatim).
        a, b, c = world[0], world[1], world[2]
        e1 = [b[k] - a[k] for k in range(3)]
        e2 = [c[k] - a[k] for k in range(3)]
        cross = [e1[1] * e2[2] - e1[2] * e2[1],
                 e1[2] * e2[0] - e1[0] * e2[2],
                 e1[0] * e2[1] - e1[1] * e2[0]]
        centre_to_face = sum(cross[k] * (a[k]) for k in range(3))
        assert centre_to_face > 0.0, "box face wound inward"
        dot = sum(cross[k] * normal[k] for k in range(3))
        assert dot > 0.0, "box face normal disagrees with winding"
    return pos, nrm, idx, 24, 36


class GltfBuilder:
    def __init__(self, generator):
        self.blob = b""
        self.views = []
        self.accessors = []
        self.meshes = []
        self.nodes = []
        self.materials = []
        self.generator = generator

    def add_material(self, name, color, rough=0.7):
        self.materials.append({
            "name": name,
            "pbrMetallicRoughness": {"baseColorFactor": list(color) + [1.0],
                                     "metallicFactor": 0.0,
                                     "roughnessFactor": rough}})
        return len(self.materials) - 1

    def add_view(self, data, target):
        self.views.append({"buffer": 0, "byteOffset": len(self.blob),
                           "byteLength": len(data), "target": target})
        self.blob += data
        return len(self.views) - 1

    def add_accessor(self, view, ctype, count, atype, minmax=None):
        acc = {"bufferView": view, "componentType": ctype, "count": count, "type": atype}
        if minmax:
            acc["min"], acc["max"] = minmax
        self.accessors.append(acc)
        return len(self.accessors) - 1

    def add_plane_accessors(self, n):
        pos, nrm, idx, vcount, icount = plane_mesh_data(n)
        pv = self.add_view(pos, 34962)
        nv = self.add_view(nrm, 34962)
        iv = self.add_view(idx, 34963)
        pa = self.add_accessor(pv, 5126, vcount, "VEC3",
                               ([-1.0, -1.0, 0.0], [1.0, 1.0, 0.0]))
        na = self.add_accessor(nv, 5126, vcount, "VEC3")
        ia = self.add_accessor(iv, 5123, icount, "SCALAR")
        return pa, na, ia

    def add_plane_node(self, name, pa, na, ia, material, centre, scale):
        # A DISTINCT glTF mesh per node even though the accessors are shared:
        # draw_run_can_join keys on the Mesh pointer, so shared mesh entries
        # would batch and the submission cost being measured would vanish.
        self.meshes.append({"name": name,
                            "primitives": [{"attributes": {"POSITION": pa, "NORMAL": na},
                                            "indices": ia, "material": material}]})
        self.nodes.append({"name": name, "mesh": len(self.meshes) - 1,
                           "translation": list(centre), "scale": list(scale)})

    def add_shared_plane_node(self, name, mesh_index, centre, scale):
        # The OPPOSITE choice, for the scatter twin: nodes share one glTF mesh
        # so the importer dedups them onto one Mesh and the instancer batches.
        self.nodes.append({"name": name, "mesh": mesh_index,
                           "translation": list(centre), "scale": list(scale)})

    def add_box_node(self, name, material, centre, half):
        pos, nrm, idx, vcount, icount = box_mesh_data(*half)
        pv = self.add_view(pos, 34962)
        nv = self.add_view(nrm, 34962)
        iv = self.add_view(idx, 34963)
        pa = self.add_accessor(pv, 5126, vcount, "VEC3",
                               ([-half[0], -half[1], -half[2]],
                                [half[0], half[1], half[2]]))
        na = self.add_accessor(nv, 5126, vcount, "VEC3")
        ia = self.add_accessor(iv, 5123, icount, "SCALAR")
        self.meshes.append({"name": name,
                            "primitives": [{"attributes": {"POSITION": pa, "NORMAL": na},
                                            "indices": ia, "material": material}]})
        self.nodes.append({"name": name, "mesh": len(self.meshes) - 1,
                           "translation": list(centre)})

    def gltf(self):
        return {
            "asset": {"version": "2.0", "generator": self.generator},
            "scene": 0,
            "scenes": [{"nodes": list(range(len(self.nodes)))}],
            "nodes": self.nodes,
            "meshes": self.meshes,
            "materials": self.materials,
            "accessors": self.accessors,
            "bufferViews": self.views,
            "buffers": [{"uri": "data:application/octet-stream;base64," +
                                base64.b64encode(self.blob).decode("ascii"),
                         "byteLength": len(self.blob)}],
        }


def node_world_bounds(builder):
    """The app's recenter math, replicated: min/max over node-transformed
    position accessors. Scale+translation only -- nothing here rotates."""
    lo = [math.inf] * 3
    hi = [-math.inf] * 3
    for node in builder.nodes:
        mesh = builder.meshes[node["mesh"]]
        acc = builder.accessors[mesh["primitives"][0]["attributes"]["POSITION"]]
        scale = node.get("scale", [1.0, 1.0, 1.0])
        trans = node.get("translation", [0.0, 0.0, 0.0])
        for k in range(3):
            a = acc["min"][k] * scale[k] + trans[k]
            b = acc["max"][k] * scale[k] + trans[k]
            lo[k] = min(lo[k], a, b)
            hi[k] = max(hi[k], a, b)
    return lo, hi


# A quarter turn about X, R_x(-90): y' = z, z' = -y, so the plane's +Z normal
# lands on +Y and the floor faces UP. The inverse turn faces it down and
# backface culling deletes it -- which is why the mapping is asserted below
# rather than trusted, the file's own winding rule applied to its last mesh.
FLOOR_ROTATION = [-math.sqrt(0.5), 0.0, 0.0, math.sqrt(0.5)]


def add_floor_and_assert_recentered(builder, tag, pa, na, ia, material, half_x, floor_z):
    """The floor strip, and the recenter no-op it exists to guarantee.

    Bounds are taken BEFORE the floor is added, because its rotation is the one
    transform node_world_bounds cannot fold; the floor's true world box is
    unioned by hand. The authored occluders:[] boxes are world-space on the
    Scene and do NOT follow a recenter shift, so the shift must be zero, not
    merely small.
    """
    lo, hi = node_world_bounds(builder)
    assert lo[1] > 0.0, f"{tag}: geometry dips below the floor ({lo[1]})"

    builder.add_plane_node("occl_floor", pa, na, ia, material,
                           (0.0, 0.0, 0.0), (half_x, floor_z, 1.0))
    builder.nodes[-1]["rotation"] = FLOOR_ROTATION
    # Quaternion-rotate the plane's +Z normal: its y component is 2(yz - wx),
    # and it must come out +1 or the floor faces down and backface culling
    # deletes it.
    x, y, z, w = FLOOR_ROTATION
    rotated_y = 2.0 * (y * z - w * x)
    assert rotated_y > 0.99, f"{tag}: floor faces {rotated_y:+.2f}Y; must face up"

    lo[1] = 0.0  # the floor's own y
    lo[0] = min(-half_x, lo[0])
    hi[0] = max(half_x, hi[0])
    lo[2] = min(-floor_z, lo[2])
    hi[2] = max(floor_z, hi[2])
    cx = (lo[0] + hi[0]) * 0.5
    cz = (lo[2] + hi[2]) * 0.5
    assert abs(cx) < 1e-4, f"{tag}: recenter would shift x by {-cx}"
    assert abs(cz) < 1e-4, f"{tag}: recenter would shift z by {-cz}"


def build_portal():
    b = GltfBuilder("gen_occlusion_fixture.py")
    wall_mat = b.add_material("occl_wall", (0.45, 0.44, 0.42))
    prop_mat = b.add_material("occl_prop", (0.30, 0.34, 0.40))
    door_mat = b.add_material("occl_door", (0.95, 0.75, 0.20), rough=0.4)

    # The window-framing slabs. Boxes, so the material-flagged variant's AABB
    # proxy is the same solid the picture shows.
    hz = WALL_THICK * 0.5
    zc = -hz  # front face exactly on z = 0
    slabs = {
        "occl_wall_left": ((-(WALL_X + WIN_X) * 0.5, CY, zc),
                           ((WALL_X - WIN_X) * 0.5, WALL_Y, hz)),
        "occl_wall_right": (((WALL_X + WIN_X) * 0.5, CY, zc),
                            ((WALL_X - WIN_X) * 0.5, WALL_Y, hz)),
        "occl_wall_bottom": ((0.0, CY - (WALL_Y + WIN_Y) * 0.5, zc),
                             (WIN_X, (WALL_Y - WIN_Y) * 0.5, hz)),
        "occl_wall_top": ((0.0, CY + (WALL_Y + WIN_Y) * 0.5, zc),
                          (WIN_X, (WALL_Y - WIN_Y) * 0.5, hz)),
    }
    boxes = []
    for name, (centre, half) in slabs.items():
        b.add_box_node(name, wall_mat, centre, half)
        box_min = [centre[k] - half[k] + BOX_EPS for k in range(3)]
        box_max = [centre[k] + half[k] - BOX_EPS for k in range(3)]
        for k in range(3):  # occl-hidden: a box must be a box after the inset
            assert box_min[k] < box_max[k], f"{name}: inset inverted the box"
        boxes.append({"boxMin": box_min, "boxMax": box_max})

    pa, na, ia = b.add_plane_accessors(GRID_N)

    # The hidden grid: two bands, left and right of the window, laid out on the
    # wall plane and unprojected to their depth layer.
    band_inner = WIN_X + BAND_MARGIN
    band_outer = NDC_LIMIT * HALF_W
    band_top = NDC_LIMIT * HALF_H - MESH_HY
    hidden = 0
    for band_sign in (-1.0, 1.0):
        c0 = band_inner + MESH_HX
        c1 = band_outer - MESH_HX
        for col in range(BAND_COLS):
            xw = band_sign * (c0 + (c1 - c0) * col / (BAND_COLS - 1))
            for row in range(BAND_ROWS):
                yw = CY - band_top + 2.0 * band_top * row / (BAND_ROWS - 1)
                z = LAYER_Z[hidden % len(LAYER_Z)]
                cx, cyw, s = unproject((xw, yw), z, EYE_Z, CY)
                name = f"occl_hidden_{hidden:03d}"
                b.add_plane_node(name, pa, na, ia, prop_mat,
                                 (cx, cyw, z), (MESH_HX * s, MESH_HY * s, 1.0))

                # occl-hidden: the projection sits inside one slab's projected
                # rect with >= 2 tiles of margin on every side, so the
                # conservative footprint (outward-rounded, coverage-rounded
                # down) still lands on fully covered tiles.
                rx0, rx1 = abs(xw) - MESH_HX, abs(xw) + MESH_HX
                ry0, ry1 = yw - MESH_HY, yw + MESH_HY
                assert (rx0 - WIN_X) / HALF_W >= 2.0 * TILE_X, \
                    f"{name}: window margin under 2 tiles"
                assert (WALL_X - rx1) / HALF_W >= 2.0 * TILE_X, \
                    f"{name}: outer-edge margin under 2 tiles"
                assert (ry0 - (CY - WALL_Y)) / HALF_H >= 2.0 * TILE_Y, \
                    f"{name}: bottom margin under 2 tiles"
                assert ((CY + WALL_Y) - ry1) / HALF_H >= 2.0 * TILE_Y, \
                    f"{name}: top margin under 2 tiles"
                # occl-hidden: in-frustum, so frustum culling contributes
                # exactly zero and the culled count is occlusion alone.
                assert abs(ndc_x(band_sign * rx1)) <= 0.9, f"{name}: leaves the frustum in x"
                assert abs(ndc_y(ry0)) <= 0.9 and abs(ndc_y(ry1)) <= 0.9, \
                    f"{name}: leaves the frustum in y"
                # occl-crossover: the depth gap to the wall's back face dwarfs
                # any 16-bit quantisation step whatever near/far the app derives.
                assert (-WALL_THICK) - z >= 1.0, f"{name}: too close to the wall"
                hidden += 1

    # Three meshes fully visible through the window.
    for i, xw in enumerate(DOOR_XS):
        z = LAYER_Z[i % len(LAYER_Z)]
        cx, cyw, s = unproject((xw, CY), z, EYE_Z, CY)
        name = f"occl_door_{i}"
        b.add_plane_node(name, pa, na, ia, door_mat,
                         (cx, cyw, z), (DOOR_HALF * s, DOOR_HALF * s, 1.0))
        # occl-doorway: fully inside the window with margin, so a correct
        # culler cannot touch it and the identity render shows it.
        assert abs(xw) + DOOR_HALF <= WIN_X - DOOR_MARGIN, f"{name}: too close to the jamb"
        assert DOOR_HALF <= WIN_Y - DOOR_MARGIN, f"{name}: too close to the header"

    # The marginal mesh: behind the right slab, poking into the window by a
    # couple of mask texels. Visible as a sliver; the sliver is what the
    # inward-rounding mutation deletes.
    mx0 = WIN_X - MARGINAL_POKE
    mcx_wall = mx0 + MARGINAL_HX
    z = LAYER_Z[0]
    cx, cyw, s = unproject((mcx_wall, CY), z, EYE_Z, CY)
    b.add_plane_node("occl_door_marginal", pa, na, ia, door_mat,
                     (cx, cyw, z), (MARGINAL_HX * s, MARGINAL_HY * s, 1.0))
    poke_texels = (MARGINAL_POKE / HALF_W) / TEXEL_X
    # occl-doorway: the poke must be wide enough that conservatism must keep it
    # (over a texel) and narrow enough that the inward-rounding mutation culls
    # it (under a tile).
    assert 1.0 < poke_texels < OCC_TILE_W, \
        f"marginal poke is {poke_texels:.1f} texels; want (1, {OCC_TILE_W})"
    # ...and the wall side must span at least a full tile, so the mutated
    # footprint still exists and lands on covered wall.
    wall_side = (mx0 + 2.0 * MARGINAL_HX) - WIN_X
    assert wall_side / HALF_W >= 2.0 * TILE_X, "marginal wall side under 2 tiles"
    assert MARGINAL_HY <= WIN_Y - DOOR_MARGIN, "marginal leaves the window rows"

    deepest = min(LAYER_Z)
    floor_z = max(abs(deepest) + 1.0, EYE_Z + 1.0)
    add_floor_and_assert_recentered(b, "portal", pa, na, ia, prop_mat, WALL_X + 1.0, floor_z)

    # occl-inside: the camera in its own .cscn variant sits behind the wall, in
    # front of the first layer, inside no authored box.
    inside_eye = (0.0, CY, -4.0)
    assert inside_eye[2] < -WALL_THICK and inside_eye[2] > max(LAYER_Z), \
        "inside camera is not between the wall and the first layer"
    for box in boxes:
        inside = all(box["boxMin"][k] < inside_eye[k] < box["boxMax"][k] for k in range(3))
        assert not inside, "inside camera sits inside an occluder box"

    assert hidden >= 3 and len(DOOR_XS) >= 3, "a counted class needs >= 3 members"

    # occl-near: the camera almost touching the left slab, so the app's derived
    # near plane reaches past the slab's front face and clips the wall open --
    # the renderer shows the room through the opened shell, and a raster that
    # stands any non-front face across the frame culls what is now visible
    # (measured at 118,794 px before faces were classified). The eye sits
    # centred on the slab band so the slab, not the window, fills the frame.
    near_eye = (-(WIN_X + WALL_X) / 2.0, CY, 0.03)
    assert near_eye[2] > 0.0, "near camera is not in front of the wall solid"
    assert -WALL_X + 1.0 < near_eye[0] < -WIN_X - 1.0, \
        "near camera is not centred on the left slab band"
    for box in boxes:
        inside = all(box["boxMin"][k] < near_eye[k] < box["boxMax"][k] for k in range(3))
        assert not inside, "near camera sits inside an occluder box"

    camera = {"eye": [0.0, CY, EYE_Z], "target": [0.0, CY, 0.0], "fov": FOV_DEG}
    return b, boxes, camera, inside_eye, near_eye, hidden


def build_scatter():
    b = GltfBuilder("gen_occlusion_fixture.py")
    wall_mat = b.add_material("occl_wall", (0.45, 0.44, 0.42))
    prop_mat = b.add_material("occl_prop", (0.30, 0.55, 0.40))

    half_h = SC_EYE_Z * math.tan(math.radians(FOV_DEG) * 0.5)
    half_w = half_h * ASPECT

    pa, na, ia = b.add_plane_accessors(24)
    # ONE glTF mesh entry, shared by every instance node: the importer dedups
    # to one Mesh and the instancer forms one long run -- the thing the pillars
    # are here to fragment.
    b.meshes.append({"name": "occl_inst",
                     "primitives": [{"attributes": {"POSITION": pa, "NORMAL": na},
                                     "indices": ia, "material": prop_mat}]})
    inst_mesh = len(b.meshes) - 1

    span = (SC_COLS - 1) * SC_SPACING * 0.5
    proj = SC_EYE_Z / (SC_EYE_Z - SC_INST_Z)          # instance plane -> NDC scale
    pillar_proj = SC_EYE_Z / (SC_EYE_Z - SC_PILLAR_Z)  # pillar plane -> NDC scale
    hidden_cols = [c for c in range(SC_COLS) if c % SC_HIDDEN_STRIDE == 0]

    boxes = []
    order = []
    for col in range(SC_COLS):
        x = -span + col * SC_SPACING
        for row, y in enumerate(SC_ROW_YS):
            order.append((f"occl_inst_{col}_{row}", (x, y, SC_INST_Z)))
    for name, centre in order:
        b.add_shared_plane_node(name, inst_mesh, centre, (SC_HALF, SC_HALF, 1.0))

    inst_half_ndc = SC_HALF * proj / half_w
    for col in hidden_cols:
        x = -span + col * SC_SPACING
        # The pillar covers the column's projection with margin, measured on
        # the pillar plane through the eye.
        px = x * (SC_EYE_Z - SC_PILLAR_Z) / (SC_EYE_Z - SC_INST_Z)
        need_half_ndc = inst_half_ndc + SC_PILLAR_MARGIN_TILES * TILE_X
        phx = need_half_ndc * half_w / pillar_proj
        y_lo = min(SC_ROW_YS) - SC_HALF
        y_hi = max(SC_ROW_YS) + SC_HALF
        py_lo = SC_CY + (y_lo - SC_CY) * (SC_EYE_Z - SC_PILLAR_Z) / (SC_EYE_Z - SC_INST_Z)
        py_hi = SC_CY + (y_hi - SC_CY) * (SC_EYE_Z - SC_PILLAR_Z) / (SC_EYE_Z - SC_INST_Z)
        margin_y = SC_PILLAR_MARGIN_TILES * TILE_Y * half_h / pillar_proj
        phy = (py_hi - py_lo) * 0.5 + margin_y
        pcy = (py_hi + py_lo) * 0.5
        assert pcy - phy > 0.05, "pillar reaches below the floor"
        name = f"occl_pillar_{col}"
        centre = (px, pcy, SC_PILLAR_Z - SC_PILLAR_THICK * 0.5)
        half = (phx, phy, SC_PILLAR_THICK * 0.5)
        b.add_box_node(name, wall_mat, centre, half)
        boxes.append({"boxMin": [centre[k] - half[k] + BOX_EPS for k in range(3)],
                      "boxMax": [centre[k] + half[k] - BOX_EPS for k in range(3)]})

        # occl-fragment: the visible neighbours must clear the pillar's true
        # silhouette by texels, or "visible" is an accident of rounding.
        # Signed intervals, not abs() -- the near side flips across the axis.
        pc = px * pillar_proj / half_w
        p0, p1 = pc - need_half_ndc, pc + need_half_ndc
        for ncol in (col - 1, col + 1):
            if 0 <= ncol < SC_COLS and ncol not in hidden_cols:
                nc = (-span + ncol * SC_SPACING) * proj / half_w
                n0, n1 = nc - inst_half_ndc, nc + inst_half_ndc
                true_gap = max(n0 - p1, p0 - n1) / TEXEL_X
                assert true_gap >= SC_VISIBLE_MIN_TEXELS, \
                    f"col {ncol} hides behind pillar {col} ({true_gap:.1f} texels)"

    # In-frustum for every instance, occl-fragment's exactness condition.
    for name, centre in order:
        edge = (abs(centre[0]) + SC_HALF) * proj / half_w
        assert edge <= 0.95, f"{name}: leaves the frustum"

    lo, hi = node_world_bounds(b)
    floor_z = SC_EYE_Z + 1.0
    floor_x = max(span + 3.0, abs(lo[0]), abs(hi[0]))
    add_floor_and_assert_recentered(b, "scatter", pa, na, ia, prop_mat, floor_x, floor_z)

    hidden_count = len(hidden_cols) * SC_ROWS
    assert hidden_count >= 3, "a counted class needs >= 3 members"
    camera = {"eye": [0.0, SC_CY, SC_EYE_Z], "target": [0.0, SC_CY, 0.0], "fov": FOV_DEG}
    return b, boxes, camera, hidden_count, len(order)


LIGHT = {"name": "OcclusionSun", "type": "directional",
         "direction": [0.2, -0.6, -0.77], "color": [1.0, 1.0, 1.0],
         "intensity": 3.0, "cast_shadows": True}
POST = {"tonemap": "neutral", "exposure": 1.0}


def write_json(name, payload):
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, name), "w") as f:
        json.dump(payload, f, indent=1)
        f.write("\n")


portal, portal_boxes, portal_cam, inside_eye, near_eye, hidden_n = build_portal()
write_json("occlusion_fixture.gltf", portal.gltf())
base_cscn = {
    "version": 1,
    "models": [{"path": "occlusion_fixture.gltf"}],
    "lights": [LIGHT],
    "camera": portal_cam,
    "post": POST,
}
write_json("occlusion_fixture.cscn", {**base_cscn, "occluders": portal_boxes})
write_json("occlusion_fixture_bare.cscn", base_cscn)
# The inside camera as its own variant rather than gate-side --cam-eye flags:
# a coordinate hardcoded in gates.py cannot be kept honest, because the asserts
# above constrain inside_eye against values that track CY automatically --
# nothing would ever compare a mirrored literal. A file makes drift
# unrepresentable.
write_json("occlusion_fixture_inside.cscn",
           {**base_cscn, "occluders": portal_boxes,
            "camera": {"eye": list(inside_eye),
                       "target": [inside_eye[0], inside_eye[1], inside_eye[2] - 12.0],
                       "fov": FOV_DEG}})
write_json("occlusion_fixture_near.cscn",
           {**base_cscn, "occluders": portal_boxes,
            "camera": {"eye": list(near_eye),
                       "target": [near_eye[0], near_eye[1], -20.0],
                       "fov": FOV_DEG}})
write_json("occlusion_fixture_mat.cscn",
           {**base_cscn, "materials": {"occl_wall": {"occluder": "on"}}})

scatter, scatter_boxes, scatter_cam, sc_hidden, sc_total = build_scatter()
write_json("occlusion_scatter.gltf", scatter.gltf())
write_json("occlusion_scatter.cscn", {
    "version": 1,
    "models": [{"path": "occlusion_scatter.gltf"}],
    "lights": [LIGHT],
    "camera": scatter_cam,
    "post": POST,
    "occluders": scatter_boxes,
})

print(f"wrote occlusion_fixture.gltf (+5 .cscn): {hidden_n} hidden, "
      f"{len(DOOR_XS)} door, 1 marginal, inside eye {inside_eye}, "
      f"near eye {near_eye}")
print(f"wrote occlusion_scatter.gltf (+1 .cscn): {sc_hidden} of {sc_total} "
      f"instances behind {len(scatter_boxes)} pillars")
