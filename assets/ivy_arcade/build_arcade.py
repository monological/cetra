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

import json
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
# Centre to centre. Widened from 1.4 when the foliage was cranked up: the shell
# stands 0.13 off the stone and cards lift up to CARD_LIFT_MAX beyond that, in
# every direction including along the tunnel, so at 1.4 the mass from adjacent
# ribs met in the middle and SEALED the gap. The gaps are what admit the light
# that makes the whole image, so pitch has to be set against the foliage depth,
# not against the stone alone.
PITCH = 1.75
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


# --- the ivy shell ----------------------------------------------------------
# The mass of the ivy is OPAQUE geometry, not cards. A card is doubleSided, so
# it rasterises twice, and it discards, which forfeits early-Z -- on a surface
# this large the bill is fragments rather than triangles. A dense ivy mat is
# also genuinely opaque, so this is not a cheat; the cards go where leaves
# actually resolve against the light, which is the silhouette.
SHELL = 0.13        # how far the mat stands off the stone
SHELL_AMP = 0.075   # lumpiness, peak displacement along the normal
SHELL_FREQ = 2.6
SHELL_VARIANTS = 4


def build_shell(seed):
    """An offset of the rib, displaced along its normals.

    Offsetting the RADII against the same arc centres keeps the surface parallel
    to the stone through the shoulders, where offsetting the polyline by a fixed
    distance would pinch on the inside of the curve and gap on the outside.
    """
    from mathutils import noise

    inner = boundary(SPAN_IN - SHELL, R_IN - SHELL, TOP_IN - SHELL)
    outer = boundary(X_OUT + SHELL, R_OUT + SHELL, TOP_OUT + SHELL)
    n = len(inner)

    bm = bmesh.new()
    z0, z1 = -SHELL, DEPTH + SHELL
    IF = [bm.verts.new((p[0], z0, p[2])) for p in inner]
    OF = [bm.verts.new((p[0], z0, p[2])) for p in outer]
    IB = [bm.verts.new((p[0], z1, p[2])) for p in inner]
    OB = [bm.verts.new((p[0], z1, p[2])) for p in outer]
    for i in range(n - 1):
        bm.faces.new([IF[i], OF[i], OF[i + 1], IF[i + 1]])
        bm.faces.new([IB[i + 1], OB[i + 1], OB[i], IB[i]])
        bm.faces.new([OF[i], OB[i], OB[i + 1], OF[i + 1]])
        bm.faces.new([IF[i + 1], IB[i + 1], IB[i], IF[i]])
    bm.faces.new([IF[0], IB[0], OB[0], OF[0]])
    bm.faces.new([OF[n - 1], OB[n - 1], IB[n - 1], IF[n - 1]])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])

    # Displace along the vertex normal, two octaves, seeded per variant. Vertices
    # at the foot are held down so the ivy meets the ground cleanly instead of
    # hovering or sinking into it.
    bm.normal_update()
    off = seed * 17.31
    for v in bm.verts:
        p = v.co
        a = noise.noise((p.x * SHELL_FREQ + off, p.y * SHELL_FREQ, p.z * SHELL_FREQ))
        b = noise.noise((p.x * SHELL_FREQ * 2.7, p.y * SHELL_FREQ * 2.7 + off,
                         p.z * SHELL_FREQ * 2.7))
        grounded = min(1.0, max(0.0, p.z / 0.35))
        v.co = p + v.normal * ((a + 0.45 * b) * SHELL_AMP * grounded)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    triplanar_uv(bm)

    me = bpy.data.meshes.new(f"ivy_shell_{seed}")
    bm.to_mesh(me)
    bm.free()
    # Smooth: the mat's form is carried by the normal map, and faceting a
    # 46-segment offset surface would read as the polygons it is made of.
    for p in me.polygons:
        p.use_smooth = True
    return me


