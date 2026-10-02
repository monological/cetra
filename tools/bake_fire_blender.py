"""A log fire simulated and rendered in Blender, for a fire flipbook (spec 13.14).

    /Applications/Blender.app/Contents/MacOS/Blender -b --factory-startup \
        -P tools/bake_fire_blender.py -- [--resolution 128 --frames 48]
    ... -- --render-only     # re-render the last bake, no re-simulation
    python3 tools/fire_flipbook.py out/fire_blender     # then the sheet

Builds its own scene from nothing, so nothing in a user's Blender reaches it, after the way a log
fire is commonly built in Blender's Mantaflow:

  - three logs laid as a fireplace lays them, two side by side and one in the groove on top, each
    a collision EFFECTOR the flames curl round, hidden from the camera;
  - the fuel comes from a SOURCE cylinder around each log, a little larger than it, also hidden:
    the log itself is not the emitter, so fuel comes off its whole surface and the log shapes how
    it rises;
  - the smoke is kept and ABSORBS. Dark smoke between and in front of the flames is what parts
    one tongue from the next; an emission-only fire glows as one flat mass;
  - noise upres adds the small curls the base grid cannot carry, and the simulation runs at three
    quarters of real time, which reads as a calmer fire.

Rendered in Cycles from in front through an orthographic camera onto a transparent film, so a
frame's alpha is how much the smoke covers, to OpenEXR in linear light with no view transform:
the engine tone maps the fire with the rest of its frame, so a picture tone mapped here would be
tone mapped twice. The frames are then read back in Blender's own Python, which reads EXR where
the system's may not, into frames.npy -- premultiplied RGBA, rows bottom first -- beside a
manifest.json saying what they are, which tools/fire_flipbook.py turns into a sheet.

Writes to out/fire_blender/: the bake cache, frames/, frames.npy, manifest.json, and
fire_bake.blend to open and look at.
"""

import argparse
import json
import math
import os
import random
import sys

import bpy

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "out", "fire_blender")

# The domain, in metres, Z up: 1.0 wide, 0.5 deep, 1.1 tall, its floor at 0.
DOMAIN_MIN = (-0.5, -0.25, 0.0)
DOMAIN_MAX = (0.5, 0.25, 1.1)
# The logs, along X: (centre y, centre z, radius, length). Two on the floor side by side, the
# third resting in the groove between them.
LOGS = [(-0.075, 0.06, 0.06, 0.64), (0.075, 0.06, 0.06, 0.60), (0.0, 0.147, 0.055, 0.56)]
# How much larger than its log a source cylinder is, and how far it is raised, in log radii.
SOURCE_GROWTH = 1.2
SOURCE_RAISE = 0.35
# What the camera frames: the full width, from the floor logs' centre line up.
FRAME_BOTTOM = 0.06
FRAME_HEIGHT = 0.75
FPS = 30

# What a sheet made from these frames credits: cetra's own work, under its licence.
PROVENANCE = {
    "source": "tools/bake_fire_blender.py",
    "credit": "Simulated and rendered in Blender by cetra's tools/bake_fire_blender.py",
    "license": "MIT, as cetra",
}


def args_after_dashes():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--preview", action="store_true",
                    help="a coarse domain and a handful of frames, to check the setup quickly")
    ap.add_argument("--resolution", type=int, default=256, help="domain cells along its longest side")
    ap.add_argument("--noise", type=int, default=2, help="noise upres factor, 0 = none")
    ap.add_argument("--warmup", type=int, default=80, help="frames simulated before the first kept")
    ap.add_argument("--frames", type=int, default=80, help="frames kept, the loop's overlap included")
    ap.add_argument("--width", type=int, default=512, help="rendered frame width in pixels")
    ap.add_argument("--samples", type=int, default=64)
    ap.add_argument("--flame-gain", type=float, default=20.0,
                    help="blackbody intensity per unit of the flame field")
    ap.add_argument("--no-fuel-texture", action="store_true",
                    help="fuel off the logs' whole surface, to compare against the texture")
    ap.add_argument("--fuel-scale", type=float, default=0.4,
                    help="size of the fuel texture's patches, in its own units over a log")
    ap.add_argument("--fuel-cover", type=float, default=0.25,
                    help="share of the fuel texture that gives off fuel at all")
    ap.add_argument("--render-only", action="store_true",
                    help="re-render the last bake from out/fire_blender/fire_bake.blend")
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    args = ap.parse_args(argv)
    if args.preview:
        args.resolution, args.noise, args.frames, args.width = 64, 0, 4, 256
    return args


