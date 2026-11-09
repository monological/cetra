"""The scale-twin group: the same scene shrunk 1024 times, to the pixel (spec 13.57)."""

import json
import re
import os

from gatelib.harness import ROOT, asset, render
from gatelib.images import compare
from gatelib.scenes import BOARD_VIEWS, cscn_copy


# The scale twin (spec 13.57): the same scene shrunk by TWIN_K renders the same picture. Every
# length the scene states is multiplied by K and every physical quantity matched to it, so a
# twin differs only where the ENGINE holds a length of its own.
#
# K is a power of two because multiplying by one is exact in floating point, and so is every
# sum, difference, product, quotient and square root of numbers it has scaled. So the answer is
# 0 px and not a tolerance: a pixel that moves names a length that was missed. The one place
# the argument fails is a length stored under fp16's normal range (6.1e-5 m), which a twin
# whose nearest surface is 6 cm or more from the eye stays clear of.
TWIN_K = 2.0 ** -10
# The corpus pins exposure; every row repeats it, as the golden it came from does.
TWIN_PIN = ["--no-auto-exposure", "-E", "1.0"]
# (name, scene, flags, frames, K). The golden corpus's scenes at their recipes' flags, less those
# whose physics does not scale with the scene:
#   water_fixture, water_submerged  gravity sets the waves' lengths and speeds
#   local_exposure                  abandoned_window carries dust particles and a wind
#   puppet                          an animated rig
# and tile_core for the cached tiles, which no golden renders. `calibrate` is the instrument's
# floor: its path holds no length of the engine's (no shadow map, AO, SSR or contact shadows),
# so it must be 0 px whatever else is true. It is also run GROWN, since a floor written
# max(x, metres) only bites a shrink: grown, it is the exactness argument checked alone.
TWIN_CALIBRATE = TWIN_PIN + ["--no-shadows", "--no-ssao", "--no-ssr", "--cs-distance", "0"]
# The framing two of the area-light rows share.
TWIN_AREA_VIEW = ["--no-scene-file", "--cam-eye", "0,1.6,6", "--cam-target", "0,0.5,0"]
# Its exposure, pinned where auto-exposure settles on the three-quarter view, and the contact
# shadows a board's pictures take -- which reach no light here: the panels are area lights and
# the lamp casts a map of its own.
BOARD_LOOK = ["--no-auto-exposure", "-E", "0.0088", "--contact-shadows"]
TWIN_ROWS = [
    ("calibrate", "dir_shadow_fixture.cscn", TWIN_CALIBRATE, 30, TWIN_K),
    ("calibrate-grown", "dir_shadow_fixture.cscn", TWIN_CALIBRATE, 30, 1.0 / TWIN_K),
    ("aerial", "aerial_fixture.cscn", TWIN_PIN, 30, TWIN_K),
    ("clouds", "aerial_fixture.cscn", TWIN_PIN + ["--clouds"], 30, TWIN_K),
    ("clouds-lowsun", "aerial_fixture.cscn", TWIN_PIN + ["--clouds", "--sun-elevation", "5"], 30,
     TWIN_K),
    ("froxel-fog", "contact_fixture.cscn", TWIN_PIN + ["--fog", "--no-aerial"], 60, TWIN_K),
    ("contact-debug", "contact_fixture.cscn", TWIN_PIN + ["--cs-debug"], 120, TWIN_K),
    ("ao", "ao_fixture.cscn", TWIN_PIN, 30, TWIN_K),
    ("translucent-shadow", "translucent_shadow_fixture.cscn",
     TWIN_PIN + ["--no-pcss", "--translucent-shadows"], 30, TWIN_K),
    ("mask", "mask_fixture.cscn", TWIN_PIN, 30, TWIN_K),
    ("layer", "layer_fixture.cscn", TWIN_PIN, 30, TWIN_K),
    ("layer-vt", "layer_vt_fixture.cscn", TWIN_PIN, 30, TWIN_K),
    ("cornell-box", "cornell_box.cscn", TWIN_PIN + ["--gi-volume"], 30, TWIN_K),
    ("cornell-leak", "cornell_leak.cscn", TWIN_PIN, 30, TWIN_K),
    ("cornell-point", "cornell_point.cscn", TWIN_PIN, 30, TWIN_K),
    ("cornell-rooms", "cornell_rooms.cscn", TWIN_PIN, 30, TWIN_K),
    ("dir-shadow", "dir_shadow_fixture.cscn", TWIN_PIN, 30, TWIN_K),
    ("ortho-shadow", "dir_shadow_fixture.cscn", TWIN_PIN + ["--ortho", "12"], 30, TWIN_K),
    ("roughness-sweep", "area_light_fixture.gltf",
     TWIN_PIN + TWIN_AREA_VIEW + ["--area-light", "0,2.2,1.2,0,-0.6,-0.8,2.0,0.7,30"], 2, TWIN_K),
    ("backface-dark", "area_light_fixture.gltf",
     TWIN_PIN + TWIN_AREA_VIEW + ["--area-light", "0,2.2,1.2,0,0.6,0.8,2.0,0.7,30"], 2, TWIN_K),
    ("guard-thin-panel", "area_light_guard.gltf",
     TWIN_PIN + ["--no-scene-file", "--fov", "50.0",
                 "--cam-eye", "0.549,0.780,1.525", "--cam-target", "0.549,0.500,0.000",
                 "--area-light", "0,2.2,1.2,0,-0.6,-0.8,0.25,1.8,30"], 2, TWIN_K),
    ("sheen", "sheen_fixture.gltf", TWIN_PIN, 30, TWIN_K),
    ("specular", "specular_fixture.gltf", TWIN_PIN, 30, TWIN_K),
    ("clearcoat", "clearcoat_fixture.gltf", TWIN_PIN, 30, TWIN_K),
    ("skin-curvature", "skin_curvature_fixture.cscn",
     TWIN_PIN + ["--no-bloom", "--no-sss", "--no-shadows"], 30, TWIN_K),
    ("skin-sss", "skin_curvature_fixture.cscn", TWIN_PIN + ["--no-bloom", "--no-shadows"], 30,
     TWIN_K),
    ("oit-moments", "oit_sphere_fixture.gltf",
     ["--no-auto-exposure", "-E", "0.08", "--oit-moments"], 30, TWIN_K),
    ("dof", "dof_fixture.cscn",
     TWIN_PIN + ["--no-bloom", "--dof", "--dof-focus", "6", "--dof-range", "1.5",
                 "--dof-max-coc", "8", "--dof-blades", "6", "--dof-rotation", "15"], 30, TWIN_K),
    ("flare", "flare_fixture.cscn",
     TWIN_PIN + ["--flare", "0.15", "--chromatic-aberration", "12"], 30, TWIN_K),
    ("purkinje", "purkinje_fixture.cscn",
     ["--no-auto-exposure", "--no-dither", "--no-vignette", "--no-bloom", "--no-ssao",
      "--purkinje"], 30, TWIN_K),
    ("tile-core", "tile_core_fixture.cscn", TWIN_PIN, 30, TWIN_K),
    # GROWN: the board's view depths run from a millimetre to a few centimetres, and shrunk a
    # further 1024 times they fall under fp16's normal range, the one exception TWIN_K states.
    *((f"board-{view}", "board_fixture.cscn", BOARD_LOOK + flags, 30, 1.0 / TWIN_K)
      for view, flags in BOARD_VIEWS.items()),
]
# The render app's flags the rows pass, by what their argument is: a switch takes none, a value
# is unitless, and a length's comma-separated components are multiplied by K -- all of them, or
# those a tuple names. A flag not here is refused, as a scene key is.
TWIN_SWITCH, TWIN_VALUE, TWIN_LENGTH = "switch", "value", "length"
TWIN_FLAGS = {
    **dict.fromkeys(
        ("--no-auto-exposure", "--no-shadows", "--no-ssao", "--no-ssr", "--clouds", "--fog",
         "--no-aerial", "--cs-debug", "--no-pcss", "--translucent-shadows", "--gi-volume",
         "--no-scene-file", "--no-bloom", "--no-sss", "--oit-moments", "--dof", "--no-dither",
         "--no-vignette", "--purkinje", "--contact-shadows"), TWIN_SWITCH),
    **dict.fromkeys(
        ("-E", "--sun-elevation", "--fov", "--dof-max-coc", "--dof-blades", "--dof-rotation",
         "--flare", "--chromatic-aberration"), TWIN_VALUE),
    **dict.fromkeys(
        ("--cam-eye", "--cam-target", "--ortho", "--dof-focus", "--dof-range", "--cs-distance"),
        TWIN_LENGTH),
    "--area-light": (0, 1, 2, 6, 7),  # px,py,pz,dx,dy,dz,w,h,I[,r,g,b]
}
# Flags that make the render app build a sky. --world-scale is another, which is why a twin
# passes it only where its original already has one.
TWIN_SKY_FLAGS = ("--sky", "--clouds", "--sun-elevation", "--sun-azimuth")
# How each block of a .cscn a twin walks is shrunk, key by key: LEN is a length, PER_LEN per
# length (extinction per metre), KEEP neither, and a key in none of them is refused.
TWIN_LEN, TWIN_PER_LEN, TWIN_KEEP = "len", "per_len", "keep"
TWIN_LIGHT_KEYS = {
    **dict.fromkeys(("position", "range", "size", "source_radius", "source_length",
                     "shadow_near"), TWIN_LEN),
    **dict.fromkeys(("name", "type", "color", "intensity", "intensity_unit", "direction", "up",
                     "cone", "profile", "cast_shadows", "shadow_cache", "specular",
                     "shadow_follow"), TWIN_KEEP),
}
TWIN_PROBE_KEYS = {**dict.fromkeys(("position", "boxMin", "boxMax"), TWIN_LEN),
                   **dict.fromkeys(("intensity", "boxFade", "envOnly"), TWIN_KEEP)}
