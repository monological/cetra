"""silent's flashlight: the army chest lamp the player clips to their jacket (spec 13.55), made
from the model as it was downloaded, sized and placed for the game.

    /Applications/Blender.app/Contents/MacOS/Blender -b --factory-startup \
        -P apps/silent/tools/make_flashlight_blender.py -- [--preview]

Reads assets/models/army_flashlight.glb, which says who made it and under what licence, and
writes:

  - assets/models/flashlight.glb: the lamp as one mesh, LAMP_HEIGHT tall, its lens toward +z, the
    back of its clip at its origin level with the lens's middle, and a node `lens` at the lens's
    middle, where the beam starts. The source's credit is in the file's copyright;
  - with --preview, stills of it from four sides into out/flashlight_blender/sheet.png.

MODEL space is the game's: y up, the lens toward +z. The source comes in no stated unit, about 620
of them tall, with its lens toward Blender's -y -- which is the game's +z once exported -- so it
is only scaled and moved.
"""

import argparse
import os
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glb import read_glb  # noqa: E402 -- the tools' own module, found beside this one

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
SOURCE = os.path.join(ROOT, "assets", "models", "army_flashlight.glb")
GLB = os.path.join(ROOT, "assets", "models", "flashlight.glb")
OUT = os.path.join(ROOT, "out", "flashlight_blender")
LAMP_HEIGHT = 0.15     # metres from its foot to the top of the loop it hangs by
LENS_PART = "FlashLight_16"  # the source's part that is the lens's glass


def credit(path):
    notes = read_glb(path)[0].get("asset", {}).get("extras", {})
    return (f"Flashlight: \"{notes.get('title', '')}\" by {notes.get('author', '')}, "
            f"{notes.get('source', '')}, {notes.get('license', '')}; resized.")


def bounds(objects):
    lo, hi = Vector((1e30,) * 3), Vector((-1e30,) * 3)
    for o in objects:
        for c in o.bound_box:
            w = o.matrix_world @ Vector(c)
            lo, hi = Vector(map(min, lo, w)), Vector(map(max, hi, w))
    return lo, hi


