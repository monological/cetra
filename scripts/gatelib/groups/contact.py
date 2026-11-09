"""The contact group: contact shadows for the lights with no shadow map (spec 11.56)."""

import json
import os

from gatelib.harness import asset, render
from gatelib.images import _read_ppm
from gatelib.scenes import _cscn_camera, _projector, cscn_copy


# Contact shadows from the lights that cannot have a shadow map (spec 11.56), on
# assets/contact_local_fixture.
#
# The only contact-shadow test before this was a 0 px golden of the debug view on a
# sun-only scene, which pins the march's arithmetic and says nothing about which lights
# it marches. These arms are about the lights: a point or spot holding no punctual layer
# -- which past the second point light in a scene is every one of them, since the pool
# holds 16 layers and a point spends 6.
#
# Read through --cs-debug rather than off the lit frame. The term is what is under test
# and the lit frame carries it multiplied by csStrength into a sum that also holds
# ambient, both practicals and the AO factor; dividing two lit frames would recover it
# only up to everything else that moved.
#
# These numbers mirror gen_contact_local_fixture.py and have to match it or the gate
# reads a scan line the fixture does not predict; the camera comes from the scene file.
CONTACT_FIXTURE = "contact_local_fixture.cscn"
CONTACT_BARE_MODEL = "contact_local_bare.gltf"
CONTACT_LOCAL = _cscn_camera(
    CONTACT_FIXTURE,
    strip_z=0.55,       # the receiver line, in front of the cube's z = +0.40 face
    strip_half_x=0.30,  # sampled across this, all of it with the cube behind it
    open_x=1.5,         # the in-frame control, on the SAME line
    open_half_x=0.20,
    samples=41,
)
# Pinned rather than left to the app's scene-radius heuristic, for the reason the cloud
# arms pin their coverage: an arm wants the configuration where the property is legible
# and must not be silently re-tuned by a change of default.
CONTACT_CS_DISTANCE = "0.8"
# 0.364 measured. The bar is a floor on the DARKENING, not a ceiling on the term, so a
# change that weakens the march fails here instead of drifting toward invisible.
CONTACT_DARKEN_MIN = 0.20
# Where nothing occludes, the term is exactly 1 -- an identity, not an approximation, so
# the bar is one 8-bit code off it rather than a tolerance.
CONTACT_LIT_MIN = 1.0 - 1.5 / 255.0
# What backing the light's own occlusion out of three different intensity ratios is
# allowed to disagree by, relative. Measured 0.8%.
CONTACT_FOLD_SPREAD_MAX = 0.03
# ...and how far apart the three terms must be before that agreement means anything.
# Three identical frames agree perfectly. Measured 0.44.
CONTACT_FOLD_SEPARATION_MIN = 0.25
# The map-less reading a mapped light must return to under --no-shadows. Two renders of
# one build are byte-identical here, so this is quantization on the sample line only.
CONTACT_SKIP_TOL = 0.004

# A shadow-casting directional for the MIXED path -- a key light and local lights folded
# together, which the fixture cannot author because it exists to have no directional at all.
# Travel direction is up-and-toward-the-camera reversed, so the strip's ray to it leaves
# over the open ground and the sun contributes weight WITHOUT occlusion of its own: the arm
# then reads dilution alone. Intensity is chosen so its weight lands near the two
# practicals' combined weight, which keeps both steps visible in 8 bits.
CONTACT_SUN = {"name": "key_sun", "type": "directional",
               "direction": [0.0, -0.70710678, -0.70710678],
               "color": [1.0, 1.0, 1.0], "intensity": 1.0, "cast_shadows": True}
# The mapped lamp is a copy of practical_front -- same place, so it is unoccluded too and
# can only enter the denominator -- at this multiple of its intensity.
CONTACT_MAPPED_GAIN = 3.0
# Measured 0.1866 and 0.0765. Each step is a light joining the DENOMINATOR and nothing
# else, so a build that drops either reads exactly 0.0000 rather than a smaller number --
# these floors are generous because the failure is total, not gradual.
CONTACT_KEY_STEP_MIN = 0.08
CONTACT_MAPPED_STEP_MIN = 0.03
# The frame --shadows-off-at fires on. Anything after the first depth pass reaches the
# transition; 5 leaves 25 frames rendering in the stale state before the capture.
CONTACT_SHADOWS_OFF_FRAME = "5"


