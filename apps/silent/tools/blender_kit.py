"""What silent's Blender tools share (spec 13.55): the model space they author in and Blender's, a
mesh built from model-space points, materials and Poly Haven's maps as images, a surface baked
into one atlas (spec 13.56), Cycles on the GPU, a sheet of stills, the glTF export and a glb's
objects by name.

For Blender's own Python only: it imports bpy. glb.py and polyhaven.py are the halves plain Python
can use too.
"""

import math
import os
import re
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector

from polyhaven import fetched, polyhaven_urls

# ---------------------------------------------------------------------------------------------
# Spaces


def bv(x, y, z):
    """A model-space point (x left, y up, z forward) in Blender's space."""
    return Vector((x, -z, y))


# The model-to-Blender turn as a matrix, for transforms rather than points.
BV = Matrix(((1.0, 0.0, 0.0), (0.0, 0.0, -1.0), (0.0, 1.0, 0.0)))


def mv(v):
    """A Blender-space vector in model space."""
    return Vector((v.x, v.z, -v.y))


def smooth(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3.0 - 2.0 * t)


def srgb(hexcode):
    """A hex colour's linear values, as a material takes them."""
    c = [int(hexcode[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]
    return tuple(x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c)


# ---------------------------------------------------------------------------------------------
# Meshes in model space


class Parts:
    """Vertices and faces gathered in model space for one object."""

    def __init__(self):
        self.v = []
        self.f = []

    def add(self, verts, faces):
        o = len(self.v)
        self.v += verts
        self.f += [tuple(i + o for i in face) for face in faces]


def tube(pts, r, segs=8, closed=False):
    """A round tube of radius `r` along model-space points, its rings turned by parallel
    transport so it never twists, capped at both ends when it is open."""
    n = len(pts)
    tangents = []
    for i in range(n):
        if closed:
            a, b = pts[i - 1], pts[(i + 1) % n]
        else:
            a, b = pts[max(i - 1, 0)], pts[min(i + 1, n - 1)]
        tangents.append((b - a).normalized())
    t0 = tangents[0]
    ref = Vector((0.0, 1.0, 0.0)) if abs(t0.y) < 0.9 else Vector((1.0, 0.0, 0.0))
    normal = (ref - t0 * ref.dot(t0)).normalized()
    verts = []
    for i in range(n):
        if i > 0:
            axis = tangents[i - 1].cross(tangents[i])
            if axis.length > 1e-9:
                normal = Matrix.Rotation(tangents[i - 1].angle(tangents[i]), 3,
                                         axis.normalized()) @ normal
            normal = (normal - tangents[i] * normal.dot(tangents[i])).normalized()
        binormal = tangents[i].cross(normal)
        for k in range(segs):
            a = 2.0 * math.pi * k / segs
            verts.append(pts[i] + (normal * math.cos(a) + binormal * math.sin(a)) * r)
    faces = []
    for i in range(n if closed else n - 1):
        j = (i + 1) % n
        for k in range(segs):
            k2 = (k + 1) % segs
            faces.append((i * segs + k, i * segs + k2, j * segs + k2, j * segs + k))
    if not closed:
        start, end = len(verts), len(verts) + 1
        verts += [pts[0], pts[-1]]
        last = (n - 1) * segs
        for k in range(segs):
            k2 = (k + 1) % segs
            faces.append((start, k2, k))
            faces.append((end, last + k, last + k2))
    return verts, faces


def lathe(profile, segs=48):
    """A solid of revolution about the y axis from (radius, height) points, bottom to top. An end
    of radius 0 closes that end on the axis; any other end is left open."""
    bottom = profile[0] if profile[0][0] == 0.0 else None
    top = profile[-1] if profile[-1][0] == 0.0 else None
    ring = profile[(1 if bottom else 0):len(profile) - (1 if top else 0)]
    verts = []
    for r, y in ring:
        for k in range(segs):
            a = 2.0 * math.pi * k / segs
            verts.append(Vector((r * math.sin(a), y, r * math.cos(a))))
    faces = []
    for i in range(len(ring) - 1):
        for k in range(segs):
            k2 = (k + 1) % segs
            faces.append((i * segs + k, i * segs + k2, (i + 1) * segs + k2, (i + 1) * segs + k))
    if bottom:
        c = len(verts)
        verts.append(Vector((0.0, bottom[1], 0.0)))
        faces += [(c, (k + 1) % segs, k) for k in range(segs)]
    if top:
        c, last = len(verts), (len(ring) - 1) * segs
        verts.append(Vector((0.0, top[1], 0.0)))
        faces += [(c, last + k, last + (k + 1) % segs) for k in range(segs)]
    return verts, faces


def sweep(rings, cap_start=True, cap_end=True):
    """Faces joining a list of equal-length rings, capped by a point at either end."""
    n = len(rings[0])
    verts = [p for r in rings for p in r]
    faces = []
    for i in range(len(rings) - 1):
        for k in range(n):
            k2 = (k + 1) % n
            faces.append((i * n + k, i * n + k2, (i + 1) * n + k2, (i + 1) * n + k))
    for at, index, flip in ((cap_start, 0, True), (cap_end, len(rings) - 1, False)):
        if not at:
            continue
        c = len(verts)
        verts.append(sum(rings[index], Vector()) / n)
        for k in range(n):
            k2 = (k + 1) % n
            f = (c, index * n + k, index * n + k2)
            faces.append(f[::-1] if flip else f)
    return verts, faces


def placed(shape, at):
    """A shape's vertices moved to `at`, its faces as they were."""
    verts, faces = shape
    return [v + at for v in verts], faces


def sphere(centre, r, rings=8, segs=12):
    profile = [(r * math.sin(math.pi * i / rings), -r * math.cos(math.pi * i / rings))
               for i in range(1, rings)]
    return placed(lathe([(0.0, -r)] + profile + [(0.0, r)], segs), centre)


def pin(r, half):
    """A short upright cylinder, capped, about its middle."""
    return lathe([(0.0, -half), (r, -half), (r, half), (0.0, half)], 12)


def mark_sharp(bm, sharp_degrees):
    """Every face smooth, but for edges sharper than `sharp_degrees` and open ones."""
    limit = math.radians(sharp_degrees)
    for f in bm.faces:
        f.smooth = True
    for e in bm.edges:
        if not e.is_manifold or e.calc_face_angle(0.0) > limit:
            e.smooth = False


def finish(name, parts, mat, sharp_degrees=35.0):
    """An object of the gathered model-space parts, in Blender space, smooth but for edges
    sharper than `sharp_degrees`."""
    me = bpy.data.meshes.new(name)
    me.from_pydata([bv(*v) for v in parts.v], [], parts.f)
    me.update()
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-7)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    mark_sharp(bm, sharp_degrees)
    bm.to_mesh(me)
    bm.free()
    if mat:
        me.materials.append(mat)
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def resmooth(ob, sharp_degrees=35.0):
    """Smooth again after a cut, but for edges sharper than `sharp_degrees`."""
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    mark_sharp(bm, sharp_degrees)
    bm.to_mesh(ob.data)
    bm.free()


def apply_boolean(ob, cutter):
    """`ob` with `cutter` taken out of it, the cutter removed."""
    mod = ob.modifiers.new("cut", "BOOLEAN")
    mod.operation = "DIFFERENCE"
    mod.solver = "EXACT"
    mod.object = cutter
    depsgraph = bpy.context.evaluated_depsgraph_get()
    cut = bpy.data.meshes.new_from_object(ob.evaluated_get(depsgraph))
    ob.modifiers.remove(mod)
    old = ob.data
    ob.data = cut
    bpy.data.meshes.remove(old)
    bpy.data.objects.remove(cutter)


# ---------------------------------------------------------------------------------------------
# Materials and their maps

# The engine packs every material's mask maps -- roughness, metal, occlusion -- into one texture
# array at its largest one's size, so a single large map takes every layer up with it: one 2048
# map made the array 1.26 GB. Every model's masks are made this size.
MASK_SIZE = 256


def material(name, colour, rough, metal=0.0):
    """A plain material of one colour, roughness and metal, and its shader."""
    m = bpy.data.materials.new(name)
    m.use_backface_culling = True
    bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Base Color"].default_value = (*colour, 1.0)
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = metal
    return m, bsdf


def polyhaven_images(name, keys, resolution):
    """A Poly Haven set's maps as Blender images by the caller's names for Poly Haven's keys,
    colour in sRGB and the rest as data; and the set's info."""
    urls, info = polyhaven_urls(name, keys, resolution)
    images = {}
    for kind, url in urls.items():
        img = bpy.data.images.load(fetched(url), check_existing=True)
        img.colorspace_settings.name = "sRGB" if kind == "albedo" else "Non-Color"
        images[kind] = img
    return images, info


def shrink(image, size):
    """`image` made no larger than `size` square, in place."""
    if image.size[0] > size or image.size[1] > size:
        image.scale(min(size, image.size[0]), min(size, image.size[1]))


def shrink_masks(mat, size=MASK_SIZE):
    """Every mask map in `mat` -- an image read as data, and not a normal map -- made no larger
    than `size` square."""
    for node in mat.node_tree.nodes:
        if node.type != "TEX_IMAGE" or not node.image:
            continue
        if node.image.colorspace_settings.name != "Non-Color":
            continue
        if any(link.to_node.type == "NORMAL_MAP" for link in node.outputs["Color"].links):
            continue
        shrink(node.image, size)


# ---------------------------------------------------------------------------------------------
# A surface baked into one atlas: a shader over a scan, unwrapped parts, Cycles


def sock(sockets, identifier):
    return next(s for s in sockets if s.identifier == identifier)


class Graph:
    """A material's node tree, built a node at a time."""

    def __init__(self, mat):
        self.tree = mat.node_tree
        self.tree.nodes.clear()

    def node(self, kind, **props):
        n = self.tree.nodes.new(kind)
        for k, v in props.items():
            setattr(n, k, v)
        return n

    def link(self, out, into):
        self.tree.links.new(out, into)

    def math(self, op, a, b, clamp=True):
        n = self.node("ShaderNodeMath", operation=op, use_clamp=clamp)
        for socket, value in zip(n.inputs, (a, b)):
            if isinstance(value, float):
                socket.default_value = value
            else:
                self.link(value, socket)
        return n.outputs[0]

    def mix(self, kind, factor, a, b, blend="MIX"):
        """A mix of two of `kind` (FLOAT or RGBA), `factor` of the way to `b`."""
        n = self.node("ShaderNodeMix", data_type=kind, blend_type=blend)
        suffix = "Float" if kind == "FLOAT" else "Color"
        for ident, value in (("Factor_Float", factor), ("A_" + suffix, a), ("B_" + suffix, b)):
            socket = sock(n.inputs, ident)
            if isinstance(value, (float, tuple)):
                socket.default_value = value
            else:
                self.link(value, socket)
        return sock(n.outputs, "Result_" + suffix)


def scan(name, scale):
    """A Poly Haven scan's colour, its packed occlusion, roughness and metal, and its height, as
    Blender images; and how many metres one copy of it covers, its scanned size times `scale`."""
    images, info = polyhaven_images(
        name, {"albedo": "Diffuse", "arm": "arm", "height": "Displacement"}, "2k")
    size = (info.get("dimensions") or [1000.0])[0] / 1000.0
    return images, size * scale


def mean_brightness(image):
    """An image's mean luminance, over its pixels as stored."""
    import numpy as np
    px = np.empty(len(image.pixels), dtype=np.float32)
    image.pixels.foreach_get(px)
    rgb = px.reshape(-1, 4)[:, :3]
    return float(np.mean(rgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)))


