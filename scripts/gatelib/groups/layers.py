"""The layers group: layered surfaces and their composite cache (specs 11.60, 11.66)."""

import re
import os
import subprocess

from gatelib.harness import FOREST, RENDER, ROOT, _import_fixture_gen, _run, asset, render
from gatelib.images import _read_ppm, compare
from gatelib.scenes import _cscn_camera, _projector, cscn_copy
from gatelib.world import REGION_CHURN, REGION_WALK, _region_run


# --- Layered surfaces and their composite cache (specs 11.60, 11.66) ---------
#
# Every arm here reads `--render-mode 6`, the albedo view, and that is the whole
# reason the fixture is legible. In that view the shader returns
# linearToSRGB(albedoFactor * sRGBToLinear(blend)) and the fixture's factor is
# white, so a correct renderer hands back the exact byte the generator painted --
# no lighting, no exposure, no tonemap in the path to argue about. A lit read
# would be a claim about the BRDF as much as about the blend.
#
# The fixture's constants are IMPORTED rather than restated. gen_layer_fixture
# paints the ground truth, so the gate asserting the renderer reproduces it is
# not circular -- the renderer is what is under test. The lut gate declines the
# same move for the opposite reason worth keeping straight: there the generator
# owns a closed form for the ANSWER, and importing it would make ground truth a
# function of the interpolator being measured.

_LAYER_GEN = None
_LAYER_VT_GEN = None


def _layer_gen():
    global _LAYER_GEN
    if _LAYER_GEN is None:
        _LAYER_GEN = _import_fixture_gen("gen_layer_fixture.py")
    return _LAYER_GEN


def _layer_vt_gen():
    global _LAYER_VT_GEN
    if _LAYER_VT_GEN is None:
        _LAYER_VT_GEN = _import_fixture_gen("gen_layer_vt_fixture.py")
    return _LAYER_VT_GEN


# How far off the truth the coarse fallback alone must read on the road's edge
# (spec 11.68) -- the anti-vacuity for the clause above it, since "pages match
# the truth" is also satisfied where the fallback matches it too and pages are
# doing nothing. Measured 18 codes of smear; the bar is 10, and a build whose
# roads never reach the atlas reads 0 here because every leg is bare ground.
ROAD_PAGES_SMEAR_MIN = 10

# One statement of the probe line's shape, shared by every consumer: the
# fixture runner and the forest walk arm read the same print, and two regexes
# would let a probe format change strand one of them silently.
_VT_PROBE = re.compile(
    r"layers-vt-probe frame=(\d+) grid=(\d+) slots=(\d+) resident=(\d+) wanted=(\d+) "
    r"requested=(\d+) loaded=(\d+) evicted=(\d+) digest=([0-9a-f]{8})")


def _vt_probe_rows(text):
    """Every --layers-vt-probe line in `text`, oldest first, as dicts."""
    return [{"frame": int(m.group(1)), "grid": int(m.group(2)), "slots": int(m.group(3)),
             "resident": int(m.group(4)), "wanted": int(m.group(5)),
             "requested": int(m.group(6)), "loaded": int(m.group(7)),
             "evicted": int(m.group(8)), "digest": m.group(9)}
            for m in _VT_PROBE.finditer(text)]


def _vt_point(u, v):
    """World point on the VT fixture at splat coordinate (u, v).

    The floor and the 45-degree ramp meet at z = 0, so one mapping serves both:
    y is -z on the ramp and 0 on the floor. v runs from the ramp's crest (0) to
    the floor's near edge (1), matching the splat rows the generator paints.
    Geometry from the VT generator, which OWNS it -- today it inherits the
    parent's HALF, but that is one editable line over there, and a scan built
    from the parent's number would silently mis-sample the day they diverge.
    """
    g2 = _layer_vt_gen()
    x = -g2.HALF + u * 2.0 * g2.HALF
    z = -g2.HALF + v * 2.0 * g2.HALF
    return (x, max(0.0, -z), z)


def _layer_floor(u, v):
    """World point on the floor plate at splat coordinate (u, v)."""
    g = _layer_gen()
    return (-g.HALF + u * 2.0 * g.HALF, 0.0, g.HALF - v * 2.0 * g.HALF)


def _layer_wall(u, v):
    """World point on the wall plate at splat coordinate (u, v)."""
    g = _layer_gen()
    return (-g.HALF + u * 2.0 * g.HALF, v * g.WALL_H, -g.HALF)


def _layer_sample(frame, world):
    """The raw sRGB byte at a world point -- the painted code, in the albedo view.

    RAISES for a point that projects off-screen rather than clamping to the
    border. A clamp returns a number, and a number is what an arm reads: a
    fixture whose framing has drifted would go on passing against whatever pixel
    happened to sit at the edge.
    """
    w, h, pix, project = frame
    px, py = project(world)
    x, y = int(round(px)), int(round(py))
    if x < 0 or y < 0 or x >= w or y >= h:
        raise ValueError(f"layer sample at {world} projects to ({x}, {y}), outside {w}x{h}")
    o = (y * w + x) * 3
    return (pix[o] + pix[o + 1] + pix[o + 2]) / 3.0


def _layer_scan(frame, points):
    """Mean byte at each of a list of world points."""
    return [_layer_sample(frame, p) for p in points]


def _layer_transitions(vals, lo, hi):
    """Count crossings of the midpoint between lo and hi, with hysteresis.

    Hysteresis rather than a bare threshold because a mip-softened checker edge
    wanders either side of the midpoint over a pixel or two, and a bare compare
    would count each wobble as a fresh cell.
    """
    mid = 0.5 * (lo + hi)
    band = 0.2 * (hi - lo)
    state = None
    count = 0
    for v in vals:
        if v > mid + band:
            new = True
        elif v < mid - band:
            new = False
        else:
            continue
        if state is not None and new != state:
            count += 1
        state = new
    return count


def _layer_ramp(vals, positions, lo, hi):
    """(10-90 width, 50% crossover position) for a monotone ramp from lo to hi.

    The CROSSOVER is the half that matters and the first version of this did not
    return it. The 10-90 width of a height blend is 0.889 x LAYER_BLEND_RANGE and
    is algebraically independent of the layer heights -- so an arm reading only
    the width is measuring one shader constant, and deleting the height term
    entirely leaves it unchanged. The height decides WHERE the two layers trade
    places, not how fast.

    Returns None if the scan is not monotone, rather than measuring it anyway: a
    single sample the wrong side of a threshold inside a plateau otherwise
    returns a spuriously narrow or wide width and passes silently.
    """
    drops = sum(1 for i in range(1, len(vals)) if vals[i] < vals[i - 1] - 8.0)
    if drops:
        return None

    def crossing(frac):
        t = lo + frac * (hi - lo)
        for i in range(1, len(vals)):
            a, b = vals[i - 1], vals[i]
            if (a - t) * (b - t) <= 0.0 and a != b:
                k = (t - a) / (b - a)
                return positions[i - 1] + k * (positions[i] - positions[i - 1])
        return None

    first, mid, last = crossing(0.1), crossing(0.5), crossing(0.9)
    if first is None or mid is None or last is None:
        return None
    return (last - first, mid)