def _contact_light(d, name):
    return next(light for light in d["lights"] if light["name"] == name)


def _contact_term(pix, w, h, project, x0, x1):
    """Mean contact-shadow visibility along the ground line z = strip_z, x in [x0, x1].

    RAW bytes, NOT _linear_luma. --cs-debug returns early from the tonemap and writes the
    term straight out with no display encode on it, so decoding sRGB here would report a
    measured 0.6360 as 0.3599 and every predicted ratio would miss.

    A LINE and not a box, because the fixture's two lights are mirrored about this z and
    only about this z. Spreading the sample in depth breaks the symmetry the fold
    prediction is derived from -- 2.8% of it at only 0.02 units either side.
    """
    z = CONTACT_LOCAL["strip_z"]
    n = CONTACT_LOCAL["samples"]
    total = 0.0
    for i in range(n):
        px, py = project((x0 + (x1 - x0) * (i + 0.5) / n, 0.0, z))
        x = max(0, min(w - 1, int(round(px))))
        y = max(0, min(h - 1, int(round(py))))
        total += pix[(y * w + x) * 3] / 255.0
    return total / n


def _contact_read(workdir, tag, mutate, extra=None):
    """Render one variant of the fixture and return (strip, control), or (None, error).

    Both reads come out of ONE frame and one row of pixels, so a projection off by a
    texel moves them together and the control stays a control.
    """
    scn = os.path.join(workdir, f"contact_{tag}.cscn")
    cscn_copy(asset(CONTACT_FIXTURE), scn, mutate)
    out = os.path.join(workdir, f"contact_{tag}.ppm")
    err = render(scn, out, ["-W", "640", "-H", "400", "--no-auto-exposure", "-E", "1.0",
                            "--cs-debug", "--cs-distance", CONTACT_CS_DISTANCE] + (extra or []))
    if err:
        return None, err
    w, h, pix = _read_ppm(out)
    project = _projector(CONTACT_LOCAL, w, h)
    p = CONTACT_LOCAL
    return ((_contact_term(pix, w, h, project, -p["strip_half_x"], p["strip_half_x"]),
             _contact_term(pix, w, h, project, p["open_x"] - p["open_half_x"],
                           p["open_x"] + p["open_half_x"])), None)


