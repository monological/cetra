"""The terrain and terrain-stream groups:

  terrain             Heightfield terrain and erosion (spec 11.59 / D6-D8)
  terrain-stream      terrain streaming (spec 11.69 / D4)
"""

import math
import struct
import re
import os

from gatelib.harness import FOREST, _run, asset
from gatelib.world import REGION_CHURN, REGION_WALK, _ORIGIN_TRACE, _region_run


# CROSSOVER_SEPARATION is deleted rather than retuned. It required the effect to
# beat three times a MEASURED floor, and there is no multiplier that works: the
# floor is a sample, so the bar it produced was a random variable: 0% to 21%
# against an effect of 5-14%, which failed correct code 7 runs in 8 and still
# failed one in five once the floor was estimated properly. _timing_delta's
# `separated` asks the question the ratio was reaching for -- do the two sets of
# readings overlap -- and needs no constant to ask it.


TERRAIN_FIXTURE = "terrain_fixture.r16"
# The world range the fixture's 0..1 is loaded over. Deliberately not round and
# not symmetric: a range-mapping bug that swapped or scaled the endpoints would
# survive [-1, 1] and cannot survive this.
TERRAIN_RANGE = ("-40", "85")

# The fixture's closed form, RESTATED here rather than imported.
#
# NOT for the reason the LUT group gives. That one imports nothing because
# gen_lut_fixture.py really does write at module scope, so importing it would
# rewrite a committed asset mid-run; this generator writes only inside main()
# under a __main__ guard, and importing it would be perfectly safe. An earlier
# version of this comment claimed otherwise and was simply wrong about the file
# beside it.
#
# The real reason is narrower: the gate should be able to fail when the generator
# and the committed asset disagree, which it cannot if it derives its expectation
# from the generator. The cost is that these constants can drift from the
# generator's -- and terrain-closed-form failing is what that drift looks like.
_T_BASE, _T_AMP, _T_UC, _T_VC, _T_UT, _T_VT = 0.50, 0.24, 3.0, 2.0, 0.12, -0.09
_T_FA, _T_FU, _T_FV = 0.08, 34.0, 29.0


def _terrain_truth(u, v, lo=-40.0, hi=85.0):
    h01 = (_T_BASE
           + _T_AMP * math.sin(_T_UC * math.pi * u) * math.cos(_T_VC * math.pi * v)
           + _T_FA * math.sin(2.0 * math.pi * _T_FU * u) * math.cos(2.0 * math.pi * _T_FV * v)
           + _T_UT * u + _T_VT * v)
    return lo + h01 * (hi - lo)


_TERRAIN_SAMPLE = re.compile(
    r"terrain-height-probe sample x=(-?[\d.]+) z=(-?[\d.]+) h=(-?[\d.]+) fbm=(-?[\d.]+) "
    r"ny=(-?[\d.]+) flow=(-?[\d.]+) deposit=(-?[\d.]+) wear=(-?[\d.]+)")
_TERRAIN_CLAMP = re.compile(
    r"terrain-height-probe clamp x=(-?[\d.]+) z=(-?[\d.]+) h=(-?[\d.]+) "
    r"edge_x=(-?[\d.]+) edge_z=(-?[\d.]+) edge_h=(-?[\d.]+)")
_TERRAIN_MID = re.compile(
    r"terrain-height-probe mid x=(-?[\d.]+) z=(-?[\d.]+) h=(-?[\d.]+)")
_TERRAIN_HEADER = re.compile(
    r"terrain-height-probe header source=(\w+) res=(\d+) extent=([\d.]+) cell=([\d.]+)")
_TERRAIN_DIGEST = re.compile(r"terrain-erosion-probe digest value=([0-9a-f]+)")
_TERRAIN_BUDGET = re.compile(
    r"terrain-erosion-probe budget before=(-?[\d.]+) after=(-?[\d.]+) suspended=(-?[\d.]+) "
    r"closure=(-?[\d.eE+-]+) rel=([\d.eE+-]+)")
_TERRAIN_SAVED = re.compile(
    r"terrain-erosion-probe saved path=(\S+) min=(-?[\d.eE+-]+) max=(-?[\d.eE+-]+)")


def _terrain_run(workdir, tag, extra):
    """One forest run with the terrain probes, returning its parsed rows.

    Two frames, the smallest window the app accepts and no fog: every arm here
    reads printed numbers and none of them looks at a pixel, so any time spent
    rendering is time the sim is not running.
    """
    # No -S: every arm here reads printed numbers and none looks at a pixel, so a
    # screenshot is a full readback and a file write for nothing.
    #
    # --no-cook on the whole group (spec 11.99): these arms measure the LIVE
    # sim. The archetype hazard is terrain-threads, whose three runs differ
    # only in --erode-workers -- a count deliberately NOT in the cook key, so
    # warm, runs 2-3 would hit and the three digests would be copies of one
    # artefact: vacuously green on a broken band split. The others read probe
    # rows a cook hit refuses to print.
    cmd = [FOREST, "-x", "-f", "2", "-W", "320", "-H", "200", "--no-fog", "--no-cook"] + extra
    r = _run(cmd, capture_output=True, text=True)
    text = r.stdout + r.stderr
    if r.returncode != 0:
        print(f"  terrain      ERROR {tag} exited {r.returncode}: {text.strip()[-300:]}")
        return None
    return {
        "text": text,
        "header": _TERRAIN_HEADER.search(text),
        "samples": [tuple(float(g) for g in m.groups())
                    for m in _TERRAIN_SAMPLE.finditer(text)],
        "clamps": [tuple(float(g) for g in m.groups())
                   for m in _TERRAIN_CLAMP.finditer(text)],
        "mids": [tuple(float(g) for g in m.groups())
                 for m in _TERRAIN_MID.finditer(text)],
        "digest": (_TERRAIN_DIGEST.search(text).group(1)
                   if _TERRAIN_DIGEST.search(text) else None),
        "budget": (tuple(float(g) for g in _TERRAIN_BUDGET.search(text).groups())
                   if _TERRAIN_BUDGET.search(text) else None),
        "saved": (_TERRAIN_SAVED.search(text).groups()
                  if _TERRAIN_SAVED.search(text) else None),
    }


