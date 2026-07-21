"""Cetra scene exporter: writes <name>.glb + <name>.cscn from the open Blender scene.

The GLB carries what glTF can (geometry, flat/textured PBR materials, punctual
lights, cameras); the .cscn scene file carries what it cannot: environment/HDR,
area lights (converted to points -- cetra has no area shading path yet), sun
angular softness, volumetric fog, post grade (tonemap/exposure/bloom), material
SSS tags, and the camera framing. The render app opens the .cscn directly:

    ./out/bin/render assets/<name>.cscn

Run inside Blender (Scripting tab) after setting OUT_PATH below, or headless:

    blender -b scene.blend -P tools/cetra_export.py -- --out assets/name.glb

Exclusions: objects with a custom property `cetra_exclude`, wire-display
helpers, volume-material objects (they become fog), and anything in a
collection named `_cetra_exclude`.

Materials whose node graphs glTF cannot express are BAKED to texture sets
(diffuse/roughness/normal/emissive, alpha packed into base color) on a
generated `cetra_bake` UV layer, one atlas per material across all its user
meshes. Bakes are cached in <out_dir>/textures/ and skipped when up to date;
pass -- --rebake (or set REBAKE) to force. During export the materials are
swapped to baked stand-ins (name-preserving) and restored afterward.
"""

import json
import math
import os
import shutil
import sys

import bpy
from mathutils import Vector

# ---------------------------------------------------------------------------
# Tunables

OUT_PATH = None  # set in the Script editor, or pass -- --out <path> headless

# Area-light watts -> cetra point intensity (cetra's renderer scale is ~1-10
# with default attenuation; a 15-20W Blender area fill lands around 5-7).
AREA_TO_POINT_K = 0.33

# Blender fog volume density -> cetra fog extinction per world unit.
FOG_K = 1.0

# Compositor Glare(Bloom) strength -> cetra bloom_strength.
BLOOM_STRENGTH_K = 0.75

# Capture the scene into the local reflection probe (interior scenes want
# this; it turns the environment light into local bounce).
PROBE_SCENE = True

# --- baking ---
# Bake sample count: these are value maps (albedo/roughness/normals), not
# light transport, so a handful of samples converges.
BAKE_SAMPLES = 8
BAKE_MARGIN = 8  # px dilation past island borders
REBAKE = False   # force re-bake even when cached textures are up to date
BAKE_UV = "cetra_bake"
# Atlas resolution from a texel-density target: res ~= sqrt(area) * density,
# rounded up to a power of two. Keeps px/m roughly constant instead of
# splitting one fixed-size atlas across arbitrarily large surfaces.
BAKE_TEXEL_DENSITY = 384  # px per meter
BAKE_RES_MIN = 512
BAKE_RES_MAX = 4096

# ---------------------------------------------------------------------------


def yup(v):
    """Blender Z-up world -> glTF Y-up world."""
    return [round(v[0], 4), round(v[2], 4), round(-v[1], 4)]


def volume_materials():
    """Materials with a Principled Volume node, computed once: they mark
    objects as fog volumes (excluded from the GLB) and carry the fog params."""
    found = set()
    for m in bpy.data.materials:
        if m.use_nodes and any(n.type == "PRINCIPLED_VOLUME" for n in m.node_tree.nodes):
            found.add(m)
    return found


def in_excluded_collection(obj):
    for coll in obj.users_collection:
        if coll.name == "_cetra_exclude":
            return True
    return False


def is_excluded(obj, volume_mats):
    if obj.get("cetra_exclude"):
        return True
    if getattr(obj, "display_type", "") == "WIRE":
        return True
    if in_excluded_collection(obj):
        return True
    if obj.type == "MESH" and any(s.material in volume_mats for s in obj.material_slots):
        return True
    return False


def principled_node(mat):
    if not mat or not mat.use_nodes:
        return None
    for n in mat.node_tree.nodes:
        if n.type == "BSDF_PRINCIPLED":
            return n
    return None


def socket_value(node, name, default=None):
    s = node.inputs.get(name)
    return s.default_value if s is not None else default