def box_object(name, lo, hi):
    """A cube spanning `lo` to `hi`."""
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=[(a + b) / 2 for a, b in zip(lo, hi)])
    obj = bpy.context.active_object
    obj.name = name
    obj.scale = [b - a for a, b in zip(lo, hi)]
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return obj


def cylinder_along_x(name, y, z, radius, length, vertices):
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius, depth=length,
                                        location=(0.0, y, z), rotation=(0.0, math.pi / 2, 0.0))
    obj = bpy.context.active_object
    obj.name = name
    obj.hide_render = True
    return obj


def make_log(index, y, z, radius, length):
    """A log the flames curl round."""
    log = cylinder_along_x(f"log_{index}", y, z, radius, length, 24)
    mod = log.modifiers.new("Fluid", "FLUID")
    mod.fluid_type = "EFFECTOR"
    mod.effector_settings.effector_type = "COLLISION"


def fuel_texture(scale, cover):
    """Clouds at a high contrast, biased so only the brightest `cover` of the pattern gives off
    any fuel: fuel off a log's whole length burns as one sheet, off sparse patches as separate
    tongues with dark air between. Biased rather than merely contrasted, because at contrast
    alone most of the bark still gives off enough to burn and the patches merge back into the
    sheet. A patch must also span several grid cells or the simulation smears them together: at
    a scale of 0.1 a log carries about twenty, too fine for any grid this bakes."""
    tex = bpy.data.textures.new("fuel", type="CLOUDS")
    tex.noise_scale = scale
    tex.contrast = 5.0
    # Blender applies intensity as an offset after contrast, (v - 0.5) * contrast + intensity
    # - 0.5, and fuel follows only the positive part. So the intensity that puts zero at the
    # (1 - cover) quantile of the pattern is 1 minus that quantile at intensity 1.
    tex.intensity = 1.0
    # Measured unclamped: clamped, every sample past 1 reads as 1 and a small cover's quantile
    # lands among them.
    tex.use_clamp = False
    rng = random.Random(7)
    samples = sorted(tex.evaluate((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)))[3]
                     for _ in range(4000))
    tex.intensity = 1.0 - samples[int((1.0 - cover) * (len(samples) - 1))]
    tex.use_clamp = True
    lit = sum(1 for _ in range(4000)
              if tex.evaluate((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)))[3] > 0)
    print(f"fuel texture: intensity {tex.intensity:.3f}, {lit / 40:.0f}% of the pattern alight",
          flush=True)
    return tex


def make_source(index, y, z, radius, length, texture):
    """Where a log's fuel comes from: a cylinder a little larger than the log, raised so its
    lower side lies inside the log. What it would emit inside the log is inside an effector and
    removed, so the fuel comes off the log's top and flanks, where the texture lets it, and not
    its underside: wood flames from where it is charred and open to the air."""
    source = cylinder_along_x(f"source_{index}", y, z + radius * SOURCE_RAISE,
                              radius * SOURCE_GROWTH, length * 1.02, 16)
    set_fuel(source, index, texture)


def set_fuel(source, index, texture):
    mod = source.modifiers.new("Fluid", "FLUID")
    mod.fluid_type = "FLOW"
    f = mod.flow_settings
    f.flow_type = "FIRE"
    f.flow_behavior = "INFLOW"
    f.flow_source = "MESH"
    f.surface_distance = 1.75  # in domain CELLS, not metres
    f.fuel_amount = 1.0
    f.subframes = 0  # subframes are for an emitter that moves, and this does not
    if texture:
        f.use_texture = True
        f.noise_texture = texture
        f.texture_map_type = "AUTO"
        # The pattern drifts through the bark, 0.7 of the texture over 150 frames, so which
        # patches are alight keeps changing. Each log starts somewhere else in it. A simple
        # expression, which Blender evaluates without Python: a background run has script
        # auto-execution off, and a Python driver would silently hold its first value.
        driver = f.driver_add("texture_offset").driver
        driver.type = "SCRIPTED"
        driver.expression = f"{0.37 * index:.2f} + frame * {0.7 / 150:.6f}"


