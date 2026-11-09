"""The lut group: 3D LUT colour grading (spec 11.58 / E2)."""

import json
import os
import shutil
import subprocess

from gatelib.harness import RENDER, asset, asset_ref, render
from gatelib.images import LSB, _read_ppm, compare
from gatelib.scenes import _cscn_camera, _projector, cscn_copy


# ---- 3D LUT colour grading (spec 11.58) ------------------------------------
#
# Every arm renders with --no-dither and --no-vignette. Not tidiness: the reads
# are single patch centres and per-channel spreads of a few 8-bit codes, which
# is exactly the size of one dither LSB -- the eight existing --no-dither sites
# are here for the same reason. The vignette is off so the outer patches are not
# darkened toward each other, which would compress the range the table is
# sampled over.
LUT_FIXTURE = "lut_fixture.gltf"
LUT_SCENE = "lut_fixture.cscn"
# Tolerance for "the shader agrees with an independent interpolator", in 8-bit
# codes. Not zero: the gate's reference reads the frame AFTER it was quantized
# to 8 bits, so a value the shader computed at .49 of a code and rounded down is
# re-graded here from the rounded number. One code is that requantization and
# nothing else -- a wrong axis order, a missing inset or a wrong interpolant all
# move whole tens of codes.
LUT_AGREE_CODES = 1.5
# What the neutral probe must show. Trilinear measured 5 codes of tint on the
# rendered chart and tetrahedral exactly 0, so the floor sits below the first
# and the ceiling AT the second -- tetrahedral's exactness is the claim, and a
# tolerance here would let a half-broken decomposition through.
LUT_TINT_MIN = 2
LUT_TINT_MAX = 0
# The coarse probe's measured tri-vs-tet separation is 7.841 codes; half of it,
# so the arm discriminates a PARTLY wrong decomposition rather than only a fully
# degenerate one.
LUT_PROBE_MIN_CODES = 4


def _lut_render(workdir, name, extra, scene=None):
    out = os.path.join(workdir, name + ".ppm")
    src = scene or asset(LUT_SCENE)
    err = render(src, out, ["--no-auto-exposure", "-E", "1.0", "--no-dither", "--no-vignette"]
                 + extra)
    return (None, err) if err else (out, None)


def _lut_patch_points():
    """Patch centres, read out of the fixture's own glTF node translations.

    Read rather than restated for _cscn_camera's reason: a transcribed copy
    agrees with the asset only until someone edits the generator, and the
    failure is silent -- the gate keeps passing while sampling the backdrop.

    Deliberately NOT by importing gen_lut_fixture.py, which would make the
    ground truth a function of the thing under test and, since that generator
    writes at module scope, would rewrite the committed assets mid-gate.
    """
    with open(asset(LUT_FIXTURE)) as f:
        nodes = json.load(f)["nodes"]
    return [(n["name"], tuple(n["translation"])) for n in nodes
            if n.get("name", "").startswith("patch_")]


def _lut_read_patches(path):
    """(name, (r, g, b)) per patch, sampled at its projected centre."""
    w, h, pix = _read_ppm(path)
    proj = _projector(_cscn_camera(LUT_SCENE), w, h)
    out = []
    for name, centre in _lut_patch_points():
        x, y = proj(centre)
        o = (int(y) * w + int(x)) * 3
        out.append((name, (pix[o], pix[o + 1], pix[o + 2])))
    return out


def _lut_read_cube(path):
    """(size, [(r, g, b), ...]) with the file's own red-fastest ordering kept."""
    size, data = None, []
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            if s.upper().startswith("LUT_3D_SIZE"):
                size = int(s.split()[1])
                continue
            if s.upper().startswith(("TITLE", "DOMAIN_", "LUT_1D_SIZE")):
                continue
            parts = s.split()
            if len(parts) == 3:
                data.append(tuple(float(v) for v in parts))
    return size, data


