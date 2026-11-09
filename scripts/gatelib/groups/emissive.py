"""The emissive group: emissive geometry as area lights (fit, intent, light; spec 11.49)."""

import json
import os

from gatelib.harness import NO_HALOS, asset, render
from gatelib.images import _absorb_box_rgb, _read_ppm, compare
from gatelib.probes import _probe_render
from gatelib.scenes import CORNELL_FIXTURE, _cscn_camera, _projector, cscn_copy


# Every gate group in run order, as (selector, banner, fn).
#
# A table rather than a sequence of calls so --only can filter it. The suite is
# minutes long, and a change that touches one subsystem should not have to pay for
# all of them -- a check nobody waits for is a check nobody runs. The full run stays
# the default, so CI and the specs' verification blocks are unaffected.
# ---------------------------------------------------------------------------
# Emissive geometry as LTC area lights (spec 11.49 / roadmap C2)
# ---------------------------------------------------------------------------

EMISSIVE_FIXTURE = "emissive_fixture.gltf"
EMISSIVE_WATER_FIXTURE = "water_fixture.gltf"

# The cornell quad, read off gen_cornell_rooms.py rather than off a render: the
# panel sits at TOP - 0.02 with TOP = 2.0 and spans +/-0.35 in x and z. Stated
# here so the arm compares the fit against the GEOMETRY, not against itself.
#
# Note 1.98 and not the 1.97 the fixture's own area light is authored at. That
# 1 cm is a real discrepancy in the fixture -- its comment says the light mirrors
# the quad -- and the fit is a fit of the MESH, so this is the number it owes.
EMISSIVE_CORNELL_CENTER = (0.0, 1.98, 0.0)
EMISSIVE_CORNELL_NORMAL = (0.0, -1.0, 0.0)
EMISSIVE_CORNELL_SIZE = (0.7, 0.7)
EMISSIVE_GEOM_EPS = 1e-3

# Half black, half white, so the linear mean is 0.5 by construction. The
# tolerance is the 8-bit sRGB round trip through the top mip, not slack: 0.5
# encodes to byte 188, which decodes to 0.5029.
EMISSIVE_TEX_MEAN = 0.5
EMISSIVE_TEX_EPS = 0.005

# The strip is 2.0 x 0.25 rotated 30 degrees in its own plane. An axis-aligned
# bound on the reference frame reads 1.8571 x 1.2165, so this separates the
# minimum-area search from the obvious thing to have written.
EMISSIVE_STRIP_SIZE = (0.25, 2.0)

# The two fill rejects, read off gen_emissive_fixture.py's own constants rather
# than off a probe run, so the arm compares the fit against the GEOMETRY.
#
# split  two 1.0 x 0.25 strips whose inner edges are 5.0 apart: 0.5 of area
#        against a 7.0 x 0.25 bound = 0.285714
# ell    two 1.0 x 0.3 arms sharing a corner: 0.3 + 0.21 against a 1.0 x 1.0
#        bound = 0.51
#
# Both sit far under EMISSIVE_FIT_MIN_FILL 0.65, which is where a disc (0.785)
# stays accepted -- so the epsilon is against the minimum-area search's angular
# quantisation, not against the distance to the threshold.
EMISSIVE_SPLIT_FILL = 0.285714
EMISSIVE_ELL_FILL = 0.51
EMISSIVE_FILL_EPS = 2e-3

# Derived against hand-authored at equal radiance. Not tighter, because the
# fixture's own 1 cm offset puts the derived panel very slightly further from
# everything -- which is the residual this bar is sized to admit and name.
EMISSIVE_MATCH_EPS = 0.03

# The GI arm compares two LIFTS, so it is looser than the direct match above: it
# divides two ratios, each carrying the probe volume's own quantisation, and the
# derived side is very slightly darker because the authored fixture's quad still
# emits its token 1 nit into the capture where the derived one is silenced whole.
# It failed at 1.3134 before the capture learned to silence a derived emitter, so
# 5% discriminates by a wide margin.
EMISSIVE_GI_EPS = 0.05
EMISSIVE_GI_FRAMES = 120

# Bare literals in the arms below became these, because five thresholds in this
# group already carried a stated reason and three did not.
#
# PLANARITY_MAX  a closed volume reads 0 exactly (its faces cancel); the loosest
#                shape that should still reject is far under this.
# SHADOW_LIFT_MIN  the unshadowed floor measured 1.37x the shadowed one on this
#                fixture, so this is well inside the effect and well outside the
#                noise -- the arm is asserting a shadow exists, not sizing it.
# PRESENCE_MIN   two black frames agree on every ratio, so the acceptance arms
#                demand the floor is actually lit. Linear, against a floor that
#                reads ~0.12 lit and ~0.001 under a clear colour.
# GI_LIFT_MIN    the indirect term must actually lift, or a ratio of two absent
#                lifts is 1.0 and says nothing.
# OFF_MOVED_MIN  the flag must actually change the frame, or "off equals opted
#                out" passes on two identical blank renders.
EMISSIVE_PLANARITY_MAX = 0.5
EMISSIVE_SHADOW_LIFT_MIN = 1.10
EMISSIVE_PRESENCE_MIN = 0.02
EMISSIVE_GI_LIFT_MIN = 1.05
EMISSIVE_OFF_MOVED_MIN = 0.10