# ---------------------------------------------------------------------------
# Name-preserving material swap (stand-ins keep the authored name so the GLB
# and the .cscn keep matching on it; originals are renamed aside and restored)


def swap_in_materials(standins):
    """standins: {authored_name: stand_in_material}. The stand-in takes over
    the authored name for the duration. Returns the restore list."""
    restore = []
    for name, standin in standins.items():
        orig = bpy.data.materials.get(name)
        if not orig or orig is standin:
            continue
        aside = name + ".cscn_orig"
        stale = bpy.data.materials.get(aside)  # crashed prior run
        if stale is not None and stale is not orig:
            stale.name = aside + ".stale"
        orig.name = aside
        standin.name = name
        for obj in bpy.data.objects:
            if obj.type not in ("MESH", "CURVE"):
                continue
            for i, slot in enumerate(obj.material_slots):
                if slot.material is orig:
                    restore.append((obj.name, i, name))
                    slot.material = standin
    return restore


def restore_materials(restore):
    origs = {name: bpy.data.materials.get(name + ".cscn_orig")
             for name in set(m for (_o, _i, m) in restore)}
    for obj_name, slot_idx, mat_name in restore:
        obj = bpy.data.objects.get(obj_name)
        if obj and origs.get(mat_name):
            obj.material_slots[slot_idx].material = origs[mat_name]
    for name, orig in origs.items():
        if not orig:
            continue
        standin = bpy.data.materials.get(name)
        if standin is not None and standin is not orig:
            bpy.data.materials.remove(standin)
        orig.name = name


# ---------------------------------------------------------------------------
# Baking: procedural materials -> texture sets on a generated UV layer


def socket_is_expressible(node, name):
    """glTF can carry this input iff it's a flat value or a direct image."""
    s = node.inputs.get(name)
    if s is None or not s.links:
        return True
    src = s.links[0].from_node
    if src.type == "TEX_IMAGE":
        return True
    if name == "Normal" and src.type == "NORMAL_MAP":
        col = src.inputs.get("Color")
        return bool(col and col.links and col.links[0].from_node.type == "TEX_IMAGE")
    return False


def emission_node(mat):
    for n in mat.node_tree.nodes:
        if n.type == "EMISSION":
            return n
    return None


def alpha_chain_source(mat):
    """The socket driving this material's opacity, if procedural: a linked
    Principled Alpha, or the Fac of a Mix Shader blending a Transparent BSDF."""
    p = principled_node(mat)
    if p:
        s = p.inputs.get("Alpha")
        if s is not None and s.links:
            return s.links[0].from_socket
    for n in mat.node_tree.nodes:
        if n.type != "MIX_SHADER":
            continue
        feeds = [l.from_node.type for l in mat.node_tree.links if l.to_node == n]
        if "BSDF_TRANSPARENT" in feeds:
            fac = n.inputs.get("Fac")
            if fac is not None and fac.links:
                return fac.links[0].from_socket
    return None


def material_bake_passes(mat):
    """Which passes this material needs, or [] when glTF can carry it as-is."""
    if not mat or not mat.use_nodes or mat.get("cetra_no_bake"):
        return []
    passes = []
    p = principled_node(mat)
    if p:
        if not socket_is_expressible(p, "Base Color"):
            passes.append("base")
        if not socket_is_expressible(p, "Roughness"):
            passes.append("rough")
        if not socket_is_expressible(p, "Normal"):
            passes.append("normal")
        es = p.inputs.get("Emission Strength")
        ec = p.inputs.get("Emission Color")
        if ec is not None and ec.links and es is not None:
            passes.append("emit")
    else:
        em = emission_node(mat)
        if em and em.inputs["Color"].links:
            passes.append("emit")
        # Non-principled shading graphs (diffuse/translucent mixes): bake the
        # diffuse color; roughness on such graphs is rarely meaningful.
        for n in mat.node_tree.nodes:
            if n.type in ("BSDF_DIFFUSE",) and n.inputs["Color"].links:
                if "base" not in passes:
                    passes.append("base")
    if alpha_chain_source(mat) is not None:
        if "base" not in passes:
            passes.append("base")  # alpha packs into the base texture
        passes.append("alpha")
    return passes


