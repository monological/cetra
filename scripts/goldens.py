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
RENDER = os.path.join(ROOT, "out", "bin", "render")
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
    {"name": "froxel_fog", "scene": "assets/contact_fixture.gltf", "size": (1280, 800),
     "flags": ["--fog", "--no-aerial", "-f", "30", "--no-auto-exposure", "-E", "1.0",
               "-W", "640", "-H", "400"]},

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

    # --- global illumination and punctual shadows -----------------------------
    {"name": "cornell_box", "scene": "assets/cornell_box.gltf", "size": (1600, 1200),
     "flags": ["--gi-volume", "-f", "30", "--no-auto-exposure", "-E", "1.0",
               "-W", "800", "-H", "600"]},
    {"name": "cornell_leak", "scene": "assets/cornell_leak.gltf", "size": (1600, 1200),
     "flags": ["-f", "30", "--no-auto-exposure", "-E", "1.0", "-W", "800", "-H", "600"]},
    {"name": "cornell_point", "scene": "assets/cornell_point.gltf", "size": (1600, 1200),
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
]


def golden_path(name):
    return os.path.join(ASSETS, f"{name}_golden.png")


def _render(recipe, out):
    """Render one recipe. Returns None on success, else the tail of its output."""
    cmd = ([RENDER, "-m", os.path.join(ROOT, recipe["scene"]), "-x"]
           + recipe["flags"] + ["-S", out])
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
    args = ap.parse_args()

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
