"""silent's birdcage, the box the front door's key is kept in, modelled in Blender (spec 13.55).

    /Applications/Blender.app/Contents/MacOS/Blender -b --factory-startup \
        -P apps/silent/tools/make_birdcage_blender.py -- [--surface ID] [--preview] [--no-export]

Builds everything from nothing, so nothing in a user's Blender reaches it, and writes:

  - assets/models/birdcage.glb: the cage, its door -- the face -- hung on its hinge, and where
    the key hangs inside it;
  - out/birdcage_blender/birdcage.blend, to open and look at, its baked maps beside it, and with
    --preview, stills lit as the basement lights it, tiled into
    out/birdcage_blender/preview/sheet_<surface>.png.

make_arms_blender.py imports this for the cage's dimensions and where its door, hinge and hook
are, and stands birdcage.glb in its own scene, so the hands are posed round the real thing.
Nothing runs on import.

Everything is authored in MODEL space as the game reads it: x the cage's left, y up, z forward --
the face looks along +z -- and the base's foot at y = 0. Blender is z up; blender_kit's `bv` is
the one place the two meet. Its roughness and metal map is MASK_SIZE square, as every model's
masks are.

THE CAGE is a birdcage cast solid: a round base, and over it a closed shell the shape of a cage's
bars and dome, hollow inside, with the bars, rings and tracery standing out of it in relief, a
crown with a ring to carry it by, and on its front a round door cast as a face, hinged on the side
to the viewer's left, over an opening into the dark inside. Its file holds four nodes the game
finds by name: `cage`, and its children `cage_shell`, `cage_door` (its origin on the hinge, which
turns about its own y, a negative angle opening it), and `key_hang` (where the key's middle hangs
and how it is turned, bow up on the hook, its face to the door).

ITS SURFACE is baked. The parts are unwrapped together into one atlas, and a shader takes a CC0
scan of rusted metal from Poly Haven (fetched through polyhaven.py; `rusty_metal_04` unless
--surface names another) and shapes it by what the form itself says -- how sheltered each point is
and how much it stands out: worn to bare iron on what stands out, grimed in the crevices, dark deep
inside. Cycles bakes that into a colour map, a roughness and metal map packed as glTF packs them,
and a normal map, which the file carries.
"""

import argparse
import math
import os
import sys

import bpy
from mathutils import Matrix, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# The tools' own modules, found beside this one.
from blender_kit import (BV, Graph, Parts, apply_boolean, assign_material,  # noqa: E402
                         bake_atlas, baked_material, blender_argv, bv, export_model, finish, lathe,
                         pin, placed, rendered, resmooth, save_sheet, scan, smooth, sphere, tube,
                         use_gpu)
from glb import read_glb, triangles  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT = os.path.join(ROOT, "out", "birdcage_blender")
CAGE_GLB = os.path.join(ROOT, "assets", "models", "birdcage.glb")


# ---------------------------------------------------------------------------------------------
# The cage. Model metres; the base's foot at y = 0.

BASE_TOP = 0.026        # the cage's floor
CAGE_R = 0.058          # the bars' circle
SHELL_SINK = 0.0008     # the shell's outside under the bars' middles, which stand out of it
SHELL_T = 0.003         # its wall
BARS = 16
BAR_WIRE = 0.0017
RING_WIRE = 0.0022
LACE_WIRE = 0.0012
SHOULDER_Y = 0.165      # where the straight bars turn into the dome
DOME_TOP_Y = 0.218
DOME_TOP_R = 0.013
RINGS_Y = (0.030, 0.058, 0.138, SHOULDER_Y)
ARCH_SPRING_Y = 0.034   # the tracery's arches spring from the bars here, in the lower band
CIRCLE_Y = 0.1515       # the circles' middle, in the upper band

# The door: a round plate DOOR_R across its middle, on the cage's front at DOOR_Y, its back just
# clear of its frame's ring and the face cast on its front, hinged to the viewer's left.
DOOR_Y = 0.100
DOOR_R = 0.034
DOOR_FRAME_R = CAGE_R + 0.0004
DOOR_BACK = DOOR_FRAME_R + RING_WIRE + 0.0003
DOOR_T = 0.0025
DOOR_GAP = 1.04         # the opening in the bars, as a share of the door's radius
DOOR_OPEN = -1.75       # radians it turns about its hinge to stand open