def run_layers_gate(workdir):
    """Layered surfaces: the per-texel blend, and the composite cache over it.

    The arms, in the order they run. Keep this list and the code in step: a
    docstring naming a different set than the function runs is what a reviewer
    reads to decide what is covered. (The scatter arms live in a helper and are
    therefore invisible to gate-arm-docs; they run last, after everything below:
    layers-scatter, then layers-road-scatter off the same forest run.)

      layers-select      each splat column resolves the layer it selects
      layers-srgb        a mid-grey layer round-trips through the decode
      layers-height      the height blend interlocks where linear smears
      layers-triplanar   the wall is textured by its own axis, at the right size
      layers-vt-identity the cached path reproduces the per-texel blend where the
                         atlas resolves the macro, and a forced-coarse atlas
                         moves the frame -- which is what proves the flag arms a
                         real path at all
      layers-vt-detail   checker crossings at full grain density through the
                         cache, on the floor and on the 45-degree ramp
      layers-vt-macro    at an atlas texel WIDER than a checker period the
                         selection still reads exact painted bytes while the
                         grain still crosses -- the one arm a plain baked atlas
                         (the roadmap's original stage 1) fails
      layers-vt-invalidate a mid-run layerBlend change re-bakes the cache: the
                         crossover moves to the linear 0.5, which a stale atlas
                         cannot show
      layers-vt-budget   the bake's own MB log equals the gate's closed form at
                         the gate's own derived resolution
      layers-vt-pages-identity pages on vs off at the derived resolution is
                         splat-edge resample only, AND the probe proves pages
                         went resident -- the pixel half alone passes a build
                         whose pages never arm
      layers-vt-pages-effect at a forced-coarse fallback, pages restore most of
                         what the coarseness lost against the full reference
      layers-vt-pages-churn four slots against a 25-page grid plus a --cam-at
                         teleport: capacity holds, the teleport evicts and
                         reloads, the tail is stable, two runs are identical
      layers-vt-pages-walk forest's 400-frame region walk churns pages under
                         the capacity clamp, on the design's own 34-page grid
      layers-vt-feedback-sky aimed at the sky the vote pass requests zero pages
                         while prediction still wants some
      layers-vt-feedback-occlusion behind the ramp only the ramp's pages vote;
                         the frustum-visible hidden floor casts none -- the arm
                         that discriminates feedback from prediction
      layers-road-select a road reads its own layer's painted byte through the
                         cache, over two different base layers, and leaves the
                         ground past its shoulder untouched; the per-texel path
                         reads the same bytes
      layers-road-edge   the shoulder crossover sits where the HEIGHTS put it and
                         collapses to a narrow band, against a linear twin that
                         crosses at the mask's own half -- the arm that proves
                         the override happens BEFORE the height blend
      layers-road-pages  with a shoulder finer than a fallback texel, pages on vs
                         off differ and pages land nearer the per-texel truth:
                         the first content that is not a 0 px page identity
      layers-road-invalidate a mid-run --road-width-at re-bakes the cache, so a
                         point outside the bake-time shoulder becomes road

    WHAT THE FIRST FOUR ARMS CANNOT SEE: layer_fixture's splat is UV1-space, and
    the composite cache serves world-XZ splats only, so those arms cover the
    per-texel path alone -- which stays live (walls, props, UV1 splats) and
    needs the coverage. The cache's coverage is the vt arms, on their own
    world-XZ fixture.
    """
    failures = []
    src = asset("layer_fixture.cscn")
    if not os.path.exists(src):
        print("  layers-select     SKIP  (missing layer_fixture.cscn)")
        return []
    g = _layer_gen()
    if g is None:
        return []

    ALBEDO = ["--render-mode", "6", "--no-auto-exposure", "-E", "1.0"]

    def shot(name, overrides=None, extra=None):
        out = os.path.join(workdir, f"layers_{name}.ppm")
        scene = src
        if overrides:
            scene = os.path.join(workdir, f"layers_{name}.cscn")
            cscn_copy(src, scene, overrides)
        err = render(scene, out, ALBEDO + (extra or []), frames=4)
        if err:
            return None
        w, h, pix = _read_ppm(out)
        # From the COMMITTED scene file, which is what was rendered. Reading the
        # generator's in-memory dict instead would let the gate keep passing
        # while predicting a different scene than it measured.
        return w, h, pix, _projector(_cscn_camera("layer_fixture.cscn"), w, h)

    base = shot("base")
    if base is None:
        print("  layers-select     ERROR while rendering the fixture")
        return ["layers-select"]
    w, h, pix, project = base

    # --- layers-select -------------------------------------------------------
    # The three FLAT columns, read at the centre of each so no bilinear bleed
    # from a neighbouring column reaches the sample. Column 1 is the checker and
    # is not a flat colour; the triplanar arm is what verifies it got selected.
    codes = {0: g.GREY_CODE, 2: g.DARK_CODE, 3: g.LIGHT_CODE}
    LAYER_SELECT_TOL = 4.0
    readings = []
    for col, expect in sorted(codes.items()):
        u = (col + 0.5) / 4.0
        got = _layer_sample(base, _layer_floor(u, 0.25))
        readings.append((col, expect, got))
    worst = max(abs(got - expect) for _, expect, got in readings)
    ok = worst <= LAYER_SELECT_TOL
    detail = ", ".join(f"col{c} {got:.0f} (want {e})" for c, e, got in readings)
    print(f"  layers-select     {'PASS' if ok else 'FAIL'}  {detail}; worst off by "
          f"{worst:.1f} (want <= {LAYER_SELECT_TOL})")
    if not ok:
        failures.append("layers-select")

    # --- layers-srgb ---------------------------------------------------------
    # TWO codes, on the WALL. The first version of this read the same pixel as
    # layers-select's column 0, so nothing could fail it without also failing
    # that -- four arms wearing five names. Two points are also what separates a
    # curve from a line: a decode error that happens to be affine reproduces one
    # sample and cannot reproduce two. The wall additionally routes the read
    # through the Z projection, which no other known-code read touches.
    srgb_reads = [(0.125, g.GREY_CODE), (0.625, g.DARK_CODE)]
    got = [(_layer_sample(base, _layer_wall(u, 0.25)), want) for u, want in srgb_reads]
    worst_srgb = max(abs(a - b) for a, b in got)
    ok = worst_srgb <= 3.0
    print(f"  layers-srgb       {'PASS' if ok else 'FAIL'}  wall reads "
          + ", ".join(f"{a:.0f} (want {b})" for a, b in got)
          + f"; worst off by {worst_srgb:.1f} (want <= 3. A doubled decode reads ~56 for "
            f"128, a missing one ~187)")
    if not ok:
        failures.append("layers-srgb")

    # --- layers-height -------------------------------------------------------
    # The wall's upper band ramps layer 2 into layer 3 across the full width. The
    # dark layer stands proud, so a height blend holds it well past the halfway
    # point and then gives way over a few texels; a linear blend crosses over
    # gradually across the whole ramp. Measured as the 10%-to-90% width, and the
    # linear leg is rendered rather than predicted so the two share everything
    # except the one authored knob.
    SCAN = 140
    us = [0.02 + i * (0.96 / (SCAN - 1)) for i in range(SCAN)]
    line = [_layer_wall(u, 0.78) for u in us]

    def ramp(frame):
        vals = _layer_scan(frame, line)
        return _layer_ramp(vals, us, g.DARK_CODE, g.LIGHT_CODE)

    # Closed form: the two layers trade places where their weights plus their
    # scaled heights meet, i.e. at (1 + (dark - light)/255 * sharpness) / 2. The
    # linear leg has no height term at all and crosses at exactly 0.5.
    expect_mid = (1.0 + (g.DARK_HEIGHT - g.LIGHT_HEIGHT) / 255.0 * g.LAYER_BLEND_SHARPNESS) / 2.0
    linear = shot("linear", lambda d: d["materials"]["layered_surface"].update(
        {"layerBlend": 0.0}))
    if linear is None:
        print("  layers-height     ERROR while rendering the linear leg")
        failures.append("layers-height")
    else:
        rh, rl = ramp(base), ramp(linear)
        if rh is None or rl is None:
            print("  layers-height     FAIL  the ramp is not a measurable monotone transition")
            failures.append("layers-height")
        else:
            w_height, mid_height = rh
            w_linear, mid_linear = rl
            ok = (abs(mid_height - expect_mid) <= 0.03 and abs(mid_linear - 0.5) <= 0.03
                  and w_height / w_linear <= 0.35)
            print(f"  layers-height     {'PASS' if ok else 'FAIL'}  crossover at "
                  f"{mid_height:.3f} (want {expect_mid:.3f} +/- 0.03; the linear leg reads "
                  f"{mid_linear:.3f}, want 0.500) -- the crossover is what the HEIGHTS "
                  f"decide, where the width {w_height:.3f} vs {w_linear:.3f} only reads "
                  f"LAYER_BLEND_RANGE")
            if not ok:
                failures.append("layers-height")

    # --- layers-triplanar ----------------------------------------------------
    # The wall's normal is +Z, so only the Z projection can give it structure
    # along y. A projection frozen on Y reads the wall's constant z as its second
    # coordinate and returns a wall that varies with x alone -- which is why the
    # count here is taken VERTICALLY, and why the fixture's layer 1 is a checker
    # rather than stripes.
    # The SAME world length on both plates, or the two counts are not comparable.
    # The wall's v spans WALL_H and the floor's spans 2*HALF, so the fractions
    # differ; the span in world units is what has to match.
    SPAN = 1.5
    col_u = 0.375  # centre of the checker column
    N = 96
    wall_v0, wall_dv = 0.06, SPAN / g.WALL_H
    floor_v0, floor_dv = 0.06, SPAN / (2.0 * g.HALF)
    wall_vals = _layer_scan(
        base, [_layer_wall(col_u, wall_v0 + wall_dv * i / (N - 1)) for i in range(N)])
    floor_vals = _layer_scan(
        base, [_layer_floor(col_u, floor_v0 + floor_dv * i / (N - 1)) for i in range(N)])
    crossings = _layer_transitions(wall_vals, g.STRIPE_DARK, g.STRIPE_LIGHT)
    floor_cross = _layer_transitions(floor_vals, g.STRIPE_DARK, g.STRIPE_LIGHT)

    span = SPAN
    # CROSSINGS, not cells: a scan of n cells crosses between them n-1 times, and
    # where the scan starts relative to a cell boundary moves that by one either
    # way. Comparing crossings against a cell count is what left this arm's lower
    # bound below the value a correct renderer produces.
    expected = span / g.CHECKER_CELL_WORLD - 1.0
    floor_range = max(floor_vals) - min(floor_vals)

    # Two-sided, and counted on BOTH plates. A lower bound alone passes a
    # projection that has dropped layerUvScale (cells half the size, twice the
    # count) and one frozen on X rather than Y (same count, wrong axis). What the
    # feature actually claims is that ONE texture appears at ONE size on two
    # perpendicular surfaces, so the two counts have to agree with each other and
    # with the closed form.
    lo, hi = expected - 1.5, expected + 1.5
    ok = (lo <= crossings <= hi and lo <= floor_cross <= hi
          and abs(crossings - floor_cross) <= 1 and floor_range >= 100.0)
    print(f"  layers-triplanar  {'PASS' if ok else 'FAIL'}  wall {crossings} / floor "
          f"{floor_cross} checker crossings over {span:.2f} units (want each in "
          f"[{lo:.1f}, {hi:.1f}] and within 1 of each other; a projection frozen on Y "
          f"reads 0 on the wall, a dropped uvScale roughly doubles both), floor range "
          f"{floor_range:.0f} (want >= 100)")
    if not ok:
        failures.append("layers-triplanar")

    # --- the composite cache (spec 11.66) ------------------------------------
    # Its own fixture, because this one's splat is UV1-space and the cache
    # serves world-XZ splats only -- no arm on layer_fixture can see the cache
    # in either direction. Inline rather than in a helper: gate-arm-docs reads
    # this function's own source, and a helper's arms would read as documented
    # but never run.
    vt_src = asset("layer_vt_fixture.cscn")
    g2 = _layer_vt_gen()
    if not os.path.exists(vt_src) or g2 is None:
        if g2 is not None:
            print("  layers-vt-identity SKIP  (missing layer_vt_fixture.cscn)")
        failures += _layers_scatter_arm()
        return failures

    def vt_shot(name, extra=None, frames=8, overrides=None):
        # 8, derived: two frames of texture uploads, one for the material
        # array, one for the bake that samples it, and margin. Since spec 14.8
        # the uploads are done before frame 1, so it is margin now; kept where
        # every reading below was taken.
        out = os.path.join(workdir, f"layers_vt_{name}.ppm")
        # The committed fixture is ROAD-FREE and stays that way: its golden and
        # every arm above render it unmutated, so a road arm builds its variant
        # here -- the sibling shot() idiom. The camera still comes from the
        # committed file, because no mutation touches it.
        scene = vt_src
        if overrides:
            scene = os.path.join(workdir, f"layers_vt_{name}.cscn")
            cscn_copy(vt_src, scene, overrides)
        err = render(scene, out, ALBEDO + (extra or []), frames=frames)
        if err:
            return None
        w2, h2, pix2 = _read_ppm(out)
        return (w2, h2, pix2, _projector(_cscn_camera("layer_vt_fixture.cscn"), w2, h2)), out

    cached = vt_shot("cached")
    plain = vt_shot("plain", ["--no-layers-vt"])
    # Pages OFF on this leg deliberately: it measures the stage-1 FALLBACK's
    # coarseness (the anti-vacuity that the res flag arms a real path), and
    # pages restoring sharpness would eat most of its floor. The vt-pages arms
    # below measure the pages against their own shots.
    coarse = vt_shot("coarse",
                     ["--layers-vt-res", str(g2.VT_MACRO_RES), "--no-layers-vt-pages"])
    if not (cached and plain and coarse):
        print("  layers-vt-identity ERROR while rendering the cache legs")
        failures.append("layers-vt-identity")
        failures += _layers_scatter_arm()
        return failures
    (cf, cpath), (pf, ppath), (of, opath) = cached, plain, coarse

    # --- layers-vt-identity --------------------------------------------------
    # Three claims in one A/B/C: the cached path reads the exact painted byte on
    # every flat column AND matches the per-texel frame away from splat-resample
    # edges and the blend crossover; and the forced-coarse atlas moves the frame
    # against per-texel, which is the anti-vacuity -- a build whose flag arms
    # nothing reads 0 there and fails, where the identity half alone would pass
    # it. Floor measured first: this fixture is 0 px across two runs of one
    # build, so these are hard numbers, not tolerances.
    def vt_columns_off(frame):
        # Worst deviation from the painted byte over the flat selection columns
        # (`codes`, the select arm's own table -- the vt fixture paints the same
        # column band).
        return max(abs(_layer_sample(frame, _vt_point((col + 0.5) / 4.0, 0.875)) - expect)
                   for col, expect in codes.items())

    worst_pt = max(vt_columns_off(cf), vt_columns_off(pf))
    ae_id, _ = compare(cpath, ppath)
    ae_coarse, _ = compare(opath, ppath)
    VT_ID_CEILING = 6000     # measured 2330 of 480000: edges + the crossover patch
    VT_COARSE_FLOOR = 10000  # measured 32161; 0 = the flag switched nothing
    ok = worst_pt <= 1.0 and ae_id <= VT_ID_CEILING and ae_coarse >= VT_COARSE_FLOOR
    print(f"  layers-vt-identity {'PASS' if ok else 'FAIL'}  flat columns off by "
          f"{worst_pt:.1f} (want <= 1), cached vs per-texel {ae_id} px (want <= "
          f"{VT_ID_CEILING}: splat-resample edges and the blend crossover only), coarse vs "
          f"per-texel {ae_coarse} px (want >= {VT_COARSE_FLOOR}; 0 = the flag arms nothing)")
    if not ok:
        failures.append("layers-vt-identity")

    # --- layers-vt-detail ----------------------------------------------------
    # Checker crossings through the CACHED path, on the floor's checker column
    # (a v-scan inside column 1) and across the ramp (a u-scan) -- the ramp is
    # the two-projection surface, which is what the per-texel path's triplanar
    # arm covers and a Y-only cache would lose. Counted the layers-triplanar
    # way: crossings, two-sided, against the cell size the generator derives.
    cell = g.CHECKER_CELL_WORLD
    NV = 96
    fv0, fv1 = 0.76, 0.99
    floor_pts = [_vt_point(0.375, fv0 + (fv1 - fv0) * i / (NV - 1)) for i in range(NV)]
    floor_span = (fv1 - fv0) * 2.0 * g2.HALF
    ramp_us = [0.02 + 0.96 * i / (NV - 1) for i in range(NV)]
    ramp_pts = [_vt_point(uu, 0.25) for uu in ramp_us]
    ramp_span = 0.96 * 2.0 * g2.HALF

    fcross = _layer_transitions(_layer_scan(cf, floor_pts), g.STRIPE_DARK, g.STRIPE_LIGHT)
    rcross = _layer_transitions(_layer_scan(cf, ramp_pts), g.STRIPE_DARK, g.STRIPE_LIGHT)
    f_expect = floor_span / cell - 1.0
    r_expect = ramp_span / cell - 1.0
    ok = abs(fcross - f_expect) <= 1.5 and abs(rcross - r_expect) <= 2.5
    print(f"  layers-vt-detail   {'PASS' if ok else 'FAIL'}  floor {fcross} crossings over "
          f"{floor_span:.2f} units (want {f_expect:.1f} +/- 1.5), ramp {rcross} over "
          f"{ramp_span:.2f} (want {r_expect:.1f} +/- 2.5; a dropped dominant index reads "
          f"layer 0's flat grey and crosses nowhere)")
    if not ok:
        failures.append("layers-vt-detail")

    # --- layers-vt-macro -----------------------------------------------------
    # The design's central claim, at a resolution where it is legible: the
    # forced atlas texel is WIDER than a checker period (generator-asserted), so
    # nothing grain-shaped can live in the cache -- yet the selection columns
    # still read their exact painted bytes (the macro survives) and the ramp
    # checker still crosses at full density (the grain arrives via the detail
    # term). A plain baked atlas -- the roadmap's original stage 1 -- holds the
    # first and fails the second.
    texel = 2.0 * g2.HALF / g2.VT_MACRO_RES
    coarse_worst = vt_columns_off(of)
    coarse_rcross = _layer_transitions(_layer_scan(of, ramp_pts), g.STRIPE_DARK, g.STRIPE_LIGHT)
    ok = coarse_worst <= 1.0 and abs(coarse_rcross - r_expect) <= 2.5
    print(f"  layers-vt-macro    {'PASS' if ok else 'FAIL'}  at {texel:.2f} units/texel "
          f"(a checker cell is {cell:.2f}) the columns read off by {coarse_worst:.1f} "
          f"(want <= 1) and the ramp still crosses {coarse_rcross} times (want "
          f"{r_expect:.1f} +/- 2.5)")
    if not ok:
        failures.append("layers-vt-macro")

    # --- layers-vt-invalidate ------------------------------------------------
    # A fresh process always bakes from the final authored values, so the
    # by-value key is unreachable without a mid-run transition; --layer-blend-at
    # is that transition. The crossover through the RE-BAKED cache must land on
    # the linear 0.5 where the startup bake put it at the height-blend position
    # -- a stale atlas keeps reading the latter. Known blind spot: this proves
    # the FRAME went fresh, not that the re-baked cache is what rendered it --
    # an implementation whose "invalidation" is falling back to per-texel
    # forever reads 0.5 here too, and only the startup coarse leg above pins
    # the cached path as live.
    #
    # 16 frames: the startup bake's 8, the transition at 10, one more ensure to
    # re-bake, and margin. The scan reuses the height arm's `us` and its
    # `expect_mid` closed form -- same band, same heights, one statement.
    trans = vt_shot("blendtrans", ["--layer-blend-at", "10:0.0"], frames=16)

    def vt_band_ramp(frame):
        vals = _layer_scan(frame, [_vt_point(uu, 0.625) for uu in us])
        return _layer_ramp(vals, us, g.DARK_CODE, g.LIGHT_CODE)

    if trans is None:
        print("  layers-vt-invalidate ERROR while rendering the transition leg")
        failures.append("layers-vt-invalidate")
    else:
        rh2 = vt_band_ramp(cf)
        rl2 = vt_band_ramp(trans[0])
        if rh2 is None or rl2 is None:
            print("  layers-vt-invalidate FAIL  the transition band is not measurable")
            failures.append("layers-vt-invalidate")
        else:
            ok = abs(rh2[1] - expect_mid) <= 0.03 and abs(rl2[1] - 0.5) <= 0.03
            print(f"  layers-vt-invalidate {'PASS' if ok else 'FAIL'}  crossover "
                  f"{rh2[1]:.3f} at bake (want {expect_mid:.3f} +/- 0.03), then "
                  f"{rl2[1]:.3f} after a frame-10 layerBlend 0 (want 0.500; a stale "
                  f"cache keeps reading {expect_mid:.3f})")
            if not ok:
                failures.append("layers-vt-invalidate")

    # --- layers-vt-budget ----------------------------------------------------
    # The bake's own MB line against the gate's OWN closed form -- the
    # layers-scatter lesson: a bar read from the process under test moves with
    # the thing it checks. The derived-resolution rule is restated here for the
    # same reason, seeded from the vt generator's floor so the two gate-side
    # restatements cannot drift apart silently. The closed-form equality is the
    # whole bar: the loop caps the expected res, so any leak past it fails the
    # equality before it could fail a ceiling.
    r = subprocess.run([RENDER, "-m", vt_src, "-x", "-f", "8", "-W", "200", "-H", "150"],
                       capture_output=True, text=True)
    m = re.search(r"Layers VT: material '[^']+' composite (\d+)x(\d+) pair, ([\d.]+) MB",
                  r.stdout + r.stderr)
    if not m:
        tail = (r.stderr or r.stdout).strip().splitlines()
        last = tail[-1][:90] if tail else "no output"
        print(f"  layers-vt-budget   ERROR  the run printed no bake line "
              f"(exited {r.returncode}: {last})")
        failures.append("layers-vt-budget")
    else:
        res_x, res_y, mb = int(m.group(1)), int(m.group(2)), float(m.group(3))
        domain = 2.0 * g2.HALF
        er = g2.VT_DERIVED_RES_MIN
        while er < domain / 0.5 and er < 2048:
            er *= 2
        closed = round(2.0 * er * er * 4.0 * (4.0 / 3.0) / (1024.0 * 1024.0), 1)
        ok = res_x == er and res_y == er and abs(mb - closed) <= 0.05
        print(f"  layers-vt-budget   {'PASS' if ok else 'FAIL'}  {res_x}x{res_y} at {mb} MB "
              f"(want {er} squared at {closed} MB by the gate's own 2*N^2*4*(4/3))")
        if not ok:
            failures.append("layers-vt-budget")

    # --- the paged near field (spec 11.67) -----------------------------------
    # Pages hold the SAME macro at 4x the fallback's density, so at the
    # fixture's derived resolution they resolve almost nothing (the identity)
    # and at a forced-coarse fallback they restore what it lost (the effect).
    # Residency is read through --layers-vt-probe, whose whole history is a
    # pure function of the flags -- measured bit-identical across runs.
    nopages = vt_shot("nopages", ["--no-layers-vt-pages"])
    coarse_pages = vt_shot("coarsepages", ["--layers-vt-res", str(g2.VT_MACRO_RES)])
    if not (nopages and coarse_pages):
        print("  layers-vt-pages-identity ERROR while rendering the page legs")
        failures.append("layers-vt-pages-identity")
        failures += _layers_scatter_arm()
        return failures

    def vt_probe_run(extra, frames, overrides=None, name="probe"):
        # The group's own 400x300 and ALBEDO framing, not a smaller size: the
        # occlusion arm's numbers were measured here, and the depth-off leak it
        # exists to catch did not reproduce at 200x150. Probe interval 4 against
        # 24 frames: six rows, and the last sits past the teleport frame plus
        # the 4-deep feedback ring plus several budget-2 bake frames, so every
        # arm reads a settled tail rather than a transient.
        scene = vt_src
        if overrides:
            # Keyed like vt_shot's, so a second probe variant cannot overwrite
            # the first and --keep leaves both to look at.
            scene = os.path.join(workdir, f"layers_vt_{name}.cscn")
            cscn_copy(vt_src, scene, overrides)
        r = subprocess.run([RENDER, "-m", scene, "-x", "-f", str(frames), "-W", "400",
                            "-H", "300"] + ALBEDO + ["--layers-vt-probe", "4"] + extra,
                           capture_output=True, text=True)
        return _vt_probe_rows(r.stdout + r.stderr)

    # --- layers-vt-pages-identity --------------------------------------------
    # Pages on vs off at the derived resolution: the pages resample the same
    # macro, so the difference is confined to sub-code resample of splat edges.
    # The anti-vacuity is the probe, not the pixels -- a build whose pages never
    # arm reads 0 px here and PASSES the ceiling, so the arm also demands
    # residency happened at all.
    ae_pages, _ = compare(cpath, nopages[1])
    probe = vt_probe_run([], 16)
    # Measured 696 of 480000: splat-edge resample only. Which page set sits
    # resident at the capture frame shifts with the vote decode (the tie-riding
    # encoding the review replaced read 490 here), so small moves inside the
    # ceiling track residency order, not blend arithmetic.
    VT_PAGES_ID_CEILING = 2000
    if not probe:
        print("  layers-vt-pages-identity FAIL  the probe printed no rows "
              "(a crash or a silenced probe, not a residency reading)")
        failures.append("layers-vt-pages-identity")
    else:
        resident_final = probe[-1]["resident"]
        ok = ae_pages <= VT_PAGES_ID_CEILING and resident_final > 0
        print(f"  layers-vt-pages-identity {'PASS' if ok else 'FAIL'}  pages on vs off "
              f"{ae_pages} px (want <= {VT_PAGES_ID_CEILING}: the pages hold the same macro), "
              f"{resident_final} pages resident at the end (want > 0, or the identity is the "
              f"feature never arming)")
        if not ok:
            failures.append("layers-vt-pages-identity")

    # --- layers-vt-pages-effect ----------------------------------------------
    # At a fallback the derived rule would refuse, pages carry the macro the
    # fallback cannot: coarse+pages must move far from coarse-only AND land
    # much nearer the full-density reference. Density stated in the fixture's
    # own units: the coarse fallback is 0.5 units/texel, its pages 0.125.
    ae_restore, _ = compare(coarse_pages[1], opath)
    ae_left, _ = compare(coarse_pages[1], nopages[1])
    ae_coarse_ref, _ = compare(opath, nopages[1])
    ok = (ae_restore >= 15000 and ae_coarse_ref > 0
          and ae_left <= int(0.6 * ae_coarse_ref))
    print(f"  layers-vt-pages-effect {'PASS' if ok else 'FAIL'}  coarse+pages moves "
          f"{ae_restore} px off coarse-only (want >= 15000; measured 32289) and sits "
          f"{ae_left} px from the full reference against coarse-only's {ae_coarse_ref} "
          f"(want <= 0.6 of it: pages recover most of what coarseness lost)")
    if not ok:
        failures.append("layers-vt-pages-effect")

    # --- layers-vt-pages-churn -----------------------------------------------
    # Slots forced far under the 25-page virtual grid, plus a --cam-at teleport
    # -- the worst case no walk can produce, every page missing at once. The
    # capacity clamp must hold at every print, the teleport must evict and
    # reload, the tail must be STABLE (loaded stops growing: the anti-thrash),
    # and the whole history must be deterministic across two runs.
    # Feedback OFF here, deliberately: this arm tests the eviction machinery in
    # isolation, and the seen-first boost makes residency smarter than the
    # arm's forcing -- after the teleport the still-visible old pages vote,
    # stay top-ranked, and correctly nothing moves. Feedback's own behaviour
    # has its two arms below.
    # The teleport pose drops the camera low over the fixture's left half,
    # looking along it -- chosen so its nearest-first want only partially
    # overlaps the default framing's, which at 4 slots is what forces both
    # evictions and reloads rather than a want that happens to be resident.
    churn_flags = ["--layers-vt-page-slots", "4", "--no-layers-vt-feedback", "--cam-at",
                   "10:-1.5,2.0,3.0,-1.5,0.0,-1.0"]
    rows_a = vt_probe_run(churn_flags, 24)
    rows_b = vt_probe_run(churn_flags, 24)
    if len(rows_a) < 4:
        print("  layers-vt-pages-churn FAIL  the probe printed too few rows")
        failures.append("layers-vt-pages-churn")
    else:
        cap_ok = all(r["resident"] <= r["slots"] for r in rows_a)
        # loaded > slots is what proves a slot was REFILLED: at cap 4, a fifth
        # load has nowhere to land without an eviction first.
        churned = rows_a[-1]["evicted"] >= 1 and rows_a[-1]["loaded"] > rows_a[-1]["slots"]
        stable = rows_a[-1]["loaded"] == rows_a[-2]["loaded"]
        deterministic = rows_a == rows_b
        ok = cap_ok and churned and stable and deterministic
        print(f"  layers-vt-pages-churn {'PASS' if ok else 'FAIL'}  resident <= slots at "
              f"every print: {cap_ok}; teleport evicted {rows_a[-1]['evicted']} and drove "
              f"loads to {rows_a[-1]['loaded']} against {rows_a[-1]['slots']} slots (want "
              f"churn); tail stable: {stable} (loads still growing = thrash); two runs "
              f"identical: {deterministic}")
        if not ok:
            failures.append("layers-vt-pages-churn")

    # --- layers-vt-pages-walk ------------------------------------------------
    # The integration read, on forest: the derived grid must be the design's own
    # bound (34, from ceil(4 * 2048 / usable) -- a number the gate states, never
    # reads back), residency must follow a real walk under the capacity clamp,
    # and the walk must churn pages the way it churns regions.
    if not os.path.exists(FOREST):
        print("  layers-vt-pages-walk SKIP  (forest not built)")
    else:
        # The regions' own churn walk, through the regions' own runner: same
        # constants, same launch, so this arm cannot drift from the walk the
        # region arms measure -- the probe line just rides along, and the
        # region probe's own output is ignored here by prefix.
        _, _, walk_text = _region_run(REGION_CHURN + REGION_WALK +
                                      ["--layers-vt-probe", "50"])
        rows = _vt_probe_rows(walk_text)
        if len(rows) < 4:
            print("  layers-vt-pages-walk FAIL  the probe printed too few rows")
            failures.append("layers-vt-pages-walk")
        else:
            grid_ok = all(r2["grid"] == 34 for r2 in rows)
            cap_ok = all(r2["resident"] <= r2["slots"] for r2 in rows)
            churned = rows[-1]["evicted"] > 0 and rows[-1]["loaded"] > rows[-1]["slots"]
            # The anti-thrash bound, TWO-SIDED because a walk both churns and
            # settles: healthy measures 79 loads over the round trip (71 on the
            # pre-review vote encoding -- the seen boost follows the decode);
            # evicting wanted pages (the hysteresis-and-want guard deleted)
            # measured 701, a 10x contrast the decode shift cannot blur.
            no_thrash = rows[-1]["loaded"] <= 200
            ok = grid_ok and cap_ok and churned and no_thrash
            print(f"  layers-vt-pages-walk {'PASS' if ok else 'FAIL'}  grid 34 at every "
                  f"print: {grid_ok} (the relative-density bound); resident <= slots: "
                  f"{cap_ok}; the walk loaded {rows[-1]['loaded']} and evicted "
                  f"{rows[-1]['evicted']} against {rows[-1]['slots']} slots (want real "
                  f"churn, as the walk churns regions, and loads <= 200 -- the "
                  f"want-guard deleted measures 701, a 10x thrash)")
            if not ok:
                failures.append("layers-vt-pages-walk")

    # --- layers-vt-feedback-sky ----------------------------------------------
    # A camera aimed at the sky rasterizes no paged surface, so feedback must
    # request ZERO pages -- while prediction's tall conservative frustum boxes
    # still WANT pages, which is the anti-vacuity: zero against a zero want
    # would also describe a pass that never ran.
    sky_rows = vt_probe_run(["--cam-eye", "0,5,8", "--cam-target", "0,60,8"], 24)
    if len(sky_rows) < 3:
        print("  layers-vt-feedback-sky FAIL  the probe printed too few rows")
        failures.append("layers-vt-feedback-sky")
    else:
        ok = sky_rows[-1]["requested"] == 0 and sky_rows[-1]["wanted"] > 0
        print(f"  layers-vt-feedback-sky {'PASS' if ok else 'FAIL'}  aimed at the sky the "
              f"vote pass requested {sky_rows[-1]['requested']} pages (want exactly 0) while "
              f"prediction wanted {sky_rows[-1]['wanted']} (want > 0, or the pass never ran "
              f"and zero proves nothing)")
        if not ok:
            failures.append("layers-vt-feedback-sky")

    # --- layers-vt-feedback-occlusion ----------------------------------------
    # The arm that discriminates feedback from prediction: a low camera behind
    # the 45-degree ramp sees the ramp and NOT the floor beyond its crest, so
    # the floor's pages must cast no votes while the frustum wants all 25 --
    # the fixture's WHOLE virtual grid, ceil(4 * 256 / 248) = 5 per axis at its
    # derived-minimum 256 fallback, squared. The band is two-sided: below it
    # the pass lost the ramp too, above it the depth test stopped rejecting
    # the hidden floor.
    occ_rows = vt_probe_run(["--cam-eye", "0,0.4,-3.5", "--cam-target", "0,0.5,3"], 24)
    if len(occ_rows) < 3:
        print("  layers-vt-feedback-occlusion FAIL  the probe printed too few rows")
        failures.append("layers-vt-feedback-occlusion")
    else:
        req = occ_rows[-1]["requested"]
        ok = 8 <= req <= 14 and occ_rows[-1]["wanted"] == 25
        print(f"  layers-vt-feedback-occlusion {'PASS' if ok else 'FAIL'}  behind the ramp "
              f"the vote pass requested {req} pages (want 8..14, measured 13: the ramp's "
              f"own) while prediction wanted {occ_rows[-1]['wanted']} (want 25 -- the "
              f"occluded floor is frustum-visible, and only the depth-tested vote knows "
              f"it is hidden)")
        if not ok:
            failures.append("layers-vt-feedback-occlusion")

    # --- roads (spec 11.68) --------------------------------------------------
    # Every road arm renders a runtime VARIANT: the committed fixture carries no
    # roads, so its golden and the sixteen arms above cannot move.
    # feather is the only field any arm varies; **material carries anything else
    # the same mutation wants to set, which is what keeps a two-key variant one
    # call rather than a lambda sequencing two side effects.
    def _vt_road(feather=None, **material):
        f = g2.ROAD_FEATHER if feather is None else feather

        def mutate(d):
            mat = d["materials"]["layered_vt_surface"]
            mat["roads"] = [{"points": [[-g2.HALF, g2.ROAD_Z], [g2.HALF, g2.ROAD_Z]],
                             "width": g2.ROAD_WIDTH, "feather": f, "layer": g2.ROAD_LAYER}]
            mat.update(material)
        return mutate

    # The byte at a world z on the floor, which is the only space these arms
    # reason in: _vt_point maps splat coords to world, so this inverts the half
    # of it the caller would otherwise have to invert at every read.
    def road_byte(shot_data, z, u=0.125):
        return _layer_sample(shot_data[0], _vt_point(u, (z + g2.HALF) / g2.DOMAIN))

    ROAD_HALF = g2.ROAD_WIDTH / 2.0

    # --- layers-road-select --------------------------------------------------
    # The road is MADE of layer 2, so it reads that layer's painted byte -- over
    # column 0's grey and column 3's light alike, which is what says the override
    # replaced the splat's choice rather than tinting it. Past the shoulder the
    # ground reads its own byte untouched, and the per-texel leg reads the same
    # bytes as the cache: roads reach both paths through one function.
    road_cached = vt_shot("road", overrides=_vt_road())
    road_plain = vt_shot("road_plain", ["--no-layers-vt"], overrides=_vt_road())
    if not (road_cached and road_plain):
        print("  layers-road-select ERROR while rendering the road legs")
        failures.append("layers-road-select")
    else:
        on = [road_byte(road_cached, g2.ROAD_Z_ON, u) for u in (0.125, 0.875)]
        off = [road_byte(road_cached, g2.ROAD_Z_OFF, u) for u in (0.125, 0.875)]
        par = [road_byte(road_plain, g2.ROAD_Z_ON, u) for u in (0.125, 0.875)]
        base_codes = (g.GREY_CODE, g.LIGHT_CODE)
        on_ok = all(abs(c - g.DARK_CODE) <= 1 for c in on)
        off_ok = all(abs(c - b) <= 1 for c, b in zip(off, base_codes))
        par_ok = all(a == b for a, b in zip(on, par))
        ok = on_ok and off_ok and par_ok
        print(f"  layers-road-select {'PASS' if ok else 'FAIL'}  on the road "
              f"{on} (want {g.DARK_CODE} +/- 1 over BOTH bases -- a tint would "
              f"read two different values), past the shoulder {off} (want "
              f"{list(base_codes)}), per-texel leg {par} (want the cached bytes exactly)")
        if not ok:
            failures.append("layers-road-select")

    # --- layers-road-edge ----------------------------------------------------
    # The arm that proves the override lands BEFORE the height blend. Layer 2 is
    # the tallest in the set, so a road reshaped into the weights overstays its
    # mask where the heights say it should and the transition collapses into
    # LAYER_BLEND_RANGE; a road mixed in AFTER the blend would cross at the
    # mask's own half with the feather's full width, whatever the heights are.
    # Read as the RATIO of the two legs' widths, which is what survives the
    # feather being a tuning value.
    road_lin = vt_shot("road_linear", overrides=_vt_road(layerBlend=0.0))
    if not (road_cached and road_lin):
        print("  layers-road-edge ERROR while rendering the shoulder legs")
        failures.append("layers-road-edge")
    else:
        # Scan out of the road across the shoulder into bare ground.
        z0, z1 = g2.ROAD_Z_ON, g2.ROAD_Z_OFF
        # 48 over ~0.29 world units: finer than the ~0.015 units a screen pixel
        # spans at this framing, so the scan resolves the render rather than
        # under-sampling it.
        steps = 48
        zs = [z0 + (z1 - z0) * i / (steps - 1.0) for i in range(steps)]
        legs = {}
        for tag, shot_data in (("height", road_cached), ("linear", road_lin)):
            vals = [road_byte(shot_data, z) for z in zs]
            legs[tag] = _layer_ramp(vals, zs, g.DARK_CODE, g.GREY_CODE)
        if not legs["height"] or not legs["linear"]:
            print("  layers-road-edge FAIL  a shoulder scan was not monotone")
            failures.append("layers-road-edge")
        else:
            wh, ch = legs["height"]
            wl, cl = legs["linear"]
            ratio = wh / wl if wl > 0 else 99.0
            # The linear leg crosses where the mask does, at distance
            # halfWidth + 0.5 * feather from the centreline.
            lin_want = g2.ROAD_Z + ROAD_HALF + 0.5 * g2.ROAD_FEATHER
            ok = ratio <= 0.35 and abs(cl - lin_want) <= 0.06 and ch > cl
            print(f"  layers-road-edge {'PASS' if ok else 'FAIL'}  the height leg's "
                  f"shoulder is {wh:.4f} wide against the linear twin's {wl:.4f} "
                  f"(ratio {ratio:.3f}, want <= 0.35: the blend hands the transition to "
                  f"LAYER_BLEND_RANGE, where a road mixed in after it would read 1.0); "
                  f"the linear leg crosses at z={cl:.4f} (want {lin_want:.4f} +/- 0.06, "
                  f"the mask's own half) and the proud road holds on further, to "
                  f"z={ch:.4f}")
            if not ok:
                failures.append("layers-road-edge")

    # --- layers-road-pages ---------------------------------------------------
    # The first page content in the suite that is not a 0 px identity, read as
    # BYTES on the road rather than as a frame difference. A whole-frame count
    # cannot say this: the legs differ by a code or two across every flat
    # surface in view (a macro resampled twice), which swamps a road edge in
    # tens of thousands of pixels that have nothing to do with roads.
    #
    # Forced coarse for the reason the generator states: what pages resolve and
    # the fallback cannot is a band four times narrower than a fallback texel,
    # and at the derived 256 that is finer than this fixture's camera resolves
    # at all -- so the comparison is moved to a resolution the frame can show,
    # exactly as layers-vt-pages-effect does.
    coarse_texel = g2.DOMAIN / g2.VT_ROAD_COARSE_RES
    COARSE = ["--layers-vt-res", str(g2.VT_ROAD_COARSE_RES)]
    fine = _vt_road(feather=g2.ROAD_PAGES_FEATHER)
    fine_pages = vt_shot("road_fine_pages", COARSE, overrides=fine)
    fine_nopages = vt_shot("road_fine_nopages", COARSE + ["--no-layers-vt-pages"],
                           overrides=fine)
    fine_truth = vt_shot("road_fine_truth", ["--no-layers-vt"], overrides=fine)
    fine_probe = vt_probe_run(COARSE, 16, overrides=fine, name="road_fine_probe")
    if not (fine_pages and fine_nopages and fine_truth and fine_probe):
        print("  layers-road-pages ERROR while rendering the fine-shoulder legs")
        failures.append("layers-road-pages")
    else:
        # A short RUN just inside the road's edge, not one pixel: the fallback
        # has to average across the edge through this whole band, and reading
        # five points instead of one stops the arm being a single-pixel bet on
        # where a mip boundary lands.
        band = [g2.ROAD_Z_PAGES_IN - 0.008 * k for k in range(5)]
        pv = [road_byte(fine_pages, z) for z in band]
        nv = [road_byte(fine_nopages, z) for z in band]
        tv = [road_byte(fine_truth, z) for z in band]
        p, n, t = pv[0], nv[0], tv[0]
        page_err = max(abs(a - b) for a, b in zip(pv, tv))
        fallback_err = max(abs(a - b) for a, b in zip(nv, tv))
        resident = fine_probe[-1]["resident"]
        ok = (page_err <= 1 and fallback_err >= ROAD_PAGES_SMEAR_MIN and resident > 0)
        print(f"  layers-road-pages {'PASS' if ok else 'FAIL'}  over five reads inside "
              f"the road's edge, against a {coarse_texel:.4f} fallback texel and its "
              f"{coarse_texel / g2.VT_PAGE_DENSITY_RATIO:.4f} pages: the per-texel truth "
              f"reads {t}, pages read {p} and stay within {page_err} of it (want <= 1 -- "
              f"they hold the edge) where the fallback alone reads {n} and drifts "
              f"{fallback_err} (want >= {ROAD_PAGES_SMEAR_MIN}: it averaged across an edge "
              f"it cannot represent), with {resident} pages resident")
        if not ok:
            failures.append("layers-road-pages")

    # --- layers-road-invalidate ----------------------------------------------
    # Roads join the cache's by-value key, and a fresh process cannot show it:
    # it bakes once, from the final authored width. Only a mid-run transition
    # leaves a baked atlas describing a road that is no longer there.
    # The SAME point the select arm proved is bare ground: past the authored
    # shoulder (distance 0.64 > halfWidth 0.2 + feather 0.4) and inside the
    # widened one (0.64 < the new halfWidth 0.8), so it is full road after and
    # untouched ground before, with no blend band at either end.
    wide = g2.ROAD_Z_OFF
    road_widened = vt_shot("road_widened", ["--road-width-at", "10:1.6"],
                           frames=16, overrides=_vt_road())
    if not (road_cached and road_widened):
        print("  layers-road-invalidate ERROR while rendering the widened leg")
        failures.append("layers-road-invalidate")
    else:
        before = road_byte(road_cached, wide)
        after = road_byte(road_widened, wide)
        ok = abs(before - g.GREY_CODE) <= 2 and abs(after - g.DARK_CODE) <= 2
        print(f"  layers-road-invalidate {'PASS' if ok else 'FAIL'}  a point at "
              f"z={wide:.3f} reads {before} at the authored width (want "
              f"{g.GREY_CODE} +/- 2: bare ground) and {after} after a frame-10 "
              f"widening to 1.6 (want {g.DARK_CODE} +/- 2; a cache that ignored "
              f"roads in its key keeps reading the ground)")
        if not ok:
            failures.append("layers-road-invalidate")

    failures += _layers_scatter_arm()
    return failures


