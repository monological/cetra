"""The pcss group: a short occluder keeps its soft shadow (spec 13.58)."""

import json
import math
import os

import numpy as np

from gatelib.harness import RENDER, _run, asset
from gatelib.images import _SRGB_TO_LINEAR, _ppm_array
from gatelib.lighting import _crossing
from gatelib.scenes import _projector, _world_pixel_box


# Short occluders under soft shadows (spec 13.58), on gen_short_occluder_fixture.py's slabs: a
# shadow is said to exist by the light lost in a box behind each slab against a box of open floor
# beside it, both read off the floor plane. The suns stand at azimuth 0, so every shadow falls
# toward -z by its slab's height over the tangent of the sun's elevation.
SHORT_ELEVATION, SHORT_GRAZING = 35.0, 8.0  # the fixture's two suns, in degrees
SHORT_SIZE = (800, 600)
# Each view looks down, tilted off the vertical so the projection has a horizontal axis: +x right
# and -z up in the frame.
SHORT_VIEWS = {
    "ladder": {"eye": (0.0, 4.5, 0.5), "target": (0.0, 0.0, -0.3), "fovy_deg": 40.0},
    "pillar": {"eye": (0.0, 3.0, -3.5), "target": (0.0, 0.0, -4.0), "fovy_deg": 40.0},
    "grazing": {"eye": (0.0, 4.5, 1.8), "target": (0.0, 0.0, 1.0), "fovy_deg": 40.0},
}
PCSS_SHORT_MIN = 0.8  # a slab's soft shadow keeps at least this share of its PCF one
PCSS_SHADOW_MIN = 0.2  # a shadow at all: the light a hard shadow takes from the box
PCSS_SELF_MIN = 0.99  # open floor under the grazing sun, against no shadows at all
PCSS_SPHERE_MIN = 0.98  # the sphere's lit side, the same
PCSS_PENUMBRA_MASTER = 0.0303  # the pillar's penumbra in metres before 13.58, measured
CATCHER_SHORT_MIN = 0.8  # a slab's shadow on the catcher, against the tallest's


def _short_boxes():
    """The fixture's pieces by name, each as its world box (lo, hi), from the glTF's nodes."""
    with open(asset("short_occluder_fixture.gltf")) as f:
        nodes = json.load(f)["nodes"]
    return {n["name"]: ([t - s / 2 for t, s in zip(n["translation"], n["scale"])],
                        [t + s / 2 for t, s in zip(n["translation"], n["scale"])])
            for n in nodes if "translation" in n}