def setup_domain(args):
    domain = box_object("domain", DOMAIN_MIN, DOMAIN_MAX)
    mod = domain.modifiers.new("Fluid", "FLUID")
    mod.fluid_type = "DOMAIN"
    d = mod.domain_settings
    d.domain_type = "GAS"
    d.resolution_max = args.resolution
    d.cache_directory = os.path.join(OUT, "cache")
    d.cache_type = "ALL"
    d.cache_frame_start = 1
    d.cache_frame_end = args.warmup + args.frames
    # Nothing collides with the domain's walls: what reaches one leaves.
    for side in ("top", "bottom", "front", "back", "left", "right"):
        setattr(d, f"use_collision_border_{side}", False)
    # The domain shrinks to what the simulation fills, which is most of the early frames' saving.
    d.use_adaptive_domain = True
    d.time_scale = 0.75
    if args.noise > 0:
        d.use_noise = True
        d.noise_scale = args.noise
        d.noise_strength = 1.0
        d.noise_pos_scale = 6.0
        d.noise_time_anim = 0.5
    domain.data.materials.append(fire_material(args.flame_gain))
    return domain


def fire_material(flame_gain):
    """A Principled Volume over the simulation's fields: the smoke's density through a ramp that
    tightens where it starts and ends, and blackbody emission from the flame field at the
    temperature the simulation carries. The smoke absorbs, which is what gives the fire depth."""
    mat = bpy.data.materials.new("fire")
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    nodes.clear()
    out = nodes.new("ShaderNodeOutputMaterial")
    volume = nodes.new("ShaderNodeVolumePrincipled")
    # Soot, which absorbs most of what it takes and scatters a quarter of it, the engine's
    # smoke albedo: a white Color scatters everything and absorbs nothing.
    volume.inputs["Color"].default_value = (0.25, 0.25, 0.25, 1.0)
    # The ramp below IS the density. The node multiplies its Density by its Density Attribute,
    # which defaults to the very field the ramp reads, and left set it squares the smoke away.
    volume.inputs["Density Attribute"].default_value = ""
    volume.inputs["Temperature"].default_value = 885.0

    density = nodes.new("ShaderNodeAttribute")
    density.attribute_name = "density"
    ramp = nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = 0.35
    ramp.color_ramp.elements[1].position = 0.65
    links.new(density.outputs["Fac"], ramp.inputs["Fac"])
    links.new(ramp.outputs["Color"], volume.inputs["Density"])

    flame = nodes.new("ShaderNodeAttribute")
    flame.attribute_name = "flame"
    gain = nodes.new("ShaderNodeMath")
    gain.operation = "MULTIPLY"
    gain.inputs[1].default_value = flame_gain
    links.new(flame.outputs["Fac"], gain.inputs[0])
    links.new(gain.outputs["Value"], volume.inputs["Blackbody Intensity"])

    links.new(volume.outputs["Volume"], out.inputs["Volume"])
    mat.cycles.volume_interpolation = "CUBIC"
    return mat


def setup_camera(scene, args):
    cam_data = bpy.data.cameras.new("front")
    cam_data.type = "ORTHO"
    width = DOMAIN_MAX[0] - DOMAIN_MIN[0]
    cam_data.ortho_scale = max(width, FRAME_HEIGHT)
    cam = bpy.data.objects.new("front", cam_data)
    scene.collection.objects.link(cam)
    cam.location = (0.0, -3.0, FRAME_BOTTOM + FRAME_HEIGHT / 2)
    cam.rotation_euler = (math.pi / 2, 0.0, 0.0)
    scene.camera = cam
    scene.render.resolution_x = args.width
    scene.render.resolution_y = int(round(args.width * FRAME_HEIGHT / width))
    scene.render.resolution_percentage = 100


def setup_render(scene, args):
    scene.render.engine = "CYCLES"
    prefs = bpy.context.preferences.addons["cycles"].preferences
    for kind in ("METAL", "CUDA", "OPTIX", "HIP", "ONEAPI"):
        try:
            prefs.compute_device_type = kind
            break
        except TypeError:
            continue
    prefs.get_devices()
    for dev in prefs.devices:
        dev.use = True
    scene.cycles.device = "GPU"
    scene.cycles.samples = args.samples
    scene.cycles.use_denoising = True
    # The fire is the only light: the smoke is lit by what the flames scatter into it.
    scene.cycles.volume_bounces = 2
    # Ray-marched volumes, not Blender 5's default null scattering. Null scattering estimates an
    # emission-dominated volume so badly that a fire renders as a scatter of white points, and
    # more samples make them sharper rather than fewer -- reported upstream as Blender issue
    # #146053. Denoising, clamping, cubic interpolation and the temperature attribute are each
    # innocent of it; this switch alone removes them. It is also what makes the step rate mean
    # anything.
    scene.cycles.volume_biased = True
    scene.cycles.caustics_reflective = False
    scene.cycles.caustics_refractive = False
    scene.cycles.seed = 0
    scene.render.film_transparent = True
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    fmt = scene.render.image_settings
    fmt.file_format = "OPEN_EXR"
    fmt.color_mode = "RGBA"
    fmt.color_depth = "32"
    scene.render.fps = FPS
    # A new world renders through a Background node of its own, grey, whatever its colour says.
    world = scene.world or bpy.data.worlds.new("black")
    scene.world = world
    world.color = (0.0, 0.0, 0.0)
    if world.node_tree:
        for node in world.node_tree.nodes:
            if node.type == "BACKGROUND":
                node.inputs["Strength"].default_value = 0.0


