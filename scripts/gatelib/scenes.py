"""Scenes several groups build, copy or read a camera from."""

import json
import math
import re
import os

from gatelib.harness import asset


def _cscn_camera(name, **extra):
    """Camera read out of the fixture's own .cscn, in _projector's dict shape.

    A gate that predicts where geometry lands on screen needs the exact camera
    the render used. Transcribing it leaves two copies that agree only until
    someone edits the .cscn, and the failure is silent -- the gate keeps passing
    while measuring a different scene than it predicts. Reading it removes the
    copy. Anything passed as **extra is the gate's OWN choice (sample columns,
    thresholds), which is worth keeping visibly distinct from what the scene
    file dictates.
    """
    with open(asset(name)) as f:
        return _camera_block(json.load(f)["camera"], **extra)


def _camera_block(cam, **extra):
    """A .cscn camera block (eye, target, fov) in _projector's dict shape."""
    return {"eye": tuple(cam["eye"]), "target": tuple(cam["target"]),
            "fovy_deg": float(cam["fov"]), **extra}


def cscn_copy(src, dst, mutate):
    """Copy a .cscn through `mutate(dict)`, with model paths made absolute; returns `dst`.

    Generating a twin mechanically rather than committing a hand-edited one is the point:
    the two halves then cannot differ in anything except what `mutate` touched, which is
    the property every arm built on one of these asserts.

    The path absolutization is the half worth having in one place. It is a quiet
    correctness requirement -- model paths resolve against the scene file's directory, so
    an out-of-tree copy must carry absolute ones -- and a copy that forgets it fails as a
    render error rather than as a mismatch.

    An IES profile path resolves the same way and does NOT fail the same way: a relative
    one in an out-of-tree copy loads nothing, the light keeps its analytic cone, and the
    frame renders. That is the one to get wrong silently, so it is handled here too rather
    than by each arm that happens to remember.
    """
    with open(src) as f:
        d = json.load(f)
    mutate(d)
    base = os.path.dirname(os.path.abspath(src))

    # Anything that is not an object holding a path is left as the loader would leave it: a
    # "_comment" string among the materials is legal, and the C side skips it.
    def absolute(obj, key):
        if isinstance(obj, dict) and isinstance(obj.get(key), str) and obj[key] \
                and not os.path.isabs(obj[key]):
            obj[key] = os.path.join(base, obj[key])

    for m in d.get("models", []):
        absolute(m, "path")
    for light in d.get("lights", []):
        absolute(light, "profile")
    # post.lut.path is the third of these and fails the IES way, not the model
    # way: a relative path in an out-of-tree copy loads nothing, the frame
    # renders ungraded, and an arm asserting "the LUT changed the frame" reports
    # the feature broken when the path was.
    absolute(d.get("post", {}).get("lut"), "path")
    # And a shader file (spec 13.29), which fails the IES way too: a pass, a hook or a late
    # material whose shader cannot be read is skipped by name, and the frame renders without it.
    for p in d.get("post", {}).get("passes", []):
        absolute(p, "shader")
    for h in d.get("shaderHooks", {}).values():
        absolute(h, "surface")
        absolute(h, "offset")
    for m in d.get("materials", {}).values():
        absolute(m, "lateShader")
    with open(dst, "w") as f:
        json.dump(d, f, indent=1)
    return dst


def _scale_emitters(d, factor):
    """Multiply every authored emitter in a .cscn dict by `factor`, in place.

    One copy, because two callers need the same answer to "what counts as an
    authored intensity" and a scene that scales in one and not the other is a
    scene neither can reason about. Fog's in-scatter is on the list because it
    is a real emitter -- and it was the branch a second, hand-copied version of
    this rule silently omitted.
    """
    for light in d.get("lights", []):
        if "intensity" in light:
            light["intensity"] *= factor

    env = d.get("environment")
    if env:
        # A sky scene's environment IS the sky, so its scale carries the IBL
        # already; scaling the intensity as well would count it twice.
        if env.get("mode") == "sky":
            k = env.get("sky_scale", 1.0)
            if not isinstance(k, (int, float)):
                raise ValueError(f"_scale_emitters: environment.sky_scale {k!r} has no number to "
                                 "scale; author the scale as a number in a scale fixture")
            env["sky_scale"] = k * factor
        elif "intensity" in env:
            env["intensity"] *= factor
        if "ambient" in env:
            env["ambient"] = [c * factor for c in env["ambient"]]

    fog = d.get("post", {}).get("fog")
    if fog and "ambient" in fog:
        fog["ambient"] = [c * factor for c in fog["ambient"]]
# The circuit board at true scale (gen_board_fixture.py) in its three views: the scene file's
# three-quarter camera, the whole board, and 2 mm from an 0402. Its golden is the first.
BOARD_VIEWS = {
    "quarter": [],
    "whole": ["--cam-eye", "0,0.11,0.075", "--cam-target", "0,0,0.004"],
    "close": ["--cam-eye", "-0.00897,0.00292,0.01329", "--cam-target", "-0.01,0.0018,0.012"],
}