# Stated here rather than read out of the app: see the arm below. It is the
# midpoint of terrain.h's TERRAIN_CHANNEL_FLOW_LO/HI band -- where gravel stops
# being a tint and becomes what the ground is made of -- and the arm asserts the
# app agrees, so the two can drift apart loudly rather than silently.
LAYERS_SCATTER_LIMIT = 0.73

_SCATTER_PROBE = re.compile(
    r"scatter-probe trees=(\d+) tree_flow_max=([\d.]+) tree_flow_mean=([\d.]+) "
    r"land_flow_mean=([\d.]+) land_frac_over=([\d.]+) limit=([\d.]+)")

# The trail's own row (spec 11.68), read from the SAME forest run as the row
# above -- a second launch would cost the suite a whole forest startup to learn
# something the first one already printed.
_SCATTER_ROAD = re.compile(
    r"scatter-probe road points=(\d+) trees_on=(\d+) rejected=(\d+) width=([\d.]+)")

# The trail's authored width, stated gate-side as a DRIFT ALARM: it catches the
# trail shrinking to nothing or the probe reporting a width the app is not using.
# (Not the LAYERS_SCATTER_LIMIT argument, which is a different one -- there,
# raising the app's limit would raise the bar it was checked against, so the arm
# could never fail that way. Widening the trail weakens nothing here, because
# trees_on is counted at the app's own width.)
LAYERS_TRAIL_WIDTH = 3.0
# Floored near the measurement rather than at 1 and 2. Healthy reads 165
# rejections over a 10-point course (truncated from 12 at the shoal).
LAYERS_TRAIL_MIN_REJECTED = 60
LAYERS_TRAIL_MIN_POINTS = 6