def unwrap(objects, margin):
    """Every object's faces laid out together in one atlas, at one texel density."""
    bpy.ops.object.select_all(action="DESELECT")
    for ob in objects:
        ob.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(60.0), island_margin=margin,
                             scale_to_bounds=False)
    bpy.ops.uv.pack_islands(margin=margin, rotate=True)
    bpy.ops.object.mode_set(mode="OBJECT")


def assign_material(objects, mat):
    """`mat` the one material on every face of each object."""
    for ob in objects:
        ob.data.materials.clear()
        ob.data.materials.append(mat)
        for p in ob.data.polygons:
            p.material_index = 0


def bake_atlas(objects, shader, prefix, outdir, size, margin):
    """`objects`' surface baked through `shader` -- (material, {colour, rough, metal} outputs, a
    BSDF with its relief, an emission, the output, the bake's target image node) -- into a
    colour, a normal, and a roughness and metal image packed as glTF reads them, at MASK_SIZE;
    each saved under `outdir` as `<prefix>_<map>.png`. The images by name."""
    import numpy as np
    mat, values, bsdf, emit, out, target = shader
    assign_material(objects, mat)
    unwrap(objects, margin)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    device = use_gpu()
    scene.cycles.samples = 64
    bake = scene.render.bake
    bake.margin = 16
    bake.margin_type = "EXTEND"
    os.makedirs(outdir, exist_ok=True)
    links = mat.node_tree.links

    def save(img, name):
        img.filepath_raw = os.path.join(outdir, f"{prefix}_{name}.png")
        img.file_format = "PNG"
        img.save()

    baked = {}
    for name, space in (("colour", "sRGB"), ("rough", "Non-Color"), ("metal", "Non-Color"),
                        ("normal", "Non-Color")):
        img = bpy.data.images.new(f"{prefix}_{name}", size, size, alpha=False)
        img.colorspace_settings.name = space
        target.image = img
        for link in list(out.inputs["Surface"].links) + list(emit.inputs["Color"].links):
            links.remove(link)
        if name == "normal":
            links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
            bpy.ops.object.bake(type="NORMAL", normal_space="TANGENT", use_clear=True)
        else:
            links.new(values[name], emit.inputs["Color"])
            links.new(emit.outputs["Emission"], out.inputs["Surface"])
            bpy.ops.object.bake(type="EMIT", use_clear=True)
        save(img, name)
        baked[name] = img
        print(f"baked {name} on {device}", flush=True)
    # Roughness in green and metal in blue, as glTF reads them from one image.
    px = size * size * 4
    rough = np.empty(px, dtype=np.float32)
    metal = np.empty(px, dtype=np.float32)
    baked["rough"].pixels.foreach_get(rough)
    baked["metal"].pixels.foreach_get(metal)
    packed = np.ones(px, dtype=np.float32)
    packed[1::4] = rough[0::4]
    packed[2::4] = metal[0::4]
    rm = bpy.data.images.new(f"{prefix}_rough_metal", size, size, alpha=False)
    rm.colorspace_settings.name = "Non-Color"
    rm.pixels.foreach_set(packed)
    shrink(rm, MASK_SIZE)
    save(rm, "rough_metal")
    return {"colour": baked["colour"], "rough_metal": rm, "normal": baked["normal"]}


