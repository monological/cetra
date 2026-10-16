"""silent's motorboat (spec 13.56): the user's downloaded rowboat with an old outboard clamped on its
transom, the outboard modelled here.

    /Applications/Blender.app/Contents/MacOS/Blender -b --factory-startup \
        -P apps/silent/tools/make_boat_blender.py -- [--preview] [--no-export]

Reads assets/models/wooden_boat.glb, which says who made it and under what licence, and writes:

  - assets/models/boat.glb: the boat, its outboard's parts that move, and where the game seats
    the player and ties the boat up. The hull's credit is in the file's copyright;
  - apps/silent/src/boat_shape.h, GENERATED: the boat's draft, where no water may show inside it
    as the engine's dry shapes take them, and its rim in plan for what it bumps into. Rerun this
    rather than edit it;
  - out/boat_blender/boat.blend, to open and look at, and with --preview, stills tiled into
    out/boat_blender/sheet.png.

MODEL space is the game's: x the boat's left, y up, z toward the bow, the origin on the design
waterline at the hull's middle. The source comes in centimetres, z up, its bow toward Blender's
-y, which is already the game's +z, so it is only scaled and moved.

THE HULL is the source's, its maps kept, its roughness and metal map cut to MASK_SIZE.

THE OUTBOARD is an old two-stroke tiller motor of about six horsepower, after the user's
reference photo: a rounded two-tone cowl, a pull-start handle on its front, a clamp bracket over
the transom, a long leg, a cavitation plate, a gearcase with its skeg and a three-bladed
propeller. Its file nodes, under `boat`:

  - `bracket`, the clamp over the transom, which does not turn;
  - `motor`, everything that turns with the tiller, its origin on the swivel's axis at the
    bracket's top, turning about its own y;
  - `prop`, a child of `motor`, its origin on the propshaft, spinning about its own z;
  - `cord`, a child of `motor`, the start handle at rest, pulled along its own +z;
  - `cord_rope`, a child of `motor`, the cord out of the cowl to the handle: one metre along its
    own +z, scaled to how far the handle is out.

And empties: `seat`, the seated eye on the stern seat; `bow_eye`, where a rope ties on.

A red fuel tank stands on the floor before the stern seat, its hose to the motor, in `tank`.

ITS SURFACE is baked, as the birdcage's is: every part of the outboard and the tank unwrapped
together into one atlas, and a shader that takes a CC0 scan of painted metal from Poly Haven
(`green_metal_rust`), keeps its scratches, streaks and relief and gives each part its own paint
-- worn to bare aluminium on what stands out, grimed in crevices, scummed below the waterline.
"""

import argparse
import math
import os
import re
import statistics
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# The tools' own modules, found beside this one.
from blender_kit import (Graph, Parts, assign_material, bake_atlas, baked_material,  # noqa: E402
                         blender_argv, bv, export_model, finish, import_glb, lathe,
                         mean_brightness, mv, placed, rendered, save_sheet, scan, shrink_masks,
                         srgb, sweep, tube, use_gpu)
from glb import credit, read_glb, triangles  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
SOURCE = os.path.join(ROOT, "assets", "models", "wooden_boat.glb")
GLB = os.path.join(ROOT, "assets", "models", "boat.glb")
HEADER = os.path.join(ROOT, "apps", "silent", "src", "boat_shape.h")
WATER_DRY = os.path.join(ROOT, "cetra", "shaders", "include", "water_dry_constants.glsl")
OUT = os.path.join(ROOT, "out", "boat_blender")
TEXTURES = os.path.join(OUT, "textures")

DRAFT = 0.11            # metres of hull under the water: a man, a motor and a tank in it
DRY_DEPTH = 0.025       # the dry shapes are fitted to the hull this far under the waterline,
DRY_INSET = 0.005       # and this far inside its skin, so they stay in while it heaves
PLANK = 0.028           # the planking's thickness at the waterline, outer skin to inner
SEAT_EYE = 0.78         # a seated eye over the seat's top
SEAT_ASIDE = 0.12       # and to the left of the middle, the tiller under the right hand
SEAT_FORWARD = 0.14     # and forward of the seat's middle, sitting on its fore edge


# ---------------------------------------------------------------------------------------------
# The hull


def hull_import():
    """The source's hull in model metres, one object `hull`, and its measures."""
    imported = list(import_glb(SOURCE).values())
    parts = [o for o in imported if o.type == "MESH"]
    for o in parts:
        world = o.matrix_world.copy()
        o.data.transform(world)
        o.parent = None
        o.matrix_world = Matrix.Identity(4)
    for o in [o for o in imported if o.type != "MESH"]:
        bpy.data.objects.remove(o)
    if len(parts) > 1:
        with bpy.context.temp_override(active_object=parts[0], selected_editable_objects=parts):
            bpy.ops.object.join()
    hull = parts[0]
    hull.name = hull.data.name = "hull"
    ys = [v.co.y for v in hull.data.vertices]
    middle = 0.5 * (min(ys) + max(ys))
    hull.data.transform(Matrix.Translation((0.0, -0.01 * middle, -DRAFT)) @ Matrix.Scale(0.01, 4))
    for mat in hull.data.materials:
        shrink_masks(mat)
    return hull


def model_points(ob):
    return [mv(v.co) for v in ob.data.vertices]


