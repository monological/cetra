"""The moon group: the moon: disc, derived phase, terminator and a second casting light (spec
11.82).
"""

import json
import math
import os

from gatelib.harness import NO_HALOS, _probe_render, asset, render
from gatelib.images import _SRGB_TO_LINEAR, _read_ppm, compare
from gatelib.scenes import _projector, cscn_copy
from gatelib.sky import (
    CYCLE_LATITUDE, CYCLE_QUIESCE_HOUR, NIGHTFLOOR_GROUND_BAND, STARS_BAND, STARS_FIXTURE,
    STARS_PIN, _cycle_sun_path, _moon_dir, _moon_disc_geometry, _moon_face_pixels, _stars_delta,
)


# --- the moon (spec 11.82): an analytic disc, a derived phase, a second casting light ---
#
# Same fixture, same pinning and the same twin-delta doctrine as the stars, floor and
# cycle groups above -- every quantitative PIXEL read is the moon CONTRIBUTION, the
# difference against a --no-moon twin at the same configuration, never a raw brightness.
# The one raw read is moon-probe's, and that is arithmetic rather than radiance.
#
# STARS_FIXTURE, STARS_PIN, STARS_BAND and NIGHTFLOOR_GROUND_BAND are reused rather than
# restated: four night groups share one fixture and one pair of bands, and a second copy
# is a second thing to drift.
#
# WHY THE MOON SITS WHERE IT DOES, which is geometry and not taste. With the moon at
# elevation E and the sun rolled about the moon vector at elongation e, the sun's
# elevation on the worst roll is asin(sin(e + E)) -- so the sun CLEARS THE HORIZON unless
# e > 180 - E. moon-terminator needs four rolls, hence a high moon; moon-lit needs a
# CRESCENT rung (e = 45), which forces the moon low. Two placements, each derived.
MOON_EL_HIGH = 78.0
MOON_EL_LOW = 15.0
# A THIRD placement, for moon-env alone, and it is derived rather than chosen: that arm
# needs a FULL moon at a 40-degree diagnostic disc, a full moon puts the sun at its exact
# antipode, and the sun's own disc must clear the horizon -- so the moon has to stand
# above the disc's 20-degree half-angle with margin.
MOON_EL_ENV = 30.0
MOON_AZ = 200.0
MOON_ELONG_ROLL = 115.0
MOON_ROLLS = [0.0, 90.0, 180.0, 270.0]
# ELONGATION, not phase angle: 180 is FULL. Predicted lit fractions 1.000 / 0.854 /
# 0.500 / 0.146 and Krisciunas-Schaefer factors 1.0000 / 0.3352 / 0.0910 / 0.0116. The
# ladder straddles 90 deliberately -- that is the ONE point where the elongation and the
# phase angle agree, so a reader that confuses them is invisible there and nowhere else.
MOON_ELONGS = [180.0, 135.0, 90.0, 45.0]
# The disc is 3.8 px in RADIUS at its real 0.53 degrees on a 400x300 frame, which is why
# every arm that reads its INTERIOR blows it up. That is legitimate rather than
# convenient: the phase construction is a function of position on the unit-sphere face,
# so angular radius SCALES the picture and cannot rotate it. The shipping 0.53 is pinned
# exactly and separately, by moon-config reading the dumped row.
MOON_DISC_BIG = 12.0
MOON_DISC_HUGE = 40.0
MOON_DISC_SHIPPING = 0.53
# --no-bloom on every disc arm is load-bearing rather than hygiene: the bloom pyramid
# spreads the disc's energy across the whole frame, so an arm asserting the disc is
# confined to its own solid angle would be reading the blur kernel. --no-dither for the
# neighbouring reason -- the maria read is a small-signal sigma where +/-1 LSB is large.
# --no-moon-glow on every DISC arm, for the reason --no-bloom is there: the
# aureole is real light OUTSIDE the disc, so an arm asserting the disc is
# confined to its own solid angle would be reading the halo. The halo has its
# own arm. (It also has to come off because the halo's width follows the DRAWN
# radius, and these arms drive that to 12 degrees for measurement -- which is a
# halo most of a frame wide, wanted by nobody.)
MOON_DISC_FLAGS = STARS_PIN + [*NO_HALOS, "--no-dither", "--no-moon-glow"]

# create_sky_atmosphere's own cycle_moon_offset default. Named rather than typed
# out, because moon-cycle's 0 px identity rests on the C and this twin agreeing
# about it: a change there with no change here reads as a moon that moved.
MOON_CYCLE_LAG = 12.0

MOON_PROBE_TOL = 1e-4        # float32 C against doubles here, and acos is ill-conditioned at +-1
MOON_LIT_TOL = 0.12          # measured worst 0.043 across the four rungs
# 25 degrees, and the width is the MARIA rather than slack. The centroid is weighted by
# the contribution, the maria are a fixed pattern on a face the lit region slides across,
# so each roll's centroid is pulled a little off the pure sunward axis by whichever
# patches it happens to cover. Measured worst 0.9577 (16.7 degrees) at the shipping
# contrast. It still refuses every mutation this arm exists for by a wide margin: a
# mirrored disc reads 180 degrees, a transposed basis 90, and a constant screen direction
# is 90 or 180 off on three of the four rolls.
MOON_LIMB_COS_MIN = 0.906
MOON_LIMB_OFFSET_MIN = 0.10  # of the disc radius; measured worst 0.30
MOON_KS_RATIO_MIN = 5.0      # full/quarter, against a predicted 10.99 through the tonemap
# Correct code reads 0.013 and a sun-locked maria field 0.113; the bar sits
# between them. It is NOT zero any more and cannot be: since the surface gained
# relief, a pixel bright under one sun roll can sit in a crater's own shadow
# under another, so the albedo ratio the arm reads carries a couple of 8-bit
# codes of quantization that no threshold removes. Before relief this read an
# exact 0.000.
MOON_MARIA_DRIFT_MAX = 0.026
# Where the drift read starts, as a fraction of the lit face's own peak, and it
# has to be a high one now that the surface has RELIEF: a pixel bright under one
# sun roll can sit in a crater's own shadow under another, and the albedo ratio
# across that pair is mostly quantization. Requiring both rolls to be strongly
# lit is what keeps the read about the albedo pattern rather than the shading.
# Stated relatively because an absolute floor emptied the set outright the moment
# the disc radiance was retuned -- the same bar failing the other way.
MOON_MARIA_BASE_FRAC = 0.45
# The rest of the bars, hoisted out of the arm bodies with their MEASURED readings
# beside them -- the shape the block above already had and the arms had been ignoring.
# Each pairs the reading on correct code with what that arm's named mutation produces,
# so a reader can tell margin from a knife edge.
MOON_DISC_FILL_MIN = 0.55     # 0.72 measured, ceiling 0.71 lit at this elongation
MOON_DISC_HIT_FRAC = 0.02     # of the frame's own peak: "this pixel carries the disc"
MOON_LIT_HIT_FRAC = 0.10      # of peak; rides the peak because it falls 100x over the ladder
MOON_MARIA_MIN_PX = 200       # 10,614 measured; 0 with the maria term deleted
MOON_MARIA_SIGMA_MIN = 0.25   # 0.86 measured; a flat tint reads 0
MOON_MARIA_OVERLAP_MIN = 500  # 10,779 measured; keeps the drift read from being empty
MOON_EARTH_MIN_PX = 50        # 3,641 measured on the dark limb
MOON_EARTH_GAIN_MIN = 1e-5    # 3.93e-03 measured; 0 with earthshine deleted
MOON_EARTH_LIT_RATIO = 4.0    # lit-side move must stay under this x the dark gain (1.17e-04)
MOON_ENV_SKY_MIN = 200        # 73,707 measured; the anti-vacuity half of moon-env
MOON_SHADOW_MIN_PX = 500      # 786 measured; 0 with cast_shadows forced false
MOON_CLOUDS_MAX = 0.85        # 0.005 measured under full overcast; ~1.0 after the multiply
MOON_CYCLE_MOVE_MIN = 2000    # 480,000 measured between two hours