def baked_material(name, images):
    """What the game draws: baked maps, wired as glTF exports them."""
    mat = bpy.data.materials.new(name)
    mat.use_backface_culling = True
    g = Graph(mat)
    bsdf = g.node("ShaderNodeBsdfPrincipled")
    out = g.node("ShaderNodeOutputMaterial")
    g.link(bsdf.outputs["BSDF"], out.inputs["Surface"])
    colour = g.node("ShaderNodeTexImage", image=images["colour"])
    g.link(colour.outputs["Color"], bsdf.inputs["Base Color"])
    rm = g.node("ShaderNodeTexImage", image=images["rough_metal"])
    split = g.node("ShaderNodeSeparateColor")
    g.link(rm.outputs["Color"], split.inputs["Color"])
    g.link(split.outputs["Green"], bsdf.inputs["Roughness"])
    g.link(split.outputs["Blue"], bsdf.inputs["Metallic"])
    normal = g.node("ShaderNodeTexImage", image=images["normal"])
    nmap = g.node("ShaderNodeNormalMap")
    g.link(normal.outputs["Color"], nmap.inputs["Color"])
    g.link(nmap.outputs["Normal"], bsdf.inputs["Normal"])
    return mat


# ---------------------------------------------------------------------------------------------
# Rendering


def use_gpu():
    """Cycles on whatever GPU this machine has, or its CPU; which, as a word."""
    prefs = bpy.context.preferences.addons["cycles"].preferences
    for kind in ("METAL", "OPTIX", "CUDA", "HIP", "ONEAPI"):
        try:
            prefs.compute_device_type = kind
        except TypeError:
            continue
        prefs.get_devices()
        if any(d.type == kind for d in prefs.devices):
            for d in prefs.devices:
                d.use = d.type == kind
            bpy.context.scene.cycles.device = "GPU"
            return kind
    bpy.context.scene.cycles.device = "CPU"
    return "CPU"