def collect_bake_set(exported):
    """{material: {passes, meshes, objects}} over the exported objects.
    Meshes are unique datablocks (instances bake once); objects holds one
    representative per mesh."""
    bake = {}
    for obj in exported:
        if obj.type != "MESH":
            continue
        for slot in obj.material_slots:
            mat = slot.material
            if not mat:
                continue
            if mat not in bake:
                bake[mat] = {"passes": material_bake_passes(mat), "meshes": {}, "area": 0.0}
            entry = bake[mat]
            if entry["passes"] and obj.data.name not in entry["meshes"]:
                entry["meshes"][obj.data.name] = obj
                scale = obj.matrix_world.median_scale
                entry["area"] += sum(p.area for p in obj.data.polygons) * scale * scale
    return {m: e for m, e in bake.items() if e["passes"]}


def bake_resolution(area):
    want = math.sqrt(max(area, 0.01)) * BAKE_TEXEL_DENSITY
    res = BAKE_RES_MIN
    while res < want and res < BAKE_RES_MAX:
        res *= 2
    return res


def tex_paths(tex_dir, mat, passes):
    safe = bpy.path.clean_name(mat.name)
    files = {p: os.path.join(tex_dir, "%s_%s.png" % (safe, p)) for p in passes if p != "alpha"}
    info = os.path.join(tex_dir, safe + ".bakeinfo")
    return files, info


def bake_cached(tex_dir, mat, passes, res):
    files, info = tex_paths(tex_dir, mat, passes)
    if REBAKE or not os.path.exists(info):
        return False
    try:
        with open(info) as f:
            saved = json.load(f)
    except (OSError, ValueError):
        return False
    if saved.get("res") != res or saved.get("passes") != sorted(passes):
        return False
    return all(os.path.exists(p) for p in files.values())


def ensure_bake_uv(entry):
    """Smart-project all of the material's user meshes into BAKE_UV and pack
    their islands jointly so they share one atlas."""
    objs = list(entry["meshes"].values())
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
        uv = o.data.uv_layers.get(BAKE_UV)
        if uv is None:
            uv = o.data.uv_layers.new(name=BAKE_UV)
        o.data.uv_layers.active = uv
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    # Tight margins: the bake's pixel dilation (BAKE_MARGIN) handles bleed;
    # fat UV gutters just waste atlas area.
    bpy.ops.uv.smart_project(angle_limit=math.radians(66.0), island_margin=0.005)
    bpy.ops.uv.pack_islands(margin=0.003)
    bpy.ops.object.mode_set(mode="OBJECT")


def strip_superseded_uvs(bake_set, manifest):
    """Baked meshes must expose BAKE_UV as TEXCOORD_0 (engines read channel
    0). Their prior UV layers are superseded by the bake (default-cube or
    absent), so remove them."""
    stripped = 0
    for entry in bake_set.values():
        for obj in entry["meshes"].values():
            uvs = obj.data.uv_layers
            for name in [u.name for u in uvs if u.name != BAKE_UV]:
                uvs.remove(uvs[name])
                stripped += 1
            if BAKE_UV in uvs:
                uvs.active = uvs[BAKE_UV]
    if stripped:
        manifest.append("removed %d superseded UV layer(s) on baked meshes" % stripped)


def new_bake_image(name, res, non_color=False, fill=(0, 0, 0, 1)):
    img = bpy.data.images.get(name)
    if img:
        bpy.data.images.remove(img)
    img = bpy.data.images.new(name, res, res, alpha=True)
    img.generated_color = fill
    if non_color:
        img.colorspace_settings.name = "Non-Color"
    return img


