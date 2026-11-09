"""The translucent and translucent-offpath groups:

  translucent         translucent shadows (analytic layer stack, spec 11.26 / C1)
  translucent-offpath translucent shadows (off-path identity and the inverse arm)
"""

import math
import os

from gatelib.harness import NO_HALOS, RENDER, ROOT, _run, asset
from gatelib.images import _oit_untonemap, _read_ppm, compare
from gatelib.scenes import _cscn_camera, _projector


# Translucent shadows (spec 11.26 / C1), on assets/translucent_shadow_fixture.
#
# The fixture's whole point is that the answer is knowable: transmittance
# through N translucent layers is prod(1 - a), which is ORDER-INDEPENDENT and
# equal to exp of the summed absorbances. So these arms constrain the ANSWER and
# not the storage -- an implementation that accumulates absorbance additively
# and one that multiplies transmittances satisfy them identically.
TSL_ALPHA = 0.35                # authored on the staircase panels
TSL_PANELS = 4
TSL_STACK_TOL = 0.02            # absolute error against 0.65^k
TSL_RATIO_TOL = 0.03            # per-layer ratio, measured without a reference
TSL_RAMP_RMS_MAX = 0.03         # ramp against 1 - alpha(x)
TSL_RAMP_DISCRIM = 3.0          # ... and how much better than a binary step
TSL_OPAQUE_MAX = 0.02           # an opaque umbra must stay an umbra
TSL_ACNE_TOL = 0.05             # bare ground must stay lit
TSL_MONO_TOL = 0.02             # the red panel is monochrome as shipped
TSL_LIVE_MIN_PX = 50000         # the inverse arm: the flag must DO something

# Geometry mirrors of the generator. Not read from the .gltf: these are the
# numbers the PREDICTION uses, and a gate that derived them from the asset would
# agree with a broken asset.
TSL_BAND_X0, TSL_BAND_W = -7.0, 2.0
TSL_CANOPY_XR = 3.0
TSL_SHIFT = 8.0 / math.tan(math.radians(75.0))   # sun elevation 75
TSL_STAIR_ZC = -5.5 + TSL_SHIFT                  # centre of the cast band
TSL_RAMP_Z0, TSL_RAMP_Z1 = -3.5, -1.5
TSL_RAMP_ZC = 0.5 * (TSL_RAMP_Z0 + TSL_RAMP_Z1) + TSL_SHIFT
TSL_RED_ZC = 0.0 + TSL_SHIFT
# Lit reference taken just clear of the canopy, NOT at the frame edge: the
# default vignette is radial, so a reference out at x=-10 sits darker than the
# bands it normalises and pulls every transmittance up. --no-vignette below
# removes it; sampling near the bands keeps the arm honest if it ever returns.
TSL_LIT_X = 4.0
# A point in the opaque block's cast umbra that the camera can actually SEE.
# The block is 1.6 tall at elevation 75, so it shifts its own shadow only 0.43
# in z and stands in front of the rest: the umbra runs z -3.57..-1.57 but the
# block's own footprint hides it back to -2.0, leaving a 0.43-deep sliver.
# Centred in that sliver, and in the block's x. The first version sampled
# (6, 0, -2.57) -- inside the block, and off an 800-wide frame at px 809.
TSL_OPAQUE_SAMPLE = (6.0, 0.0, -1.786)


def _tsl_render(workdir, tag, extra):
    out = os.path.join(workdir, f"tsl_{tag}.ppm")
    scene = asset("translucent_shadow_fixture.cscn")
    # --no-ssao is load-bearing, not tidiness: the arms compare shadowed ground
    # against lit ground, and GTAO darkens near the panels by an amount the open
    # reference does not get. Measured: it biased every band ~2.8%. The rest
    # keeps anything that could add to flat lit ground out of frame.
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "-W", "800", "-H", "500",
           "--no-auto-exposure", "-E", "1.0", "--no-pcss", "--no-ssao", "--no-ssr",
           *NO_HALOS, "--no-dither", "--no-vignette", "-S", out] + extra
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    return out


def _tsl_reader(path):
    """World point -> linear radiance, through the CORRECT tonemap inversion.

    NOT _term_reader: that uses _linear_luma, which inverts the sRGB curve but
    not Khronos Neutral, and reads ~23% low. Its own arms are immune (they sit
    at ~0, ~1 and 50% crossings); an assertion of the form T = (1-a)^k is not.
    """
    w, h, pix = _read_ppm(path)
    cam = _cscn_camera("translucent_shadow_fixture.cscn")
    project = _projector(cam, w, h)

    def at(p):
        px, py = project(p)
        x, y = int(round(px)), int(round(py))
        # Raising rather than clamping, because clamping is how a sample walks
        # off the frame and keeps reporting: the first opaque-umbra point
        # projected to px 809 of 800 and silently read the frame's right edge.
        if not (0 <= x < w and 0 <= y < h):
            raise AssertionError(f"sample {p} projects to ({px:.1f}, {py:.1f}), outside {w}x{h}")
        o = (y * w + x) * 3
        return _oit_untonemap([pix[o + k] / 255.0 for k in range(3)])

    return at


