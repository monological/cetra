"""The island group: the island (spec 11.63)."""

import math
import re
import os

from gatelib.harness import FOREST, _run


_ISLAND_HEADER = re.compile(r"terrain-height-probe island start=([\d.]+) depth=([\d.]+)")
_ISLAND_ROW = re.compile(
    r"terrain-height-probe island az=(\d+) r=([\d.]+) h=(-?[\d.]+) flat=(-?[\d.]+) "
    r"drop=(-?[\d.]+) t=([\d.]+) floor=(-?[\d.]+)")
_SCATTER_SHORE = re.compile(
    r"scatter-probe shore trees=(\d+) low_y=(-?[\d.]+) water=(\d+) water_level=(-?[\d.]+)")

# World units. The falloff is a smoothstep, so it is exactly flat inside its start
# and the drop there is a true zero rather than a small number -- but the height it
# is subtracted from is the fbm at ~95 units, so the bar is loose enough to survive
# the subtraction's own rounding and tight enough that a falloff leaking inland by
# any visible amount fails.
ISLAND_INLAND_MAX = 1e-3
# The falloff is a smoothstep in fp32, so its saturated value is an exact 1 and
# its steps are far larger than this. The slack is for the read, not the value.
ISLAND_FALLOFF_EPS = 1e-4
# How deep the rim has to be by the domain's edge, as a fraction of the island's
# depth. Not 1.0, because the sea floor deliberately keeps a QUARTER of the
# terrain's own relief rather than going exactly flat -- a run of exactly coplanar
# triangles is a collider Jolt's tree builder cannot split, and its fallback
# TRACES into a handler that aborts the process.
ISLAND_RIM_FRACTION = 0.6
# The axis radius the round-vs-square comparison is anchored at. Inside the
# falloff's own band (which starts at 0.72) so the axis has partly fallen and the
# bar has somewhere to fail -- at 0.72 or below both sides read 0 and at 1.0 both
# read 1, and either end passes for a square island too.
ISLAND_AXIS_R = 0.8
# The fraction of the half-extent the scatter draws inside (forest's sample_ground
# clips to it). Ground outside it is not a candidate, so it cannot be evidence
# that a placement rule rejected anything.
SCATTER_MARGIN = 0.96


def _island_probe(extra):
    """The height probe's island rows, as (header, rows) or (None, [])."""
    cmd = ([FOREST, "-x", "-f", "1", "-W", "200", "-H", "150", "--no-fog", "--seed", "1337",
            "--terrain-height-probe"] + extra)
    r = _run(cmd, capture_output=True, text=True)
    text = r.stdout + r.stderr
    head = _ISLAND_HEADER.search(text)
    rows = [{"az": int(m.group(1)), "r": float(m.group(2)), "h": float(m.group(3)),
             "flat": float(m.group(4)), "drop": float(m.group(5)), "t": float(m.group(6)),
             "floor": float(m.group(7))}
            for m in _ISLAND_ROW.finditer(text)]
    # (None, None) for "did not run" against (None, []) for "ran and printed no
    # island rows". The control arm reads an empty list as proof the shaping is
    # off, so a run that crashed must not look like one that was asked not to
    # shape.
    if r.returncode != 0:
        return None, None
    if not head:
        return None, rows
    return {"start": float(head.group(1)), "depth": float(head.group(2))}, rows


