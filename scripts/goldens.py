#!/usr/bin/env python3
"""Check and re-bake the golden reference renders (spec 11.25).

    python3 scripts/goldens.py                      # check all of them
    python3 scripts/goldens.py --only dof_fixture   # check one
    python3 scripts/goldens.py --list
    python3 scripts/goldens.py --rebake roughness_sweep
    python3 scripts/goldens.py --rebake             # all of them

A golden is a stored PNG; checking one means re-rendering its scene and
requiring 0 differing pixels. That check is objective and this file adds nothing
to it. What it adds is RECIPES -- the first single place recording which images
exist and what command makes each one.

WHY THIS EXISTS: the recipes used to live in whichever spec introduced each
golden, plus one per-feature ledger, and nothing enumerated them. Three specs in
a row then concluded that goldens were unrecoverable which were not -- 11.17
named three, 11.21 repeated it and named four, 11.24 named six and wrote it into
the roadmap. Every one was reproducible. Four had their recipe sitting in
assets/area_light_goldens.md; the two "cloud_fixture" goldens turned out not to
name a fixture at all, since no such file has ever existed in this repository --
they are the aerial fixture with --clouds.

The rule that follows: a render command lives HERE and nowhere else. Prose about
what a golden should look like belongs in a document (assets/area_light_goldens.md
is the model), but the command itself must have exactly one home, because a
command in two places drifts and the drift stays silent until someone spends a
session re-deriving it.

Deliberately NOT part of gates.py. A gate is self-verifying -- it compares two
renders of one build, so it needs no stored reference and can run anywhere. A
golden needs a committed "before" and cannot.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gates import compare  # noqa: E402  (the (AE, PAE) shell-out, already written)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Same override gates.py takes, and the same env var, so one export covers both.
#
# But note the goldens are COMMITTED PNGs and a build type is part of what baked
# them: -O2 changes CPU float results (AGENTS.md records the erosion digest
# differing between -O0 and -O2). Checking release-built frames against
# debug-baked goldens is a comparison across two builds, which this repo's own
# rule says does not hold. Change the build type here only together with
# --rebake, and only deliberately.
def _bin(name):
    """An app binary in BIN_DIR, spelled the way the host names executables."""
    return os.path.join(BIN_DIR, name + (".exe" if sys.platform == "win32" else ""))


BIN_DIR = os.environ.get("CETRA_BIN_DIR") or os.path.join(ROOT, "out", "bin")
RENDER = _bin("render")

# Framebuffer pixels per requested pixel, measured once per run. Every recipe
# below states both numbers -- a "size" that is the stored golden's, and a -W/-H
# in its flags that is half of it -- because these were baked on a HiDPI display,
# where a request comes back doubled. _env_mismatch already reports the fallout
# on a 1x machine; this is what stops it happening, by asking for whatever lands
# on the recipe's own stated size.
_FB_SCALE = None
ASSETS = os.path.join(ROOT, "assets")

# Every recipe carries the frame size it was baked at. `size` is the size of the
# STORED PNG, which is the framebuffer size -- double the requested -W/-H on a
# HiDPI display. Recording it is what lets a 1x machine say "this environment
# renders half-size" instead of reporting nineteen regressions.
#
# `-x` and `-S <out>` are added by the runner; every recipe needs them and needs
# them identically.
RECIPES = [
    # --- environment and atmosphere -------------------------------------------
    {"name": "aerial_fixture", "scene": "assets/aerial_fixture.gltf", "size": (1600, 1000),
     "flags": ["-f", "30", "-W", "800", "-H", "500", "--no-auto-exposure", "-E", "1.0"]},
    # cloud_fixture is NOT a file and never was: it is the aerial fixture with
    # clouds switched on. Recovered by measurement in 11.25 after three specs
    # recorded it as unreproducible.
    {"name": "cloud_fixture", "scene": "assets/aerial_fixture.gltf", "size": (1600, 1000),
     "flags": ["--clouds", "-f", "30", "-W", "800", "-H", "500",
               "--no-auto-exposure", "-E", "1.0"]},
    {"name": "cloud_fixture_lowsun", "scene": "assets/aerial_fixture.gltf", "size": (1600, 1000),
     "flags": ["--clouds", "--sun-elevation", "5", "-f", "30", "-W", "800", "-H", "500",
               "--no-auto-exposure", "-E", "1.0"]},
    # --no-aerial is what keeps this a FOG golden rather than an atmosphere one.
    #
    # SIXTY frames, where the rest of this table uses 30, and it is the froxel
    # volume's own accumulator that asks for it: an alpha=0.1 EWMA still retains
    # 4.2% of its cold start at frame 30 and 0.18% -- sub-LSB -- at 60. Baked at
    # 30 this golden was pinning a transient, which is why fixing the frame-0
    # history sentinel in postfx.c moved it by 25,716 px while every 60-frame fog
    # ARM was unmoved. At 60 it pins the converged fog instead, and is insensitive
    # to that whole class of cold-start change.
    #
    # The two cloud goldens above share the accumulator but NOT the problem:
    # sky_clouds.c always carried the >= 0 guard postfx.c lacked, so their frame
    # 30 is a stable, reproducible point rather than a wrong one. They are left
    # alone because nothing forced them and a golden's job is reproducibility.
    {"name": "froxel_fog", "scene": "assets/contact_fixture.gltf", "size": (1280, 800),
     "flags": ["--fog", "--no-aerial", "-f", "60", "--no-auto-exposure", "-E", "1.0",
               "-W", "640", "-H", "400"]},
    # The water surface (spec 11.32). The surface and every one of its properties come
    # from the fixture's own `water` block since 11.33 phase 5, so this asks for none of
    # it -- which makes the golden the one thing that guards that authoring path across
    # builds. Exposure is pinned by the .cscn as well, but repeated here: a golden is
    # compared across BUILDS, where auto-exposure is the largest known source of drift.
    {"name": "water_fixture", "scene": "assets/water_fixture.cscn", "size": (800, 600),
     "flags": ["-f", "30", "-W", "400", "-H", "300", "--no-auto-exposure", "-E", "1.0"]},
    # The same fixture with the surface raised over the fixed eye at y 1.35, so the camera
    # is UNDER it (spec 11.40). The one thing the sibling above cannot see.
    #
    # It exists for a change 11.39 shipped and could not measure: the global fog density now
    # uploads as zero when the app did not ask for fog, which can only move frames where
    # fog_enabled is false AND water_medium is set -- submerged ones. Every other golden is
    # above water, so the 0 px run 11.39 cited as evidence was guaranteed whatever the change
    # did. Measured here: reproducing the old unconditional upload (--fog-density 0.004032,
    # which is 0.08 / this fixture's 19.84 scene radius) moves 4,339 px, so this frame is
    # genuinely sensitive to the quantity and would trip if it were ever re-widened.
    #
    # --water-level rather than --cam-eye because the gate arms already prove this exact
    # configuration renders; flags otherwise match the sibling, so the two differ in one
    # variable. Deterministic x2 at 30 frames, which is not automatic -- the submerged path
    # arms the froxel volume, and 11.39 found the volume-COUNT arming route unstable there.
    {"name": "water_submerged", "scene": "assets/water_fixture.cscn", "size": (800, 600),
     "flags": ["--water-level", "1.9", "-f", "30", "-W", "400", "-H", "300",
               "--no-auto-exposure", "-E", "1.0"]},

    # --- shadows and occlusion ------------------------------------------------
    # The --cs-debug term itself, so the image is independent of cs_strength.
    # Debug views return early from the tonemap, so this one carries no dither.
    {"name": "contact_debug", "scene": "assets/contact_fixture.gltf", "size": (1280, 800),
     "flags": ["-W", "640", "-H", "400", "-f", "120", "--no-auto-exposure", "-E", "1.0",
               "--cs-debug"]},
    {"name": "ao_fixture", "scene": "assets/ao_fixture.gltf", "size": (1600, 1000),
     "flags": ["-f", "30", "-W", "800", "-H", "500", "--no-auto-exposure", "-E", "1.0"]},
    # The gate arms sample lines and erode every band edge by construction. What
    # a scalar cannot see is two-dimensional: acne in the transmittance target,
    # a silhouette losing its shape, a cascade seam cutting a band diagonally --
    # which is exactly how the half-covered resolve presented before it was
    # found. Dither and vignette stay ON: this is a picture, not a data read.
    # --no-pcss because at a canopy height of 8 the default emitter smears the
    # bands into each other and the image stops being legible.
    {"name": "translucent_shadow", "scene": "assets/translucent_shadow_fixture.cscn",
     "size": (1600, 1000),
     "flags": ["-f", "30", "-W", "800", "-H", "500", "--no-auto-exposure", "-E", "1.0",
               "--no-pcss", "--translucent-shadows"]},

    # The only golden that can SEE masked geometry, and it exists because the
    # other twenty pass whether masked rendering is right or wrong. The corpus's
    # one other MASK material is translucent_shadow's ramp panel, which is a
    # caster held above the frame -- what that golden measures is its shadow. So
    # the blend defect spec 11.31 fixed, which moved 12.8% of the raiden recipe,
    # moved exactly zero stored pixels and had survived two specs undetected.
    #
    # NO --taa, deliberately: the default 4x MSAA path is the one where
    # alpha-to-coverage is live, so this covers the coverage chain as well as the
    # cutoff. The three quads are an opaque reference, a MASK quad above the
    # cutoff and one below it, over a backdrop that makes wrong coverage visible
    # as occlusion rather than invisible.
    {"name": "mask_fixture", "scene": "assets/mask_fixture.cscn", "size": (800, 600),
     "flags": ["-f", "30", "-W", "400", "-H", "300", "--no-auto-exposure", "-E", "1.0"]},

    # The layered surface, LIT (spec 11.60). The gate arms all read the albedo
    # view, which has no BRDF in it at all -- so without this nothing covers a
    # layered material actually being shaded: its world-space normal reaching the
    # lighting, its per-layer roughness, or the blend holding together across the
    # floor/wall seam where two projections meet.
    {"name": "layer_fixture", "scene": "assets/layer_fixture.cscn", "size": (800, 600),
     "flags": ["-f", "30", "-W", "400", "-H", "300", "--no-auto-exposure", "-E", "1.0"]},

    # The composite cache, LIT (spec 11.66). The vt gate arms read the albedo
    # view, so without this nothing covers the CACHED path being shaded: the
    # macro normal recomposed onto the geometric normal, the detail normal
    # whiteout on top of it, and per-layer roughness/AO arriving as deviations
    # from the baked means. Also the loudest nondeterminism trap for the bake --
    # cmd_rebake renders twice and refuses on any difference.
    {"name": "layer_vt_fixture", "scene": "assets/layer_vt_fixture.cscn", "size": (800, 600),
     "flags": ["-f", "30", "-W", "400", "-H", "300", "--no-auto-exposure", "-E", "1.0"]},

    # --- global illumination and punctual shadows -----------------------------
    {"name": "cornell_box", "scene": "assets/cornell_box.gltf", "size": (1600, 1200),
     "flags": ["--gi-volume", "-f", "30", "--no-auto-exposure", "-E", "1.0",
               "-W", "800", "-H", "600"]},
    {"name": "cornell_leak", "scene": "assets/cornell_leak.gltf", "size": (1600, 1200),
     "flags": ["-f", "30", "--no-auto-exposure", "-E", "1.0", "-W", "800", "-H", "600"]},
    {"name": "cornell_point", "scene": "assets/cornell_point.gltf", "size": (1600, 1200),
     "flags": ["-f", "30", "--no-auto-exposure", "-E", "1.0", "-W", "800", "-H", "600"]},
    # Two rooms over one polished floor, a probe in each (spec 11.70). The only
    # committed frame with more than one reflection probe in it, and the only
    # one that renders the atlas blend at all -- the gate arms beside it read
    # patches and ratios, which is what a golden cannot do and vice versa.
    # The fixture's own `environment` block is load-bearing rather than
    # decoration: a probe cannot be created without a precomputed IBL, and a sun
    # below the horizon supplies one without lighting the rooms. It is authored
    # there rather than passed here so that opening the file by hand renders
    # what the arms render.
    {"name": "cornell_rooms", "scene": "assets/cornell_rooms.gltf", "size": (1600, 1200),
     "flags": ["-f", "30", "--no-auto-exposure", "-E", "1.0", "-W", "800",
               "-H", "600"]},

    # The cascade fixture, here for a reason the other cascade coverage does not
    # give: run_dir_shadow_gate and run_translucent_offpath_gate both read it
    # WITHIN one build (a ratio against its own --no-shadows reference, and an
    # on-vs-off identity), so neither can see a change that moves the whole
    # frame. This is the only stored reference it has across builds. Matches
    # _dir_render's flags so a golden failure and a gate failure describe the
    # same picture.
    {"name": "dir_shadow", "scene": "assets/dir_shadow_fixture.cscn", "size": (1600, 1200),
     "flags": ["-f", "30", "--no-auto-exposure", "-E", "1.0", "-W", "800", "-H", "600"]},

    # --- LTC area lights (prose: assets/area_light_goldens.md) ----------------
    # --no-scene-file is required: the fixture has a sibling .cscn the app would
    # auto-load, which lights the scene with a second panel. No -e <hdr>: the
    # HDRs live outside the repo, so a golden needing one could not be
    # regenerated by anyone else.
    {"name": "roughness_sweep", "scene": "assets/area_light_fixture.gltf", "size": (1280, 720),
     "flags": ["-W", "640", "-H", "360", "-f", "2", "--no-auto-exposure", "-E", "1.0",
               "--no-scene-file", "--cam-eye", "0,1.6,6", "--cam-target", "0,0.5,0",
               "--area-light", "0,2.2,1.2,0,-0.6,-0.8,2.0,0.7,30"]},
    # The same panel with its normal negated: every sphere and the ground must
    # be pure black, since a panel lights only the half-space it points into.
    {"name": "backface_dark", "scene": "assets/area_light_fixture.gltf", "size": (1280, 720),
     "flags": ["-W", "640", "-H", "360", "-f", "2", "--no-auto-exposure", "-E", "1.0",
               "--no-scene-file", "--cam-eye", "0,1.6,6", "--cam-target", "0,0.5,0",
               "--area-light", "0,2.2,1.2,0,0.6,0.8,2.0,0.7,30"]},
    # The framing is load-bearing: dead-on the edge-singularity artifact is
    # faint, and the grazing offset is where it blows up.
    {"name": "guard_thin_panel", "scene": "assets/area_light_guard.gltf", "size": (1600, 1200),
     "flags": ["--no-scene-file", "-W", "800", "-H", "600", "--fov", "50.0", "-f", "2",
               "--no-auto-exposure", "-E", "1.0",
               "--cam-eye", "0.549,0.780,1.525", "--cam-target", "0.549,0.500,0.000",
               "--area-light", "0,2.2,1.2,0,-0.6,-0.8,0.25,1.8,30"]},

    # --- materials ------------------------------------------------------------
    {"name": "sheen_fixture", "scene": "assets/sheen_fixture.gltf", "size": (1600, 900),
     "flags": ["-f", "30", "-W", "800", "-H", "450", "--no-auto-exposure", "-E", "1.0"]},
    {"name": "specular_fixture", "scene": "assets/specular_fixture.gltf", "size": (1600, 900),
     "flags": ["-f", "30", "-W", "800", "-H", "450", "--no-auto-exposure", "-E", "1.0"]},
    # The third KHR material extension, and until now the only one of the three
    # with no stored reference at all -- sheen and specular above have had one
    # for specs. What that cost is on the record: 11.85 tagged the clearcoat
    # normal NORMAL, so it took BC5, and the shader read a .z that format does
    # not store, decoding it to -1 -- a coat normal pointing INTO the surface,
    # 27,364 px at PAE 226/255, past a fully green suite twice over.
    #
    # Flags match its two siblings exactly, including the AUTO-FRAMED camera.
    # Pinning one was considered and is the wrong trade here: auto-framing is a
    # function of the committed .gltf's own bounds, which fixture-gen holds to
    # byte equality, so it can only drift if it drifts for sheen and specular
    # too. The clearcoat gate group pins its own tighter framing instead,
    # because the weave wants pixels and this golden wants comparability.
    #
    # The fixture's generator says it is "meant to be viewed under an HDR
    # environment"; this renders under the app's fallback three-point rig,
    # since a golden needing -e could not be regenerated (see roughness_sweep).
    # The coat lobe is legible either way -- the defect above measured PAE
    # 226/255 under exactly this lighting.
    {"name": "clearcoat_fixture", "scene": "assets/clearcoat_fixture.gltf", "size": (1600, 900),
     "flags": ["-f", "30", "-W", "800", "-H", "450", "--no-auto-exposure", "-E", "1.0"]},
    # --no-sss pins the ANALYTIC falloff alone; its sibling below is the first
    # SSS-on golden and differs only in that flag.
    {"name": "skin_curvature_fixture", "scene": "assets/skin_curvature_fixture.cscn",
     "size": (2400, 1500),
     "flags": ["-f", "30", "-W", "1200", "-H", "750", "--no-auto-exposure", "-E", "1.0",
               "--no-bloom", "--no-sss", "--no-shadows"]},
    {"name": "skin_curvature_sss", "scene": "assets/skin_curvature_fixture.cscn",
     "size": (2400, 1500),
     "flags": ["-f", "30", "-W", "1200", "-H", "750", "--no-auto-exposure", "-E", "1.0",
               "--no-bloom", "--no-shadows"]},

    # --- transparency and post ------------------------------------------------
    # -E 0.08 rather than 1.0: the sphere fixture is emissive and would clip.
    {"name": "oit_sphere_moments", "scene": "assets/oit_sphere_fixture.gltf", "size": (1280, 800),
     "flags": ["-f", "30", "-W", "640", "-H", "400", "--no-auto-exposure", "-E", "0.08",
               "--oit-moments"]},
    # --no-bloom because a bloom halo would mask the bokeh the golden exists for.
    {"name": "dof_fixture", "scene": "assets/dof_fixture.gltf", "size": (1600, 1000),
     "flags": ["-f", "30", "-W", "800", "-H", "500", "--no-auto-exposure", "-E", "1.0",
               "--no-bloom", "--dof", "--dof-focus", "6", "--dof-range", "1.5",
               "--dof-max-coc", "8", "--dof-blades", "6", "--dof-rotation", "15"]},
    # The one golden where flare and chromatic aberration are LIVE; every other
    # one asserts they are dormant.
    {"name": "flare_fixture", "scene": "assets/flare_fixture.cscn", "size": (1600, 1000),
     "flags": ["-f", "30", "-W", "800", "-H", "500", "--no-auto-exposure", "-E", "1.0",
               "--flare", "0.15", "--chromatic-aberration", "12"]},
    # The scotopic shift, on the one frame it is legible in (spec 11.83). The
    # gate group measures the operator's PROPERTIES -- that it drains, shifts
    # blue, is per-pixel and is exposure-invariant -- and none of those is the
    # question "does this look like night". This is the only artifact that
    # answers it, and the only cross-build reference the look will ever have.
    #
    # The chart rather than a night scene, deliberately: the corpus's night
    # frames have almost nothing to drain (the sky is already blue, the terrain
    # near-achromatic), so a night golden would be a picture of the feature
    # barely acting. Here every rung and every hue is visible at once.
    #
    # Exposure comes from the fixture, which authors it -- see its generator's
    # header for why that value is load-bearing rather than framing.
    {"name": "purkinje_night", "scene": "assets/purkinje_fixture.cscn", "size": (1040, 800),
     "flags": ["-f", "30", "-W", "520", "-H", "400", "--no-auto-exposure", "--no-dither",
               "--no-vignette", "--no-bloom", "--no-ssao", "--purkinje"]},
]


def golden_path(name):
    return os.path.join(ASSETS, f"{name}_golden.png")


def _detect_fb_scale():
    """Framebuffer pixels per requested pixel, measured on this machine.

    Rendered rather than inferred: it is a property of the DISPLAY, not the OS, so
    a Mac on a 1x external monitor answers 1 and must. On failure it answers with
    the scale the corpus was baked at, which leaves every recipe's flags as
    authored and the run behaving exactly as it did before this existed.
    """
    global _FB_SCALE
    if _FB_SCALE is not None:
        return _FB_SCALE
    fd, probe = tempfile.mkstemp(suffix=".ppm")
    os.close(fd)
    r = subprocess.run([RENDER, "-m", os.path.join(ROOT, "assets", "parallax_fixture.gltf"),
                        "-x", "-f", "1", "-W", "100", "-H", "100", "-S", probe],
                       capture_output=True, text=True, cwd=ROOT)
    got = _dims(probe) if r.returncode == 0 else None
    _FB_SCALE = max(1, int(round(got[0] / 100.0))) if got else 2
    return _FB_SCALE


def _sized_flags(recipe):
    """The recipe's flags with -W/-H asking for the golden's own stored size.

    The recipe states the answer -- "size" is the framebuffer the golden holds --
    so the request is derived from it rather than from a global assumption about
    how much a display doubles.
    """
    scale = _detect_fb_scale()
    flags = list(recipe["flags"])
    want = {"-W": recipe["size"][0], "-H": recipe["size"][1]}
    for i in range(len(flags) - 1):
        if flags[i] in want:
            flags[i + 1] = str(max(1, want[flags[i]] // scale))
    return flags


def _render(recipe, out):
    """Render one recipe. Returns None on success, else the tail of its output."""
    cmd = ([RENDER, "-m", os.path.join(ROOT, recipe["scene"]), "-x"]
           + _sized_flags(recipe) + ["-S", out])
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0 or not os.path.exists(out):
        return (r.stdout + r.stderr)[-400:]
    return None


def _dims(path):
    r = subprocess.run(["magick", "identify", "-format", "%wx%h", path],
                       capture_output=True, text=True)
    try:
        w, h = (r.stderr or r.stdout).strip().split("x")
        return int(w), int(h)
    except ValueError:
        return None


def _env_mismatch(recipe, got):
    """Describe a framebuffer-size mismatch, or None when the size is right.

    The goldens are stored at the framebuffer size, which a HiDPI display makes
    double the requested -W/-H. On a 1x machine every render comes back halved
    and every comparison fails on dimensions -- which reads as nineteen
    regressions unless something says otherwise.
    """
    want = tuple(recipe["size"])
    if got == want:
        return None
    if got and got[0] and want[0] % got[0] == 0:
        return (f"rendered {got[0]}x{got[1]}, golden is {want[0]}x{want[1]} "
                f"({want[0] // got[0]}x scale): this display is not the one it was baked on")
    return f"rendered {got[0]}x{got[1]}, golden is {want[0]}x{want[1]}" if got else "unreadable"


def cmd_check(recipes, workdir):
    failures = []
    for r in recipes:
        name = r["name"]
        g = golden_path(name)
        if not os.path.exists(g):
            print(f"  {name:<24} SKIP  no golden committed")
            continue
        out = os.path.join(workdir, f"{name}.ppm")
        err = _render(r, out)
        if err:
            print(f"  {name:<24} ERROR render failed: {err.splitlines()[-1] if err else ''}")
            failures.append(name)
            continue
        env = _env_mismatch(r, _dims(out))
        if env:
            print(f"  {name:<24} ENV   {env}")
            failures.append(name)
            continue
        ae, _ = compare(out, g)
        print(f"  {name:<24} {'PASS' if ae == 0 else 'FAIL'}  {ae} px")
        if ae != 0:
            failures.append(name)
    return failures


def cmd_rebake(recipes, workdir):
    failures = []
    for r in recipes:
        name = r["name"]
        a = os.path.join(workdir, f"{name}_a.ppm")
        b = os.path.join(workdir, f"{name}_b.ppm")
        err = _render(r, a) or _render(r, b)
        if err:
            print(f"  {name:<24} ERROR render failed")
            failures.append(name)
            continue
        # Twice, and they must agree. A golden baked from a non-deterministic
        # render reproduces on the machine that baked it and nowhere else, and
        # nothing downstream can tell that from a real reference.
        drift, _ = compare(a, b)
        if drift != 0:
            print(f"  {name:<24} REFUSED two renders differ by {drift} px, not deterministic")
            failures.append(name)
            continue
        env = _env_mismatch(r, _dims(a))
        if env:
            print(f"  {name:<24} REFUSED {env}")
            failures.append(name)
            continue
        g = golden_path(name)
        if os.path.exists(g) and compare(a, g)[0] == 0:
            # Byte-identical: rewriting would be a re-encode and nothing else,
            # and these files are ~1 MB each.
            print(f"  {name:<24} same  unchanged, left alone")
            continue
        subprocess.run(["magick", a, g], check=True)
        print(f"  {name:<24} baked {g[len(ROOT) + 1:]}")
    return failures


def main():
    ap = argparse.ArgumentParser(description="check or re-bake the golden renders")
    ap.add_argument("--rebake", nargs="*", metavar="NAME",
                    help="re-render and overwrite (no names = all)")
    ap.add_argument("--only", metavar="NAME", help="check just this one")
    ap.add_argument("--list", action="store_true", help="print the recipe table")
    ap.add_argument("--keep", action="store_true", help="keep the rendered frames")
    ap.add_argument("--bin-dir", metavar="DIR",
                    help="where to find the render binary (default out/bin). "
                         "Changing this without --rebake compares across builds")
    args = ap.parse_args()

    if args.bin_dir:
        global BIN_DIR, RENDER
        BIN_DIR = os.path.abspath(args.bin_dir)
        RENDER = _bin("render")

    if args.list:
        for r in RECIPES:
            print(f"{r['name']:<24} {r['size'][0]}x{r['size'][1]}  {r['scene']}")
            print(f"{'':24} {' '.join(r['flags'])}")
        return 0

    if not os.path.exists(RENDER):
        sys.exit(f"{RENDER} not found -- run ./build.sh first")

    names = args.rebake if args.rebake else ([args.only] if args.only else None)
    recipes = RECIPES
    if names:
        known = {r["name"] for r in RECIPES}
        unknown = [n for n in names if n not in known]
        if unknown:
            sys.exit(f"unknown golden(s): {', '.join(unknown)}")
        recipes = [r for r in RECIPES if r["name"] in set(names)]

    workdir = tempfile.mkdtemp(prefix="cetra_goldens_")
    # A per-run cook cache (spec 11.99): a persistent one can move golden
    # pixels through a mip chain another build cooked. Same line gates.py
    # carries, same reason.
    os.environ["CETRA_COOK_DIR"] = os.path.join(workdir, "cook")
    try:
        if args.rebake is not None:
            print(f"re-baking {len(recipes)} golden(s):")
            failures = cmd_rebake(recipes, workdir)
        else:
            print(f"checking {len(recipes)} golden(s):")
            failures = cmd_check(recipes, workdir)
    finally:
        if args.keep:
            print(f"\nframes in {workdir}")
        else:
            shutil.rmtree(workdir, ignore_errors=True)

    if failures:
        print(f"\n{len(failures)} golden(s) failed: {', '.join(failures)}")
        return 1
    print(f"\nall {len(recipes)} goldens passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
