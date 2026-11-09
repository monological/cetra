"""The fogdepth group: fog at the translucent depth (spec 11.78)."""

import os

from gatelib.harness import _import_fixture_gen, asset, render
from gatelib.images import _box_luma, _read_ppm
from gatelib.scenes import _cscn_camera, _projector


FOGDEPTH_FIXTURE = "fog_glass_fixture.cscn"
FOGDEPTH_GEN = "gen_fog_glass_fixture.py"
# Sixty, for FOGVOL_FRAMES' reason: the froxel volume has its own temporal accumulator and
# has not settled at thirty.
FOGDEPTH_FRAMES = "60"
# Fraction of a pane's projected width the read box keeps, and the rows it spans. Inset far
# enough that no box reaches an antialiased edge at either the pane's depth or the
# backdrop's.
FOGDEPTH_INSET = 0.55
FOGDEPTH_ROWS = (0.32, 0.68)


def _fogdepth_layout(gen, w, h):
    """Read boxes PROJECTED from the fixture's own geometry, never transcribed.

    Four panes at one depth with bare backdrop between them, so a pane and its reference
    are the same screen rows and the vignette and tone curve cancel. Both come out of the
    generator's numbers through the camera in its .cscn, because transcribing either leaves
    two copies that agree until someone edits the fixture -- and the failure is silent, the
    gate still passing while sampling something else. Falsified by hand: PANE_X inner 1.05
    -> 1.20 keeps every one of the generator's own asserts green and slides the half pane's
    edge from 0.311 to 0.294, across a hand-written gap box ending at 0.295.

    Returns (boxes, neighbours). A pane's backdrop reference is the mean of the gaps either
    side and has to be LOCAL: the backdrop is a plane, so the path to it grows as 1/cos of
    the horizontal angle and its fog ratio runs 0.621 at the frame edge against 0.667 dead
    centre. One shared reference would read that gradient as signal.
    """
    project = _projector(_cscn_camera(FOGDEPTH_FIXTURE), w, h)
    y0, y1 = FOGDEPTH_ROWS

    def span(x_lo, x_hi, z):
        """Fractional x-range of a world segment at depth z, as seen on screen."""
        lo = project((x_lo, gen.PANE_Y, z))[0] / w
        hi = project((x_hi, gen.PANE_Y, z))[0] / w
        return (min(lo, hi), max(lo, hi))

    def inset(lo, hi, frac):
        mid, half = (lo + hi) * 0.5, (hi - lo) * 0.5 * frac
        return (mid - half, y0, mid + half, y1)

    names = ["opaque", "half", "quarter", "dark"]
    order = sorted(range(len(gen.PANE_X)), key=lambda i: project(
        (gen.PANE_X[i], gen.PANE_Y, gen.PANE_Z))[0])
    boxes, edges = {}, []
    for name, i in zip(names, order):
        lo, hi = span(gen.PANE_X[i] - gen.PANE_HALF_W, gen.PANE_X[i] + gen.PANE_HALF_W,
                      gen.PANE_Z)
        boxes[name] = inset(lo, hi, FOGDEPTH_INSET)
        edges.append((name, lo, hi))

    # Gaps: bare backdrop between consecutive panes, plus one outside each end pane.
    gap_names = ["edgeL"] + [f"gap{a[0].upper()}{b[0].upper()}"
                             for a, b in zip(names, names[1:])] + ["edgeR"]
    gaps = ([(0.0, edges[0][1])]
            + [(edges[k][2], edges[k + 1][1]) for k in range(len(edges) - 1)]
            + [(edges[-1][2], 1.0)])
    for name, (lo, hi) in zip(gap_names, gaps):
        boxes[name] = inset(lo, hi, FOGDEPTH_INSET)

    neighbours = {names[k]: (gap_names[k], gap_names[k + 1]) for k in range(len(names))}
    return boxes, neighbours