def render_frames(scene, warmup, frames):
    if frames < 1:
        # Before anything is written: an export of nothing would overwrite the last good one.
        sys.exit(f"no frames to render after a warmup of {warmup}")
    frames_dir = os.path.join(OUT, "frames")
    os.makedirs(frames_dir, exist_ok=True)
    for k in range(frames):
        scene.frame_set(warmup + 1 + k)
        scene.render.filepath = os.path.join(frames_dir, f"frame_{k:04d}.exr")
        bpy.ops.render.render(write_still=True)
        print(f"rendered {k + 1}/{frames}", flush=True)
    export_frames(scene, frames)


def export_frames(scene, frames):
    """The rendered frames as tools/fire_flipbook.py reads them: frames.npy, every frame's
    premultiplied RGBA in linear light with its rows bottom first, which is Blender's own pixel
    order, and manifest.json, what they are and where they came from."""
    import numpy as np

    stack = None
    for k in range(frames):
        img = bpy.data.images.load(os.path.join(OUT, "frames", f"frame_{k:04d}.exr"))
        w, h = img.size
        px = np.empty(w * h * 4, dtype=np.float32)
        img.pixels.foreach_get(px)
        bpy.data.images.remove(img)
        if stack is None:
            stack = np.empty((frames, h, w, 4), dtype=np.float32)
        stack[k] = px.reshape(h, w, 4)
    np.save(os.path.join(OUT, "frames.npy"), stack)
    width = DOMAIN_MAX[0] - DOMAIN_MIN[0]
    manifest = dict(PROVENANCE, frames=frames, fps=float(scene.render.fps),
                    metres=[width, width * scene.render.resolution_y / scene.render.resolution_x])
    with open(os.path.join(OUT, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
        f.write("\n")
    print(f"exported {frames} frames of {stack.shape[2]}x{stack.shape[1]}", flush=True)


def main():
    args = args_after_dashes()
    blend = os.path.join(OUT, "fire_bake.blend")
    if args.render_only:
        # The saved file carries the bake and knows it is baked; a scene rebuilt from nothing
        # would not read the cache. The frames kept are the ones the bake was run for.
        bpy.ops.wm.open_mainfile(filepath=blend)
        scene = bpy.context.scene
        domain = bpy.data.objects["domain"]
        domain.data.materials[0] = fire_material(args.flame_gain)
        setup_render(scene, args)
        # The warmup the bake was run with, and its frames, as it recorded them: a command line
        # that differs would render spin-up frames or none.
        warmup = int(scene.get("fire_warmup", args.warmup))
        frames = min(args.frames, int(scene.get("fire_frames", args.frames)))
        render_frames(scene, warmup, frames)
        return

    os.makedirs(OUT, exist_ok=True)
    # Mantaflow's noise upres writes its wavelet tile, waveletNoiseTile.bin, to the working
    # directory: bake from inside the output, not from wherever Blender was started.
    os.chdir(OUT)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    end = args.warmup + args.frames
    scene.frame_start, scene.frame_end = 1, end
    scene["fire_warmup"] = args.warmup
    scene["fire_frames"] = args.frames

    domain = setup_domain(args)
    texture = None if args.no_fuel_texture else fuel_texture(args.fuel_scale, args.fuel_cover)
    for i, log in enumerate(LOGS):
        make_log(i, *log)
        make_source(i, *log, texture)
    setup_camera(scene, args)
    setup_render(scene, args)
    bpy.ops.wm.save_as_mainfile(filepath=blend)

    print(f"bake: {args.resolution} cells, noise x{args.noise}, frames 1..{end}", flush=True)
    with bpy.context.temp_override(scene=scene, object=domain, active_object=domain):
        bpy.ops.fluid.bake_all()
    bpy.ops.wm.save_as_mainfile(filepath=blend)
    render_frames(scene, args.warmup, args.frames)


main()