def run_translucent_shadow_gate(workdir):
    fixture = asset("translucent_shadow_fixture.cscn")
    if not os.path.exists(fixture):
        print("  tsl          SKIP  (missing translucent_shadow_fixture.cscn)")
        return []

    on = _tsl_render(workdir, "on", ["--translucent-shadows"])
    if on is None:
        print("  tsl          ERROR while rendering the fixture")
        return ["tsl"]
    at = _tsl_reader(on)

    def lum(p):
        return sum(at(p)) / 3.0

    lit = lum((TSL_LIT_X, 0.0, TSL_STAIR_ZC))

    # The zero of the scale is 0 BY CONSTRUCTION, not measured: the fixture
    # declares one directional light and no environment, so a fully shadowed
    # ground texel receives nothing. Taking it from the opaque block's umbra
    # instead made tsl-opaque a tautology -- it asserted that (black - black)
    # was under a bound, which is exactly 0 for any renderer whatsoever, and
    # would have passed a shader that flooded every umbra with full light.
    def transmittance(p):
        return lum(p) / lit

    failures = []

    # --- the staircase, absolute -------------------------------------------
    bands = [transmittance((TSL_BAND_X0 + (j + 0.5) * TSL_BAND_W, 0.0, TSL_STAIR_ZC))
             for j in range(TSL_PANELS + 1)]
    pred = [(1.0 - TSL_ALPHA) ** j for j in range(TSL_PANELS + 1)]
    worst = max(abs(b - p) for b, p in zip(bands, pred))
    ok = worst <= TSL_STACK_TOL
    print(f"  tsl-stack    {'PASS' if ok else 'FAIL'}  worst {worst:.4f} against 0.65^k "
          f"(bound {TSL_STACK_TOL}); measured "
          + " ".join(f"{b:.3f}" for b in bands))
    if not ok:
        failures.append("tsl-stack")

    # --- the staircase, as ratios, with NO reference frame ------------------
    # Deliberately a second path to the same claim: this one is immune to the
    # tonemap inversion, the exposure pin and the black point, so the two arms
    # cannot fail for the same instrument reason.
    raw = [lum((TSL_BAND_X0 + (j + 0.5) * TSL_BAND_W, 0.0, TSL_STAIR_ZC))
           for j in range(TSL_PANELS + 1)]
    ratios = [raw[j + 1] / raw[j] for j in range(TSL_PANELS) if raw[j] > 1e-6]
    worst_r = max(abs(r - (1.0 - TSL_ALPHA)) for r in ratios) if ratios else 1.0
    ok = worst_r <= TSL_RATIO_TOL
    print(f"  tsl-ratio    {'PASS' if ok else 'FAIL'}  worst |ratio - 0.65| {worst_r:.4f} "
          f"(bound {TSL_RATIO_TOL}); " + " ".join(f"{r:.3f}" for r in ratios))
    if not ok:
        failures.append("tsl-ratio")

    # --- the mask ramp ------------------------------------------------------
    # A LINEAR ramp is preserved exactly by any symmetric filter -- PCF,
    # bilinear, a mip -- so filtering cannot bias this and only a wrong law can.
    # A binary alpha test at the cutoff is the law this is here to reject.
    xs = [TSL_BAND_X0 + TSL_BAND_W + (TSL_CANOPY_XR - (TSL_BAND_X0 + TSL_BAND_W)) *
          (0.1 + 0.8 * i / 23.0) for i in range(24)]
    meas = [transmittance((x, 0.0, TSL_RAMP_ZC)) for x in xs]
    truth = [1.0 - (x - (TSL_BAND_X0 + TSL_BAND_W)) /
             (TSL_CANOPY_XR - (TSL_BAND_X0 + TSL_BAND_W)) for x in xs]
    rms = math.sqrt(sum((m - t) ** 2 for m, t in zip(meas, truth)) / len(meas))
    ok = rms <= TSL_RAMP_RMS_MAX
    print(f"  tsl-ramp     {'PASS' if ok else 'FAIL'}  RMS {rms:.4f} against 1-alpha(x) "
          f"(bound {TSL_RAMP_RMS_MAX})")
    if not ok:
        failures.append("tsl-ramp")

    # The inverse of the arm above. Without it, a fixture that stopped
    # resolving the ramp scores its BEST possible mark and asserts nothing.
    best_step = min(
        math.sqrt(sum((m - (1.0 if x < c else 0.0)) ** 2 for m, x in zip(meas, xs)) / len(meas))
        for c in xs)
    ok = rms * TSL_RAMP_DISCRIM <= best_step
    print(f"  tsl-discrim  {'PASS' if ok else 'FAIL'}  best binary step RMS {best_step:.4f} "
          f"must be >= {TSL_RAMP_DISCRIM}x the ramp's {rms:.4f}")
    if not ok:
        failures.append("tsl-discrim")

    # --- solid stays solid, bare stays bare, both in THIS frame -------------
    # The gap in z BETWEEN the stair cast band (ends -2.36) and the ramp's
    # (starts -1.30), sampled as a strip. Deliberately not band 0, which
    # tsl-stack already reads -- that would restate tsl-stack at a looser bound
    # and catch nothing new. Not the x < -8 bare strip either: at this camera
    # the frame only reaches x = -7.5, so those samples are off-picture.
    acne = [transmittance((x, 0.0, -1.83)) for x in (-4.0, -2.0, 0.0, 2.0, 4.0)]
    worst_acne = max(abs(a - 1.0) for a in acne)
    ok = worst_acne <= TSL_ACNE_TOL
    print(f"  tsl-acne     {'PASS' if ok else 'FAIL'}  worst |T-1| over uncovered ground "
          f"{worst_acne:.4f} (bound {TSL_ACNE_TOL}: a target never cleared darkens everything)")
    if not ok:
        failures.append("tsl-acne")

    opaque_t = transmittance(TSL_OPAQUE_SAMPLE)
    ok = abs(opaque_t) <= TSL_OPAQUE_MAX
    print(f"  tsl-opaque   {'PASS' if ok else 'FAIL'}  opaque umbra reads {opaque_t:.4f} "
          f"(want <= {TSL_OPAQUE_MAX}: transmittance must not leak onto opaque casters)")
    if not ok:
        failures.append("tsl-opaque")

    # --- monochrome, as shipped --------------------------------------------
    # Two claims on one sample, because the spread alone is 0 on bare ground and
    # 0 in a full umbra -- it passes if the sample walks off the panel. Pinning
    # the level too says the sample IS under the panel, and checks the
    # single-layer prediction at an alpha the staircase does not use.
    red = at((0.0, 0.0, TSL_RED_ZC))
    spread = max(red) - min(red)
    red_t = (sum(red) / 3.0) / lit
    ok = spread <= TSL_MONO_TOL and abs(red_t - 0.5) <= TSL_STACK_TOL
    print(f"  tsl-mono     {'PASS' if ok else 'FAIL'}  red panel T {red_t:.4f} (want 0.5 "
          f"+/-{TSL_STACK_TOL}) at a channel spread of {spread:.4f} (want <= {TSL_MONO_TOL})")
    if not ok:
        failures.append("tsl-mono")

    return failures