# --- leaf cards -------------------------------------------------------------
# The shell alone reads as MOSS: a smooth surface wearing a normal map cannot
# produce overlapping leaf silhouettes, and silhouette is most of what makes
# foliage look deep. So cards go over the WHOLE shell rather than only its
# fringe, and the shell is demoted to a backing -- it stops the eye seeing
# through the mass, and it is what casts the shadow, since it is opaque and
# needs no per-material opt-in to reach the depth pass.
# Density is set by LAYERS, not by taste, and the number that matters is
# EFFECTIVE coverage: a card's quad is CARD_LEN x CARD_LEN*CARD_ASPECT and the
# leaf's alpha fills a bit over half of it, so one card contributes roughly
# 0.0165 m^2. At 45/m^2 that is 0.7 of a layer -- a sparse skin of individually
# legible leaves, which is exactly what it looked like. 150 gave 2.5 layers and
# the stone still showed through in the gaps. 340 is about 5.6 layers, which is
# what it takes for the shell underneath to stop being visible at all.
#
# Leaf SIZE is the cheaper half of that: area goes as the square, so growing the
# card from 0.17 to 0.22 buys 1.7x the coverage per card and keeps the count
# (and the vertex count, and the file) far below what raw density would cost.
# Raised from 340 to bury the shell completely. At 340 the mat still showed
# through as smooth "mossy" surface between leaves; the shell is a backing, and
# any of it the eye can resolve reads as the wrong material rather than as
# shadow. Paired with darkening the shell in the .cscn, since the cheapest way
# to stop seeing something is for it to be the colour of the gaps.
CARD_DENSITY = 640.0
# Stem to tip of a CLUMP (which is 5-9 leaves), so a leaf inside it is ~7 cm --
# actual ivy. Sized against the shot: the arcade is read from outside and well
# back, where the mass is what registers and a leaf is fine texture. Cards big
# enough to be individually legible from there are the reason an earlier pass
# looked like banana palms.
CARD_LEN = 0.22
# Quads per clump, intersecting along the clump's own axis. TWO is the whole
# point of the rebuild: a lone quad seen edge-on collapses to a sliver, and a
# canopy viewed from inside is mostly grazing angles, so a scatter of single
# quads reads as a field of white slashes. Crossed quads always present
# something to the camera. It also pays for itself -- a clump covers far more
# than a leaf, so 55 clumps x 2 quads is 110 quads/m^2 against the 340 single
# leaves it replaces.
CARD_CROSS = 1
CARD_ATLAS_CELLS = 1  # single-leaf card: the whole UV range is the leaf
CARD_ASPECT = 0.62
CARD_TILT = 0.75      # radians the tip lifts away from the surface, at most
PHASE_FREQ = 0.55     # world frequency of the phase field; low = broad gusts
# How far a card's shading normal is pulled toward the surface it sits on. 1.0
# would shade the whole canopy as if it were the shell and lose every leaf; 0
# leaves each card lighting itself, which is the confetti case above.
CARD_NORMAL_BLEND = 0.62
# Cards stand off the shell by a varying amount rather than lying on it, so the
# mass is a canopy with volume instead of a skin. Biased toward the surface:
# most leaves are buried, a few catch the light.
CARD_LIFT_MIN = 0.012
CARD_LIFT_MAX = 0.28
CARD_LIFT_BIAS = 1.6
# Buried leaves are darker. Baked per card into COLOR_0 RGB, which is what
# tree_gen does for canopy occlusion -- and the alpha stays exactly 1.0, because
# pbr_frag multiplies vertex alpha into the cutoff test and anything less
# silently erodes the leaf.
CARD_SHADE_DEEP = 0.30


def _card_frame(n, rng):
    """A stem-to-tip direction in the surface's tangent plane, then tilted out."""
    from mathutils import Vector
    seed = Vector((0.0, 0.0, 1.0))
    if abs(n.dot(seed)) > 0.9:
        seed = Vector((1.0, 0.0, 0.0))
    t = (seed - n * seed.dot(n)).normalized()
    b = n.cross(t)
    a = rng.uniform(0.0, math.tau)
    up = (t * math.cos(a) + b * math.sin(a)).normalized()
    tilt = rng.uniform(0.15, CARD_TILT)
    up = (up * math.cos(tilt) + n * math.sin(tilt)).normalized()
    side = up.cross(n).normalized()
    return up, side