TWIN_FOG_KEYS = {**dict.fromkeys(("near", "far"), TWIN_LEN), "density": TWIN_PER_LEN,
                 **dict.fromkeys(("enabled", "anisotropy", "depthDistribution", "ambient"),
                                 TWIN_KEEP)}
# post's other blocks are exposure, tone and lens settings, none of them a length.
TWIN_POST_KEEP = ("tonemap", "exposure", "auto_exposure", "camera", "render_scale", "flare",
                  "bloom", "chromatic_aberration", "metering", "lut", "purkinje", "glare",
                  "local_exposure")
TWIN_SSS_KEYS = {"radius": TWIN_LEN, "color": TWIN_KEEP}
TWIN_LAYER_KEYS = {"uvScale": TWIN_LEN, "albedo": TWIN_KEEP, "surface": TWIN_KEEP}
# Material keys whose lengths a twin cannot reach: a shader's own numbers, and roads.
TWIN_MATERIAL_REFUSED = ("roads", "shaderParams", "lateShader", "shaderHook")


def _material_params():
    """material.c's MATERIAL_PARAMS: every row's name, and the names of the rows marked lengths.

    Read from the source rather than restated, so a length added there is shrunk by the twin
    the day it is added rather than copied as a ratio.
    """
    with open(os.path.join(ROOT, "cetra", "src", "material.c")) as f:
        src = f.read()
    table = src[src.index("MATERIAL_PARAMS[] = {"):]
    table = table[:table.index("\n};")]
    names = set(re.findall(r'\{"(\w+)",\s*"', table))
    lengths = set(re.findall(r'\{"(\w+)",[^{}]*\.length\s*=\s*true', table))
    return names, lengths