def section(ob, y):
    """The hull cut level at model height `y`: the cut's points in model space."""
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    cut = bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:],
                                 plane_co=(0.0, 0.0, y), plane_no=(0.0, 0.0, 1.0))
    pts = [mv(e.co) for e in cut["geom_cut"] if isinstance(e, bmesh.types.BMVert)]
    bm.free()
    return pts


def plan_hull(pts):
    """The convex hull in plan, (x, z), of points and their mirror images across the keel, round
    anticlockwise. The hull's plan and its waterline are convex, and its vertices stand 20 to 30
    cm apart along it, so between them only the hull says where the skin is."""
    xz = sorted(set((round(s * p.x, 5), round(p.z, 5)) for p in pts for s in (1.0, -1.0)))

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    lower, upper = [], []
    for q in xz:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], q) <= 0.0:
            lower.pop()
        lower.append(q)
    for q in reversed(xz):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], q) <= 0.0:
            upper.pop()
        upper.append(q)
    return lower[:-1] + upper[:-1]


def half_width(poly, z):
    """How far a convex plan outline reaches across at `z`."""
    best = 0.0
    for i in range(len(poly)):
        (x0, z0), (x1, z1) = poly[i], poly[(i + 1) % len(poly)]
        if min(z0, z1) <= z <= max(z0, z1) and z1 != z0:
            best = max(best, abs(x0 + (x1 - x0) * (z - z0) / (z1 - z0)))
    return best


def outline(pts, step=0.01):
    """The outer half-width of a level cut along z, in `step`s, off its convex hull."""
    poly = plan_hull(pts)
    zs = [z for _, z in poly]
    z0, z1 = min(zs), max(zs)
    n = int((z1 - z0) / step) + 1
    grid = [z0 + i * step for i in range(n)]
    return grid, [half_width(poly, z) for z in grid]


def stern_seat(hull):
    """The stern seat's top: the aftmost wide upward face between 15 and 38 cm over the keel."""
    bm = bmesh.new()
    bm.from_mesh(hull.data)
    best = None
    keel = min(v.co.z for v in bm.verts)
    for f in bm.faces:
        c = f.calc_center_median()
        up = c.z - keel
        if f.normal.z > 0.95 and 0.15 < up < 0.38 and f.calc_area() > 0.001:
            z = -c.y
            if best is None or z < best[1]:
                best = (c.z, z)
    bm.free()
    return best  # (top height, model z)


def transom(points):
    """The transom's top: its height, and the model z of its aft face there."""
    aft = min(p.z for p in points)
    top = [p for p in points if p.z < aft + 0.03]
    return max(p.y for p in top), aft


def gunwale(points, stations=12):
    """The gunwale's height, what the boat rests on turned over: the median of the hull's tops at
    `stations` along it. The source's rail is a few vertices tens of centimetres apart, so a
    station can miss them and see only the frames, and the stem rises over the rest."""
    zs = [p.z for p in points]
    z0, z1 = min(zs), max(zs)
    tops = []
    for i in range(stations):
        a, b = z0 + (z1 - z0) * i / stations, z0 + (z1 - z0) * (i + 1) / stations
        ys = [p.y for p in points if a <= p.z < b]
        if ys:
            tops.append(max(ys))
    return statistics.median(tops)


# ---------------------------------------------------------------------------------------------
# Shapes, in model space

def superellipse_ring(cx, cz, hw, hl, y, p, n=40):
    """A ring of points at height y, round a superellipse hw across and hl along."""
    ring = []
    for i in range(n):
        t = 2.0 * math.pi * i / n
        c, s = math.cos(t), math.sin(t)
        x = hw * math.copysign(abs(c) ** (2.0 / p), c)
        z = hl * math.copysign(abs(s) ** (2.0 / p), s)
        ring.append(Vector((cx + x, y, cz + z)))
    return ring


def airfoil_ring(cz, chord, half, y, n=32):
    """A symmetric NACA section at height y, its leading edge at cz and running aft (-z),
    `half` its greatest half-thickness."""
    ring = []
    for i in range(n):
        u = 0.5 - 0.5 * math.cos(2.0 * math.pi * i / n)  # 0 at the nose, 1 at the tail, and back
        side = 1.0 if i < n / 2 else -1.0
        t = 5.0 * (0.2969 * math.sqrt(u) - 0.126 * u - 0.3516 * u * u + 0.2843 * u ** 3 -
                   0.1036 * u ** 4) / 0.5
        ring.append(Vector((side * half * t, y, cz - chord * u)))
    return ring


def lathe_z(profile, x, y, segs=24):
    """A solid of revolution about a line along z through (x, y), from (radius, z) points; an end
    of radius 0 closes it on the axis."""
    rings = []
    for r, z in profile:
        rings.append([Vector((x + r * math.cos(2 * math.pi * k / segs),
                              y + r * math.sin(2 * math.pi * k / segs), z)) for k in range(segs)])
    return sweep(rings)


