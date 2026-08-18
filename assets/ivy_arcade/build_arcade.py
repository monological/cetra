#!/usr/bin/env python3
"""Build assets/ivy_arcade/ivy_arcade.glb -- an ivy-clad stone arcade (spec 11.51).

Run inside Blender:
    blender --background --python assets/ivy_arcade/build_arcade.py

Committed for the reason every fixture in assets/ has a gen_*.py: an art asset
assembled by hand is a binary nobody can re-derive, and the wind channels this
one carries are invisible in the file. Here the authoring rules are code.

THE PROFILE IS A QUAD STRIP, NOT AN N-GON, and that is the whole reason this
file exists rather than a few bmesh calls. The rib outline is CONCAVE -- an "n"
-- and handing it to bmesh.faces.new() plus triangulate() fills straight across
the opening, turning every arch into a solid slab. It renders as a plausible
dark wall rather than as an error, and the tunnel is simply not there. Building
the band as matched inner/outer boundaries and joining them explicitly cannot
express that mistake.

Inner and outer boundaries share arc CENTRES and angles, so point i on one
corresponds to point i on the other and the band is a constant PIER thick the
whole way round -- including through the shoulders, where offsetting a polyline
by a fixed distance would not be.

WIND DATA. Nothing here carries any yet; the ivy phases add it. When they do,
UV1 is not texture coordinates (see spec 11.51): the shader reads .x as a phase
in [0,1) and .y as a flex weight, and Blender's exporter flips v, so flex is
authored as 1 - flex and a leaf's stem goes at v = 1.
"""

import math
import os
import sys

import bmesh
import bpy

# --- the "n" -----------------------------------------------------------------
# Interior 3.0 wide by 4.0 to the flat top. R_IN is what decides whether this
# reads as an "n" or as an arch: at 1.2 the curve eats 80% of the half-span and
# the result is a semicircle by another name. 0.45 leaves a 2.1 m flat top with
# tight shoulders, which is the shape being copied.
SPAN_IN, PIER, TOP_IN, R_IN = 1.5, 0.7, 4.0, 0.45
CX, CZ = SPAN_IN - R_IN, TOP_IN - R_IN
R_OUT = R_IN + PIER
X_OUT, TOP_OUT = CX + R_OUT, CZ + R_OUT
DEPTH = 0.5          # rib thickness along the tunnel
PITCH = 1.4          # centre to centre, so the gap is PITCH - DEPTH = 0.9
RIBS = 24
K, SEG, M = 6, 10, 8  # samples: pier, shoulder arc, flat top
UV_SCALE = 0.5        # 2 m per texture tile


def boundary(xp, r, topz):
    """One side of the band: left foot, up, over the top, down to the right foot."""
    pts = [(-xp, 0.0, CZ * i / K) for i in range(K + 1)]
    for i in range(1, SEG + 1):
        a = math.pi - (math.pi / 2) * i / SEG
        pts.append((-CX + r * math.cos(a), 0.0, CZ + r * math.sin(a)))
    for i in range(1, M + 1):
        pts.append((-CX + 2 * CX * i / M, 0.0, topz))
    for i in range(1, SEG + 1):
        a = math.pi / 2 - (math.pi / 2) * i / SEG
        pts.append((CX + r * math.cos(a), 0.0, CZ + r * math.sin(a)))
    pts += [(xp, 0.0, CZ * (K - i) / K) for i in range(1, K + 1)]
    return pts


def triplanar_uv(bm):
    """UVs from the dominant axis of each face's normal.

    Per FACE, not per vertex: the band's front, its soffit and its outer wall
    meet at right angles, and one projection cannot serve all three without
    smearing whichever two it does not suit.
    """
    uv = bm.loops.layers.uv.verify()
    for f in bm.faces:
        n = f.normal
        ax = max(range(3), key=lambda k: abs(n[k]))
        for loop in f.loops:
            c = loop.vert.co
            u, v = ((c.x, c.z) if ax == 1 else (c.y, c.z) if ax == 0 else (c.x, c.y))
            loop[uv].uv = (u * UV_SCALE, v * UV_SCALE)


