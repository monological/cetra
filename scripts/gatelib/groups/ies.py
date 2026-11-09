"""The ies group: IES photometric profiles (table, symmetry, seeding, fold; spec 11.57)."""

import math
import os

from gatelib.harness import _probe_render, asset, render
from gatelib.images import _SRGB_TO_LINEAR, _read_ppm, compare
from gatelib.scenes import cscn_copy


# IES photometric profiles (spec 11.57), on assets/ies_fixture and its four .ies files.
#
# Read through --ies-probe rather than off a frame, and the reason is the whole shape of
# this group: a table resampled off the wrong plane, folded with a modulo where LM-63
# mirrors, or scaled by the wrong multiplier still lights a room plausibly. There is no
# picture that distinguishes a correct luminaire from a confident wrong one.
#
# The numbers below are CONSTRUCTED, not measured. assets/gen_ies_fixture.py writes every
# candela as a closed form of its two angles, so the arms re-evaluate that same form and
# compare -- ground truth painted rather than read back off the thing under test. Mirrors
# that generator and has to match it.
IES_PEAK_CD = 1200.0
IES_FILES = [
    # name, declared span, symmetric
    ("ies_symmetric.ies", 360.0, True),
    ("ies_bilateral.ies", 180.0, False),
    ("ies_quadrant.ies", 90.0, False),
    ("ies_asymmetric.ies", 360.0, False),
    # The one file whose 90-degree candela is nonzero IN THE FILE (1.2e-6 cd). The
    # other four paint a literal 0, which reads back as 0 whether or not the snap
    # in ies.c exists -- so they measure the arithmetic and this one measures the
    # guarantee pbr_frag's early-out rests on.
    ("ies_tinytail.ies", 360.0, True),
]
# How far a tap may sit from its closed form, in units of the PEAK. The fixture's own
# angles are a subset of the resampled grid, so the agreement is exact arithmetic rather
# than a fit and the residual is float32 round-trip alone: the table is stored normalised
# and the probe multiplies the peak back in. Measured 9.5e-7.
#
# Relative to the peak rather than to the tap's own value, which is the choice worth
# stating because the obvious reading is the other one. The residual is quantisation of the
# NORMALISED table, so it is uniform in units of peak and does NOT scale with how bright
# the tap is -- dividing by the expected value would demand ever-tighter agreement down the
# tail and reduce to 0/0 at the zeros. Dividing by a bare candela figure would be wrong
# too: a fixture at 1200 cd and one at 12 cd would then need different numbers.
IES_TAP_REL_EPS = 1e-5


def _ies_expected(v_deg, h_deg, span):
    """The fixture's closed form, in candela. Mirrors gen_ies_fixture.py's candela().

    Restated rather than imported. Importing the generator would make the ground truth a
    function of the thing under test, which is the one property this whole group exists to
    avoid -- the same reason CONE_INNER_DEG is restated a few hundred lines below.

    The tail is 0 for every file including the one whose FILE says 1.2e-6: the snap turns a
    residual under 1e-6 relative into an exact zero, so 0 is the right expectation for all
    five and only ies_tinytail.ies makes that a claim rather than a copy.
    """
    lobe = 0.0 if v_deg >= 90.0 else math.cos(math.radians(v_deg))
    return IES_PEAK_CD * lobe * (1.0 - h_deg / span)


def _ies_probe(workdir, scene, extra=None):
    """Run --ies-probe and split its rows by kind. Returns (profiles, samples, mirror, text)."""
    rows, text = _probe_render(scene, "--ies-probe", "ies-probe", extra=extra, frames=2)
    return ([r for r in rows if r.get("kind") == "profile"],
            [r for r in rows if r.get("kind") == "sample"],
            [r for r in rows if r.get("kind") == "mirror"], text)


