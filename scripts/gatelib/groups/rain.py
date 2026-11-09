"""The rainfall and fog-miss groups:

  rainfall            rain: the physics it derives and the state it accumulates (spec 13.9)
  fog-miss            fog cells without history while the camera turns (spec 13.45)
"""

import math
import re
import os
import sys

import numpy as np

from gatelib.harness import (
    NO_HALOS, RENDER, ROOT, _probe_render, _probe_rows, _run, _shared_constant, asset, render,
)
from gatelib.images import _DISPLAY_TO_LINEAR, _box_luma_dense, _ppm_array, _read_ppm, compare
from gatelib.probes import _PBR_VARIANT, _pbr_feature_bits
from gatelib.scenes import WATER_BOUNDS_AWAY, WATER_DRY_OVER, _cscn_camera, _projector, cscn_copy


RAIN_FIXTURE = "rain_fixture.cscn"
RAIN_TOL = 1e-5        # relative, float32 physics against its double twin
RAIN_PACE_TOL = 1e-4   # absolute, one schedule sliced at 60 and at 30 fps
RAIN_TICK_FRAMES = 60
# Points on rain_fixture and whether rain reaches them. The roof spans x -4..4, z -7..-1
# at 3 m, and the fixture's wind (1.5 m/s toward -z) against a 10 mm/h median drop's
# 5.3 m/s fall carries the dry patch 0.84 m toward the wall. The last two points of the
# first group sit half a metre either side of the roof's footprint, where a map rendered
# straight down gives the OPPOSITE answer -- they are what holds the direction.
# The streak arms read the rain's LIGHT, which is only separable from the scene where there
# is nothing behind it: the fixture's upper frame, above the wall, is bare clear colour. A
# plain point lamp -- no profile, so the geometry alone decides its light -- sits 8 m in front
# of the camera for the backlit case and 8 m behind it for the front-lit one, the same
# distance either side of the rain round the camera, and a lamp of intensity 0 is the dark.
RAIN_SKY_BOX = (0.05, 0.02, 0.95, 0.28)
RAIN_LAMP_CD = 60.0
RAIN_LAMP_BACK = [0.0, 3.5, 3.0]
RAIN_LAMP_FRONT = [0.0, 3.5, 19.0]
RAIN_LINEAR = ["--tonemap", "linear", *NO_HALOS, "--no-dither", "--no-vignette", "-E", "1"]
# The glint arm lights the rain from the SIDE, level with it and beside the camera. Backlit,
# the few drops on the lamp's line of sight clip long before the rest leave the 8-bit floor,
# and either end biases a sum: clipping loses glints and rounding loses spread light, so the
# two readings disagree by the method rather than by the pattern. Side-lit rain has no such
# peak, so one exposure can hold every lit pixel well inside the code range.
RAIN_LAMP_SIDE = [9.0, 3.0, 11.0]
RAIN_GLINT_CD = 400.0
# Refraction throws 85% of a drop's light into a g = 0.8 lobe, which puts about two decades
# between looking into the light and looking away from it. A phase evaluated with its angle
# the wrong way round lands below 1.
RAIN_LOBE_MIN = 10.0
# Glints move light along a streak and add none, but a streak need not hold whole cycles of
# its pattern, so the frame's total agrees only on average over its streaks.
RAIN_GLINT_TOL = 0.05
# The wet-surface arm's points, (name, world point): two under the roof, more than a metre
# inside the dry patch's soft edge, and two in the open, one on each ground half.
RAIN_WET_POINTS = [("covered porous", (-2.0, 0.0, -4.5)), ("covered sealed", (2.0, 0.0, -4.5)),
                   ("open porous", (-5.0, 0.0, 3.0)), ("open sealed", (5.0, 0.0, 3.0))]
# A fully porous, fully wet surface loses half its albedo and then takes the hue power
# (0.35^0.55 = 0.56 on this grey), about 0.28 of its dry diffuse in all.
RAIN_WET_POROUS_MAX = 0.5
RAIN_COVER_POINTS = [
    ((0.0, 0.0, -4.0), False), ((-2.0, 0.0, -4.0), False), ((2.0, 0.0, -4.0), False),
    ((0.0, 0.0, 4.0), True), ((-6.0, 0.0, 4.0), True), ((6.0, 0.0, 4.0), True),
    ((0.0, 3.2, -4.0), True), ((0.0, 2.0, -11.25), True),
    ((0.0, 0.0, -1.5), True), ((0.0, 0.0, -7.5), False),
]
RAIN_WATER_FIXTURE = "rain_water_fixture.cscn"
# The wall the wind drives the rain at, 2 m up: exposed, and vertical, so it sheds. A ROW,
# because the puddle field varies only in x across a wall facing z, and one point can sit
# where the field would hold no water even if the wall were level.
RAIN_WALL_POINTS = [(x, 2.0, -11.25) for x in (-4.0, -3.0, -2.0, -1.0, 0.0, 1.0, 2.0, 3.0, 4.0)]
# The ripple arm looks OUT from under the roof, 1.2 m up, with rings a metre across. The
# covered water is then the nearest in frame, where a ring could not hide; from the fixture's
# own camera that water is 13 m out, where rings have already faded into their own footprint
# and a cover that did nothing passes.
RAIN_UNDER_ROOF_CAMERA = {"eye": [0.0, 1.2, -5.5], "target": [0.0, 0.0, 1.0], "fov": 70}
RAIN_RING_SIZE = 1.0
# A patch under the roof, x and z extents, clear of the dry patch's softened edge: the roof's
# footprint carried 0.84 m toward the wall by the wind, less the cover's spread from 3 m up.
# An arm counts every pixel of its image, because droplets are sparse, and a few points a box
# wide let a splash under the roof land between them.
RAIN_COVERED_PATCH = ((-1.5, 1.5), (-4.0, -2.8))
# Fewer pixels than this in the patch's image and the arm could not see it at all.
RAIN_COVERED_MIN_PX = 20000
# The splash arm's own knobs, from the same camera: a square wide enough that the open ground
# beyond the roof is well inside it rather than in its fade, and droplets big enough to read
# five metres off.
RAIN_SPLASH_ARM = {"splashRadius": 12.0, "splashSize": 3.0}
# Splash droplets are a few pixels each and fly for a fraction of a slot's life, so their share
# of a frame is small; this floor is a tenth of what the arm reads, and zero is the failure.
RAIN_SPLASH_MIN = 0.0005
# The fraction of a frame a feature has to move for it to count as there at all.
RAIN_FEATURE_MIN = 0.01
# The CPU cover query's points, one under the roof and one in the open, and enough frames for
# its readback ring to have answered (three slots) several times over.
RAIN_ASK_POINTS = [(0.0, 0.05, -4.5), (0.0, 0.05, 4.0)]
RAIN_ASK_FRAMES = 8
# Values nothing defaults to, so a restore that did nothing cannot pass by landing on them; the
# .cscn key each is authored under, against the probe's state-row name it reads back as.
RAIN_CONFIG_TUNED = {"rate": 3.3, "mist": 2.5, "streakRadius": 3.1}
RAIN_CONFIG_READ = {"rate": "rate", "mist": "mist", "streakRadius": "streak_radius"}
# Everything the rain draws in the air switched off, so a pair of frames differs only in what
# lands on the surfaces.
RAIN_SURFACES_ONLY = {"streakCount": 0, "splashCount": 0, "mist": 0.0}
# The SSR arm's regions, from the fixture's own camera: open wet ground in front of the roof
# (ground x and z extents), dry ground under it, and the wall the wet ground runs up to, from
# a few centimetres above its base -- where a wet pair's coverage folded across the silhouette
# would darken it.
RAIN_SSR_OPEN = ((-3.0, 3.0), (1.0, 4.0))
RAIN_SSR_COVERED = ((-2.0, 2.0), (-5.5, -3.0))
RAIN_SSR_WALL = [(x, y, -11.25) for x in (-3.0, 3.0) for y in (0.05, 1.5)]
# The fixture's own camera sees these regions smaller than the under-roof camera sees its patch.
RAIN_SSR_MIN_PX = 1000
# The SSR arms under TAA put the fixture under a procedural sky: its own ambient-only environment
# leaves wet ground too little of the environment's reflection to replace for a mismatch to show.
RAIN_SKY_TAA = ["--sky", "--taa", "--headless-jitter"]
RAIN_TAAU = ["--render-scale", "0.5"]
RAIN_NEG_FRAMES = 30
# The wet replacement at the leading edge of a turn (spec 13.46), on the shared TURN, under the sky
# for the reason RAIN_SKY_TAA gives, and with a reflection probe. Wet ground reflects within its
# own column, so at the frame's edge the trace's hits fade, and the probe is what takes their
# weight, as silent's do: with none the pair there is nearly empty, and wet ground at the edge
# replaced too little for a fallback to be told from a reflection. Nothing that reads the whole
# frame may run -- the meter, bloom -- or SSR's effect elsewhere reaches the edge's pixels and none
# can equal its --no-ssr twin.
RAIN_EDGE_ARGS = ["--sky", "--probe", "--no-auto-exposure", "-E", "1.0", "--no-bloom",
                  "--no-dither"]
RAIN_EDGE_MIN_PX = 2000  # wet pixels at the leading edge: measured 4130
RAIN_EDGE_BARE_MAX = 0.10  # share of them left at their --no-ssr value: 98.0% before, 1.8% after
# A medium lobe well off the streaks' 0.8, so a frame that read the wrong one would show it.
RAIN_MIST_G = 0.3
# A scene wind that gusts fast enough for two probe runs a few frames apart to sit at different
# points of its envelope, toward the wall as the fixture's own rain wind blows.
RAIN_SCENE_WIND = {"direction": [0.0, 0.0, -1.0], "airSpeed": 3.0, "gustAmount": 0.5,
                   "gustFrequency": 2.0}
RAIN_GUST_FRAMES = 40
# Deeper than a splash crown rises off the bed (a 6 mm drop's droplets top out near 0.2 m).
RAIN_DEEP_WATER = 0.5
# The glazed twin's panes, each an inscribed quad on the face a camera sees, inset from its
# edges: the open pane's street face, the covered pane's, and the UNDERSIDE of the flat glass
# canopy, seen from below. The canopy shelters the point a hand under it, as any roof does, so
# the underside's own cover is none and only the side the rain strikes beads it. A wall pane
# cannot show that: the rain meets it nearly edge on and the cover map's slope bias stores it
# deeper than the point behind it, which therefore reads as open sky either way. The twin's own
# camera sees the two that stand; this one sees the canopy from under it.
RAIN_GLASS_FIXTURE = "rain_glass_fixture.cscn"
RAIN_GLASS_BELOW_CAMERA = {"eye": [0.0, 0.8, 3.2], "target": [0.0, 3.0, -2.0], "fov": 60}
RAIN_GLASS_OPEN = [(x, y, 4.003) for x in (1.2, 2.8) for y in (0.45, 1.65)]
RAIN_GLASS_COVERED = [(x, y, -3.997) for x in (-2.8, -1.9) for y in (0.45, 1.65)]
RAIN_GLASS_CANOPY = [(x, 2.397, z) for x in (-0.8, 0.8) for z in (0.2, 1.8)]
# Fewer pixels than this in a pane's or a drip column's image and the arm could not see it.
RAIN_REGION_MIN_PX = 500
# Seconds since the rain stopped: soon enough that the panes are still beaded and the edges
# still dripping, and long enough that the film is under the floor rain_active() keeps and
# nothing of the rain is left.
RAIN_DAMP_S = 20.0
RAIN_DRIED_S = 5000.0
# Three drip lines unlike in every way that matters to their share of the slots -- a point at
# 2 m, a 2 m line at 5 m dripping fast, and a 6 m line at 3 m dripping slowly onto ground 1 m
# up -- and few enough slots that the shares are small integers the twin must hit exactly.
RAIN_DRIP_SHARE_LINES = [
    {"from": [0.0, 2.0, 0.0], "rate": 3.0},
    {"from": [-1.0, 5.0, 2.0], "to": [1.0, 5.0, 2.0], "rate": 10.0},
    {"from": [-3.0, 3.0, 4.0], "to": [3.0, 3.0, 4.0], "rate": 1.0, "ground": 1.0},
]
RAIN_DRIP_SHARE_COUNT = 100
# A line along the fixture roof's front edge, dripping hard and drawn bright: a single drop at
# its physical opacity is a few hundredths of the pixels it crosses, which an 8-bit frame
# rounds away often enough to leave an arm reading noise.
RAIN_DRIP_LINE = {"from": [-3.5, 3.0, -1.0], "to": [3.5, 3.0, -1.0], "rate": 400.0}
RAIN_DRIP_ARM = {"dripCount": 2048, "dripBrightness": 50.0}
# The column under that line, half a metre in from its ends and 0.3 m clear of the edge and
# the ground.
RAIN_DRIP_COLUMN = [(x, y, RAIN_DRIP_LINE["from"][2])
                    for x in (RAIN_DRIP_LINE["from"][0] + 0.5, RAIN_DRIP_LINE["to"][0] - 0.5)
                    for y in (0.3, RAIN_DRIP_LINE["from"][1] - 0.3)]
