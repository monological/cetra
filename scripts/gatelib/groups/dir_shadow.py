"""The dir-shadow group: cascade shadow (analytic ellipse)."""

import os

from gatelib.harness import asset
from gatelib.lighting import (
    DIR_CHURN_FACTOR, DIR_CHURN_SLACK_PX, DIR_ERODE, DIR_SHADOW, _crossing, _dir_ellipse,
    _dir_render, _dir_visible, _taa_churn, _term_reader,
)


DIR_HOLE_MAX = 0.02   # umbra term; a single authored light means true umbra ~ 0
# The pillar's ground band, from the same projection the ellipses use: a point at
# height h lands at z - h/tan(elev), so the 0.3 x 0.3 x 3.0 pillar at x = -3.5
# shadows z = 0.15 back to z = -3.725. Bounds below are eroded off both ends,
# clear of the tip and of the pillar's own footprint.
#
# A note on why this arm exists, since the hole arm above looks like it covers
# the same ground: it does not, and that was measured rather than assumed.
# Swapping the four-moment reconstruction for the two-moment Chebyshev bound
# (VSM -- the technique specs/10.4 declined for leaking) moves 6339 px of the
# frame and leaves the hole arm reading 0.0084, to four decimals, unchanged. The
# eroded ellipse interiors are where every reconstruction agrees; the difference
# lives on the ellipse RIMS, which DIR_ERODE deliberately excludes, and on this
# band. A slab seen near edge-on by the map puts almost no depth SPREAD in a
# texel footprint but a large caster-receiver gap behind it, which is the
# configuration two moments cannot represent and four can.
DIR_PILLAR_X = (-3.61, -3.39)
DIR_PILLAR_Z = (-3.2, -0.6)
# PCF reads 0.0000 here and the moment path at the shipped blur reads 0.0074, so
# this sits 2.7x above what passes and 21x below what fails (blur 1.0 leaks
# 0.4159, the two-moment bound 0.4249).
DIR_PILLAR_MAX = 0.02
# The inverse half, and the arm that makes the other six mean anything.
#
# Every MSM arm asserts the moment path is no WORSE than PCF -- and
# shadow_build_msm falls back to the depth cascades without logging on three
# paths (flag unparsed, program missing, allocation failed), which renders a
# frame byte-identical to --no-msm and passes all six while measuring nothing.
# That is the defect run_flare_gate records at its own arm 1 and run_oit_gate
# guards with OIT_DISCRIMINATION_MIN.
#
# Blur is the knob that provably moves this band, so leaking under it is what
# says the moments were built and read. Bound is a fifth of the 0.4159 measured
# at spacing 1.0 -- far above PCF's 0.0000, far below the reading it checks for.
DIR_PILLAR_BLUR_MIN = 0.10
DIR_ACNE_TOL = 0.05   # |term - 1| on bare ground; acne and false-dark are 10x+
DIR_EDGE_TOL = 0.05   # world units; ~3 outermost texels