def _twin_scene(d, k):
    """Shrink a .cscn dict by `k` in place: every length it states times k, its physics matched.

    Point and spot intensity goes as k^2, so the light a surface receives is unchanged; a
    directional's lux and a panel's nits are already per area and stay. Fog density is per
    metre and goes as 1/k. Nothing in `environment` is a length: angles, radiances and the sky's
    own kilometres, which the render app joins to the scene through --world-scale.

    A key the twin does not know is REFUSED by name rather than copied. Copied unscaled it would
    render a twin that differs, and the difference would read as a length the engine holds.
    """
    def times(v, f=k):
        return [x * f for x in v] if isinstance(v, list) else v * f

    def refuse(what):
        raise ValueError(f"the scale twin does not know how to shrink {what}")

    def shrink(block, rules, where):
        for sub in block:
            rule = rules.get(sub)
            if rule == TWIN_LEN:
                block[sub] = times(block[sub])
            elif rule == TWIN_PER_LEN:
                block[sub] = times(block[sub], 1.0 / k)
            elif rule != TWIN_KEEP and not sub.startswith("_"):
                refuse(f"{where}.{sub}")

    param_names, param_lengths = _material_params()
    for key in list(d):
        if key.startswith("_") or key in ("version", "models", "environment"):
            continue
        if key == "typicalSize":
            d[key] = times(d[key])
        elif key == "camera":
            shrink(d["camera"], {"eye": TWIN_LEN, "target": TWIN_LEN, "fov": TWIN_KEEP,
                                 "fitClip": TWIN_KEEP}, "camera")
        elif key == "lights":
            for light in d["lights"]:
                shrink(light, TWIN_LIGHT_KEYS, f"light {light.get('name')!r}")
                if light.get("type", "point") in ("point", "spot"):
                    # Absent, the loader seeds it from an IES file's own candela, which the
                    # twin cannot reach.
                    if "intensity" not in light:
                        refuse(f"light {light.get('name')!r} with no authored intensity")
                    light["intensity"] *= k * k
        elif key == "probes":
            for probe in d["probes"]:
                shrink(probe, TWIN_PROBE_KEYS, "probe")
        elif key == "materials":
            for name, m in d["materials"].items():
                if not isinstance(m, dict):
                    continue
                where = f"materials.{name}"
                for sub in m:
                    if sub in TWIN_MATERIAL_REFUSED:
                        refuse(f"{where}.{sub}")
                    elif sub == "sss":
                        # Absent, the radius is the engine's default profile's.
                        if "radius" not in m["sss"]:
                            refuse(f"{where}.sss with no radius")
                        shrink(m["sss"], TWIN_SSS_KEYS, f"{where}.sss")
                    elif sub == "layers":
                        for layer in m["layers"]:
                            if "uvScale" not in layer:
                                refuse(f"a layer of {where} with no uvScale")
                            shrink(layer, TWIN_LAYER_KEYS, f"a layer of {where}")
                    elif sub in param_lengths or sub == "splatDomain":
                        m[sub] = times(m[sub])
                    elif sub not in param_names and not sub.startswith("_"):
                        refuse(f"{where}.{sub}")
        elif key == "post":
            for sub in d["post"]:
                if sub == "fog":
                    shrink(d["post"]["fog"], TWIN_FOG_KEYS, "post.fog")
                elif sub not in TWIN_POST_KEEP:
                    refuse(f"post.{sub}")
        else:
            refuse(key)