# The key, as house_key.c makes it: KEY_LEN long, its middle KEY_LEN / 2 down from its bow's
# top, and the inside of its bow's ring KEY_BOW_REST below that top, which is what rests on the
# hook. It hangs in the hook's curl, HOOK_CURL across, its middle KEY_DROP under the door's, so
# the door opens on its bow and not only its shank.
KEY_LEN = 0.1
KEY_BOW_REST = 0.007
KEY_DROP = 0.017
HOOK_WIRE = 0.0011
HOOK_CURL = 0.004
HOOK_Y = DOOR_Y - KEY_DROP + 0.5 * KEY_LEN - KEY_BOW_REST + HOOK_WIRE  # the curl's lowest point


def angle_of(s, radius):
    """The angle round the cage a length `s` along its surface at `radius` from its front."""
    return s / radius


def on_cylinder(s, y, radius):
    a = angle_of(s, radius)
    return Vector((radius * math.sin(a), y, radius * math.cos(a)))


def dome_profile(phi, inset=0.0):
    """(radius, height) of the dome `phi` from the shoulder (0) to the top (pi / 2), `inset`
    under the bars' own curve."""
    r = DOME_TOP_R + (CAGE_R - DOME_TOP_R) * math.cos(phi) - inset * math.cos(phi)
    y = SHOULDER_Y + (DOME_TOP_Y - SHOULDER_Y) * math.sin(phi) - inset * math.sin(phi)
    return r, y


def dome_point(theta, phi):
    """A point on a bar's dome, `phi` from the shoulder (0) to the top (pi / 2)."""
    r, y = dome_profile(phi)
    return Vector((r * math.sin(theta), y, r * math.cos(theta)))


def shell():
    """The body the pattern stands out of: a closed shell over the base, SHELL_T thick and hollow
    inside, its outside SHELL_SINK under the bars' circle and their dome, its top under the
    crown. Up its outside from the base and down its inside again, as one solid of
    revolution."""
    n = 16
    outside = [(CAGE_R - SHELL_SINK, BASE_TOP - 0.001)] + [
        dome_profile(0.5 * math.pi * k / n, SHELL_SINK) for k in range(n + 1)]
    inside = [(CAGE_R - SHELL_SINK - SHELL_T, BASE_TOP)] + [
        dome_profile(0.5 * math.pi * k / n, SHELL_SINK + SHELL_T) for k in range(n + 1)]
    profile = ([(0.0, inside[-1][1])] + inside[::-1] + outside + [(0.0, outside[-1][1])])
    return lathe(profile, 96)


def base(parts):
    profile = [(0.0, 0.0), (0.066, 0.0), (0.068, 0.0025), (0.068, 0.0065), (0.065, 0.0085),
               (0.0615, 0.012), (0.0605, 0.0155), (0.0625, 0.0185), (0.0632, 0.0215),
               (0.0632, 0.024), (0.0605, BASE_TOP), (0.0, BASE_TOP)]
    parts.add(*lathe(profile, 64))


def bars(parts):
    gap_top = DOOR_Y + DOOR_GAP * DOOR_R
    for i in range(BARS):
        theta = 2.0 * math.pi * i / BARS
        s = CAGE_R * (theta if theta <= math.pi else theta - 2.0 * math.pi)
        up = [Vector((CAGE_R * math.sin(theta), y, CAGE_R * math.cos(theta)))
              for y in (BASE_TOP - 0.002, SHOULDER_Y)]
        dome = [dome_point(theta, 0.5 * math.pi * k / 14) for k in range(1, 15)]
        half = (DOOR_GAP * DOOR_R) ** 2 - s * s
        if half > 0.0:
            # Through the door's opening: cut, each end into the door's frame.
            h = math.sqrt(half)
            parts.add(*tube([up[0], Vector((up[0].x, DOOR_Y - h, up[0].z))], BAR_WIRE))
            parts.add(*tube([Vector((up[0].x, DOOR_Y + h, up[0].z))] + [up[1]] + dome, BAR_WIRE))
        else:
            parts.add(*tube(up + dome, BAR_WIRE))