def run_dir_shadow_gate(workdir):
    fixture = asset("dir_shadow_fixture.cscn")
    if not os.path.exists(fixture):
        print("  dir-shadow   SKIP  (missing dir_shadow_fixture.cscn)")
        return []
    failures = []

    # --- fixture renders ----------------------------------------------------
    # --no-pcss for every positional measure: PCSS is on by default with an
    # emitter of scene_radius * 0.08, which smears the analytic edge. The moment
    # arms name it too, even though --msm clears PCSS by itself, so the two
    # paths differ by exactly one flag and neither inherits a live default.
    hard = ["--no-pcss"]
    msm = hard + ["--msm"]
    r40 = _dir_render(workdir, "dir_shadow_fixture.cscn", "40", hard)
    r40_ns = _dir_render(workdir, "dir_shadow_fixture.cscn", "40_ns", hard + ["--no-shadows"])
    r40_cc1 = _dir_render(workdir, "dir_shadow_fixture.cscn", "40_cc1",
                          hard + ["--shadow-cascades", "1"])
    r10 = _dir_render(workdir, "dir_shadow_fixture_lowsun.cscn", "10", hard)
    r10_ns = _dir_render(workdir, "dir_shadow_fixture_lowsun.cscn", "10_ns",
                         hard + ["--no-shadows"])
    # Moment shadow maps (spec 11.22). The --no-shadows references are what the
    # shadow term divides BY and carry no shadow algorithm at all, so they are
    # shared rather than re-rendered: three extra frames, not six.
    m40 = _dir_render(workdir, "dir_shadow_fixture.cscn", "40_msm", msm)
    m40_cc1 = _dir_render(workdir, "dir_shadow_fixture.cscn", "40_cc1_msm",
                          msm + ["--shadow-cascades", "1"])
    m10 = _dir_render(workdir, "dir_shadow_fixture_lowsun.cscn", "10_msm", msm)
    # Deliberately blurred, for the inverse arm below. It is the only render
    # here whose job is to FAIL a bound rather than pass one.
    mblur = _dir_render(workdir, "dir_shadow_fixture.cscn", "40_msm_blur",
                        msm + ["--msm-blur", "1.0"])
    if not all((r40, r40_ns, r40_cc1, r10, r10_ns, m40, m40_cc1, m10, mblur)):
        print("  dir-shadow   ERROR while rendering the fixture")
        return failures + ["dir-shadow"]

    term40 = _term_reader(r40, r40_ns)
    term40_cc1 = _term_reader(r40_cc1, r40_ns)
    term10 = _term_reader(r10, r10_ns)
    mterm40 = _term_reader(m40, r40_ns)
    mterm40_cc1 = _term_reader(m40_cc1, r40_ns)
    mterm10 = _term_reader(m10, r10_ns)
    mterm_blur = _term_reader(mblur, r40_ns)

    # --- holes: eroded umbra of both ellipses must be dark ------------------
    # Light leaking through an umbra is the signature failure of every filterable
    # shadow representation -- the reason specs/10.4 declined VSM and the reason
    # fog_esm_frag confines ESM to the medium. For the moment path this arm is
    # not a regression check but the measurement the technique lives or dies on.
    for label, term in (("pcf", term40), ("msm", mterm40)):
        try:
            worst_hole = 0.0
            measured = skipped = 0
            for centre in (DIR_SHADOW["float_c"], DIR_SHADOW["rest_c"]):
                cx, cz, sx, sz = _dir_ellipse(centre, DIR_SHADOW["elev_deg"])
                for iu in range(-4, 5):
                    for iv in range(-4, 5):
                        u, v = iu / 4.0, iv / 4.0
                        if u * u + v * v > 1.0:
                            continue
                        p = (cx + DIR_ERODE * sx * u, 0.0, cz + DIR_ERODE * sz * v)
                        if not _dir_visible(p):
                            skipped += 1
                            continue
                        worst_hole = max(worst_hole, term(p))
                        measured += 1
            if measured < 30:
                # No silent caps: enough of the umbra must actually be measured.
                print(f"  hole-{label:<7}ERROR only {measured} umbra samples visible "
                      f"({skipped} occluded)")
                failures.append(f"dir-hole-{label}")
            else:
                ok = worst_hole <= DIR_HOLE_MAX
                print(f"  hole-{label:<7}{'PASS' if ok else 'FAIL'}  worst umbra term "
                      f"{worst_hole:.4f} over {measured} samples, {skipped} caster-occluded "
                      f"(want <= {DIR_HOLE_MAX})")
                if not ok:
                    failures.append(f"dir-hole-{label}")
        except ValueError as e:
            print(f"  hole-{label:<7}ERROR {e}")
            failures.append(f"dir-hole-{label}")

    # --- pillar band: the thin caster, where reconstructions differ ---------
    # Hoisted: the band is fixture geometry and carries no shadow technique, so
    # rebuilding it per label would report one geometry fault once per label.
    pillar_pts = [(DIR_PILLAR_X[0] + (DIR_PILLAR_X[1] - DIR_PILLAR_X[0]) * ix / 2.0, 0.0,
                   DIR_PILLAR_Z[0] + (DIR_PILLAR_Z[1] - DIR_PILLAR_Z[0]) * iz / 12.0)
                  for ix in range(3) for iz in range(13)]
    for label, term in (("pcf", term40), ("msm", mterm40)):
        try:
            worst = max(term(p) for p in pillar_pts)
            ok = worst <= DIR_PILLAR_MAX
            print(f"  pillar-{label:<5} {'PASS' if ok else 'FAIL'}  worst band term {worst:.4f} "
                  f"over {len(pillar_pts)} samples (want <= {DIR_PILLAR_MAX})")
            if not ok:
                failures.append(f"dir-pillar-{label}")
        except ValueError as e:
            print(f"  pillar-{label:<5} ERROR {e}")
            failures.append(f"dir-pillar-{label}")

    try:
        leak = max(mterm_blur(p) for p in pillar_pts)
        ok = leak >= DIR_PILLAR_BLUR_MIN
        print(f"  pillar-blur  {'PASS' if ok else 'FAIL'}  blurred moments leak {leak:.4f} "
              f"(want >= {DIR_PILLAR_BLUR_MIN}; under it the moment path never ran)")
        if not ok:
            failures.append("dir-pillar-blur")
    except ValueError as e:
        print(f"  pillar-blur  ERROR {e}")
        failures.append("dir-pillar-blur")

    # --- acne: bare ground must divide to 1, at both elevations -------------
    for label, term in (("40deg", term40), ("10deg", term10),
                        ("40msm", mterm40), ("10msm", mterm10)):
        try:
            worst = 0.0
            (xa, xb), (za, zb) = DIR_SHADOW["strip_x"], DIR_SHADOW["strip_z"]
            steps = 10
            for ix in range(steps + 1):
                for iz in range(steps + 1):
                    p = (xa + (xb - xa) * ix / steps, 0.0, za + (zb - za) * iz / steps)
                    worst = max(worst, abs(term(p) - 1.0))
            ok = worst <= DIR_ACNE_TOL
            print(f"  acne-{label:<7} {'PASS' if ok else 'FAIL'}  worst |term-1| {worst:.4f} "
                  f"(want <= {DIR_ACNE_TOL})")
            if not ok:
                failures.append(f"dir-acne-{label}")
        except ValueError as e:
            print(f"  acne-{label:<7} ERROR {e}")
            failures.append(f"dir-acne-{label}")

    # --- edge position: 50% crossing of the floating sphere's near edge -----
    cx, cz, sx, sz = _dir_ellipse(DIR_SHADOW["float_c"], DIR_SHADOW["elev_deg"])
    want_edge = cz + sz
    # A symmetric filter cannot move the 50% crossing of a step, so this is what
    # says the moment blur SOFTENS the edge rather than displacing it -- the
    # failure mode that kept specs/10.4 Phase 4 from shipping.
    for label, term in (("cc3", term40), ("cc1", term40_cc1),
                        ("cc3msm", mterm40), ("cc1msm", mterm40_cc1)):
        zs = [want_edge - 0.5 + i * 0.002 for i in range(int(1.0 / 0.002))]
        try:
            vals = [term((cx, 0.0, z)) for z in zs]  # umbra -> lit as z rises
        except ValueError as e:
            print(f"  edge-{label:<7} ERROR {e}")
            failures.append(f"dir-edge-{label}")
            continue
        umbra, lit = min(vals), max(vals)
        if lit - umbra < 0.5:
            print(f"  edge-{label:<7} ERROR no shadow edge on the scan")
            failures.append(f"dir-edge-{label}")
            continue
        mid = _crossing(zs, vals, umbra, lit, 0.50)
        ok = abs(mid - want_edge) <= DIR_EDGE_TOL
        print(f"  edge-{label:<7} {'PASS' if ok else 'FAIL'}  crossing {mid:.4f} "
              f"(want {want_edge:.4f} +/-{DIR_EDGE_TOL})")
        if not ok:
            failures.append(f"dir-edge-{label}")

    # --- churn: shadows must not add temporal instability under TAA ---------
    # Static camera, jitter on (windowed parity). Frames 90 and 120 of the same
    # run are compared; the --no-shadows pair is the noise floor everything else
    # in the pipeline contributes, and the shadowed pair must stay at it.
    floor = _taa_churn(workdir, fixture, "dir_ns", hard + ["--no-shadows"])
    if floor is None:
        print("  churn        ERROR while rendering the no-shadow floor")
        failures.append("dir-churn")
    else:
        for label, extra in (("pcf", hard), ("msm", msm)):
            churn = _taa_churn(workdir, fixture, f"dir_{label}", extra)
            if churn is None:
                print(f"  churn-{label:<6}ERROR while rendering the TAA sequence")
                failures.append(f"dir-churn-{label}")
                continue
            ok = churn <= floor * DIR_CHURN_FACTOR + DIR_CHURN_SLACK_PX
            print(f"  churn-{label:<6}{'PASS' if ok else 'FAIL'}  {churn} px frame-to-frame "
                  f"(no-shadow floor {floor} px)")
            if not ok:
                failures.append(f"dir-churn-{label}")

    return failures