# The two sweep panes carry alpha 0.5 and 0.25 and are mirrored about x=0, so the
# difference of their fog ratios is (0.5 - 0.25) x the near-vs-far advantage with the
# backdrop gradient cancelled. Measured 0.0676 against 0.0677 predicted; before the fix it
# was 0.0008, because coverage reached the composite nowhere at all.
FOGDEPTH_COVER_MIN = 0.040
# An alpha-0.5 pane must be fogged clear of the backdrop behind it. Measured lifts of
# 0.1594 (centre pane) and 0.1743 (outer pane) after, against 0.0039 and 0.0121 before.
FOGDEPTH_LIFT_MIN = 0.080
# ... and must stay under the fully-near answer the opaque pane reads. Without this the
# floor above is passed by fogging EVERYTHING at the near depth, which is the same defect
# mirrored. Measured 0.8195 against 0.9309.
FOGDEPTH_NEAR_MARGIN = 0.030
# Opaque pixels must not move when the moment path arms. Their colour comes from the opaque
# pass and a composite whose arithmetic this feature does not touch, so the true expectation
# is exactly 0 and this is slack, not a tolerance. Sized as a code's worth of a mid-grey
# ratio rather than as a code: these are ratios of 12x12-averaged LINEAR lumas.
FOGDEPTH_OPAQUE_EPS = 1.5 / 255.0


def _fogdepth_ratios(workdir, name, gen, extra):
    """Fog-over-no-fog ratio per crop box, which is what makes the reads comparable.

    The moment OIT reconstruction does not return a blend pane's composite exactly -- 4.4%
    low at alpha 0.5 on this fixture -- and that loss is present in both renders, so a
    ratio divides it out and leaves the fog. Reading absolute codes instead would fold the
    reconstruction's error into every number below.

    The medium comes from the generator, so the frame is rendered in the one its own
    asserts are calibrated in.
    """
    scene = asset(FOGDEPTH_FIXTURE)
    fog = ["--fog", "--fog-density", str(gen.FOG_DENSITY), "--fog-height", str(gen.FOG_HEIGHT)]
    pin = ["--no-auto-exposure", "-E", "1.0", "--no-dither"]
    out, boxes = {}, None
    for tag, flags in (("on", pin + fog + extra), ("off", pin + extra)):
        path = os.path.join(workdir, f"fogdepth_{name}_{tag}.ppm")
        err = render(scene, path, flags, frames=FOGDEPTH_FRAMES)
        if err:
            return None, None, err
        w, h, pix = _read_ppm(path)
        boxes, neighbours = _fogdepth_layout(gen, w, h)
        out[tag] = {k: _box_luma(pix, w, h, b) for k, b in boxes.items()}

    ratios = {k: out["on"][k] / max(out["off"][k], 1e-6) for k in boxes}
    return ratios, neighbours, None