def rings(parts):
    for y in RINGS_Y:
        pts = [Vector((CAGE_R * math.sin(a), y, CAGE_R * math.cos(a)))
               for a in (2.0 * math.pi * k / 96 for k in range(96))]
        parts.add(*tube(pts, RING_WIRE, 8, closed=True))
    # One more round the dome, half way up it.
    pts = [dome_point(2.0 * math.pi * k / 72, 0.25 * math.pi) for k in range(72)]
    parts.add(*tube(pts, RING_WIRE * 0.8, 8, closed=True))
    # The door's frame, round the opening in the bars.
    pts = [on_cylinder(DOOR_GAP * DOOR_R * math.cos(t), DOOR_Y + DOOR_GAP * DOOR_R * math.sin(t),
                       DOOR_FRAME_R)
           for t in (2.0 * math.pi * k / 72 for k in range(72))]
    parts.add(*tube(pts, RING_WIRE, 8, closed=True))


def tracery(parts):
    """Between each pair of bars: a pointed arch in the lower band with a ring in its head, and a
    ring in the upper band."""
    step = 2.0 * math.pi * CAGE_R / BARS
    for i in range(BARS):
        s0 = step * i
        sm = s0 + 0.5 * step
        # The arch: two arcs, each centred on the other side's springing, meeting in a point.
        arch = []
        apex_a = math.pi / 3.0
        for k in range(10):
            a = apex_a * k / 9
            arch.append((s0 + step - step * math.cos(a), ARCH_SPRING_Y + step * math.sin(a)))
        for k in range(1, 10):
            a = apex_a * (1.0 - k / 9)
            arch.append((s0 + step * math.cos(a), ARCH_SPRING_Y + step * math.sin(a)))
        # Lift the arch so its apex meets the band's top ring.
        lift = (RINGS_Y[1] - (ARCH_SPRING_Y + step * math.sin(apex_a)))
        pts = [on_cylinder(s, y + lift * (y - ARCH_SPRING_Y) / (step * math.sin(apex_a)), CAGE_R)
               for s, y in arch]
        parts.add(*tube(pts, LACE_WIRE, 6))
        ring_r = 0.0042
        head = [on_cylinder(sm + ring_r * math.cos(t), ARCH_SPRING_Y + 0.0105 + ring_r * math.sin(t),
                            CAGE_R) for t in (2.0 * math.pi * k / 20 for k in range(20))]
        parts.add(*tube(head, LACE_WIRE, 6, closed=True))
        r = 0.0098
        circle = [on_cylinder(sm + r * math.cos(t), CIRCLE_Y + r * math.sin(t), CAGE_R)
                  for t in (2.0 * math.pi * k / 24 for k in range(24))]
        parts.add(*tube(circle, LACE_WIRE, 6, closed=True))


def crown(parts):
    profile = [(0.0, DOME_TOP_Y - 0.004), (0.0145, DOME_TOP_Y - 0.004), (0.0165, DOME_TOP_Y - 0.001),
               (0.0165, DOME_TOP_Y + 0.003), (0.012, DOME_TOP_Y + 0.0075), (0.006, DOME_TOP_Y + 0.0105),
               (0.0042, DOME_TOP_Y + 0.016), (0.0055, DOME_TOP_Y + 0.020),
               (0.0035, DOME_TOP_Y + 0.0235), (0.0, DOME_TOP_Y + 0.0245)]
    parts.add(*lathe(profile, 32))
    # The ring to carry it by, in the plane of its face.
    c, r = Vector((0.0, DOME_TOP_Y + 0.0245 + 0.0135, 0.0)), 0.0135
    pts = [c + Vector((r * math.sin(t), -r * math.cos(t), 0.0))
           for t in (2.0 * math.pi * k / 40 for k in range(40))]
    parts.add(*tube(pts, 0.0024, 8, closed=True))