def run_bake_pass(mat, entry, img, bake_type, pass_filter=None):
    """Bake one pass for every user mesh, accumulating into the shared image."""
    nt = mat.node_tree
    target = nt.nodes.new("ShaderNodeTexImage")
    target.image = img
    nt.nodes.active = target
    target.select = True
    try:
        for obj in entry["meshes"].values():
            bpy.ops.object.select_all(action="DESELECT")
            obj.select_set(True)
            bpy.context.view_layer.objects.active = obj
            kwargs = {"type": bake_type, "margin": BAKE_MARGIN, "use_clear": False,
                      "uv_layer": BAKE_UV}
            if pass_filter is not None:
                kwargs["pass_filter"] = pass_filter
            bpy.ops.object.bake(**kwargs)
    finally:
        nt.nodes.remove(target)


def bake_alpha_pass(mat, entry, img):
    """EMIT-bake the alpha chain through a temporary emission rig."""
    nt = mat.node_tree
    src = alpha_chain_source(mat)
    out = next((n for n in nt.nodes if n.type == "OUTPUT_MATERIAL" and n.is_active_output), None)
    if src is None or out is None:
        return False
    surf = out.inputs["Surface"]
    prev = surf.links[0].from_socket if surf.links else None
    em = nt.nodes.new("ShaderNodeEmission")
    nt.links.new(src, em.inputs["Color"])
    nt.links.new(em.outputs[0], surf)
    try:
        run_bake_pass(mat, entry, img, "EMIT")
    finally:
        nt.nodes.remove(em)
        if prev is not None:
            nt.links.new(prev, surf)
    return True


def pack_alpha_into(base_img, alpha_img):
    import numpy as np
    base = np.array(base_img.pixels[:], dtype=np.float32).reshape(-1, 4)
    alpha = np.array(alpha_img.pixels[:], dtype=np.float32).reshape(-1, 4)
    base[:, 3] = alpha[:, 0]
    base_img.pixels = base.ravel()


def save_image(img, path):
    img.filepath_raw = path
    img.file_format = "PNG"
    img.save()


def bake_material(mat, entry, tex_dir, manifest):
    passes = entry["passes"]
    res = bake_resolution(entry["area"])
    files, info = tex_paths(tex_dir, mat, passes)
    if bake_cached(tex_dir, mat, passes, res):
        manifest.append("bake '%s': cached (%s, %d)" % (mat.name, "+".join(passes), res))
        return files
    ensure_bake_uv(entry)
    safe = bpy.path.clean_name(mat.name)

    # Emission strength must be 1.0 during the EMIT bake or the radiance
    # clips in the PNG; the stand-in restores the strength as a factor.
    emit_strength = 1.0
    strength_socket = None
    p = principled_node(mat)
    em = emission_node(mat)
    if "emit" in passes:
        s = (p.inputs.get("Emission Strength") if p else None) or \
            (em.inputs.get("Strength") if em else None)
        if s is not None and not s.links:
            strength_socket = s
            emit_strength = float(s.default_value)
            s.default_value = 1.0

    try:
        if "base" in passes:
            img = new_bake_image(safe + "_base", res)
            run_bake_pass(mat, entry, img, "DIFFUSE", pass_filter={"COLOR"})
            if "alpha" in passes:
                a_img = new_bake_image(safe + "_alpha", res, non_color=True)
                if bake_alpha_pass(mat, entry, a_img):
                    pack_alpha_into(img, a_img)
                bpy.data.images.remove(a_img)
            save_image(img, files["base"])
        if "rough" in passes:
            img = new_bake_image(safe + "_rough", res, non_color=True)
            run_bake_pass(mat, entry, img, "ROUGHNESS")
            save_image(img, files["rough"])
        if "normal" in passes:
            img = new_bake_image(safe + "_normal", res, non_color=True,
                                 fill=(0.5, 0.5, 1.0, 1.0))
            run_bake_pass(mat, entry, img, "NORMAL")
            save_image(img, files["normal"])
        if "emit" in passes:
            img = new_bake_image(safe + "_emit", res)
            run_bake_pass(mat, entry, img, "EMIT")
            save_image(img, files["emit"])
    finally:
        if strength_socket is not None:
            strength_socket.default_value = emit_strength

    with open(info, "w") as f:
        json.dump({"res": res, "passes": sorted(passes), "emit_strength": emit_strength}, f)
    manifest.append("bake '%s': %s @ %d (%d mesh(es), area %.1f)"
                    % (mat.name, "+".join(passes), res, len(entry["meshes"]), entry["area"]))
    return files