# The relief twin (spec 13.12): one ground whose height map is trenches and plateaus in bands
# across z, 2 m each -- trenches centred on z = 2 + 4k, plateaus on z = 4k. These are the open
# ground's bands, a metre in from either edge so the map's filtered step stays out of them.
RAIN_RELIEF_FIXTURE = "rain_relief_fixture.cscn"
RAIN_RELIEF_TRENCHES = [((-3.0, 3.0), (1.5, 2.5)), ((-3.0, 3.0), (5.5, 6.5))]
RAIN_RELIEF_PLATEAUS = [((-3.0, 3.0), (-0.5, 0.5)), ((-3.0, 3.0), (3.5, 4.5))]
# Fewer pixels than this read in either kind of band and the arm could not tell them apart.
RAIN_RELIEF_MIN_PX = 20000
# How much of a band the relief must fill or drain: the map moves the ground a quarter of the
# noise's range either way, which takes a band at the fixture's level from about half puddled
# to nearly all or nearly none.
RAIN_RELIEF_TRENCH_MIN = 0.8
RAIN_RELIEF_PLATEAU_MAX = 0.2
# The two kinds of band at relief 0, which differ only by the noise each happens to hold.
RAIN_RELIEF_AGREE = 0.15
RAIN_RELIEF_FLAGS = ["--render-mode", "8", "--no-parallax"]


def _rain_twin(rate):
    """The physics rain.c derives, in double: Marshall-Palmer and Atlas 1973.

    The splash flux is integrated NUMERICALLY here -- density times fall speed over the drops
    that splash -- where the C uses its closed form, so a term dropped from one cannot be
    dropped from both."""
    lam = 4.1 * rate ** -0.21
    d0 = 3.672 / lam
    return {"lambda": lam,
            "beta": math.pi * 8000.0 / lam ** 3 * 1e-6,
            "density": 8000.0 / lam * math.exp(-lam * 0.5),
            "d0": d0,
            "v0": _rain_velocity(d0),
            "splash": _simpson(lambda d: 8000.0 * math.exp(-lam * d) * _rain_velocity(d),
                               _rain_constant("RAIN_SPLASH_MIN_MM"), 40.0, 4000)}


def _rain_constant(name):
    return _shared_constant("rain_constants.glsl", name)


def _simpson(f, a, b, n):
    h = (b - a) / n
    return h / 3.0 * (f(a) + f(b) + sum((4 if i % 2 else 2) * f(a + i * h) for i in range(1, n)))


def _rain_velocity(d_mm):
    return max(0.0, 9.65 - 10.3 * math.exp(-0.6 * d_mm))


def _drip_fall_time(height):
    """Seconds a drip takes to fall `height` metres from rest under linear drag, in double: the
    speed vt (1 - exp(-t g / vt)) integrated NUMERICALLY for the distance, and the time found by
    bisection -- where the C solves the closed-form distance by Newton, so a term dropped from
    one cannot be dropped from both. Falling for h / vt + vt / g covers any height, since the
    drop is never more than vt / g seconds' worth of terminal speed behind."""
    vt = _rain_velocity(_rain_constant("RAIN_DRIP_MM"))
    if height <= 0.0:
        return 0.0
    g = _rain_constant("RAIN_GRAVITY")

    def fallen(t):
        return _simpson(lambda s: vt * (1.0 - math.exp(-s * g / vt)), 0.0, t, 200)

    lo, hi = 0.0, height / vt + vt / g
    for _ in range(60):
        mid = 0.5 * (lo + hi)
        lo, hi = (mid, hi) if fallen(mid) < height else (lo, mid)
    return 0.5 * (lo + hi)


def _gust_mean():
    """The scene wind's gust envelope averaged over a cycle, integrated numerically: wind.glsl's
    mix(1 - a, 1, s^3) for s = (1 + sin) / 2, with a = RAIN_SCENE_WIND's gust amount."""
    a = RAIN_SCENE_WIND["gustAmount"]
    return _simpson(lambda th: 1.0 - a + a * ((1.0 + math.sin(th)) / 2.0) ** 3, 0.0,
                    2.0 * math.pi, 64) / (2.0 * math.pi)


def _glsl_const(name, include):
    """A `const float` from one of the shader includes, read rather than copied."""
    with open(os.path.join(ROOT, "cetra", "shaders", "include", include)) as f:
        m = re.search(rf"const float {name} = ([0-9.]+);", f.read())
    return float(m.group(1))


def _drip_shares(lines, count):
    """The slots each drip line gets of `count`: one each when there are enough, the rest in
    proportion to its rate times its cycle onto its own ground, by largest remainder, ties to
    the earlier line."""
    weights = []
    for line in lines:
        top = max(line["from"][1], line.get("to", line["from"])[1])
        period = max(_rain_constant("RAIN_DRIP_PERIOD_MIN"),
                     _rain_constant("RAIN_DRIP_HANG_MIN") +
                     _drip_fall_time(top - line.get("ground", 0.0)) +
                     _rain_constant("RAIN_SPLASH_LIFE"))
        weights.append(line.get("rate", 1.0) * period)
    live = [w > 0.0 for w in weights]
    base = 1 if count >= sum(live) else 0
    left = count - base * sum(live)
    total = sum(weights)
    out, rem = [], []
    for w, alive in zip(weights, live):
        quota = left * w / total if alive else 0.0
        out.append(base + math.floor(quota) if alive else 0)
        rem.append(quota - math.floor(quota) if alive else -2.0)
    while sum(out) < count:
        best = max(range(len(out)), key=lambda i: (rem[i], -i))
        out[best] += 1
        rem[best] = -1.0
    return out


def _rain_rows(scene, extra=None, frames=2):
    """The --rain-probe rows by kind, and the run's whole output for what else it logged."""
    rows, text = _probe_render(scene, "--rain-probe", "rain-probe", frames=frames, extra=extra)
    out = {}
    for rec in rows:
        kind = rec.pop("kind", None)
        vals = {k: float(v) for k, v in rec.items()}
        out.setdefault(kind, []).append(vals)
    return out, text


def _inscribed_rect(project, quad):
    """The screen rectangle inscribed in the image of a world quadrilateral, (x0, y0, x1, y1) with
    the far bounds exclusive. Inscribed, so every pixel in it is of the quad: a ground patch seen
    in perspective is a trapezoid whose far edge is the narrower, and the rectangle takes that
    width. The inner two of the four projected x and of the four projected y bound it, whichever
    way the camera maps world axes to the screen."""
    at = [project(p) for p in quad]
    xs, ys = sorted(p[0] for p in at), sorted(p[1] for p in at)
    return int(math.ceil(xs[1])), int(math.ceil(ys[1])), int(xs[2]), int(ys[2])


def _moved_in_rect(pa, pb, w, h, x0, y0, x1, y1):
    """(pixels differing, pixels) between two frames' pixels over the rectangle [x0, x1) x
    [y0, y1), clipped to the frame."""
    moved = total = 0
    for py in range(max(0, y0), min(h, y1)):
        for px in range(max(0, x0), min(w, x1)):
            i = 3 * (py * w + px)
            moved += pa[i:i + 3] != pb[i:i + 3]
            total += 1
    return moved, total


def _moved_fraction(a_path, b_path):
    """The fraction of the frame two renders differ in; 0 when either is missing."""
    if not (a_path and b_path):
        return 0.0
    w, h, _ = _read_ppm(a_path)
    ae, _ = compare(a_path, b_path)
    return ae / (w * h)


def _cscn_view(cam):
    """A .cscn camera block in the shape _projector takes."""
    return {"eye": tuple(cam["eye"]), "target": tuple(cam["target"]),
            "fovy_deg": float(cam["fov"])}


def _rain_ssr_edge(workdir):
    """rain-ssr-edge's (ok, detail), from the shared TURN and its held twin, each with SSR and
    without. Exact, so it needs no contrast: a wet pixel that fell back to the environment's share
    alone is exactly what --no-ssr shows, and one that took a reflection is not."""
    runs, failed = _turn_runs(workdir, "rain_edge", RAIN_EDGE_ARGS, (
        ("turn", [], False), ("turn_bare", ["--no-ssr"], False),
        ("held", [], True), ("held_bare", ["--no-ssr"], True)))
    if failed is not None:
        return False, f"a render failed: {failed}"
    (turned, _), (turned_bare, _), (held, _), (held_bare, _) = runs
    edge = np.array([grow is None for grow in _turn_columns(turned.shape[1])])
    wet = (held != held_bare).any(axis=2) & edge
    bare = wet & (turned == turned_bare).all(axis=2)
    n_wet, n_bare = int(wet.sum()), int(bare.sum())
    ok = n_wet >= RAIN_EDGE_MIN_PX and n_bare <= RAIN_EDGE_BARE_MAX * n_wet
    return ok, (f"of the {n_wet} px in the {int(edge.sum())} leading columns that SSR changes "
                f"when held (want >= {RAIN_EDGE_MIN_PX}), the turn left {n_bare} "
                f"({n_bare / max(n_wet, 1):.1%}) at their --no-ssr value (want <= "
                f"{RAIN_EDGE_BARE_MAX:.0%})")