def hook(parts):
    """A rod down from under the crown, its end curled forward and up into a J for the key's bow
    to hang in: the curl's lowest point HOOK_Y, HOOK_CURL in front of the rod."""
    c = Vector((0.0, HOOK_Y + HOOK_CURL, HOOK_CURL))
    curl = [c + Vector((0.0, -HOOK_CURL * math.sin(a), -HOOK_CURL * math.cos(a)))
            for a in (1.25 * math.pi * k / 12 for k in range(13))]
    parts.add(*tube([Vector((0.0, DOME_TOP_Y - 0.004, 0.0))] + curl, HOOK_WIRE, 6))


def hinge_point():
    """The door's hinge: the left edge of its plate as the viewer sees it, half way through it."""
    return on_cylinder(-DOOR_R, DOOR_Y, DOOR_BACK + 0.5 * DOOR_T)


def knuckles(parts):
    """The hinge's knuckles on the frame, above and below the door's own."""
    h = hinge_point()
    for v in (-0.55, 0.55):
        parts.add(*placed(pin(0.0021, 0.006), Vector((h.x, DOOR_Y + v * DOOR_R, h.z))))


# ---------------------------------------------------------------------------------------------
# The face: the door's front as a height over its plate. (u, v) run across it from the viewer's
# left and up it, 1 at its edge; heights are metres off the plate's front.


def gauss(x, w):
    return math.exp(-(x / w) ** 2)


def ellipse(u, v, cu, cv, a, b):
    return ((u - cu) / a) ** 2 + ((v - cv) / b) ** 2


def face_height(u, v):
    """A gaunt face: sockets with nothing in them, the mouth open in an O, sunken cheeks under a
    heavy brow, ridged round like a shell. The sockets and the mouth are holes (DOOR_HOLES)."""
    r = math.hypot(u, v)
    phi = math.atan2(v, u)
    h = 0.0011 * gauss(r - 0.93, 0.035)
    # Ridged like a shell round the face.
    ridges = 0.5 + 0.5 * math.cos(18.0 * phi)
    h += 0.0005 * ridges ** 3 * smooth((r - 0.7) / 0.04) * (1.0 - smooth((r - 0.86) / 0.04))
    m = ellipse(u, v, 0.0, 0.0, 0.5, 0.68)
    if m < 1.0:
        h += 0.0032 * (1.0 - m) ** 0.5
    # Sunken cheeks and a heavy brow over sockets with nothing in them.
    for cu in (-0.26, 0.26):
        h -= 0.0012 * gauss(math.hypot(u - cu, v + 0.12), 0.12)
        h += 0.0009 * gauss(math.hypot(u - cu, v - 0.34), 0.1)
        h += 0.0008 * gauss(math.sqrt(ellipse(u, v, cu, 0.18, 0.16, 0.13)) - 1.0, 0.18)
    h += 0.0009 * gauss(u, 0.05) * smooth((v + 0.1) / 0.05) * (1.0 - smooth((v - 0.15) / 0.05))
    # A rim round the mouth's hole.
    h += 0.0009 * gauss(math.sqrt(ellipse(u, v, 0.0, -0.4, 0.11, 0.14)) - 1.0, 0.25)
    return h


# The holes cut through the door, each an oval in (u, v): its middle and its two radii -- the
# sockets, the mouth and the nostrils.
DOOR_HOLES = ((-0.26, 0.18, 0.12, 0.09), (0.26, 0.18, 0.12, 0.09), (0.0, -0.4, 0.09, 0.12),
              (-0.04, -0.08, 0.02, 0.05), (0.04, -0.08, 0.02, 0.05))


def door_point(u, v, rho):
    return on_cylinder(u * DOOR_R, DOOR_Y + v * DOOR_R, rho)