def run_contact_gate(workdir):
    """Contact shadows for local lights, which is the population with no other occlusion.

      contact-local   a map-less practical darkens a contact, open ground in the same
                      frame reads exactly 1, and so does the same scene with the occluder
                      deleted
      contact-mapped  a light that HAS a punctual map contributes nothing to the term --
                      and the same scene under --no-shadows darkens again, so the skip is
                      reading the map rather than the authored flag
      contact-fold    the fold is weighted by contribution: scaling the unoccluded light
                      16x recovers the same occluder to under a percent, which an
                      unweighted fold cannot do
      contact-mixed   a key directional and local lights fold TOGETHER, and each light
                      that reaches the pixel divides the term -- including a mapped one,
                      which is never marched but still lights the pixel
      contact-stale   a mapped light whose shadow system is switched off MID-RUN is
                      marched again, because shadow_layer is only maintained while the
                      depth pass runs and --no-shadows cannot reach that transition

    The occluder-removed half of the first arm is a committed twin rather than a mutation,
    because a .cscn cannot delete a node: gen_contact_local_fixture.py emits the bare
    ground from the same vertex arrays, so the two files differ in the drawn geometry and
    in nothing else.

    NO ARM HERE IS SAFE ALONE, and contact-mapped is the one to watch: "the light with a
    map contributed nothing" is satisfied perfectly by a build that marches no local light
    at all. Falsified by hand at 11.56 -- stubbing the cluster list empty fails
    contact-local and contact-fold and leaves contact-mapped green; removing the map skip
    fails contact-mapped alone; making the fold unweighted fails contact-fold alone;
    dropping skipped lights from the denominator collapses contact-mixed's second step to
    exactly 0.0000; packing the raw shadow_layer takes contact-stale from the map-less
    reading to 1.0000.
    """
    fixture = asset(CONTACT_FIXTURE)
    if not os.path.exists(fixture):
        print(f"  contact-local SKIP  ({CONTACT_FIXTURE} not present)")
        return []
    failures = []

    # Every arm reads the unmutated fixture, so losing it fails all three -- named
    # one per line rather than once, or two of them appear in the summary with
    # nothing above explaining why.
    base, err = _contact_read(workdir, "base", lambda d: None)
    if err:
        for arm in ("contact-local", "contact-mapped", "contact-fold"):
            print(f"  {arm} ERROR the base render failed: {err.strip()[-200:]}")
        return ["contact-local", "contact-mapped", "contact-fold"]
    bare, err = _contact_read(
        workdir, "bare",
        # Absolute: a relative model resolves against the SOURCE scene's own
        # directory, which is assets/scenes/ since 12.8, and the models are a
        # sibling directory away.
        lambda d: d["models"].__setitem__(0, {"path": asset(CONTACT_BARE_MODEL)}))
    if err:
        print(f"  contact-local ERROR bare-twin render failed: {err.strip()[-200:]}")
        failures.append("contact-local")
    else:
        darken = 1.0 - base[0]
        ok = (darken >= CONTACT_DARKEN_MIN and base[1] >= CONTACT_LIT_MIN
              and bare[0] >= CONTACT_LIT_MIN)
        print(f"  contact-local {'PASS' if ok else 'FAIL'}  strip {base[0]:.4f} "
              f"(darkening {darken:.4f}, want >= {CONTACT_DARKEN_MIN}); open ground "
              f"{base[1]:.4f} and occluder removed {bare[0]:.4f} "
              f"(both want >= {CONTACT_LIT_MIN:.4f} -- the falsifiers, one in frame and "
              f"one without the cube)")
        if not ok:
            failures.append("contact-local")

    def _map_back(d):
        _contact_light(d, "practical_back")["cast_shadows"] = True

    mapped, err = _contact_read(workdir, "mapped", _map_back)
    unmapped, err2 = _contact_read(workdir, "mapped_nomaps", _map_back, ["--no-shadows"])
    if err or err2:
        print(f"  contact-mapped ERROR render failed: {(err or err2).strip()[-200:]}")
        failures.append("contact-mapped")
    else:
        drift = abs(unmapped[0] - base[0])
        ok = mapped[0] >= CONTACT_LIT_MIN and drift <= CONTACT_SKIP_TOL
        print(f"  contact-mapped {'PASS' if ok else 'FAIL'}  the mapped light's strip "
              f"{mapped[0]:.4f} (want >= {CONTACT_LIT_MIN:.4f}: its map already resolves "
              f"the contact); under --no-shadows the same scene reads {unmapped[0]:.4f} "
              f"vs {base[0]:.4f} map-less, drift {drift:.4f} (want <= {CONTACT_SKIP_TOL})")
        if not ok:
            failures.append("contact-mapped")

    with open(fixture) as fh:
        authored = json.load(fh)
    i_back = _contact_light(authored, "practical_back")["intensity"]
    i_front = _contact_light(authored, "practical_front")["intensity"]

    def _scale_front(factor):
        def mutate(d):
            _contact_light(d, "practical_front")["intensity"] = i_front * factor
        return mutate

    # The occluded light's share of the two weights. Exact rather than fitted: the pair is
    # equidistant from every point of this line and shares an N.L there, so their weight
    # ratio IS their intensity ratio and nothing else about the geometry enters.
    reads, fold_err = {1.0: base}, None
    for factor in (4.0, 0.25):
        reads[factor], fold_err = _contact_read(workdir, f"front{factor:g}".replace(".", ""),
                                                _scale_front(factor))
        if fold_err:
            break
    if fold_err:
        print(f"  contact-fold  ERROR render failed: {fold_err.strip()[-200:]}")
        failures.append("contact-fold")
    else:
        occ, terms = {}, {}
        for factor, read in reads.items():
            share = i_back / (i_back + i_front * factor)
            occ[factor] = (1.0 - read[0]) / share
            terms[factor] = read[0]
        spread = max(occ.values()) / max(min(occ.values()), 1e-6) - 1.0
        separation = max(terms.values()) - min(terms.values())
        ok = spread <= CONTACT_FOLD_SPREAD_MAX and separation >= CONTACT_FOLD_SEPARATION_MIN
        detail = " ".join(f"x{f:g}: term {terms[f]:.4f} -> occ {occ[f]:.4f}"
                          for f in sorted(reads))
        print(f"  contact-fold  {'PASS' if ok else 'FAIL'}  {detail}; spread "
              f"{spread:.4f} (want <= {CONTACT_FOLD_SPREAD_MAX}), term separation "
              f"{separation:.4f} (want >= {CONTACT_FOLD_SEPARATION_MIN}, or three equal "
              f"frames would agree perfectly)")
        if not ok:
            failures.append("contact-fold")

    # THE MIXED PATH: a key directional folded together with local lights, which the
    # fixture cannot author itself. Two steps, each adding exactly one light that
    # contributes to the DENOMINATOR and nothing else -- the sun is unoccluded at the
    # strip by construction, and the mapped lamp is never marched at all. So each step
    # measures the denominator alone, and a build that omits either reads 0.0000 for it.
    def _add_sun(d):
        d["lights"].append(dict(CONTACT_SUN))

    def _add_sun_and_mapped(d):
        _add_sun(d)
        front = _contact_light(d, "practical_front")
        d["lights"].append(dict(front, name="mapped_lamp", cast_shadows=True,
                                intensity=front["intensity"] * CONTACT_MAPPED_GAIN))

    keyed, err = _contact_read(workdir, "keyed", _add_sun)
    keyed_mapped, err2 = _contact_read(workdir, "keyed_mapped", _add_sun_and_mapped)
    if err or err2:
        print(f"  contact-mixed ERROR render failed: {(err or err2).strip()[-200:]}")
        failures.append("contact-mixed")
    else:
        key_step = keyed[0] - base[0]
        mapped_step = keyed_mapped[0] - keyed[0]
        ok = (key_step >= CONTACT_KEY_STEP_MIN and mapped_step >= CONTACT_MAPPED_STEP_MIN
              and keyed_mapped[1] >= CONTACT_LIT_MIN)
        print(f"  contact-mixed {'PASS' if ok else 'FAIL'}  strip {base[0]:.4f} local-only -> "
              f"{keyed[0]:.4f} with a key directional (step {key_step:.4f}, want >= "
              f"{CONTACT_KEY_STEP_MIN}) -> {keyed_mapped[0]:.4f} with a MAPPED lamp beside it "
              f"(step {mapped_step:.4f}, want >= {CONTACT_MAPPED_STEP_MIN}: a light with its "
              f"own map still lights the pixel, so it still divides the term); control "
              f"{keyed_mapped[1]:.4f}")
        if not ok:
            failures.append("contact-mixed")

    # Light.shadow_layer is maintained only while the depth pass runs, so turning the
    # shadow system off AFTER it has run leaves a mapped light claiming a layer nothing
    # draws. --no-shadows cannot reach that state -- it clears the switch before frame 0,
    # so no layer was ever assigned -- which is why --shadows-off-at exists.
    stale, err = _contact_read(workdir, "stale", _map_back,
                               ["--shadows-off-at", CONTACT_SHADOWS_OFF_FRAME])
    if err or mapped is None:
        # mapped is the shadows-ON half of this comparison, so losing it above takes
        # this arm with it rather than leaving it to read one number against nothing.
        print(f"  contact-stale ERROR render failed: {(err or 'the mapped read').strip()[-200:]}")
        failures.append("contact-stale")
    else:
        ok = abs(stale[0] - base[0]) <= CONTACT_SKIP_TOL and mapped[0] >= CONTACT_LIT_MIN
        print(f"  contact-stale {'PASS' if ok else 'FAIL'}  the same mapped light after the "
              f"shadow system is switched off mid-run: strip {stale[0]:.4f} vs {base[0]:.4f} "
              f"map-less (want within {CONTACT_SKIP_TOL}); with the system left ON it reads "
              f"{mapped[0]:.4f} (want >= {CONTACT_LIT_MIN:.4f}, or the two states are the "
              f"same and this measures nothing)")
        if not ok:
            failures.append("contact-stale")

    return failures