def _short_render(workdir, tag, scene, view, exposure, extra=()):
    """The frame `scene` renders from SHORT_VIEWS[view], as linear light, with its projection."""
    out = os.path.join(workdir, f"short_{tag}.ppm")
    cam = SHORT_VIEWS[view]
    r = _run([RENDER, "-m", asset(scene), "-x", "-f", "30", "-W", str(SHORT_SIZE[0]),
              "-H", str(SHORT_SIZE[1]), "--no-auto-exposure", "-E", f"{exposure:g}",
              "-F", f"{cam['fovy_deg']:g}", "--cam-eye", ",".join(f"{v:g}" for v in cam["eye"]),
              "--cam-target", ",".join(f"{v:g}" for v in cam["target"]), "-S", out, *extra],
             capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None, None
    img = np.array(_SRGB_TO_LINEAR)[_ppm_array(out).astype(int)].mean(axis=2)
    return img, _projector(cam, img.shape[1], img.shape[0])


def _floor_mean(img, project, lo, hi):
    """Mean linear light over a rectangle of floor (y = 0) from (x, z) `lo` to `hi`."""
    x0, y0, x1, y1 = _world_pixel_box(project, (lo[0], 0.0, lo[1]), (hi[0], 0.0, hi[1]), 1)
    return float(img[y0:y1, x0:x1].mean())


def _slab_darkness(img, project, slab, elevation):
    """The share of light lost in the middle of a slab's shadow, against open floor beside it."""
    lo, hi = slab
    length = (hi[1] - lo[1]) / math.tan(math.radians(elevation))
    cx = (lo[0] + hi[0]) / 2
    z0, z1 = lo[2] - 0.75 * length, lo[2] - 0.25 * length
    shadow = _floor_mean(img, project, (cx - 0.12, z0), (cx + 0.12, z1))
    open_ = _floor_mean(img, project, (hi[0] + 0.07, z0), (hi[0] + 0.23, z1))
    return 1.0 - shadow / open_


def run_pcss_gate(workdir):
    """A short occluder keeps its soft shadow (spec 13.58).

    PCSS asks first which texels nearer the light than a receiver block it, and a pixel with
    none is lit whatever the filter after would say. Until 13.58 "nearer" was 1% of the distance
    from the cascade's light eye, which the cascade's padding sets at about 4% of the scene's
    radius: every slab here under that height lost its soft shadow, at any scale.

      pcss-short     each slab, 0.5% to 10% of the scene's radius tall, keeps at least
                     PCSS_SHORT_MIN of the shadow PCF gives it under the 35 degree sun
      pcss-self      under the 8 degree sun, open floor and the sphere's lit side read as with
                     no shadows at all: a receiver does not block itself
      pcss-penumbra  the pillar's penumbra 1.9 m from its base, 10% to 90% across its side edge,
                     is no narrower than master's: a margin too small to keep the receiver's
                     own texels out of the blocker average collapses it
      catcher-short  over the sky's shadow catcher, each slab's shadow is at least
                     CATCHER_SHORT_MIN of the tallest slab's
    """
    failures = []
    pieces = _short_boxes()
    slabs = [pieces[f"slab_{i}"] for i in range(5)]

    soft, project = _short_render(workdir, "soft", "short_occluder_fixture.cscn", "ladder", 1.0)
    hard, _ = _short_render(workdir, "hard", "short_occluder_fixture.cscn", "ladder", 1.0,
                            ["--no-pcss"])
    if soft is None or hard is None:
        ok, detail = False, "a render failed"
    else:
        pairs = [(_slab_darkness(soft, project, s, SHORT_ELEVATION),
                  _slab_darkness(hard, project, s, SHORT_ELEVATION)) for s in slabs]
        ok = all(h >= PCSS_SHADOW_MIN and s >= PCSS_SHORT_MIN * h for s, h in pairs)
        detail = ("soft / PCF darkness by slab: " + ", ".join(f"{s:.3f}/{h:.3f}" for s, h in pairs)
                  + f" (want each soft >= {PCSS_SHORT_MIN} of its PCF, PCF >= {PCSS_SHADOW_MIN})")
    print(f"  pcss-short     {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("pcss-short")

    soft, project = _short_render(workdir, "graze", "short_occluder_grazing.cscn", "grazing", 1.0)
    bare, _ = _short_render(workdir, "graze_bare", "short_occluder_grazing.cscn", "grazing", 1.0,
                            ["--no-shadows"])
    if soft is None or bare is None:
        ok, detail = False, "a render failed"
    else:
        floor = (_floor_mean(soft, project, (0.5, 0.6), (1.5, 1.2))
                 / _floor_mean(bare, project, (0.5, 0.6), (1.5, 1.2)))
        lo, hi = pieces["sphere"]
        centre = [(a + b) / 2 for a, b in zip(lo, hi)]
        radius = (hi[0] - lo[0]) / 2
        n = (0.0, 0.8, 0.6)  # up and toward the sun: the lit side, seen from above
        x, y = project(tuple(c + radius * d for c, d in zip(centre, n)))
        x, y = int(x), int(y)
        sphere = float(soft[y - 4:y + 4, x - 4:x + 4].mean() / bare[y - 4:y + 4, x - 4:x + 4].mean())
        ok = floor >= PCSS_SELF_MIN and sphere >= PCSS_SPHERE_MIN
        detail = (f"open floor {floor:.4f} (want >= {PCSS_SELF_MIN}), the sphere's lit side "
                  f"{sphere:.4f} (want >= {PCSS_SPHERE_MIN}) of no shadows")
    print(f"  pcss-self      {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("pcss-self")

    widths = {}
    for tag, extra in (("soft", ()), ("hard", ("--no-pcss",))):
        img, project = _short_render(workdir, f"pillar_{tag}", "short_occluder_fixture.cscn",
                                     "pillar", 1.0, extra)
        if img is None:
            continue
        xs = [0.005 * i for i in range(121)]
        vals = []
        for x in xs:
            px, py = project((x, 0.0, -4.0))
            vals.append(float(img[int(py) - 3:int(py) + 4, int(px)].mean()))
        umbra, lit = sum(vals[:6]) / 6, sum(vals[-10:]) / 10
        widths[tag] = (_crossing(xs, vals, umbra, lit, 0.9)
                       - _crossing(xs, vals, umbra, lit, 0.1))
    if len(widths) != 2:
        ok, detail = False, "a render failed"
    else:
        ok = widths["soft"] >= 0.9 * PCSS_PENUMBRA_MASTER
        detail = (f"soft {widths['soft'] * 100:.2f} cm against PCF's {widths['hard'] * 100:.2f} "
                  f"(want >= 0.9 of master's {PCSS_PENUMBRA_MASTER * 100:.2f})")
    print(f"  pcss-penumbra  {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("pcss-penumbra")

    img, project = _short_render(workdir, "catcher", "short_occluder_catcher.cscn", "ladder", 1.0)
    if img is None:
        ok, detail = False, "a render failed"
    else:
        dark = [_slab_darkness(img, project, s, SHORT_ELEVATION) for s in slabs]
        ok = dark[-1] >= PCSS_SHADOW_MIN and all(d >= CATCHER_SHORT_MIN * dark[-1] for d in dark)
        detail = ("darkness by slab: " + ", ".join(f"{d:.3f}" for d in dark)
                  + f" (want each >= {CATCHER_SHORT_MIN} of the tallest's, which >= "
                  f"{PCSS_SHADOW_MIN})")
    print(f"  catcher-short  {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("catcher-short")
    return failures