def door_plate(rings_n=56, segs=144):
    """The door as a closed solid: a front following the face's height, a back on its curve, and
    a round edge between."""
    parts = Parts()
    front = [door_point(0.0, 0.0, DOOR_BACK + DOOR_T + face_height(0.0, 0.0))]
    back = [door_point(0.0, 0.0, DOOR_BACK)]
    for k in range(1, rings_n + 1):
        rr = k / rings_n
        for m in range(segs):
            a = 2.0 * math.pi * m / segs
            u, v = rr * math.cos(a), rr * math.sin(a)
            front.append(door_point(u, v, DOOR_BACK + DOOR_T + face_height(u, v)))
            back.append(door_point(u, v, DOOR_BACK))

    def grid(offset, flip):
        faces = []
        for m in range(segs):
            m2 = (m + 1) % segs
            f = (offset, offset + 1 + m, offset + 1 + m2)
            faces.append(f[::-1] if flip else f)
        for k in range(rings_n - 1):
            a, b = offset + 1 + k * segs, offset + 1 + (k + 1) * segs
            for m in range(segs):
                m2 = (m + 1) % segs
                f = (a + m, b + m, b + m2, a + m2)
                faces.append(f[::-1] if flip else f)
        return faces

    faces = grid(0, False) + grid(len(front), True)
    edge_f = 1 + (rings_n - 1) * segs
    edge_b = len(front) + edge_f
    for m in range(segs):
        m2 = (m + 1) % segs
        faces.append((edge_f + m, edge_b + m, edge_b + m2, edge_f + m2))
    parts.add(front + back, faces)
    # The latch: a knob at the free edge.
    parts.add(*sphere(door_point(0.93, 0.0, DOOR_BACK + DOOR_T + 0.0012), 0.0024))
    # The door's own knuckle, between the frame's two.
    h = hinge_point()
    parts.add(*placed(pin(0.0021, 0.007), Vector((h.x, DOOR_Y, h.z))))
    return parts


def prism(ring, point, near, far):
    """A closed solid of a convex outline, from `near` to `far` along whatever `point(a, b, at)`
    places the outline's (a, b) at."""
    n = len(ring)
    ca = sum(a for a, _ in ring) / n
    cb = sum(b for _, b in ring) / n
    verts = [point(a, b, near) for a, b in ring] + [point(a, b, far) for a, b in ring]
    verts += [point(ca, cb, near), point(ca, cb, far)]
    c0, c1 = 2 * n, 2 * n + 1
    faces = [(i, (i + 1) % n, n + (i + 1) % n, n + i) for i in range(n)]
    faces += [(c0, (i + 1) % n, i) for i in range(n)]
    faces += [(c1, n + i, n + (i + 1) % n) for i in range(n)]
    return verts, faces


def oval(cu, cv, a, b, n=40):
    return [(cu + a * math.cos(t), cv + b * math.sin(t))
            for t in (2.0 * math.pi * k / n for k in range(n))]


def door_cutter():
    """One closed solid of every hole in the door, passing through its plate."""
    parts = Parts()
    for hole in DOOR_HOLES:
        parts.add(*prism(oval(*hole), door_point, DOOR_BACK - 0.004, DOOR_BACK + DOOR_T + 0.01))
    return parts


def opening_cutter():
    """The opening the door covers, straight through the shell's front."""
    parts = Parts()
    r = DOOR_GAP * DOOR_R - RING_WIRE * 0.5
    parts.add(*prism(oval(0.0, DOOR_Y, r, r, 72), lambda x, y, z: Vector((x, y, z)),
                     CAGE_R - 0.03, CAGE_R + 0.01))
    return parts


