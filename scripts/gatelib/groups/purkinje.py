"""The purkinje group: Purkinje / scotopic shift (spec 11.83 / B14)."""

import json
import math
import os

from gatelib.harness import NO_HALOS, asset, render
from gatelib.images import _read_ppm, compare
from gatelib.scenes import _cscn_camera, _projector, cscn_copy
from gatelib.sky import STARS_FIXTURE, _moon_disc_geometry, _moon_face_pixels


# ---------------------------------------------------------------------------
# Purkinje / scotopic shift (spec 11.83)
# ---------------------------------------------------------------------------

PK_FIXTURE = "purkinje_fixture.gltf"
PK_SCENE = "purkinje_fixture.cscn"
# Every arm renders with these three, and none of them is tidiness.
#   --no-dither    the dim rungs are read at low code values, where one dither
#                  LSB is the size of the signal
#   --no-vignette  the outer patches are the ladder's own ends; darkening them
#                  toward each other compresses the axis under test
#   --no-bloom     LOAD-BEARING HERE: a bright rung's halo raises its dim
#                  neighbour's ABSOLUTE radiance, which moves that neighbour's
#                  rod weight. With bloom on, purkinje-ladder reads the bloom
#                  radius rather than the mesopic ramp.
#   --no-ssao      LOAD-BEARING TOO, and less obvious: aoFactor MULTIPLIES the
#                  radiance the local ramp reads (tonemap_frag's composite), and
#                  the ramp's slope is ~76 codes per stop, so a 0.97 AO factor is
#                  already 3.4 codes against purkinje-agree's 2.0-code bar. The
#                  twin models the authored emissive alone.
#
# NOTE there is deliberately no -E here. The fixture AUTHORS its exposure, and
# that authored value is the whole reason its dim rungs are legible in 8 bits --
# overriding it with -E 1.0 in every arm is how beach_fixture's `waves: fft` and
# water_fixture's sun both came to be authored and unexercised. Arms that need a
# different exposure (purkinje-absolute) pass -E explicitly and it wins.
PK_PIN = ["--no-auto-exposure", "--no-dither", "--no-vignette", *NO_HALOS, "--no-ssao"]

# The shipped ramp, restated so the Python twin can evaluate it. HELD BY
# purkinje-agree, which evaluates this copy against the rendered debug view --
# move the edges in the GLSL without moving them here and that arm fails.
#
# NOT by purkinje-config, which an earlier version of this comment claimed: that
# arm reads a config snapshot, and these are GLSL consts with no C-side
# representation, so no snapshot can carry them. The same shape of false
# attribution the IES fold shipped for a spec cycle -- a comment naming a holder
# that cannot hold, which is worse than naming none because the next reader
# trusts it.
PK_LOCAL_LO, PK_LOCAL_HI = -8.0, -3.0
PK_DEFAULT_STRENGTH = 0.7
# Agreement bar for the twin, in 8-bit codes. One code is the quantization of
# the debug view itself, so this is as tight as the readout allows.
PK_AGREE_CODES = 2.0
# WHICH RUNGS THE COLOUR ARMS READ, and both are measured choices rather than the
# ladder's ends. Seven stops of radiance cannot all be legible in 8 bits -- the
# neutral tonemap crushes its dark end hard, so the dimmest rung lands at code 2
# where a chroma read is mostly quantization. Rung 5 carries weight 0.627 with
# the neutral row still at code 7-18, which is where the signal peaks; rung 1 is
# past the ramp's bright edge at weight EXACTLY 0, which is what makes it the
# in-frame unshifted reference rather than a second render.
PK_DIM_RUNG = 5
PK_LIT_RUNG = 1


def _pk_render(workdir, name, extra, scene=None):
    out = os.path.join(workdir, "pk_" + name + ".ppm")
    src = scene or asset(PK_SCENE)
    err = render(src, out, PK_PIN + extra)
    return (None, err) if err else (out, None)


def _pk_patch_points():
    """Patch centres and their (row, rung), out of the fixture's own glTF.

    Read rather than restated, _lut_patch_points' reason: a transcribed copy
    agrees with the asset only until someone edits the generator, and the
    failure is silent -- the arm keeps passing while sampling the backdrop.
    """
    with open(asset(PK_FIXTURE)) as f:
        nodes = json.load(f)["nodes"]
    out = []
    for n in nodes:
        nm = n.get("name", "")
        if not nm.startswith("pk_"):
            continue
        _, row, rung = nm.split("_")
        out.append((row, int(rung), tuple(n["translation"])))
    return out