# Read bands, as WORLD patches on named surfaces of the cornell room, projected
# through the fixture's own camera.
#
# They were hand-written screen boxes and every one of them measured something
# other than its name. Ray-cast against the room and tallied over the same 12x12
# grid _absorb_box_rgb samples: the "floor" box hit 59 short-box, 34 tall-box and
# 33 floor samples -- 65% boxes -- and the two wall boxes were each EXACTLY half
# background, because they straddled the room's silhouette. A wall ratio blended
# 50/50 with the G-buffer clear is not a wall ratio, and the number this group
# printed as `left=0.9969` never was one.
#
# Derived here for the reason _cscn_camera exists (its own docstring: "the
# failure is silent -- the gate keeps passing while measuring a different scene
# than it predicts"), and stated as the surface each band is ON so the name and
# the measurement cannot come apart again.
#
# Placement respects the room's own furniture: the tall box spans x -0.79..0.06
# and the short one x -0.05..0.81 (gen_cornell_rooms.py box(cx, cz, w, h, d, yaw),
# half-diagonal 0.42 under yaw), so the floor patch sits left of both and forward
# of the tall one; the wall patches sit ABOVE the tall box's 1.2 height, where
# nothing occludes them.
# `shadow` is floor the TALL BOX hides from the ceiling panel, which the lit
# bands above deliberately avoid -- so the first version of emissive-override,
# reading `floor`, compared two frames that were identical there and reported
# 1.0000x. Checked rather than eyeballed: a floor point (x, z) sees the panel
# centre through height 1.2 at (0.394x, 1.2, 0.394z), and for this patch that
# lands inside the box's yawed 0.6 top face at every corner.
EMISSIVE_WORLD_BANDS = {
    "floor": [(-0.80, 0.0, 0.30), (-0.20, 0.0, 0.30), (-0.20, 0.0, 0.85), (-0.80, 0.0, 0.85)],
    "shadow": [(-0.80, 0.0, -0.80), (-0.50, 0.0, -0.80), (-0.50, 0.0, -0.50), (-0.80, 0.0, -0.50)],
    "left": [(-1.0, 1.40, -0.45), (-1.0, 1.40, 0.45), (-1.0, 1.80, 0.45), (-1.0, 1.80, -0.45)],
    "right": [(1.0, 1.40, -0.45), (1.0, 1.40, 0.45), (1.0, 1.80, 0.45), (1.0, 1.80, -0.45)],
}

# Fraction of each projected band trimmed off every side. The patches above are
# already interior, so this is against the projection's own rounding and the
# 12x12 sample grid landing on a boundary texel, not a margin for error in them.
EMISSIVE_BAND_INSET = 0.12


EMISSIVE_LEAK_FIXTURE = "cornell_leak.cscn"

# cornell_leak is the same room cut in half by a partition at x = 0, lit over the
# LEFT half only (gen_cornell_rooms.py `r.panel(-0.5)`). It exists to prove light
# does NOT arrive where it has no path.
#
# Mirrored patches on the BACK WALL (z = -1), one each side of the partition.
# The back wall and not the floor because the .cscn camera is nearly horizontal
# (eye y 1.0, target y 0.95), so the floor projects into a grazing sliver while
# the back wall faces the lens. Mirrored rather than absolute because the two
# configurations differ in how bright the LIT half is -- a 160 lumen spot is not
# an 18 nit panel -- so the reading that means anything is dark AGAINST lit
# inside one frame.
#
# Deliberately away from the open front (z = +1), which is the one real path
# light has into the occluded half and would put a legitimate reading in a band
# whose whole claim is that it receives none.
EMISSIVE_LEAK_BANDS = {
    "lit": [(-0.85, 0.55, -1.0), (-0.20, 0.55, -1.0), (-0.20, 1.35, -1.0), (-0.85, 1.35, -1.0)],
    "dark": [(0.20, 0.55, -1.0), (0.85, 0.55, -1.0), (0.85, 1.35, -1.0), (0.20, 1.35, -1.0)],
}

# The occluded half must read as a fraction of the lit half. Measured 0.0000
# exactly with the shadow on -- the room is direct-lit only, so a fully occluded
# surface is black rather than nearly so -- and 0.4115 with it off. The bar sits
# far from both, since what separates them is three orders of magnitude and not a
# margin.
#
# LEAK_MIN is the falsifier and is the more important of the two. Without it this
# arm passes on any frame where the dark half happens to be black, INCLUDING a
# build where the derived panel never lit anything at all: "no light through the
# wall" and "no light" are the same reading. The unshadowed render is what proves
# the band can detect a leak, so the shadowed zero means something.
EMISSIVE_LEAK_MAX = 0.02
EMISSIVE_LEAK_MIN = 0.15

# The specular double count (P11). A reflection probe photographs the glowing
# quad and the LTC panel integrates the same rectangle analytically, so a smooth
# surface receives it twice.
#
# Read as the derived side's PROBE LIFT over the authored side's, which controls
# for everything in the capture that is not the emitter -- the sky above all.
# The authored fixture's quad emits a token 1 nit beside its 18 nit analytic
# light, the derived one carries the full 18 in the quad itself, so the
# difference between the two lifts IS the emitter's own share of the probe.
#
# Measured worst 1.0386, against 1.3134 for the diffuse double count before P6.
# An order of magnitude smaller, and the reason is the split sum: a DDGI probe
# integrates irradiance over the whole hemisphere, where a bright ceiling panel
# dominates, while the reflection probe feeds specular through a lobe where the
# panel covers little solid angle and a dielectric reflects a few percent of it.
#
# --sun-elevation -10 is pinned and load-bearing, exactly as --cloud-coverage
# 0.10 is on the cloud-shadow arms: an arm wants the configuration where the
# property is LEGIBLE. With the default sun the sky dominates the capture and
# the same measurement reads 1.0150, so a default change could halve the arm's
# sensitivity silently. The sun is below the horizon rather than the sky absent
# because the probe requires a precomputed IBL to be created at all.
#
# Sky rather than an HDR for three reasons: my_models/ is out of tree so an HDR
# arm would SKIP in a clean checkout, the sky path is 0 px run-to-run, and the
# elevation is a dial that turns the confound DOWN. A fixed HDR has none of the
# three.
EMISSIVE_SPEC_MAX = 1.10
# The probe must actually lift, or a ratio of two absent lifts is 1.0 and the
# arm passes on a build where the capture never ran. Max lift measured 2.6464.
EMISSIVE_SPEC_LIFT_MIN = 1.5
EMISSIVE_SPEC_SKY = ["--sky", "--sun-elevation", "-10"]
# Polished, but a DIELECTRIC. Metal has no diffuse, so every lift ratio's
# denominator collapses toward black and the ratio destabilises -- measured, the
# floor and back wall go nearly black and the read stops meaning anything.
EMISSIVE_SPEC_ROUGHNESS = 0.08