def build_rib():
    inner = boundary(SPAN_IN, R_IN, TOP_IN)
    outer = boundary(X_OUT, R_OUT, TOP_OUT)
    assert len(inner) == len(outer)
    n = len(inner)

    bm = bmesh.new()
    IF = [bm.verts.new(p) for p in inner]
    OF = [bm.verts.new(p) for p in outer]
    IB = [bm.verts.new((p[0], DEPTH, p[2])) for p in inner]
    OB = [bm.verts.new((p[0], DEPTH, p[2])) for p in outer]
    for i in range(n - 1):
        bm.faces.new([IF[i], OF[i], OF[i + 1], IF[i + 1]])
        bm.faces.new([IB[i + 1], OB[i + 1], OB[i], IB[i]])
        bm.faces.new([OF[i], OB[i], OB[i + 1], OF[i + 1]])
        bm.faces.new([IF[i + 1], IB[i + 1], IB[i], IF[i]])
    bm.faces.new([IF[0], IB[0], OB[0], OF[0]])
    bm.faces.new([OF[n - 1], OB[n - 1], IB[n - 1], IF[n - 1]])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    triplanar_uv(bm)

    me = bpy.data.meshes.new("arcade_rib")
    bm.to_mesh(me)
    bm.free()
    return me


def build_ground():
    gx, y0, y1 = 9.0, -3.0, (RIBS - 1) * PITCH + DEPTH + 3.0
    nx, ny = 24, 60
    bm = bmesh.new()
    grid = {}
    for iy in range(ny + 1):
        for ix in range(nx + 1):
            x = -gx + 2 * gx * ix / nx
            y = y0 + (y1 - y0) * iy / ny
            # A slight crown, so the floor is not one perfectly flat specular
            # band running the whole length of the tunnel.
            grid[(ix, iy)] = bm.verts.new((x, y, 0.06 * (1.0 - min(1.0, (x / 3.0) ** 2))))
    for iy in range(ny):
        for ix in range(nx):
            bm.faces.new([grid[(ix, iy)], grid[(ix + 1, iy)],
                          grid[(ix + 1, iy + 1)], grid[(ix, iy + 1)]])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    uv = bm.loops.layers.uv.verify()
    for f in bm.faces:
        for loop in f.loops:
            loop[uv].uv = (loop.vert.co.x * UV_SCALE, loop.vert.co.y * UV_SCALE)
    me = bpy.data.meshes.new("arcade_ground")
    bm.to_mesh(me)
    bm.free()
    return me


def material(name, base):
    mat = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    if bsdf:
        bsdf.inputs["Base Color"].default_value = base
        bsdf.inputs["Roughness"].default_value = 0.85
    return mat


def clear():
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob, do_unlink=True)
    for me in list(bpy.data.meshes):
        bpy.data.meshes.remove(me)
    for mt in list(bpy.data.materials):
        bpy.data.materials.remove(mt)


def main():
    clear()
    stone = material("arcade_stone", (0.52, 0.50, 0.46, 1.0))
    ground = material("arcade_ground", (0.34, 0.32, 0.28, 1.0))

    rib = build_rib()
    rib.materials.append(stone)
    # Linked duplicates: one mesh datablock, RIBS objects. The exporter emits one
    # glTF mesh and RIBS nodes, assimp maps the shared node.mesh index to one
    # aiMesh, and cetra dedups to one Mesh* -- which is what the batcher keys on.
    # Linked in spatial order, because the shadow passes walk the graph raw and
    # get none of the camera lane's per-frame sort.
    for i in range(RIBS):
        ob = bpy.data.objects.new(f"rib_{i:02d}", rib)
        ob.location = (0.0, i * PITCH, 0.0)
        bpy.context.collection.objects.link(ob)

    gm = build_ground()
    gm.materials.append(ground)
    bpy.context.collection.objects.link(bpy.data.objects.new("ground", gm))

    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "ivy_arcade.glb")
    # NORMAL always: import.c dereferences mNormals with no null check, so a
    # primitive without them crashes the importer rather than shading badly.
    # No Draco, no meshopt, no GPU instances -- the vendored assimp supports none
    # of them and drops EXT_mesh_gpu_instancing silently.
    bpy.ops.export_scene.gltf(filepath=out, export_format='GLB', export_normals=True,
                              export_tangents=True, export_texcoords=True,
                              export_yup=True, use_selection=False, export_apply=True,
                              export_draco_mesh_compression_enable=False)
    print(f"wrote {out}: {RIBS} ribs of {len(rib.polygons)} faces, "
          f"tunnel {(RIBS - 1) * PITCH:.1f} m, flat top {2 * CX:.2f} m wide")


if __name__ == "__main__":
    main()