def _ies_variant(src, dst, name):
    """A copy of the fixture naming a different .ies. The only thing that varies.

    ROUTED, not bare. cscn_copy resolves a relative profile against the SOURCE
    SCENE's directory, and since 12.8 that is assets/scenes/ while the profiles
    are in assets/ies/ -- a bare name resolved next to the scene, loaded
    nothing, and every arm in this group reported zero rows. An absolute path
    passes cscn_copy's isabs check untouched, which is what the engine gets.
    """
    cscn_copy(src, dst, lambda d: d["lights"][0].__setitem__("profile", asset(name)))


def _mirror_asymmetry(path):
    """Mean |pixel - its left-right mirror|, over the frame's own mean level.

    The fixture is mirror-symmetric about x = 0 -- the lamp, the camera and the plane all
    sit on it -- so the ONLY thing that can make the frame lopsided is the profile's
    azimuthal term. Which makes this a direct read of the horizontal fold: a bilateral
    sweep MIRRORS, so the two halves must match, and a modulo wraps instead and reads the
    far side of the luminaire as the near side.

    Relative to the level because the profiles differ in how much light they put on the
    floor at all, and an absolute bound would then mean something different for each.
    """
    w, h, pix = _read_ppm(path)
    diff, total, n = 0.0, 0.0, 0
    for y in range(h):
        row = y * w
        for x in range(w // 2):
            a, b = (row + x) * 3, (row + (w - 1 - x)) * 3
            la = sum(_SRGB_TO_LINEAR[pix[a + k]] for k in range(3)) / 3.0
            lb = sum(_SRGB_TO_LINEAR[pix[b + k]] for k in range(3)) / 3.0
            diff += abs(la - lb)
            total += la + lb
            n += 1
    return diff / n / max(total / (2 * n), 1e-9)


# Sized against both sides of a hand falsification (iesFold -> mod(angle, span)):
#
#   profile      mirror   modulo
#   bilateral    0.0055   0.9861
#   quadrant     0.0059   0.8347
#
# 0.05 sits an order of magnitude above the dither floor and an order below the wrong
# answer. The symmetric file never reaches the fold (hTaps == 1) and the asymmetric one
# spans 360, where a modulo IS the identity -- so those two read 0.0047 and 1.2139 either
# way, which is what makes them the controls rather than more of the same test.
IES_MIRROR_MAX = 0.05
IES_ASYM_MIN = 0.5


def run_ies_gate(workdir):
    """IES profiles, read as numbers because no frame can tell a wrong table from a right one.

      ies-table     every tap of all four profiles matches the candela the generator
                    constructed, and the 90 degree tail is EXACTLY zero
      ies-symmetry  each file's declared symmetry and span are read off the declaration --
                    one plane is rotational, and 0..90 / 0..180 / 0..360 are quadrant,
                    bilateral and none
      ies-seed      a light authoring no intensity takes the file's own peak candela, which
                    is what makes the normalised shape lossless
      ies-shape     the profile actually reaches the frame at all
      ies-mirror    a PARTIAL sweep mirrors rather than repeats -- read off the frame,
                    since the fold is unreachable from the probe
      ies-replace   ...and the profile REPLACES the cone rather than multiplying it
      ies-flag      --ies-profile overrides what a scene authored, and reaches a scene
                    that authored nothing

    The first three read --ies-probe and the last three render. That split is deliberate:
    the probe exists because a table resampled off the wrong plane still lights a room, so
    the arms that could be fooled by a picture do not look at one -- but the shader is a
    THIRD implementation of the same table, and no probe reaches it. Both halves are
    needed and neither substitutes.
    """
    fixture = asset("ies_fixture.cscn")
    if not os.path.exists(fixture):
        print("  ies-table    SKIP  (missing ies_fixture.cscn)")
        return []
    failures = []

    # One probe run per file, since a scene names one profile at a time.
    reads = {}
    err = None
    for name, span, symmetric in IES_FILES:
        variant = os.path.join(workdir, "ies_" + name.replace(".ies", ".cscn"))
        _ies_variant(fixture, variant, name)
        profiles, samples, mirror, text = _ies_probe(workdir, variant)
        if len(profiles) != 1 or not samples:
            err = f"{name}: {len(profiles)} profile rows, {len(samples)} samples"
            break
        reads[name] = (profiles[0], samples, mirror)

    if err:
        arms = ("ies-table", "ies-symmetry", "ies-seed", "ies-mirror", "ies-shape",
                "ies-replace", "ies-flag")
        for arm in arms:
            print(f"  {arm} ERROR the probe returned nothing usable: {err}")
        return list(arms)

    worst, worst_at, tails = 0.0, "", []
    for name, span, symmetric in IES_FILES:
        _, samples, _ = reads[name]
        for s in samples:
            v, h, cd = float(s["v"]), float(s["h"]), float(s["cd"])
            want = _ies_expected(v, h, span)
            rel = abs(cd - want) / IES_PEAK_CD
            if rel > worst:
                worst, worst_at = rel, f"{name} v={v:g} h={h:g}"
            if v >= 90.0 - 1e-6:
                tails.append(cd)
    tail_ok = all(t == 0.0 for t in tails)
    ok = worst <= IES_TAP_REL_EPS and tail_ok and tails
    print(f"  ies-table    {'PASS' if ok else 'FAIL'}  worst tap error {worst:.3e} relative "
          f"over {sum(len(reads[n][1]) for n, _, _ in IES_FILES)} taps in {len(IES_FILES)} "
          f"files (want <= {IES_TAP_REL_EPS:g}{', at ' + worst_at if worst_at else ''}); "
          f"{len(tails)} tail taps, all exactly zero: {tail_ok} "
          "(exact, because pbr_frag's early-out is only safe if it is)")
    if not ok:
        failures.append("ies-table")

    problems = []
    for name, span, symmetric in IES_FILES:
        p, _, mirror = reads[name]
        got_span, got_sym = float(p["span"]), p["symmetric"] == "1"
        if abs(got_span - span) > 1e-3:
            problems.append(f"{name} span {got_span:g} != {span:g}")
        if got_sym != symmetric:
            problems.append(f"{name} symmetric={got_sym} != {symmetric}")
        # The C fold, on the one input that can distinguish a mirror from a wrap. Every
        # `sample` row above sits inside [0, span] where the two agree exactly, so without
        # these the C copy is unreachable from outside the process. A full 360 sweep has
        # no outside and prints none.
        if span < 360.0 and not mirror:
            problems.append(f"{name} spans {span:g} and printed no mirror rows")
        for m in mirror:
            inside, beyond = float(m["inside"]), float(m["beyond"])
            if abs(inside - beyond) > IES_TAP_REL_EPS:
                problems.append(f"{name} v={float(m['v']):g} folds span+{float(m['h']):g} to "
                                f"{beyond:.6f}, not to span-{float(m['h']):g}'s {inside:.6f}")
                break
    ok = not problems
    print(f"  ies-symmetry {'PASS' if ok else 'FAIL'}  " +
          (f"all {len(IES_FILES)} declarations read back: " +
           ", ".join(f"{n} {float(reads[n][0]['span']):g}deg x{reads[n][0]['h_taps']}"
                     for n, _, _ in IES_FILES)
           if ok else "; ".join(problems)))
    if not ok:
        failures.append("ies-symmetry")

    # The fixture authors NO intensity, so the light must come out at the file's peak.
    _, _, _, text = _ies_probe(workdir, fixture)
    seeded = f"takes its {IES_PEAK_CD:.1f} cd" in text
    lit = f"intensity={IES_PEAK_CD:.6f} cd" in text
    ok = seeded and lit
    print(f"  ies-seed     {'PASS' if ok else 'FAIL'}  unpriced light seeded from the file: "
          f"reported={seeded} on the light={lit} (want both at {IES_PEAK_CD:.0f} cd -- "
          "normalised x peak IS absolute, and this is the step that makes it so)")
    if not ok:
        failures.append("ies-seed")

    # ...and the arms that look at a frame, because "the table is right" and "the shader
    # reads it" are different claims -- and the second is the one that decides pixels.
    def _no_profile(d):
        d["lights"][0].pop("profile", None)
        # The bare light must still be as bright as the profiled one, or this arm
        # measures the SEEDING rather than the shape: without a profile there is
        # no peak to seed from, so it would otherwise fall back to 1 cd.
        d["lights"][0]["intensity"] = IES_PEAK_CD

    base = os.path.join(workdir, "ies_shape_none.cscn")
    cscn_copy(fixture, base, _no_profile)

    shots, render_err = {}, None
    scenes = [("none", base)]
    for name, _, _ in IES_FILES:
        tag = name.replace("ies_", "").replace(".ies", "")
        variant = os.path.join(workdir, f"ies_shape_{tag}.cscn")
        _ies_variant(fixture, variant, name)
        scenes.append((tag, variant))
    for tag, scene in scenes:
        out = os.path.join(workdir, f"ies_shape_{tag}.ppm")
        render_err = render(scene, out, ["-W", "400", "-H", "300", "--no-auto-exposure",
                                         "-E", "0.02"])
        if render_err:
            break
        shots[tag] = out

    if render_err:
        for arm in ("ies-shape", "ies-mirror"):
            print(f"  {arm} ERROR render failed: {render_err.strip()[-200:]}")
        failures += ["ies-shape", "ies-mirror"]
    else:
        moved = compare(shots["none"], shots["symmetric"])[0]
        differs = compare(shots["symmetric"], shots["asymmetric"])[0]
        ok = moved > 0 and differs > 0
        print(f"  ies-shape    {'PASS' if ok else 'FAIL'}  profile vs bare cone {moved} px "
              f"(want > 0: the shader reads the table at all); symmetric vs asymmetric "
              f"{differs} px (want > 0, or the horizontal term is inert and every profile "
              "renders as a lobe)")
        if not ok:
            failures.append("ies-shape")

        # The FOLD, which nothing above can reach: every probe row lies inside [0, span],
        # where a mirror and a modulo agree exactly. Only a rendered frame asks for an
        # angle outside the sweep, and only the two partial sweeps have an outside.
        asym = {name: _mirror_asymmetry(shots[name]) for name in
                ("symmetric", "bilateral", "quadrant", "asymmetric")}
        folded_ok = asym["bilateral"] <= IES_MIRROR_MAX and asym["quadrant"] <= IES_MIRROR_MAX
        # ...and the control, without which "every frame is symmetric" passes perfectly on
        # a shader that ignores the horizontal angle entirely.
        live_ok = asym["asymmetric"] >= IES_ASYM_MIN
        ok = folded_ok and live_ok
        print(f"  ies-mirror   {'PASS' if ok else 'FAIL'}  partial sweeps mirror: bilateral "
              f"{asym['bilateral']:.4f}, quadrant {asym['quadrant']:.4f} "
              f"(want <= {IES_MIRROR_MAX}; a modulo fold reads 0.99 and 0.83); "
              f"controls symmetric {asym['symmetric']:.4f}, asymmetric "
              f"{asym['asymmetric']:.4f} (want >= {IES_ASYM_MIN}, or the azimuth is inert)")
        if not ok:
            failures.append("ies-mirror")

    # A profile REPLACES the cone rather than multiplying it. Untestable on the fixture as
    # authored -- it is a POINT light, and spotConeFactor returns 1.0 for every non-spot,
    # so multiplying and replacing are byte-identical there. Two SPOTS differing in
    # nothing but their cone: the profile carries the cutoff, so the cone must reach
    # nothing and the two frames must be the same one. Multiply instead and the 8-degree
    # cone clips the lobe the 85-degree one leaves whole.
    #
    # Exact 0 px is safe to demand: the clusterer never culls by cone (cutOff is packed
    # and not otherwise read), and this light does not cast, so no shadow frustum is
    # fitted from it either.
    def _spot(cone):
        def mutate(d):
            d["lights"][0].update({"type": "spot", "direction": [0.0, -1.0, 0.0],
                                   "cone": cone, "profile": asset("ies_symmetric.ies")})
        return mutate

    cone_shots, cone_err = {}, None
    for tag, cone in (("narrow", [5.0, 8.0]), ("wide", [80.0, 85.0])):
        scene = os.path.join(workdir, f"ies_cone_{tag}.cscn")
        cscn_copy(fixture, scene, _spot(cone))
        out = os.path.join(workdir, f"ies_cone_{tag}.ppm")
        cone_err = render(scene, out, ["-W", "400", "-H", "300", "--no-auto-exposure",
                                       "-E", "0.02"])
        if cone_err:
            break
        cone_shots[tag] = out
    if cone_err:
        print(f"  ies-replace  ERROR render failed: {cone_err.strip()[-200:]}")
        failures.append("ies-replace")
    else:
        px = compare(cone_shots["narrow"], cone_shots["wide"])[0]
        ok = px == 0
        print(f"  ies-replace  {'PASS' if ok else 'FAIL'}  a 5/8 deg cone and an 80/85 deg "
              f"cone under one profile differ by {px} px (want exactly 0: the profile is "
              "the whole distribution, cutoff included, so the authored cone reaches "
              "nothing)")
        if not ok:
            failures.append("ies-replace")

    # --ies-profile, the only route to a profile that does not go through a scene file.
    # Two claims, and the second is the one that can rot: the flag must WIN over what the
    # scene authored, and it must reach a light the scene never gave a profile at all.
    #
    # Read as pixels against the authored frame rather than off the probe, because a flag
    # that loaded the file and applied it to nothing prints an identical probe -- the
    # library is scene state, and loading is not applying.
    flag_shots, flag_err = {}, None
    flag_runs = (
        # The fixture authors ies_symmetric; overriding with the asymmetric file must move
        # the frame, and must land on the SAME frame the authored asymmetric variant gives.
        ("override", fixture, ["--ies-profile", asset("ies_asymmetric.ies")]),
        # ...and a scene with two point lights and no profile anywhere in it.
        ("bare", asset("contact_local_fixture.cscn"), []),
        ("bare_ies", asset("contact_local_fixture.cscn"),
         ["--ies-profile", asset("ies_asymmetric.ies")]),
    )
    for tag, scene, extra in flag_runs:
        if not os.path.exists(scene):
            flag_err = f"missing {os.path.basename(scene)}"
            break
        out = os.path.join(workdir, f"ies_flag_{tag}.ppm")
        flag_err = render(scene, out, ["-W", "400", "-H", "300", "--no-auto-exposure",
                                       "-E", "0.02"] + extra)
        if flag_err:
            break
        flag_shots[tag] = out

    if flag_err:
        print(f"  ies-flag     ERROR render failed: {flag_err.strip()[-200:]}")
        failures.append("ies-flag")
    else:
        # The authored asymmetric variant was rendered above by ies-shape.
        same = compare(flag_shots["override"], shots["asymmetric"])[0]
        moved = compare(flag_shots["override"], shots["symmetric"])[0]
        reached = compare(flag_shots["bare"], flag_shots["bare_ies"])[0]
        ok = same == 0 and moved > 0 and reached > 0
        print(f"  ies-flag     {'PASS' if ok else 'FAIL'}  --ies-profile over an authored "
              f"one: {same} px against the same file authored (want exactly 0) and {moved} "
              f"px against the file it replaced (want > 0, or the flag did nothing); on a "
              f"scene that authors no profile at all: {reached} px (want > 0)")
        if not ok:
            failures.append("ies-flag")

    return failures