def _moon_phase(m_el, m_az, s_el, s_az):
    """(elongation, phase angle, lit fraction, Krisciunas-Schaefer factor), all doubles.

    The phase angle is the SUPPLEMENT of the elongation -- it is measured at the moon, so
    0 is full and 180 is new. Confusing the two inverts the feature and is invisible at
    exactly 90, which is why MOON_ELONGS straddles it.
    """
    m, s = _moon_dir(m_el, m_az), _moon_dir(s_el, s_az)
    d = max(-1.0, min(1.0, sum(a * b for a, b in zip(m, s))))
    elong = math.degrees(math.acos(d))
    alpha = 180.0 - elong
    lit = 0.5 * (1.0 + math.cos(math.radians(alpha)))
    ks = 10.0 ** (-0.4 * (0.026 * alpha + 4e-9 * alpha ** 4))
    return elong, alpha, lit, ks


def _moon_roll_basis(m_el, m_az):
    """(m, e1, e2): the moon vector and two axes perpendicular to it, e1 being world up
    projected into that plane. e2 = m x e1 therefore has y EXACTLY zero, which is what
    makes the sun's elevation below a closed form of the roll alone.
    """
    m = _moon_dir(m_el, m_az)
    up = (0.0, 1.0, 0.0)
    d = sum(a * b for a, b in zip(up, m))
    e1 = [u - d * c for u, c in zip(up, m)]
    n = math.sqrt(sum(c * c for c in e1)) or 1.0
    e1 = [c / n for c in e1]
    e2 = [m[1] * e1[2] - m[2] * e1[1], m[2] * e1[0] - m[0] * e1[2],
          m[0] * e1[1] - m[1] * e1[0]]
    return m, e1, e2


def _moon_sun_at_elongation(m_el, m_az, elongation, roll=180.0):
    """The sun (elevation, azimuth) at `elongation` from the moon, on a chosen ROLL about
    the moon vector.

    Rolling rather than solving at a fixed sun elevation, because the two are not
    independent: a FULL moon (elongation 180) puts the sun at the moon's exact antipode
    and there is no azimuth left to choose. Roll 180 is the default because it is the
    roll that tilts AWAY from the zenith -- sun elevation there is exactly
    asin(sin(E - e)), the lowest the pair admits -- which is what keeps the sun's own
    disc out of frame under the group's standing constraint.
    """
    m, e1, e2 = _moon_roll_basis(m_el, m_az)
    ce, se = math.cos(math.radians(elongation)), math.sin(math.radians(elongation))
    cr, sr = math.cos(math.radians(roll)), math.sin(math.radians(roll))
    s = [ce * m[k] + se * (cr * e1[k] + sr * e2[k]) for k in range(3)]
    el = math.degrees(math.asin(max(-1.0, min(1.0, s[1]))))
    az = math.degrees(math.atan2(s[0], s[2])) % 360.0
    return el, az


def _moon_camera(el_deg, az_deg, eye=(0.0, 400.0, 500.0), reach=20000.0):
    """A camera dict + argv aimed straight down a sky direction, so the body lands at the
    exact frame centre. The dict feeds _projector; the argv feeds render().
    """
    d = _moon_dir(el_deg, az_deg)
    target = tuple(e + reach * c for e, c in zip(eye, d))
    cam = {"eye": eye, "target": target, "fovy_deg": 40.0}
    argv = ["--cam-eye", ",".join(f"{c:.6f}" for c in eye),
            "--cam-target", ",".join(f"{c:.6f}" for c in target)]
    return cam, argv


def _moon_render(workdir, tag, extra, frames=30, moon=True):
    """One leg of a moon twin. `moon` False renders the --no-moon reference.

    The switch goes LAST, and that is not style. --moon-elevation and --moon-azimuth both
    IMPLY --moon (the four-site idiom's "a value arms the feature"), so a --no-moon
    leading a configuration that also names an angle is re-armed by its own placement and
    the twin renders two identical frames. Every arm here passes angles, so every off leg
    would have been silently on.
    """
    scene = asset(STARS_FIXTURE)
    out = os.path.join(workdir, f"moon_{tag}.ppm")
    tail = ["--moon"] if moon else ["--no-moon"]
    err = render(scene, out, extra + tail, frames=frames)
    return out, err


def _moon_disc_cells(on_path, off_path, geom):
    """(cells, peak, outside) for a twin pair inside a predicted disc.

    cells is [(x, y, delta)] of linear on-minus-off inside the mask; outside counts
    pixels that moved beyond it. The delta is computed only INSIDE the radius test --
    it is six table lookups, and the surrounding band needs nothing but the byte
    inequality, which took this from 61.8 ms per call to 16.7.
    """
    w, h, on = _read_ppm(on_path)
    _, _, off = _read_ppm(off_path)
    radius, cx, cy = geom
    cells, peak, outside = [], 0.0, 0
    pad = int(radius * 3.0) + 8
    for y in range(max(0, int(cy) - pad), min(h, int(cy) + pad + 1)):
        row = y * w
        for x in range(max(0, int(cx) - pad), min(w, int(cx) + pad + 1)):
            o = (row + x) * 3
            r = math.hypot(x - cx, y - cy)
            if r <= radius:
                delta = sum(_SRGB_TO_LINEAR[on[o + k]] - _SRGB_TO_LINEAR[off[o + k]]
                            for k in range(3)) / 3.0
                cells.append((x, y, delta))
                peak = max(peak, delta)
            elif r > radius + 4.0 and any(on[o + k] != off[o + k] for k in range(3)):
                outside += 1
    return cells, peak, outside


def _moon_pair(workdir, tag, extra, frames=30):
    """The moon-on / moon-off twin at one configuration -> (on, off, err).

    The shape _stars_pair and _nightfloor_pair already have; this group was written
    without it and spelled the pair out five times.
    """
    on, e1 = _moon_render(workdir, tag + "_on", extra, frames=frames)
    off, e2 = _moon_render(workdir, tag + "_off", extra, frames=frames, moon=False)
    return on, off, e1 or e2