def run_fogdepth_gate(workdir):
    """Fog at the translucent depth (spec 11.78): a pane fogged at its own distance.

    The atmosphere composite reads ONE linear Z per pixel out of the aux attachment, and
    the late pass never writes it, so a translucent surface used to be fogged at the depth
    of whatever opaque thing stood behind it. The moments carry what aux cannot -- b0 is
    the stack's absorbance and b1/b0 its mean warped depth -- and the composite now folds
    its two media at that depth as well and mixes by coverage.

    Every read is a fog-over-no-fog RATIO, so the moment reconstruction's own loss divides
    out. Three arms, none implying the others:

      fogdepth-coverage  the alpha-0.5 and alpha-0.25 panes differ. They are mirrored about
                         x=0, so the backdrop's 1/cos path gradient cancels and what is
                         left is coverage. Fails any build where cover does not reach the
                         mix -- which is exactly the shipping state before this spec, where
                         the two panes differed by 0.0008.
      fogdepth-lift      both alpha-0.5 panes read clear of the backdrop beside them AND
                         below the opaque pane's fully-near answer. The ceiling is not
                         decoration: the floor alone is passed by fogging everything at the
                         near depth, which is the same defect with its sign flipped.
      fogdepth-offpath   --no-oit-moments reaches the PRE-FIX frame: the panes fall back
                         onto the backdrop's fog (lift under the floor above) while the
                         opaque pane and all five gaps do not move at all. Both halves are
                         needed and neither substitutes. The first fails a build that arms
                         the second depth regardless of the flag, so the bisect lever stops
                         being one -- and the weighted-blended accumulation carries no
                         depth statistic, so there is nothing for it to arm FROM. The
                         second fails any leak onto pixels with no translucency in front of
                         them, which no golden can see because none puts a blend surface in
                         fog.

    Stated honestly, because a coverage claim is the least-verified sentence in most specs.
    Four mutations were run by hand -- opacity forced to 0 and to 1, the wrong texture bound
    on the moment unit, and the b0 guard deleted -- and the two arms above caught the first
    three between them while fogdepth-offpath caught none.

    That is not the same as inert, and its two halves are worth different things. The
    FALLBACK half cannot be broken from the shader at all: with no atlas generated there is
    no b0 to arm from whatever the shader does. It is worth having the day someone gives
    the weighted-blended path a depth statistic and wires it in here. The IDENTITY half is
    the arm that underwrites "the goldens do not move" -- it is the only assertion anywhere
    that the b0 gate leaves a fog frame with no translucency in it exactly as it was, and a
    later edit hoisting the opacity out of that gate, or arming the path unconditionally,
    lands here and nowhere else. It survived the four mutations because none of them touched
    the gate, not because nothing can.

    Neither half covers the atlas addressing under --render-scale: these arms render at
    scale 1, where the atlas and the draw are the same size.

    --no-dither because the reads are ratios of small luma differences where a +/-1 LSB is
    large, and sixty frames because the froxel volume has its own accumulator. No --no-ssao,
    unlike the sibling fog-volume group: its reason is arming attribution, which does not
    apply here, and AO is constant between the two legs of every ratio anyway.
    """
    scene = asset(FOGDEPTH_FIXTURE)
    if not os.path.exists(scene):
        print(f"  fogdepth-coverage SKIP  ({FOGDEPTH_FIXTURE} not present)")
        return []
    gen = _import_fixture_gen(FOGDEPTH_GEN, "fogdepth-coverage")
    if gen is None:
        return []

    r, neighbours, err = _fogdepth_ratios(workdir, "base", gen, [])
    if err:
        print(f"  fogdepth-coverage ERROR render failed: {err.strip()[-200:]}")
        return ["fogdepth-coverage"]

    failures = []

    def backdrop(pane, vals=None):
        a, b = neighbours[pane]
        v = vals if vals is not None else r
        return 0.5 * (v[a] + v[b])

    spread = r["half"] - r["quarter"]
    ok = spread >= FOGDEPTH_COVER_MIN
    print(f"  fogdepth-coverage {'PASS' if ok else 'FAIL'}  a=0.5 {r['half']:.4f} - "
          f"a=0.25 {r['quarter']:.4f} = {spread:.4f} want >={FOGDEPTH_COVER_MIN}")
    if not ok:
        failures.append("fogdepth-coverage")

    ok = True
    detail = []
    for pane in ("half", "dark"):
        lift = r[pane] - backdrop(pane)
        under = r["opaque"] - r[pane]
        detail.append(f"{pane} lift={lift:.4f} under-near={under:.4f}")
        if lift < FOGDEPTH_LIFT_MIN or under < FOGDEPTH_NEAR_MARGIN:
            ok = False
    print(f"  fogdepth-lift     {'PASS' if ok else 'FAIL'}  {', '.join(detail)} "
          f"want lift>={FOGDEPTH_LIFT_MIN} and under-near>={FOGDEPTH_NEAR_MARGIN}")
    if not ok:
        failures.append("fogdepth-lift")

    nomom, _, err = _fogdepth_ratios(workdir, "nomom", gen, ["--no-oit-moments"])
    if err:
        print(f"  fogdepth-offpath  ERROR render failed: {err.strip()[-200:]}")
        return failures + ["fogdepth-offpath"]
    untouched = ["opaque"] + [k for k in r if k.startswith(("gap", "edge"))]
    worst = max(abs(r[k] - nomom[k]) for k in untouched)
    where = max(untouched, key=lambda k: abs(r[k] - nomom[k]))
    fell_back = max(nomom[p] - backdrop(p, nomom) for p in neighbours if p != "opaque")
    ok = worst <= FOGDEPTH_OPAQUE_EPS and fell_back < FOGDEPTH_LIFT_MIN
    print(f"  fogdepth-offpath  {'PASS' if ok else 'FAIL'}  panes fall back to lift "
          f"{fell_back:.4f} (want <{FOGDEPTH_LIFT_MIN}), opaque and gaps move "
          f"{worst:.5f} at {where} (want <={FOGDEPTH_OPAQUE_EPS:.5f})")
    if not ok:
        failures.append("fogdepth-offpath")

    return failures
