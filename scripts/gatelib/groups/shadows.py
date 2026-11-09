"""The penumbra and shadow-tiles groups:

  penumbra            area shadow (analytic penumbra)
  shadow-tiles        cached point-light shadows (tiles, views, movers; spec 13.16)
"""

import json
import re
import os
import random

from gatelib.harness import RENDER, _run, _shared_constant, asset
from gatelib.images import (
    _DISPLAY_TO_LINEAR, _SRGB_TO_LINEAR, _box_luma_dense, _linear_luma, _pixels_past, _read_ppm,
    compare,
)
from gatelib.lighting import (
    LSTREAM_FIXTURE, LSTREAM_PROP_ROOM, LSTREAM_TILE_LIGHTS, _crossing, _dir_render,
    _lstream_cam_eye, _lstream_rooms,
)
from gatelib.probes import _probe_rows
from gatelib.scenes import _cscn_camera, _projector, cscn_copy


# Area-shadow penumbra gate (spec 10.4). The fixture's geometry is chosen so the
# answer is known before rendering: a panel of half-width r at height H over an
# occluder of half-width a at height h puts the umbra edge at
# r + (a - r)*H/(H - h) and the fully-lit edge at -r + (a + r)*H/(H - h), so the
# penumbra is a band of width 2*r*h/(H - h) centred between them.
#
# This is the only image gate in the repo with an analytic answer rather than a
# stored reference, which is the whole reason it exists: a golden can only say a
# change to the shadow projection moved the picture, not that it moved it right.
#
# The four geometry numbers mirror gen_area_shadow_fixture.py and have to match
# it or the gate predicts a penumbra the scene does not cast; the camera comes
# from the scene file.
PENUMBRA = _cscn_camera(
    "area_shadow_fixture.cscn",
    panel_half=0.3, panel_h=3.0, occluder_half=0.5, occluder_h=1.0,
)
# What the CENTRE of the transition is allowed to drift, in world units. It is a
# geometric fact, so every phase must hold it; only the WIDTH is expected to move
# (it is ~0.01 with a hard 3x3 filter and should approach the analytic band once
# a source-sized penumbra lands).
PENUMBRA_CENTRE_TOL = 0.01

# cornell_leak: a lamp by the ceiling of one room and a partition between it and another. The lit
# room left of the partition and the dark one right of it, as fractions of the frame; what a light
# that respects the partition leaves in the far room against the near one, and what the same
# light leaves there unshadowed, or the partition is all that darkens it. A cached lamp leaves
# 0.0004; unshadowed it is 0.73.
LEAK_NEAR_ROOM = (0.15, 0.30, 0.45, 0.80)
LEAK_FAR_ROOM = (0.58, 0.30, 0.88, 0.80)
LEAK_DARK_MAX = 0.01
LEAK_LIT_MIN = 0.2


def _leak_far_share(img):
    """The far room's mean over the near one's, in a cornell_leak frame."""
    w, h, pix = img
    return _box_luma_dense(pix, w, h, LEAK_FAR_ROOM) / _box_luma_dense(pix, w, h, LEAK_NEAR_ROOM)


# A panel (spec 13.27). It lights its whole front hemisphere and its shadow has to cover all of
# it: cornell_leak's far room, behind the partition and above about 1 m more than 60 degrees off
# the normal of a panel by the ceiling, is what one 120-degree map left lit.
AREA_PANEL = {"name": "Panel", "type": "area", "size": [0.4, 0.4], "color": [1.0, 0.95, 0.88],
              "intensity": 30.0, "cast_shadows": True}


def _leak_panel(cache=False):
    """cornell_leak's lamp made AREA_PANEL where it hangs, facing down and with its range,
    cached or drawn every frame."""
    def mutate(d):
        lamp = d["lights"][0]
        for key in ("cone", "intensity_unit"):
            lamp.pop(key, None)
        lamp.update(AREA_PANEL, shadow_cache=cache)
    return mutate


def _shadow_twin(workdir, tag, scene, extra=()):
    """A render and its unshadowed twin: ((shadowed, bare), None) or (None, error), each one
    _tiles_render's (image, probe rows, log, path)."""
    a, err = _tiles_render(workdir, tag, scene, list(extra))
    if err:
        return None, err
    b, err = _tiles_render(workdir, f"{tag}_none", scene, list(extra) + ["--no-shadows"])
    if err:
        return None, err
    return (a, b), None


# A panel turned off every world axis, its roll off them too, against the point light at its
# centre: both test visibility from one point, so where both light they must agree.
AREA_TILT_AT = [0.0, 0.9, 0.3]
AREA_TILT_DIR = [-0.55, -0.6, -0.58]
AREA_TILT_UP = [0.3, 0.2, -0.9]
AREA_TILT_LIT = 0.02             # linear luma both unshadowed frames must pass for a pixel to count
AREA_TILT_MAX = 0.005            # of those, the share whose shadow terms differ by more than half
AREA_TILT_CONTENT_MIN = 0.05     # ...and the share in shadow in either, or nothing was compared


def _shadow_terms(shadowed, bare, lit):
    """Per pixel, a frame over its unshadowed twin -- the shadow term alone -- or None where the
    twin is darker than `lit` and the ratio says nothing."""
    w, h, pix = shadowed
    _, _, ref = bare
    d = _SRGB_TO_LINEAR
    terms = []
    for o in range(0, w * h * 3, 3):
        b = (d[ref[o]] + d[ref[o + 1]] + d[ref[o + 2]]) / 3.0
        s = (d[pix[o]] + d[pix[o + 1]] + d[pix[o + 2]]) / 3.0
        terms.append(s / b if b >= lit else None)
    return terms


def _penumbra_edges():
    p = PENUMBRA
    k = p["panel_h"] / (p["panel_h"] - p["occluder_h"])
    inner = p["panel_half"] + k * (p["occluder_half"] - p["panel_half"])
    outer = -p["panel_half"] + k * (p["occluder_half"] + p["panel_half"])
    return inner, outer