def run_terrain_gate(workdir):
    """Heightfield terrain and its erosion bake (spec 11.59 / D6-D8):

      terrain-seed         a field seeded from the fbm agrees with it at the nodes
      terrain-closed-form  a loaded field samples what the fixture painted
      terrain-clamp        outside the domain reads the edge, exactly
      terrain-bicubic      the normal stays smooth across cell boundaries
      terrain-masks        masks come from the SIM, not from the geometry
      terrain-threads      the bake is bit-identical at 1, 3 and 8 workers
      terrain-budget       the sediment budget closes
      terrain-roundtrip    save then load carries the height AND the three masks
      terrain-refuse       a bad heightmap is refused, not half-loaded

    Read as PRINTED NUMBERS rather than as pixels, and that is forced rather than
    chosen: apps/forest is not pixel-deterministic (the Hillaire sky moves ~35,000
    px run to run), and it is the only terrain consumer in the tree. Every claim
    here is about what the height function RETURNS, which no frame can show --
    a field wired to the wrong world scale, transposed, or clamped to zero outside
    its domain all render as perfectly plausible terrain.
    """
    fixture = asset(TERRAIN_FIXTURE)
    if not os.path.exists(FOREST):
        print(f"  terrain-closed-form SKIP  (forest not built)")
        return []
    if not os.path.exists(fixture):
        print(f"  terrain-closed-form SKIP  ({TERRAIN_FIXTURE} not present)")
        return []

    failures = []
    load = ["--heightmap", fixture, "--heightmap-range", TERRAIN_RANGE[0], TERRAIN_RANGE[1],
            "--terrain-height-probe"]
    loaded = _terrain_run(workdir, "loaded", load)
    if loaded is None or not loaded["samples"] or not loaded["header"]:
        print("  terrain-closed-form FAIL  the loaded run printed no probe rows")
        return ["terrain-closed-form"]

    extent = float(loaded["header"].group(3))
    lo, hi = float(TERRAIN_RANGE[0]), float(TERRAIN_RANGE[1])

    # --- terrain-seed --------------------------------------------------------
    # The invariant the whole design rests on, and the only arm that can state it
    # from inside a gate: installing a field must not change what the terrain IS.
    #
    # A byte comparison against a pre-11.59 build is the direct form and no gate
    # can hold one. This is the reachable equivalent -- seed a field from the fbm,
    # run no sim, and require the field and the fbm to agree at the nodes the seed
    # wrote. Both numbers come from ONE process at ONE point, because the probe
    # snaps its samples onto the lattice and two runs would not be asking about
    # the same places. It fails on a transposed seed, a res-vs-res-1 slip either
    # way, and a sampler whose node convention disagrees with the seeder's -- none
    # of which any other arm sees, since they all run against a LOADED field the
    # fbm never touched.
    seeded = _terrain_run(workdir, "seeded", ["--erode", "--erode-res", "257",
                                              "--erode-iterations", "-1",
                                              "--terrain-height-probe"])
    if seeded is None or not seeded["samples"]:
        print("  terrain-seed FAIL  the seeded run printed no probe rows")
        failures.append("terrain-seed")
    else:
        worst = max(abs(h - fbm) for _x, _z, h, fbm, *_ in seeded["samples"])
        span = max(abs(h) for _x, _z, h, *_ in seeded["samples"]) or 1.0
        ok = worst / span < 1e-5
        print(f"  terrain-seed {'PASS' if ok else 'FAIL'}  {len(seeded['samples'])} nodes, "
              f"field vs fbm worst {worst:.6f} against a {span:.1f} range "
              f"({worst / span:.2e} relative, want < 1e-5; a transposed or off-by-one "
              f"lattice reads whole units)")
        if not ok:
            failures.append("terrain-seed")

    # --- terrain-closed-form ------------------------------------------------
    # The bar is one 16-bit code of the loaded range plus the cubic's own error
    # on a smooth field, which at 256 across is far below it. Tight enough that a
    # transpose (which the generator asserts moves >500 codes) cannot hide.
    # The probe snaps these rows onto field NODES, where every correct sampler
    # returns the stored value exactly, so the only error left is quantisation.
    # That is what keeps this bar at a couple of codes while the fixture carries a
    # near-Nyquist band -- read between nodes it would be 76, and a bar loose
    # enough to admit that is a bar that no longer says much.
    step = (hi - lo) / 65535.0
    worst, worst_at = 0.0, None
    for x, z, h, _fbm, _ny, _f, _d, _w in loaded["samples"]:
        u = (x + extent) / (2.0 * extent)
        v = (z + extent) / (2.0 * extent)
        d = abs(h - _terrain_truth(u, v, lo, hi))
        if d > worst:
            worst, worst_at = d, (x, z)
    bar = 3.0 * step
    ok = worst <= bar
    print(f"  terrain-closed-form {'PASS' if ok else 'FAIL'}  {len(loaded['samples'])} samples, "
          f"worst {worst / step:.2f} codes at {worst_at} (want <= {bar / step:.0f}; ground truth "
          f"is painted, so this catches a transpose, a node/centre slip or a range error)")
    if not ok:
        failures.append("terrain-closed-form")

    # --- terrain-clamp -------------------------------------------------------
    # EXACT equality, not a tolerance. The camera really does query out here, and
    # the policy is "read the edge", which is a statement about identical values
    # rather than about nearby ones.
    bad = [(x, z, h, eh) for x, z, h, _ex, _ez, eh in loaded["clamps"] if h != eh]
    ok = bool(loaded["clamps"]) and not bad
    print(f"  terrain-clamp {'PASS' if ok else 'FAIL'}  {len(loaded['clamps'])} out-of-domain "
          f"queries, {len(bad)} differ from their edge point (want 0, exactly; returning 0 or "
          f"extrapolating the cubic both drop or launch the camera)")
    if not ok:
        failures.append("terrain-clamp")

    # --- terrain-bicubic -----------------------------------------------------
    # At CELL MIDPOINTS, against the painted closed form, and both halves of that
    # are the result of the arm failing to falsify twice.
    #
    # It first compared the largest normal step along a scan line to the median,
    # on the theory that a C0 filter concentrates its change at cell boundaries.
    # Replacing Catmull-Rom with linear interpolation moved that from 2.94 to 4.93
    # against an 8.0 bar -- green on the mutation it existed to catch. Rewriting it
    # against the closed form's analytic normal was no better (0.00005 error on the
    # mutation), for a reason worth keeping: terrain_normal_at central-differences
    # over less than one cell, and inside a cell a bilinear surface IS the
    # linearisation that difference estimates, so the normal cannot see the filter
    # at all. Only the VALUE halfway between two nodes can -- every interpolant
    # agrees exactly on the lattice, and a chord is furthest from its curve at the
    # midpoint. The fixture carries a near-Nyquist band so that gap is 458 codes
    # rather than the 5 its first version had.
    mids = loaded["mids"]
    if not mids:
        print("  terrain-bicubic FAIL  the probe printed no midpoint rows")
        failures.append("terrain-bicubic")
    else:
        worst_m, worst_m_at = 0.0, None
        for x, z, h in mids:
            u = (x + extent) / (2.0 * extent)
            v = (z + extent) / (2.0 * extent)
            d = abs(h - _terrain_truth(u, v, lo, hi))
            if d > worst_m:
                worst_m, worst_m_at = d, (x, z)
        ok = worst_m <= 150.0 * step
        print(f"  terrain-bicubic {'PASS' if ok else 'FAIL'}  worst {worst_m / step:.1f} codes "
              f"at a cell midpoint {worst_m_at} (want <= 150; bicubic reads ~53 here and "
              f"bilinear several hundred, where neither a smoothness ratio nor the normal "
              f"can see the difference at all)")
        if not ok:
            failures.append("terrain-bicubic")

    # --- terrain-masks -------------------------------------------------------
    # Two-sided, and the zero half is what makes it mean anything: a mask secretly
    # derived from slope would be non-zero on this loaded field too. Only a mask
    # that comes from a SIM is exactly zero before one has run.
    eroded = _terrain_run(workdir, "eroded", ["--erode", "--erode-res", "192",
                                              "--erode-iterations", "120",
                                              "--terrain-height-probe"])
    if eroded is None or not eroded["samples"]:
        print("  terrain-masks FAIL  the eroded run printed no probe rows")
        failures.append("terrain-masks")
    else:
        pre = max(max(s[5], s[6], s[7]) for s in loaded["samples"])
        flows = [s[5] for s in eroded["samples"]]
        post = max(max(s[5], s[6], s[7]) for s in eroded["samples"])
        spread = max(flows) - min(flows)
        ok = pre == 0.0 and post > 0.2 and spread > 0.2
        print(f"  terrain-masks {'PASS' if ok else 'FAIL'}  un-eroded peak {pre:.6f} (want "
              f"exactly 0 -- a slope-derived mask would not be), eroded peak {post:.4f} and "
              f"flow spread {spread:.4f} (want > 0.2 each; a flat mask is a sim that ran and "
              f"did nothing)")
        if not ok:
            failures.append("terrain-masks")

    # --- terrain-threads -----------------------------------------------------
    # The digest, not the sums: addition hides compensating differences, so two
    # thread counts that disagree cell by cell can still report identical totals.
    # Counts chosen to include ones that do NOT divide the resolution, since a
    # remainder-free split is where a band partition goes wrong.
    digests = {}
    for w in ("1", "3", "8"):
        run = _terrain_run(workdir, f"w{w}", ["--erode", "--erode-res", "96",
                                              "--erode-iterations", "60",
                                              "--erode-workers", w,
                                              "--terrain-erosion-probe"])
        digests[w] = run["digest"] if run else None
    values = set(digests.values())
    ok = len(values) == 1 and None not in values
    print(f"  terrain-threads {'PASS' if ok else 'FAIL'}  " +
          ", ".join(f"{w}={d}" for w, d in digests.items()) +
          " (want one value; 96 is not divisible by 3, which is where a band split slips)")
    if not ok:
        failures.append("terrain-threads")

    # --- terrain-budget ------------------------------------------------------
    # The hydraulic stages only MOVE material, so ground plus suspended load has
    # to return to what it started as. Semi-Lagrangian advection shipped first and
    # leaked 3.06% here while producing terrain that looked entirely correct.
    budget = _terrain_run(workdir, "budget", ["--erode", "--erode-res", "128",
                                              "--erode-iterations", "100",
                                              "--terrain-erosion-probe"])
    if budget is None or budget["budget"] is None:
        print("  terrain-budget FAIL  the run printed no budget row")
        failures.append("terrain-budget")
    else:
        rel = budget["budget"][4]
        ok = rel < 1e-6
        print(f"  terrain-budget {'PASS' if ok else 'FAIL'}  closure {rel:.3e} relative "
              f"(want < 1e-6; float rounding over this many cells lands near 5e-9, and a "
              f"non-conservative transport lands near 3e-2)")
        if not ok:
            failures.append("terrain-budget")

    # --- terrain-roundtrip ---------------------------------------------------
    saved = os.path.join(workdir, "terrain_rt.r16")
    rt = _terrain_run(workdir, "rt", ["--erode", "--erode-res", "128",
                                      "--erode-iterations", "60",
                                      "--erode-save", saved,
                                      "--terrain-erosion-probe", "--terrain-height-probe"])
    if rt is None or rt["saved"] is None or not rt["samples"]:
        print("  terrain-roundtrip FAIL  the bake did not report a saved range")
        failures.append("terrain-roundtrip")
    else:
        _p, smin, smax = rt["saved"]
        back = _terrain_run(workdir, "rtback",
                            ["--heightmap", saved, "--heightmap-range", smin, smax,
                             "--terrain-height-probe"])
        if back is None or len(back["samples"]) != len(rt["samples"]):
            print("  terrain-roundtrip FAIL  the reloaded field printed no matching rows")
            failures.append("terrain-roundtrip")
        else:
            rstep = (float(smax) - float(smin)) / 65535.0
            worst = max(abs(a[2] - b[2]) for a, b in zip(rt["samples"], back["samples"]))
            # The MASKS as well, and this half is why the arm exists in this shape.
            # It first compared heights only, and the save wrote heights only, so a
            # shipping load got the eroded geometry shaded by the slope-and-altitude
            # guess erosion was built to replace -- the feature's own opening failure,
            # arrived at through its own round trip, with every arm green.
            mask_before = max(max(a[5], a[6], a[7]) for a in rt["samples"])
            mask_worst = max(abs(a[i] - b[i])
                             for a, b in zip(rt["samples"], back["samples"]) for i in (5, 6, 7))
            mstep = 1.0 / 255.0
            ok = worst <= 0.5 * rstep and mask_before > 0.2 and mask_worst <= mstep
            print(f"  terrain-roundtrip {'PASS' if ok else 'FAIL'}  height worst "
                  f"{worst / rstep:.3f} of one 16-bit step (want <= 0.5, which is what "
                  f"round-to-nearest owes); masks peak {mask_before:.4f} before (want > 0.2, "
                  f"or there is nothing to carry) and worst {mask_worst / mstep:.3f} of one "
                  f"8-bit step across the trip (want <= 1)")
            if not ok:
                failures.append("terrain-roundtrip")

    # --- terrain-refuse ------------------------------------------------------
    # A headerless format cannot tell a truncated file from a smaller terrain
    # except by the square check, so this is the one thing standing between a
    # damaged download and a silently wrong world.
    truncated = os.path.join(workdir, "terrain_bad.r16")
    with open(fixture, "rb") as src, open(truncated, "wb") as dst:
        dst.write(src.read()[:-6])
    bad = _terrain_run(workdir, "refuse",
                       ["--heightmap", truncated, "--heightmap-range",
                        TERRAIN_RANGE[0], TERRAIN_RANGE[1], "--terrain-height-probe"])
    if bad is None or not bad["header"]:
        print("  terrain-refuse FAIL  the run produced no probe header")
        failures.append("terrain-refuse")
    else:
        named = "not a square" in bad["text"]
        fell_back = bad["header"].group(1) == "analytic"
        ok = named and fell_back
        print(f"  terrain-refuse {'PASS' if ok else 'FAIL'}  truncated file "
              f"{'named' if named else 'NOT named'} in a warning and the source fell back to "
              f"{bad['header'].group(1)} (want analytic; a half-loaded field is worse than "
              f"none, because it renders)")
        if not ok:
            failures.append("terrain-refuse")

    return failures