def build_cage():
    """The cage's parts, with no material until their surface is baked, and the key's hang."""
    body = Parts()
    base(body)
    bars(body)
    rings(body)
    tracery(body)
    crown(body)
    hook(body)
    knuckles(body)
    cage = finish("cage", body, None)

    shell_parts = Parts()
    shell_parts.add(*shell())
    hull = finish("cage_shell", shell_parts, None)
    apply_boolean(hull, finish("cage_opening", opening_cutter(), None))
    resmooth(hull)
    hull.parent = cage

    door = finish("cage_door", door_plate(), None)
    apply_boolean(door, finish("cage_door_cut", door_cutter(), None))
    resmooth(door)
    # Hung on its hinge: its origin there, a child of the cage.
    h = bv(*hinge_point())
    door.data.transform(Matrix.Translation(-h))
    door.location = h
    door.parent = cage

    # Where the key's middle hangs: its model's +z (bow to bit) down, its +x (through its face)
    # to the door, so +y (toward its bit) runs to the cage's right.
    hang = bpy.data.objects.new("key_hang", None)
    bpy.context.scene.collection.objects.link(hang)
    turn = Matrix(((0.0, -1.0, 0.0), (0.0, 0.0, -1.0), (1.0, 0.0, 0.0)))
    middle = HOOK_Y - HOOK_WIRE + KEY_BOW_REST - 0.5 * KEY_LEN
    hang.matrix_world = Matrix.Translation(bv(0.0, middle, HOOK_CURL)) @ (
        BV @ turn @ BV.inverted()).to_4x4()
    hang.parent = cage
    return cage, hull, door, hang


# ---------------------------------------------------------------------------------------------
# The surface: a scan, shaped by the cage's own form, baked into one atlas

SURFACE = "rusty_metal_04"  # the Poly Haven scan the cage is made of, unless asked otherwise
SURFACE_SCALE = 0.3         # one copy of a scan covers its own scanned size times this
ATLAS = 2048
TEXTURES = os.path.join(OUT, "textures")
BARE_IRON = (0.32, 0.31, 0.3, 1.0)  # what wear lays bare: iron, a metal's reflectance
WEAR = 0.55                 # how far the wear goes toward it on what stands out most
GRIME = 0.6                 # how dark the grime makes the most sheltered crevice
INSIDE = 0.92               # how dark it is deep in the hollow, as a share
BUMP = 0.0006               # metres of relief the scan's height map makes, white over black


def surface_material(images, tile):
    """The cage's surface as a shader: the scan, worn bare where the form stands out, grimed where
    it is sheltered and dark deep inside. It writes the colour, the roughness and the metal into
    an emission for the bakes that take one, and keeps a BSDF with its relief for the normal's."""
    mat = bpy.data.materials.new("cage_bake")
    g = Graph(mat)
    coord = g.node("ShaderNodeTexCoord")
    mapping = g.node("ShaderNodeMapping")
    mapping.inputs["Scale"].default_value = (1.0 / tile,) * 3
    g.link(coord.outputs["Object"], mapping.inputs["Vector"])

    def tex(img):
        n = g.node("ShaderNodeTexImage", image=img, projection="BOX", projection_blend=0.3,
                   interpolation="Cubic")
        g.link(mapping.outputs["Vector"], n.inputs["Vector"])
        return n

    albedo, arm, height = tex(images["albedo"]), tex(images["arm"]), tex(images["height"])
    split = g.node("ShaderNodeSeparateColor")
    g.link(arm.outputs["Color"], split.inputs["Color"])
    near = g.node("ShaderNodeAmbientOcclusion", samples=16, only_local=True)
    near.inputs["Distance"].default_value = 0.004
    deep = g.node("ShaderNodeAmbientOcclusion", samples=16, only_local=False)
    deep.inputs["Distance"].default_value = 0.05
    geometry = g.node("ShaderNodeNewGeometry")
    crevice = g.math("SUBTRACT", 1.0, near.outputs["AO"])
    hollow = g.math("SUBTRACT", 1.0, deep.outputs["AO"])
    stands = g.node("ShaderNodeMapRange", clamp=True)
    stands.inputs["From Min"].default_value = 0.52
    stands.inputs["From Max"].default_value = 0.62
    g.link(geometry.outputs["Pointiness"], stands.inputs["Value"])
    wear = g.math("MULTIPLY", g.math("MULTIPLY", stands.outputs["Result"],
                                     g.math("SUBTRACT", 1.0, crevice)), WEAR)
    shade = g.math("MULTIPLY", g.math("SUBTRACT", 1.0, g.math("MULTIPLY", crevice, GRIME)),
                   g.math("SUBTRACT", 1.0, g.math("MULTIPLY", hollow, INSIDE)))
    colour = g.mix("RGBA", 1.0, g.mix("RGBA", wear, albedo.outputs["Color"], BARE_IRON), shade,
                   blend="MULTIPLY")
    rough = g.mix("FLOAT", wear, split.outputs["Green"], 0.3)
    metal = g.mix("FLOAT", wear, split.outputs["Blue"], 1.0)
    bump = g.node("ShaderNodeBump")
    bump.inputs["Distance"].default_value = BUMP
    g.link(height.outputs["Color"], bump.inputs["Height"])
    bsdf = g.node("ShaderNodeBsdfPrincipled")
    g.link(colour, bsdf.inputs["Base Color"])
    g.link(rough, bsdf.inputs["Roughness"])
    g.link(metal, bsdf.inputs["Metallic"])
    g.link(bump.outputs["Normal"], bsdf.inputs["Normal"])
    emit = g.node("ShaderNodeEmission")
    out = g.node("ShaderNodeOutputMaterial")
    target = g.node("ShaderNodeTexImage")
    g.tree.nodes.active = target
    return mat, {"colour": colour, "rough": rough, "metal": metal}, bsdf, emit, out, target