def build_cards(shell, rib_index):
    """Crossed clump cards scattered over one shell, as a single mesh.

    ONE MESH PER RIB, and that is a wind constraint rather than a batching one:
    uWindMaskMinY/MaxY come from the per-mesh AABB, so a chunk has to span the
    full rib height. A flat or near-horizontal chunk would saturate the height
    mask across millimetres and translate bodily along the wind instead of
    bending.
    """
    import random
    from mathutils import Vector, noise

    rng = random.Random(9000 + rib_index)
    bm = bmesh.new()
    uv0 = bm.loops.layers.uv.new("UVMap")
    uv1 = bm.loops.layers.uv.new("wind")
    col = bm.loops.layers.color.new("Col")

    carry = 0.0
    card_normals = []
    for poly in shell.polygons:
        vs = [Vector(shell.vertices[i].co) for i in poly.vertices]
        n = Vector(poly.normal).normalized()
        # Fan into triangles and sample each by AREA, so density is uniform over
        # the surface rather than per face -- the shell's faces vary several-fold
        # between the piers and the shoulders.
        for k in range(1, len(vs) - 1):
            a, b, c = vs[0], vs[k], vs[k + 1]
            area = (b - a).cross(c - a).length * 0.5
            want = area * CARD_DENSITY + carry
            count = int(want)
            carry = want - count
            for _ in range(count):
                r1, r2 = rng.random(), rng.random()
                if r1 + r2 > 1.0:
                    r1, r2 = 1.0 - r1, 1.0 - r2
                # Stand-off varies so the mass is a canopy with volume rather
                # than a skin, biased toward the surface: most leaves are buried,
                # a few catch the light.
                depth = rng.random() ** CARD_LIFT_BIAS
                lift = CARD_LIFT_MIN + (CARD_LIFT_MAX - CARD_LIFT_MIN) * depth
                p = a + (b - a) * r1 + (c - a) * r2 + n * lift

                up, side0 = _card_frame(n, rng)
                ln = CARD_LEN * rng.uniform(0.72, 1.28)
                hw = ln * CARD_ASPECT * 0.5
                tip = p + up * ln

                # --- per CLUMP, shared by its crossed quads -------------------
                cell = rng.randrange(CARD_ATLAS_CELLS)
                u0, u1 = cell / CARD_ATLAS_CELLS, (cell + 1) / CARD_ATLAS_CELLS
                # Depth darkening plus hue jitter, so a mass built from one leaf
                # image does not read as one leaf repeated at one brightness.
                sh = CARD_SHADE_DEEP + (1.0 - CARD_SHADE_DEEP) * depth
                tint = (sh * rng.uniform(0.80, 1.10),
                        sh * rng.uniform(0.92, 1.12),
                        sh * rng.uniform(0.74, 1.02), 1.0)
                # Phase from a smooth world field, not per card: neighbouring
                # leaves in a gust move together, and a purely random phase reads
                # as static rather than as wind. Wrapped into [0,1) because mode
                # 2's flutter uses phase * 7.0, so 0.3 and 1.3 are NOT the same.
                wp = p + Vector((0.0, rib_index * PITCH, 0.0))
                ph = (noise.noise(wp * PHASE_FREQ) * 0.5 + 0.5
                      + rng.uniform(0.0, 0.12)) % 1.0
                # Ivy has no runner arc to measure along, so exposure stands in
                # for tree_gen's root distance: a clump further off the wall is
                # further into the airstream. Superlinear, as there.
                expo = max(0.0, up.dot(n)) * 0.5 + depth * 0.5
                flex = min(1.0, 0.25 + 0.75 * (expo ** 1.2))

                for j in range(CARD_CROSS):
                    # Rotate the quad's width axis about the clump's own axis so
                    # the pair INTERSECT along it rather than sitting parallel.
                    ang = math.pi * j / CARD_CROSS
                    sd = (side0 * math.cos(ang)
                          + up.cross(side0).normalized() * math.sin(ang)).normalized()
                    f = bm.faces.new([bm.verts.new(q) for q in
                                      (p - sd * hw, p + sd * hw,
                                       tip + sd * hw, tip - sd * hw)])
                    f.normal_update()
                    # Wind so the front faces AWAY from the shell. A soup of
                    # loose quads has no "outside", so it is decided per quad
                    # here -- see the note by the absent recalc_face_normals.
                    if f.normal.dot(n) < 0.0:
                        bmesh.ops.reverse_faces(bm, faces=[f])
                        f.normal_update()
                    # Shade toward the SURFACE normal rather than the quad's own.
                    # A flat quad at a random tilt lights independently of its
                    # neighbours, which is what makes scattered cards read as
                    # confetti -- and under a sky IBL it is worse than untidy: a
                    # quad tilted past horizontal samples the atmosphere's warm
                    # GROUND hemisphere while the one beside it samples blue sky,
                    # so the canopy goes patchy brown and teal.
                    card_normals.append((n * CARD_NORMAL_BLEND + f.normal *
                                         (1.0 - CARD_NORMAL_BLEND)).normalized())
                    # Stem corners take Blender v = 1 so the shader reads 0 there
                    # and the clump pivots at its stem; u spans this cell only.
                    for loop, (u, v) in zip(f.loops, ((u0, 1.0), (u1, 1.0),
                                                      (u1, 0.0), (u0, 0.0))):
                        loop[uv0].uv = (u, v)
                        loop[uv1].uv = (ph, 1.0 - flex)
                        loop[col] = tint

    # NO recalc_face_normals here, deliberately. It orients faces outward for a
    # CLOSED manifold; a soup of loose quads has no outside, so it flips them at
    # random -- which is how the normals ended up pointing every way at once.
    me = bpy.data.meshes.new(f"ivy_cards_{rib_index:02d}")
    bm.to_mesh(me)
    bm.free()
    for poly in me.polygons:
        poly.use_smooth = True
    me.normals_split_custom_set([card_normals[i // 4] for i in range(len(me.loops))])
    return me


def leaf_material():
    """ALPHA_MASK, cutoff 0.4, two-sided -- matching tree.c and forest.c.

    The mask has to live in the albedo texture's ALPHA: baseColorFactor[3]
    becomes materialOpacity in cetra, a different quantity on a different path,
    and a MASK material with a fractional factor and no texture has coverage 1.0.
    """
    mat = bpy.data.materials.new("ivy_leaf")
    mat.use_nodes = True
    mat.use_backface_culling = False       # -> glTF doubleSided
    mat.blend_method = "CLIP"              # -> glTF alphaMode MASK
    mat.alpha_threshold = 0.4
    nt = mat.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    bsdf.inputs["Roughness"].default_value = 0.62
    bsdf.inputs["Metallic"].default_value = 0.0
    here = os.path.dirname(os.path.abspath(__file__))
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(os.path.join(here, "textures", "ivy_leaf_base.png"))
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
    return mat


def force_alpha_mask(path, cutoffs):
    """Rewrite alphaMode to MASK in the exported .gltf.

    BLENDER CANNOT EMIT THIS, and it fails silently. `blend_method = "CLIP"` is
    accepted -- CLIP is in the enum -- and then coerced to HASHED, because EEVEE
    Next dropped clipping; HASHED exports as glTF BLEND. So the material reads
    correct in this script and arrives blended.

    In cetra that is a LANE, not a shading nicety. ALPHA_BLEND lands in
    DRAW_LANE_BLEND and goes through order-independent transparency: measured
    148.6 ms of oit accumulate plus 17.5 ms of moments against a 10.4 ms opaque
    pass, and the layers composite to pale mush instead of crisp silhouettes.

    THE EXPORT IS .gltf + .bin RATHER THAN .glb FOR THIS FUNCTION'S SAKE.
    Patching a GLB means rewriting its JSON chunk, which moves every byte after
    it; assimp then reads the embedded images from the wrong offsets and the
    texture silently fails to decode, leaving the foliage WHITE with no error
    logged anywhere. A .gltf keeps its JSON in a separate text file, so this is
    an edit rather than a binary surgery, and the .bin cannot be disturbed.
    """
    with open(path) as f:
        doc = json.load(f)
    hit = 0
    for m in doc.get("materials", []):
        if m.get("name") in cutoffs:
            m["alphaMode"] = "MASK"
            m["alphaCutoff"] = cutoffs[m["name"]]
            hit += 1
    with open(path, "w") as f:
        json.dump(doc, f)
    return hit


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


def material(name, base, tex=None):
    """A Principled material, optionally wearing base/normal/roughness maps.

    Textures are wired because the glTF exporter only emits an image when a node
    feeds the BSDF -- a material carrying a colour alone exports as a factor, and
    the mat would arrive in cetra as flat green.
    """
    mat = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    if not bsdf:
        return mat
    bsdf.inputs["Base Color"].default_value = base
    bsdf.inputs["Roughness"].default_value = 0.85
    bsdf.inputs["Metallic"].default_value = 0.0
    if not tex:
        return mat

    here = os.path.dirname(os.path.abspath(__file__))

    def img(fname, non_color):
        node = nt.nodes.new("ShaderNodeTexImage")
        node.image = bpy.data.images.load(os.path.join(here, "textures", fname))
        if non_color:
            node.image.colorspace_settings.name = "Non-Color"
        return node

    nt.links.new(img(f"{tex}_base.png", False).outputs["Color"],
                 bsdf.inputs["Base Color"])
    nt.links.new(img(f"{tex}_rough.png", True).outputs["Color"],
                 bsdf.inputs["Roughness"])
    nm = nt.nodes.new("ShaderNodeNormalMap")
    nt.links.new(img(f"{tex}_normal.png", True).outputs["Color"], nm.inputs["Color"])
    nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
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

    # Several shells rather than 24 copies of one. Variety is close to free
    # here: the camera lane groups by mesh WITHIN a depth bucket, and 32 m of
    # tunnel already spreads the ribs across buckets, so distinct meshes cost
    # little that depth has not already spent.
    ivy = material("ivy_mat", (0.30, 0.42, 0.20, 1.0), tex="ivy_mat")
    shells = []
    for s in range(SHELL_VARIANTS):
        me = build_shell(s)
        me.materials.append(ivy)
        shells.append(me)
    for i in range(RIBS):
        ob = bpy.data.objects.new(f"ivy_{i:02d}", shells[i % SHELL_VARIANTS])
        ob.location = (0.0, i * PITCH, 0.0)
        bpy.context.collection.objects.link(ob)

    # Cards are per rib and NOT shared, unlike the shells: with only four shell
    # variants, sharing the scatter too would put identical leaves on every
    # fourth arch, and at a 1.4 m pitch that repeat is visible straight down the
    # tunnel. 24 meshes is 24 draws, which is nothing.
    leaf = leaf_material()
    total = 0
    for i in range(RIBS):
        me = build_cards(shells[i % SHELL_VARIANTS], i)
        me.materials.append(leaf)
        total += len(me.polygons)
        ob = bpy.data.objects.new(f"leaf_{i:02d}", me)
        ob.location = (0.0, i * PITCH, 0.0)
        bpy.context.collection.objects.link(ob)

    gm = build_ground()
    gm.materials.append(ground)
    bpy.context.collection.objects.link(bpy.data.objects.new("ground", gm))

    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "ivy_arcade.gltf")
    # NORMAL always: import.c dereferences mNormals with no null check, so a
    # primitive without them crashes the importer rather than shading badly.
    # No Draco, no meshopt, no GPU instances -- the vendored assimp supports none
    # of them and drops EXT_mesh_gpu_instancing silently.
    bpy.ops.export_scene.gltf(filepath=out, export_format='GLTF_SEPARATE',
                              export_normals=True,
                              export_tangents=True, export_texcoords=True,
                              export_yup=True, use_selection=False, export_apply=True,
                              export_draco_mesh_compression_enable=False)
    masked = force_alpha_mask(out, {"ivy_leaf": 0.4})
    print(f"wrote {out}: {RIBS} ribs of {len(rib.polygons)} faces, "
          f"{SHELL_VARIANTS} ivy shells of {len(shells[0].polygons)}, "
          f"{total} leaf cards, "
          f"tunnel {(RIBS - 1) * PITCH:.1f} m, flat top {2 * CX:.2f} m wide, "
          f"{masked} material(s) forced to ALPHA_MASK")


if __name__ == "__main__":
    main()