def build_baked_material(name, files, passes, emit_strength, alpha):
    """Stand-in Principled wired to the baked textures via the BAKE_UV map."""
    m = bpy.data.materials.new(name + ".baked_tmp")
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    b = nt.nodes.new("ShaderNodeBsdfPrincipled")
    nt.links.new(b.outputs[0], out.inputs["Surface"])
    uvn = nt.nodes.new("ShaderNodeUVMap")
    uvn.uv_map = BAKE_UV

    def img_node(path, non_color=False):
        n = nt.nodes.new("ShaderNodeTexImage")
        n.image = bpy.data.images.load(path, check_existing=True)
        if non_color:
            n.image.colorspace_settings.name = "Non-Color"
        nt.links.new(uvn.outputs[0], n.inputs["Vector"])
        return n

    if "base" in passes:
        n = img_node(files["base"])
        nt.links.new(n.outputs["Color"], b.inputs["Base Color"])
        if alpha:
            nt.links.new(n.outputs["Alpha"], b.inputs["Alpha"])
            try:
                m.surface_render_method = "BLENDED"
            except AttributeError:
                m.blend_method = "BLEND"
    if "rough" in passes:
        n = img_node(files["rough"], non_color=True)
        nt.links.new(n.outputs["Color"], b.inputs["Roughness"])
    if "normal" in passes:
        n = img_node(files["normal"], non_color=True)
        nm = nt.nodes.new("ShaderNodeNormalMap")
        nm.uv_map = BAKE_UV
        nt.links.new(n.outputs["Color"], nm.inputs["Color"])
        nt.links.new(nm.outputs[0], b.inputs["Normal"])
    if "emit" in passes:
        n = img_node(files["emit"])
        nt.links.new(n.outputs["Color"], b.inputs["Emission Color"])
        b.inputs["Emission Strength"].default_value = emit_strength
    return m


def cycles_for_bake():
    """Enable Cycles (GPU when available), switch the scene to it, and return
    a restore callable."""
    import addon_utils
    addon_utils.enable("cycles")
    scene = bpy.context.scene
    prev_engine = scene.render.engine
    scene.render.engine = "CYCLES"
    scene.cycles.samples = BAKE_SAMPLES
    scene.cycles.use_denoising = False
    try:
        prefs = bpy.context.preferences.addons["cycles"].preferences
        prefs.compute_device_type = "METAL"
        prefs.get_devices()
        for d in prefs.devices:
            d.use = True
        scene.cycles.device = "GPU"
    except Exception:
        scene.cycles.device = "CPU"

    def restore():
        scene.render.engine = prev_engine

    return restore


def bake_all(bake_set, tex_dir, manifest):
    os.makedirs(tex_dir, exist_ok=True)
    restore_engine = cycles_for_bake()
    results = {}
    try:
        for mat, entry in bake_set.items():
            results[mat] = bake_material(mat, entry, tex_dir, manifest)
    finally:
        restore_engine()
    return results


def baked_standins(bake_set, tex_dir):
    standins = {}
    for mat, entry in bake_set.items():
        files, info = tex_paths(tex_dir, mat, entry["passes"])
        emit_strength = 1.0
        try:
            with open(info) as f:
                emit_strength = float(json.load(f).get("emit_strength", 1.0))
        except (OSError, ValueError):
            pass
        standins[mat.name] = build_baked_material(
            mat.name, files, entry["passes"], emit_strength,
            alpha="alpha" in entry["passes"])
    return standins


# ---------------------------------------------------------------------------
# Scene-file collectors