def _lut_tetrahedral(size, data, c):
    """The gate's own tetrahedral interpolator.

    An INDEPENDENT implementation on purpose. The whole point of lut-agree is
    that a second reading of the same .cube reaches the same pixel, so sharing
    code with the shader -- or with the generator's closed forms -- would make
    the arm agree with itself.
    """
    def fetch(ir, ig, ib):
        d = size - 1
        ir, ig, ib = min(ir, d), min(ig, d), min(ib, d)
        return data[ib * size * size + ig * size + ir]

    d = size - 1
    p = [max(0.0, min(1.0, v)) * d for v in c]
    i = [min(int(v), d - 1) for v in p]
    fr, fg, fb = (p[k] - i[k] for k in range(3))
    c000, c111 = fetch(*i), fetch(i[0] + 1, i[1] + 1, i[2] + 1)
    if fr > fg:
        if fg > fb:
            a, b, w = fetch(i[0] + 1, i[1], i[2]), fetch(i[0] + 1, i[1] + 1, i[2]), \
                (fr - fg, fg - fb, fb)
        elif fr > fb:
            a, b, w = fetch(i[0] + 1, i[1], i[2]), fetch(i[0] + 1, i[1], i[2] + 1), \
                (fr - fb, fb - fg, fg)
        else:
            a, b, w = fetch(i[0], i[1], i[2] + 1), fetch(i[0] + 1, i[1], i[2] + 1), \
                (fb - fr, fr - fg, fg)
    else:
        if fb > fg:
            a, b, w = fetch(i[0], i[1], i[2] + 1), fetch(i[0], i[1] + 1, i[2] + 1), \
                (fb - fg, fg - fr, fr)
        elif fb > fr:
            a, b, w = fetch(i[0], i[1] + 1, i[2]), fetch(i[0], i[1] + 1, i[2] + 1), \
                (fg - fb, fb - fr, fr)
        else:
            a, b, w = fetch(i[0], i[1] + 1, i[2]), fetch(i[0] + 1, i[1] + 1, i[2]), \
                (fg - fr, fr - fb, fb)
    w0 = 1.0 - sum(w)
    return tuple(w0 * c000[k] + w[0] * a[k] + w[1] * b[k] + w[2] * c111[k] for k in range(3))