# ---------------------------------------------------------------------------
# terrain streaming (spec 11.69)
# ---------------------------------------------------------------------------

# The source field these arms stream is WRITTEN HERE, and the committed fixture
# is why: terrain_fixture.r16 is 256 nodes, 255 is odd, so a node-centred grid
# halves zero times and it carries no coarse level at all -- the save refuses it
# by name, and a stream of it would have nothing to fall to. 257 halves seven
# times, and painting it from the SAME closed form keeps one statement of what
# this terrain is rather than adding a second asset to drift from the first.
STREAM_RES = 257
# Stated here rather than read back off the probe, the "grid 34" discipline: a
# gate that takes the design's own numbers from the thing under test cannot fail
# when the design changes underneath it.
STREAM_TILE = 64    # TERRAIN_STREAM_TILE_NODES
STREAM_LEVELS = 7   # 257, 129, 65, 33, 17, 9, 5
STREAM_L0_TILES = 5 # ceil(257 / 64), so 25 tiles cover level 0
# Forced-small residency, the --layers-vt-page-slots 4 idiom. resident-res 32
# leaves levels 0..3 streaming; the region radius is what sizes level 0's own
# window, so it has to come down too or the finest level covers the domain and
# there is nothing to miss.
STREAM_SMALL = ["--terrain-stream-resident-res", "32", "--terrain-stream-window", "2",
                "--terrain-stream-budget", "1", "--region-radius", "60", "--region-span", "40"]