def box(lo, hi):
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    v = [Vector(c) for c in ((x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
                             (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1))]
    f = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    return v, f


def fin(outline_zy, x0, x1):
    """A flat plate in the yz plane from x0 to x1 over a convex outline of (z, y) points."""
    a = [Vector((x0, y, z)) for z, y in outline_zy]
    b = [Vector((x1, y, z)) for z, y in outline_zy]
    return sweep([a, b])


# ---------------------------------------------------------------------------------------------
# The outboard, in its own frame: the swivel's axis on y, its top at y = 0, the bow toward +z.

# Paint, as (colour, roughness, metal). Colours are sRGB hex.
COWL = ("a9adaa", 0.42, 0.0)    # the hood's pale grey
SEAL = ("141414", 0.7, 0.0)     # the rubber where hood meets pan
PAN = ("2e3133", 0.5, 0.0)      # the lower cowl and the leg's dark grey
LEG = ("3a3d3f", 0.48, 0.0)
CAST = ("4a4c4c", 0.55, 0.3)    # the bracket's cast aluminium, painted and worn
RUBBER = ("101010", 0.85, 0.0)
PROP = ("8a8a86", 0.35, 1.0)    # bare aluminium
TANK_RED = ("7a1810", 0.45, 0.0)

COWL_Z = -0.11   # the hood's middle, aft of the swivel
PAN_Y = (0.035, 0.115)  # the lower cowl, over the bracket's top
HOOD_Y0 = 0.115
CORD_Y = 0.2     # the start handle's height on the hood's front
LEG_TOP = 0.04
PLATE_Y = -0.4   # the cavitation plate, under the waterline once clamped on
GEAR_Y = -0.475  # the propshaft's height
PROP_R = 0.1


def part(parts, shape, paint):
    parts.setdefault(paint, Parts()).add(*shape)


def hood(parts):
    """The hood: a rounded box, its sides near upright and its top flat, its edges rounded, as an
    old two-stroke's were."""
    rings = []
    for y, hw, hl, p in ((HOOD_Y0, 0.112, 0.168, 4.6), (HOOD_Y0 + 0.05, 0.116, 0.172, 4.6),
                         (HOOD_Y0 + 0.13, 0.114, 0.17, 4.4), (HOOD_Y0 + 0.17, 0.108, 0.162, 4.0),
                         (HOOD_Y0 + 0.19, 0.096, 0.148, 3.6), (HOOD_Y0 + 0.2, 0.07, 0.115, 3.2)):
        rings.append(superellipse_ring(0.0, COWL_Z, hw, hl, y, p))
    part(parts, sweep(rings), COWL)
    # The seal round the hood's foot, standing just proud of it.
    seal = [superellipse_ring(0.0, COWL_Z, 0.119, 0.175, y, 4.6) for y in (HOOD_Y0 - 0.008,
                                                                              HOOD_Y0 + 0.008)]
    part(parts, sweep(seal), SEAL)
    # The fuel cap on top, aft.
    cap = lathe([(0.0, 0.0), (0.022, 0.0), (0.022, 0.012), (0.0, 0.014)], 16)
    part(parts, placed(cap, Vector((0.0, HOOD_Y0 + 0.196, COWL_Z - 0.07))), RUBBER)


def pan(parts):
    rings = [superellipse_ring(0.0, COWL_Z + 0.005, hw, hl, y, 4.2)
             for y, hw, hl in ((PAN_Y[0], 0.095, 0.15), (PAN_Y[0] + 0.04, 0.108, 0.165),
                               (PAN_Y[1], 0.111, 0.168))]
    part(parts, sweep(rings), PAN)


def leg(parts):
    rings = []
    for y, chord, half in ((LEG_TOP + 0.01, 0.18, 0.045), (LEG_TOP - 0.1, 0.17, 0.04),
                           (PLATE_Y + 0.12, 0.155, 0.034), (PLATE_Y + 0.01, 0.145, 0.031),
                           (GEAR_Y + 0.03, 0.135, 0.027)):
        rings.append(airfoil_ring(-0.035, chord, half, y))
    part(parts, sweep(rings), LEG)
    # The swivel housing round the bracket's tube, top and bottom, part of what turns.
    sw = lathe([(0.0, -0.03), (0.034, -0.03), (0.034, 0.03), (0.0, 0.03)], 16)
    for y0 in (-0.02, -0.17):
        part(parts, placed(sw, Vector((0.0, y0, 0.0))), CAST)
    # The cavitation plate over the propeller.
    plate = [superellipse_ring(0.0, -0.12, 0.075, 0.125, y, 2.6) for y in (PLATE_Y - 0.004,
                                                                             PLATE_Y + 0.004)]
    part(parts, sweep(plate), LEG)


def gearcase(parts):
    torpedo = [(0.0, 0.05), (0.014, 0.046), (0.026, 0.03), (0.033, 0.0), (0.035, -0.05),
               (0.034, -0.14), (0.03, -0.185), (0.023, -0.2)]
    part(parts, lathe_z(torpedo, 0.0, GEAR_Y, 24), LEG)
    skeg = [(-0.02, GEAR_Y - 0.02), (-0.18, GEAR_Y - 0.022), (-0.15, GEAR_Y - 0.11),
            (-0.06, GEAR_Y - 0.1)]
    part(parts, fin(skeg, -0.004, 0.004), LEG)


def tiller(parts):
    """The arm forward from the pan's front into the boat, rising to the hand, its twist grip
    ribbed."""
    path = [Vector((0.0, 0.07, 0.02)), Vector((0.0, 0.09, 0.15)), Vector((0.0, 0.13, 0.3)),
            Vector((0.0, 0.16, 0.42))]
    part(parts, tube(path, 0.017, 10), PAN)
    grip = [Vector((0.0, 0.16, 0.42)), Vector((0.0, 0.18, 0.54))]
    part(parts, tube(grip, 0.022, 12), RUBBER)
    for i in range(6):
        z = 0.43 + 0.018 * i
        y = 0.16 + 0.02 * (z - 0.42) / 0.12
        ring = [Vector((0.0, y, z - 0.004)), Vector((0.0, y + 0.0013, z + 0.004))]
        part(parts, tube(ring, 0.0245, 12), RUBBER)
    # Where the arm meets the pan: a boss.
    part(parts, lathe_z([(0.0, -0.01), (0.03, -0.01), (0.03, 0.05), (0.0, 0.05)], 0.0, 0.07, 14),
         PAN)


def bracket(parts, transom_thick):
    """The clamp over the transom's top: a saddle on it, a plate down its aft face carrying the
    swivel tube, a plate down its inner face, and two T-handled screws through that."""
    aft = 0.05  # the swivel axis behind the transom's aft face
    part(parts, box((-0.07, -0.012, aft - 0.035), (0.07, 0.012, aft + transom_thick + 0.01)), CAST)
    part(parts, box((-0.07, -0.2, aft - 0.035), (0.07, 0.0, aft - 0.02)), CAST)
    part(parts, box((-0.07, -0.14, aft + transom_thick - 0.005),
                    (0.07, 0.0, aft + transom_thick + 0.01)), CAST)
    tube_pts = [Vector((0.0, 0.0, 0.0)), Vector((0.0, -0.2, 0.0))]
    part(parts, tube(tube_pts, 0.022, 14), CAST)
    for x in (-0.045, 0.045):
        z = aft + transom_thick + 0.01
        screw = [Vector((x, -0.09, z)), Vector((x, -0.09, z + 0.06))]
        part(parts, tube(screw, 0.007, 8), CAST)
        handle = [Vector((x - 0.035, -0.09, z + 0.06)), Vector((x + 0.035, -0.09, z + 0.06))]
        part(parts, tube(handle, 0.008, 8), CAST)


def propeller():
    """The hub and three blades round the propshaft, in the propeller's own frame: the shaft on z,
    the hub's front at z = 0."""
    parts = {}
    part(parts, lathe_z([(0.0, 0.0), (0.03, 0.0), (0.03, -0.05), (0.02, -0.075), (0.0, -0.085)],
                        0.0, 0.0, 20), PROP)
    pitch = 0.2
    for b in range(3):
        phi0 = 2.0 * math.pi * b / 3.0
        front, back = [], []
        rs = [0.028 + (PROP_R - 0.028) * i / 6 for i in range(7)]
        for r in rs:
            beta = math.atan(pitch / (2.0 * math.pi * r))
            chord = 0.07 * math.sin(math.pi * (0.15 + 0.85 * (r - 0.028) / (PROP_R - 0.028)))
            chord = max(chord, 0.018)
            thick = 0.006 * (1.0 - 0.7 * (r - 0.028) / (PROP_R - 0.028))
            row_f, row_b = [], []
            for j in range(5):
                c = (j / 4.0 - 0.5) * chord
                phi = phi0 + c * math.cos(beta) / r
                z = -0.03 - c * math.sin(beta)
                n = Vector((-math.sin(phi) * math.sin(beta), math.cos(phi) * math.sin(beta),
                            math.cos(beta)))
                p = Vector((r * math.cos(phi), r * math.sin(phi), z))
                row_f.append(p + n * 0.5 * thick)
                row_b.append(p - n * 0.5 * thick)
            front.append(row_f)
            back.append(row_b)
        verts, faces = [], []
        rows, cols = len(rs), 5
        for grid, flip in ((front, False), (back, True)):
            o = len(verts)
            verts += [v for row in grid for v in row]
            for i in range(rows - 1):
                for j in range(cols - 1):
                    q = (o + i * cols + j, o + i * cols + j + 1, o + (i + 1) * cols + j + 1,
                         o + (i + 1) * cols + j)
                    faces.append(q[::-1] if flip else q)
        # The blade's edges and tip, front to back.
        nf = rows * cols
        for i in range(rows - 1):
            for j in (0, cols - 1):
                a, b2 = i * cols + j, (i + 1) * cols + j
                faces.append((a, b2, nf + b2, nf + a) if j == 0 else (b2, a, nf + a, nf + b2))
        for j in range(cols - 1):
            a, b2 = (rows - 1) * cols + j, (rows - 1) * cols + j + 1
            faces.append((a, b2, nf + b2, nf + a))
        part(parts, (verts, faces), PROP)
    return parts


def fuel_tank(parts, at):
    """A red steel tank of three gallons on the floor at `at`, its cap, its handle, and the hose."""
    x, y, z = at
    rings = [superellipse_ring(x, z, hw, hl, y + h, 5.0) for h, hw, hl in
             ((0.0, 0.11, 0.17), (0.012, 0.118, 0.178), (0.17, 0.118, 0.178), (0.19, 0.11, 0.17))]
    part(parts, sweep(rings), TANK_RED)
    cap = lathe([(0.0, 0.0), (0.03, 0.0), (0.03, 0.022), (0.0, 0.024)], 16)
    part(parts, placed(cap, Vector((x + 0.05, y + 0.19, z - 0.09))), RUBBER)
    handle = [Vector((x - 0.05, y + 0.19, z + 0.04)), Vector((x - 0.05, y + 0.25, z + 0.06)),
              Vector((x - 0.05, y + 0.25, z + 0.12)), Vector((x - 0.05, y + 0.19, z + 0.14))]
    part(parts, tube(handle, 0.009, 8), TANK_RED)


def build_outboard(hull, points):
    """The outboard clamped on the transom's middle, and the tank, as objects: the bracket, what
    turns, the propeller and the start handle and cord, each with its origin where it moves."""
    top, aft = transom(points)
    pivot = Vector((0.0, top, aft - 0.05))
    # How thick the transom is at its top: from its aft face to its inner face.
    inner = [p.z for p in points if abs(p.y - top) < 0.03 and abs(p.x) < 0.1 and p.z > aft + 0.005]
    thick = max(0.03, min(inner) - aft if inner else 0.03)

    def object_of(name, parts, origin):
        obs = []
        for paint, pp in parts.items():
            ob = finish(f"{name}_{paint[0]}", pp, None)
            paint_object(ob, paint)
            obs.append(ob)
        if len(obs) > 1:
            with bpy.context.temp_override(active_object=obs[0], selected_editable_objects=obs):
                bpy.ops.object.join()
        ob = obs[0]
        ob.name = ob.data.name = name
        ob.data.transform(Matrix.Translation(-bv(*origin)))
        ob.location = bv(*origin)
        return ob

    turning = {}
    hood(turning)
    pan(turning)
    leg(turning)
    gearcase(turning)
    tiller(turning)
    motor = object_of("motor", shifted(turning, pivot), pivot)

    fixed = {}
    bracket(fixed, thick)
    brk = object_of("bracket", shifted(fixed, pivot), pivot)

    hub = pivot + Vector((0.0, GEAR_Y, -0.2))
    prop = object_of("prop", shifted(propeller(), hub), hub)

    # The start handle on the hood's front, a T of black plastic on the cord's end.
    front = pivot + Vector((0.0, CORD_Y, COWL_Z + 0.172))
    handle = {}
    part(handle, tube([Vector((-0.038, 0.0, 0.01)), Vector((0.038, 0.0, 0.01))], 0.009, 10), RUBBER)
    part(handle, tube([Vector((0.0, 0.0, 0.0)), Vector((0.0, 0.0, 0.01))], 0.006, 8), RUBBER)
    cord = object_of("cord", shifted(handle, front), front)
    rope = {}
    part(rope, tube([Vector((0.0, 0.0, 0.0)), Vector((0.0, 0.0, 1.0))], 0.0025, 6), RUBBER)
    cord_rope = object_of("cord_rope", shifted(rope, front), front)
    seat_top, seat_z = stern_seat(hull)
    # The tank on the floor before the stern seat, left of the middle, standing on the highest of
    # the bottom under it so no corner goes through.
    tank = {}
    tx, tz = -0.12, seat_z + 0.45
    floor = max(p.y for p in points if abs(p.x - tx) < 0.12 and abs(p.z - tz) < 0.18 and p.y < 0.05)
    tank_at = (tx, floor + 0.01, tz)
    fuel_tank(tank, tank_at)
    # The hose from the tank's cap round to the pan's front.
    hose = [Vector((tank_at[0] + 0.05, tank_at[1] + 0.21, tank_at[2] - 0.09)),
            Vector((-0.15, tank_at[1] + 0.3, seat_z)),
            Vector((-0.08, top + 0.05, aft + 0.12)),
            pivot + Vector((-0.06, PAN_Y[0] + 0.05, COWL_Z + 0.17))]
    part(tank, tube(smooth_path(hose, 6), 0.008, 8), RUBBER)
    tank_ob = object_of("tank", tank, Vector((0.0, 0.0, 0.0)))
    return motor, brk, prop, cord, cord_rope, tank_ob, pivot, (seat_top, seat_z)


def shifted(parts, origin):
    """Parts built in a frame at `origin`, moved into model space."""
    out = {}
    for paint, pp in parts.items():
        q = Parts()
        q.v = [v + origin for v in pp.v]
        q.f = list(pp.f)
        out[paint] = q
    return out


def smooth_path(pts, per):
    """A Catmull-Rom through `pts`, `per` points a span."""
    out = []
    p = [pts[0]] + pts + [pts[-1]]
    for i in range(1, len(p) - 2):
        for k in range(per):
            t = k / per
            a, b, c, d = p[i - 1], p[i], p[i + 1], p[i + 2]
            out.append(0.5 * ((2 * b) + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t * t +
                              (-a + 3 * b - 3 * c + d) * t * t * t))
    out.append(pts[-1])
    return out


def paint_object(ob, paint):
    """A part's paint as attributes every corner of it carries, read by the baking shader."""
    colour = srgb(paint[0])
    me = ob.data
    att = me.color_attributes.new("paint", "FLOAT_COLOR", "CORNER")
    fin_ = me.color_attributes.new("finish", "FLOAT_COLOR", "CORNER")
    for i in range(len(me.loops)):
        att.data[i].color = (*colour, 1.0)
        fin_.data[i].color = (paint[1], paint[2], 0.0, 1.0)


# ---------------------------------------------------------------------------------------------
# The surface: a painted-metal scan, given each part's paint, worn and grimed, baked to one atlas

SURFACE = "green_metal_rust"
SURFACE_SCALE = 0.35
ATLAS = 2048
BARE = (0.55, 0.56, 0.56, 1.0)   # what wear lays bare: aluminium
WEAR = 0.6
GRIME = 0.55
SCUM = (0.06, 0.07, 0.04, 1.0)   # what grows under the water
BUMP = 0.0004
DETAIL = 1.6                     # the scan's brightness about its mean, raised to this
CHIPS = (0.72, 0.86)             # where the scan's roughness says its paint is worn through


def surface_material(images, tile, mean, waterline):
    """The paint, the scan's detail on it, worn, grimed and scummed; an emission for the colour,
    roughness and metal bakes and a BSDF with relief for the normal's."""
    mat = bpy.data.materials.new("outboard_bake")
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
    paint = g.node("ShaderNodeAttribute", attribute_type="GEOMETRY", attribute_name="paint")
    finish_ = g.node("ShaderNodeAttribute", attribute_type="GEOMETRY", attribute_name="finish")
    fsplit = g.node("ShaderNodeSeparateColor")
    g.link(finish_.outputs["Color"], fsplit.inputs["Color"])
    # The scan's scratches, streaks and stains as a brightness about its own mean, laid over the
    # part's paint with more contrast than the scan's green has.
    bw = g.node("ShaderNodeRGBToBW")
    g.link(albedo.outputs["Color"], bw.inputs["Color"])
    detail = g.math("POWER", g.math("DIVIDE", bw.outputs["Val"], mean, clamp=False), DETAIL,
                    clamp=False)
    tone = g.node("ShaderNodeCombineColor")
    for ch in ("Red", "Green", "Blue"):
        g.link(detail, tone.inputs[ch])
    shaded = g.mix("RGBA", 1.0, paint.outputs["Color"], tone.outputs["Color"], blend="MULTIPLY")
    argb = g.node("ShaderNodeSeparateColor")
    g.link(arm.outputs["Color"], argb.inputs["Color"])
    # Where the scan's paint is worn rough, chips down to the metal.
    chips = g.node("ShaderNodeMapRange", clamp=True)
    chips.inputs["From Min"].default_value = CHIPS[0]
    chips.inputs["From Max"].default_value = CHIPS[1]
    g.link(argb.outputs["Green"], chips.inputs["Value"])
    near = g.node("ShaderNodeAmbientOcclusion", samples=16, only_local=True)
    near.inputs["Distance"].default_value = 0.01
    geometry = g.node("ShaderNodeNewGeometry")
    crevice = g.math("SUBTRACT", 1.0, near.outputs["AO"])
    stands = g.node("ShaderNodeMapRange", clamp=True)
    stands.inputs["From Min"].default_value = 0.52
    stands.inputs["From Max"].default_value = 0.6
    g.link(geometry.outputs["Pointiness"], stands.inputs["Value"])
    paint_only = g.math("SUBTRACT", 1.0, fsplit.outputs["Green"])
    edges = g.math("MULTIPLY", g.math("MULTIPLY", stands.outputs["Result"],
                                      g.math("SUBTRACT", 1.0, crevice)), WEAR)
    wear = g.math("MULTIPLY", g.math("MAXIMUM", edges, chips.outputs["Result"]), paint_only)
    shade = g.math("SUBTRACT", 1.0, g.math("MULTIPLY", crevice, GRIME))
    worn = g.mix("RGBA", wear, shaded, BARE)
    # Scum under the waterline, fading up a hand's breadth over it.
    split = g.node("ShaderNodeSeparateXYZ")
    g.link(geometry.outputs["Position"], split.inputs["Vector"])
    under = g.node("ShaderNodeMapRange", clamp=True)
    under.inputs["From Min"].default_value = waterline + 0.04
    under.inputs["From Max"].default_value = waterline - 0.03
    g.link(split.outputs["Z"], under.inputs["Value"])
    scummed = g.mix("RGBA", g.math("MULTIPLY", under.outputs["Result"], 0.8), worn, SCUM)
    colour = g.mix("RGBA", 1.0, scummed, shade, blend="MULTIPLY")
    rough_paint = g.mix("FLOAT", 0.35, fsplit.outputs["Red"], argb.outputs["Green"])
    rough = g.mix("FLOAT", wear, rough_paint, 0.35)
    metal = g.mix("FLOAT", wear, fsplit.outputs["Green"], 1.0)
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


def texture(objects):
    """The outboard's and the tank's surface baked into one atlas, saved under TEXTURES, and drawn
    from it; the paint attributes the bake read dropped."""
    images, tile = scan(SURFACE, SURFACE_SCALE)
    # The parts stand where they will in the boat, whose waterline is Blender's z = 0.
    shader = surface_material(images, tile, mean_brightness(images["albedo"]), 0.0)
    baked = bake_atlas(objects, shader, "outboard", TEXTURES, ATLAS, 0.003)
    assign_material(objects, baked_material("outboard", baked))
    for ob in objects:
        for name in [a.name for a in ob.data.color_attributes]:
            ob.data.color_attributes.remove(ob.data.color_attributes[name])


# ---------------------------------------------------------------------------------------------
# Where no water may show: superellipses inside the hull's waterline, fitted greedily


def fit_dry(grid, width, count, coverage=0.99):
    """Up to `count` superellipses (centre z, half length, half width, power), each in turn the
    one covering most of what the others leave inside the planking, all inside the outer skin.
    Between the two skins the planks hide the water; inside the inner one is where it would
    show."""
    import numpy as np
    zs = np.array(grid)
    ws = np.maximum(np.array(width) - DRY_INSET, 0.0)
    inner = np.maximum(np.array(width) - PLANK, 0.0)

    def w_at(z):
        return np.interp(z, zs, ws, left=0.0, right=0.0)

    # The inside of the planking as points, to count what is covered.
    pts = []
    for z in np.linspace(zs[0], zs[-1], 160):
        w = float(np.interp(z, zs, inner))
        if w <= 0.0:
            continue
        for x in np.linspace(-w, w, 15):
            pts.append((x, z))
    pts = np.array(pts)
    covered = np.zeros(len(pts), dtype=bool)
    t = np.linspace(0.0, 2.0 * np.pi, 72, endpoint=False)
    c, s = np.cos(t), np.sin(t)
    # A unit superellipse of each power, its points across and along.
    rings = {p: (np.sign(c) * np.abs(c) ** (2.0 / p), np.sign(s) * np.abs(s) ** (2.0 / p))
             for p in (2.0, 2.4, 3.0, 4.0)}
    shapes = []
    for _ in range(count):
        best = None
        for cz in np.linspace(zs[0], zs[-1], 48):
            for hl in np.linspace(0.08, 0.5 * (zs[-1] - zs[0]), 32):
                for p, (ex, along) in rings.items():
                    # The widest it may be: every boundary point inside the outline.
                    room = w_at(cz + hl * along)
                    need = np.abs(ex)
                    ok = need > 1e-6
                    if np.any(room[~ok] <= 0.0):
                        continue
                    hw = float(np.min(room[ok] / need[ok])) if np.any(ok) else 0.0
                    if hw <= 0.02:
                        continue
                    inside = (np.abs(pts[:, 0] / hw) ** p + np.abs((pts[:, 1] - cz) / hl) ** p) <= 1.0
                    gain = int(np.sum(inside & ~covered))
                    if best is None or gain > best[0]:
                        best = (gain, (float(cz), float(hl), hw, p), inside)
        if best is None or best[0] == 0:
            break
        shapes.append(best[1])
        covered |= best[2]
        if covered.mean() >= coverage:
            break
    return shapes, float(covered.mean())


def rim(points, n=16):
    """The hull's outline in plan at its widest, as `n` points round it from the stern, along its
    left and back down its right: what it bumps into with."""
    poly = plan_hull(points)
    zs = [z for _, z in poly]
    # Just inside the ends, so the transom's width is in it and the stem's point is.
    z0, z1 = min(zs) + 0.005, max(zs) - 0.005
    half = [(half_width(poly, z0 + (z1 - z0) * i / (n // 2)), z0 + (z1 - z0) * i / (n // 2))
            for i in range(n // 2 + 1)]
    return half + [(-x, z) for x, z in reversed(half[1:-1])]


def water_dry_max():
    """How many dry shapes the engine's water takes, from the constants both its languages read."""
    with open(WATER_DRY) as f:
        return int(re.search(r"#define\s+WATER_DRY_MAX\s+(\d+)", f.read()).group(1))


def write_header(shapes, cover, rim_pts, extent, gunwale_y):
    lines = [
        "// GENERATED by apps/silent/tools/make_boat_blender.py from assets/models/wooden_boat.glb",
        "// (spec 13.56). Do not edit: rerun the tool.",
        "#ifndef _SILENT_BOAT_SHAPE_H_",
        "#define _SILENT_BOAT_SHAPE_H_",
        "",
        "// In the boat's model space: x its left, y up, z toward the bow, the origin on the design",
        "// waterline at the hull's middle.",
        f"#define BOAT_DRAFT     {DRAFT:.4f}f // metres of hull under the water",
        f"#define BOAT_BOW_Z     {extent[1]:.4f}f // the stem's foremost point",
        f"#define BOAT_STERN_Z   {extent[0]:.4f}f // the transom's aftmost",
        f"#define BOAT_HALF_BEAM {extent[2]:.4f}f",
        f"#define BOAT_GUNWALE_Y {gunwale_y:.4f}f // its top, which it rests on turned over",
        "",
        f"// Where no water may show inside the hull: superellipses round the z axis, inside its",
        f"// waterline {DRY_DEPTH * 100:.1f} cm under the design one; together they cover"
        f" {cover * 100:.1f}% of it.",
        "// Each is {centre z, half length, half width, power}.",
        f"#define BOAT_DRY_COUNT {len(shapes)}",
        "static const float BOAT_DRY[BOAT_DRY_COUNT][4] = {",
    ]
    for cz, hl, hw, p in shapes:
        lines.append(f"    {{{cz:.4f}f, {hl:.4f}f, {hw:.4f}f, {p:.2f}f}},")
    lines += [
        "};",
        "",
        "// The hull's outline in plan at its widest, round from the stern, as {x, z}.",
        f"#define BOAT_RIM_COUNT {len(rim_pts)}",
        "static const float BOAT_RIM[BOAT_RIM_COUNT][2] = {",
    ]
    for x, z in rim_pts:
        lines.append(f"    {{{x:.4f}f, {z:.4f}f}},")
    lines += ["};", "", "#endif // _SILENT_BOAT_SHAPE_H_", ""]
    with open(HEADER, "w") as f:
        f.write("\n".join(lines))
    print(f"wrote {HEADER}: {len(shapes)} dry shapes covering {cover * 100:.1f}%", flush=True)


# ---------------------------------------------------------------------------------------------
# Assembly, export and its checks


def empty(name, at, parent):
    e = bpy.data.objects.new(name, None)
    bpy.context.scene.collection.objects.link(e)
    e.location = bv(*at)
    e.parent = parent
    return e


def build():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    hull = hull_import()
    points = model_points(hull)
    motor, brk, prop, cord, rope, tank, pivot, seat = build_outboard(hull, points)
    texture([motor, brk, prop, cord, rope, tank])
    root = bpy.data.objects.new("boat", None)
    bpy.context.scene.collection.objects.link(root)
    for ob in (hull, brk, motor, tank):
        ob.parent = root
    for ob in (prop, cord, rope):
        world = ob.matrix_world.copy()
        ob.parent = motor
        ob.matrix_world = world
    rope.scale = (1.0, 0.001, 1.0)  # Blender's y is the model's -z: the cord in, at rest
    seat_top, seat_z = seat
    eye = empty("seat", (SEAT_ASIDE, seat_top + SEAT_EYE, seat_z + SEAT_FORWARD), root)
    bow = max(points, key=lambda p: p.z)
    stem = [p for p in points if p.z > bow.z - 0.12]
    bow_eye = empty("bow_eye", (0.0, max(p.y for p in stem) - 0.06, bow.z - 0.06), root)
    zs = [p.z for p in points]
    extent = (min(zs), max(zs), max(abs(p.x) for p in points))
    grid, width = outline(section(hull, -DRY_DEPTH))
    shapes, cover = fit_dry(grid, width, water_dry_max())
    write_header(shapes, cover, rim(points), extent, gunwale(points))
    print(f"boat: {extent[1] - extent[0]:.2f} m long, {2 * extent[2]:.2f} m in the beam; the "
          f"motor's swivel at z {pivot.z:.3f}, {pivot.y:.3f} over the water; the seat's eye at "
          f"z {seat_z + SEAT_FORWARD:.3f}, {seat_top + SEAT_EYE:.3f} over it", flush=True)
    return root, [hull, brk, motor, prop, cord, rope, tank, eye, bow_eye]


NODES = ("boat", "hull", "bracket", "motor", "prop", "cord", "cord_rope", "tank", "seat",
         "bow_eye")


def self_check(path):
    js, _ = read_glb(path)
    names = {n.get("name") for n in js["nodes"]}
    problems = [f"no node '{n}'" for n in NODES if n not in names]
    if not js.get("asset", {}).get("copyright"):
        problems.append("no credit in its copyright")
    if problems:
        sys.exit("self-check:\n  " + "\n  ".join(problems))
    print(f"self-check: {len(NODES)} nodes; {triangles(js)} triangles, "
          f"{os.path.getsize(path) / 1024:.0f} KiB", flush=True)


def preview():
    """The boat on a grey ground under a soft sky, from four sides and from the seat."""
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    use_gpu()
    scene.cycles.samples = 96
    scene.cycles.use_denoising = True
    scene.render.resolution_x, scene.render.resolution_y = 640, 480
    world = scene.world or bpy.data.worlds.new("w")
    scene.world = world
    world.use_nodes = True
    bg = next(n for n in world.node_tree.nodes if n.type == "BACKGROUND")
    bg.inputs["Color"].default_value = (0.55, 0.58, 0.6, 1.0)
    bg.inputs["Strength"].default_value = 0.8
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 2.5
    sun.data.angle = math.radians(8.0)
    sun.rotation_euler = (0.75, 0.15, -0.7)
    scene.collection.objects.link(sun)
    cam = bpy.data.objects.new("eye", bpy.data.cameras.new("eye"))
    cam.data.lens = 35
    scene.collection.objects.link(cam)
    scene.camera = cam
    os.makedirs(OUT, exist_ok=True)

    def shoot(eye, target, stem):
        cam.location = bv(*eye)
        cam.rotation_euler = (bv(*target) - bv(*eye)).to_track_quat("-Z", "Y").to_euler()
        return rendered(os.path.join(OUT, f"{stem}.png"))

    seat_m = mv(bpy.data.objects["seat"].matrix_world.translation)
    motor_m = mv(bpy.data.objects["motor"].matrix_world.translation)
    tiles = [
        shoot(Vector((-3.2, 1.6, -3.4)), Vector((0.0, 0.0, -0.4)), "aft_quarter"),
        shoot(Vector((-4.6, 0.6, 0.2)), Vector((0.0, 0.0, 0.0)), "side"),
        shoot(Vector((2.6, 2.4, 3.2)), Vector((0.0, -0.1, 0.0)), "bow_quarter"),
        shoot(motor_m + Vector((-1.1, 0.25, -0.9)), motor_m + Vector((0.0, -0.15, 0.0)), "motor"),
        shoot(seat_m, seat_m + Vector((0.0, -0.35, 1.0)), "seat_forward"),
        shoot(seat_m + Vector((0.0, 0.0, 0.05)), motor_m + Vector((0.0, 0.15, 0.0)), "seat_aft"),
    ]
    save_sheet(tiles, 3, os.path.join(OUT, "sheet.png"))


def args_after_dashes():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--preview", action="store_true", help="render stills of the boat")
    ap.add_argument("--no-export", action="store_true", help="build and preview only")
    return ap.parse_args(blender_argv())


def main():
    args = args_after_dashes()
    os.makedirs(OUT, exist_ok=True)
    root, objects = build()
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, "boat.blend"))
    if args.no_export:
        print("not exported", flush=True)
    else:
        export_model(GLB, [root] + objects,
                     credit(SOURCE, "Hull") + "; the outboard and tank modelled for silent.")
        self_check(GLB)
        print(f"wrote {GLB}", flush=True)
    if args.preview:
        preview()


if __name__ == "__main__":
    main()