def run_island_gate(workdir):
    """The island: ground that falls to a shoreline, and a sea past it.

      island-shore  the falloff is exactly nothing inland of where it starts,
                       grows without reversing outside it, and has reached the sea
                       floor by the domain's edge -- on every azimuth, so the shore
                       is a ring and not a band that happens to cross the frame.
                       Read as the DROP against the same terrain unshaped, which is
                       what makes it a statement about the falloff rather than
                       about this seed's noise.
      island-dry       no prop of any kind stands below the waterline, on terrain
                       that reaches the sea floor -- so the rule had drowned ground
                       to reject rather than passing on a world with none. ROCKS
                       and not just trees: a tree is turned away from the shoal by
                       the slope test long before the waterline matters, where a
                       rock tolerates 57 degrees and the rim is 45. Falsified by
                       deleting sample_ground's waterline rejection, which puts the
                       lowest prop at about y -175 on the sea floor. The second bar
                       -- reachable ground BELOW the waterline -- is what stops the
                       first passing on a world with nothing to reject, and it is
                       bounded by SCATTER_MARGIN because ground the scatter clips
                       away was never a candidate.

    --no-island is the control on the first, and a real one: it prints no island
    rows at all, which is what says the shaping is off rather than merely small.
    """
    if not os.path.exists(FOREST):
        print("  island-shore SKIP  (forest not built)")
        return []
    failures = []

    head, rows = _island_probe([])
    _, flat_rows = _island_probe(["--no-island"])
    if not head or not rows:
        print("  island-shore ERROR the height probe produced no island rows")
        return ["island-shore", "island-dry"]

    # The FALLOFF is read off the row rather than recovered from the drop. The
    # drop is the falloff times a span that includes the noise AND the sea floor's
    # share of it, so it follows every dip in the terrain and is not monotone in r
    # -- and reconstructing it here would restate arithmetic only terrain.c can
    # see.
    #
    # NOT monotonicity of t along a ray, which this arm used to assert: the probe
    # sweeps radially and the falloff is a function of radius, so t rises with r
    # for ANY radial falloff and the bar cannot fail.
    #
    # And NOT "the diagonals have fallen further at one radius", which was the
    # first replacement and is unsatisfiable for the same reason from the other
    # side: the probe samples in POLAR coordinates and the falloff is polar, so
    # every azimuth reads an identical t at a given r BY CONSTRUCTION. It read
    # 0.708 against 0.708.
    #
    # What can fail is the shape read against the SQUARE the domain actually is.
    # Box fraction -- how far out a point is as a share of the half-extent along
    # its own dominant axis -- is what a Chebyshev falloff would be a function of.
    # So the bar is: a diagonal sample NO FURTHER OUT in box terms than an axis
    # sample has still fallen FURTHER. True of a circle inscribed in the square,
    # false of the square. Concretely the diagonal at r 1.1 sits at box fraction
    # 0.778 against the axis sample's 0.8, and reads t 1.000 against 0.198; a
    # Chebyshev falloff would read 0.106 there and fail.
    def box_fraction(row):
        ang = 2.0 * math.pi * row["az"] / len({x["az"] for x in rows})
        return row["r"] * max(abs(math.cos(ang)), abs(math.sin(ang)))

    inland = max((abs(x["drop"]) for x in rows if x["r"] <= head["start"]), default=0.0)
    azimuths = sorted({x["az"] for x in rows})
    # az 0/2/4/6 are the axes, 1/3/5/7 the diagonals (terrain.c sweeps 8 evenly).
    axis_row = next((x for x in rows if x["az"] % 2 == 0
                     and abs(x["r"] - ISLAND_AXIS_R) < 1e-4), None)
    diag_rows = []
    for az in sorted({x["az"] for x in rows if x["az"] % 2 == 1}):
        under = [x for x in rows if x["az"] == az and box_fraction(x) <= ISLAND_AXIS_R + 1e-6]
        if under:
            diag_rows.append(max(under, key=box_fraction))
    # Every diagonal, not the best one: an azimuth-dependent falloff would satisfy
    # a bar that took the maximum and is exactly what the ring shape is about.
    round_island = (axis_row is not None and len(diag_rows) == 4
                    and all(d["t"] > axis_row["t"] + 1e-6 for d in diag_rows))
    axis_t = [axis_row["t"]] if axis_row else []
    diag_t = [d["t"] for d in diag_rows]
    rim_short = 0
    for az in azimuths:
        seq = sorted((x for x in rows if x["az"] == az), key=lambda x: x["r"])
        # Two bars at the rim, and they answer different questions: the falloff has
        # to have RUN OUT (t == 1, exactly what the smoothstep owes past its upper
        # edge), and the ground has to be deep water rather than merely damp. The
        # second is not implied by the first -- a floor that kept all of the
        # terrain's relief instead of a quarter of it would satisfy t and surface.
        rim = [x for x in seq if x["r"] >= 1.0]
        # The rim bar is the DEPTH, not t: t == 1 past the smoothstep's upper edge
        # is the clamp's own endpoint and cannot fail. Depth can -- a sea floor
        # that kept all of the terrain's relief instead of a quarter of it would
        # still saturate t and still surface.
        if not rim or max(x["h"] for x in rim) > -head["depth"] * ISLAND_RIM_FRACTION:
            rim_short += 1
    ok = (inland <= ISLAND_INLAND_MAX and round_island and rim_short == 0
          and len(azimuths) >= 8 and flat_rows is not None and not flat_rows)
    print(f"  island-shore {'PASS' if ok else 'FAIL'}  worst drop inland of r "
          f"{head['start']:.2f} is {inland:.2e} units (want <= {ISLAND_INLAND_MAX:.0e}); at box "
          f"fraction {max((box_fraction(d) for d in diag_rows), default=0):.3f} the diagonals have "
          f"fallen {min(diag_t) if diag_t else 0:.3f} against the axes' "
          f"{max(axis_t) if axis_t else 0:.3f} at a LARGER {ISLAND_AXIS_R:.2f} (want strictly "
          f"more, or the island is square and not round); {rim_short} azimuths short of the "
          f"{-head['depth'] * ISLAND_RIM_FRACTION:.0f}-unit rim (want 0), and --no-island prints "
          f"{len(flat_rows) if flat_rows is not None else 'a crash'} island rows (want 0)")
    if not ok:
        failures.append("island-shore")

    def shore(extra):
        cmd = ([FOREST, "-x", "-f", "1", "-W", "200", "-H", "150", "--no-fog", "--seed", "1337",
                "--scatter-probe"] + extra)
        r = _run(cmd, capture_output=True, text=True)
        m = _SCATTER_SHORE.search(r.stdout + r.stderr)
        if r.returncode != 0 or not m:
            return None
        return {"trees": int(m.group(1)), "low_y": float(m.group(2)), "water": int(m.group(3)),
                "level": float(m.group(4))}

    isle = shore([])
    if not isle:
        print("  island-dry ERROR the scatter probe produced no shore row")
        return failures + ["island-dry"]
    # The GROUND's own minimum is the control, not the props of an unshaped run.
    # An unshaped forest has almost no terrain below the waterline either -- its
    # heights are the fbm about zero -- so comparing the two would compare two
    # numbers that are equal because neither had anywhere to go. What says the
    # rule had work is that the island's rim reaches the sea floor, which is well
    # below where anything is allowed to stand.
    #
    # Bounded to SCATTER_MARGIN, because ground the scatter cannot draw from is
    # not ground the rule rejected. sample_ground clips to 0.96 of the extent and
    # the probe sweeps to 1.4, so an unbounded min reports drowned ground that was
    # never a candidate.
    ground_low = min((x["h"] for x in rows if x["r"] <= SCATTER_MARGIN), default=0.0)
    ok = (isle["water"] == 1 and isle["trees"] > 0 and isle["low_y"] >= isle["level"]
          and ground_low < isle["level"])
    print(f"  island-dry {'PASS' if ok else 'FAIL'}  the lowest prop of any kind stands at y "
          f"{isle['low_y']:.1f} against a waterline at {isle['level']:.1f} (want above it), on "
          f"reachable terrain that reaches {ground_low:.1f} (want below it, or there was no "
          f"drowned ground the scatter could have drawn from) -- over {isle['trees']} trees and "
          f"the rocks beside them")
    if not ok:
        failures.append("island-dry")

    return failures
