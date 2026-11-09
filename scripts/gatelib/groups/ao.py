"""The ao group: ambient occlusion reaches the frame (spec 11.75)."""

import os

from gatelib.harness import asset, render
from gatelib.images import _read_ppm, compare


AO_SCENE = asset("cornell_rooms.cscn")
# AO on vs off moves 32% of this frame at PAE 71/255. The bar is a fraction of
# that with room to spare: what it exists to catch is the chain going SILENT --
# an FBO the driver refuses, a gate that stopped arming, a format nothing can
# render to -- not a shift in how dark the corners get.
AO_ACTIVE_MIN_FRAC = 0.05

# The ring read. Columns across the contact band on cornell_rooms' floor, and
# the rows that span it -- picked because the AO buffer is strictly monotone
# through this window, which is what makes it usable as a control.
AO_RING_COLS = range(600, 1001, 50)
AO_RING_ROWS = (745, 815)
# A step smaller than this is dither and quantisation, not a band. The bands
# this exists to catch measured 17-22 codes.
AO_RING_TOL = 4
# One column has to actually reach into shadow or the monotonicity assertion is
# satisfied by a flat frame. The deepest column reads 5 with the term working.
AO_RING_DIP_MAX = 128
# The open floor, past the contact band and still on lit geometry. The second
# half of that is a constraint, not a description: below row ~815 this frame is
# black background, where the term's zero-normal guard hands back plain AO and
# reads 255 whatever the term did. The first draft of this arm sampled exactly
# there and passed a build whose numerator and denominator described different
# lobes -- so the sampled band is checked against the LIT frame every run.
AO_RING_OPEN_ROWS = range(785, 816, 5)
AO_RING_OPEN_COLS = range(300, 1301, 50)
# Max channel a sample must reach in the lit frame to count as geometry.
AO_RING_OPEN_MIN_LIT = 32


def _ao_ring_reversals(px, w, x):
    """Steps that DARKEN by more than the tolerance while scanning outward from
    the darkest row of the window. Approaching an occluder is allowed to darken;
    coming back out into the open and darkening again is the artifact."""
    y0, y1 = AO_RING_ROWS
    vals = [px[(y * w + x) * 3] for y in range(y0, y1)]
    lo = min(range(len(vals)), key=lambda k: vals[k])
    return vals, sum(1 for k in range(lo + 1, len(vals))
                     if vals[k - 1] - vals[k] > AO_RING_TOL)