def run_penumbra_gate(workdir):
    """Area-light shadows.

      penumbra     a panel over an occluder: its shadow, over the unshadowed frame, crosses half
                   where the analytic penumbra's centre is
      area-reach   cornell_leak lit by a panel by the ceiling: the far room, behind the partition
                   and largely more than 60 degrees off the panel's normal, dark against the near
                   one; unshadowed, the same panel lights it (spec 13.27)
      area-tilt    a panel turned off every world axis, and its roll with it, shadows as a point
                   light at its centre does wherever both light (spec 13.27)

    Falsified by hand at 13.27: with the single 120-degree map back in place of the cube,
    area-reach fails at 0.0948 and area-tilt at 0.0099; with the world-axis face opposite the
    normal's dominant axis left out rather than the frame's -n, area-tilt alone fails, at 0.0250;
    with the width axis's sign flipped, both fail, at 0.2287 and 0.0196.
    """
    failures = []
    # Rendered twice, and the shadowed frame is DIVIDED by the unshadowed one. Measuring the
    # shadowed frame alone takes its "lit" reference from the brightest sample on the scan, but
    # the panel's own falloff varies across that scan, so the reference is wrong everywhere except
    # at one point. On a narrow transition that hardly matters; on a 0.3-wide band it moved the
    # apparent centre by 0.06 -- an artifact of the measurement, read as a bias in the shadow. The
    # ratio is the shadow term on its own, flat 0..1.
    shadowed = ref_frame = None
    if not os.path.exists(asset("area_shadow_fixture.gltf")):
        print("  penumbra     SKIP  (missing area_shadow_fixture.gltf)")
    else:
        shadowed = _dir_render(workdir, "area_shadow_fixture.gltf", "penumbra_shadow", [])
        ref_frame = _dir_render(workdir, "area_shadow_fixture.gltf", "penumbra_nolight",
                                ["--no-shadows"])
        if shadowed is None or ref_frame is None:
            print("  penumbra     ERROR while rendering the fixture")
            failures.append("penumbra")
    if shadowed is not None and ref_frame is not None:
        w, h, pix = shadowed
        _, _, ref = ref_frame
        project = _projector(PENUMBRA, w, h)
        inner, outer = _penumbra_edges()

        # Scan world x along +X at z=0 on the ground, well outside the band both ways.
        xs = [inner - 0.3 + i * 0.002 for i in range(int((outer - inner + 0.6) / 0.002))]
        vals = []
        for x in xs:
            px, py = project((x, 0.0, 0.0))
            lit_here = _linear_luma(ref, w, h, px, py)
            vals.append(_linear_luma(pix, w, h, px, py) / lit_here if lit_here > 1e-4 else 1.0)
        umbra, lit = min(vals), max(vals)
        if lit - umbra < 0.5:
            print(f"  penumbra     ERROR no shadow edge found (umbra {umbra:.3f}, lit {lit:.3f})")
            failures.append("penumbra")
        else:
            lo = _crossing(xs, vals, umbra, lit, 0.10)
            mid = _crossing(xs, vals, umbra, lit, 0.50)
            hi = _crossing(xs, vals, umbra, lit, 0.90)
            want_centre = 0.5 * (inner + outer)
            ok = abs(mid - want_centre) <= PENUMBRA_CENTRE_TOL
            print(f"  penumbra     {'PASS' if ok else 'FAIL'}  centre {mid:.4f} "
                  f"(want {want_centre:.4f} +/-{PENUMBRA_CENTRE_TOL}), "
                  f"10-90 width {hi - lo:.4f} (analytic {outer - inner:.4f})")
            if not ok:
                failures.append("penumbra")

    leak, point = asset("cornell_leak.cscn"), asset("cornell_point.cscn")
    if not (os.path.exists(leak) and os.path.exists(point)):
        print("  area-reach   SKIP  (cornell_leak.cscn or cornell_point.cscn missing)")
        print("  area-tilt    SKIP  (cornell_leak.cscn or cornell_point.cscn missing)")
        return failures

    def scene(tag, src, mutate):
        return cscn_copy(src, os.path.join(workdir, f"area_{tag}.cscn"), mutate)

    # --- a panel's reach -----------------------------------------------------------------------
    reach, err = _shadow_twin(workdir, "area_reach", scene("reach", leak, _leak_panel()))
    if err:
        print(f"  area-reach   ERROR render failed: {err}")
        failures.append("area-reach")
    else:
        ratio, bare = _leak_far_share(reach[0][0]), _leak_far_share(reach[1][0])
        ok = ratio <= LEAK_DARK_MAX and bare >= LEAK_LIT_MIN
        print(f"  area-reach   {'PASS' if ok else 'FAIL'}  far room {ratio:.4f} of the near one "
              f"(want <= {LEAK_DARK_MAX}), {bare:.4f} unshadowed (want >= {LEAK_LIT_MIN})")
        if not ok:
            failures.append("area-reach")

    # --- a panel off the world's axes ----------------------------------------------------------
    tilted, err = _shadow_twin(workdir, "area_tilt", scene(
        "tilt", point, lambda d: d.update(lights=[dict(
            AREA_PANEL, position=AREA_TILT_AT, direction=AREA_TILT_DIR, up=AREA_TILT_UP)])))
    lamp, err2 = (None, None) if err else _shadow_twin(workdir, "area_tilt_point", scene(
        "tilt_point", point, lambda d: d["lights"][0].update(position=AREA_TILT_AT)))
    if err or err2:
        print(f"  area-tilt    ERROR render failed: {err or err2}")
        failures.append("area-tilt")
    else:
        panel_terms = _shadow_terms(tilted[0][0], tilted[1][0], AREA_TILT_LIT)
        lamp_terms = _shadow_terms(lamp[0][0], lamp[1][0], AREA_TILT_LIT)
        both = [(a, b) for a, b in zip(panel_terms, lamp_terms) if a is not None and b is not None]
        counted = max(len(both), 1)
        apart = sum(1 for a, b in both if abs(a - b) > 0.5) / counted
        content = sum(1 for a, b in both if min(a, b) < 0.5) / counted
        ok = apart <= AREA_TILT_MAX and content >= AREA_TILT_CONTENT_MIN
        print(f"  area-tilt    {'PASS' if ok else 'FAIL'}  where both light, {apart:.4f} of the "
              f"pixels disagree on the shadow by more than half (want <= {AREA_TILT_MAX}); "
              f"{content:.4f} in shadow in either (want >= {AREA_TILT_CONTENT_MIN})")
        if not ok:
            failures.append("area-tilt")
    return failures