def _emissive_boxes(w, h):
    """The world bands above as fractional screen boxes, for _absorb_box_rgb."""
    project = _projector(_cscn_camera(CORNELL_FIXTURE), w, h)
    boxes = {}
    for name, pts in EMISSIVE_WORLD_BANDS.items():
        xs, ys = zip(*(project(p) for p in pts))
        x0, x1 = min(xs) / w, max(xs) / w
        y0, y1 = min(ys) / h, max(ys) / h
        dx, dy = (x1 - x0) * EMISSIVE_BAND_INSET, (y1 - y0) * EMISSIVE_BAND_INSET
        boxes[name] = (x0 + dx, y0 + dy, x1 - dx, y1 - dy)
    return boxes


def _emissive_lumas(path):
    """Mean linear luma per band. One place that knows how a band is measured."""
    w, h, pix = _read_ppm(path)
    return {k: sum(_absorb_box_rgb(pix, w, h, b)) / 3.0
            for k, b in _emissive_boxes(w, h).items()}


def _emissive_worst(base, other):
    """Per-band ratios and the band furthest from 1.0.

    `max` over a key rather than a running best against a 0.0 seed. The seeded
    form was written three times here and one copy carried `or worst == 0.0`,
    which made 0.0 both "not set yet" and a legal reading -- so a band whose
    ratio collapsed to exactly zero, the strongest failure available, was
    overwritten by the next band and the arm passed. There is no sentinel here
    to get that wrong.
    """
    ratios = {k: other[k] / max(base[k], 1e-6) for k in base}
    name = max(ratios, key=lambda k: abs(ratios[k] - 1.0))
    return ratios, name, ratios[name]


def _emissive_probe(scene, extra=None, frames=8):
    """Run --emissive-light-probe and parse its lines into dicts.

    Reads the INSTRUMENT rather than the image on purpose: a panel half a metre
    out or sqrt(2) too wide still lights a room, so a frame cannot tell a correct
    fit from a plausible one. Every geometric arm here goes through this.
    """
    return _probe_render(scene, "--emissive-light-probe", "emissive-light-probe",
                         extra=extra, frames=frames)


def _emissive_vec(rec, key):
    try:
        return tuple(float(x) for x in rec[key].split(","))
    except (KeyError, ValueError):
        return None


def _emissive_named(rows, kind, node):
    for r in rows:
        if r.get("kind") == kind and r.get("node") == node:
            return r
    return None