def collect_lights(volume_mats, manifest):
    """Area lights -> cscn point entries; sun angle -> light_overrides."""
    lights = []
    overrides = []
    for obj in bpy.data.objects:
        if obj.type != "LIGHT" or is_excluded(obj, volume_mats):
            continue
        data = obj.data
        if data.type == "AREA":
            lights.append(
                {
                    "name": obj.name,
                    "type": "point",
                    "position": yup(obj.matrix_world.translation),
                    "color": [round(c, 4) for c in data.color],
                    "intensity": round(data.energy * AREA_TO_POINT_K, 3),
                }
            )
            manifest.append("area light '%s' -> point (intensity %.2f)"
                            % (obj.name, data.energy * AREA_TO_POINT_K))
        elif data.type == "SUN" and data.angle > 0.0:
            overrides.append({"name": obj.name, "size_from_angle": round(data.angle, 4)})
            manifest.append("sun '%s' softness angle %.3f rad" % (obj.name, data.angle))
    return lights, overrides


def collect_fog(volume_mats, manifest):
    """First Principled Volume material becomes cetra's fog settings."""
    for m in sorted(volume_mats, key=lambda m: m.name):
        for n in m.node_tree.nodes:
            if n.type == "PRINCIPLED_VOLUME":
                density = float(socket_value(n, "Density", 0.01)) * FOG_K
                aniso = float(socket_value(n, "Anisotropy", 0.0))
                manifest.append("volume material '%s' -> fog density %.4f aniso %.2f"
                                % (m.name, density, aniso))
                return {
                    "enabled": True,
                    "density": round(density, 5),
                    "anisotropy": round(aniso, 3),
                }
    return None


def world_strength(world):
    for n in world.node_tree.nodes:
        if n.type == "BACKGROUND":
            s = n.inputs.get("Strength")
            if s is not None:
                return float(s.default_value)
    return None


def collect_environment(out_dir, base, manifest):
    world = bpy.context.scene.world
    if world and world.use_nodes:
        strength = world_strength(world)
        for n in world.node_tree.nodes:
            if n.type == "TEX_ENVIRONMENT" and n.image and n.image.filepath:
                src = bpy.path.abspath(n.image.filepath)
                if os.path.exists(src):
                    # Deterministic pipeline name: source files often come out
                    # of addon caches with temp names (tmpXXXX.hdr). Skip the
                    # copy when the destination already holds this file (same
                    # path would even raise SameFileError).
                    ext = os.path.splitext(src)[1] or ".hdr"
                    dst_name = base + "_env" + ext
                    dst = os.path.join(out_dir, dst_name)
                    same = os.path.exists(dst) and os.path.samefile(src, dst)
                    fresh = (not same and os.path.exists(dst)
                             and os.path.getsize(dst) == os.path.getsize(src)
                             and os.path.getmtime(dst) >= os.path.getmtime(src))
                    if not (same or fresh):
                        shutil.copy2(src, dst)
                    manifest.append("world HDR -> '%s'%s" % (dst_name,
                                    " (up to date)" if same or fresh else ""))
                    env = {"mode": "hdr", "hdr": dst_name, "probe_scene": PROBE_SCENE}
                    if strength is not None:
                        env["intensity"] = round(strength, 4)
                        manifest.append("world strength %.3f -> environment intensity" % strength)
                    return env
                manifest.append("world HDR missing on disk (%s); using sky" % src)
    manifest.append("environment: procedural sky")
    return {"mode": "sky", "probe_scene": PROBE_SCENE}