def _layers_scatter_arm():
    """The scatter places into ground the splat has not painted as a stream bed.

    Runs with --erode because the whole claim is about the erosion masks, and
    without a bake they read zero: the placement rule is then trivially satisfied
    and an arm checking only the trees would pass against a build that never
    implemented any of it. So the terrain's OWN drainage range is asserted too,
    and that is what makes this a test rather than a tautology.
    """
    # FOREST, not a hardcoded out/bin: --bin-dir rebinds it, and running every
    # other forest arm against release while this one silently measured the
    # debug binary is exactly the "which binaries a number describes" mistake
    # the suite prints its resolved directory to avoid.
    forest = FOREST
    if not os.path.exists(forest):
        print("  layers-scatter    SKIP  (forest not built)")
        return []

    cmd = [forest, "-x", "-f", "1", "-W", "200", "-H", "120", "--erode", "--erode-res", "256",
           "--erode-iterations", "120", "--scatter-probe"]
    r = _run(cmd, capture_output=True, text=True, cwd=ROOT)
    m = _SCATTER_PROBE.search(r.stdout + r.stderr)
    if not m:
        print("  layers-scatter    ERROR  the run printed no probe row")
        return ["layers-scatter"]

    trees = int(m.group(1))
    tree_max, tree_mean = float(m.group(2)), float(m.group(3))
    land_mean, land_over = float(m.group(4)), float(m.group(5))
    limit = float(m.group(6))

    # The bar is stated HERE, not read out of the process under test. Taking it
    # from the probe's own `limit=` meant raising TREE_MAX_FLOW -- the exact
    # weakening this arm exists to prevent -- also raised the bar it was checked
    # against, so the arm could never fail that way.
    holds = tree_max <= LAYERS_SCATTER_LIMIT + 1e-4
    agrees = abs(limit - LAYERS_SCATTER_LIMIT) < 1e-4
    # And the anti-vacuity check is the FRACTION of the scatter's domain the rule
    # rejects, not the peak. erosion.c normalises flow to its own maximum, so a
    # peak near 1.0 is true of any sim that ran at all and says nothing about
    # whether there were candidates to reject.
    has_channels = land_over >= 0.02
    prefers_dry = tree_mean < land_mean
    ok = trees > 0 and holds and agrees and has_channels and prefers_dry
    print(f"  layers-scatter    {'PASS' if ok else 'FAIL'}  {trees} trees, drainage max "
          f"{tree_max:.4f} against a gate-side limit of {LAYERS_SCATTER_LIMIT:.4f} (the app "
          f"says {limit:.4f}); {land_over * 100:.1f}% of the scatter's domain is above it "
          f"(want >= 2%, or the rule rejected nothing and this proves nothing), tree mean "
          f"{tree_mean:.4f} vs land {land_mean:.4f}")
    failures = [] if ok else ["layers-scatter"]

    # --- layers-road-scatter (spec 11.68) ------------------------------------
    # Nothing stands on the trail, off the SAME run. `rejected` is the
    # anti-vacuity and it is the load-bearing half: "no tree is on the road" is
    # satisfied perfectly by a build with no road at all, or one whose trail ran
    # where nothing would have grown anyway.
    mr = _SCATTER_ROAD.search(r.stdout + r.stderr)
    if not mr:
        print("  layers-road-scatter ERROR  the run printed no trail row")
        return failures + ["layers-road-scatter"]
    points, on_road = int(mr.group(1)), int(mr.group(2))
    rejected, width = int(mr.group(3)), float(mr.group(4))
    # The reject ran at half-width plus feather plus margin and this counts at
    # the bare half-width, so a CPU distance that had drifted from the shader's
    # by less than the whole shoulder still reads zero here -- which is why the
    # rejected count, not this one, is what says the rule ran.
    #
    # And both bars are FLOORED NEAR THE MEASUREMENT, which is the lesson the
    # arm above wrote down: "rejected >= 1" is satisfied by a trail crossing one
    # candidate's worth of ground, and "points >= 2" by a trail truncated to a
    # stub. Measured 165 of 2101 candidates (7.9%) over a 10-point course.
    road_ok = (points >= LAYERS_TRAIL_MIN_POINTS and on_road == 0
               and rejected >= LAYERS_TRAIL_MIN_REJECTED
               and abs(width - LAYERS_TRAIL_WIDTH) < 1e-4)
    print(f"  layers-road-scatter {'PASS' if road_ok else 'FAIL'}  a {points}-point trail "
          f"{width:.2f} wide (the gate says {LAYERS_TRAIL_WIDTH:.2f}, want >= "
          f"{LAYERS_TRAIL_MIN_POINTS} points) carries {on_road} trees (want 0) and turned "
          f"{rejected} candidates away (want >= {LAYERS_TRAIL_MIN_REJECTED}, measured 165: a "
          f"floor of 1 is satisfied by a trail that crossed one candidate and proves nothing)")
    if not road_ok:
        failures.append("layers-road-scatter")
    return failures