def run_rain_gate(workdir):
    """Rain (spec 13.9): the physics it derives and the state it accumulates.

      rain-physics  Marshall-Palmer's slope, the extinction pi N0 / Lambda^3, the drop
                    density above 0.5 mm, the median volume diameter 3.672 / Lambda,
                    Atlas's terminal velocity and the flux of drops big enough to splash,
                    at five rates and four diameters, against this file's twin -- which
                    integrates the flux numerically where the C has a closed form. Heavier
                    rain must also mean bigger, faster drops, more splashes and a dimmer
                    sky -- the ladder has to be monotone, since a twin that shared a sign
                    error with the C would pass the first half.
      rain-wetting  one schedule -- ten seconds of 10 mm/h, thirty of none -- sliced at
                    60 and at 30 fps lands on the same state, and that state is the
                    closed form: a film with a time constant shortened by the rate, and
                    a puddle level heading for coverage x (1 - exp(-R / R_ref)).
      rain-tick     the fixture opened DRY rather than settled, after sixty headless
                    frames, reads the same closed form at one second -- so the engine
                    advances the state once a frame on the frame's own clock, and a
                    settled scene lands exactly on the rate's steady state.
      rain-exposure the occlusion map answers cover at ten points: dry under the roof on
                    both ground halves, wet in the open, on the roof and on the wall the
                    wind drives the rain at, and -- at the two points either side of the
                    roof's footprint -- displaced along the wind, which a map rendered
                    straight down gets backwards. A point 80 m out is off the map.
      rain-ask      the CPU cover query -- a texel read back through a ring at fixed
                    latency, which is what a listener under a roof asks -- asked every frame
                    at a point under the roof and at one in the open, answers as the exact
                    readback of the same point does.
      rain-config   a variant's rain settings dumped by the session snapshot and restored
                    over the plain fixture read back as the variant's -- a row that writes
                    and does not apply fails.
      rain-tenant   the same ten answers and the same stored depths with the lamp CASTING,
                    which puts its six cube faces ahead of the rain in the punctual array,
                    and with shadows switched off, which leaves the rain alone in it. The
                    layer moves (6, then 0); the cover does not.
      rain-off      a rain block at rate 0 is 0 px from the same scene with none: nothing
                    falls, nothing is wet, and no layer is rendered.
      rain-determinism  two runs of the fixture are 0 px apart. The drops are functions of
                    their index and the frame's clock, so nothing may differ between runs.
      rain-lobe     the light a lamp puts into the rain, read against bare sky at sheen 0:
                    backlit, with the lamp beyond the rain, it is more than RAIN_LOBE_MIN
                    times what the same lamp gives from behind the camera -- and nonzero
                    there, since a fraction of every drop's light goes everywhere.
      rain-glint    a side-lit lamp's light in the rain with every lit streak's light in
                    glints and with none, within RAIN_GLINT_TOL and with no pixel clipped:
                    the glints redistribute it, and a pattern that added light would read as
                    rain that brightened when it sparkled.
      rain-wet      the soaked fixture against itself dry, surfaces only: under the roof the
                    ground is EXACTLY as it was on both halves, and in the open the porous
                    half falls below RAIN_WET_POROUS_MAX of its dry brightness while the
                    sealed half, which only takes a film, stays well above the porous one.
      rain-puddles  puddles against none on the same soaked surfaces: they move more than
                    RAIN_FEATURE_MIN of the frame, and not one pixel round the ground under
                    the roof or on the wall -- standing water needs rain AND level ground.
      rain-splashes from under the roof, splashes against none: they move the open ground
                    beyond it, and not a pixel of the ground under the roof nearest the
                    camera -- a splash lands where the occlusion map says the rain does.
      rain-mist     the medium the post chain was handed is the rate's extinction times
                    `mist`, handed over at the outermost streak box, and arms the volume;
                    and on its own, in a fixture with no fog, it moves the frame.
      rain-mist-g   the medium's lobe is its own knob: the post chain is handed it, the streaks
                    keep theirs, a streaks-only frame does not move by a pixel, and a misty
                    frame does.
      rain-wind     a gusting scene wind that states an air speed carries the rain: the cover
                    and the splashes travel along the gust cycle's mean, which the probe holds
                    against its closed form, and the drops in this instant's air, which gusts;
                    the cover map is the same at two frames a gust apart; and a rain told not
                    to follow falls along its own wind.
      rain-ssr      the soaked surfaces with SSR against without: the open wet ground moves,
                    and not a pixel of the dry ground under the roof or of the wall the wet
                    ground runs up to -- the replacing fold averages each class only with its
                    own, so a wet pair's coverage cannot darken the wall above a puddle.
      rain-ssr-taa  the fixture under a procedural sky with TAA: on all thirty frames not one
                    pixel the tonemap reads is below zero, and SSR still moves the open wet
                    ground. Wet ground's reflection replaces a share of the environment's, and
                    taken out after TAA -- this frame's share from a frame TAA had already
                    resolved -- it went below zero on a ripple or a wet edge, up to 283 px a
                    frame here, until spec 13.21 folded it in ahead of TAA. Every other arm in
                    this group runs without TAA, where the two agree.
      rain-ssr-taau the same at render scale 0.5, where TAA also upscales: up to 578 px a frame
                    before the fix.
      rain-ssr-edge on the last frame of a turn, no more than RAIN_EDGE_BARE_MAX of the wet
                    pixels at the leading edge -- those SSR changes when the camera is held --
                    sit at exactly their --no-ssr value. Wet ground reads the frame before's
                    trace, and where the frame before did not reach it fell back to the
                    environment's share alone, which flashed a strip at the leading edge of
                    every turn in silent.
      rain-ripples  the flooded twin seen from under the roof, rings against none: the open
                    water rings, and the covered water nearest the camera does not move by a
                    pixel. From the fixture's own camera that water is too far off for a
                    ring to show, and a cover that did nothing would pass there.
      rain-splash-water  the twin flooded RAIN_DEEP_WATER deep, splashes against none: they
                    move the open water, which a crown thrown from the bed never reaches -- the
                    occlusion map holds no water, so the drop has to be met at the surface --
                    and not a pixel of the water under the roof.
      water-bounds-rain  the same twin with the water's bounds off it (spec 13.41): no surface
                    to strike, so the splashes land on the bed, 0 px from the twin with no
                    water at all.
      water-dry-rain  the same twin under a dry shape over all of it (spec 13.56): 0 px from
                    the twin with no water, as for the bounds.
      rain-ledger  the rain variant declares no more samplers than the dry variant of the
                    same materials: the cover is a tenant of the punctual array, and a unit
                    spent on it would be one the full variant does not have.
      rain-glass    the glazed twin, beaded panes against bare ones: the pane in the open moves,
                    and not a pixel of the pane under the roof's middle -- drops form where the
                    occlusion map says the rain strikes, and nowhere else.
      rain-glass-inner  from below, the flat glass canopy shows the drops the rain leaves on its
                    top, though the canopy shelters its own underside: the cover is asked on the
                    side the rain strikes, not the side that is seen.
      rain-glass-lens  the drops' lens at 1 against 0 moves the view through the open pane: a
                    drop bends what is seen through it, not only how it reflects.
      rain-glass-dry  RAIN_DAMP_S after the rain stopped the open pane is still beaded;
                    RAIN_DRIED_S after, the frame is 0 px from the twin with no rain at all.
      rain-glass-determinism  two runs of the beaded panes are 0 px apart.
      rain-drips-share  three drip lines' slots, as the probe reports them, are exactly what a
                    twin shares out -- one each, then the rest by largest remainder in proportion
                    to each line's rate times its cycle onto its own ground, the cycle built
                    from a fall time solved here in double -- and those fall times agree.
      rain-drips    a line along the roof's front edge moves the column under it, and not a
                    pixel of the open ground in front or the bare sky.
      rain-drips-dry  RAIN_DAMP_S after the rain stopped the line still drips; at
                    RAIN_DRIED_S the frame is 0 px from no rain at all.
      rain-drips-water  on the flooded twin, RAIN_DEEP_WATER deep, the line dripping onto the
                    ground under the water is 0 px from the same line dripping onto the water's
                    surface: a drop meets the water first, as a splash does.
      rain-drips-determinism  two runs of the dripping fixture are 0 px apart.
      rain-relief   the relief twin, in the roughness view: at relief 1 the trenches in its
                    height map stand nearly all under water and the plateaus nearly none, where
                    at relief 0 the two kinds of band hold the same share -- the puddles stand
                    in the ground's own lows, from the same noise.
      rain-relief-off  on the plain fixture, whose grounds carry no height map, relief 1 is 0 px
                    from relief 0 and no variant compiles the relief bit.
      rain-relief-ledger  each relief variant declares one sampler more than the same mask without
                    the bit -- the height map's -- and at relief 0 no variant carries it.

    Everything here runs on rain_fixture and its flooded, glazed and relief twins, whose answers
    are known from their geometry and their closed forms; the streak arms read the fixture at
    sheen 0, since the sheen is a look laid over the physics rather than part of it.
    """
    scene = asset(RAIN_FIXTURE)
    if not os.path.exists(scene):
        print(f"  rain-physics SKIP  ({RAIN_FIXTURE} not present)")
        return []
    failures = []
    # The cover points ride the first run: the physics, the state and the medium do not care.
    at = []
    for (x, y, z), _ in RAIN_COVER_POINTS + [((80.0, 0.0, 0.0), None)]:
        at += ["--rain-probe-at", f"{x},{y},{z}"]
    got, got_text = _rain_rows(scene, extra=at)

    errs, rows = [], got.get("physics", [])
    for row in rows:
        want = _rain_twin(row["rate"])
        errs += [abs(row[k] - want[k]) / want[k] for k in want]
    for row in got.get("velocity", []):
        want = _rain_velocity(row["d"])
        errs.append(abs(row["v"] - want) / want)
    ladder = sorted(rows, key=lambda r: r["rate"])
    monotone = all(b[k] > a[k] for a, b in zip(ladder, ladder[1:])
                   for k in ("beta", "density", "d0", "v0", "splash"))
    worst = (float("inf") if len(rows) != 5 or any(math.isnan(e) for e in errs)
             else max(errs))
    ok = worst <= RAIN_TOL and monotone
    at10 = next((r for r in rows if r["rate"] == 10.0), {})
    print(f"  rain-physics {'PASS' if ok else 'FAIL'}  worst relative error {worst:.2e} over "
          f"{len(rows)} rates and {len(got.get('velocity', []))} diameters (want "
          f"<={RAIN_TOL}); the ladder is {'monotone' if monotone else 'NOT MONOTONE'}; at "
          f"10 mm/h beta {at10.get('beta', float('nan')):.4g}/m, a meteorological "
          f"visibility of {3.912 / at10.get('beta', float('nan')) / 1000.0:.2f} km")
    if not ok:
        failures.append("rain-physics")

    d = (got.get("defaults") or [{}])[0]
    sched = {int(r["fps"]): r for r in got.get("schedule", [])}
    if d and 60 in sched and 30 in sched:
        rate, ref = sched[60]["rate"], d["reference"]
        wet_mid = 1.0 - math.exp(-sched[60]["rain_s"] / (d["wet_time"] * ref / rate))
        target = d["coverage"] * (1.0 - math.exp(-rate / ref))
        puddle_mid = target * (1.0 - math.exp(-sched[60]["rain_s"] / (d["fill_time"] * ref / rate)))
        want = {"wet_mid": wet_mid, "puddle_mid": puddle_mid,
                "wet_end": wet_mid * math.exp(-sched[60]["dry_s"] / d["dry_time"]),
                "puddle_end": puddle_mid * math.exp(-sched[60]["dry_s"] / d["drain_time"])}
        pace = max(abs(sched[60][k] - sched[30][k]) for k in want)
        closed = max(abs(sched[60][k] - v) for k, v in want.items())
        ok = pace <= RAIN_PACE_TOL and closed <= RAIN_PACE_TOL
        detail = (f"60 against 30 fps differ by {pace:.2e} and the 60 fps run misses the "
                  f"closed form by {closed:.2e} (want <={RAIN_PACE_TOL}); film "
                  f"{sched[60]['wet_mid']:.4f} after the rain and {sched[60]['wet_end']:.4f} "
                  f"after drying, puddles {sched[60]['puddle_mid']:.4f} then "
                  f"{sched[60]['puddle_end']:.4f}")
    else:
        ok, detail = False, "the probe printed no schedule or no defaults"
    print(f"  rain-wetting {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("rain-wetting")

    unsettled = os.path.join(workdir, "rain_dry.cscn")
    cscn_copy(scene, unsettled, lambda s: s["rain"].update({"settled": False}))
    settled = (got.get("state") or [{}])[0]
    ticked = (_rain_rows(unsettled, frames=RAIN_TICK_FRAMES)[0].get("state") or [{}])[0]
    if ticked.get("present") == 1.0 and settled.get("present") == 1.0 and d:
        rate, ref = ticked["rate"], d["reference"]
        elapsed = RAIN_TICK_FRAMES / 60.0
        wet = 1.0 - math.exp(-elapsed / (ticked["wet_time"] * ref / rate))
        puddle_target = ticked["coverage"] * (1.0 - math.exp(-rate / ref))
        puddle = puddle_target * (1.0 - math.exp(-elapsed / (ticked["fill_time"] * ref / rate)))
        err = max(abs(ticked["wetness"] - wet), abs(ticked["puddle"] - puddle),
                  abs(settled["wetness"] - 1.0), abs(settled["puddle"] - puddle_target))
        ok = err <= RAIN_PACE_TOL
        detail = (f"from dry, {RAIN_TICK_FRAMES} frames leave a film of {ticked['wetness']:.5f} "
                  f"(want {wet:.5f}) and puddles of {ticked['puddle']:.5f} (want {puddle:.5f}); "
                  f"settled, {settled['wetness']:.5f} and {settled['puddle']:.5f} (want 1 and "
                  f"{puddle_target:.5f}); worst {err:.2e}")
    else:
        ok, detail = False, "the fixture's rain did not reach the scene"
    print(f"  rain-tick {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("rain-tick")

    def cover_of(rows):
        return (rows.get("cover") or [{}])[0], rows.get("exposure", [])

    def cover(scene_path, extra=()):
        return cover_of(_rain_rows(scene_path, extra=at + list(extra))[0])

    head, pts = cover_of(got)
    wrong = [f"({p['x']:g},{p['y']:g},{p['z']:g})"
             for p, (_, want) in zip(pts, RAIN_COVER_POINTS)
             if p.get("inside") != 1.0 or bool(p.get("exposed")) != want]
    off_map = len(pts) == len(RAIN_COVER_POINTS) + 1 and pts[-1].get("inside") == 0.0
    ok = head.get("present") == 1.0 and not wrong and off_map
    print(f"  rain-exposure {'PASS' if ok else 'FAIL'}  {len(RAIN_COVER_POINTS) - len(wrong)} of "
          f"{len(RAIN_COVER_POINTS)} points answer as the geometry says (wrong: "
          f"{', '.join(wrong) or 'none'}); the point 80 m out is "
          f"{'off the map' if off_map else 'NOT off the map'}; layer "
          f"{head.get('layer', float('nan')):g} of an array at {head.get('edge', float('nan')):g}^2")
    if not ok:
        failures.append("rain-exposure")

    # The CPU cover query, asked every frame as a listener asks it, against the exact readback
    # of the same point: one under the roof and one in the open, so it has to say both.
    asks = []
    for point in RAIN_ASK_POINTS:
        where = ",".join(f"{c:g}" for c in point)
        rows, _ = _rain_rows(scene, extra=["--rain-ask", where, "--rain-probe-at", where],
                             frames=RAIN_ASK_FRAMES)
        ask = (rows.get("ask") or [{}])[0]
        exact = (rows.get("exposure") or [{}])[0]
        asks.append((where, ask.get("answered"), ask.get("open"), exact.get("exposed")))
    ok = (all(answered == 1.0 and open_ == exact for _, answered, open_, exact in asks)
          and sorted(exact for *_, exact in asks) == [0.0, 1.0])
    detail = "; ".join(f"({w}) answered {a:g}, open {o:g} against {e:g}" for w, a, o, e in asks
                       if None not in (a, o, e)) or "no answer"
    print(f"  rain-ask {'PASS' if ok else 'FAIL'}  {detail} (want answered, and the exact "
          f"readback's answer: one covered, one open)")
    if not ok:
        failures.append("rain-ask")

    # The session snapshot carries the rain: a variant's settings dumped, then restored over the
    # plain fixture, read back through the probe's own state row.
    tuned = os.path.join(workdir, "rain_tuned.cscn")
    cscn_copy(scene, tuned, lambda s: s["rain"].update(RAIN_CONFIG_TUNED))
    dump = os.path.join(workdir, "rain_config.json")
    _rain_rows(tuned, extra=["--config-dump", dump], frames=2)
    restored = (_rain_rows(scene, extra=["--config", dump], frames=2)[0].get("state") or [{}])[0]
    want = {RAIN_CONFIG_READ[k]: v for k, v in RAIN_CONFIG_TUNED.items()}
    back = {k: restored.get(k) for k in want}
    ok = os.path.exists(dump) and all(
        back[k] is not None and abs(back[k] - v) <= 1e-6 * max(1.0, abs(v))
        for k, v in want.items())
    print(f"  rain-config {'PASS' if ok else 'FAIL'}  restored {back} (want {want})")
    if not ok:
        failures.append("rain-config")

    casting = os.path.join(workdir, "rain_casting.cscn")
    cscn_copy(scene, casting, lambda s: s["lights"][0].update({"cast_shadows": True}))
    runs = {"lamp casting": cover(casting), "shadows off": cover(scene, ["--no-shadows"])}
    base = [(p.get("exposed"), p.get("map")) for p in pts]
    same = {k: [(p.get("exposed"), p.get("map")) for p in v[1]] == base for k, v in runs.items()}
    layers = {k: v[0].get("layer") for k, v in runs.items()}
    ok = all(same.values()) and layers == {"lamp casting": 6.0, "shadows off": 0.0}
    detail = "; ".join(f"{k}: layer {layers[k]:g}, cover and stored depth "
                       f"{'identical' if same[k] else 'DIFFERENT'}" for k in runs)
    print(f"  rain-tenant {'PASS' if ok else 'FAIL'}  {detail} (want layers 6 and 0)")
    if not ok:
        failures.append("rain-tenant")

    def variant(name, mutate=None, base=scene, camera=None, rain=None):
        """`base` copied as `name`: its camera replaced, its rain updated, then `mutate`."""
        def edit(s):
            if camera is not None:
                s["camera"] = dict(camera)
            if rain is not None:
                s["rain"].update(rain)
            if mutate is not None:
                mutate(s)
        path = os.path.join(workdir, f"rain_{name}.cscn")
        cscn_copy(base, path, edit)
        return path

    def frame(name, scene_path, extra=()):
        out = os.path.join(workdir, f"rain_{name}.ppm")
        err = render(scene_path, out, list(extra))
        return None if err else out

    def shot(name, flags=RAIN_LINEAR, **kw):
        """A frame of variant(name, **kw)."""
        return frame(name, variant(name, **kw), flags)

    no_rain = variant("none", lambda s: s.pop("rain"))
    still = variant("still", lambda s: s["rain"].update({"rate": 0.0}))
    a, b = frame("none", no_rain), frame("still", still)
    ae, pae = compare(a, b) if a and b else (sys.maxsize, 1.0)
    ok = ae == 0
    print(f"  rain-off {'PASS' if ok else 'FAIL'}  rate 0 against no rain block: {ae} px "
          f"(want 0)")
    if not ok:
        failures.append("rain-off")

    a, b = frame("run1", scene), frame("run2", scene)
    ae, pae = compare(a, b) if a and b else (sys.maxsize, 1.0)
    ok = ae == 0
    print(f"  rain-determinism {'PASS' if ok else 'FAIL'}  two runs {ae} px apart (want 0)")
    if not ok:
        failures.append("rain-determinism")

    # Sheen 0: these read the light a drop scatters, and the sheen is a look laid over what it
    # refracts, which a lamp that lights the backdrop would move as well. No mist either: the
    # medium past the streaks scatters the same lamp into the same sky, and these read streaks.
    def lamp_at(pos, cd, glint=None):
        def mutate(s):
            s["lights"] = [{"name": "rain_probe_lamp", "type": "point", "position": pos,
                            "color": [1.0, 1.0, 1.0], "intensity": cd, "range": 40.0}]
            s["rain"]["streakSheen"] = 0.0
            s["rain"]["mist"] = 0.0
            if glint is not None:
                s["rain"]["streakGlint"] = glint
        return mutate

    def sky_light(name, mutate, flags):
        """(mean linear luma over the sky crop, pixels in it at code 255)."""
        path = frame(name, variant(name, mutate), flags)
        if not path:
            return float("nan"), 0
        w, h, pix = _read_ppm(path)
        x0, y0, x1, y1 = RAIN_SKY_BOX
        clipped = sum(1 for py in range(int(y0 * h), int(y1 * h))
                      for px in range(int(x0 * w), int(x1 * w))
                      if max(pix[3 * (py * w + px):3 * (py * w + px) + 3]) == 255)
        return _box_luma_dense(pix, w, h, RAIN_SKY_BOX, _DISPLAY_TO_LINEAR), clipped

    # Glints off for the lobe: it is a claim about the phase. The backlit frame clips where
    # the eye looks straight through the rain at the lamp, and that is allowed: clipping can
    # only UNDERSTATE the backlit light, so the ratio read off it is a lower bound.
    dark, _ = sky_light("dark", lamp_at(RAIN_LAMP_BACK, 0.0), RAIN_LINEAR)
    back_on, back_clip = sky_light("back", lamp_at(RAIN_LAMP_BACK, RAIN_LAMP_CD, 0.0),
                                   RAIN_LINEAR)
    front_on, _ = sky_light("front", lamp_at(RAIN_LAMP_FRONT, RAIN_LAMP_CD, 0.0), RAIN_LINEAR)
    back, front = back_on - dark, front_on - dark
    ratio = back / front if front > 0 else float("inf")
    ok = front > 0 and ratio > RAIN_LOBE_MIN
    print(f"  rain-lobe {'PASS' if ok else 'FAIL'}  the lamp adds {back:.3e} to the sky crop "
          f"backlit and {front:.3e} front-lit, a ratio of {ratio:.1f} (want > "
          f"{RAIN_LOBE_MIN:g}, and front-lit above 0); {back_clip} backlit pixels clip, which "
          f"only lowers the ratio")
    if not ok:
        failures.append("rain-lobe")

    # The glint arm must NOT clip -- a clipped glint would hide a pattern that adds light --
    # which is why it lights the rain from the side (see RAIN_LAMP_SIDE).
    smooth_on, smooth_clip = sky_light("smooth", lamp_at(RAIN_LAMP_SIDE, RAIN_GLINT_CD, 0.0),
                                       RAIN_LINEAR)
    sparkle_on, sparkle_clip = sky_light("sparkle", lamp_at(RAIN_LAMP_SIDE, RAIN_GLINT_CD, 1.0),
                                         RAIN_LINEAR)
    smooth, sparkle = smooth_on - dark, sparkle_on - dark
    drift = abs(sparkle - smooth) / smooth if smooth > 0 else float("inf")
    clipped = smooth_clip + sparkle_clip
    ok = drift <= RAIN_GLINT_TOL and clipped == 0
    print(f"  rain-glint {'PASS' if ok else 'FAIL'}  side-lit rain's light {smooth:.4e} with no "
          f"glints and {sparkle:.4e} with all of it in glints, {drift:.2%} apart (want <= "
          f"{RAIN_GLINT_TOL:.0%}); {clipped} pixels clip (want 0)")
    if not ok:
        failures.append("rain-glint")

    # Surfaces only: no streaks, no splashes and no mist, so every difference between the two
    # frames is the wetting.
    surfaces = variant("surfaces", lambda s: s["rain"].update(RAIN_SURFACES_ONLY))
    wet_path = frame("wet", surfaces, RAIN_LINEAR)
    dry_path = frame("dry", surfaces, RAIN_LINEAR + ["--no-rain"])
    ratios = {}
    if wet_path and dry_path:
        w, h, wet_pix = _read_ppm(wet_path)
        _, _, dry_pix = _read_ppm(dry_path)
        project = _projector(_cscn_camera(RAIN_FIXTURE), w, h)
        for name, point in RAIN_WET_POINTS:
            x, y = project(point)
            box = ((x - 3) / w, (y - 3) / h, (x + 4) / w, (y + 4) / h)
            dry_luma = _box_luma_dense(dry_pix, w, h, box, _DISPLAY_TO_LINEAR)
            ratios[name] = (_box_luma_dense(wet_pix, w, h, box, _DISPLAY_TO_LINEAR) / dry_luma
                            if dry_luma > 0 else float("nan"))
    covered = [ratios.get(n, float("nan")) for n in ("covered porous", "covered sealed")]
    porous = ratios.get("open porous", float("nan"))
    sealed = ratios.get("open sealed", float("nan"))
    ok = (all(r == 1.0 for r in covered) and porous < RAIN_WET_POROUS_MAX
          and sealed > 1.5 * porous)
    print(f"  rain-wet {'PASS' if ok else 'FAIL'}  wet over dry: under the roof "
          f"{covered[0]:.4f} and {covered[1]:.4f} (want exactly 1), in the open porous "
          f"{porous:.3f} (want < {RAIN_WET_POROUS_MAX}) and sealed {sealed:.3f} (want over 1.5x "
          f"the porous)")
    if not ok:
        failures.append("rain-wet")

    def moved_round(a_path, b_path, view, points):
        """(pixels differing in a 7x7 box round each point's image, pixels in those boxes).

        The second count is how the caller knows every point was in frame: a point off the
        edge contributes no pixels, and a box of none cannot have moved."""
        w, h, pa = _read_ppm(a_path)
        _, _, pb = _read_ppm(b_path)
        project = _projector(view, w, h)
        moved = total = 0
        for p in points:
            x, y = (int(c) for c in project(p))
            m, t = _moved_in_rect(pa, pb, w, h, x - 3, y - 3, x + 4, y + 4)
            moved, total = moved + m, total + t
        return moved, total

    def moved_in_quad(a_path, b_path, view, quad):
        """(pixels differing, pixels) in the rectangle inscribed in a world quad's image."""
        w, h, pa = _read_ppm(a_path)
        _, _, pb = _read_ppm(b_path)
        return _moved_in_rect(pa, pb, w, h, *_inscribed_rect(_projector(view, w, h), quad))

    def quad_moved(a_path, b_path, view, quad):
        """(fraction of the quad's pixels moved, pixels moved, pixels), or (nan, -1, 0) when a
        frame is missing."""
        if not (a_path and b_path):
            return float("nan"), -1, 0
        m, t = moved_in_quad(a_path, b_path, view, quad)
        return (m / t if t else float("nan")), m, t

    def apart(a_path, b_path):
        """Pixels two frames differ in; the most there could be when either is missing."""
        return compare(a_path, b_path)[0] if a_path and b_path else sys.maxsize

    def ground_quad(extents, y):
        (xa, xb), (za, zb) = extents
        return [(xa, y, za), (xb, y, za), (xa, y, zb), (xb, y, zb)]

    # Puddles against none, on the same soaked surfaces: rain_wet's frame is the puddled one.
    still_points = [p for n, p in RAIN_WET_POINTS if n.startswith("covered")] + RAIN_WALL_POINTS
    unpooled = frame("unpooled", variant("unpooled", lambda s: s["rain"].update(
        {**RAIN_SURFACES_ONLY, "puddleCoverage": 0.0})), RAIN_LINEAR)
    frac, still_moved, still_total = _moved_fraction(wet_path, unpooled), -1, 0
    if wet_path and unpooled:
        still_moved, still_total = moved_round(wet_path, unpooled, _cscn_camera(RAIN_FIXTURE),
                                               still_points)
    ok = frac > RAIN_FEATURE_MIN and still_total == 49 * len(still_points) and still_moved == 0
    print(f"  rain-puddles {'PASS' if ok else 'FAIL'}  puddles move {frac:.1%} of the frame "
          f"(want > {RAIN_FEATURE_MIN:.0%}) and {still_moved} of {still_total} pixels round the "
          f"covered ground and the wall (want 0 of {49 * len(still_points)})")
    if not ok:
        failures.append("rain-puddles")

    # Splashes against none, from under the roof looking out: they land where the occlusion map
    # says the rain reaches, so the ground under the roof, nearest the camera, cannot move.
    def splashes_under_roof(name, base=scene, rain=None, floor_y=0.0):
        """Splashes against none, seen from under the roof: (fraction of the frame they move,
        pixels moved, pixels) of the floor under the roof nearest the camera, at `floor_y`."""
        splashing = {"streakCount": 0, "mist": 0.0, **RAIN_SPLASH_ARM, **(rain or {})}
        on = shot(name, base=base, camera=RAIN_UNDER_ROOF_CAMERA, rain=splashing)
        off = shot(name + "_none", base=base, camera=RAIN_UNDER_ROOF_CAMERA,
                   rain={**splashing, "splashCount": 0})
        _, moved, total = quad_moved(on, off, _cscn_view(RAIN_UNDER_ROOF_CAMERA),
                                     ground_quad(RAIN_COVERED_PATCH, floor_y))
        return _moved_fraction(on, off), moved, total

    frac, covered_moved, covered_total = splashes_under_roof("splashed")
    ok = (frac > RAIN_SPLASH_MIN and covered_total >= RAIN_COVERED_MIN_PX
          and covered_moved == 0)
    print(f"  rain-splashes {'PASS' if ok else 'FAIL'}  splashes move {frac:.2%} of the frame "
          f"(want > {RAIN_SPLASH_MIN:.2%}) and {covered_moved} of {covered_total} pixels of the "
          f"ground under the roof (want 0, of at least {RAIN_COVERED_MIN_PX})")
    if not ok:
        failures.append("rain-splashes")

    # The rain as a medium: what the post chain was handed, against the rate's extinction and
    # the streaks' reach as the scene sets them, and then that it reaches a frame on its own --
    # the fixture has no fog, so nothing else arms the volume.
    medium = (got.get("medium") or [{}])[0]
    nan = float("nan")
    want_sigma = (_rain_twin(settled["rate"])["beta"] * settled["mist"] if "rate" in settled
                  else nan)
    want_near = (settled.get("streak_radius", nan) * _rain_constant("RAIN_STREAK_BOX_SCALE") **
                 (_rain_constant("RAIN_STREAK_BOXES") - 1)
                 if settled.get("streak_count", 0) > 0 else 0.0)
    sigma_err = abs(medium.get("sigma", nan) - want_sigma) / want_sigma
    near_err = abs(medium.get("near", nan) - want_near)

    # Against the surfaces-only frame, which is this fixture with the medium at 0.
    misted = frame("misted", variant("misted", lambda s: s["rain"].update(
        {**RAIN_SURFACES_ONLY, "mist": 1.0})), RAIN_LINEAR)
    frac = _moved_fraction(wet_path, misted)
    ok = (sigma_err <= RAIN_TOL and near_err <= 1e-4 and medium.get("armed") == 1.0
          and frac > RAIN_FEATURE_MIN)
    print(f"  rain-mist {'PASS' if ok else 'FAIL'}  published extinction {medium.get('sigma', nan):.6g}"
          f"/m against {want_sigma:.6g} ({sigma_err:.1e} apart, want <= {RAIN_TOL:g}), handover "
          f"{medium.get('near', nan):g} m against {want_near:g}, armed {medium.get('armed', nan):g} "
          f"(want 1); the medium alone moves {frac:.1%} of a fogless frame (want > "
          f"{RAIN_FEATURE_MIN:.0%})")
    if not ok:
        failures.append("rain-mist")

    # The medium's lobe on its own: handed to the post chain, the streaks keeping theirs. A frame
    # with no medium cannot see it; one with the medium has to -- with no streaks, so the medium
    # starts at the eye rather than past the fixture's far wall.
    lobe_rows, _ = _rain_rows(variant("mist_g", rain={"mistForwardG": RAIN_MIST_G}))
    lobe_medium = (lobe_rows.get("medium") or [{}])[0]
    lobe_air = (lobe_rows.get("air") or [{}])[0]
    streak_g = (got.get("air") or [{}])[0].get("streak_g", nan)
    still_a = shot("lobe_dry_a", rain={"mist": 0.0})
    still_b = shot("lobe_dry_b", rain={"mist": 0.0, "mistForwardG": RAIN_MIST_G})
    misty_b = shot("lobe_misty",
                   rain={**RAIN_SURFACES_ONLY, "mist": 1.0, "mistForwardG": RAIN_MIST_G})
    still_ae = apart(still_a, still_b)
    misty_frac = _moved_fraction(misted, misty_b)
    ok = (abs(lobe_medium.get("g", nan) - RAIN_MIST_G) <= 1e-6
          and lobe_air.get("streak_g", nan) == streak_g and still_ae == 0 and misty_frac > 0)
    print(f"  rain-mist-g {'PASS' if ok else 'FAIL'}  published lobe {lobe_medium.get('g', nan):g} "
          f"(want {RAIN_MIST_G:g}), the streaks' {lobe_air.get('streak_g', nan):g} (want theirs "
          f"unchanged, {streak_g:g}); with no medium {still_ae} px moved (want 0), with it "
          f"{misty_frac:.2%} of the frame (want > 0)")
    if not ok:
        failures.append("rain-mist-g")

    # The scene's wind, gusting, and no wind of the rain's own: what carries it is the scene's.
    def windy(name, follow=True):
        def mutate(s):
            s["wind"] = dict(RAIN_SCENE_WIND)
            s["rain"].update({"wind": [0.0, 0.0, 0.0], "followSceneWind": follow})
        return variant(name, mutate)

    def air_at(path, frames):
        rows, _ = _rain_rows(path, extra=at, frames=frames)
        return ((rows.get("air") or [{}])[0], [p.get("map") for p in rows.get("exposure", [])])

    blowing = windy("windy")
    (air_a, map_a), (air_b, map_b) = air_at(blowing, 2), air_at(blowing, RAIN_GUST_FRAMES)
    calm_air, _ = air_at(windy("windy_unfollowed", follow=False), 2)
    d = RAIN_SCENE_WIND["direction"]
    across = math.hypot(d[0], d[2])
    peak, depth = RAIN_SCENE_WIND["airSpeed"], RAIN_SCENE_WIND["gustAmount"]
    mean = peak * _gust_mean()
    want_mean = (d[0] / across * mean, d[2] / across * mean)
    fall = _rain_twin(settled.get("rate", nan))["v0"]
    norm = math.sqrt(want_mean[0] ** 2 + fall ** 2 + want_mean[1] ** 2)
    want_travel = (want_mean[0] / norm, -fall / norm, want_mean[1] / norm)
    mean_err = max(abs(air_a.get("mean_x", nan) - want_mean[0]),
                   abs(air_a.get("mean_z", nan) - want_mean[1]))
    travel_err = max(abs(air_a.get(k, nan) - w)
                     for k, w in zip(("travel_x", "travel_y", "travel_z"), want_travel))
    now = [math.hypot(a.get("now_x", nan), a.get("now_z", nan)) for a in (air_a, air_b)]
    gusts = (all(peak * (1.0 - depth) - 1e-4 <= n <= peak + 1e-4 for n in now)
             and abs(now[0] - now[1]) > 0.05 * peak)
    still_cover = bool(map_a) and map_a == map_b
    straight = abs(calm_air.get("travel_y", nan) + 1.0) <= 1e-6
    ok = mean_err <= 1e-5 and travel_err <= 1e-5 and gusts and still_cover and straight
    print(f"  rain-wind {'PASS' if ok else 'FAIL'}  mean air ({air_a.get('mean_x', nan):.4f}, "
          f"{air_a.get('mean_z', nan):.4f}) m/s against ({want_mean[0]:.4f}, {want_mean[1]:.4f}), "
          f"travel {travel_err:.1e} off its closed form (want <= 1e-5); this instant's air "
          f"{now[0]:.3f} then {now[1]:.3f} m/s (want inside the envelope and moving); the cover "
          f"{'identical' if still_cover else 'DIFFERENT'} a gust apart; unfollowed, travel_y "
          f"{calm_air.get('travel_y', nan):.6f} (want -1)")
    if not ok:
        failures.append("rain-wind")

    # Wet ground in SSR, on the soaked surfaces alone: traced where the ground is wet, and not a
    # pixel moved where it is dry or on the wall above it. The surfaces frame has SSR on.
    surfaces_nossr = frame("ssr_off", surfaces, RAIN_LINEAR + ["--no-ssr"])
    regions = {}
    if wet_path and surfaces_nossr:
        for name, quad in (("open", ground_quad(RAIN_SSR_OPEN, 0.0)),
                           ("covered", ground_quad(RAIN_SSR_COVERED, 0.0)),
                           ("wall", RAIN_SSR_WALL)):
            regions[name] = moved_in_quad(wet_path, surfaces_nossr, _cscn_camera(RAIN_FIXTURE),
                                          quad)
    moved = {k: m / t if t else float("nan") for k, (m, t) in regions.items()}
    ok = (len(regions) == 3 and all(t >= RAIN_SSR_MIN_PX for _, t in regions.values())
          and moved["open"] > RAIN_FEATURE_MIN and regions["covered"][0] == 0
          and regions["wall"][0] == 0)
    detail = ", ".join(f"{k} {m} of {t}" for k, (m, t) in regions.items()) or "no frames"
    print(f"  rain-ssr {'PASS' if ok else 'FAIL'}  SSR against none, pixels moved: {detail} (want "
          f"the open ground over {RAIN_FEATURE_MIN:.0%}, the covered ground and the wall 0, each "
          f"region at least {RAIN_SSR_MIN_PX} px)")
    if not ok:
        failures.append("rain-ssr")

    # Under TAA, where the late fold used to take this frame's specular out of a frame TAA had
    # already resolved (spec 13.21): every frame read before the tonemap. The open wet ground must
    # still move against --no-ssr, or a fold that dropped the reflection would pass.
    def below_zero(name, flags):
        traced = os.path.join(workdir, f"rain_{name}.ppm")
        bare = os.path.join(workdir, f"rain_{name}_nossr.ppm")
        rows, _ = _probe_render(scene, "--negative-probe", "negative-probe", frames=RAIN_NEG_FRAMES,
                                extra=RAIN_SKY_TAA + flags + ["-S", traced])
        failed = render(scene, bare, RAIN_SKY_TAA + flags + ["--no-ssr"], frames=RAIN_NEG_FRAMES)
        below = [int(r["below_0005"]) for r in rows]
        lowest = min((float(r["min"]) for r in rows), default=float("nan"))
        frac, _, _ = quad_moved(traced if os.path.exists(traced) else None,
                                None if failed else bare, _cscn_camera(RAIN_FIXTURE),
                                ground_quad(RAIN_SSR_OPEN, 0.0))
        ok = len(rows) == RAIN_NEG_FRAMES and sum(below) == 0 and frac > RAIN_FEATURE_MIN
        return ok, (f"{sum(below)} pixels below zero on {sum(1 for b in below if b)} of "
                    f"{len(rows)} frames, the lowest {lowest:.4f} (want 0 on all "
                    f"{RAIN_NEG_FRAMES}); SSR moves {frac:.1%} of the open wet ground (want > "
                    f"{RAIN_FEATURE_MIN:.0%})")

    ok, detail = below_zero("ssr_taa", [])
    print(f"  rain-ssr-taa {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("rain-ssr-taa")
    ok, detail = below_zero("ssr_taau", RAIN_TAAU)
    print(f"  rain-ssr-taau {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("rain-ssr-taau")

    ok, detail = _rain_ssr_edge(workdir)
    print(f"  rain-ssr-edge {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("rain-ssr-edge")

    # Rings against none on the flooded twin, looking out from under the roof.
    water = asset(RAIN_WATER_FIXTURE)
    if not os.path.exists(water):
        print(f"  rain-ripples SKIP  ({RAIN_WATER_FIXTURE} not present)")
        return failures

    def under_roof(name, strength):
        def mutate(s):
            s["camera"] = dict(RAIN_UNDER_ROOF_CAMERA)
            s["rain"].update({**RAIN_SURFACES_ONLY, "rippleSize": RAIN_RING_SIZE})
            if strength is not None:
                s["rain"]["rippleStrength"] = strength
        return frame(name, variant(name, mutate, base=water), RAIN_LINEAR)

    ringed, calm = under_roof("ringed", None), under_roof("calm", 0.0)
    frac, covered_moved, covered_total = _moved_fraction(ringed, calm), -1, 0
    if ringed and calm:
        covered_moved, covered_total = moved_in_quad(ringed, calm,
                                                     _cscn_view(RAIN_UNDER_ROOF_CAMERA),
                                                     ground_quad(RAIN_COVERED_PATCH, 0.05))
    ok = (frac > RAIN_FEATURE_MIN and covered_total >= RAIN_COVERED_MIN_PX
          and covered_moved == 0)
    print(f"  rain-ripples {'PASS' if ok else 'FAIL'}  rings move {frac:.1%} of the frame (want > "
          f"{RAIN_FEATURE_MIN:.0%}) and {covered_moved} of {covered_total} pixels of the water "
          f"under the roof (want 0, of at least {RAIN_COVERED_MIN_PX})")
    if not ok:
        failures.append("rain-ripples")

    # Splashes on the water's surface rather than on the bed under it: the twin flooded deep
    # enough that a crown thrown from the bed never reaches the surface. Still water, so the
    # rings do not move it either way.
    deep = variant("deep", lambda s: s["water"].update({"level": RAIN_DEEP_WATER}), base=water)
    frac, covered_moved, covered_total = splashes_under_roof(
        "struck", base=deep, rain={"rippleStrength": 0.0}, floor_y=RAIN_DEEP_WATER)
    ok = (frac > RAIN_SPLASH_MIN and covered_total >= RAIN_COVERED_MIN_PX
          and covered_moved == 0)
    print(f"  rain-splash-water {'PASS' if ok else 'FAIL'}  splashes on water {RAIN_DEEP_WATER:g} m "
          f"deep move {frac:.2%} of the frame (want > {RAIN_SPLASH_MIN:.2%}) and {covered_moved} "
          f"of {covered_total} pixels of the water under the roof (want 0, of at least "
          f"{RAIN_COVERED_MIN_PX})")
    if not ok:
        failures.append("rain-splash-water")

    # And only where the water is (spec 13.41): bounds off it leave no surface to strike, so the
    # splashes land on the bed exactly as they do with no water at all.
    splashing = {"streakCount": 0, "mist": 0.0, "rippleStrength": 0.0, **RAIN_SPLASH_ARM}
    away = shot("bounds_away", RAIN_LINEAR + ["--water-bounds", WATER_BOUNDS_AWAY], base=deep,
                camera=RAIN_UNDER_ROOF_CAMERA, rain=splashing)
    dry = shot("bounds_dry", base=deep, camera=RAIN_UNDER_ROOF_CAMERA, rain=splashing,
               mutate=lambda s: s.pop("water"))
    moved = compare(away, dry)[0] if away and dry else sys.maxsize
    ok = moved == 0
    print(f"  water-bounds-rain {'PASS' if ok else 'FAIL'}  splashes under bounds off the water: "
          f"{moved} px from no water (want 0)")
    if not ok:
        failures.append("water-bounds-rain")

    # And not where a dry shape is (spec 13.56): one over the whole twin is no water to strike.
    covered = shot("dry_over", RAIN_LINEAR + ["--water-dry", WATER_DRY_OVER], base=deep,
                   camera=RAIN_UNDER_ROOF_CAMERA, rain=splashing)
    moved = compare(covered, dry)[0] if covered and dry else sys.maxsize
    ok = moved == 0
    print(f"  water-dry-rain {'PASS' if ok else 'FAIL'}  splashes under a dry shape over the "
          f"water: {moved} px from no water (want 0)")
    if not ok:
        failures.append("water-dry-rain")

    bits, full = _pbr_feature_bits()
    rain_bit = bits["rain"]

    def samplers(text):
        """{variant name: (feature mask, samplers declared)} for every variant the materials
        resolved to -- not the full uber-shaders, which declare every sampler whatever rains."""
        return {m.group(1): (int(m.group(2)), int(m.group(4))) for m in _PBR_VARIANT.finditer(text)
                if int(m.group(2)) != full}

    wet_logged = samplers(got_text)
    dry_logged = samplers(_rain_rows(scene, extra=["--no-rain"])[1])
    wet_variants = {n: count for n, (_, count) in wet_logged.items()}
    dry_variants = {n: count for n, (_, count) in dry_logged.items()}
    wet_count = max(wet_variants.values(), default=-1)
    dry_count = max(dry_variants.values(), default=-1)
    ok = (wet_count >= 0 and dry_count >= 0 and wet_count == dry_count
          and any(mask & rain_bit for mask, _ in wet_logged.values()))
    print(f"  rain-ledger {'PASS' if ok else 'FAIL'}  wet {wet_variants or 'none'} against dry "
          f"{dry_variants or 'none'} (want the rain bit set wet, and the same sampler count)")
    if not ok:
        failures.append("rain-ledger")

    # Drops on glass, on the glazed twin with nothing in the air, so a pair of frames differs only
    # in what the rain leaves on the panes.
    glazed = asset(RAIN_GLASS_FIXTURE)
    glass_view = _cscn_camera(RAIN_GLASS_FIXTURE)
    below_view = _cscn_view(RAIN_GLASS_BELOW_CAMERA)

    def unbeaded(s):
        s["materials"]["rain_glass"]["rainBeads"] = "off"

    def glass(name, rain=None, **kw):
        return shot(name, base=glazed, rain={**RAIN_SURFACES_ONLY, **(rain or {})}, **kw)

    beaded, bare = glass("beaded"), glass("bare", mutate=unbeaded)
    open_f, _, open_t = quad_moved(beaded, bare, glass_view, RAIN_GLASS_OPEN)
    _, cov_m, cov_t = quad_moved(beaded, bare, glass_view, RAIN_GLASS_COVERED)
    ok = (open_t >= RAIN_REGION_MIN_PX and open_f > RAIN_FEATURE_MIN
          and cov_t >= RAIN_REGION_MIN_PX and cov_m == 0)
    print(f"  rain-glass {'PASS' if ok else 'FAIL'}  beads move {open_f:.1%} of the open pane's "
          f"{open_t} px (want > {RAIN_FEATURE_MIN:.0%}) and {cov_m} of the covered pane's {cov_t} "
          f"(want 0, each pane at least {RAIN_REGION_MIN_PX} px)")
    if not ok:
        failures.append("rain-glass")

    below = glass("below_beaded", camera=RAIN_GLASS_BELOW_CAMERA)
    below_bare = glass("below_bare", camera=RAIN_GLASS_BELOW_CAMERA, mutate=unbeaded)
    under_f, _, under_t = quad_moved(below, below_bare, below_view, RAIN_GLASS_CANOPY)
    ok = under_t >= RAIN_REGION_MIN_PX and under_f > RAIN_FEATURE_MIN
    print(f"  rain-glass-inner {'PASS' if ok else 'FAIL'}  seen from below, the beads on the "
          f"canopy's top move {under_f:.1%} of its underside's {under_t} px (want > "
          f"{RAIN_FEATURE_MIN:.0%})")
    if not ok:
        failures.append("rain-glass-inner")

    unbent = glass("unbent", rain={"glassLens": 0.0})
    lens_f, _, lens_t = quad_moved(beaded, unbent, glass_view, RAIN_GLASS_OPEN)
    ok = lens_t >= RAIN_REGION_MIN_PX and lens_f > RAIN_FEATURE_MIN
    print(f"  rain-glass-lens {'PASS' if ok else 'FAIL'}  the lens at 1 against 0 moves "
          f"{lens_f:.1%} of the open pane's {lens_t} px (want > {RAIN_FEATURE_MIN:.0%})")
    if not ok:
        failures.append("rain-glass-lens")

    damp = glass("damp", rain={"dryFor": RAIN_DAMP_S})
    damp_bare = glass("damp_bare", rain={"dryFor": RAIN_DAMP_S}, mutate=unbeaded)
    damp_f, _, damp_t = quad_moved(damp, damp_bare, glass_view, RAIN_GLASS_OPEN)
    dried = glass("dried", rain={"dryFor": RAIN_DRIED_S})
    never = shot("never", base=glazed, mutate=lambda s: s.pop("rain"))
    dried_ae = apart(dried, never)
    ok = damp_t >= RAIN_REGION_MIN_PX and damp_f > RAIN_FEATURE_MIN and dried_ae == 0
    print(f"  rain-glass-dry {'PASS' if ok else 'FAIL'}  {RAIN_DAMP_S:g} s after the rain the "
          f"beads still move {damp_f:.1%} of the open pane (want > {RAIN_FEATURE_MIN:.0%}); "
          f"{RAIN_DRIED_S:g} s after, {dried_ae} px from no rain at all (want 0)")
    if not ok:
        failures.append("rain-glass-dry")

    twice = apart(beaded, glass("beaded_again"))
    ok = twice == 0
    print(f"  rain-glass-determinism {'PASS' if ok else 'FAIL'}  two runs {twice} px apart (want 0)")
    if not ok:
        failures.append("rain-glass-determinism")

    # Drips: the share of the slots each line gets, against the twin, and the fall time behind it.
    share_rows, _ = _rain_rows(variant(
        "drip_share", rain={"drips": RAIN_DRIP_SHARE_LINES, "dripCount": RAIN_DRIP_SHARE_COUNT}))
    got_slots = [int(r["slots"]) for r in share_rows.get("drip", [])]
    want_slots = _drip_shares(RAIN_DRIP_SHARE_LINES, RAIN_DRIP_SHARE_COUNT)
    falls = share_rows.get("dripfall", [])
    fall_err = max((abs(r["t"] - _drip_fall_time(r["h"])) / _drip_fall_time(r["h"])
                    for r in falls), default=float("inf"))
    given = (share_rows.get("drips") or [{}])[0].get("given")
    ok = got_slots == want_slots and given == RAIN_DRIP_SHARE_COUNT and fall_err <= RAIN_TOL
    print(f"  rain-drips-share {'PASS' if ok else 'FAIL'}  slots {got_slots} against the twin's "
          f"{want_slots}, {given} handed out of {RAIN_DRIP_SHARE_COUNT}; fall times "
          f"{fall_err:.1e} off the twin's (want <= {RAIN_TOL:g})")
    if not ok:
        failures.append("rain-drips-share")

    # The drips on their own: no streaks, no splashes, no mist, so a pair of frames differs only
    # in a line's drops and their crowns. With no line the same rain draws nothing at all, which
    # is the surfaces frame rendered above.
    def drips(name, line=None, rain=None, base=scene):
        return shot(name, base=base, rain={**RAIN_SURFACES_ONLY, **RAIN_DRIP_ARM,
                                           "drips": [line] if line else [], **(rain or {})})

    fixture_view = _cscn_camera(RAIN_FIXTURE)
    dripping = drips("dripping", RAIN_DRIP_LINE)
    col_f, _, col_t = quad_moved(dripping, wet_path, fixture_view, RAIN_DRIP_COLUMN)
    _, open_m, open_t = quad_moved(dripping, wet_path, fixture_view,
                                   ground_quad(RAIN_SSR_OPEN, 0.0))
    sky_m = -1
    if dripping and wet_path:
        w, h, pa = _read_ppm(dripping)
        _, _, pb = _read_ppm(wet_path)
        x0, y0, x1, y1 = RAIN_SKY_BOX
        sky_m, _ = _moved_in_rect(pa, pb, w, h, int(x0 * w), int(y0 * h), int(x1 * w), int(y1 * h))
    ok = (col_t >= RAIN_REGION_MIN_PX and col_f > RAIN_FEATURE_MIN and open_t >= RAIN_SSR_MIN_PX
          and open_m == 0 and sky_m == 0)
    print(f"  rain-drips {'PASS' if ok else 'FAIL'}  the line moves {col_f:.1%} of the column's "
          f"{col_t} px under it (want > {RAIN_FEATURE_MIN:.0%}), {open_m} of {open_t} px of the "
          f"open ground and {sky_m} of the sky (want 0)")
    if not ok:
        failures.append("rain-drips")

    damp_f, _, _ = quad_moved(drips("drip_damp", RAIN_DRIP_LINE, {"dryFor": RAIN_DAMP_S}),
                              drips("drip_damp_bare", rain={"dryFor": RAIN_DAMP_S}), fixture_view,
                              RAIN_DRIP_COLUMN)
    dried_ae = apart(drips("drip_dried", RAIN_DRIP_LINE, {"dryFor": RAIN_DRIED_S}),
                     frame("drip_never", no_rain, RAIN_LINEAR))
    ok = damp_f > RAIN_FEATURE_MIN and dried_ae == 0
    print(f"  rain-drips-dry {'PASS' if ok else 'FAIL'}  {RAIN_DAMP_S:g} s after the rain the "
          f"line still moves {damp_f:.1%} of its column (want > {RAIN_FEATURE_MIN:.0%}); "
          f"{RAIN_DRIED_S:g} s after, {dried_ae} px from no rain at all (want 0)")
    if not ok:
        failures.append("rain-drips-dry")

    under = drips("drip_under", {**RAIN_DRIP_LINE, "ground": 0.0}, base=deep)
    onto = drips("drip_onto", {**RAIN_DRIP_LINE, "ground": RAIN_DEEP_WATER}, base=deep)
    landed = apart(under, onto)
    shown = _moved_fraction(onto, drips("drip_flood_bare", base=deep))
    ok = landed == 0 and shown > 0.0
    print(f"  rain-drips-water {'PASS' if ok else 'FAIL'}  dripping onto the ground under "
          f"{RAIN_DEEP_WATER:g} m of water against onto its surface: {landed} px (want 0); the "
          f"drips move {shown:.2%} of the flooded frame (want > 0)")
    if not ok:
        failures.append("rain-drips-water")

    twice = apart(dripping, drips("dripping_again", RAIN_DRIP_LINE))
    ok = twice == 0
    print(f"  rain-drips-determinism {'PASS' if ok else 'FAIL'}  two runs {twice} px apart (want 0)")
    if not ok:
        failures.append("rain-drips-determinism")

    # Puddles from the ground's own lows, read in the roughness view: still water's roughness is
    # below anything a film reaches, so every pixel is puddled or not. No parallax, which the
    # engine arms for any ground with a height map and which would declare the same sampler.
    reliefed = asset(RAIN_RELIEF_FIXTURE)
    # The roughness view's green, as a byte, below which a pixel is standing water: halfway
    # between still water's roughness and the fullest film the open ground takes.
    puddle_g = 255.0 * 0.5 * (_glsl_const("RAIN_PUDDLE_ROUGHNESS", "rain_surface.glsl") +
                              _glsl_const("WET_ROUGHNESS", "wet_surface.glsl"))

    def puddled(path, bands):
        """(fraction of pixels standing water, pixels) over the rectangles inscribed in the
        images of the ground bands."""
        if not path:
            return float("nan"), 0
        w, h, pix = _read_ppm(path)
        project = _projector(_cscn_camera(RAIN_RELIEF_FIXTURE), w, h)
        wet = total = 0
        for band in bands:
            x0, y0, x1, y1 = _inscribed_rect(project, ground_quad(band, 0.0))
            for py in range(max(0, y0), min(h, y1)):
                for px in range(max(0, x0), min(w, x1)):
                    wet += pix[3 * (py * w + px) + 1] < puddle_g
                    total += 1
        return (wet / total if total else float("nan")), total

    relief_on_scene = variant("relief_on", base=reliefed, rain={"puddleRelief": 1.0})
    relief_off_scene = variant("relief_off", base=reliefed, rain={"puddleRelief": 0.0})
    relief_on = frame("relief_on", relief_on_scene, RAIN_RELIEF_FLAGS)
    relief_off = frame("relief_off", relief_off_scene, RAIN_RELIEF_FLAGS)
    trench1, trench_px = puddled(relief_on, RAIN_RELIEF_TRENCHES)
    plateau1, plateau_px = puddled(relief_on, RAIN_RELIEF_PLATEAUS)
    trench0, _ = puddled(relief_off, RAIN_RELIEF_TRENCHES)
    plateau0, _ = puddled(relief_off, RAIN_RELIEF_PLATEAUS)
    ok = (min(trench_px, plateau_px) >= RAIN_RELIEF_MIN_PX and trench1 > RAIN_RELIEF_TRENCH_MIN
          and plateau1 < RAIN_RELIEF_PLATEAU_MAX and abs(trench0 - plateau0) < RAIN_RELIEF_AGREE)
    print(f"  rain-relief {'PASS' if ok else 'FAIL'}  at relief 1 the trenches stand "
          f"{trench1:.1%} under water (want > {RAIN_RELIEF_TRENCH_MIN:.0%}) and the plateaus "
          f"{plateau1:.1%} (want < {RAIN_RELIEF_PLATEAU_MAX:.0%}); at 0, {trench0:.1%} and "
          f"{plateau0:.1%} (want within {RAIN_RELIEF_AGREE:.0%}); {trench_px} and {plateau_px} px "
          f"read (want >= {RAIN_RELIEF_MIN_PX} each)")
    if not ok:
        failures.append("rain-relief")

    # A ground with no height map has no lows to find: the knob is inert on it, and no material
    # compiles the relief bit.
    relief_bit = bits["relief"]
    flat_on = variant("flat_relief_on", rain={"puddleRelief": 1.0})
    flat_ae = apart(frame("flat_relief_on", flat_on, RAIN_LINEAR),
                    shot("flat_relief_off", rain={"puddleRelief": 0.0}))
    flat_logged = samplers(_rain_rows(flat_on)[1])
    carried = sorted(n for n, (mask, _) in flat_logged.items() if mask & relief_bit)
    ok = flat_ae == 0 and bool(flat_logged) and not carried
    print(f"  rain-relief-off {'PASS' if ok else 'FAIL'}  relief 1 against 0 on {RAIN_FIXTURE}, "
          f"whose grounds carry no height map: {flat_ae} px (want 0); variants carrying the "
          f"relief bit: {carried or 'none'} (want none)")
    if not ok:
        failures.append("rain-relief-off")

    # The relief bit costs exactly the height map's unit, and only where a material carries it.
    on_logged = samplers(_rain_rows(relief_on_scene, extra=RAIN_RELIEF_FLAGS)[1])
    off_logged = samplers(_rain_rows(relief_off_scene, extra=RAIN_RELIEF_FLAGS)[1])
    by_mask = {mask: count for mask, count in off_logged.values()}
    pairs = [(mask, count, by_mask.get(mask & ~relief_bit))
             for mask, count in on_logged.values() if mask & relief_bit]
    leaked = sorted(n for n, (mask, _) in off_logged.items() if mask & relief_bit)
    ok = (bool(pairs) and all(dry is not None and count == dry + 1 for _, count, dry in pairs)
          and not leaked)
    shown = ", ".join(f"{mask}: {count} against {dry}" for mask, count, dry in pairs) or "none"
    print(f"  rain-relief-ledger {'PASS' if ok else 'FAIL'}  relief variants against the same mask "
          f"without the bit: {shown} (want one more each); at relief 0 the bit is carried by "
          f"{leaked or 'none'} (want none)")
    if not ok:
        failures.append("rain-relief-ledger")
    return failures


# A level camera turning about rain_fixture's eye at silent's rate (specs 13.45 and 13.46): held
# TURN_STILL frames, then turned TURN_STEP degrees a frame for TURN_STEPS more. It ends with the
# lamp's lit rain and the sealed, filmed half of the ground at the leading edge, where whatever
# reads the frame before has nothing to read.
TURN_FIXTURE = RAIN_FIXTURE
# The fixture's eye-to-target distance, 15.048, rounded and kept rather than read: the render app
# derives its near clip from that distance, and every figure taken on the turn moves with it.
TURN_DIST = 15.05
TURN_YAW0 = -41.0  # degrees from -z, toward +x
TURN_STEP = 1.72   # degrees a frame: silent's 1.8 rad/s at 60 Hz
TURN_STILL = 60    # frames held before the turn, enough for the fog volume to converge
TURN_STEPS = 15
TURN_SIZE = (640, 400)


def _turn_pose(yaw_deg):
    """The fixture's eye and a target turned to `yaw_deg` about it, as --cam-eye and
    --cam-target take them."""
    eye = _cscn_camera(TURN_FIXTURE)["eye"]
    yaw = math.radians(yaw_deg)
    target = (eye[0] + TURN_DIST * math.sin(yaw), eye[1], eye[2] - TURN_DIST * math.cos(yaw))
    return ",".join(f"{c:g}" for c in eye), ",".join(f"{c:.6f}" for c in target)


def _turn_columns(n):
    """For each of `n` columns across the frame, None where its previous position on each frame
    of the turn is off the frame, else how much its height on screen grew since.

    A level camera turning about its eye keeps a direction's height and its distance in the
    horizontal plane, so a column at angle t from the view axis lay at t + step in the previous
    frame, and its height grew by cos(t) / cos(t + step). A point's depth does not enter, so this
    holds for a pixel as for a fog cell."""
    tan_h = (math.tan(math.radians(_cscn_camera(TURN_FIXTURE)["fovy_deg"]) / 2)
             * TURN_SIZE[0] / TURN_SIZE[1])
    step = math.radians(TURN_STEP)
    columns = []
    for c in range(n):
        t = math.atan((2 * (c + 0.5) / n - 1) * tan_h)
        off = abs(math.tan(t + step)) > tan_h
        columns.append(None if off else math.cos(t) / math.cos(t + step))
    return columns


def _turn_runs(workdir, prefix, args, variants):
    """Each (tag, extra, held) of `variants` rendered on the turn, or held at its last pose
    throughout; ([(codes as an array, log)], None), or (None, the first failure's log tail)."""
    yaws = [TURN_YAW0 + k * TURN_STEP for k in range(TURN_STEPS + 1)]
    runs = []
    for tag, extra, held in variants:
        out = os.path.join(workdir, f"{prefix}_{tag}.ppm")
        poses = yaws[-1:] if held else yaws
        eye, target = _turn_pose(poses[0])
        pose = ["--cam-eye", eye, "--cam-target", target]
        for k, yaw in enumerate(poses[1:]):
            pose += ["--cam-at", f"{TURN_STILL + k}:" + ",".join(_turn_pose(yaw))]
        cmd = [RENDER, "-m", asset(TURN_FIXTURE), "-x", "-f", str(TURN_STILL + TURN_STEPS),
               "-W", str(TURN_SIZE[0]), "-H", str(TURN_SIZE[1]), "-S", out]
        r = _run(cmd + args + extra + pose, capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(out):
            return None, (r.stdout + r.stderr).strip()[-300:]
        runs.append((_ppm_array(out), r.stdout + r.stderr))
    return runs, None


FOGMISS_ARGS = ["--fog", "--no-aerial", "--no-auto-exposure", "-E", "1.0", "--no-dither",
                "--no-bloom", "--no-vignette", "--no-ssao"]
FOGMISS_SPREAD = 2  # cells a history's trilinear read carries a missed cell's value past it
FOGMISS_FLOOR_MIN = 0.25  # codes: the floor a band is measured against, never below a quarter code
FOGMISS_RATIO_MAX = 1.5   # measured 0.53; 5.60 with one sample
FOGMISS_REDUCE_MAX = 0.25  # the band against one sample's: measured 0.094
FOGMISS_PRESENT_MIN = 3.0  # one sample's band against the floor, so there is one to remove: 5.60
FOGMISS_ONE = ["--fog-history-miss-samples", "1"]
# What a window and silent run: the volume is built from the jittered projection.
FOGMISS_JITTER = ["--taa", "--headless-jitter"]


def _fogmiss_grid():
    """The fog volume's dimensions, read from the engine's own header."""
    with open(os.path.join(ROOT, "cetra", "src", "postfx.h")) as f:
        text = f.read()
    return tuple(int(re.search(rf"#define POSTFX_FROXEL_{a}\s+(\d+)", text).group(1))
                 for a in "XYZ")


def _fogmiss_columns(img, ref, grid_x):
    """Each fog column's mean signed luma difference, in 8-bit codes: a band is an offset over
    the whole column, which the mean keeps and pixel noise cancels out of."""
    weights = np.array([0.2126, 0.7152, 0.0722])
    per_x = (img @ weights - ref @ weights).mean(axis=0)
    cols = np.arange(img.shape[1]) * grid_x // img.shape[1]
    return np.array([per_x[cols == c].mean() for c in range(grid_x)])


def _fogmiss_probe(log):
    """{frame: (armed, cells)} from the run's --fog-miss-probe rows."""
    return {int(r["frame"]): (int(r["armed"]), int(r["cells"]))
            for r in _probe_rows(log, "fog-miss-probe")}


def run_fog_miss_gate(workdir):
    """Fog cells without history while the camera turns (spec 13.45).

      fog-miss-band      on the turn's last frame, the columns that missed history differ from
                         the converged frame by no more than FOGMISS_RATIO_MAX of the clean
                         columns' floor, and by no more than FOGMISS_REDUCE_MAX of what they did
                         with one sample, which itself stood FOGMISS_PRESENT_MIN over the floor
      fog-miss-count     no cell took several samples before the turn, and on every frame of it
                         exactly the cells the geometry says missed did
      fog-miss-confined  against one sample, the turn's clean columns are 0 px away from the edge
                         rows a missed row's value could have reached: no cell with history was
                         touched
      fog-miss-jitter    a still camera under TAA's jitter, the projection a window builds the
                         volume from, takes several samples in no cell on any armed frame

    The capture is closed form: a level camera turning about its eye moves a cell by its column
    and its height on screen alone, so which cells missed is worked out here from the two cameras
    rather than read back from the run it checks. That a still frame is the same picture it was
    before 13.45 is the froxel_fog golden's to hold: every run here goes through the program
    that carries the branch.
    """
    names = ("fog-miss-band", "fog-miss-count", "fog-miss-confined", "fog-miss-jitter")
    if not os.path.exists(asset(TURN_FIXTURE)):
        for name in names:
            print(f"  {name} SKIP  {TURN_FIXTURE} not found")
        return []
    failures = []
    grid_x, grid_y, grid_z = _fogmiss_grid()
    columns = _turn_columns(grid_x)
    missed = [c for c, grow in enumerate(columns) if grow is None]
    kept = [grow for grow in columns if grow is not None]
    # In a column that does not miss, a cell's height on screen grew by the column's growth, which
    # takes the top and bottom rows near the leading edge out of the frame.
    missed_rows = sum(1 for grow in kept for r in range(grid_y)
                      if abs((2 * (r + 0.5) / grid_y - 1) * grow) > 1)
    # That growth is also why a cell reads its history from nearer the top or bottom than itself,
    # so a missed row's value is carried inward over the turn, and the turn carries it across the
    # screen. The edge rows are those the leading edge's growth, compounded over every frame of the
    # turn, could have reached.
    reach = max(kept + [1.0]) ** TURN_STEPS
    edge_rows = sum(1 for r in range(grid_y // 2)
                    if abs(2 * (r + 0.5) / grid_y - 1) * reach > 1) + FOGMISS_SPREAD
    swept = len(missed) * TURN_STEPS
    clean = list(range(0, max(0, grid_x - swept - FOGMISS_SPREAD)))
    probe = ["--fog-miss-probe"]
    runs, failed = _turn_runs(workdir, "fogmiss", FOGMISS_ARGS, (
        ("turn", probe, False), ("held", [], True), ("turn_one", FOGMISS_ONE, False),
        ("held_jitter", probe + FOGMISS_JITTER, True)))
    if failed is not None:
        for name in names:
            print(f"  {name} ERROR  {failed}")
            failures.append(name)
        return failures
    (turned, turn_log), (held, _), (one, _), (_, jitter_log) = runs

    d = _fogmiss_columns(turned, held, grid_x)
    d_one = _fogmiss_columns(one, held, grid_x)
    band = np.abs(d[missed]).mean()
    band_one = np.abs(d_one[missed]).mean()
    floor = max(np.abs(d[clean]).mean(), FOGMISS_FLOOR_MIN)
    ok = (bool(missed) and band / floor <= FOGMISS_RATIO_MAX
          and band_one / floor >= FOGMISS_PRESENT_MIN
          and band <= FOGMISS_REDUCE_MAX * band_one)
    print(f"  fog-miss-band {'PASS' if ok else 'FAIL'}  the {len(missed)} columns that missed "
          f"history differ by {band:.3f} codes, {band / floor:.2f}x the floor (want <= "
          f"{FOGMISS_RATIO_MAX}); one sample's {band_one:.3f}, {band_one / floor:.2f}x (want >= "
          f"{FOGMISS_PRESENT_MIN}); {band / max(band_one, 1e-9):.3f} of it (want <= "
          f"{FOGMISS_REDUCE_MAX})")
    if not ok:
        failures.append("fog-miss-band")

    want = len(missed) * grid_y * grid_z + missed_rows * grid_z
    counts = _fogmiss_probe(turn_log)
    frames = TURN_STILL + TURN_STEPS
    turning = range(TURN_STILL, frames)
    # Frame 0 has no history at all, and takes one sample; every still frame after it is armed
    # and finds nothing to take them, which is what keeps a still frame's cost and pixels its own.
    still_wrong = [f for f in range(1, TURN_STILL) if counts.get(f) != (1, 0)]
    wrong = [(f, counts.get(f)) for f in turning if counts.get(f) != (1, want)]
    ok = len(counts) == frames and counts.get(0) == (0, 0) and not still_wrong and not wrong
    print(f"  fog-miss-count {'PASS' if ok else 'FAIL'}  {len(counts)} of {frames} frames probed; "
          f"frame 0 {counts.get(0)} (want unarmed); {TURN_STILL - 1 - len(still_wrong)} of "
          f"{TURN_STILL - 1} still frames armed with no cell taking several samples; "
          f"{len(turning) - len(wrong)} of {len(turning)} turn frames took them in {want} cells "
          f"({len(missed)} columns and {missed_rows} column-rows)"
          f"{'' if not wrong else f', first wrong {wrong[0]}'}")
    if not ok:
        failures.append("fog-miss-count")

    h, w, _ = turned.shape
    y0, y1 = edge_rows * h // grid_y, (grid_y - edge_rows) * h // grid_y
    clean_px = int((turned != one).any(axis=2)[y0:y1, :len(clean) * w // grid_x].sum())
    ok = bool(clean) and y1 > y0 and clean_px == 0
    print(f"  fog-miss-confined {'PASS' if ok else 'FAIL'}  against one sample, {clean_px} px in "
          f"the {len(clean)} clean columns away from the {edge_rows} rows at each edge (want 0)")
    if not ok:
        failures.append("fog-miss-confined")

    counts = _fogmiss_probe(jitter_log)
    armed = [f for f, (a, _) in counts.items() if a]
    took = [f for f in armed if counts[f][1] != 0]
    ok = len(armed) == frames - 1 and not took
    print(f"  fog-miss-jitter {'PASS' if ok else 'FAIL'}  a still camera under TAA's jitter took "
          f"several samples in a cell on {len(took)} of {len(armed)} armed frames (want 0 of "
          f"{frames - 1})")
    if not ok:
        failures.append("fog-miss-jitter")
    return failures