def collect_post(fog, manifest):
    post = {}
    vs = bpy.context.scene.view_settings
    transform_map = {"AgX": "agx", "Filmic": "aces", "Standard": "neutral"}
    tonemap = transform_map.get(vs.view_transform)
    if tonemap:
        post["tonemap"] = tonemap
    if abs(vs.exposure) > 1e-6:
        # Blender exposure is in stops (image scaled by 2^x); the .cscn field
        # is a linear multiplier, so convert here like every other unit.
        post["exposure"] = round(2.0 ** vs.exposure, 4)
    # Compositor Glare (Bloom) -> cetra bloom (Blender 5: socket-driven node
    # inside the scene's compositing node group).
    ng = getattr(bpy.context.scene, "compositing_node_group", None)
    if ng:
        for n in ng.nodes:
            if n.bl_idname == "CompositorNodeGlare":
                def sock(name, default):
                    s = n.inputs.get(name)
                    try:
                        return float(s.default_value) if s is not None else default
                    except TypeError:
                        return default
                post["bloom"] = {
                    "enabled": True,
                    "strength": round(sock("Strength", 0.5) * BLOOM_STRENGTH_K, 3),
                    "threshold": round(sock("Threshold", 1.0), 3),
                }
                manifest.append("compositor bloom -> strength %.2f" % post["bloom"]["strength"])
                break
    if fog:
        post["fog"] = fog
    return post


def collect_sss(manifest):
    materials = {}
    for m in bpy.data.materials:
        node = principled_node(m)
        if not node:
            continue
        weight = socket_value(node, "Subsurface Weight", 0.0)
        if not weight or weight <= 0.0:
            continue
        radius_v = socket_value(node, "Subsurface Radius", (1.0, 0.2, 0.1))
        scale = socket_value(node, "Subsurface Scale", 0.05)
        peak = max(radius_v[0], radius_v[1], radius_v[2], 1e-6)
        # Temper the normalized radius toward neutral: the raw ratio makes an
        # oversaturated transmission tint (acid green on foliage).
        color = [round(0.5 + 0.5 * (radius_v[i] / peak), 3) for i in range(3)]
        materials[m.name] = {"sss": {"color": color, "radius": round(float(scale), 4)}}
        manifest.append("sss material '%s' (radius %.3f)" % (m.name, scale))
    return materials


def scene_bounds_center(objects):
    lo = Vector((1e30, 1e30, 1e30))
    hi = Vector((-1e30, -1e30, -1e30))
    found = False
    for obj in objects:
        if obj.type != "MESH":
            continue
        for corner in obj.bound_box:
            w = obj.matrix_world @ Vector(corner)
            lo = Vector(map(min, lo, w))
            hi = Vector(map(max, hi, w))
            found = True
    return (lo + hi) / 2.0 if found else None


def collect_camera(exported, manifest):
    cam = bpy.context.scene.camera
    if not cam:
        return None
    eye_b = cam.matrix_world.translation
    fwd_b = cam.matrix_world.to_quaternion() @ Vector((0.0, 0.0, -1.0))
    # The target is the app's orbit pivot, so use a real datum: the DoF focus
    # distance when authored, else the exported geometry's center projected
    # onto the view ray. A bare constant would masquerade as authored data.
    dist = 3.0
    if cam.data.dof.use_dof and cam.data.dof.focus_distance > 0.0:
        dist = cam.data.dof.focus_distance
    else:
        center = scene_bounds_center(exported)
        if center is not None:
            along = (center - eye_b).dot(fwd_b)
            if along > 0.1:
                dist = along
    target_b = eye_b + fwd_b * dist
    vfov = math.degrees(cam.data.angle_y)
    manifest.append("camera '%s' (vfov %.1f, target %.1fm along view)" % (cam.name, vfov, dist))
    return {"eye": yup(eye_b), "target": yup(target_b), "fov": round(vfov, 2)}


# ---------------------------------------------------------------------------