# Cached shadows (spec 13.16). Every fixture is mutated in memory from a committed one, so the
# cached half and its uncached twin differ in the light's flags and nothing else.
# In fog the far room keeps 0.39 of the in-scatter a per-frame map leaves there, the per-frame
# map shadowing surfaces and no haze.
TILES_FOG_MAX = 0.6
# A cached light against the per-frame map of the same light, at 8 codes: the grazing ceiling
# edge and silhouette texels; unshadowed the two differ across most of the floor.
TILES_MATCH_CODES = 8
TILES_MATCH_MAX = 0.02
TILES_MATCH_FALSIFIER_MIN = 0.1
# A cached panel against the same panel drawn every frame (spec 13.27): the near room's mean,
# where nothing stands between the panel and what it lights, within this fraction of the
# per-frame panel's. Measured 0.00016, all of it the room's creases, which a 256^2 tile and a
# 2048^2 face resolve differently.
TILES_AREA_SIZE_MAX = 0.005
# The core fixture: the reference and the kept views against the traced profile, by distance
# from the axis, and where the full shadow ends.
TILES_TRUTH_MARKS = [0.01 * c for c in range(10, 37, 2)]
TILES_TRUTH_REF_MAX = 0.05
TILES_TRUTH_KEPT_MAX = 0.08
TILES_CORE_KEPT_MAX = 0.025
TILES_CORE_ONE_MIN = 0.06
TILES_CORE_SAMPLES = 8000
# The core fixture's frame, square and orthographic: its width in metres across the wall.
TILES_CORE_ORTHO = 0.8
# The candle against the reference of its current body, region by region: 0.145 with the dance,
# 0.376 with its shift removed.
TILES_DANCE_MAX = 0.25
TILES_DANCE_CAM = ["--cam-eye", "0.95,1.45,-0.55", "--cam-target", "0.7,1.3,-1.1"]
TILES_DANCE_BOX = (0.234, 0.370, 0.599, 0.694)
# A wind strong enough to swing the tall box's hem visibly, so a face that follows the sway and a
# face held at rest are different pictures (spec 13.26).
TILES_WIND = {"direction": [1.0, 0.0, 0.0], "strength": 0.15, "speed": 3.14159,
              "gustAmount": 0.0, "turbulence": 0.35}
# The tall box's material set to sway and back: its one mesh changes how a kept face draws it
# twice.
TILES_TOGGLE_CHANGES = "2"
# tile_rounding_fixture's lamp moved in this many steps of these metres along x and z, a fraction
# of a millimetre each, and the middle of the wall it squarely faces, which its -Z face holds:
# lit at every step, against its unshadowed twin, by at least this much.
TILES_ROUNDING_STEPS = 8
TILES_ROUNDING_STEP = (0.00053, 0.00037)
TILES_ROUNDING_BOX = (0.3, 0.3, 0.7, 0.7)
TILES_ROUNDING_MIN = 0.95


def _tiles_render(workdir, tag, scene, extra, frames=30, size=("400", "300")):
    """Render, returning ((image, probe rows, log, path), None) or (None, error)."""
    out = os.path.join(workdir, f"tiles_{tag}.ppm")
    cmd = [RENDER, "-m", scene, "-x", "-f", str(frames), "-W", size[0], "-H", size[1],
           "--tiles-probe", "-S", out] + extra
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None, (r.stdout + r.stderr).strip()[-300:]
    return (_read_ppm(out), _probe_rows(r.stdout, "tiles-probe"), r.stdout + r.stderr, out), None


def _tiles_box_mean(img, box):
    w, h, pix = img
    return _box_luma_dense(pix, w, h, box)


def _match_shares(kept, per_frame, bare):
    """The shares of the frame past TILES_MATCH_CODES between a cached light's frame and its
    per-frame twin, and between that twin and the unshadowed one: how near the two shadows are,
    and how far a shadow is from none."""
    w, h, ref = per_frame
    whole = (0.0, 0.0, 1.0, 1.0)
    return (_pixels_past(kept[2], ref, w, h, whole, TILES_MATCH_CODES) / (w * h),
            _pixels_past(bare[2], ref, w, h, whole, TILES_MATCH_CODES) / (w * h))


def _tiles_region(rows):
    return next((row for row in rows if row.get("kind") == "region"), {})


def _tiles_blocks(rows):
    return [row for row in rows if "block" in row]


def _tiles_block(rows, light):
    """The probe's block for one light, {} when it holds none."""
    return next((b for b in _tiles_blocks(rows) if b.get("light") == light), {})