# The walk's two-sided bound, both measured from two runs before being written
# down: 475 tiles read and 4461 kB of window, identical across runs and with an
# identical residency digest -- the walk is deterministic by construction, so
# these are exact numbers rather than samples and the ceiling can sit close.
#
# Deleting the dead-band measures 598. That is a 1.26x separation where 11.67's
# equivalent got 10x, and the reason is worth knowing before trusting this arm
# too far: a scripted walk is the MILD case for a dead-band. It crosses tile
# boundaries monotonically at constant speed, so re-centring every frame lands
# on much the same tile-aligned positions. What a dead-band is really for is a
# camera breathing across one boundary, which no headless arm produces.
STREAM_WALK_LOAD_MAX = 520
STREAM_WALK_WINDOW_KB = 5120

# How far the character's height above the terrain may step between two trace
# rows while it walks.
#
# NOT origin-physics' 0.05, and the difference is the measurement: that arm
# holds a RESTING capsule across a shift, where this one walks a triangle mesh
# and crosses region seams. Measured 0.1200 on the streamed walk and 0.1200 on
# the same walk with the field fully resident -- the same number, which is the
# real content here and what the bar is floored just above. A collider built
# from the fall does not read smoother, it reads adrift: the printed terrain
# column still comes from level 0 while the ground under the capsule does not,
# and the first version of this feature measured 70.03 that way.
STREAM_CLEARANCE_MAX = 0.20

