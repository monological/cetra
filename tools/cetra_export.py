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

Milestone 2 will replace FLATTEN_OVERRIDES with real texture baking; until
then, materials with procedural node graphs (which glTF cannot express) are
swapped to flat colors from this table during export and restored afterward.
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

# Procedural-material flatten table (milestone-2 baking replaces this).
# material name -> dict(color=(r,g,b), rough=f, alpha=f|None, emissive=(r,g,b,strength)|None)
FLATTEN_OVERRIDES = {
    "IvyLeafReal": {"color": (0.10, 0.30, 0.05), "rough": 0.5},
    "LaceCurtain": {"color": (0.92, 0.90, 0.86), "rough": 0.6, "alpha": 0.55},
    "SheerCurtain": {"color": (0.90, 0.88, 0.84), "rough": 0.6, "alpha": 0.7},
    "Glass_Grime": {"color": (0.80, 0.85, 0.80), "rough": 0.3, "alpha": 0.15},
    "PaintFlake": {"color": (0.33, 0.31, 0.27), "rough": 0.8},
    "grey_plaster_02": {"color": (0.145, 0.125, 0.105), "rough": 0.95},
    "sill_painted_mossy": {"color": (0.30, 0.28, 0.23), "rough": 0.85},
    "distressed_painted_planks": {"color": (0.52, 0.50, 0.45), "rough": 0.7},
    "SkyGlow": {"color": (1.0, 1.0, 1.0), "rough": 1.0, "emissive": (1.0, 0.96, 0.86, 2.5)},
}

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
# Material flattening (restore-after-export)


def build_flat_material(name, spec):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    b = nt.nodes.new("ShaderNodeBsdfPrincipled")
    b.inputs["Base Color"].default_value = (*spec["color"], 1.0)
    b.inputs["Roughness"].default_value = spec.get("rough", 0.8)
    if spec.get("alpha") is not None:
        b.inputs["Alpha"].default_value = spec["alpha"]
        try:
            m.surface_render_method = "BLENDED"
        except AttributeError:
            m.blend_method = "BLEND"
    emissive = spec.get("emissive")
    if emissive:
        b.inputs["Emission Color"].default_value = (*emissive[:3], 1.0)
        b.inputs["Emission Strength"].default_value = emissive[3]
    nt.links.new(b.outputs[0], out.inputs["Surface"])
    return m


def flatten_materials():
    """Swap procedural materials to flat export stand-ins that KEEP the
    authored material name (the .cscn matches overrides on it). The original
    is renamed aside for the duration and restored afterward. Returns the
    restore list."""
    restore = []
    for name, spec in FLATTEN_OVERRIDES.items():
        orig = bpy.data.materials.get(name)
        if not orig:
            continue
        aside = name + ".cscn_orig"
        stale = bpy.data.materials.get(aside)  # crashed prior run
        if stale is not None and stale is not orig:
            stale.name = aside + ".stale"
        orig.name = aside
        flat = build_flat_material(name, spec)  # takes the vacated name
        for obj in bpy.data.objects:
            if obj.type not in ("MESH", "CURVE"):
                continue
            for i, slot in enumerate(obj.material_slots):
                if slot.material is orig:
                    restore.append((obj.name, i, name))
                    slot.material = flat
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
        flat = bpy.data.materials.get(name)
        if flat is not None and flat is not orig:
            bpy.data.materials.remove(flat)
        orig.name = name


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
        color = [round(radius_v[i] / peak, 3) for i in range(3)]
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

    # ----- GLB (flatten procedural materials, restore after) -----
    restore = flatten_materials()
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
        flattened = sorted(set(m for (_o, _i, m) in restore))
        print("  flattened materials (milestone-2 baking pending): " + ", ".join(flattened))
    return out_path


def _parse_cli_out():
    argv = sys.argv
    if "--" in argv:
        rest = argv[argv.index("--") + 1:]
        for i, a in enumerate(rest):
            if a == "--out" and i + 1 < len(rest):
                return rest[i + 1]
    return OUT_PATH


if __name__ == "__main__":
    out = _parse_cli_out()
    if not out:
        raise SystemExit("cetra_export: set OUT_PATH or pass -- --out <path>")
    export(out)