def texture(objects, surface):
    """The cage's parts' surface baked into one atlas, saved under TEXTURES, and drawn from it."""
    images, tile = scan(surface, SURFACE_SCALE)
    baked = bake_atlas(objects, surface_material(images, tile), "cage", TEXTURES, ATLAS, 0.002)
    assign_material(objects, baked_material("cage_iron", baked))


# ---------------------------------------------------------------------------------------------
# What the export is held to


def self_check_cage(path):
    js, _ = read_glb(path)
    nodes = js["nodes"]
    by_name = {n.get("name", ""): i for i, n in enumerate(nodes)}
    children = ("cage_shell", "cage_door", "key_hang")
    problems = [f"no node '{n}'" for n in ("cage",) + children if n not in by_name]
    if not problems:
        under = nodes[by_name["cage"]].get("children", [])
        problems = [f"'{n}' is not a child of 'cage'" for n in children if by_name[n] not in under]
    if problems:
        sys.exit("self-check:\n  " + "\n  ".join(problems))
    print(f"self-check: cage, shell, door and key hang; {triangles(js)} triangles, "
          f"{os.path.getsize(path) / 1024:.0f} KiB", flush=True)


# ---------------------------------------------------------------------------------------------
# Stills, to look at the cage without the engine


def key_stand_in(hang):
    """A rough brass key where the game will hang the real one, for the stills only."""
    mat = bpy.data.materials.new("preview_brass")
    bsdf = next(n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Base Color"].default_value = (0.78, 0.56, 0.22, 1.0)
    bsdf.inputs["Metallic"].default_value = 1.0
    bsdf.inputs["Roughness"].default_value = 0.35
    parts = Parts()
    ring = [Vector((0.0, 0.0115 * math.sin(t), -0.035 + 0.0115 * math.cos(t)))
            for t in (2.0 * math.pi * k / 20 for k in range(20))]
    parts.add(*tube(ring, 0.0035, 6, closed=True))
    parts.add(*tube([Vector((0.0, 0.0, -0.02)), Vector((0.0, 0.0, 0.05))], 0.004, 8))
    for y0, y1, z0, z1 in ((0.0, 0.016, 0.032, 0.038), (0.0, 0.009, 0.038, 0.042),
                           (0.0, 0.016, 0.042, 0.05)):
        verts = [Vector((x, y, z)) for x in (-0.0025, 0.0025) for y in (y0, y1) for z in (z0, z1)]
        parts.add(verts, [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4),
                          (1, 5, 7, 3)])
    me = bpy.data.meshes.new("preview_key")
    # In the key's own frame, which `hang` places in the cage.
    me.from_pydata([BV @ v for v in parts.v], [], parts.f)
    me.materials.append(mat)
    ob = bpy.data.objects.new("preview_key", me)
    bpy.context.scene.collection.objects.link(ob)
    ob.matrix_world = hang.matrix_world
    return ob