def run_emissive_gate(workdir):
    """Emissive geometry derives an LTC area light (spec 11.49).

    The arms split into three groups, and the split is the point.

    GEOMETRY, read off the probe against numbers taken from the generators:

      emissive-fit      the cornell quad's centre, normal and size. Exact, not
                        approximate -- the mesh is a flat axis-aligned square and
                        the fit owes it to the digit.
      emissive-strip    a 2.0 x 0.25 quad rotated 30 degrees in its own plane.
                        Fails the covariance principal axis this was first written
                        as, and fails an axis-aligned bound, both of which look
                        right on every other shape in the corpus.
      emissive-reject   a closed cube rejects, AND for the stated reason. The
                        planarity number alone does not discriminate: the probe
                        zeroes the fit for any non-geometric reject, so every
                        other reason prints 0.000000 and satisfies a bare
                        threshold -- an opted-out material used to pass this.
      emissive-fill     two coplanar strips with a gap, and an L. Both read
                        planarity exactly 1.0, so the flatness test above says
                        nothing about either, and both would otherwise get a panel
                        radiating from space with no geometry in it. Two shapes
                        rather than one because they fail for different reasons --
                        disjoint against concave -- and a threshold catching only
                        one of them would look correct.
      emissive-texmean  a half-black half-white emissive texture reads 0.5. The
                        only arm that sees the top-mip readback at all.
      emissive-placed   a panel on a TRANSLATED node lands in the same place at 4
                        frames and at 40. Every fixture node is identity-
                        transformed, which is the one arrangement where the local
                        fit and the world placement cannot be told apart -- and
                        this module shipped re-applying the node transform to its
                        own output every frame, which no other arm can see.

    INTENT, because the feature has to be able to decline:

      emissive-unlit    water_fixture's bed and ramp are emissive over a BLACK
                        base -- the unlit-flat-colour idiom -- and the probe
                        reports both. This arm exists because that measurement is
                        why the feature is off by default; without it a later
                        change could quietly start treating them as lamps and
                        nothing would object.
      emissive-optout   emissiveLight: "off" takes the cornell panel to zero
                        derived lights, and the probe still REPORTS it declining.
                        Authored as the LABEL, so it covers the scene-file change
                        too. The count alone is what a scene that failed to load
                        prints, which is why the reject row is asserted with it.

    LIGHT, which is what any of it was for:

      emissive-match    a derived panel and a hand-authored one, at equal
                        radiance, light the room the same. THE acceptance test.
                        Read on floor and walls, never the ceiling panel, whose
                        own pixels differ 18x between the two by construction.
      emissive-override light_overrides can NAME a derived panel -- a light the
                        scene file could not have known about -- which is the
                        whole ordering claim. Asserted on the renderer's own
                        report rather than on a shadow, which the penumbra group
                        owns.
      emissive-occluded the occlusion the arm above cannot see, on the fixture
                        built to ask it. cornell_leak is one room cut in half by
                        a partition and lit over one side only. A derived panel
                        inherits cast_shadows FALSE, so by default it lights the
                        far half straight through the wall -- 0.41 of the lit
                        half -- and a light_overrides entry takes that to 0.
                        BOTH are asserted: without the leaking frame, "no light
                        through the wall" and "no light at all" are one reading.
      emissive-gi       a derived panel and a hand-authored one lift the indirect
                        term by the same amount. A DDGI probe capture runs the
                        full forward shader, so it sees the emissive surface AND
                        the surfaces the panel already lit -- the first bounce
                        lands twice, and it read 1.31x on the floor before the
                        capture learned to silence a derived emitter. Reads the
                        RATIO of lifts, so it does not move when the fixture's
                        exposure or geometry does.
      emissive-spec     the SPECULAR double count, BOUNDED rather than removed. A
                        reflection probe photographs the glowing quad and the LTC
                        panel integrates the same rectangle analytically, so a
                        smooth surface gets it twice. Read as the derived side's
                        probe lift over the authored side's, which cancels
                        everything in the capture that is not the emitter.
                        Measured 1.0386 against the arm above's 1.3134, and the
                        fix was declined at that size: unlike the GI case the
                        emitter must STAY in a radiance capture, or a mirror
                        reflects a uniform rectangle instead of the surface. So
                        this one does not assert 1.0 -- it asserts the effect
                        stays small enough for that judgement to hold.
      emissive-off      two ways of having no derived panel are BYTE-IDENTICAL,
                        and both differ from having one. NOT "the room goes
                        dark", which is what this was first written as: strip the
                        authored light and turn the feature off and the scene has
                        no lights at all, so the render app substitutes its
                        three-point fallback and the room gets BRIGHTER, 16x.

    Every band read here is derived from the fixture's own camera and stated as a
    WORLD patch (EMISSIVE_WORLD_BANDS). The hand-written screen boxes this group
    shipped with measured a "floor" that was 65% the two boxes and walls that were
    half empty background.
    """
    failures = []
    fixture = asset(EMISSIVE_FIXTURE)
    cornell = asset(CORNELL_FIXTURE)

    # --- geometry -----------------------------------------------------------
    if not os.path.exists(cornell):
        print(f"  emissive-fit   SKIP  ({CORNELL_FIXTURE} not present)")
    else:
        rows, _ = _emissive_probe(cornell)
        rec = _emissive_named(rows, "panel", "cornell_light")
        if not rec:
            print("  emissive-fit   FAIL  no panel derived from cornell_light")
            failures.append("emissive-fit")
        else:
            c = _emissive_vec(rec, "center")
            n = _emissive_vec(rec, "normal")
            s = _emissive_vec(rec, "size")
            dc = max(abs(c[i] - EMISSIVE_CORNELL_CENTER[i]) for i in range(3))
            dn = max(abs(n[i] - EMISSIVE_CORNELL_NORMAL[i]) for i in range(3))
            ds = max(abs(sorted(s)[i] - sorted(EMISSIVE_CORNELL_SIZE)[i]) for i in range(2))
            plan = float(rec.get("planarity", 0.0))
            ok = (dc <= EMISSIVE_GEOM_EPS and dn <= EMISSIVE_GEOM_EPS
                  and ds <= EMISSIVE_GEOM_EPS and abs(plan - 1.0) <= EMISSIVE_GEOM_EPS)
            print(f"  emissive-fit   {'PASS' if ok else 'FAIL'}  center={c} normal={n} "
                  f"size={s} planarity={plan:.6f} worst_delta={max(dc, dn, ds):.6f} "
                  f"want <={EMISSIVE_GEOM_EPS}")
            if not ok:
                failures.append("emissive-fit")

    if not os.path.exists(fixture):
        print(f"  emissive-strip SKIP  ({EMISSIVE_FIXTURE} not present)")
    else:
        rows, _ = _emissive_probe(fixture)

        rec = _emissive_named(rows, "panel", "emissive_strip")
        if not rec:
            print("  emissive-strip FAIL  no panel derived from emissive_strip")
            failures.append("emissive-strip")
        else:
            s = sorted(_emissive_vec(rec, "size"))
            want = sorted(EMISSIVE_STRIP_SIZE)
            ds = max(abs(s[i] - want[i]) for i in range(2))
            ok = ds <= EMISSIVE_GEOM_EPS
            print(f"  emissive-strip {'PASS' if ok else 'FAIL'}  size={s[0]:.6f}x{s[1]:.6f} "
                  f"want {want[0]}x{want[1]} delta={ds:.6f}")
            if not ok:
                failures.append("emissive-strip")

        # The REASON is asserted, not just printed. planarity alone cannot carry
        # this: _probe_node zeroes the fit and only runs it for a geometric
        # reject, so EVERY other reason prints planarity=0.000000 and satisfies a
        # bare threshold. An `opted-out` line passed this arm -- so had the
        # material key defaulted wrong, or the emissiveLight row been mis-wired
        # so every material read as opted out, this would have reported PASS on a
        # cube it never tested.
        rec = _emissive_named(rows, "reject", "emissive_box")
        got = rec.get("reason", "-") if rec else "no reject line"
        plan = float(rec["planarity"]) if rec and "planarity" in rec else float("nan")
        ok = rec is not None and got == "not-planar" and plan <= EMISSIVE_PLANARITY_MAX
        print(f"  emissive-reject {'PASS' if ok else 'FAIL'} reason={got} want not-planar; "
              f"planarity={plan:.6f} want <={EMISSIVE_PLANARITY_MAX}")
        if not ok:
            failures.append("emissive-reject")

        # FILL: flat is not the same as rectangular, and planarity only tests
        # flat. Both of these read planarity 1.0 and both would otherwise get a
        # panel spanning geometry that is not there -- the split one radiating
        # from the empty gap between its two strips, which is a lamp where there
        # is no lamp. Two shapes rather than one because they fail for different
        # reasons, disjoint against concave, and a threshold catching only one of
        # them would look correct.
        for node_name, want_fill in (("emissive_split", EMISSIVE_SPLIT_FILL),
                                     ("emissive_ell", EMISSIVE_ELL_FILL)):
            rec = _emissive_named(rows, "reject", node_name)
            got = rec.get("reason", "-") if rec else "no reject line"
            plan = float(rec["planarity"]) if rec and "planarity" in rec else float("nan")
            fill = float(rec["fill"]) if rec and "fill" in rec else float("nan")
            # Planarity asserted at 1.0 alongside, because that is the whole
            # point: this shape is rejected by fill and NOT by flatness, and an
            # arm that did not say so would pass if the two tests were confused.
            ok = (rec is not None and got == "not-filled"
                  and abs(fill - want_fill) <= EMISSIVE_FILL_EPS
                  and abs(plan - 1.0) <= EMISSIVE_GEOM_EPS)
            print(f"  emissive-fill  {'PASS' if ok else 'FAIL'} {node_name} reason={got} "
                  f"want not-filled; fill={fill:.6f} want {want_fill} "
                  f"+/-{EMISSIVE_FILL_EPS}; planarity={plan:.6f} want 1.0 "
                  "(flat, and still not a rectangle)")
            if not ok and "emissive-fill" not in failures:
                failures.append("emissive-fill")

        rec = _emissive_named(rows, "panel", "emissive_quad")
        if not rec or rec.get("radiance") != "final":
            state = rec.get("radiance", "absent") if rec else "absent"
            print(f"  emissive-texmean FAIL  radiance={state}, want final "
                  "(the texture's mip never arrived)")
            failures.append("emissive-texmean")
        else:
            nits = float(rec["nits"])
            d = abs(nits - EMISSIVE_TEX_MEAN)
            ok = d <= EMISSIVE_TEX_EPS
            print(f"  emissive-texmean {'PASS' if ok else 'FAIL'} nits={nits:.6f} "
                  f"want {EMISSIVE_TEX_MEAN} +/-{EMISSIVE_TEX_EPS} (delta={d:.6f})")
            if not ok:
                failures.append("emissive-texmean")

        # PLACEMENT, on a TRANSFORMED node, which nothing else in the corpus has.
        #
        # The fit is local; where the panel ends up is the other half, and every
        # emissive fixture node is identity-transformed -- the one arrangement
        # where world == local and the two halves cannot be told apart. This
        # module shipped writing world values back into the fields it read the
        # local fit from, so from frame two the node transform was re-applied to
        # its own output: a translated node walked its panel away, compounding,
        # and nine green arms could not see it.
        #
        # Rendering the same scene twice at 10x the frames and demanding the same
        # answer is what catches an accumulating error, where any single frame
        # looks plausible.
        moved = os.path.join(workdir, "emissive_moved.gltf")
        with open(fixture) as f:
            doc = json.load(f)
        for n in doc["nodes"]:
            if n["name"] == "emissive_quad":
                n["translation"] = [0.5, 0.0, 0.0]
        for b in doc.get("buffers", []):
            b.pop("_", None)
        with open(moved, "w") as f:
            json.dump(doc, f)

        short_rows, _ = _emissive_probe(moved, ["--emissive-lights"], frames=4)
        long_rows, _ = _emissive_probe(moved, ["--emissive-lights"], frames=40)
        s_rec = _emissive_named(short_rows, "placed", "emissive_quad")
        l_rec = _emissive_named(long_rows, "placed", "emissive_quad")
        if not s_rec or not l_rec:
            print("  emissive-placed FAIL  no placed row (the panel was never built)")
            failures.append("emissive-placed")
        else:
            sc, lc = _emissive_vec(s_rec, "center"), _emissive_vec(l_rec, "center")
            ss, ls = _emissive_vec(s_rec, "size"), _emissive_vec(l_rec, "size")
            drift = max(max(abs(sc[i] - lc[i]) for i in range(3)),
                        max(abs(ss[i] - ls[i]) for i in range(2)))
            ok = drift <= EMISSIVE_GEOM_EPS
            print(f"  emissive-placed {'PASS' if ok else 'FAIL'} 4 frames vs 40 on a "
                  f"translated node: centre {sc} -> {lc} size {ss} -> {ls} "
                  f"drift={drift:.6f} want <={EMISSIVE_GEOM_EPS}")
            if not ok:
                failures.append("emissive-placed")

    # --- intent -------------------------------------------------------------
    water = asset(EMISSIVE_WATER_FIXTURE)
    if not os.path.exists(water):
        print(f"  emissive-unlit SKIP  ({EMISSIVE_WATER_FIXTURE} not present)")
    else:
        rows, _ = _emissive_probe(water, ["--no-water"])
        panels = sorted(r["node"] for r in rows if r.get("kind") == "panel")
        ok = panels == ["water_bed", "water_ramp"]
        print(f"  emissive-unlit {'PASS' if ok else 'FAIL'}  panels={panels} "
              "want ['water_bed', 'water_ramp'] (emissive over a black base is not a lamp)")
        if not ok:
            failures.append("emissive-unlit")

    if os.path.exists(cornell):
        optout = os.path.join(workdir, "cornell_optout.cscn")
        cscn_copy(cornell, optout,
                  lambda d: d.setdefault("materials", {})
                             .setdefault("cornell_light", {})
                             .update({"emissiveLight": "off"}))
        rows, _ = _emissive_probe(optout, ["--emissive-lights"])
        header = next((r for r in rows if r.get("kind") == "header"), None)
        count = int(header["count"]) if header and "count" in header else -1
        # A count of 0 is also what a scene that failed to load prints, and what
        # any scene with no emissive mesh prints. The reject row is the evidence
        # that the mesh was SEEN and DECLINED, which is the actual claim -- and it
        # comes free out of rows already in hand.
        rec = _emissive_named(rows, "reject", "cornell_light")
        got = rec.get("reason", "-") if rec else "no reject line"
        ok = count == 0 and got == "opted-out"
        print(f"  emissive-optout {'PASS' if ok else 'FAIL'} derived={count} want 0, "
              f"cornell_light reason={got} want opted-out "
              "(authored as the label, not the number)")
        if not ok:
            failures.append("emissive-optout")

    # --- light --------------------------------------------------------------
    if not os.path.exists(cornell):
        print(f"  emissive-match SKIP  ({CORNELL_FIXTURE} not present)")
        return failures

    # --no-bloom is load-bearing, not hygiene. The derived twin carries
    # emissiveStrength 18, so its QUAD is 18x the authored one's -- handled for
    # direct pixels by never reading the ceiling, but bloom carries that
    # difference out of the quad and across the frame, asymmetrically, into every
    # band this group reads. Without it the residual these arms measure is part
    # panel placement and part glow, and the bar absorbs an effect nobody has
    # separated.
    base = ["--no-auto-exposure", "-E", "1.0", "--no-dither", *NO_HALOS]

    # The derived twin: the quad carries the authored light's radiance, the
    # authored light is gone, and a light_overrides entry gives the panel the
    # shadow the authored one had. Generated rather than committed, so the two
    # halves cannot differ in anything this does not touch.
    def _derive(d):
        d["lights"] = []
        d["light_overrides"] = [{"name": "cornell_light", "cast_shadows": True}]
        d.setdefault("materials", {}).setdefault("cornell_light", {})["emissiveStrength"] = 18.0

    derived = os.path.join(workdir, "cornell_derived.cscn")
    cscn_copy(cornell, derived, _derive)

    a = os.path.join(workdir, "emissive_authored.ppm")
    b = os.path.join(workdir, "emissive_derived.ppm")
    err = render(cornell, a, base)
    if err:
        print(f"  emissive-match ERROR authored render failed: {err.strip()[-200:]}")
        return failures + ["emissive-match"]
    err = render(derived, b, base + ["--emissive-lights"])
    if err:
        print(f"  emissive-match ERROR derived render failed: {err.strip()[-200:]}")
        return failures + ["emissive-match"]

    lum_a = _emissive_lumas(a)
    lum_b = _emissive_lumas(b)
    ratios, worst_name, worst = _emissive_worst(lum_a, lum_b)
    # A presence floor, because every band ratio here is 1.0 on two BLACK frames
    # and this arm is the acceptance test. The floor band is lit by the panel in
    # both variants, so it is the honest one to demand light from; the value is
    # well under the ~30/255 it actually reads and well over a clear colour.
    lit = min(lum_a["floor"], lum_b["floor"])
    detail = " ".join(f"{k}={v:.4f}" for k, v in ratios.items())
    ok = abs(worst - 1.0) <= EMISSIVE_MATCH_EPS and lit >= EMISSIVE_PRESENCE_MIN
    print(f"  emissive-match {'PASS' if ok else 'FAIL'}  derived/authored {detail} "
          f"worst={worst:.4f} at {worst_name} want 1.0 +/-{EMISSIVE_MATCH_EPS}; "
          f"floor lit={lit:.4f} want >={EMISSIVE_PRESENCE_MIN}")
    if not ok:
        failures.append("emissive-match")

    # Asserted on the OVERRIDE BEING APPLIED, not on a shadow appearing.
    #
    # This arm first compared the no-override frame against the AUTHORED one and
    # read 1.3684x, which looked like the shadow and was not: those two frames
    # also differ by the 18x quad and the 1 cm offset, and all three push the
    # ratio the same way. Compared against the frame that differs ONLY in the
    # override, every band is identical to four decimals.
    #
    # A panel's shadow is the penumbra group's; asserting it here would be asserting
    # someone else's feature through an instrument built for another.
    #
    # What this arm owns is that a light_overrides entry can NAME a light that did
    # not exist when the scene file was written -- which is the whole ordering
    # claim -- and the renderer says so itself. The failure mode is loud and
    # distinct: cscene_apply prints "matches no light" instead.
    _, text = _emissive_probe(derived, ["--emissive-lights"])
    applied = "light 'cornell_light' cast_shadows on" in text
    unmatched = "matches no light" in text
    ok = applied and not unmatched
    print(f"  emissive-override {'PASS' if ok else 'FAIL'} override reached the derived "
          f"panel: applied={applied} unmatched={unmatched} "
          "(names a light the scene file could not have known about)")
    if not ok:
        failures.append("emissive-override")

    # OCCLUSION, which the arm above explicitly cannot measure: it records that an
    # area panel's shadow does not move any band on cornell_box, for either the
    # derived panel or the fixture's own authored one. cornell_leak is the fixture
    # where it does move, because the whole room is built around one occluder.
    #
    # A derived panel inherits cast_shadows FALSE -- create_light's default, which
    # emissive_light.c never overrides -- so out of the box it lights straight
    # through the partition. That is not a defect this feature introduced; it is
    # what every non-shadowing area light does, and it is the reason
    # cornell_leak.cscn authors a SPOT rather than reusing the box room's panel.
    # What was missing is that nothing pointed the fixture at a derived panel, so
    # the behaviour was neither asserted nor written down anywhere a change could
    # trip over it.
    #
    # Both directions in one arm, because separately neither is worth much: the
    # shadowed zero is only meaningful beside a frame proving the band can read a
    # leak at all.
    leak_src = asset(EMISSIVE_LEAK_FIXTURE)
    if not os.path.exists(leak_src):
        print(f"  emissive-occluded SKIP ({EMISSIVE_LEAK_FIXTURE} not present)")
    else:
        def _leak_variant(shadow):
            def mutate(d):
                d["lights"] = []
                d.setdefault("materials", {}).setdefault(
                    "cornell_light", {})["emissiveStrength"] = 18.0
                if shadow:
                    d["light_overrides"] = [{"name": "cornell_light", "cast_shadows": True}]
            return mutate

        leak_ok = True
        reads = {}
        for tag, shadow in (("shadowed", True), ("unshadowed", False)):
            scn = os.path.join(workdir, f"emissive_leak_{tag}.cscn")
            cscn_copy(leak_src, scn, _leak_variant(shadow))
            img = os.path.join(workdir, f"emissive_leak_{tag}.ppm")
            err = render(scn, img, base + ["--emissive-lights"])
            if err:
                print(f"  emissive-occluded ERROR {tag} render failed: {err.strip()[-200:]}")
                failures.append("emissive-occluded")
                leak_ok = False
                break
            w, h, pix = _read_ppm(img)
            project = _projector(_cscn_camera(EMISSIVE_LEAK_FIXTURE), w, h)
            band = {}
            for name, pts in EMISSIVE_LEAK_BANDS.items():
                xs, ys = zip(*(project(pt) for pt in pts))
                x0, x1 = min(xs) / w, max(xs) / w
                y0, y1 = min(ys) / h, max(ys) / h
                dx, dy = (x1 - x0) * EMISSIVE_BAND_INSET, (y1 - y0) * EMISSIVE_BAND_INSET
                band[name] = sum(_absorb_box_rgb(
                    pix, w, h, (x0 + dx, y0 + dy, x1 - dx, y1 - dy))) / 3.0
            reads[tag] = band["dark"] / max(band["lit"], 1e-6)

        if leak_ok:
            shadowed, unshadowed = reads["shadowed"], reads["unshadowed"]
            ok = shadowed <= EMISSIVE_LEAK_MAX and unshadowed >= EMISSIVE_LEAK_MIN
            print(f"  emissive-occluded {'PASS' if ok else 'FAIL'} dark/lit behind the "
                  f"partition: shadowed={shadowed:.4f} want <={EMISSIVE_LEAK_MAX}; "
                  f"unshadowed={unshadowed:.4f} want >={EMISSIVE_LEAK_MIN} "
                  "(the second is the falsifier -- it proves the band can see a leak)")
            if not ok:
                failures.append("emissive-occluded")

    # The indirect term must not count the emitter twice. Four renders because a
    # LIFT is what is being compared, not a brightness: each side is measured
    # against its own GI-off frame, so the arm survives the two sides differing
    # in anything else. 120 frames is the probe volume converging -- it captures a
    # couple of probes a frame and then idles.
    gi_a = os.path.join(workdir, "emissive_gi_authored.ppm")
    gi_b = os.path.join(workdir, "emissive_gi_derived.ppm")
    # Dedicated GI-OFF baselines at the same frame count, rather than reusing the
    # match arm's 30-frame renders. A lift divides two frames, so anything that
    # differs between them contaminates it -- and 30 against 120 is a real
    # difference here, worth 2.7 points on the floor when it was measured.
    # The authored side gets its QUAD darkened for this arm, which the direct
    # arms above deliberately do not do. Reason: the derived side's emitter is
    # suppressed inside an irradiance capture (that is the feature), so an
    # authored quad still emitting its token 1 nit into ITS capture makes the two
    # captures see different amounts of emitter and the ratio inherits the
    # difference. Measured at 0.93 before this, in the direction the asymmetry
    # predicts. Darkening it leaves both captures seeing only bounced light,
    # which is the comparison the arm claims to make.
    def _derive_gi_authored(d):
        d.setdefault("materials", {}).setdefault("cornell_light", {})["emissiveStrength"] = 0.0

    gi_authored = os.path.join(workdir, "cornell_gi_authored.cscn")
    cscn_copy(cornell, gi_authored, _derive_gi_authored)

    off_a = os.path.join(workdir, "emissive_gi_authored_off.ppm")
    off_b = os.path.join(workdir, "emissive_gi_derived_off.ppm")
    err = (render(gi_authored, off_a, base, frames=EMISSIVE_GI_FRAMES) or
           render(derived, off_b, base + ["--emissive-lights"], frames=EMISSIVE_GI_FRAMES) or
           render(gi_authored, gi_a, base + ["--gi-volume"], frames=EMISSIVE_GI_FRAMES) or
           render(derived, gi_b, base + ["--emissive-lights", "--gi-volume"],
                  frames=EMISSIVE_GI_FRAMES))
    if err:
        print(f"  emissive-gi    ERROR render failed: {err.strip()[-200:]}")
        failures.append("emissive-gi")
    else:
        lift_a, _, _ = _emissive_worst(_emissive_lumas(off_a), _emissive_lumas(gi_a))
        lift_b, _, _ = _emissive_worst(_emissive_lumas(off_b), _emissive_lumas(gi_b))
        ratios, worst_name, worst = _emissive_worst(lift_a, lift_b)
        detail = " ".join(f"{k}={v:.4f}" for k, v in ratios.items())
        # The lift itself must be real, or two GI-inert frames give lift 1.0 on
        # both sides and a ratio of 1.0 that means nothing.
        ok = (abs(worst - 1.0) <= EMISSIVE_GI_EPS and
              min(lift_a["floor"], lift_b["floor"]) >= EMISSIVE_GI_LIFT_MIN)
        print(f"  emissive-gi    {'PASS' if ok else 'FAIL'}  derived/authored GI lift "
              f"{detail} worst={worst:.4f} at {worst_name} want 1.0 +/-{EMISSIVE_GI_EPS}; "
              f"floor lift a={lift_a['floor']:.4f} b={lift_b['floor']:.4f} "
              f"want >={EMISSIVE_GI_LIFT_MIN}")
        if not ok:
            failures.append("emissive-gi")

    # The SPECULAR counterpart of the arm above, and the reason it is a separate
    # arm rather than a second read on the same renders: the two double counts
    # want OPPOSITE fixes. A DDGI capture's output is irradiance ADDED to the
    # analytic direct term, so the emitter must be absent from it -- that is what
    # emissive-gi pins. A reflection probe's output is radiance, what a mirror
    # sees, so the emitter must be PRESENT. Silencing it in both is one flag and
    # would decide this question by accident.
    #
    # This arm therefore does not assert 1.0. It asserts the effect stays SMALL,
    # because the fix was measured and declined: at 1.0386 on a fixture built to
    # maximise it, every available fix costs more than it buys. The bar is what
    # would fail if that stopped being true.
    spec_scn = {}
    for tag, mutate in (("authored", None), ("derived", True)):
        def _spec(d, derived_side=mutate):
            for m in ("cornell_shell", "cornell_left_red", "cornell_right_green"):
                d.setdefault("materials", {}).setdefault(
                    m, {})["roughness"] = EMISSIVE_SPEC_ROUGHNESS
            if derived_side:
                d["lights"] = []
                d["light_overrides"] = [{"name": "cornell_light", "cast_shadows": True}]
                d.setdefault("materials", {}).setdefault(
                    "cornell_light", {})["emissiveStrength"] = 18.0
        spec_scn[tag] = os.path.join(workdir, f"emissive_spec_{tag}.cscn")
        cscn_copy(cornell, spec_scn[tag], _spec)

    spec = {t: {k: os.path.join(workdir, f"emissive_spec_{t}_{k}.ppm")
                for k in ("on", "off")} for t in ("authored", "derived")}
    lights = ["--emissive-lights"]
    err = (render(spec_scn["authored"], spec["authored"]["off"],
                  base + EMISSIVE_SPEC_SKY, frames=60) or
           render(spec_scn["authored"], spec["authored"]["on"],
                  base + EMISSIVE_SPEC_SKY + ["--probe-scene"], frames=60) or
           render(spec_scn["derived"], spec["derived"]["off"],
                  base + EMISSIVE_SPEC_SKY + lights, frames=60) or
           render(spec_scn["derived"], spec["derived"]["on"],
                  base + EMISSIVE_SPEC_SKY + lights + ["--probe-scene"], frames=60))
    if err:
        print(f"  emissive-spec  ERROR render failed: {err.strip()[-200:]}")
        failures.append("emissive-spec")
    else:
        lift_a, _, _ = _emissive_worst(_emissive_lumas(spec["authored"]["off"]),
                                       _emissive_lumas(spec["authored"]["on"]))
        lift_b, _, _ = _emissive_worst(_emissive_lumas(spec["derived"]["off"]),
                                       _emissive_lumas(spec["derived"]["on"]))
        ratios, worst_name, worst = _emissive_worst(lift_a, lift_b)
        detail = " ".join(f"{k}={v:.4f}" for k, v in ratios.items())
        peak = max(lift_a.values())
        ok = worst <= EMISSIVE_SPEC_MAX and peak >= EMISSIVE_SPEC_LIFT_MIN
        print(f"  emissive-spec  {'PASS' if ok else 'FAIL'} derived/authored probe lift "
              f"{detail} worst={worst:.4f} at {worst_name} want <={EMISSIVE_SPEC_MAX}; "
              f"peak lift={peak:.4f} want >={EMISSIVE_SPEC_LIFT_MIN} "
              "(the emitter is counted twice on purpose -- this bounds it)")
        if not ok:
            failures.append("emissive-spec")

    # OFF IS OFF, stated as an identity rather than as "the room goes dark",
    # which is what this arm was first written as and is measured wrong.
    #
    # This variant has no authored light, so with the feature off the scene has
    # no lights at all -- and the render app then substitutes its three-point
    # fallback rig, which floods the room to 16x the authored frame. Darkness is
    # not available to assert. What IS available: two different ways of having no
    # derived panel must produce the same frame. The flag absent, and the flag
    # present with the material opted out. Both get the fallback, so it cancels,
    # and the arm fails if either route leaks a panel.
    optout_lit = os.path.join(workdir, "cornell_derived_optout.cscn")

    def _derive_optout(d):
        _derive(d)
        d["materials"]["cornell_light"]["emissiveLight"] = "off"

    cscn_copy(cornell, optout_lit, _derive_optout)

    d_off = os.path.join(workdir, "emissive_off.ppm")
    d_opt = os.path.join(workdir, "emissive_off_optout.ppm")
    err = render(derived, d_off, base) or render(optout_lit, d_opt, base + ["--emissive-lights"])
    if err:
        print(f"  emissive-off   ERROR render failed: {err.strip()[-200:]}")
        return failures + ["emissive-off"]
    # An exact identity, not a box tolerance. These two frames differ in nothing
    # a renderer should see -- same scene, same fallback rig, same 18-nit quad,
    # and no derived panel by either route -- so the honest bar is 0 px, which is
    # what the spec specified before this shipped at 3%. Reading it through boxes
    # also coupled it to EMISSIVE_MATCH_EPS, whose 3% is sized for a genuine
    # approximation elsewhere; tightening that would have silently tightened this.
    ae, pae = compare(d_off, d_opt)
    lum_off = _emissive_lumas(d_off)
    moved = abs(lum_off["floor"] - lum_b["floor"]) / max(lum_b["floor"], 1e-6)
    ok = ae == 0 and moved >= EMISSIVE_OFF_MOVED_MIN
    print(f"  emissive-off   {'PASS' if ok else 'FAIL'}  flag-absent vs opted-out "
          f"ae={ae} pae={pae:.6f} want 0 px; and vs derived moved={moved:.4f} "
          f"want >={EMISSIVE_OFF_MOVED_MIN}")
    if not ok:
        failures.append("emissive-off")

    return failures