def _twin_flags(extra, k, sky, size):
    """The render app's flags for the twin: lengths times `k`, with the import, any `sky`, and
    a typical size where the scene states `size` none.

    A component is rescaled through repr, which a C strtod reads back as the same double, so
    the float the app stores is exactly K times the original's.
    """
    out, i = [], 0
    while i < len(extra):
        flag = extra[i]
        kind = TWIN_FLAGS.get(flag)
        if kind is None:
            raise ValueError(f"the scale twin does not know how to shrink {flag}")
        out.append(flag)
        i += 1
        if kind == TWIN_SWITCH:
            continue
        arg = extra[i]
        if kind != TWIN_VALUE:
            arg = ",".join(repr(float(p) * k) if kind == TWIN_LENGTH or j in kind else p
                           for j, p in enumerate(arg.split(",")))
        out.append(arg)
        i += 1
    out += ["--import-scale", repr(k)]
    if size is None:
        out += ["--typical-size", repr(k)]
    return out + ["--world-scale", repr(1000.0 * k)] if sky else out


def run_scale_twin_gate(workdir):
    """The same scene shrunk 1024 times renders the same picture, to the pixel (spec 13.57).

    Each row renders its fixture twice, as it is and as its twin, and passes only at 0 px: the
    twin is exact by construction (see TWIN_K), so a moved pixel is a length of the engine's own
    that did not follow the scene. Both legs render a copy of the scene through cscn_copy, so the
    two differ in nothing but the twin's arithmetic.
    """
    failures = []
    originals = {}  # (scene, flags, frames) -> the original leg, which two rows can share

    def leg(name, tag, src, mutate, flags, frames):
        path = src
        if src.endswith(".cscn"):
            path = cscn_copy(src, os.path.join(workdir, f"twin_{name}_{tag}.cscn"), mutate)
        out = os.path.join(workdir, f"twin_{name}_{tag}.ppm")
        err = render(path, out, flags, frames=frames)
        if err:
            print(f"  {name:<20} ERROR rendering leg {tag}: {err.strip()[-200:]}")
            return None
        return out

    for name, scene, extra, frames, k in TWIN_ROWS:
        src = asset(scene)
        sky, size = any(f in TWIN_SKY_FLAGS for f in extra), None
        if src.endswith(".cscn"):
            with open(src) as f:
                d = json.load(f)
            sky = sky or d.get("environment", {}).get("mode") == "sky"
            size = d.get("typicalSize")
        try:
            twin_flags = _twin_flags(extra, k, sky, size)
            key = (scene, tuple(extra), frames)
            if key not in originals:
                originals[key] = leg(name, "a", src, lambda d: None, list(extra), frames)
            a = originals[key]
            b = a and leg(name, "b", src, lambda d: _twin_scene(d, k), twin_flags, frames)
        except ValueError as e:
            print(f"  {name:<20} REFUSED  {e}")
            failures.append(name)
            continue
        if not b:
            failures.append(name)
            continue
        with open(a, "rb") as fa, open(b, "rb") as fb:
            same = fa.read() == fb.read()
        ae, pae = (0, 0.0) if same else compare(a, b)
        ok = ae == 0
        print(f"  {name:<20} {'PASS' if ok else 'FAIL'}  {ae} px, peak {pae * 255:.0f}/255")
        if not ok:
            failures.append(name)
    return failures