def _moon_probe(scene, m_el, m_az, s_el, s_az):
    """The `phase` row of --moon-probe at one configuration, as floats.

    Through _probe_render like every other probe in the tree. The first draft hand-rolled
    a positional parser and was wrong twice over: the `dir` triple desynced the pairing so
    a FLOAT became a dict key, and dropping the tag let the sun row's el/az overwrite the
    body row's. It passed only because the arm reads four keys that neither bug reached.
    """
    rows, _ = _probe_render(scene, "--moon-probe", "moon-probe", frames=2,
                            extra=["--sky", "--moon",
                                   "--sun-elevation", f"{s_el:.6f}",
                                   "--sun-azimuth", f"{s_az:.6f}",
                                   "--moon-elevation", f"{m_el}",
                                   "--moon-azimuth", f"{m_az}"] + STARS_PIN)
    got = {}
    for rec in rows:
        for k, v in rec.items():
            if k == "kind":
                continue
            try:
                got[k] = float(v)
            except ValueError:
                pass
    return got


def run_moon_gate(workdir):
    """The moon (spec 11.82): an analytic disc, a derived phase, a second casting light.

      moon-probe      the phase law as a CLOSED FORM, held against this file's own twin
                      at four elongations: elongation, phase angle, lit fraction and the
                      Krisciunas-Schaefer factor to 1e-4. The only exact hold in the
                      group -- no pixel, no tonemap and no 8-bit quantization between the
                      arithmetic and the assertion. Deleting the alpha^4 term moves the
                      45-elongation rung 2.1x; reading the elongation where the phase
                      angle belongs moves every rung BUT 90, which is where the two agree
                      and why the ladder straddles it.
      moon-disc       the disc lands where the sky says and NOWHERE else. The camera is
                      aimed down this file's own copy of the elevation/azimuth formula
                      and the disc is placed by the C's, so the arm asserts that two
                      independent computations of one direction agree; the predicted mask
                      fills and nothing moves outside it. It fails FIRST and by name if
                      the moon is placed on a different azimuth convention, and every
                      disc arm below would still pass on an empty frame -- two absent
                      discs agree perfectly.
      moon-lit        the LIT AREA on the face follows (1 - cos elongation)/2 across four
                      elongations. The disc's own copy of the phase geometry, which no
                      probe reaches. Mirror-INVARIANT by construction, which is exactly
                      why it is not safe alone.
      moon-terminator the bright limb points at the sun, at FOUR sun rolls 90 degrees
                      apart about the moon vector. The centroid of the contribution,
                      measured from the disc centre, lies within 15 degrees of the
                      projected sunward direction on all four, with a real displacement.
                      The group's highest-value arm and the decal poster's lesson applied
                      to a sphere: a build that always lights the same screen side passes
                      one roll, a transposed disc basis passes two, a mirrored disc
                      passes none. Area is invariant under all of those; direction at
                      four rolls is not.
      moon-maria      the face is textured, the texture is not a flat tint, and it is
                      fixed to the FACE rather than to the sun. Against a --no-moon-maria
                      twin at the terminator's own rolls: a real sigma in the maria
                      signal, and that signal steady across rolls where the lit region
                      overlaps. The second leg is what fails a maria field evaluated in
                      the terminator's sun-relative frame -- which the terminator code
                      builds anyway, so it is the natural wrong turn rather than a
                      hypothetical one.
      moon-earthshine the dark limb is Earth-lit, not black, and bounded well under the
                      lit side. Without it the term is unasserted and every arm above
                      reads it as part of the disc.
      moon-light      the moon LIGHTS the world, and its intensity carries the same law
                      the probe holds: the terrain band lifts against a --no-moon twin,
                      and the full-moon lift is a large multiple of the quarter-moon one.
                      A wide floor deliberately -- the neutral operator subtracts a
                      per-pixel min-channel offset before it compresses anything, so this
                      is a tonemapped delta and not a radiance ratio. The exact hold
                      lives in moon-probe; this arm says only that the SAME law reached
                      the C light. A moon implemented as an ambient lift reads 1.00.
      moon-env        the firefly rule, asserted from OUTSIDE the process. At --sky-disc
                      40 against the shipping 0.53, one number apart, the TERRAIN band is
                      0 px while the sky band moves: a 40-degree disc is 3% of the sphere
                      and would dominate night irradiance by orders of magnitude if it
                      had ever entered the bake. --night-floor is on so the env->IBL path
                      is live and 0 px is not 0-vs-0. What it catches is the moon term
                      moved into sky_view_frag, the LUT -- which is exactly where 11.80
                      put the night floor, and which reaches the background, the env
                      cube, the IBL, the fog ambient and the cloud ambient at once.
      moon-shadow     the second directional actually CASTS. At night the sun does not
                      cast at all, so any shadow in the frame is the moon's and
                      --no-shadows isolates it exactly; the no-moon leg is the control
                      that says so. Nothing else here asserts a map was ever built --
                      moon-light reads the same lift from a moon that casts nothing.
      moon-clouds     under a FULL overcast the disc's contribution shrinks: the term
                      sits inside the sky accumulator, under the cloud.a multiply.
                      stars-clouds cannot cover this -- it asserts that position for the
                      STARS term, and a moon added after the multiply passes it and
                      fails nothing, which is 11.81's vacuous-arm lesson read from the
                      other side. Coverage is pinned at 1.0 because the moon is a point
                      and cannot use the stars' whole-band dodge.
      moon-cycle      a FROZEN cycle at an hour renders 0 px from explicit
                      --moon-elevation/--moon-azimuth taken from this file's twin of the
                      lag arithmetic, and the SAME angles at a different hour move real
                      pixels. The second leg is not thoroughness: without it a tick that
                      never writes the moon passes the identity whenever the twin happens
                      to predict the default.
      moon-cscn       the authored environment.moon block IS the flag path (0 px from its
                      flag twin), and --no-moon beats an authoring file at 0 px. The
                      stars-cscn / nightfloor-cscn / cycle-cscn idiom; without it the
                      whole scene-key path is unexercised.
      moon-config     a dumped session restores to the same frame, and the dump carries
                      the moon rows at their shipping defaults -- moon false and the disc
                      at 0.53. This is where "off by default" and the disc's real angular
                      size are pinned EXACTLY rather than measured through the 3.8 pixels
                      a 0.53-degree disc actually covers.
      moon-det        two runs of the heaviest configuration are 0 px. The maria are the
                      group's only new hash and a wall-clock seed is the first thing that
                      would break.

    TWO ANTI-VACUITY PAIRS, and no half of either is safe alone. moon-lit passes on a
    MIRRORED disc, because area does not change when you flip it -- the decal poster's
    failure exactly -- and moon-terminator is what refuses it; moon-terminator passes on
    a disc whose lit fraction never changes, and moon-lit is what refuses that. moon-env
    passes on a build whose moon lights nothing at all, since 0 px is 0 px, and
    moon-light is what forces the ground to be listening; moon-light passes on a build
    that bakes the disc into the cube, and moon-env is what refuses it.

    EVERY DISC ARM PINS THE SUN BELOW MINUS THE DISC HALF-ANGLE, and that is a
    correctness requirement rather than framing. The sun-disc test in sky_radiance.glsl
    is `dot(dir, sunDir) > cosRadius && dir.y > 0` -- dir is the VIEW RAY, not the sun --
    so a sun below the horizon still paints a disc wherever its half-angle reaches over.
    At --sky-disc 40 a sun at -15 shows a five-degree crescent of ITSELF, and moon-env's
    sky leg would then pass with the moon disc deleted entirely.

    Four things are deliberately NOT covered, and saying so beats leaving them looking
    tested. The maria's ORIENTATION on the face: the pattern is procedural, so there is
    no ground truth to mirror it against -- a field laid on backwards would ship, and
    only moon-det's byte identity holds it still. The moon's absolute photometric level,
    which like the night floor's radiance is a look-calibrated constant chosen against
    real frames, and no arm can assert a number picked by eye. A scene already at the
    three-caster directional cap plus a moon: that is spec 11.23's subject and no fixture
    here authors three. And the GUI's moon sliders and their disabled-under-a-cycle
    guards, for the reason run_cycle_gate gives about its own -- no headless arm reaches
    them.
    """
    failures = []
    scene = asset(STARS_FIXTURE)

    # --- moon-probe: the closed form, exact ---------------------------------
    probe_ok, probe_worst, probe_note = True, 0.0, ""
    for elong in MOON_ELONGS:
        sun_el, s_az = _moon_sun_at_elongation(MOON_EL_LOW, MOON_AZ, elong)
        got = _moon_probe(scene, MOON_EL_LOW, MOON_AZ, sun_el, s_az)
        want = dict(zip(("elongation", "alpha", "lit", "ks"),
                        _moon_phase(MOON_EL_LOW, MOON_AZ, sun_el, s_az)))
        for key, w in want.items():
            if key not in got:
                probe_ok, probe_note = False, f"the probe printed no {key}"
                break
            probe_worst = max(probe_worst, abs(got[key] - w) / max(1.0, abs(w)))
        if not probe_ok:
            break
    probe_ok = probe_ok and probe_worst <= MOON_PROBE_TOL
    print(f"  moon-probe {'PASS' if probe_ok else 'FAIL'}  four elongations against the "
          f"twin, worst relative error {probe_worst:.2e} (want <={MOON_PROBE_TOL:.0e})"
          f"{(' -- ' + probe_note) if probe_note else ''}")
    if not probe_ok:
        failures.append("moon-probe")

    # --- moon-disc: it lands where the sky says, and nowhere else -----------
    disc_sun_el, s_az_full = _moon_sun_at_elongation(MOON_EL_HIGH, MOON_AZ, MOON_ELONG_ROLL)
    cam, cam_argv = _moon_camera(MOON_EL_HIGH, MOON_AZ)
    disc_common = ["--sun-elevation", f"{disc_sun_el:.6f}", "--sun-azimuth", f"{s_az_full:.6f}",
                   "--moon-elevation", f"{MOON_EL_HIGH}", "--moon-azimuth", f"{MOON_AZ}",
                   "--sky-disc", str(MOON_DISC_BIG)] + cam_argv + MOON_DISC_FLAGS
    d_on, d_off, ed = _moon_pair(workdir, "disc", disc_common)
    if ed:
        print(f"  moon-disc ERROR render failed: {ed.strip()[-200:]}")
        failures.append("moon-disc")
        return failures
    fw, fh, _ = _read_ppm(d_on)
    geom = _moon_disc_geometry(cam, MOON_DISC_BIG, MOON_EL_HIGH, MOON_AZ, fw, fh)
    radius, cx, cy = geom
    cells, peak, outside = _moon_disc_cells(d_on, d_off, geom)
    hit = sum(1 for _, _, v in cells if v > MOON_DISC_HIT_FRAC * peak)
    fill = hit / max(1, len(cells))
    ok = fill >= MOON_DISC_FILL_MIN and outside == 0 and peak > 0.0
    print(f"  moon-disc {'PASS' if ok else 'FAIL'}  {fill:.2f} of the predicted mask "
          f"carries the disc (want >={MOON_DISC_FILL_MIN}; a lit fraction of {_moon_phase(MOON_EL_HIGH, MOON_AZ, disc_sun_el, s_az_full)[2]:.2f} "
          f"is the ceiling), {outside} px moved outside it (want 0), peak {peak:.4f}")
    if not ok:
        failures.append("moon-disc")

    # --- moon-lit: the lit AREA follows the phase geometry ------------------
    lit_cam, lit_argv = _moon_camera(MOON_EL_LOW, MOON_AZ)
    lit_rows, lit_worst, lit_err = [], 0.0, None
    for elong in MOON_ELONGS:
        lit_sun_el, s_az = _moon_sun_at_elongation(MOON_EL_LOW, MOON_AZ, elong)
        common = ["--sun-elevation", f"{lit_sun_el:.6f}", "--sun-azimuth", f"{s_az:.6f}",
                  "--moon-elevation", f"{MOON_EL_LOW}", "--moon-azimuth", f"{MOON_AZ}",
                  "--sky-disc", str(MOON_DISC_BIG)] + lit_argv + MOON_DISC_FLAGS
        on, off, el_err = _moon_pair(workdir, f"lit{int(elong)}", common)
        if el_err:
            lit_err = el_err.strip()[-160:]
            break
        cells, peak, _ = _moon_disc_cells(
            on, off,
            _moon_disc_geometry(lit_cam, MOON_DISC_BIG, MOON_EL_LOW, MOON_AZ, fw, fh))
        # A tenth of this frame's own peak: the threshold has to ride the peak because
        # the peak itself falls by two orders across the ladder, which is the phase law.
        got = sum(1 for _, _, v in cells if v > MOON_LIT_HIT_FRAC * peak) / max(1, len(cells))
        want = _moon_phase(MOON_EL_LOW, MOON_AZ, lit_sun_el, s_az)[2]
        lit_rows.append((elong, got, want))
        lit_worst = max(lit_worst, abs(got - want))
    ok = lit_err is None and lit_rows and lit_worst <= MOON_LIT_TOL
    ladder = " ".join(f"{g:.2f}/{w:.2f}" for _, g, w in lit_rows)
    print(f"  moon-lit {'PASS' if ok else 'FAIL'}  measured/predicted lit fraction "
          f"{ladder} (want within {MOON_LIT_TOL}), worst {lit_worst:.3f}"
          f"{(' -- ' + lit_err) if lit_err else ''}")
    if not ok:
        failures.append("moon-lit")

    # --- moon-terminator: the bright limb points at the sun, at four rolls ---
    # The four sun directions are built by rolling about the moon vector, so each has a
    # DIFFERENT predicted screen angle -- which is what no fixed mis-orientation can
    # satisfy. One roll cannot tell a mirror from a transpose from a constant.
    m = _moon_dir(MOON_EL_HIGH, MOON_AZ)
    term_rows, term_err = [], None
    for roll in MOON_ROLLS:
        s_el, s_az = _moon_sun_at_elongation(MOON_EL_HIGH, MOON_AZ, MOON_ELONG_ROLL, roll)
        s = _moon_dir(s_el, s_az)
        if s_el > -MOON_DISC_BIG * 0.5:
            term_err = f"roll {roll:.0f} puts the sun at {s_el:.1f}, not clear of its own disc"
            break
        # --no-moon-maria, and it ISOLATES this arm's claim rather than dodging
        # one. What is under test is which way the terminator FACES; the maria
        # are a fixed albedo pattern the lit region slides across, so they pull a
        # contribution-weighted centroid off the sunward axis by however much sea
        # happens to be lit. Deepening the seas took the worst roll from 16.7 to
        # 32 degrees without the terminator moving at all. They are asserted
        # separately, by moon-maria.
        common = ["--sun-elevation", f"{s_el:.6f}", "--sun-azimuth", f"{s_az:.6f}",
                  "--moon-elevation", f"{MOON_EL_HIGH}", "--moon-azimuth", f"{MOON_AZ}",
                  "--sky-disc", str(MOON_DISC_BIG), "--no-moon-maria"] + cam_argv + \
            MOON_DISC_FLAGS
        on, off, et = _moon_pair(workdir, f"term{int(roll)}", common)
        if et:
            term_err = et.strip()[-160:]
            break
        rad, ccx, ccy = geom
        cells, _, _ = _moon_disc_cells(on, off, geom)
        wsum = sum(v for _, _, v in cells if v > 0.0)
        if wsum <= 0.0:
            term_err = f"roll {roll:.0f} carried no contribution at all"
            break
        gx = sum(x * v for x, _, v in cells if v > 0.0) / wsum
        gy = sum(y * v for _, y, v in cells if v > 0.0) / wsum
        off_px = math.hypot(gx - ccx, gy - ccy)
        # The predicted sunward direction ON SCREEN, from the degrees the flags actually
        # carried -- projected through the same camera, so the arm never trusts its own
        # idea of which way is which.
        project = _projector(cam, *_read_ppm(on)[:2])
        b = [s[k] - sum(a * bb for a, bb in zip(s, m)) * m[k] for k in range(3)]
        nb = math.sqrt(sum(c * c for c in b))
        b = [c / nb for c in b]
        eye = cam["eye"]
        p0 = project(tuple(e + 20000.0 * c for e, c in zip(eye, m)))
        tilt = [m[k] + 0.05 * b[k] for k in range(3)]
        nt = math.sqrt(sum(c * c for c in tilt))
        p1 = project(tuple(e + 20000.0 * (c / nt) for e, c in zip(eye, tilt)))
        ux, uy = p1[0] - p0[0], p1[1] - p0[1]
        un = math.hypot(ux, uy)
        cosang = ((gx - ccx) * ux + (gy - ccy) * uy) / max(1e-9, off_px * un)
        term_rows.append((roll, cosang, off_px / rad))
    good = sum(1 for _, c, o in term_rows
               if c >= MOON_LIMB_COS_MIN and o >= MOON_LIMB_OFFSET_MIN)
    ok = term_err is None and good == len(MOON_ROLLS)
    worst_c = min((c for _, c, _ in term_rows), default=float("nan"))
    worst_o = min((o for _, _, o in term_rows), default=float("nan"))
    print(f"  moon-terminator {'PASS' if ok else 'FAIL'}  {good}/{len(MOON_ROLLS)} rolls "
          f"point at the sun (want all), worst cos {worst_c:.4f} "
          f"(want >={MOON_LIMB_COS_MIN}), worst offset {worst_o:.3f} of the radius "
          f"(want >={MOON_LIMB_OFFSET_MIN}){(' -- ' + term_err) if term_err else ''}")
    if not ok:
        failures.append("moon-terminator")

    # --- moon-maria: textured, and locked to the FACE -----------------------
    maria_err = None
    m_flat, ef = _moon_render(workdir, "maria_flat", disc_common + ["--no-moon-maria"])
    if ef:
        print(f"  moon-maria ERROR render failed: {ef.strip()[-200:]}")
        failures.append("moon-maria")
    else:
        w, h, a = _read_ppm(d_on)
        _, _, b = _read_ppm(m_flat)
        vals = []
        for _, _, o in _moon_face_pixels(cx, cy, radius, w, h, 0.9):
            lit_here = sum(_SRGB_TO_LINEAR[b[o + k]] for k in range(3)) / 3.0
            if lit_here < 0.02:  # the dark limb carries no maria to measure
                continue
            vals.append(sum(_SRGB_TO_LINEAR[a[o + k]] - _SRGB_TO_LINEAR[b[o + k]]
                            for k in range(3)) / 3.0)
        moved = sum(1 for v in vals if abs(v) > 1e-4)
        mean = sum(vals) / max(1, len(vals))
        var = sum((v - mean) ** 2 for v in vals) / max(1, len(vals))
        sigma = math.sqrt(var)
        # THE THIRD LEG, and it is the one that earns the arm its "locked to the face"
        # claim. The first two are satisfied by a maria field evaluated in ANY frame --
        # including the sun-relative one the terminator code already builds, which is
        # the natural wrong turn and which makes the pattern spin with the phase.
        # The bar is placed BETWEEN the two readings, by killing the twin first:
        # correct reads 0.000 -- exactly, because the face frame is a function of the
        # moon vector alone and both rolls share it -- against 0.044 mutated. 0.01 sits
        # four times under the mutation and is unreachable by anything face-locked.
        #
        # The comparison is the ALBEDO, on divided by off, because that is what the
        # maria multiply and it is therefore independent of how brightly the two rolls
        # happen to be lit. Compared only where both rolls light the same face patch.
        alb = {}
        for roll in (0.0, 90.0):
            r_el, r_az = _moon_sun_at_elongation(MOON_EL_HIGH, MOON_AZ, MOON_ELONG_ROLL, roll)
            r_common = ["--sun-elevation", f"{r_el:.6f}", "--sun-azimuth", f"{r_az:.6f}",
                        "--moon-elevation", f"{MOON_EL_HIGH}", "--moon-azimuth", f"{MOON_AZ}",
                        "--sky-disc", str(MOON_DISC_BIG)] + cam_argv + MOON_DISC_FLAGS
            r_on, er1 = _moon_render(workdir, f"maria_r{int(roll)}_on", r_common)
            r_off, er2 = _moon_render(workdir, f"maria_r{int(roll)}_off",
                                      r_common + ["--no-moon-maria"])
            if er1 or er2:
                maria_err = (er1 or er2).strip()[-160:]
                break
            _, _, ra = _read_ppm(r_on)
            _, _, rb = _read_ppm(r_off)
            roll_peak = 0.0
            for _, _, o in _moon_face_pixels(cx, cy, radius, w, h, 0.9):
                roll_peak = max(roll_peak,
                                sum(_SRGB_TO_LINEAR[rb[o + k]] for k in range(3)) / 3.0)
            cur = {}
            for x, y, o in _moon_face_pixels(cx, cy, radius, w, h, 0.9):
                base = sum(_SRGB_TO_LINEAR[rb[o + k]] for k in range(3)) / 3.0
                # WELL clear of the terminator, not merely non-black. The ratio is what
                # cancels everything but the albedo, and a ratio of two small 8-bit codes
                # is mostly quantization -- at 0.05 the worst pixel read 0.127 on CORRECT
                # code, which is noise wearing the shape of a finding.
                if base < MOON_MARIA_BASE_FRAC * roll_peak:
                    continue
                cur[(x, y)] = (sum(_SRGB_TO_LINEAR[ra[o + k]] for k in range(3)) / 3.0) / base
            alb[roll] = cur
        both = set(alb[0.0]) & set(alb[90.0])
        # The MEAN, not the max: one worst pixel over ten thousand is a quantization
        # story, and what this leg claims is about the whole pattern.
        drift = (sum(abs(alb[0.0][k] - alb[90.0][k]) for k in both) / len(both)) \
            if both else float("nan")
        ok = maria_err is None and moved >= MOON_MARIA_MIN_PX and sigma > 0.0 and \
            (sigma / max(abs(mean), 1e-6)) >= MOON_MARIA_SIGMA_MIN and \
            len(both) >= MOON_MARIA_OVERLAP_MIN and drift <= MOON_MARIA_DRIFT_MAX
        print(f"  moon-maria {'PASS' if ok else 'FAIL'}  {moved} px of the lit face carry "
              f"maria (want >={MOON_MARIA_MIN_PX}), sigma/|mean| {sigma / max(abs(mean), 1e-6):.2f} "
              f"(want >={MOON_MARIA_SIGMA_MIN}: a flat tint reads 0), and the pattern drifts {drift:.3f} "
              f"across two sun rolls over {len(both)} shared px "
              f"(want <={MOON_MARIA_DRIFT_MAX}: locked to "
              f"the FACE, not the sun)"
              f"{(' -- ' + maria_err) if maria_err else ''}")
        if not ok:
            failures.append("moon-maria")

    # --- moon-earthshine: the dark limb is lit, and bounded -----------------
    e_off, ee = _moon_render(workdir, "earth_off", disc_common + ["--no-earthshine"])
    if ee:
        print(f"  moon-earthshine ERROR render failed: {ee.strip()[-200:]}")
        failures.append("moon-earthshine")
    else:
        w, h, a = _read_ppm(d_on)
        _, _, b = _read_ppm(e_off)
        _, _, ref = _read_ppm(d_off)
        dark, lit_side = [], []
        for _, _, o in _moon_face_pixels(cx, cy, radius, w, h, 0.85):
            on_v = sum(_SRGB_TO_LINEAR[a[o + k]] for k in range(3)) / 3.0
            no_e = sum(_SRGB_TO_LINEAR[b[o + k]] for k in range(3)) / 3.0
            bg = sum(_SRGB_TO_LINEAR[ref[o + k]] for k in range(3)) / 3.0
            (lit_side if no_e - bg > 1e-3 else dark).append(on_v - no_e)
        gain = sum(dark) / max(1, len(dark))
        lit_mean = sum(lit_side) / max(1, len(lit_side))
        ok = len(dark) > MOON_EARTH_MIN_PX and gain > MOON_EARTH_GAIN_MIN and \
            abs(lit_mean) < gain * MOON_EARTH_LIT_RATIO
        print(f"  moon-earthshine {'PASS' if ok else 'FAIL'}  the dark limb gains "
              f"{gain:.2e} over {len(dark)} px (want >0), while the lit side moves "
              f"{lit_mean:.2e} (want small beside it: earthshine is the DARK side's term)")
        if not ok:
            failures.append("moon-earthshine")

    # --- moon-light: the moon LIGHTS the world, carrying the same law -------
    # The fixture's own camera, so the terrain band is the one the floor group reads.
    light_rows, light_err = [], None
    for elong in (180.0, 90.0):
        lg_sun_el, s_az = _moon_sun_at_elongation(MOON_EL_LOW, MOON_AZ, elong)
        common = ["--sun-elevation", f"{lg_sun_el:.6f}", "--sun-azimuth", f"{s_az:.6f}",
                  "--moon-elevation", f"{MOON_EL_LOW}", "--moon-azimuth", f"{MOON_AZ}",
                  "--night-floor", "--moon-brightness", "6"] + STARS_PIN
        on, off, elg = _moon_pair(workdir, f"lgt{int(elong)}", common)
        if elg:
            light_err = elg.strip()[-160:]
            break
        rgb, moved = _stars_delta(on, off, NIGHTFLOOR_GROUND_BAND)
        light_rows.append((elong, sum(rgb) / 3.0, moved))
    # Bound before the print, not inside it: the ok test short-circuits safely on a
    # failed leg and the format string does not, so this used to be a traceback --
    # and main() has no per-group try, so it took the whole suite with it.
    full = light_rows[0][1] if len(light_rows) > 0 else float("nan")
    quarter = light_rows[1][1] if len(light_rows) > 1 else float("nan")
    ratio = (full / quarter) if len(light_rows) == 2 and quarter > 0 else 0.0
    ok = light_err is None and len(light_rows) == 2 and full > 0 and \
        ratio >= MOON_KS_RATIO_MIN
    print(f"  moon-light {'PASS' if ok else 'FAIL'}  terrain lift full {full:.5f} "
          f"vs quarter {quarter:.5f} = "
          f"{ratio:.2f}x (want >={MOON_KS_RATIO_MIN}, predicted 10.99; an ambient lift "
          f"reads 1.00){(' -- ' + light_err) if light_err else ''}")
    if not ok:
        failures.append("moon-light")

    # --- moon-env: the firefly rule, from outside the process ---------------
    # One number apart. A 40-degree disc is 3% of the sphere; if it had ever entered the
    # bake the ground could not possibly be unmoved.
    # MOON_EL_ENV, not MOON_EL_LOW: a full moon puts the sun at its exact antipode, so
    # the moon has to stand higher than the diagnostic disc's half-angle or the SUN's own
    # disc clears the horizon and the sky leg stops being about the moon.
    env_sun_el, s_az_env = _moon_sun_at_elongation(MOON_EL_ENV, MOON_AZ, 180.0)
    # --no-bloom here for the same reason the disc arms carry it, and here it is the
    # difference between a real reading and a false one: a 40-degree disc bloomed across
    # the frame lifts the TERRAIN band by thousands of pixels, which reads exactly like
    # the disc having entered the bake. Measured 15,978 px of pure bloom before this.
    env_common = ["--sun-elevation", f"{env_sun_el:.6f}", "--sun-azimuth", f"{s_az_env:.6f}",
                  "--moon-elevation", f"{MOON_EL_ENV}", "--moon-azimuth", f"{MOON_AZ}",
                  "--night-floor", *NO_HALOS] + STARS_PIN
    e_small, e3 = _moon_render(workdir, "env_small",
                               env_common + ["--sky-disc", str(MOON_DISC_SHIPPING)])
    e_huge, e4 = _moon_render(workdir, "env_huge",
                              env_common + ["--sky-disc", str(MOON_DISC_HUGE)])
    if e3 or e4:
        print(f"  moon-env ERROR render failed: {(e3 or e4).strip()[-200:]}")
        failures.append("moon-env")
    else:
        _, ground_moved = _stars_delta(e_small, e_huge, NIGHTFLOOR_GROUND_BAND)
        _, sky_moved = _stars_delta(e_small, e_huge, STARS_BAND)
        ok = ground_moved == 0 and sky_moved >= MOON_ENV_SKY_MIN
        print(f"  moon-env {'PASS' if ok else 'FAIL'}  a 40-degree disc against the "
              f"shipping 0.53 moves {ground_moved} px of TERRAIN (want 0: it never "
              f"entered the bake) while moving {sky_moved} px of sky (want >={MOON_ENV_SKY_MIN}, or "
              f"the arm is 0-vs-0)")
        if not ok:
            failures.append("moon-env")

    # --- moon-shadow: the second directional actually CASTS -----------------
    # At night the sun does not cast at all (its fade is zero below the horizon), so
    # ANY shadow in this frame is the moon's and --no-shadows isolates it exactly.
    # Nothing else in the group asserts a shadow map was ever built for it: moon-light
    # would read the same lift from a moon that casts nothing.
    sh_el, sh_az = _moon_sun_at_elongation(MOON_EL_LOW, MOON_AZ, 180.0)
    sh_common = ["--sun-elevation", f"{sh_el:.6f}", "--sun-azimuth", f"{sh_az:.6f}",
                 "--moon-elevation", f"{MOON_EL_LOW}", "--moon-azimuth", f"{MOON_AZ}",
                 "--night-floor", "--moon-brightness", "6"] + STARS_PIN
    s_on, e10 = _moon_render(workdir, "shadow_on", sh_common)
    s_off, e11 = _moon_render(workdir, "shadow_off", sh_common + ["--no-shadows"])
    s_none, e12 = _moon_render(workdir, "shadow_nomoon", sh_common, moon=False)
    s_none_ns, e13 = _moon_render(workdir, "shadow_nomoon_ns",
                                  sh_common + ["--no-shadows"], moon=False)
    if e10 or e11 or e12 or e13:
        print(f"  moon-shadow ERROR render failed: {(e10 or e11 or e12 or e13).strip()[-200:]}")
        failures.append("moon-shadow")
    else:
        cast_px, _ = compare(s_on, s_off)
        control_px, _ = compare(s_none, s_none_ns)
        ok = cast_px >= MOON_SHADOW_MIN_PX and control_px == 0
        print(f"  moon-shadow {'PASS' if ok else 'FAIL'}  --no-shadows moves {cast_px} px "
              f"under the moon (want >={MOON_SHADOW_MIN_PX}: it built a map and used it), against {control_px} "
              f"px with no moon at all (want 0, or the sun was casting and the first leg "
              f"is not the moon's)")
        if not ok:
            failures.append("moon-shadow")

    # --- moon-clouds: the disc sits UNDER the deck --------------------------
    # stars-clouds cannot cover this. It asserts the accumulator sits under the cloud
    # multiply for the STARS term; a moon term added after that multiply passes it and
    # fails nothing. Coverage is pinned at 1.0 because the moon is a point and cannot
    # use the stars' whole-band dodge -- it would simply sit in a gap.
    cl_common = disc_common  # the same framing; the deck is the only variable
    deck = ["--clouds", "--cloud-coverage", "1.0"]
    c_clear_on, c_clear_off, ec1 = _moon_pair(workdir, "cl_clear", cl_common, frames=60)
    c_deck_on, c_deck_off, ec2 = _moon_pair(workdir, "cl_deck", cl_common + deck, frames=60)
    if ec1 or ec2:
        print(f"  moon-clouds ERROR render failed: {(ec1 or ec2).strip()[-200:]}")
        failures.append("moon-clouds")
    else:
        _, clear_peak, _ = _moon_disc_cells(c_clear_on, c_clear_off, geom)
        _, deck_peak, _ = _moon_disc_cells(c_deck_on, c_deck_off, geom)
        frac = deck_peak / clear_peak if clear_peak > 0 else float("nan")
        ok = clear_peak > 0.0 and frac <= MOON_CLOUDS_MAX
        print(f"  moon-clouds {'PASS' if ok else 'FAIL'}  the disc keeps {frac:.3f} of its "
              f"peak under a full overcast (want <={MOON_CLOUDS_MAX}: it rides inside the sky "
              f"accumulator, under the cloud.a multiply)")
        if not ok:
            failures.append("moon-clouds")

    # --- moon-cycle: a frozen clock owns the angles -------------------------
    # The twin subtracts the lag WITHOUT wrapping, exactly as the C does: sky_sun_path
    # takes sin and cos of the hour, so -5 h and 19 h agree mathematically and not
    # bitwise, and this leg is a 0 px identity.
    hour, lat = CYCLE_QUIESCE_HOUR, CYCLE_LATITUDE
    m_el, m_az = _cycle_sun_path(lat, hour - MOON_CYCLE_LAG)
    s_el2, s_az2 = _cycle_sun_path(lat, hour)
    cyc_common = ["--night-floor", "--moon-brightness", "6"] + STARS_PIN
    c_clock, e5 = _moon_render(workdir, "cyc_clock",
                               ["--day-cycle", "0", "--time-of-day", f"{hour}"] + cyc_common)
    c_expl, e6 = _moon_render(workdir, "cyc_expl",
                              ["--sun-elevation", f"{s_el2:.6f}",
                               "--sun-azimuth", f"{s_az2:.6f}",
                               "--moon-elevation", f"{m_el:.6f}",
                               "--moon-azimuth", f"{m_az:.6f}"] + cyc_common)
    c_other, e7 = _moon_render(workdir, "cyc_other",
                               ["--day-cycle", "0", "--time-of-day", "19.5"] + cyc_common)
    if e5 or e6 or e7:
        print(f"  moon-cycle ERROR render failed: {(e5 or e6 or e7).strip()[-200:]}")
        failures.append("moon-cycle")
    else:
        same, _ = compare(c_clock, c_expl)
        moved, _ = compare(c_clock, c_other)
        ok = same == 0 and moved >= MOON_CYCLE_MOVE_MIN
        print(f"  moon-cycle {'PASS' if ok else 'FAIL'}  a frozen clock at hour {hour} "
              f"vs the twin's explicit angles {same} px (want 0; moon el={m_el:.6f} "
              f"az={m_az:.6f}), and a different hour moves {moved} px (want >={MOON_CYCLE_MOVE_MIN}, or "
              f"a tick that never writes the moon would pass the identity)")
        if not ok:
            failures.append("moon-cycle")

    # --- moon-cscn: the authored block IS the flag path ---------------------
    cscn_src = asset("aerial_fixture.cscn")
    authored = os.path.join(workdir, "moon_authored.cscn")
    cscn_sun_el, cscn_sun_az = _moon_sun_at_elongation(MOON_EL_LOW, MOON_AZ, 180.0)

    def _author(d):
        d["environment"]["sun"] = {"elevation": cscn_sun_el, "azimuth": cscn_sun_az}
        d["environment"]["moon"] = {"enabled": True, "brightness": 2.5,
                                    "elevation": MOON_EL_LOW, "azimuth": MOON_AZ}

    cscn_copy(cscn_src, authored, _author)
    a_file = os.path.join(workdir, "moon_cscn_file.ppm")
    a_flag = os.path.join(workdir, "moon_cscn_flag.ppm")
    a_offf = os.path.join(workdir, "moon_cscn_off.ppm")
    a_ref = os.path.join(workdir, "moon_cscn_ref.ppm")
    e8 = render(authored, a_file, ["--night-floor"] + STARS_PIN) or \
        render(cscn_src, a_flag,
               ["--sun-elevation", f"{cscn_sun_el:.6f}",
                "--sun-azimuth", f"{cscn_sun_az:.6f}",
                "--moon", "--moon-brightness", "2.5",
                "--moon-elevation", f"{MOON_EL_LOW}", "--moon-azimuth", f"{MOON_AZ}",
                "--night-floor"] + STARS_PIN) or \
        render(authored, a_offf, ["--no-moon", "--night-floor"] + STARS_PIN) or \
        render(cscn_src, a_ref,
               ["--sun-elevation", f"{cscn_sun_el:.6f}",
                "--sun-azimuth", f"{cscn_sun_az:.6f}", "--no-moon",
                "--night-floor"] + STARS_PIN)
    if e8:
        print(f"  moon-cscn ERROR render failed: {e8.strip()[-200:]}")
        failures.append("moon-cscn")
    else:
        flag_px, _ = compare(a_file, a_flag)
        off_px, _ = compare(a_offf, a_ref)
        ok = flag_px == 0 and off_px == 0
        print(f"  moon-cscn {'PASS' if ok else 'FAIL'}  authored vs flags {flag_px} px "
              f"(want 0), --no-moon over the file {off_px} px (want 0)")
        if not ok:
            failures.append("moon-cscn")

    # --- moon-config: the rows, and the shipping defaults, pinned exactly ---
    # The honest home for two claims no image can carry: that the moon is OFF by default,
    # and that its disc is really 0.53 degrees rather than whatever three pixels suggest.
    dump = os.path.join(workdir, "moon_config.json")
    cfg_src = asset(STARS_FIXTURE)
    # --no-shadows, and it is isolating the claim rather than dodging one. The SETTINGS
    # round-trip is exact -- the two dumps compare field for field identical, and the
    # frames are 0 px with this flag. With shadows on the pair differs by 917 px lying
    # entirely along the terrain silhouette: a restored session recomputes
    # camera.near_clip from the camera-to-target distance rather than restoring it (the
    # snapshot deliberately does not carry it), and the cascade fit reads that. The moon
    # is what made it VISIBLE here, by giving this fixture a caster it never had at
    # night -- the sun does not cast below the horizon. Recorded in the spec; the arm
    # this belongs to is a config one, not a moon one.
    tuned = ["--sun-elevation", f"{cscn_sun_el:.6f}",
             "--sun-azimuth", f"{cscn_sun_az:.6f}", "--moon",
             "--moon-brightness", "2.5", "--moon-elevation", f"{MOON_EL_LOW}",
             "--moon-azimuth", f"{MOON_AZ}", "--night-floor", "--no-shadows"] + STARS_PIN
    c_a = os.path.join(workdir, "moon_cfg_a.ppm")
    c_b = os.path.join(workdir, "moon_cfg_b.ppm")
    # A SECOND dump, from a run that asked for no moon at all. Without it the
    # "off by default" half of this arm was decoration: `tuned` passes --moon, so
    # sky.moon is true in that snapshot, and asserting the key is PRESENT says
    # nothing about the default. That is the vacuity this group's own docstring
    # cites 11.81 about, committed here.
    bare = os.path.join(workdir, "moon_config_bare.json")
    c_c = os.path.join(workdir, "moon_cfg_bare.ppm")
    e9 = render(cfg_src, c_a, tuned + ["--config-dump", dump]) or \
        render(cfg_src, c_c, ["--sky", "--config-dump", bare] + STARS_PIN)
    cfg, cfg_bare = {}, {}
    if not e9 and os.path.exists(dump) and os.path.exists(bare):
        with open(dump) as fh:
            cfg = json.load(fh)
        with open(bare) as fh:
            cfg_bare = json.load(fh)
        e9 = render(cfg_src, c_b, ["--config", dump])
    if e9 or not cfg:
        print(f"  moon-config ERROR {(e9 or 'no snapshot written').strip()[-200:]}")
        failures.append("moon-config")
    else:
        sky_rows = cfg.get("sky", {})
        px, _ = compare(c_a, c_b)
        has = all(k in sky_rows for k in
                  ("moon", "moon_brightness", "moon_elevation", "moon_azimuth",
                   "cycle_moon_offset", "moon_earthshine", "moon_maria"))
        default_off = cfg_bare.get("sky", {}).get("moon") is False
        disc = sky_rows.get("sun_disc")
        want_disc = MOON_DISC_SHIPPING
        ok = px == 0 and has and default_off and disc is not None and \
            abs(disc - want_disc) < 1e-6
        print(f"  moon-config {'PASS' if ok else 'FAIL'}  restored {px} px (want 0), seven "
              f"moon rows carried={has}, a no-moon session dumps moon false={default_off}, "
              f"shipping disc {disc} (want {want_disc} exactly)")
        if not ok:
            failures.append("moon-config")

    # --- moon-det: two runs of the heaviest configuration --------------------
    # d_on is already one run of exactly disc_common, so this is the second --
    # the stars-det shape, and one process back.
    det_b, eb2 = _moon_render(workdir, "det_b", disc_common)
    if eb2:
        print(f"  moon-det ERROR render failed: {eb2.strip()[-200:]}")
        failures.append("moon-det")
    else:
        px, _ = compare(d_on, det_b)
        ok = px == 0
        print(f"  moon-det {'PASS' if ok else 'FAIL'}  {px} px between two runs (want 0: "
              f"the maria are the group's only new hash)")
        if not ok:
            failures.append("moon-det")

    return failures