def run_lut_gate(workdir):
    """3D LUT colour grading: is it the artist's table, applied where it belongs?

    A LUT is a table of answers rather than a formula, so nothing about it can
    be checked by looking -- a table read in the wrong axis order, addressed by
    texel edge instead of centre, or interpolated off the neutral axis all
    produce a graded frame that looks like a grade. Every arm here is against a
    number known before the render.

      lut-agree     the decisive arm. Read the UNGRADED patch, push it through
                    the .cube with the gate's own tetrahedral interpolator, and
                    require the graded patch to match. Independent of the
                    scene's absolute values, so it cannot be fooled by exposure
                    or a tonemap change, and independent of the shader's own
                    arithmetic. Run on the steep table, the hardest one.
      lut-swap      R and B exchanged on every patch. The one arm that can catch
                    the data block being read blue-fastest: .cube stores red
                    fastest, and the transpose is a perfectly plausible image.
                    Linear, so both interpolants agree on it exactly, which
                    isolates "is the table right" from "is the interpolation".
      lut-identity  an identity table is a no-op. BOTH interpolants, because
                    they cover different mistakes and the default cannot see
                    the one that matters here: the half-texel inset is a
                    trilinear-only concern -- tetrahedral addresses texels by
                    integer index -- so the tetrahedral leg is 0 px whether the
                    inset is right or wrong, and only the trilinear leg moves
                    (1 code with it, 8 without). CANNOT STAND ALONE either way:
                    a LUT that never loaded is also 0 px, which is 11.22's
                    lesson, so it is meaningful only beside lut-agree above.
      lut-strength  0 is bit-exact off, 1 is full, and 0.5 lands between. The
                    C-side short-circuit arm: without the middle reading, on and
                    off take one path and the check passes against a dead
                    feature (11.21 shipped exactly that arm).
      lut-off       a scene authoring post.lut renders identically to passing
                    the same file to --lut, --no-lut returns the ungraded frame
                    exactly, and the CLI beats an authored strength and interp.
                    The authored path is RELATIVE on purpose: that is the only
                    way cscn_copy's absolutizer and cscene.c's resolve_in_place
                    ever run, and both fail silently -- the table does not load
                    and the frame renders ungraded.
      lut-refuse    eight malformed tables each refused BY NAME, and two
                    odd-but-valid ones (an unknown header keyword, a UTF-8 BOM)
                    accepted. A fifth of the reader is refusal logic and every
                    fixture is well-formed, so none of it ran; a review found
                    four wrong-reason refusals and two wrong accepts in there.
                    Meaningful only beside lut-agree, which is what stops a
                    reader that refuses everything from passing.
      lut-interp    the two interpolants AGREE on the linear table and DISAGREE
                    on the coarse probe. The disagreement is the half that
                    matters: without it a tetrahedral path that silently
                    degenerated to trilinear would pass every other arm here.
      lut-neutral   through a table that is identity on the grey diagonal,
                    tetrahedral holds r == g == b EXACTLY while trilinear tints.
                    The whole justification for tetrahedral being the default,
                    and the reason trilinear is kept reachable at all.
    """
    failures = []
    scene = asset(LUT_SCENE)
    if not os.path.exists(scene):
        print("  lut          SKIP  (missing %s)" % LUT_SCENE)
        return []
    cubes = {n: asset("lut_%s.cube" % n)
             for n in ("identity", "swap", "steep", "neutral")}
    missing = [p for p in cubes.values() if not os.path.exists(p)]
    if missing:
        print("  lut          SKIP  (missing %s)" % os.path.basename(missing[0]))
        return []

    base, err = _lut_render(workdir, "lut_base", [])
    if err:
        print("  lut          ERROR (ungraded render failed)\n" + err[-800:])
        return ["lut-render"]
    ungraded = _lut_read_patches(base)

    # lut-agree -- the shader against an independent reading of the same table
    steep, err = _lut_render(workdir, "lut_steep", ["--lut", cubes["steep"]])
    if err:
        print("  lut-agree    ERROR (steep render failed)\n" + err[-800:])
        failures.append("lut-agree")
    else:
        size, data = _lut_read_cube(cubes["steep"])
        graded = _lut_read_patches(steep)
        worst, where = 0.0, ""
        for (name, before), (_, after) in zip(ungraded, graded):
            want = _lut_tetrahedral(size, data, tuple(v / 255.0 for v in before))
            for k in range(3):
                d = abs(want[k] * 255.0 - after[k])
                if d > worst:
                    worst, where = d, name
        ok = worst <= LUT_AGREE_CODES
        print(f"  lut-agree    {'PASS' if ok else 'FAIL'}  worst {worst:.2f} codes at {where} "
              f"want <={LUT_AGREE_CODES} (an independent tetrahedral read of the same "
              f"{size}^3 table; a transposed or edge-addressed table moves tens of codes)")
        if not ok:
            failures.append("lut-agree")

    # lut-swap -- red-fastest, in the one form that cannot be mistaken
    swap, err = _lut_render(workdir, "lut_swap", ["--lut", cubes["swap"]])
    if err:
        print("  lut-swap     ERROR (swap render failed)\n" + err[-800:])
        failures.append("lut-swap")
    else:
        graded = _lut_read_patches(swap)
        worst, spread = 0.0, 0
        for (_, before), (_, after) in zip(ungraded, graded):
            want = (before[2], before[1], before[0])
            worst = max(worst, max(abs(want[k] - after[k]) for k in range(3)))
            spread = max(spread, abs(before[0] - before[2]))
        # The floor is what stops a chart of greys satisfying a swap trivially.
        ok = worst <= LUT_AGREE_CODES and spread >= 40
        print(f"  lut-swap     {'PASS' if ok else 'FAIL'}  worst {worst:.0f} codes "
              f"want <={LUT_AGREE_CODES}, and the chart separates R from B by {spread} "
              f"want >=40 (or swapping them proves nothing)")
        if not ok:
            failures.append("lut-swap")

    # lut-identity -- both legs, because they cover different mistakes
    ident, err = _lut_render(workdir, "lut_ident", ["--lut", cubes["identity"]])
    ident_tri, err2 = _lut_render(workdir, "lut_ident_tri",
                                  ["--lut", cubes["identity"], "--lut-interp", "trilinear"])
    if err or err2:
        print("  lut-identity ERROR (identity renders failed)\n" + (err or err2)[-800:])
        failures.append("lut-identity")
    else:
        ae, pae = compare(base, ident)
        ae_tri, pae_tri = compare(base, ident_tri)
        # The trilinear leg is a PEAK bound, not a pixel count, and it is the
        # only thing in the group that can see the half-texel inset: tetrahedral
        # addresses texels by integer index, so the default path is exactly 0
        # whether the inset is right or not. Dropping it takes this reading from
        # 1 code to 8.
        ok = ae == 0 and pae_tri <= LSB
        print(f"  lut-identity {'PASS' if ok else 'FAIL'}  tetrahedral {ae} px want exactly 0 "
              f"(PAE {pae:.6f}); trilinear PAE {pae_tri:.6f} want <={LSB:.6f} on {ae_tri} px "
              f"-- that leg is the half-texel inset, which reads 0.031373 without it")
        if not ok:
            failures.append("lut-identity")

    # lut-strength -- and the middle reading that makes it falsifiable
    zero, e0 = _lut_render(workdir, "lut_s0", ["--lut", cubes["steep"], "--lut-strength", "0"])
    half, e5 = _lut_render(workdir, "lut_s5", ["--lut", cubes["steep"], "--lut-strength", "0.5"])
    if e0 or e5 or not steep:
        print("  lut-strength ERROR (strength renders failed)\n" + (e0 or e5)[-800:])
        failures.append("lut-strength")
    else:
        ae0, _ = compare(base, zero)
        mid = _lut_read_patches(half)
        full = _lut_read_patches(steep)
        # 0.5 must land on the MIDPOINT, not merely inside the band. The band
        # form was written first and is satisfied by any wrong blend curve that
        # stays between the endpoints -- mix(c, graded, s*s) lands at 0.25 of
        # the way and passes it. The exact answer is free here: mix is linear in
        # the display-encoded value and both endpoints are read after the same
        # encode, so the midpoint is (a + b) / 2 to within the three roundings.
        #
        # `moved` is the floor that stops a chart with nothing to blend from
        # satisfying the midpoint trivially -- at a == b every strength agrees.
        worst_mid, moved = 0.0, 0
        for (_, a), (_, m), (_, b) in zip(ungraded, mid, full):
            for k in range(3):
                if abs(a[k] - b[k]) >= 4:
                    moved += 1
                    worst_mid = max(worst_mid, abs(m[k] - 0.5 * (a[k] + b[k])))
        ok = ae0 == 0 and worst_mid <= 1.5 and moved >= 40
        print(f"  lut-strength {'PASS' if ok else 'FAIL'}  strength 0 is {ae0} px want exactly 0; "
              f"0.5 is off the midpoint by {worst_mid:.2f} codes want <=1.5 on {moved} channels "
              f"the table actually moves, want >=40 (a squared or smoothstep blend reads ~25%)")
        if not ok:
            failures.append("lut-strength")

    # lut-off -- the two authoring paths are one path, and --no-lut is the way out
    authored = os.path.join(workdir, "lut_authored.cscn")

    # All three keys, and the path RELATIVE. Authoring only an absolute `path`
    # left two things this branch added unreachable: cscn_copy's absolutizer
    # (which only fires on a relative one) and cscene.c's resolve_in_place. Both
    # fail the IES way rather than the model way -- the LUT silently does not
    # load and the frame renders ungraded -- so an arm that never exercises them
    # is exactly the shape this group exists to avoid.
    shutil.copy(cubes["swap"], os.path.join(workdir, "lut_swap.cube"))

    def _author(d):
        d.setdefault("post", {})["lut"] = {"path": asset_ref("lut_swap.cube"), "strength": 1.0,
                                           "interp": "tetrahedral"}

    cscn_copy(scene, authored, _author)
    by_scene, e_scene = _lut_render(workdir, "lut_scene", [], scene=authored)
    forced_off, e_off = _lut_render(workdir, "lut_scene_off", ["--no-lut"], scene=authored)
    # The CLI must beat all three authored keys, not just the path: a scene
    # authoring trilinear at half strength, overridden on the command line,
    # has to reach the same pixels as the pure-CLI render.
    beaten = os.path.join(workdir, "lut_beaten.cscn")

    def _author_weak(d):
        d.setdefault("post", {})["lut"] = {"path": asset_ref("lut_swap.cube"), "strength": 0.25,
                                           "interp": "trilinear"}

    cscn_copy(scene, beaten, _author_weak)
    by_cli, e_cli = _lut_render(workdir, "lut_cli_wins",
                                ["--lut", cubes["swap"], "--lut-strength", "1",
                                 "--lut-interp", "tetrahedral"], scene=beaten)
    if e_scene or e_off or e_cli or not swap:
        print("  lut-off      ERROR (scene-authored renders failed)\n"
              + ((e_scene or e_off or e_cli) or "the swap render above failed")[-800:])
        failures.append("lut-off")
    else:
        ae_same, _ = compare(swap, by_scene)
        ae_off, _ = compare(base, forced_off)
        ae_cli, _ = compare(swap, by_cli)
        ok = ae_same == 0 and ae_off == 0 and ae_cli == 0
        print(f"  lut-off      {'PASS' if ok else 'FAIL'}  post.lut vs --lut {ae_same} px want 0 "
              f"(via a RELATIVE path, so the scene-file resolver runs); --no-lut vs no LUT at all "
              f"{ae_off} px want 0; CLI over an authored strength+interp {ae_cli} px want 0")
        if not ok:
            failures.append("lut-off")

    # lut-refuse -- the reader's whole thesis, which nothing was testing
    #
    # Roughly a fifth of lut.c is refusal logic and the commit that landed it is
    # titled "a .cube reader that refuses by name". Every fixture is well-formed,
    # so none of it ran. A review pass found four wrong-reason refusals and two
    # wrong accepts in that uncovered code; this is what stops the next four.
    #
    # Cheap by construction: a refusal happens at LOAD, so one frame at 100x100
    # is enough to reach it, and the assertion is on the named reason rather
    # than on pixels. The pair with lut-agree is what makes it meaningful -- a
    # reader that refused EVERYTHING would pass this and fail that.
    bad = [
        ("a 1D LUT", 'TITLE "1d"\nLUT_1D_SIZE 4\n0 0 0\n1 1 1\n', "is a 1D LUT"),
        ("a log-domain table", "LUT_3D_SIZE 2\nDOMAIN_MAX 4 4 4\n" + "0 0 0\n" * 8,
         "only the 0..1 domain"),
        ("no declared size", 'TITLE "nosize"\n0 0 0\n1 1 1\n', "declares no LUT_3D_SIZE"),
        ("a non-numeric size", "LUT_3D_SIZE abc\n0 0 0\n", "not a number"),
        ("a size that wraps int", "LUT_3D_SIZE 4294967298\n" + "0 0 0\n" * 8,
         "outside the supported"),
        ("a truncated block", "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n", "truncated or malformed"),
        ("a trailing value", "LUT_3D_SIZE 2\n" + "0 0 0\n" * 9, "carries more than"),
        ("a value past fp16", "LUT_3D_SIZE 2\n1e30 0 0\n" + "0 0 0\n" * 7, "out-of-range value"),
    ]
    # ...and two the reader must ACCEPT. Both were refused before the review:
    # an unknown keyword ended the header, so a real Iridas extension and a
    # Windows BOM each read as a truncated data block.
    good = [
        ("an unknown keyword", "LUT_3D_SIZE 2\nLUT_3D_INPUT_RANGE 0.0 1.0\n"
         + "".join(f"{r} {g} {b}\n" for b in (0, 1) for g in (0, 1) for r in (0, 1))),
        ("a UTF-8 BOM", "﻿LUT_3D_SIZE 2\n"
         + "".join(f"{r} {g} {b}\n" for b in (0, 1) for g in (0, 1) for r in (0, 1))),
    ]
    missed, wrongly_refused = [], []
    for label, body, want in bad:
        p = os.path.join(workdir, "bad_%d.cube" % len(missed + wrongly_refused))
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(body)
        r = subprocess.run([RENDER, "-m", scene, "-x", "-f", "1", "-W", "100", "-H", "100",
                            "--lut", p], capture_output=True, text=True)
        if want not in (r.stdout + r.stderr):
            missed.append(label)
    for label, body in good:
        p = os.path.join(workdir, "good_%d.cube" % len(wrongly_refused))
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(body)
        r = subprocess.run([RENDER, "-m", scene, "-x", "-f", "1", "-W", "100", "-H", "100",
                            "--lut", p], capture_output=True, text=True)
        if "2^3" not in (r.stdout + r.stderr):
            wrongly_refused.append(label)
    ok = not missed and not wrongly_refused
    print(f"  lut-refuse   {'PASS' if ok else 'FAIL'}  {len(bad)} malformed tables each refused "
          f"BY NAME ({len(missed)} misnamed: {', '.join(missed) or 'none'}); "
          f"{len(good)} odd-but-valid tables loaded ({len(wrongly_refused)} wrongly refused: "
          f"{', '.join(wrongly_refused) or 'none'})")
    if not ok:
        failures.append("lut-refuse")

    # lut-interp -- agree where the table is linear, differ where it is not
    tri_lin, e_tri_lin = _lut_render(workdir, "lut_tri_lin",
                                     ["--lut", cubes["swap"], "--lut-interp", "trilinear"])
    tri_n, e_tri_n = _lut_render(workdir, "lut_tri_n",
                                 ["--lut", cubes["neutral"], "--lut-interp", "trilinear"])
    tet_n, e_tet_n = _lut_render(workdir, "lut_tet_n",
                                 ["--lut", cubes["neutral"], "--lut-interp", "tetrahedral"])
    if e_tri_lin or e_tri_n or e_tet_n or not swap:
        print("  lut-interp   ERROR (interp renders failed)\n"
              + ((e_tri_lin or e_tri_n or e_tet_n) or "the swap render above failed")[-800:])
        failures.append("lut-interp")
    else:
        ae_lin, pae_lin = compare(swap, tri_lin)
        ae_probe, pae_probe = compare(tri_n, tet_n)
        # The linear half is a bound on PEAK error, not a pixel count: the two
        # interpolants reach the same value by different arithmetic, so a
        # handful of pixels rounding apart is expected and a visible difference
        # is not.
        # A MAGNITUDE on the probe, not `> 0`. The spec measured 7.841 codes
        # there, so a bar of one differing pixel catches full degeneration and
        # nothing short of it -- a partially wrong decomposition would clear it.
        # 4 codes keeps 2x headroom against the measurement.
        ok = pae_lin <= LSB and pae_probe >= LUT_PROBE_MIN_CODES / 255.0
        print(f"  lut-interp   {'PASS' if ok else 'FAIL'}  on the LINEAR table the two agree to "
              f"PAE {pae_lin:.6f} want <={LSB:.6f} ({ae_lin} px); on the coarse probe they "
              f"differ by PAE {pae_probe:.6f} want >={LUT_PROBE_MIN_CODES / 255.0:.6f} "
              f"({ae_probe} px; or tetrahedral is trilinear)")
        if not ok:
            failures.append("lut-interp")

    # lut-neutral -- the claim tetrahedral is bought for
    if e_tri_n or e_tet_n:
        print("  lut-neutral  SKIP  (the interp renders failed above)")
    else:
        def _grey_spread(path):
            worst = 0
            for name, rgb in _lut_read_patches(path):
                if "grey" in name:
                    worst = max(worst, max(rgb) - min(rgb))
            return worst

        # Measured as an INCREASE over the ungraded frame's own grey spread.
        # The absolute form reads whatever chroma the pipeline already put on a
        # grey patch -- bloom is on in this group, and a colour patch sits ~47 px
        # away -- so it would move with the bloom radius or the chart pitch for
        # reasons that have nothing to do with the interpolant. The in-frame
        # control is the same idiom contact_local_fixture's exact 1.0000 uses.
        tint_base = _grey_spread(base)
        tint_tri = _grey_spread(tri_n) - tint_base
        tint_tet = _grey_spread(tet_n) - tint_base
        ok = tint_tri >= LUT_TINT_MIN and tint_tet <= LUT_TINT_MAX
        print(f"  lut-neutral  {'PASS' if ok else 'FAIL'}  through a table that is identity on "
              f"the grey diagonal, trilinear ADDS {tint_tri} codes of grey tint want "
              f">={LUT_TINT_MIN} and tetrahedral adds {tint_tet} want <={LUT_TINT_MAX} (exact, "
              f"since all six tetrahedra share that diagonal as an edge)")
        if not ok:
            failures.append("lut-neutral")

    return failures