def build():
    """The source's parts joined into one object `flashlight` in metres, and the empty `lens`."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=SOURCE)
    parts = [o for o in bpy.data.objects if o.type == "MESH"]
    glass = [o for o in parts if o.parent and o.parent.name.startswith(LENS_PART)]
    if len(glass) != 1:
        sys.exit(f"no single '{LENS_PART}' part in {SOURCE} to be the lens")
    lo, hi = bounds(parts)
    g_lo, g_hi = bounds(glass)
    # The lens's middle on its face, and the origin: the back of the clip, level with it.
    lens = Vector(((g_lo.x + g_hi.x) * 0.5, g_lo.y, (g_lo.z + g_hi.z) * 0.5))
    origin = Vector((lens.x, hi.y, lens.z))
    scale = LAMP_HEIGHT / (hi.z - lo.z)
    to_metres = Matrix.Scale(scale, 4) @ Matrix.Translation(-origin)
    for o in parts:
        world = o.matrix_world.copy()
        o.data.transform(to_metres @ world)
        if world.determinant() < 0.0:
            bm = bmesh.new()
            bm.from_mesh(o.data)
            bmesh.ops.reverse_faces(bm, faces=bm.faces)
            bm.to_mesh(o.data)
            bm.free()
        o.parent = None
        o.matrix_world = Matrix.Identity(4)
    for o in [o for o in bpy.data.objects if o.type != "MESH"]:
        bpy.data.objects.remove(o)
    with bpy.context.temp_override(active_object=parts[0], selected_editable_objects=parts):
        bpy.ops.object.join()
    lamp = parts[0]
    lamp.name = lamp.data.name = "flashlight"
    empty = bpy.data.objects.new("lens", None)
    empty.location = to_metres @ lens
    bpy.context.scene.collection.objects.link(empty)
    size = (hi - lo) * scale
    print(f"flashlight: {size.x * 100:.1f} x {size.z * 100:.1f} cm, {size.y * 100:.1f} cm deep, "
          f"the lens {abs(empty.location.y) * 100:.1f} cm out from the clip's back", flush=True)
    return lamp, empty


def export(path):
    props = bpy.ops.export_scene.gltf.get_rna_type().properties.keys()
    want = dict(
        filepath=path, export_format="GLB", export_yup=True, use_selection=False,
        export_apply=True, export_animations=False, export_materials="EXPORT",
        export_normals=True, export_texcoords=True, export_tangents=True,
        export_image_format="AUTO", export_cameras=False, export_lights=False,
        export_extras=False, export_copyright=credit(SOURCE),
    )
    missing = [k for k in want if k not in props]
    if missing:
        print(f"export: this Blender has no {', '.join(missing)}; left at its defaults", flush=True)
    bpy.ops.export_scene.gltf(**{k: v for k, v in want.items() if k in props})


def self_check(path):
    js, _ = read_glb(path)
    names = {n.get("name") for n in js["nodes"]}
    problems = [f"no node '{n}'" for n in ("flashlight", "lens") if n not in names]
    if not js.get("asset", {}).get("copyright"):
        problems.append("no credit in its copyright")
    if problems:
        sys.exit("self-check:\n  " + "\n  ".join(problems))
    tris = sum(js["accessors"][p["indices"]]["count"] // 3
               for m in js["meshes"] for p in m["primitives"])
    print(f"self-check: {tris} triangles, {os.path.getsize(path) / 1024:.0f} KiB", flush=True)


def preview(lamp):
    """The lamp from the front, the side, above and three-quarters, tiled into one sheet."""
    import numpy as np

    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 64
    scene.cycles.use_denoising = True
    scene.render.resolution_x = scene.render.resolution_y = 512
    world = bpy.data.worlds.new("preview")
    scene.world = world
    world.use_nodes = True
    bg = next(n for n in world.node_tree.nodes if n.type == "BACKGROUND")
    bg.inputs["Color"].default_value = (0.3, 0.3, 0.32, 1.0)
    cam = bpy.data.objects.new("eye", bpy.data.cameras.new("eye"))
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = 1.15 * LAMP_HEIGHT
    scene.collection.objects.link(cam)
    scene.camera = cam
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 3.0
    sun.rotation_euler = (0.7, 0.2, -0.6)
    scene.collection.objects.link(sun)
    lo, hi = bounds([lamp])
    mid = (lo + hi) * 0.5
    os.makedirs(OUT, exist_ok=True)
    tiles = []
    for name, d in (("front", Vector((0.0, -1.0, 0.0))), ("side", Vector((1.0, 0.0, 0.0))),
                    ("top", Vector((0.0, 0.0, 1.0))),
                    ("three", Vector((0.8, -1.0, 0.5)).normalized())):
        cam.location = mid + d
        cam.rotation_euler = (mid - cam.location).to_track_quat("-Z", "Y").to_euler()
        path = os.path.join(OUT, f"{name}.png")
        scene.render.filepath = path
        bpy.ops.render.render(write_still=True)
        img = bpy.data.images.load(path)
        tiles.append(np.array(img.pixels[:], dtype=np.float32).reshape(
            img.size[1], img.size[0], 4))
        bpy.data.images.remove(img)
    sheet = np.concatenate(tiles, axis=1)
    out = bpy.data.images.new("sheet", sheet.shape[1], sheet.shape[0], alpha=True)
    out.pixels.foreach_set(sheet.ravel())
    out.filepath_raw = os.path.join(OUT, "sheet.png")
    out.file_format = "PNG"
    out.save()
    print(f"preview: {out.filepath_raw}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--preview", action="store_true", help="stills into out/flashlight_blender")
    args = ap.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    lamp, _ = build()
    export(GLB)
    self_check(GLB)
    print(f"wrote {GLB}", flush=True)
    if args.preview:
        preview(lamp)


main()