def preview(door, hang, surface, width=520):
    """Stills lit as the basement lights it: a dark room, and a torch held beside the eye."""
    key_stand_in(hang)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    use_gpu()
    scene.cycles.samples = 96
    scene.cycles.use_denoising = True
    scene.render.resolution_x, scene.render.resolution_y = width, width
    scene.render.film_transparent = False
    world = scene.world or bpy.data.worlds.new("w")
    scene.world = world
    world.use_nodes = True
    background = next(n for n in world.node_tree.nodes if n.type == "BACKGROUND")
    background.inputs["Color"].default_value = (0.03, 0.03, 0.035, 1.0)
    background.inputs["Strength"].default_value = 1.0
    cam_data = bpy.data.cameras.new("preview")
    cam_data.type = "ORTHO"
    cam = bpy.data.objects.new("preview", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam
    torch_data = bpy.data.lights.new("torch", "SPOT")
    torch_data.energy = 30.0
    torch_data.spot_size = math.radians(30.0)
    torch_data.spot_blend = 0.6
    torch_data.shadow_soft_size = 0.02
    torch = bpy.data.objects.new("torch", torch_data)
    scene.collection.objects.link(torch)
    os.makedirs(os.path.join(OUT, "preview"), exist_ok=True)

    def shoot(eye, target, scale, stem):
        cam_data.ortho_scale = scale
        cam.location = bv(*eye)
        cam.rotation_euler = (bv(*target) - bv(*eye)).to_track_quat("-Z", "Y").to_euler()
        # The torch a little right of and under the eye, aimed where it looks.
        side = (target - eye).cross(Vector((0.0, 1.0, 0.0))).normalized()
        at = eye + side * 0.25 - Vector((0.0, 0.2, 0.0))
        torch.location = bv(*at)
        torch.rotation_euler = (bv(*target) - bv(*at)).to_track_quat("-Z", "Y").to_euler()
        return rendered(os.path.join(OUT, "preview", f"{surface}_{stem}.png"))

    mid = Vector((0.0, 0.13, 0.0))
    views = [("front", Vector((0.0, 0.13, 2.0))), ("threeq", Vector((-1.3, 0.8, 1.3))),
             ("side", Vector((-2.0, 0.13, 0.0))), ("above", Vector((0.0, 2.0, 0.6)))]
    shut = [shoot(e, mid, 0.3, f"shut_{n}") for n, e in views]
    door_at = Vector((0.0, DOOR_Y, 0.06))
    close = shoot(door_at + Vector((0.0, 0.0, 2.0)), door_at, 0.085, "face")
    door.rotation_euler = (0.0, 0.0, DOOR_OPEN)
    bpy.context.view_layer.update()
    opened = [shoot(e, mid, 0.3, f"open_{n}") for n, e in views[:2]]
    opened.append(shoot(Vector((-0.6, DOOR_Y + 0.25, 2.0)), door_at, 0.13, "open_close"))
    door.rotation_euler = (0.0, 0.0, 0.0)
    save_sheet(shut + opened + [close], len(shut),
               os.path.join(OUT, "preview", f"sheet_{surface}.png"))


# ---------------------------------------------------------------------------------------------


def args_after_dashes():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--surface", default=SURFACE,
                    help="the Poly Haven scan the cage is made of (a texture's id)")
    ap.add_argument("--preview", action="store_true", help="render stills of the cage")
    ap.add_argument("--no-export", action="store_true", help="build and preview only")
    return ap.parse_args(blender_argv())


def main():
    args = args_after_dashes()
    os.makedirs(OUT, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    cage, hull, door, hang = build_cage()
    tris = sum(len(p.vertices) - 2 for ob in (cage, hull, door) for p in ob.data.polygons)
    print(f"cage: {tris} triangles", flush=True)
    texture([cage, hull, door], args.surface)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, "birdcage.blend"))
    if args.no_export:
        print("not exported", flush=True)
    else:
        export_model(CAGE_GLB, [cage, hull, door, hang])
        self_check_cage(CAGE_GLB)
        print(f"wrote {CAGE_GLB}", flush=True)
    # Last, since it leaves a camera, a world and a stand-in key in the scene.
    if args.preview:
        preview(door, hang, args.surface)


if __name__ == "__main__":
    main()