def _projector(cam, w, h):
    """World -> pixel for a fixture camera dict (eye/target/fovy_deg).

    cglm's perspective fov is VERTICAL.
    """
    def sub(a, b):
        return tuple(x - y for x, y in zip(a, b))

    def norm(v):
        m = math.sqrt(sum(c * c for c in v))
        return tuple(c / m for c in v)

    def cross(a, b):
        return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])

    def dot(a, b):
        return sum(x * y for x, y in zip(a, b))

    eye, target = cam["eye"], cam["target"]
    fwd = norm(sub(target, eye))
    right = norm(cross(fwd, (0.0, 1.0, 0.0)))
    up = cross(right, fwd)
    # An orthographic camera carries its view height instead of a fov: the NDC
    # scale is then 2/height and no depth enters -- the Python twin of the
    # shader's clip w, 1 under ortho and the depth under perspective.
    ortho_h = cam.get("ortho_height", 0.0)
    if ortho_h > 0.0:
        ty = 2.0 / ortho_h
    else:
        ty = 1.0 / math.tan(math.radians(cam["fovy_deg"]) * 0.5)
    tx = ty / (w / h)

    def project(p):
        d = sub(p, eye)
        vx, vy, vz = dot(d, right), dot(d, up), -dot(d, fwd)
        cw = 1.0 if ortho_h > 0.0 else -vz
        return (((vx * tx) / cw * 0.5 + 0.5) * w, (0.5 - (vy * ty) / cw * 0.5) * h)

    return project


def _world_pixel_box(project, lo, hi, inset):
    """The pixel box a world-space rectangle (two opposite corners) covers, `inset` px in."""
    pts = [project((x, y, z)) for x in (lo[0], hi[0]) for y in (lo[1], hi[1])
           for z in (lo[2], hi[2])]
    return (int(min(p[0] for p in pts)) + inset, int(min(p[1] for p in pts)) + inset,
            int(max(p[0] for p in pts)) - inset, int(max(p[1] for p in pts)) - inset)
# The eligibility fixture: one skinned mesh on two nodes. No .cscn -- it needs
# no look, only a program that cannot read InstanceBlock.
SKIN_FIXTURE = "skinned_instance_fixture.gltf"


MASK_FIXTURE = "mask_fixture.cscn"

# Quad interiors as fractions of the frame, so the arm reads the same surface
# whatever the display scale reports. Inset well clear of the silhouettes, which
# is where TAA and the geometric AA live and neither is what this measures.
#
# Measured from the fixture rather than derived: they are tied to
# mask_fixture.cscn's camera AND to the 4:3 that `render` renders at, so a box
# that stopped landing on its quad would read the backdrop and fail loudly --
# 0.048 against a 0.47 surface is not a near miss.
#
# REF and KEPT are frame-symmetric (0.2175 and 0.7812 about the centre), and
# that is load-bearing: the vignette falls off with radius, so equal radii make
# it cancel between the pair instead of biasing one. Re-centring either box on
# its quad would quietly remove that.
MASK_REF_BOX = (0.165, 0.42, 0.270, 0.58)
# Mean linear luma of a lit quad against this fixture's backdrop, which measures
# 0.048. A load failure leaves every box reading that same background, which
# would satisfy an equality arm perfectly -- so the surfaces have to be shown to
# be THERE before their agreement means anything.
MASK_LIT_FLOOR = 0.15

FOGVOL_FIXTURE = "fog_volume_fixture.cscn"
# Three strips at the same rows, so the vignette and the tone curve cancel out of every
# comparison. left and right are the two boxes; gap is bare backdrop between them.
FOGVOL_BOXES = {
    "left": (0.18, 0.44, 0.28, 0.72),
    "gap": (0.45, 0.44, 0.55, 0.72),
    "right": (0.72, 0.44, 0.82, 0.72),
}
# SIXTY frames, not the shared default of 30, and this is load-bearing. The froxel volume
# has its own temporal accumulator, and at 30 frames this fixture's boxes have not settled
# -- two runs of ONE build differ by 3/255 across the whole frame. At 60 they are byte
# identical and the gap reads the exact no-medium value. See spec 11.39.
FOGVOL_FRAMES = "60"
# The boxes must darken the backdrop they stand in front of; measured 0.79 of it. A wide
# floor, because the arm's job is "a volume does something", not a specific opacity.
FOGVOL_DARKEN_MAX = 0.92


# Water surface (spec 11.32), on assets/water_fixture.
#
# A ramp rising out of the water over a flat bed. Absorption is monotone in path
# length, and on a flat bed the path GROWS with distance because the sight line
# gets more grazing -- so a scanline up the frame is a continuous sweep of path
# length across one surface, and the tinted channel has to fall along it.
#
# Red is the channel to read: clear water's extinction is ~7x higher in red than
# in blue, so R/B is the absorption signal and blue is very nearly the reference.
# Measured near/mid/far: 0.2133, 0.1452, 0.0938.
#
# The horizon is deliberately NOT sampled. There the Fresnel reflection of the
# sky dominates the transmitted term and R/B rises again (0.328) -- correctly, and
# it would break a monotonicity arm that reached that far. An arm has to stop
# where the quantity it names stops being the thing on screen.
WATER_FIXTURE = "water_fixture.cscn"
WATER_BOUNDS_AWAY = "1000,1000,1001,1001"
WATER_DRY_OVER = "0,0,0,1e4,1e4"  # over everything
PROBE_SKY = ["--no-auto-exposure", "-E", "1.0"]
CORNELL_FIXTURE = "cornell_box.cscn"


# --- block-compressed textures (spec 11.85) ---------------------------------
TEXCOMP_FIXTURE = "texcomp_fixture.cscn"


RENDER_CAM = re.compile(
    r"^cam (\d+) eye (\S+) (\S+) (\S+) target (\S+) (\S+) (\S+) "
    r"dist (\S+) theta (\S+) phi (\S+) ortho (\S+) near (\S+) far (\S+)$", re.M)