def _pk_read(path):
    """{(row, rung): (r, g, b)} at each patch's projected centre."""
    w, h, pix = _read_ppm(path)
    proj = _projector(_cscn_camera(PK_SCENE), w, h)
    vals = {}
    for row, rung, centre in _pk_patch_points():
        x, y = proj(centre)
        o = (int(y) * w + int(x)) * 3
        vals[(row, rung)] = (pix[o], pix[o + 1], pix[o + 2])
    return vals


def _pk_ladder_radiance():
    """Each rung's authored ABSOLUTE radiance, from the fixture's own emissives.

    The neutral row is scaled to photopic luminance 1, so its emissive value IS
    the rung's luminance -- which is what the rod ramp is a function of.
    """
    with open(asset(PK_FIXTURE)) as f:
        g = json.load(f)
    by_name = {m["name"]: m for m in g["materials"]}
    out = {}
    for rung in range(64):
        m = by_name.get(f"pk_neutral_{rung}")
        if m is None:
            break
        e = m["emissiveFactor"]
        out[rung] = 0.2126 * e[0] + 0.7152 * e[1] + 0.0722 * e[2]
    return out


def _pk_expected_local(radiance):
    """The local gate's closed form, evaluated independently of the shader."""
    x = math.log2(max(radiance, 1e-8))
    t = (x - PK_LOCAL_LO) / (PK_LOCAL_HI - PK_LOCAL_LO)
    t = max(0.0, min(1.0, t))
    return 1.0 - t * t * (3.0 - 2.0 * t)


def _pk_chroma(rgb):
    """Chromaticity as (r, b) fractions of the sum. Hue without brightness."""
    s = sum(rgb) or 1.0
    return (rgb[0] / s, rgb[2] / s)


def _pk_spread(vals, rung, rows):
    """How far apart the rows' chromaticities are at one rung.

    THE statistic for 'colour drains', and it is not saturation. Phase 4
    measured HSL mean saturation going the WRONG WAY (0.1696 off against 0.1891
    on): the rod image is monochromatic AND tinted, which is one strong hue, and
    saturation measures distance from grey rather than variety of hue. Losing
    colour vision is the rows ceasing to differ from EACH OTHER, so that is what
    this measures.
    """
    pts = [_pk_chroma(vals[(r, rung)]) for r in rows if (r, rung) in vals]
    if len(pts) < 2:
        return 0.0
    worst = 0.0
    for i in range(len(pts)):
        for j in range(i + 1, len(pts)):
            d = math.hypot(pts[i][0] - pts[j][0], pts[i][1] - pts[j][1])
            worst = max(worst, d)
    return worst