def run_ao_gate(workdir):
    """Ambient occlusion reaches the frame, and does it the same way twice.

      ao-active        AO on and AO off are different pictures, by a wide
                       margin -- the chain is armed, allocated and composited
      ao-deterministic two runs of the AO-on frame are byte-identical
      ao-ring          the specular-occlusion term dips into a contact and comes
                       back out without banding, measured against the AO buffer's
                       own monotonicity through the same window

    ao-ring IS THE ONLY EXECUTABLE COVERAGE OF ANY SPECULAR-OCCLUSION MODE.
    Before it the whole feature -- four modes, the split composite's blend
    algebra, the term itself -- was pinned by golden pixels and nothing else,
    which is how spec 11.75 shipped a cliff in it with every gate green.

    Its three assertions are each vacuous alone, and that is why there are
    three. A build that computes nothing reads a flat 255: perfectly monotone,
    perfectly exact on open floor, and wrong -- so the dip is asserted first. A
    build that saturates everything dips beautifully and bands horribly -- so
    monotonicity is asserted against the AO buffer rather than against a
    constant, because the AO is the same estimator's own output through the same
    window and is strictly monotone there. And the open-floor read catches a
    third failure neither of the others can see: where nothing occludes, the
    term's numerator and denominator are sums over the SAME sectors, so the
    ratio is exactly one by construction -- for any lobe, at any roughness, at
    any angle. Anything less means the two stopped describing one lobe, or the
    accumulation and blur moved them apart. It is an exact algebraic identity
    rather than a measurement, which is what makes it worth an equality test.

    The bar is the AO's count, not zero, deliberately. What the artifact is is
    the specular term being LESS monotone than the occlusion it derives from;
    a scene whose AO genuinely reverses through the window should be allowed to
    reverse here too. On this fixture the AO reads 0 and the cone term read 27.

    All three were falsified by hand. Making lobeSectors return no bits reads a
    flat 255 and is caught by the dip alone, with the other two passing
    perfectly. Mismatching the numerator's lobe against the denominator's
    (popCount(lobe) + 1) is caught by the exactness alone, at 0/147. The
    monotonicity arm was measured against the cone term itself -- the same
    counting over the same columns on the pre-change renderer's own output --
    which is where the 27 comes from.

    THE EXACTNESS ASSERTION WAS VACUOUS IN ITS FIRST DRAFT and the mutation is
    what found it. It sampled rows 1000-1100, which look like flat 255 in every
    version of this frame and are black background: out there the term's
    zero-normal guard hands back plain AO, so the lobe mismatch passed. The band
    moved onto lit floor, and the lit frame is now sampled alongside it so the
    same drift cannot happen silently again. It is the beach-shoreline lesson --
    a read that agrees with itself on the wrong surface.

    TWO OF THE FOUR ARMS THIS GROUP WANTED COULD NOT BE BUILT. Spec 11.75
    planned them and neither could be written honestly; both attempts are
    recorded here because the next person will have the same two ideas.

    **A silhouette arm was measured and abandoned.** 11.75's depth-aware
    upsample stops AO bleeding across a depth edge, and the effect is real --
    reverting it to plain bilinear moves 3,252 px here, peaking at 53/255. But
    it is a ONE-TO-TWO PIXEL band along silhouettes by construction, which is
    the entire reach of a 2x2 footprint, so there is no region a box mean can
    read: averaging any rectangle containing it dilutes 53 codes to 0.0003.
    Selecting the affected pixels instead would mean selecting them by whether
    the mutation moved them, which is circular. The property is real, worth
    having, and lives in the goldens rather than here.

    **A banding arm was designed against a premise that turned out false.** The
    AO target went RGBA8 -> RGBA16F in 11.75 on the theory that ~20 codes across
    an 0.92-1.00 gradient was what the visible contour banding was made of.
    Measured after: identical flat-run lengths and identical span. The bands are
    the ESTIMATOR's (2 slices, gtao_frag.glsl), not the storage's, so an arm
    asserting short runs would fail on a correct build. The format change stands
    on its other reason -- see postfx.c -- and this arm is not written.

    What is left is coverage of the thing that actually breaks silently: an AO
    chain that stops running. Nothing else in the suite asserts that; before
    this group the entire chain's only executable coverage was golden pixels.
    """
    if not os.path.exists(AO_SCENE):
        print("  ao-active    SKIP  (cornell_rooms.cscn not present)")
        return []
    failures = []
    flags = ["--sky", "--no-auto-exposure", "-E", "1.0", "-W", "800", "-H", "600"]
    on = os.path.join(workdir, "ao_on.ppm")
    off = os.path.join(workdir, "ao_off.ppm")
    on2 = os.path.join(workdir, "ao_on2.ppm")
    err = (render(AO_SCENE, on, flags) or render(AO_SCENE, off, flags + ["--no-ssao"]) or
           render(AO_SCENE, on2, flags))
    if err:
        print(f"  ao-active    FAIL  render error\n{err}")
        print("  ao-deterministic FAIL  (same)")
        return ["ao-active", "ao-deterministic"]

    moved, _ = compare(on, off)
    w, h, _ = _read_ppm(on)
    frac = moved / float(w * h)
    ok = frac >= AO_ACTIVE_MIN_FRAC
    if not ok:
        failures.append("ao-active")
    print(f"  ao-active    {'PASS' if ok else 'FAIL'}  AO moves {frac * 100.0:.1f}% of the "
          f"frame ({moved} px, want >= {AO_ACTIVE_MIN_FRAC * 100.0:.0f}%); a chain that "
          f"silently stopped running reads 0.0%")

    same, _ = compare(on, on2)
    ok = same == 0
    if not ok:
        failures.append("ao-deterministic")
    print(f"  ao-deterministic {'PASS' if ok else 'FAIL'}  two runs differ by {same} px "
          f"(want 0: no --taa, so every temporal history is force-invalidated and GTAO "
          f"is a pure function of the frame)")

    term = os.path.join(workdir, "ao_term.ppm")
    aobuf = os.path.join(workdir, "ao_raw.ppm")
    err = (render(AO_SCENE, term, flags + ["--spec-occ-debug"]) or
           render(AO_SCENE, aobuf, flags + ["--ssao-debug"]))
    if err:
        print(f"  ao-ring      FAIL  render error\n{err}")
        return failures + ["ao-ring"]

    tw, _, tpx = _read_ppm(term)
    aw, _, apx = _read_ppm(aobuf)
    term_rev = sum(_ao_ring_reversals(tpx, tw, x)[1] for x in AO_RING_COLS)
    ao_rev = sum(_ao_ring_reversals(apx, aw, x)[1] for x in AO_RING_COLS)
    deepest = min(min(_ao_ring_reversals(tpx, tw, x)[0]) for x in AO_RING_COLS)
    lw, _, lpx = _read_ppm(on)
    open_vals, open_lit = [], []
    for y in AO_RING_OPEN_ROWS:
        for x in AO_RING_OPEN_COLS:
            open_vals.append(tpx[(y * tw + x) * 3])
            o = (y * lw + x) * 3
            open_lit.append(max(lpx[o], lpx[o + 1], lpx[o + 2]))
    open_exact = sum(1 for v in open_vals if v == 255)
    open_geom = sum(1 for v in open_lit if v >= AO_RING_OPEN_MIN_LIT)

    dips = deepest <= AO_RING_DIP_MAX
    monotone = term_rev <= ao_rev
    exact = open_exact == len(open_vals) and open_geom == len(open_lit)
    ok = dips and monotone and exact
    if not ok:
        failures.append("ao-ring")
    print(f"  ao-ring      {'PASS' if ok else 'FAIL'}  the term reaches {deepest} at its "
          f"darkest (want <= {AO_RING_DIP_MAX}, or a build computing nothing reads a flat 255 "
          f"and satisfies the other two perfectly); {term_rev} darkening steps > "
          f"{AO_RING_TOL} codes on the way back out against the AO's own {ao_rev} across "
          f"{len(AO_RING_COLS)} columns (want <= it -- the cone term this replaced read 27); "
          f"open floor {open_exact}/{len(open_vals)} exactly 255 (want all: with nothing "
          f"occluding, visible and total are sums over the same sectors and the ratio is "
          f"exactly one, so anything less means they were not measured against the same "
          f"lobe or the denoise chain moved them apart), on {open_geom}/{len(open_lit)} "
          f"samples that are lit geometry (want all, or the band has slid off onto the "
          f"black background where the term is not consulted at all)")

    return failures