_STREAM_HEADER = re.compile(
    r"terrain-stream-header path=(\S+) res=(\d+) levels=(\d+) tile=(\d+) "
    r"min=(-?[\d.eE+-]+) max=(-?[\d.eE+-]+) ms=([\d.]+)")
_STREAM_PROBE = re.compile(
    r"terrain-stream-probe frame=(\d+) levels=(\d+) l0=(-?\d+),(-?\d+) window_kb=(\d+) "
    r"loaded=(\d+) ensured=(\d+) misses=(\d+) digest=([0-9a-f]{8})")
_STREAM_LEVEL = re.compile(
    r"terrain-stream-probe level idx=(\d+) res=(\d+) window=(\d+) tiles=(\d+) "
    r"resident=(\d+) whole=(\d)")
_STREAM_FALLBACK = re.compile(
    r"terrain-stream-probe fallback x=(-?[\d.]+) z=(-?[\d.]+) h=(-?[\d.eE+-]+) "
    r"expect=(-?[\d.eE+-]+) level=(\d+) resident=(\d)")
_STREAM_SAVED = re.compile(
    r"terrain-stream-probe saved path=(\S+) res=(\d+) levels=(\d+) tile=(\d+) "
    r"min=(-?[\d.eE+-]+) max=(-?[\d.eE+-]+)")


def _stream_source(workdir):
    """The 257-node twin of the committed fixture, painted from _terrain_truth."""
    path = os.path.join(workdir, "stream_src.r16")
    if not os.path.exists(path):
        lo, hi = float(TERRAIN_RANGE[0]), float(TERRAIN_RANGE[1])
        with open(path, "wb") as f:
            for j in range(STREAM_RES):
                row = bytearray()
                for i in range(STREAM_RES):
                    h01 = (_terrain_truth(i / (STREAM_RES - 1), j / (STREAM_RES - 1), lo, hi)
                           - lo) / (hi - lo)
                    row += struct.pack("<H", int(round(max(0.0, min(1.0, h01)) * 65535.0)))
                f.write(row)
    return path