def export(out_path):
    out_path = os.path.abspath(out_path)
    out_dir = os.path.dirname(out_path)
    base = os.path.splitext(os.path.basename(out_path))[0]
    os.makedirs(out_dir, exist_ok=True)
    manifest = []

    # ----- selection: everything not excluded -----
    volume_mats = volume_materials()
    excluded = []
    exported = []
    bpy.ops.object.select_all(action="DESELECT")
    for obj in bpy.context.view_layer.objects:
        if is_excluded(obj, volume_mats):
            excluded.append(obj.name)
            continue
        obj.select_set(True)
        exported.append(obj)

    # ----- scene file -----
    lights, overrides = collect_lights(volume_mats, manifest)
    fog = collect_fog(volume_mats, manifest)
    environment = collect_environment(out_dir, base, manifest)
    post = collect_post(fog, manifest)
    materials = collect_sss(manifest)
    camera = collect_camera(exported, manifest)

    cscn = {"version": 1, "models": [{"path": base + ".glb"}]}
    if environment:
        cscn["environment"] = environment
    if lights:
        cscn["lights"] = lights
    if overrides:
        cscn["light_overrides"] = overrides
    if post:
        cscn["post"] = post
    if materials:
        cscn["materials"] = materials
    if camera:
        cscn["camera"] = camera

    cscn_path = os.path.join(out_dir, base + ".cscn")
    with open(cscn_path, "w") as f:
        json.dump(cscn, f, indent=2)
        f.write("\n")

    # ----- bake procedural materials, swap to baked stand-ins, export -----
    bake_set = collect_bake_set(exported)
    tex_dir = os.path.join(out_dir, "textures")
    restore = []
    if bake_set:
        bake_all(bake_set, tex_dir, manifest)
        strip_superseded_uvs(bake_set, manifest)
        # bake ops clobber the selection; restore it for use_selection export
        bpy.ops.object.select_all(action="DESELECT")
        for obj in exported:
            obj.select_set(True)
        restore = swap_in_materials(baked_standins(bake_set, tex_dir))
    try:
        bpy.ops.export_scene.gltf(
            filepath=out_path,
            export_format="GLB",
            use_selection=True,
            export_lights=True,
            export_cameras=True,
            export_apply=True,
            export_yup=True,
        )
    finally:
        restore_materials(restore)

    # ----- manifest -----
    print("cetra_export: %s (%d objects) + %s" % (out_path, len(exported), cscn_path))
    for line in manifest:
        print("  - " + line)
    if excluded:
        print("  excluded: " + ", ".join(sorted(excluded)))
    if restore:
        baked = sorted(set(m for (_o, _i, m) in restore))
        print("  baked materials: " + ", ".join(baked))
    return out_path


# ---------------------------------------------------------------------------
# Stepwise driving (keeps individual calls short when run over MCP): call
# prepare_bake(out_path) once, bake_one(i) per material, then export(out_path)
# which hits the bake cache.

_BAKE_STATE = {}


def collect_exported():
    volume_mats = volume_materials()
    return [o for o in bpy.context.view_layer.objects if not is_excluded(o, volume_mats)]


def prepare_bake(out_path):
    out_dir = os.path.dirname(os.path.abspath(out_path))
    tex_dir = os.path.join(out_dir, "textures")
    bake_set = collect_bake_set(collect_exported())
    _BAKE_STATE.clear()
    _BAKE_STATE["tex_dir"] = tex_dir
    _BAKE_STATE["entries"] = list(bake_set.items())
    report = []
    for mat, entry in _BAKE_STATE["entries"]:
        res = bake_resolution(entry["area"])
        cached = bake_cached(tex_dir, mat, entry["passes"], res)
        report.append("%s: %s @ %d, %d mesh(es)%s"
                      % (mat.name, "+".join(entry["passes"]), res, len(entry["meshes"]),
                         " [cached]" if cached else ""))
    return report


def bake_one(index):
    mat, entry = _BAKE_STATE["entries"][index]
    os.makedirs(_BAKE_STATE["tex_dir"], exist_ok=True)
    manifest = []
    restore_engine = cycles_for_bake()
    try:
        bake_material(mat, entry, _BAKE_STATE["tex_dir"], manifest)
    finally:
        restore_engine()
    return manifest


def _parse_cli():
    global REBAKE
    out = OUT_PATH
    argv = sys.argv
    if "--" in argv:
        rest = argv[argv.index("--") + 1:]
        for i, a in enumerate(rest):
            if a == "--out" and i + 1 < len(rest):
                out = rest[i + 1]
            elif a == "--rebake":
                REBAKE = True
    return out


if __name__ == "__main__":
    out = _parse_cli()
    if not out:
        raise SystemExit("cetra_export: set OUT_PATH or pass -- --out <path>")
    export(out)