def rendered(path):
    """The scene rendered from its camera into `path`, and read back as pixels."""
    import numpy as np

    bpy.context.scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(path)
    px = np.array(img.pixels[:], dtype=np.float32).reshape(img.size[1], img.size[0], 4)
    bpy.data.images.remove(img)
    return px


def save_sheet(tiles, cols, path):
    """Stills tiled `cols` across, the first at the top left, into a PNG at `path`."""
    import numpy as np

    tiles = list(tiles)
    while len(tiles) % cols:
        tiles.append(np.zeros_like(tiles[0]))
    rows = [np.concatenate(tiles[i:i + cols], axis=1) for i in range(0, len(tiles), cols)]
    # Blender's pixels run from the bottom row up.
    sheet = np.concatenate(rows[::-1], axis=0)
    out = bpy.data.images.new("sheet", sheet.shape[1], sheet.shape[0], alpha=True)
    out.pixels.foreach_set(sheet.ravel())
    out.filepath_raw = path
    out.file_format = "PNG"
    out.save()
    print(f"preview: {path}", flush=True)


# ---------------------------------------------------------------------------------------------
# glTF


def export_glb(**settings):
    """The scene, or its selection, exported with `settings`: any this Blender does not have are
    said and left at its defaults."""
    props = bpy.ops.export_scene.gltf.get_rna_type().properties.keys()
    missing = [k for k in settings if k not in props]
    if missing:
        print(f"export: this Blender has no {', '.join(missing)}; left at its defaults", flush=True)
    bpy.ops.export_scene.gltf(**{k: v for k, v in settings.items() if k in props})


def export_model(path, objects, copyright=None):
    """`objects` into a glb at `path` as every model is: y up, their materials with JPEG colour,
    tangents, and no cameras, lights, animation or skins; `copyright` its credit."""
    for ob in bpy.context.scene.objects:
        ob.select_set(ob in objects)
    settings = dict(
        filepath=path, export_format="GLB", export_yup=True, use_selection=True,
        export_apply=False, export_animations=False, export_skins=False,
        export_materials="EXPORT", export_normals=True, export_texcoords=True,
        export_tangents=True, export_image_format="JPEG", export_jpeg_quality=92,
        export_cameras=False, export_lights=False, export_extras=False)
    if copyright:
        settings["export_copyright"] = copyright
    export_glb(**settings)


def blender_argv():
    """The tool's own arguments: what follows `--` on Blender's command line."""
    return sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def import_glb(path, **settings):
    """A glb's objects, by the names its file gives them whatever Blender suffixed to a second
    import's."""
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path, **settings)
    return {re.sub(r"\.\d+$", "", o.name): o for o in set(bpy.data.objects) - before}