def _stream_run(workdir, tag, extra):
    """One forest run carrying both probes, since the stream rows ride the launch."""
    r = _terrain_run(workdir, tag, extra)
    if r is None:
        return None
    text = r["text"]
    r["sheader"] = _STREAM_HEADER.search(text)
    r["ssaved"] = _STREAM_SAVED.search(text)
    r["probe"] = [{"frame": int(m.group(1)), "levels": int(m.group(2)),
                   "l0": (int(m.group(3)), int(m.group(4))), "window_kb": int(m.group(5)),
                   "loaded": int(m.group(6)), "ensured": int(m.group(7)),
                   "misses": int(m.group(8)), "digest": m.group(9)}
                  for m in _STREAM_PROBE.finditer(text)]
    r["slevels"] = [tuple(int(g) for g in m.groups()) for m in _STREAM_LEVEL.finditer(text)]
    r["fallback"] = [{"x": float(m.group(1)), "z": float(m.group(2)), "h": float(m.group(3)),
                      "expect": float(m.group(4)), "level": int(m.group(5)),
                      "resident": int(m.group(6))}
                     for m in _STREAM_FALLBACK.finditer(text)]
    return r


def run_terrain_stream_gate(workdir):
    """Terrain streaming: a tiled field on disk, windows over it (spec 11.69 / D4):

      terrain-stream-identity   a streamed field answers what the resident one did
      terrain-stream-correct    under eviction pressure it still reads what was painted
      terrain-stream-fallback   a miss reads the coarse level exactly, never a zero
      terrain-stream-refuse     a damaged file is refused whole, not half-loaded
      terrain-stream-walk       windows follow a walk and the reads stay bounded
      terrain-stream-collider   the character stays grounded while tiles churn under it

    Read as printed numbers, for the reason the terrain group states and one
    more: residency is invisible in a frame BY DESIGN. The fall returns a
    smoother surface, not a broken one, so a field that never became resident
    renders as plausible terrain -- which is what makes the anti-vacuity clause
    load-bearing on every arm here rather than a formality.

    The source field is painted at gate time (_stream_source) instead of
    committed. The committed 256-node fixture cannot stand in: 255 is odd, so it
    halves zero times, carries no coarse level, and the save refuses it.
    """
    failures = []
    src = _stream_source(workdir)
    cts = os.path.join(workdir, "stream.cts")

    # --- terrain-stream-identity ---------------------------------------------
    # One launch writes the tiled file and prints the resident field's own
    # answers; the next reads that file back. Comparing the two is the whole
    # arm, and it is a comparison of EVERY probe row rather than a sampled one
    # -- samples on the lattice, mids between nodes and clamps outside the
    # domain each fail differently under a paging bug.
    whole = _stream_run(workdir, "stream_whole",
                        ["--heightmap", src, "--heightmap-range", TERRAIN_RANGE[0],
                         TERRAIN_RANGE[1], "--terrain-height-probe",
                         "--terrain-stream-save", cts])
    streamed = _stream_run(workdir, "stream_read",
                           ["--terrain-stream", cts, "--terrain-height-probe",
                            "--terrain-stream-probe", "1"])
    if whole is None or streamed is None or not whole["ssaved"]:
        print("  terrain-stream-identity FAIL  a run did not produce its rows")
        failures.append("terrain-stream-identity")
    else:
        same = (whole["samples"] == streamed["samples"] and whole["clamps"] == streamed["clamps"]
                and whole["mids"] == streamed["mids"])
        # The anti-vacuity, and it is not a formality: a build whose stream
        # never opened falls back to the analytic terrain, which would differ
        # loudly -- but one that opened and never READ anything would answer
        # from the coarse tail and still look like terrain.
        armed = streamed["sheader"] is not None
        loaded = streamed["probe"][-1]["loaded"] if streamed["probe"] else 0
        levels = int(streamed["sheader"].group(3)) if armed else 0
        tile = int(streamed["sheader"].group(4)) if armed else 0
        ok = same and armed and loaded > 0 and levels == STREAM_LEVELS and tile == STREAM_TILE
        print(f"  terrain-stream-identity {'PASS' if ok else 'FAIL'}  "
              f"{len(streamed['samples'])} samples, {len(streamed['mids'])} midpoints and "
              f"{len(streamed['clamps'])} clamp pairs identical to the resident field: {same}; "
              f"the file opened with {levels} levels at tile {tile} (want {STREAM_LEVELS} and "
              f"{STREAM_TILE}, both stated here rather than read back) and read "
              f"{loaded} tiles (want > 0, or the identity is a stream that never armed)")
        if not ok:
            failures.append("terrain-stream-identity")

    # --- terrain-stream-correct ----------------------------------------------
    # The same painted ground truth the committed fixture is read against, now
    # under a residency too small to hold the domain: the probe walks 25 points
    # across the whole field at a one-tile budget, so tiles are read, dropped
    # and read again underneath it. A wrong tile INDEX is what this catches, and
    # it is the failure mode paging has that nothing else does -- the generator
    # asserts a transpose moves over 500 codes against a 3-code bar.
    small = _stream_run(workdir, "stream_small",
                        ["--terrain-stream", cts, "--terrain-height-probe",
                         "--terrain-stream-probe", "1"] + STREAM_SMALL)
    small2 = _stream_run(workdir, "stream_small2",
                         ["--terrain-stream", cts, "--terrain-height-probe",
                          "--terrain-stream-probe", "1"] + STREAM_SMALL)
    if small is None or small2 is None or not small["samples"]:
        print("  terrain-stream-correct FAIL  a run did not produce its rows")
        print("  terrain-stream-fallback FAIL  (same run)")
        failures += ["terrain-stream-correct", "terrain-stream-fallback"]
    else:
        lo, hi = float(TERRAIN_RANGE[0]), float(TERRAIN_RANGE[1])
        extent = float(small["header"].group(3))
        step = (hi - lo) / 65535.0
        worst, worst_at = 0.0, None
        for x, z, h, _fbm, _ny, _f, _d, _w in small["samples"]:
            u = (x + extent) / (2.0 * extent)
            v = (z + extent) / (2.0 * extent)
            d = abs(h - _terrain_truth(u, v, lo, hi))
            if d > worst:
                worst, worst_at = d, (x, z)
        bar = 3.0 * step
        # Determinism folded in here rather than given an arm of its own: two
        # runs of one build must agree on every row a reader can see, which is
        # the stream rows AND the heights they produced.
        deterministic = (small["samples"] == small2["samples"]
                         and small["mids"] == small2["mids"]
                         and [p["digest"] for p in small["probe"]]
                         == [p["digest"] for p in small2["probe"]])
        # More tiles read than level 0 HAS is the refill proof: 25 cover it, so
        # a 26th read is one that had to come back after being dropped.
        read = small["probe"][-1]["loaded"] if small["probe"] else 0
        # The residency this arm claims to be measuring, asserted rather than
        # assumed: a forced-small run must produce a MIXED pyramid. All-whole is
        # the identity configuration, where every number above is true and none
        # of it is about streaming.
        mixed = (any(r[5] == 1 for r in small["slevels"])
                 and any(r[5] == 0 for r in small["slevels"]))
        ok = (worst <= bar and deterministic and mixed
              and read > STREAM_L0_TILES * STREAM_L0_TILES)
        print(f"  terrain-stream-correct {'PASS' if ok else 'FAIL'}  "
              f"{len(small['samples'])} samples under a 1-tile budget, worst "
              f"{worst / step:.2f} codes at {worst_at} (want <= {bar / step:.0f}: a tile index "
              f"off by one moves hundreds); two runs identical: {deterministic}; "
              f"{sum(1 for r in small['slevels'] if r[5] == 0)} of "
              f"{len(small['slevels'])} levels streaming (want a mix, or this is the identity "
              f"config wearing the arm's name); {read} tiles read against "
              f"{STREAM_L0_TILES * STREAM_L0_TILES} in level 0 (want more, or nothing was "
              f"ever re-read)")
        if not ok:
            failures.append("terrain-stream-correct")

        # --- terrain-stream-fallback -----------------------------------------
        # Rides the same launch. The fallback rows are the only ones that do NOT
        # ensure, so they are the only place the miss policy is observable: `h`
        # asks for level 0 and falls, `expect` asks for the level it settled on
        # and does not. Equal only if the fall returns that level's own value.
        rows = small["fallback"]
        missed = [r for r in rows if r["resident"] == 0]
        bad = [r for r in rows if r["h"] != r["expect"]]
        spread = (max(r["expect"] for r in rows) - min(r["expect"] for r in rows)) if rows else 0.0
        misses = small["probe"][-1]["misses"] if small["probe"] else 0
        ok = bool(rows) and not bad and bool(missed) and misses > 0 and spread > 0.0
        print(f"  terrain-stream-fallback {'PASS' if ok else 'FAIL'}  {len(rows)} probe points, "
              f"{len(bad)} disagree with the level they fell to (want 0, exactly: a zero fill, "
              f"a clamp to the window or the wrong plane all separate them); {len(missed)} sat "
              f"outside level 0's window and the run logged {misses} falls (want > 0 each, or "
              f"nothing missed); coarse spread {spread:.2f} (want > 0, or the fallback is flat)")
        if not ok:
            failures.append("terrain-stream-fallback")

    # --- terrain-stream-refuse -----------------------------------------------
    # The terrain-refuse discipline on the new format. Forced small so the
    # refusal is exercised in the configuration the feature actually runs in,
    # where most of the file is never read at open.
    #
    # What holds this arm is NOT the manifest length check, and the mutation
    # round is how that was learned: deleting the check leaves it green in every
    # configuration tried. The coarse levels sit at the END of the layout and
    # are read whole whatever the residency, so a truncated tail always fails a
    # READ first. The check is defence in depth against a file whose header and
    # length disagree without the tail being touched; what this arm actually
    # pins is the refusal path itself -- that a failed read refuses the whole
    # field instead of installing a half-loaded one.
    trunc = os.path.join(workdir, "stream_bad.cts")
    with open(cts, "rb") as fsrc, open(trunc, "wb") as fdst:
        blob = fsrc.read()
        fdst.write(blob[:len(blob) - 4096])
    bad_run = _stream_run(workdir, "stream_bad",
                          ["--terrain-stream", trunc, "--terrain-height-probe"] + STREAM_SMALL)
    if bad_run is None or not bad_run["header"]:
        print("  terrain-stream-refuse FAIL  the run produced no probe header")
        failures.append("terrain-stream-refuse")
    else:
        named = "terrain_stream" in bad_run["text"] and trunc in bad_run["text"]
        fell_back = bad_run["header"].group(1) == "analytic"
        ok = named and fell_back
        print(f"  terrain-stream-refuse {'PASS' if ok else 'FAIL'}  truncated file "
              f"{'named' if named else 'NOT named'} in a warning and the source fell back to "
              f"{bad_run['header'].group(1)} (want analytic; a half-loaded field is worse than "
              f"none, because it renders)")
        if not ok:
            failures.append("terrain-stream-refuse")

    # --- terrain-stream-walk / terrain-stream-collider -----------------------
    # One walk, two arms, in the region group's own idiom: residency follows the
    # camera, the camera follows the player, and --trace-player rides along so
    # "the windows moved" and "the ground stayed under the character" are read
    # off the same 400 frames rather than two runs that merely shared flags.
    #
    # A 513 field rather than the 257 above, because this needs level 0 bigger
    # than a window that can plausibly hold it while still baking in under a
    # second: --erode-iterations -1 seeds and installs, running no sim.
    walk_cts = os.path.join(workdir, "walk.cts")
    seeded = _stream_run(workdir, "stream_walk_bake",
                         ["--erode", "--erode-res", "513", "--erode-iterations", "-1",
                          "--terrain-stream-save", walk_cts])
    if seeded is None or not seeded["ssaved"]:
        print("  terrain-stream-walk FAIL  the 513 field did not save")
        print("  terrain-stream-collider FAIL  (same run)")
        failures += ["terrain-stream-walk", "terrain-stream-collider"]
        return failures

    _, _, walk_text = _region_run(REGION_CHURN + REGION_WALK +
                                  ["--terrain-stream", walk_cts,
                                   "--terrain-stream-resident-res", "64",
                                   "--terrain-stream-window", "4",
                                   "--terrain-stream-probe", "50"])
    rows = [{"frame": int(m.group(1)), "l0": (int(m.group(3)), int(m.group(4))),
             "window_kb": int(m.group(5)), "loaded": int(m.group(6)), "digest": m.group(9)}
            for m in _STREAM_PROBE.finditer(walk_text)]
    if len(rows) < 4:
        print(f"  terrain-stream-walk FAIL  the probe printed {len(rows)} rows, want at least 4")
        failures.append("terrain-stream-walk")
    else:
        moved = len({r["l0"] for r in rows}) > 1
        grew = rows[-1]["loaded"] > rows[0]["loaded"]
        # The thrash ceiling, the 71/701/200 template. A walk both churns and
        # settles, so the bound is two-sided: growth proves the windows follow,
        # the ceiling proves they are not re-reading ground they already hold.
        no_thrash = rows[-1]["loaded"] <= STREAM_WALK_LOAD_MAX
        capped = all(r["window_kb"] <= STREAM_WALK_WINDOW_KB for r in rows)
        ok = moved and grew and no_thrash and capped
        print(f"  terrain-stream-walk {'PASS' if ok else 'FAIL'}  level 0's window took "
              f"{len({r['l0'] for r in rows})} distinct positions over the round trip (want > 1: "
              f"it follows the walk); reads went {rows[0]['loaded']} -> {rows[-1]['loaded']} "
              f"(want growth, and <= {STREAM_WALK_LOAD_MAX} -- the dead-band deleted re-reads a "
              f"column a frame); windows held <= {STREAM_WALK_WINDOW_KB} kB at every print: "
              f"{capped}")
        if not ok:
            failures.append("terrain-stream-walk")

    # --- terrain-stream-collider ---------------------------------------------
    # The hazard this whole design is arranged around. A collider is built from
    # heights and handed to Jolt, which keeps its own copy: if the mesh under
    # the player were ever built from the fall, the floor would step by the
    # difference between two pyramid levels and the capsule would sink or hover.
    # Read as the clearance origin-physics already reads, against its bound.
    trace = [{"t": float(m.group(1)), "x": float(m.group(2)), "y": float(m.group(3)),
              "grounded": m.group(5), "terrain": float(m.group(6))}
             for m in _ORIGIN_TRACE.finditer(walk_text)]
    settled = trace[1:]
    if len(settled) < 3:
        print(f"  terrain-stream-collider FAIL  {len(settled)} settled trace rows, want 3+")
        failures.append("terrain-stream-collider")
    else:
        clearances = [r["y"] - r["terrain"] for r in settled]
        drift = max(abs(b - a) for a, b in zip(clearances, clearances[1:]))
        grounded = all(r["grounded"] == "1" for r in settled)
        # Anti-vacuity, and it is the whole point of riding this launch: the
        # character has to have stayed grounded WHILE tiles were being read
        # under it, not on a run where residency never moved.
        churned = bool(rows) and rows[-1]["loaded"] > rows[0]["loaded"]
        ok = grounded and drift <= STREAM_CLEARANCE_MAX and churned
        print(f"  terrain-stream-collider {'PASS' if ok else 'FAIL'}  {len(settled)} settled "
              f"rows, grounded at every one: {grounded}; worst clearance step {drift:.3f} "
              f"(want <= {STREAM_CLEARANCE_MAX}, the same 0.120 a fully resident walk measures: "
              f"a collider built from the fall steps by the gap between two levels); "
              f"the same run read "
              f"{rows[-1]['loaded'] - rows[0]['loaded']} tiles while it walked (want > 0, or "
              f"nothing was streaming)")
        if not ok:
            failures.append("terrain-stream-collider")

    return failures