def run_translucent_offpath_gate(workdir):
    """Off is byte-identical; ON is byte-identical too where nothing is
    translucent; and the flag must visibly DO something where something is.

    The third arm is the one 11.22 had to learn twice: three zeros are also what
    a dead flag, an unparsed argument or a failed allocation produce.
    """
    failures = []
    for name, rel, extra in (
        ("cornell_point", "assets/scenes/cornell_point.cscn", ["-W", "800", "-H", "600"]),
        ("dir_shadow", "assets/scenes/dir_shadow_fixture.cscn", ["-W", "800", "-H", "600", "--no-pcss"]),
        ("contact", "assets/scenes/contact_fixture.cscn", ["-W", "640", "-H", "400"]),
    ):
        scene = os.path.join(ROOT, rel)
        if not os.path.exists(scene):
            print(f"  tsl-off      SKIP  (missing {rel})")
            continue
        outs = []
        for tag, flag in (("off", []), ("on", ["--translucent-shadows"])):
            out = os.path.join(workdir, f"tsloff_{name}_{tag}.ppm")
            cmd = [RENDER, "-m", scene, "-x", "-f", "30", "--no-auto-exposure", "-E", "1.0",
                   "-S", out] + extra + flag
            r = _run(cmd, capture_output=True, text=True)
            # The exit code, not just the file: a render that dies partway can
            # leave a short or half-written PPM behind, and existence alone
            # would read that as a successful frame and compare it. The child's
            # output goes with the error, because a gate that says only "ERROR"
            # gives the next person nothing to work from.
            if r.returncode != 0:
                print(f"  tsl-off      ERROR {name}/{tag} exited {r.returncode}: "
                      f"{(r.stdout + r.stderr).strip()[-300:]}")
                outs.append(None)
                continue
            outs.append(out if os.path.exists(out) else None)
        if not all(outs):
            print(f"  tsl-off      ERROR while rendering {name}")
            failures.append("tsl-offpath")
            continue
        ae, _ = compare(outs[0], outs[1])
        ok = ae == 0
        print(f"  tsl-off      {'PASS' if ok else 'FAIL'}  {name}: {ae} px with the flag ON "
              f"(want exactly 0; nothing here is translucent)")
        if not ok:
            failures.append("tsl-offpath")

    off = _tsl_render(workdir, "live_off", ["--no-translucent-shadows"])
    on = _tsl_render(workdir, "live_on", ["--translucent-shadows"])
    if off is None or on is None:
        print("  tsl-live     ERROR while rendering the fixture pair")
        return failures + ["tsl-live"]
    ae, _ = compare(off, on)
    ok = ae >= TSL_LIVE_MIN_PX
    print(f"  tsl-live     {'PASS' if ok else 'FAIL'}  {ae} px between off and on "
          f"(want >= {TSL_LIVE_MIN_PX}: the three zeros above are also what a dead flag gives)")
    if not ok:
        failures.append("tsl-live")
    return failures