def run_purkinje_gate(workdir):
    """Purkinje / scotopic shift: rod weight, drain, blue shift, per-pixel gating.

      purkinje-agree     the local ramp against a Python twin of its closed form
      purkinje-absolute  the weight is a function of ABSOLUTE radiance, so the
                         same scene at 4x exposure reads the same weight
      purkinje-drain     row chromaticities CONVERGE as the ladder darkens
      purkinje-blue      the neutral row shifts blue, and only where it is dim
      purkinje-ladder    the drain varies ALONG the ladder, with an interior
                         crossover -- the arm a global implementation fails
      purkinje-rowhue    the rod-tint row moves least of all rows
      purkinje-scotopic  the rod WEIGHTS: red goes far darker than blue, which
                         no chromaticity read can see
      purkinje-lamp      a bright disc keeps its colour while its dim surround drains
      purkinje-scene     a night frame moves, a daylight frame is 0 px
      purkinje-terms     acuity and noise are each scaled by the weight: both are
                         bit-identical at the lit rung and both move at the dim one
      purkinje-sharpen   the four sharpen taps run the stage and are inert at zero
                         weight -- the only arm in the suite that renders --sharpen
      purkinje-strength  0.5 reads half the weight of 1.0; 0 is a bit-exact identity
      purkinje-off       authored == flags; --no-purkinje == never authored
      purkinje-config    the rows exist and read their shipping defaults
      purkinje-det       two runs of the heaviest configuration, 0 px

    ANTI-VACUITY PAIRS, and no half of any of them is safe alone. Written as
    prose rather than as an indented list because gate-arm-docs reads a
    two-space name as an arm, and these are cross-references.

    THE DRAIN AND THE BLUE SHIFT. A desaturate-only build moves the neutral row
    by EXACTLY 0 codes, since grey is a fixed point of desaturation, so the blue
    arm fails at its floor rather than near it. A tint-only build RAISES blue's
    saturation where it lowers red's, which is why the drain arm reads
    chromaticity CONVERGENCE and not a mean -- and measurement is what forced
    that: HSL mean saturation goes the WRONG WAY on a correct build, because a
    monochromatic tinted image is one strong hue and saturation measures
    distance from grey rather than variety of hue.

    AGREE AND ABSOLUTE. A constant weight passes `absolute` trivially -- it is 0
    px across exposures whatever it is -- so that arm cannot stand alone, the
    way lut-identity cannot.

    LADDER AND LAMP. A synthetic chart against a real frame with a sky, bloom
    and a tone curve in the path.

    THE SCENE ARM'S OWN TWO LEGS. 0 px alone passes a dead feature, 11.22's
    lesson.
    """
    fixture = asset(PK_SCENE)
    if not os.path.exists(fixture):
        print("  purkinje     SKIP  (missing purkinje_fixture.cscn)")
        return []

    failures = []
    rows = ["neutral", "red", "green", "blue", "warm", "rodhue"]
    chromatic = ["red", "green", "blue", "warm"]
    ladder = _pk_ladder_radiance()
    rungs = sorted(ladder)

    # --- purkinje-agree ----------------------------------------------------
    dbg, err = _pk_render(workdir, "dbg", ["--purkinje", "--purkinje-debug"])
    if err:
        print(f"  purkinje-agree ERROR {err}")
        return ["purkinje-agree"]
    seen = _pk_read(dbg)
    worst, worst_at = 0.0, None
    for rung in rungs:
        want = _pk_expected_local(ladder[rung]) * 255.0
        got = seen[("neutral", rung)][1]  # G is the local gate
        if abs(got - want) > worst:
            worst, worst_at = abs(got - want), rung
    ok = worst <= PK_AGREE_CODES
    print(f"  purkinje-agree {'PASS' if ok else 'FAIL'}  local ramp vs its closed form: worst "
          f"{worst:.2f} codes at rung {worst_at} (want <={PK_AGREE_CODES}; scaling the ramp's "
          f"knee by 2 reads tens)")
    if not ok:
        failures.append("purkinje-agree")

    # --- purkinje-absolute -------------------------------------------------
    # 4x the fixture's OWN authored exposure, read rather than transcribed --
    # and a power-of-two ratio, so both exposures and their quotient are exact.
    # At a non-exact ratio the two weights differ in the last fp32 bit and
    # quantize to different codes on the ramp's steep part (measured 148 px).
    with open(asset(PK_SCENE)) as fh:
        _authored_e = float(json.load(fh)["post"]["exposure"])
    dbg4, err = _pk_render(workdir, "dbg4",
                           ["-E", repr(_authored_e * 4.0), "--purkinje", "--purkinje-debug"])
    if err:
        print(f"  purkinje-absolute ERROR {err}")
        failures.append("purkinje-absolute")
    else:
        ae, _ = compare(dbg, dbg4)
        ok = ae == 0
        print(f"  purkinje-absolute {'PASS' if ok else 'FAIL'}  the weight at 1x and 4x exposure: "
              f"{ae} px (want exactly 0 -- it is a function of ABSOLUTE radiance, so dropping "
              f"oneOverPreExposure shifts the whole ladder by 2 stops)")
        if not ok:
            failures.append("purkinje-absolute")

    # --- the on/off pair every colour arm reads ----------------------------
    off, e1 = _pk_render(workdir, "off", [])
    # SPECTRAL HALF ONLY. Acuity pools across patch boundaries, lifting a dim
    # rung toward its brighter neighbour -- which cancelled the tint's own
    # darkening and made purkinje-blue read R+0 where the shift alone gives R-2.
    # The same confound the generator's pitch assert guards for bloom. Acuity and
    # noise get their own arm rather than contaminating the colour reads.
    on, e2 = _pk_render(workdir, "on", ["--purkinje", "--no-purkinje-noise",
                                        "--no-purkinje-acuity"])
    if e1 or e2:
        print(f"  purkinje-drain ERROR {e1 or e2}")
        return failures + ["purkinje-drain"]
    v_off, v_on = _pk_read(off), _pk_read(on)

    # --- purkinje-drain ----------------------------------------------------
    # The rows' chromaticities must CONVERGE toward each other as the ladder
    # darkens. Read at the two ends, so the claim is about the axis rather than
    # about one patch.
    bright, dim = PK_LIT_RUNG, PK_DIM_RUNG
    sp_off_dim = _pk_spread(v_off, dim, chromatic)
    sp_on_dim = _pk_spread(v_on, dim, chromatic)
    sp_on_bright = _pk_spread(v_on, bright, chromatic)
    sp_off_bright = _pk_spread(v_off, bright, chromatic)
    shrink = sp_on_dim / sp_off_dim if sp_off_dim else 1.0
    keep = sp_on_bright / sp_off_bright if sp_off_bright else 1.0
    ok = shrink <= 0.85 and keep >= 0.98
    print(f"  purkinje-drain {'PASS' if ok else 'FAIL'}  hue spread across the chromatic rows: "
          f"dim rung {sp_off_dim:.4f} -> {sp_on_dim:.4f} ({shrink:.3f}x, want <=0.85 -- correct "
          f"reads 0.689 and a build that shifts nothing reads 1.000), lit rung {keep:.3f}x "
          f"(want >=0.98: past the ramp the weight is 0 and colour must survive)")
    if not ok:
        failures.append("purkinje-drain")

    # --- purkinje-blue -----------------------------------------------------
    # The neutral row is an EXACT fixed point of desaturation, so anything that
    # moves it is the tint. Signed: B up and R down, not merely different.
    # B MINUS R, not a single channel. The dim rungs sit at single-digit codes
    # where one channel's move is the size of the quantization, but the SPLIT
    # between the ends of the spectrum is several codes and the off patch's own
    # split is exactly 0 by construction -- it is grey.
    n_off, n_on = v_off[("neutral", dim)], v_on[("neutral", dim)]
    split_off, split_on = n_off[2] - n_off[0], n_on[2] - n_on[0]
    d_bright = tuple(v_on[("neutral", bright)][k] - v_off[("neutral", bright)][k] for k in range(3))
    moved_bright = max(abs(x) for x in d_bright)
    ok = split_off == 0 and split_on >= 5 and moved_bright == 0
    print(f"  purkinje-blue {'PASS' if ok else 'FAIL'}  the neutral row's B-R split: {split_off} "
          f"off (want exactly 0 -- grey is an exact fixed point of desaturation, so anything "
          f"here is the TINT) -> {split_on} on (want >=5; correct reads 11, a tint of vec3(1) "
          f"reads 0), and the lit rung moves {moved_bright} codes (want exactly 0)")
    if not ok:
        failures.append("purkinje-blue")

    # --- purkinje-ladder ---------------------------------------------------
    # THE arm a global implementation cannot pass, and it reads the WEIGHT
    # ITSELF rather than a colour shift. The first version measured the red
    # row's chroma displacement along the ladder and PASSED the global
    # mutation: with a uniform weight the displacement still varies rung to
    # rung, because the tone curve is not linear, so it was reading curvature
    # rather than the ramp. The weight is the quantity that must vary per pixel,
    # so the weight is what to look at.
    ws = [seen[("neutral", r)][0] / 255.0 for r in rungs]
    rising = all(ws[i] <= ws[i + 1] + 0.01 for i in range(len(ws) - 1))
    span = ws[-1] - ws[0]
    interior = [i for i in range(1, len(ws) - 1)
                if ws[0] + 0.15 * span < ws[i] < ws[-1] - 0.15 * span]
    ok = rising and span >= 0.5 and len(interior) >= 2 and ws[0] <= 0.02
    print(f"  purkinje-ladder {'PASS' if ok else 'FAIL'}  the rod weight along 8 rungs of ONE "
          f"chromaticity: {ws[0]:.3f} -> {ws[-1]:.3f} (span {span:.3f}, want >=0.5), monotone="
          f"{rising}, {len(interior)} interior rungs (want >=2), lit end {ws[0]:.3f} (want "
          f"<=0.02). A global weight is CONSTANT here: span 0, 0 interior.")
    if not ok:
        failures.append("purkinje-ladder")

    # --- purkinje-rowhue ---------------------------------------------------
    # The rod tint's own chromaticity is a near fixed point of the whole
    # operator. Only a wrong tint HUE fails this.
    moves = {}
    for r in rows[1:]:
        a = _pk_chroma(v_off[(r, dim)])
        b = _pk_chroma(v_on[(r, dim)])
        moves[r] = math.hypot(a[0] - b[0], a[1] - b[1])
    least = min(moves, key=moves.get)
    ok = least == "rodhue"
    print(f"  purkinje-rowhue {'PASS' if ok else 'FAIL'}  least-moved row at the dim rung is "
          f"'{least}' (want 'rodhue'): " +
          ", ".join(f"{k} {v:.4f}" for k, v in sorted(moves.items(), key=lambda kv: kv[1])))
    if not ok:
        failures.append("purkinje-rowhue")

    # --- purkinje-scotopic -------------------------------------------------
    # THE ROD WEIGHTS THEMSELVES, and nothing else in this group can see them.
    # Every colour arm above reads CHROMATICITY, which is scale-invariant -- the
    # rod image is (rod luminance) x (tint), so its hue is the tint's whatever
    # the weights are. Replacing PURKINJE_SCOTOPIC_W with photopic luma passed
    # the entire group, which means "reds go near-black" was untested.
    #
    # So this reads LUMINANCE. The rows are normalised to equal PHOTOPIC
    # luminance by the generator, so off they are equally bright by
    # construction; on, red must go far darker than blue, because V'(610) is
    # about 1/32 of V(610) while blue sits near the rods' own peak. Under
    # photopic weights the two would stay equal, which is the mutation.
    def _luma(c):
        return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]

    lum_off = {r: _luma(v_off[(r, dim)]) for r in ("red", "blue")}
    lum_on = {r: _luma(v_on[(r, dim)]) for r in ("red", "blue")}
    base = abs(lum_off["red"] - lum_off["blue"]) / max(lum_off["red"], 1.0)
    ratio = (lum_on["red"] / lum_on["blue"]) if lum_on["blue"] else 1.0
    ok = base <= 0.15 and ratio <= 0.6
    print(f"  purkinje-scotopic {'PASS' if ok else 'FAIL'}  the rows are equally bright OFF "
          f"({lum_off['red']:.1f} vs {lum_off['blue']:.1f}, {base:.3f} apart, want <=0.15 -- the "
          f"generator normalises them to equal photopic luma), and ON red reads {ratio:.3f} of "
          f"blue (want <=0.6: rods barely see red. Correct reads 0.418; photopic weights read "
          f"0.760, not 1.000 -- the tint and the tone curve both survive the mutation)")
    if not ok:
        failures.append("purkinje-scotopic")

    # --- purkinje-lamp -----------------------------------------------------
    # The per-pixel claim on a REAL frame: the moon's disc is locally bright and
    # must keep its colour while the star field around it drains. The disc mask
    # is PREDICTED from the fixture's own angles, never found by brightness --
    # beach-shoreline was a known red for four specs because an argmax read dry
    # sand.
    # A SMALLER DISC THAN THE FIXTURE SHIPS, and it is a correctness requirement
    # rather than framing. moon_fixture draws the moon at 49x life size, which
    # covers enough of the frame to BE the meter's kept-brightest-30% population
    # -- so the frame's metered mean is the disc, the global gate closes, and the
    # fixture renders its own subject unshifted (measured: global gate exactly
    # 0.0000). At 12x the disc is still hundreds of pixels to sample while the
    # kept population stays the dark star field, and the gate opens.
    moon_src = asset("moon_fixture.cscn")
    if not os.path.exists(moon_src):
        print("  purkinje-lamp SKIP  (missing moon_fixture.cscn)")
    else:
        PK_LAMP_SIZE = 12.0
        moon_scene = os.path.join(workdir, "pk_lamp.cscn")
        cscn_copy(moon_src, moon_scene,
                  lambda d: d["environment"]["moon"].update({"size": PK_LAMP_SIZE}))
        m_flags = ["--no-auto-exposure", "--no-dither", *NO_HALOS, "--no-purkinje-noise",
                   "--no-purkinje-acuity"]
        m_off = os.path.join(workdir, "pk_moon_off.ppm")
        m_on = os.path.join(workdir, "pk_moon_on.ppm")
        e1 = render(moon_scene, m_off, m_flags)
        e2 = render(moon_scene, m_on, m_flags + ["--purkinje"])
        if e1 or e2:
            print(f"  purkinje-lamp ERROR {e1 or e2}")
            failures.append("purkinje-lamp")
        else:
            w, h, pa = _read_ppm(m_off)
            _, _, pb = _read_ppm(m_on)
            with open(moon_scene) as f:
                mj = json.load(f)["environment"]["moon"]
            cam = _cscn_camera("moon_fixture.cscn")
            radius, cx, cy = _moon_disc_geometry(cam, 0.53 * PK_LAMP_SIZE, mj["elevation"],
                                                 mj["azimuth"], w, h)
            disc = {(px, py) for px, py, _ in _moon_face_pixels(cx, cy, radius, w, h, 0.6)}

            def chroma_shift(pixels):
                tot, n = 0.0, 0
                for (x, y) in pixels:
                    o = (y * w + x) * 3
                    a = _pk_chroma((pa[o], pa[o + 1], pa[o + 2]))
                    b = _pk_chroma((pb[o], pb[o + 1], pb[o + 2]))
                    if sum((pa[o], pa[o + 1], pa[o + 2])) < 12:
                        continue
                    tot += math.hypot(a[0] - b[0], a[1] - b[1])
                    n += 1
                return tot / n if n else 0.0

            sky = [(x, y) for y in range(0, h, 3) for x in range(0, w, 3)
                   if (x, y) not in disc]
            d_disc = chroma_shift(sorted(disc)[::3])
            d_sky = chroma_shift(sky)
            ok = d_sky > 0.004 and d_disc <= d_sky * 0.5
            print(f"  purkinje-lamp {'PASS' if ok else 'FAIL'}  the lit disc shifts {d_disc:.5f} "
                  f"against its star field's {d_sky:.5f} (want disc <=half and field >0.004; a "
                  f"global weight drains both equally)")
            if not ok:
                failures.append("purkinje-lamp")

    # --- purkinje-scene ----------------------------------------------------
    aerial = asset(STARS_FIXTURE)
    common = ["--no-scene-file", "--sky", "--no-auto-exposure", "-E", "1.0", "--no-dither"]
    night = common + ["--stars", "--night-floor", "--sun-elevation", "-12"]
    day = common + ["--sun-elevation", "35"]
    paths = {}
    err = None
    for tag, flags, pk in (("n_off", night, False), ("n_on", night, True),
                           ("d_off", day, False), ("d_on", day, True)):
        p = os.path.join(workdir, f"pk_sc_{tag}.ppm")
        err = err or render(aerial, p, flags + (["--purkinje"] if pk else []))
        paths[tag] = p
    if err:
        print(f"  purkinje-scene ERROR {err}")
        failures.append("purkinje-scene")
    else:
        ae_n, _ = compare(paths["n_off"], paths["n_on"])
        ae_d, _ = compare(paths["d_off"], paths["d_on"])
        ok = ae_n >= 20000 and ae_d == 0
        print(f"  purkinje-scene {'PASS' if ok else 'FAIL'}  a night frame moves {ae_n} px "
              f"(want >=20000) while a daylight frame moves {ae_d} (want exactly 0: the global "
              f"gate's smoothstep is exactly 1 past its edge, so the weight is exactly 0)")
        if not ok:
            failures.append("purkinje-scene")

    # --- purkinje-terms ----------------------------------------------------
    # Both terms are SCALED BY THE WEIGHT, and the first version of this arm
    # could not see that: it asserted only that each moved some pixels, so
    # deleting `* w` from either -- making the pooling and the noise uniform over
    # the whole frame -- kept it green. Same shape as the blind spot the
    # falsification round found in the colour arms: complete against the
    # structure, blind to what scales it.
    #
    # The lever is --purkinje-bias -30, which drags both ramps so far below the
    # fixture's radiances that every pixel sits past their high edge and w is
    # EXACTLY 0 frame-wide. A term carrying `* w` is then inert to the pixel; one
    # that has lost it is not. Two wrong read regions were tried first and are
    # worth recording: patch CENTRES read 0 for acuity (averaging a constant
    # returns it), and a patch-edge box reads the lit column moving MORE than the
    # dim one, because an edge pixel is part backdrop and therefore dim whatever
    # rung it belongs to -- the effect there scales with the patch's brightness,
    # not with the weight.
    # THE LEVER IS STRENGTH, not a zero-weight bias, and the first attempt got
    # that wrong in an instructive way. Forcing w to 0 frame-wide does not test
    # the terms at all: purkinjeShift and the pooling are called only when
    # w > 0, so the CALLER's gate makes both inert whatever the terms do
    # internally -- the mutation passed. Halving purkinjeStrength halves w
    # everywhere while keeping it non-zero, so a term carrying `* w` shrinks with
    # it and one that has lost it does not.
    HALF = ["--purkinje-strength", "0.35"]
    spec, e1 = _pk_render(workdir, "spec",
                          ["--purkinje", "--no-purkinje-acuity", "--no-purkinje-noise"])
    acu, e2 = _pk_render(workdir, "acu", ["--purkinje", "--no-purkinje-noise"])
    noi, e3 = _pk_render(workdir, "noi", ["--purkinje", "--no-purkinje-acuity"])
    h_spec, e4 = _pk_render(workdir, "h_spec",
                            ["--purkinje", "--no-purkinje-acuity", "--no-purkinje-noise"] + HALF)
    h_acu, e5 = _pk_render(workdir, "h_acu", ["--purkinje", "--no-purkinje-noise"] + HALF)
    h_noi, e6 = _pk_render(workdir, "h_noi", ["--purkinje", "--no-purkinje-acuity"] + HALF)
    err = e1 or e2 or e3 or e4 or e5 or e6
    if err:
        print(f"  purkinje-terms ERROR {err}")
        failures.append("purkinje-terms")
    else:
        # ACUITY is measured by EXTENT and NOISE by AMPLITUDE, because that is
        # how each one carries w. A smaller pooling radius touches fewer pixels
        # while its peak at a bright edge barely moves (measured 0.957x on PAE,
        # which is why the first version of this read the wrong statistic);
        # noise is a multiply, so its peak halves exactly with w.
        pa_full, _ = compare(spec, acu)
        pa_half, _ = compare(h_spec, h_acu)
        _, pn_full = compare(spec, noi)
        _, pn_half = compare(h_spec, h_noi)
        ra = pa_half / pa_full if pa_full else 1.0
        rn = pn_half / pn_full if pn_full else 1.0
        ok = pa_full > 0 and pn_full > 0 and ra <= 0.85 and rn <= 0.7
        print(f"  purkinje-terms {'PASS' if ok else 'FAIL'}  halving the strength takes acuity's "
              f"reach from {pa_full} to {pa_half} px ({ra:.3f}x, want <=0.85) and rod noise's peak "
              f"from {pn_full:.4f} to {pn_half:.4f} ({rn:.3f}x, want <=0.7) -- each term is scaled "
              f"by w, and one that lost its `* w` does not shrink when w does")
        if not ok:
            failures.append("purkinje-terms")

    # --- purkinje-sharpen --------------------------------------------------
    # THE ONLY ARM IN THE SUITE THAT RENDERS --sharpen AT ALL. sceneToToned is
    # called five times under it, and the four neighbour taps were previously
    # executed by nothing: --sharpen appears once in this file (with the shift
    # off) and never in goldens.py, so the whole tap path was dead under gate
    # conditions.
    #
    # WHAT IT HOLDS, stated plainly because it is less than it looks: that the
    # sharpen taps run the stage, and that they are inert where the weight is
    # zero. It CANNOT distinguish a tap pooling its own neighbourhood from all
    # four pooling the centre's -- both produce a plausible frame, and the
    # difference is a fraction of a texel in an unsharp mask. That claim rests on
    # the signature, which now takes the coordinate ALONE and derives the sample
    # from it, so a mismatched pair is unrepresentable rather than merely untested.
    sh_on, e1 = _pk_render(workdir, "sh_on",
                           ["--purkinje", "--no-purkinje-noise", "--sharpen", "0.8"])
    sh_off, e2 = _pk_render(workdir, "sh_off",
                            ["--purkinje", "--no-purkinje-noise", "--no-purkinje-acuity",
                             "--sharpen", "0.8"])
    ZERO_W = ["--purkinje-bias", "-30"]
    sh_z_on, e3 = _pk_render(workdir, "sh_z_on",
                             ["--purkinje", "--no-purkinje-noise", "--sharpen", "0.8"] + ZERO_W)
    sh_z_off, e4 = _pk_render(workdir, "sh_z_off",
                              ["--purkinje", "--no-purkinje-noise", "--no-purkinje-acuity",
                               "--sharpen", "0.8"] + ZERO_W)
    err = e1 or e2 or e3 or e4
    if err:
        print(f"  purkinje-sharpen ERROR {err}")
        failures.append("purkinje-sharpen")
    else:
        live, _ = compare(sh_off, sh_on)
        zero, _ = compare(sh_z_off, sh_z_on)
        ok = live > 0 and zero == 0
        print(f"  purkinje-sharpen {'PASS' if ok else 'FAIL'}  under --sharpen the pooling moves "
              f"{live} px at the shipping ramp and {zero} with the weight forced to zero (want >0 "
              f"and exactly 0). The only arm that executes the four neighbour taps at all")
        if not ok:
            failures.append("purkinje-sharpen")

    # --- purkinje-strength -------------------------------------------------
    half, e_h = _pk_render(workdir, "half",
                         ["--purkinje", "--purkinje-strength", "0.35",
                          "--purkinje-debug"])
    zero, e_z = _pk_render(workdir, "zero",
                         ["--purkinje", "--purkinje-strength", "0"])
    if e_h or e_z:
        print(f"  purkinje-strength ERROR {e_h or e_z}")
        failures.append("purkinje-strength")
    else:
        # Half the DEFAULT strength must read half the weight, at a patch where
        # the local gate is fully open so the ratio is not confounded by it.
        full_w = seen[("neutral", rungs[-1])][0]
        half_w = _pk_read(half)[("neutral", rungs[-1])][0]
        ratio = half_w / full_w if full_w else 0.0
        ae0, _ = compare(off, zero)
        ok = abs(ratio - 0.5) <= 0.03 and ae0 == 0
        print(f"  purkinje-strength {'PASS' if ok else 'FAIL'}  0.35 against the default "
              f"{PK_DEFAULT_STRENGTH} reads {ratio:.4f} of the weight (want 0.5 +/-0.03; w*s*s "
              f"reads 0.25), and strength 0 is {ae0} px (want exactly 0)")
        if not ok:
            failures.append("purkinje-strength")

    # --- purkinje-off ------------------------------------------------------
    authored = os.path.join(workdir, "pk_authored.cscn")
    cscn_copy(asset(PK_SCENE), authored, lambda d: d["post"].update(
        {"purkinje": {"enabled": True, "strength": 0.55}}))
    a_out, e1 = _pk_render(workdir, "authored", [], scene=authored)
    c_out, e2 = _pk_render(workdir, "bycli", ["--purkinje", "--purkinje-strength", "0.55"])
    n_out, e3 = _pk_render(workdir, "authoff", ["--no-purkinje"], scene=authored)
    if e1 or e2 or e3:
        print(f"  purkinje-off ERROR {e1 or e2 or e3}")
        failures.append("purkinje-off")
    else:
        ae_same, _ = compare(a_out, c_out)
        ae_off, _ = compare(n_out, off)
        ok = ae_same == 0 and ae_off == 0
        print(f"  purkinje-off {'PASS' if ok else 'FAIL'}  authored vs the same flags {ae_same} px "
              f"(want 0: the two paths are one path), --no-purkinje over the file {ae_off} px "
              f"(want 0: the flag undoes the whole authored block)")
        if not ok:
            failures.append("purkinje-off")

    # --- purkinje-config ---------------------------------------------------
    dump = os.path.join(workdir, "pk_config.json")
    err = render(asset(PK_SCENE),
                 os.path.join(workdir, "pk_cfg.ppm"),
                 PK_PIN + ["--config-dump", dump])
    if err or not os.path.exists(dump):
        print(f"  purkinje-config ERROR {err or 'no dump'}")
        failures.append("purkinje-config")
    else:
        with open(dump) as f:
            cfg = json.load(f)
        blk = cfg.get("postfx", {}).get("purkinje", {})
        want_keys = {"enabled", "strength", "bias_ev", "acuity", "noise"}
        have = want_keys <= set(blk)
        # OFF BY DEFAULT is the claim, and naming the field is the only way to
        # pin it -- 27 goldens reading 0 px prove the branch is gated and nothing
        # about which way the switch points.
        ok = (have and blk.get("enabled") is False
              and abs(blk.get("strength", -1) - PK_DEFAULT_STRENGTH) < 1e-6
              and abs(blk.get("bias_ev", -1)) < 1e-6)
        print(f"  purkinje-config {'PASS' if ok else 'FAIL'}  five rows carried={have}, "
              f"enabled={blk.get('enabled')} (want False), strength={blk.get('strength')} "
              f"(want {PK_DEFAULT_STRENGTH}), bias_ev={blk.get('bias_ev')} (want 0)")
        if not ok:
            failures.append("purkinje-config")

    # --- purkinje-det ------------------------------------------------------
    det_a, e_a = _pk_render(workdir, "det_a", ["--purkinje"])
    det_b, e_b = _pk_render(workdir, "det_b", ["--purkinje"])
    if e_a or e_b:
        print(f"  purkinje-det ERROR {e_a or e_b}")
        failures.append("purkinje-det")
    else:
        ae, _ = compare(det_a, det_b)
        ok = ae == 0
        print(f"  purkinje-det {'PASS' if ok else 'FAIL'}  {ae} px between two runs of the full "
              f"stack (want 0: the rod noise rides the frame seed, which is deterministic across "
              f"equal --frames runs)")
        if not ok:
            failures.append("purkinje-det")

    return failures