def _tiles_profile(img, base):
    """Visibility across the core fixture's wall by distance from the axis: the render over its
    unshadowed twin, along the four half-axes from the frame's centre. Both are read through
    --tonemap linear, so decoded with the display's own curve."""
    w, h, pix = img
    _, _, ref = base
    cx, cy = w // 2, h // 2
    metres = TILES_CORE_ORTHO / w
    prof = []
    for r in range(w // 2 - 1):
        vals = []
        for d in range(-6, 7):
            for x, y in ((cx + r, cy + d), (cx - 1 - r, cy + d), (cx + d, cy + r),
                         (cx + d, cy - 1 - r)):
                lit = _linear_luma(ref, w, h, x, y, _DISPLAY_TO_LINEAR)
                if lit > 1e-4:
                    vals.append(_linear_luma(pix, w, h, x, y, _DISPLAY_TO_LINEAR) / lit)
        prof.append(((r + 0.5) * metres, sum(vals) / max(len(vals), 1)))
    return prof


def _tiles_at(prof, m):
    return min(prof, key=lambda p: abs(p[0] - m))[1]


def _tiles_core_edge(prof):
    """Where the full shadow ends: the first radius past the rim reaching 2 per cent."""
    past = [(r, v) for r, v in prof if r > 0.02]
    return _crossing([r for r, _ in past], [v for _, v in past], 0.0, 1.0, 0.02)


def _tiles_truth():
    """The core fixture's shadow traced exactly, with no shadow map: the light's body sampled the
    way the engine defines it (u uniform along the segment, a point uniform in the ball), each
    sample's segment to the wall point tested against the rim box. Read off the generated scene
    and model, so the geometry has one statement."""
    with open(asset("tile_core_fixture.cscn")) as fh:
        light = json.load(fh)["lights"][0]
    with open(asset("tile_core_fixture.gltf")) as fh:
        nodes = {n["name"]: n for n in json.load(fh)["nodes"]}
    rim, wall = nodes["rim"], nodes["wall"]
    centre, axis = light["position"], light["direction"]
    radius, length = light["source_radius"], light["source_length"]
    lo = [rim["translation"][i] - 0.5 * rim["scale"][i] for i in range(3)]
    hi = [rim["translation"][i] + 0.5 * rim["scale"][i] for i in range(3)]
    wall_z = wall["translation"][2] + 0.5 * wall["scale"][2]

    def blocked(a, b):
        t0, t1 = 0.0, 1.0
        for i in range(3):
            d = b[i] - a[i]
            if abs(d) < 1e-12:
                if a[i] < lo[i] or a[i] > hi[i]:
                    return False
                continue
            u0, u1 = (lo[i] - a[i]) / d, (hi[i] - a[i]) / d
            t0, t1 = max(t0, min(u0, u1)), min(t1, max(u0, u1))
            if t0 > t1:
                return False
        return True

    rng = random.Random(1)
    samples = []
    while len(samples) < TILES_CORE_SAMPLES:
        p = [rng.uniform(-1, 1) for _ in range(3)]
        if sum(c * c for c in p) > 1.0:
            continue
        along = (rng.random() - 0.5) * length
        samples.append(tuple(centre[i] + axis[i] * along + radius * p[i] for i in range(3)))
    prof = []
    r = 0.0
    while r <= 0.40:
        at = (r, centre[1], wall_z)
        prof.append((r, sum(0 if blocked(at, s) else 1 for s in samples) / len(samples)))
        r += 0.0025
    return prof


def run_shadow_tiles_gate(workdir):
    """Cached shadows: tiles kept across frames, views over a light's body, movers, panels.

      tiles-off      a light flagged shadow_cache that casts no shadow takes no tiles and
                     renders 0 px against the same light without the flag
      tiles-leak     cornell_leak's lamp cached darkens the room behind its partition; the
                     same lamp unshadowed lights it
      tiles-fog      ...and the haze behind the partition loses the in-scatter a per-frame map,
                     which shadows surfaces and no haze, leaves there
      tiles-match    a cached light with no body against the per-frame map of the same light;
                     unshadowed the two are far apart
      tiles-fresh    the kept faces at frame 30 equal faces redrawn every frame
      tiles-draws    every face is drawn once, and none again on a still scene
      tiles-follow   ...and a light set to follow, standing still, draws none again either, and is
                     the same picture as the light kept
      tiles-rounding a wall squarely facing a light with no body half a metre off, 50 m from the
                     origin, stays lit wherever the light is moved by a fraction of a millimetre
      tiles-area    a panel cached, from its centre over six faces, against the same panel's
                     per-frame cube; unshadowed the two are far apart (spec 13.27)
      tiles-area-size ...and where nothing stands between it and the room, the cached panel
                     lights the room as the per-frame one does, its size where the LTC reads it
      tiles-views    a light with a body is drawn from eight views and one without from one
      tiles-truth    on the core fixture, the reference against a trace of the same body
                     against the rim with no shadow map, and the kept views against the trace
      tiles-core     the kept views' full shadow ends where the trace's does; one view's ends
                     far outside it
      tiles-movers   a box swinging under a cached lamp is drawn over a copy of the still
                     casters each frame: at its rest frame the result is 0 px from the scene
                     never moved, mid-swing it is not, and no kept face is drawn
      tiles-rest     a box swaying as cloth, kept at rest by its material, is held in the kept
                     faces at rest: no face drawn over a copy, and the faces equal the box with
                     no wind response; left to sway, faces are drawn over a copy
      tiles-windless a material with a wind response under no wind moves nothing, so draws no
                     face over a copy; under a wind the same box does
      tiles-rest-toggle the material set to sway and back with the graph unchanged: the kept
                     faces end as the faces that never swayed, the box drawn again where it
                     stands at each change and no face of the region lost
      tiles-graph-near a node taken out of the graph under the cached lamp draws again some of
                     its faces and not all, loses no face of the region, and is the frame of
                     the node removed at load (spec 13.38)
      tiles-graph-far ...and out of every cached light's reach, draws none
      tiles-graph-back ...and put back, is the frame of the node never taken out
      tiles-graph-replace a node freed and a new one hung in its place, moved, with its mesh --
                     the new node may take the old one's address -- is the frame of every face
                     drawn every frame
      tiles-budget   cached lights past the tile budget are refused by name
      tiles-store    with sixteen bodied lights holding the whole budget, a moving caster's faces
                     are drawn over copies kept in the store pool on top of it; with no pool
                     they are drawn whole, the refusal names the nearest light, and the two
                     pictures are the same
      tiles-shape    a flame's light carries its spine: radius, length between the caps, axis
      tiles-dance    the candle's kept views, drawn at its first frame, read near the reference
                     of its body at frame 120

    Falsified by hand at 13.16: tiles-truth and tiles-core fail on the march this spec built
    first (full shadow to 18.6 cm against 12.5 traced); tiles-dance fails with the dance's shift
    removed (0.376 against 0.145); tiles-movers fails if the overlay is skipped, since the store
    leaves the box out of the copy. At 13.26: tiles-rest fails with the rest material drawn
    swaying in the kept faces, tiles-windless with a material's response alone deciding that it
    sways, tiles-rest-toggle with the kept faces not told of a material change, and tiles-store
    with the pool carved out of the lights' budget. At 13.27: tiles-area fails with the area
    branch never reading the tiles (0.3545, the unshadowed share), and tiles-area-size with the
    near plane packed over the panel's size again (the near room 41-fold darker). At 13.31:
    tiles-follow fails with following an unconditional redraw (6 faces drawn on frame 30), and
    tiles-rounding with no rounding allowance (the wall's middle 0.000 of its twin). At 13.38:
    with a graph change drawing every face again, as it did, tiles-graph-near and tiles-graph-far
    each draw 6 of 6 faces, and every graph arm reads generation 2.
    """
    point = asset("cornell_point.cscn")
    core = asset("tile_core_fixture.cscn")
    fire = asset("fire_fixture.cscn")
    leak = asset("cornell_leak.cscn")
    rounding = asset("tile_rounding_fixture.cscn")
    if not all(os.path.exists(p) for p in (RENDER, point, core, fire, leak, rounding)):
        print("  tiles-off    SKIP  (render or a tiles fixture not present)")
        return []
    failures = []

    def failed(arm, err):
        print(f"  {arm:<12} ERROR render failed: {err}")
        failures.append(arm)

    def lamp(mutation):
        def mutate(d):
            mutation(d["lights"][0])
        return mutate

    def scene(tag, src, mutate):
        return cscn_copy(src, os.path.join(workdir, f"tiles_{tag}.cscn"), mutate)

    # --- off ---------------------------------------------------------------------------------
    off_flag = scene("off_flag", point, lamp(lambda l: l.update(cast_shadows=False,
                                                                 shadow_cache=True)))
    off_none = scene("off_none", point, lamp(lambda l: l.update(cast_shadows=False)))
    a, err = _tiles_render(workdir, "off_flag", off_flag, [])
    b, err2 = (None, None) if err else _tiles_render(workdir, "off_none", off_none, [])
    if err or err2:
        failed("tiles-off", err or err2)
    else:
        blocks = _tiles_region(a[1]).get("blocks", "?")
        differ = compare(a[3], b[3])[0]
        ok = blocks == "0" and differ == 0
        print(f"  tiles-off    {'PASS' if ok else 'FAIL'}  blocks {blocks} (want 0) and {differ} "
              f"px against the same light unflagged (want 0)")
        if not ok:
            failures.append("tiles-off")

    # --- leak and fog ------------------------------------------------------------------------
    # cornell_leak's spot made a point light, which a cache takes, with the same place and power.
    def leak_point(cache):
        def mutate(light):
            light.pop("direction", None)
            light.pop("cone", None)
            light.update(type="point", shadow_cache=cache)
        return lamp(mutate)

    cached = scene("leak_cached", leak, leak_point(True))
    frame = scene("leak_frame", leak, leak_point(False))
    lt, err = _shadow_twin(workdir, "leak_cached", cached)
    if err:
        failed("tiles-leak", err)
    else:
        ratio, bare = _leak_far_share(lt[0][0]), _leak_far_share(lt[1][0])
        ok = ratio <= LEAK_DARK_MAX and bare >= LEAK_LIT_MIN
        print(f"  tiles-leak   {'PASS' if ok else 'FAIL'}  far room {ratio:.4f} of the near one "
              f"cached (want <= {LEAK_DARK_MAX}), {bare:.4f} unshadowed (want >= "
              f"{LEAK_LIT_MIN})")
        if not ok:
            failures.append("tiles-leak")
    fog = ["--fog", "--fog-density", "0.3"]
    fc, err = _tiles_render(workdir, "fog_cached", cached, fog, frames=60)
    ff, err2 = _tiles_render(workdir, "fog_frame", frame, fog, frames=60)
    if err or err2:
        failed("tiles-fog", err or err2)
    else:
        kept = _tiles_box_mean(fc[0], LEAK_FAR_ROOM)
        per_frame = _tiles_box_mean(ff[0], LEAK_FAR_ROOM)
        ok = kept <= TILES_FOG_MAX * per_frame
        print(f"  tiles-fog    {'PASS' if ok else 'FAIL'}  the far room's haze {kept:.4f} cached "
              f"against {per_frame:.4f} with a per-frame map (want at most {TILES_FOG_MAX} of it)")
        if not ok:
            failures.append("tiles-fog")

    # --- match, fresh, draws, views ----------------------------------------------------------
    point_cached = scene("point_cached", point, lamp(lambda l: l.update(shadow_cache=True)))
    # The lamp's kept faces too, which are read at exit and change nothing in the frame.
    point_map = os.path.join(workdir, "tiles_point_kept_map.ppm")
    pk, err = _tiles_render(workdir, "point_kept", point_cached,
                            ["--tile-map", "PointLamp", point_map])
    pf, err2 = _tiles_render(workdir, "point_frame", point, [])
    pn, err3 = _tiles_render(workdir, "point_none", point, ["--no-shadows"])
    pr, err4 = _tiles_render(workdir, "point_refresh", point_cached, ["--tiles-refresh"])
    p1, err5 = _tiles_render(workdir, "point_first", point_cached, [], frames=1)
    if err or err2 or err3:
        failed("tiles-match", err or err2 or err3)
    else:
        near, far = _match_shares(pk[0], pf[0], pn[0])
        ok = near <= TILES_MATCH_MAX and far >= TILES_MATCH_FALSIFIER_MIN
        print(f"  tiles-match  {'PASS' if ok else 'FAIL'}  {near:.4f} of the frame past "
              f"{TILES_MATCH_CODES} codes "
              f"from the per-frame map (want <= {TILES_MATCH_MAX}); unshadowed {far:.4f} (want "
              f">= {TILES_MATCH_FALSIFIER_MIN})")
        if not ok:
            failures.append("tiles-match")
    if err or err4:
        failed("tiles-fresh", err or err4)
    else:
        differ = compare(pk[3], pr[3])[0]
        ok = differ == 0
        print(f"  tiles-fresh  {'PASS' if ok else 'FAIL'}  {differ} px between faces kept from "
              f"frame 1 and faces redrawn every frame (want 0)")
        if not ok:
            failures.append("tiles-fresh")
    if err or err5:
        failed("tiles-draws", err or err5)
    else:
        first = _tiles_region(p1[1]).get("faces_drawn", "?")
        later = _tiles_region(pk[1]).get("faces_drawn", "?")
        ok = later == "0" and first not in ("0", "?")
        print(f"  tiles-draws  {'PASS' if ok else 'FAIL'}  {first} faces drawn on frame 1, "
              f"{later} on frame 30 (want some, then 0)")
        if not ok:
            failures.append("tiles-draws")

    # --- follow and rounding (spec 13.31) ----------------------------------------------------
    follow = scene("point_follow", point,
                   lamp(lambda l: l.update(shadow_cache=True, shadow_follow=True)))
    pw, err6 = _tiles_render(workdir, "point_follow", follow, [])
    if err or err6:
        failed("tiles-follow", err or err6)
    else:
        later = _tiles_region(pw[1]).get("faces_drawn", "?")
        differ = compare(pk[3], pw[3])[0]
        ok = later == "0" and differ == 0
        print(f"  tiles-follow {'PASS' if ok else 'FAIL'}  a still light that follows: {later} "
              f"faces drawn on frame 30 (want 0), {differ} px from the same light kept (want 0)")
        if not ok:
            failures.append("tiles-follow")

    def nudged(k):
        def mutate(d):
            at = d["lights"][0]["position"]
            at[0] += k * TILES_ROUNDING_STEP[0]
            at[2] += k * TILES_ROUNDING_STEP[1]
        return mutate

    far = ["--no-recenter"]
    rb, err = _tiles_render(workdir, "rounding_bare", rounding, far + ["--no-shadows"], frames=3)
    shares = []
    for k in range(TILES_ROUNDING_STEPS if not err else 0):
        rk, err = _tiles_render(workdir, f"rounding_{k}",
                                scene(f"rounding_{k}", rounding, nudged(k)), far, frames=3)
        if err:
            break
        shares.append(_tiles_box_mean(rk[0], TILES_ROUNDING_BOX) /
                      _tiles_box_mean(rb[0], TILES_ROUNDING_BOX))
    if err:
        failed("tiles-rounding", err)
    else:
        worst = min(shares)
        ok = worst >= TILES_ROUNDING_MIN
        print(f"  tiles-rounding {'PASS' if ok else 'FAIL'}  the wall's middle over its unshadowed "
              f"twin at {len(shares)} light positions: least {worst:.3f} (want >= "
              f"{TILES_ROUNDING_MIN})")
        if not ok:
            failures.append("tiles-rounding")

    # --- a panel -----------------------------------------------------------------------------
    ac, err = _tiles_render(workdir, "area_cached",
                            scene("area_cached", leak, _leak_panel(cache=True)), [])
    at, err2 = (None, None) if err else _shadow_twin(
        workdir, "area_frame", scene("area_frame", leak, _leak_panel()))
    if err or err2:
        failed("tiles-area", err or err2)
        failed("tiles-area-size", err or err2)
    else:
        af, an = at
        near, far = _match_shares(ac[0], af[0], an[0])
        block = _tiles_block(ac[1], AREA_PANEL["name"])
        shape = (block.get("views"), block.get("cells"))
        ok = near <= TILES_MATCH_MAX and far >= TILES_MATCH_FALSIFIER_MIN and shape == ("1", "6")
        print(f"  tiles-area   {'PASS' if ok else 'FAIL'}  {near:.4f} of the frame past "
              f"{TILES_MATCH_CODES} codes from the panel drawn every frame (want <= "
              f"{TILES_MATCH_MAX}); unshadowed {far:.4f} (want >= {TILES_MATCH_FALSIFIER_MIN}); "
              f"(views, cells) {shape} (want ('1', '6'))")
        if not ok:
            failures.append("tiles-area")
        kept = _tiles_box_mean(ac[0], LEAK_NEAR_ROOM)
        per_frame = _tiles_box_mean(af[0], LEAK_NEAR_ROOM)
        apart = abs(kept - per_frame) / per_frame
        ok = apart <= TILES_AREA_SIZE_MAX
        print(f"  tiles-area-size {'PASS' if ok else 'FAIL'}  the near room's mean {kept:.5f} "
              f"cached against {per_frame:.5f} drawn every frame, {apart:.5f} apart (want <= "
              f"{TILES_AREA_SIZE_MAX}: nothing stands between the panel and that room)")
        if not ok:
            failures.append("tiles-area-size")

    # --- the core fixture: views, truth, core ------------------------------------------------
    ortho = ["--ortho", str(TILES_CORE_ORTHO), "--tonemap", "linear", "--no-bloom", "--no-ssao",
             "--no-ssr", "--no-vignette", "--no-dither"]
    square = ("400", "400")
    cn, err = _tiles_render(workdir, "core_none", core, ortho + ["--no-shadows"], size=square)
    cr, err2 = _tiles_render(workdir, "core_ref", core, ortho + ["--tile-reference", "32"],
                             size=square)
    taa = ["--taa", "--headless-jitter"]
    ct, err3 = _tiles_render(workdir, "core_kept", core, ortho + taa, frames=64, size=square)
    cnt, err4 = _tiles_render(workdir, "core_none_taa", core, ortho + taa + ["--no-shadows"],
                              frames=64, size=square)
    c1, err5 = _tiles_render(workdir, "core_one", core, ortho + ["--tile-views", "1"],
                             size=square)
    if ct is None or pk is None:
        failed("tiles-views", err3 or "cornell_point's cached render")
    else:
        body = _tiles_blocks(ct[1])
        flat = _tiles_blocks(pk[1])
        got = [(b.get("views"), b.get("cells")) for b in body + flat]
        views = int(_shared_constant("shadow_tile_constants.glsl", "SHADOW_TILE_VIEWS"))
        want = (str(views), str(6 * views))
        ok = bool(body and flat and (body[0].get("views"), body[0].get("cells")) == want
                  and (flat[0].get("views"), flat[0].get("cells")) == ("1", "6"))
        print(f"  tiles-views  {'PASS' if ok else 'FAIL'}  (views, cells) {got} (want {want} for "
              f"the flame, ('1', '6') for cornell_point's lamp)")
        if not ok:
            failures.append("tiles-views")
    if err or err2 or err3 or err4 or err5:
        failed("tiles-truth", err or err2 or err3 or err4 or err5)
        failed("tiles-core", err or err2 or err3 or err4 or err5)
    else:
        truth = _tiles_truth()
        ref = _tiles_profile(cr[0], cn[0])
        kept = _tiles_profile(ct[0], cnt[0])
        one = _tiles_profile(c1[0], cn[0])
        ref_err = max(abs(_tiles_at(ref, m) - _tiles_at(truth, m)) for m in TILES_TRUTH_MARKS)
        kept_err = max(abs(_tiles_at(kept, m) - _tiles_at(truth, m)) for m in TILES_TRUTH_MARKS)
        ok = ref_err <= TILES_TRUTH_REF_MAX and kept_err <= TILES_TRUTH_KEPT_MAX
        print(f"  tiles-truth  {'PASS' if ok else 'FAIL'}  largest gap to the trace over 10-36 "
              f"cm: reference {ref_err:.3f} (want <= {TILES_TRUTH_REF_MAX}), kept views under "
              f"TAA {kept_err:.3f} (want <= {TILES_TRUTH_KEPT_MAX})")
        if not ok:
            failures.append("tiles-truth")
        edge_t, edge_k, edge_1 = (_tiles_core_edge(p) for p in (truth, kept, one))
        ok = (abs(edge_k - edge_t) <= TILES_CORE_KEPT_MAX
              and abs(edge_1 - edge_t) >= TILES_CORE_ONE_MIN)
        print(f"  tiles-core   {'PASS' if ok else 'FAIL'}  full shadow ends at {100 * edge_k:.1f} "
              f"cm kept, {100 * edge_t:.1f} traced (want within {100 * TILES_CORE_KEPT_MAX:.1f}); "
              f"one view {100 * edge_1:.1f} (want {100 * TILES_CORE_ONE_MIN:.0f} or more away)")
        if not ok:
            failures.append("tiles-core")

    # --- movers ------------------------------------------------------------------------------
    swing = ["--node-swing", "cornell_short_box", "0.1"]
    swung, err = _tiles_render(workdir, "swing_rest", point_cached, swing, frames=30)
    mid, err2 = _tiles_render(workdir, "swing_mid", point_cached, swing, frames=15)
    if err or err2 or pk is None:
        failed("tiles-movers", err or err2 or "cornell_point's cached render")
    else:
        region = _tiles_region(swung[1])
        still = compare(swung[3], pk[3])[0]
        moved = compare(mid[3], pk[3])[0]
        ok = (still == 0 and moved > 100 and region.get("movers") == "1"
              and region.get("faces_drawn") == "0" and region.get("mover_faces_drawn") != "0")
        print(f"  tiles-movers {'PASS' if ok else 'FAIL'}  at the swing's rest {still} px from "
              f"the box never moved (want 0), mid-swing {moved} (want > 100); movers "
              f"{region.get('movers')}, kept faces drawn {region.get('faces_drawn')}, faces "
              f"drawn over a copy {region.get('mover_faces_drawn')}")
        if not ok:
            failures.append("tiles-movers")

    # --- swaying casters (spec 13.26) --------------------------------------------------------
    # The tall box swaying as cloth under the cached lamp -- cloth because the box carries no
    # UV1, which the vegetation modes read. Kept at rest by its material it is held in the kept
    # faces as an unmoving caster is; left to sway, every face it reaches is redrawn every frame.
    def swaying(wind=True, how=None):
        def mutate(d):
            if wind:
                d["wind"] = dict(TILES_WIND)
            material = {"windResponse": 1.0, "windMode": "cloth"}
            if how:
                material["cachedShadowWind"] = how
            d["materials"] = {"cornell_tall_box": material}
        return mutate

    def kept_map(tag, src, extra=()):
        path = os.path.join(workdir, f"tiles_{tag}_map.ppm")
        run, err = _tiles_render(workdir, tag, src, ["--tile-map", "PointLamp", path] + list(extra))
        if not err and not os.path.exists(path):
            err = "no tile map written"
        return run, path, err

    rest_scene = scene("wind_rest", point_cached, swaying(how="rest"))
    rest_run, rest_map, rest_err = kept_map("wind_rest", rest_scene)
    sway, sway_err = _tiles_render(workdir, "wind_sway", scene("wind_sway", point_cached,
                                                               swaying()), [])
    swayed = None if sway_err else _tiles_region(sway[1]).get("mover_faces_drawn")
    if rest_err or sway_err or pk is None:
        failed("tiles-rest", rest_err or sway_err or "cornell_point's cached render")
    else:
        drawn = _tiles_region(rest_run[1]).get("mover_faces_drawn")
        dynamic = _tiles_block(rest_run[1], "PointLamp").get("dynamic")
        held = compare(rest_map, point_map)[0]
        ok = drawn == "0" and dynamic == "0" and held == 0 and swayed not in (None, "0")
        print(f"  tiles-rest   {'PASS' if ok else 'FAIL'}  a swaying box kept at rest: {drawn} "
              f"faces drawn over a copy and {dynamic} dynamic (want 0 and 0), its kept faces "
              f"{held} px from the box with no wind response (want 0); left to sway, {swayed} "
              f"faces drawn over a copy (want some)")
        if not ok:
            failures.append("tiles-rest")

    windless, err = _tiles_render(workdir, "windless", scene("windless", point_cached,
                                                              swaying(wind=False)), [])
    if err or sway_err:
        failed("tiles-windless", err or sway_err)
    else:
        drawn = _tiles_region(windless[1]).get("mover_faces_drawn")
        ok = drawn == "0" and swayed not in (None, "0")
        print(f"  tiles-windless {'PASS' if ok else 'FAIL'}  a material with a wind response in a "
              f"scene with no wind: {drawn} faces drawn over a copy (want 0, since nothing sways); "
              f"under a wind, {swayed} (want some)")
        if not ok:
            failures.append("tiles-windless")

    # Set to sway and back with nothing in the graph changing: the faces must hold the box at rest
    # again, rather than a store copy that left it out -- by drawing the box's own faces again,
    # each time, and not every face of every light.
    toggle = ["--material-at", "10", "cornell_tall_box", "cachedShadowWind", "sway",
              "--material-at", "20", "cornell_tall_box", "cachedShadowWind", "rest"]
    toggled, toggled_map, err = kept_map("wind_toggle", rest_scene, toggle)
    if err or rest_err:
        failed("tiles-rest-toggle", err or rest_err)
    else:
        differ = compare(toggled_map, rest_map)[0]
        region, plain = _tiles_region(toggled[1]), _tiles_region(rest_run[1])
        changes = region.get("kept_changes")
        same = region.get("generation") == plain.get("generation")
        ok = differ == 0 and changes == TILES_TOGGLE_CHANGES and same
        print(f"  tiles-rest-toggle {'PASS' if ok else 'FAIL'}  swayed from frame 10 and kept at "
              f"rest again from 20: the kept faces {differ} px from never having swayed (want 0); "
              f"{changes} casters redrawn where they stand (want {TILES_TOGGLE_CHANGES}), every "
              f"face kept {'through it' if same else 'NOT through it'}")
        if not ok:
            failures.append("tiles-rest-toggle")

    # --- graph changes (spec 13.38) ----------------------------------------------------------
    # A node taken out of the graph and put back, and one freed with a new one hung in its place,
    # under the cached lamp: the faces it was in or is in are drawn again and no others, never the
    # whole region, and every frame is the frame its own reference gives.
    plain_gen = None if pk is None else _tiles_region(pk[1]).get("generation")
    near, err = _tiles_render(workdir, "graph_out", point_cached,
                              ["--graph-churn", "cornell_short_box", "10", "0"], frames=10)
    gone, err2 = _tiles_render(workdir, "graph_gone", point_cached,
                               ["--remove-node", "cornell_short_box"], frames=10)
    if err or err2 or pk is None:
        failed("tiles-graph-near", err or err2 or "cornell_point's cached render")
    else:
        region = _tiles_region(near[1])
        drawn, cells = region.get("faces_drawn", "?"), region.get("cells", "?")
        differ = compare(near[3], gone[3])[0]
        some = drawn.isdigit() and cells.isdigit() and 0 < int(drawn) < int(cells)
        ok = some and region.get("generation") == plain_gen and differ == 0
        print(f"  tiles-graph-near {'PASS' if ok else 'FAIL'}  the short box taken out on frame "
              f"10: {drawn} of the lamp's {cells} faces drawn again (want some, not all), the "
              f"region's generation {region.get('generation')} (want {plain_gen}: no face lost), "
              f"{differ} px from the box removed at load (want 0)")
        if not ok:
            failures.append("tiles-graph-near")

    far_lamp = scene("graph_far", point, lamp(lambda l: l.update(
        shadow_cache=True, position=[-0.8, 1.8, 0.8], range=0.3)))
    far, err = _tiles_render(workdir, "graph_far", far_lamp,
                             ["--graph-churn", "cornell_short_box", "10", "0"], frames=10)
    if err:
        failed("tiles-graph-far", err)
    else:
        region = _tiles_region(far[1])
        ok = region.get("faces_drawn") == "0" and region.get("generation") == "0"
        print(f"  tiles-graph-far {'PASS' if ok else 'FAIL'}  the short box taken out of a "
              f"graph whose only cached lamp cannot reach it: {region.get('faces_drawn')} faces "
              f"drawn again (want 0), generation {region.get('generation')} (want 0)")
        if not ok:
            failures.append("tiles-graph-far")

    back, err = _tiles_render(workdir, "graph_back", point_cached,
                              ["--graph-churn", "cornell_short_box", "10", "20"], frames=30)
    if err or pk is None:
        failed("tiles-graph-back", err or "cornell_point's cached render")
    else:
        region = _tiles_region(back[1])
        differ = compare(back[3], pk[3])[0]
        ok = differ == 0 and region.get("generation") == plain_gen
        print(f"  tiles-graph-back {'PASS' if ok else 'FAIL'}  the box out on frame 10 and back on "
              f"20: {differ} px from never taken out (want 0), generation "
              f"{region.get('generation')} (want {plain_gen})")
        if not ok:
            failures.append("tiles-graph-back")

    replace = ["--graph-replace", "cornell_short_box", "10", "0.25,0,0.1"]
    swapped, err = _tiles_render(workdir, "graph_replace", point_cached, replace, frames=20)
    swapped_ref, err2 = _tiles_render(workdir, "graph_replace_ref", point_cached,
                                      replace + ["--tiles-refresh"], frames=20)
    if err or err2 or pk is None:
        failed("tiles-graph-replace", err or err2 or "cornell_point's cached render")
    else:
        region = _tiles_region(swapped[1])
        differ = compare(swapped[3], swapped_ref[3])[0]
        moved = compare(swapped[3], pk[3])[0]
        ok = differ == 0 and moved > 100 and region.get("generation") == plain_gen
        print(f"  tiles-graph-replace {'PASS' if ok else 'FAIL'}  the box freed on frame 10 and a "
              f"new node hung 0.27 m over with its mesh: {differ} px from every face drawn every "
              f"frame (want 0), {moved} from the box never moved (want > 100), generation "
              f"{region.get('generation')} (want {plain_gen})")
        if not ok:
            failures.append("tiles-graph-replace")

    # --- budget ------------------------------------------------------------------------------
    # Three lights at the reference's 64 views ask 384 cells each, which the budget holds two of;
    # a scene file stops at sixteen lights, which at eight views is the budget exactly.
    def crowd(d):
        light = d["lights"][0]
        d["lights"] = [dict(light, name=f"flame_{i}", position=[0.2 * i, 1.6, 0.42])
                       for i in range(3)]

    crowded = scene("crowd", core, crowd)
    cw, err = _tiles_render(workdir, "crowd", crowded, ["--tile-reference", "64"], frames=3)
    if err:
        failed("tiles-budget", err)
    else:
        refused = "Cached shadow tiles full" in cw[2]
        held = len(_tiles_blocks(cw[1]))
        ok = refused and held < 3
        print(f"  tiles-budget {'PASS' if ok else 'FAIL'}  three lights at 64 views: {held} held a "
              f"block, refusal {'logged' if refused else 'not logged'} (want fewer than three, "
              f"and logged)")
        if not ok:
            failures.append("tiles-budget")

    # --- the store pool (spec 13.26) ---------------------------------------------------------
    # stream_rooms' room 3: sixteen bodied lights hold the whole budget and a skinned prop moves
    # every frame. The faces it reaches keep their still casters in the pool, on top of the
    # budget, and drawn whole with no pool they are the same picture.
    rooms, _ = _lstream_rooms()
    look = _lstream_cam_eye(rooms[LSTREAM_PROP_ROOM])
    pooled, err = _tiles_render(workdir, "store_pool", asset(LSTREAM_FIXTURE), look)
    unpooled, err2 = _tiles_render(workdir, "store_none", asset(LSTREAM_FIXTURE),
                                   look + ["--tile-stores", "0"])
    if err or err2:
        failed("tiles-store", err or err2)
    else:
        region, bare = _tiles_region(pooled[1]), _tiles_region(unpooled[1])
        cells = [b.get("cells") for b in _tiles_blocks(pooled[1])]
        block = str(6 * int(_shared_constant("shadow_tile_constants.glsl", "SHADOW_TILE_VIEWS")))
        room = f"Room{LSTREAM_PROP_ROOM}"
        copied, whole = int(region.get("mover_faces_copied", 0)), region.get("mover_faces_whole")
        refused = re.search(r"Cached shadow stores full \(0 faces\): '(\w+)'", unpooled[2])
        differ = compare(pooled[3], unpooled[3])[0]
        ok = (cells == [block] * LSTREAM_TILE_LIGHTS and region.get("store_cells") == "64"
              and copied > 0 and whole == "0" and bare.get("mover_faces_copied") == "0"
              and int(bare.get("mover_faces_whole", 0)) > 0
              and refused is not None and refused.group(1).startswith(room) and differ == 0)
        print(f"  tiles-store  {'PASS' if ok else 'FAIL'}  {len(cells)} lights at {set(cells)} "
              f"cells (want {LSTREAM_TILE_LIGHTS} at {block}, the whole budget); pool "
              f"{region.get('store_cells')} cells (want 64), {copied} faces drawn over a copy and "
              f"{whole} whole (want some and 0); with no pool {bare.get('mover_faces_whole')} "
              f"whole, refusal naming {refused.group(1) if refused else 'nobody'} (want a "
              f"{room} light); the two pictures {differ} px apart (want 0)")
        if not ok:
            failures.append("tiles-store")

    # --- the flame ---------------------------------------------------------------------------
    fire_cached = scene("fire", fire, lambda d: next(
        l for l in d["lights"] if l["name"] == "candle_light").update(cast_shadows=True,
                                                                       shadow_cache=True))
    fs, err = _tiles_render(workdir, "fire_shape", fire_cached, [], frames=60)
    if err:
        failed("tiles-shape", err)
    else:
        flame = _tiles_block(fs[1], "candle_light")
        try:
            radius, length = float(flame["radius"]), float(flame["length"])
            up = float(flame["axis"].split(",")[1])
            ok = 0.004 <= radius <= 0.009 and 0.015 <= length <= 0.035 and up >= 0.94
            text = (f"radius {1000 * radius:.1f} mm (want 4-9), length between the caps "
                    f"{100 * length:.2f} cm (want 1.5-3.5), axis {up:.3f} upright (want >= "
                    f"0.94, 20 degrees)")
        except (KeyError, ValueError, IndexError):
            ok, text = False, "the probe printed no candle_light block"
        print(f"  tiles-shape  {'PASS' if ok else 'FAIL'}  {text}")
        if not ok:
            failures.append("tiles-shape")
    fd, err = _tiles_render(workdir, "dance_kept", fire_cached, TILES_DANCE_CAM + taa,
                            frames=120)
    fr, err2 = _tiles_render(workdir, "dance_ref", fire_cached,
                             TILES_DANCE_CAM + taa + ["--tile-reference", "8"], frames=120)
    if err or err2:
        failed("tiles-dance", err or err2)
    else:
        fx0, fy0, fx1, fy1 = TILES_DANCE_BOX
        worst = 0.0
        for r in range(4):
            for c in range(8):
                box = (fx0 + (fx1 - fx0) * c / 8, fy0 + (fy1 - fy0) * r / 4,
                       fx0 + (fx1 - fx0) * (c + 1) / 8, fy0 + (fy1 - fy0) * (r + 1) / 4)
                ref_mean = _tiles_box_mean(fr[0], box)
                if ref_mean > 1e-4:
                    worst = max(worst, abs(_tiles_box_mean(fd[0], box) / ref_mean - 1.0))
        ok = worst <= TILES_DANCE_MAX
        print(f"  tiles-dance  {'PASS' if ok else 'FAIL'}  largest region gap to the reference of "
              f"the flame as it is at frame 120: {worst:.3f} (want <= {TILES_DANCE_MAX}; 0.376 "
              f"with the dance removed)")
        if not ok:
            failures.append("tiles-dance")

    return failures
