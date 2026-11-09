"""The decals group: clustered decals (projection, fade, surface, masks; spec 11.73)."""

import json
import os
import subprocess

from gatelib.harness import RENDER, ROOT, _import_fixture_gen, _run, asset
from gatelib.images import _read_ppm, compare
from gatelib.probes import _config_run
from gatelib.scenes import PROBE_SKY, _cscn_camera, _projector, cscn_copy


# --- clustered decals (spec 11.73) ------------------------------------------
DECAL_FIXTURE = "decal_fixture.cscn"
_DECAL_GEN = None


def _decal_gen():
    """The decal fixture's generator, IMPORTED rather than transcribed.

    This block used to carry a comment saying exactly that while listing nine
    hand-copied literals underneath it -- including two derived by hand from the
    .gltf's baseColorFactors, two steps from their source. The mechanism the
    comment described already existed and is used by the layers gate; it just
    was not called.
    """
    global _DECAL_GEN
    if _DECAL_GEN is None:
        _DECAL_GEN = _import_fixture_gen("gen_decal_fixture.py", "decals")
    return _DECAL_GEN


def _decal_substrate(gen, material):
    """The albedo view's byte for an authored baseColorFactor.

    linearToSRGB in this engine is pow(1/2.2), not the piecewise sRGB curve --
    reading a substrate through the piecewise one is a two-code error on the
    floor, which is inside this gate's tolerance and would hide a real one.
    """
    for m in gen.GLTF["materials"]:
        if m["name"] == material:
            f = m["pbrMetallicRoughness"]["baseColorFactor"]
            return tuple(int(round((c ** (1.0 / 2.2)) * 255.0)) for c in f[:3])
    raise KeyError(material)


# How far a byte may sit from its painted value. Two codes of rounding through
# the linear/encode round trip, plus one of slack; anything the decal path gets
# WRONG is tens of codes. NOT dither -- --render-mode 6 takes the passthrough
# blit, which skips the tonemap where the dither lives.
DECAL_CODE_EPS = 3
# The froxel grid, from light_cluster.h. A decal is a small box, so the two in
# this fixture claim a tiny fraction of it -- measured 130 of 3072. The bound is
# the whole grid rather than a tight fit around that number: what it exists to
# catch is culling that stopped working altogether (marking every cell reads
# 6144), not a shift of a few cells when the camera or the boxes move.
DECAL_GRID_CELLS = 16 * 8 * 24


def _decal_run(workdir, tag, mutate=None, extra=None, frames=4):
    """Render the decal fixture, optionally through a mutation.

    Returns (pixels, w, h, output) or (None, None, None, error).
    """
    src = asset(DECAL_FIXTURE)
    if not os.path.exists(src):
        return None, None, None, "missing fixture"
    scene = src
    if mutate is not None:
        scene = os.path.join(workdir, f"decal_{tag}.cscn")
        cscn_copy(src, scene, mutate)
    out = os.path.join(workdir, f"decal_{tag}.ppm")
    cmd = [RENDER, "-m", scene, "-t", os.path.join(ROOT, "assets", "textures"), "-x", "-f", str(frames),
           "-W", "400", "-H", "300", "-S", out,
           "--no-auto-exposure", "-E", "1.0"] + (extra or [])
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None, None, None, r.stdout + r.stderr
    w, h, pix = _read_ppm(out)
    return pix, w, h, r.stdout + r.stderr


def _decal_byte(pix, w, h, project, world):
    """The raw RGB triple at a world point. RAW, not linearised: the albedo view
    hands back the exact code the generator painted, so the assertion is against
    a byte and introducing a decode would only add error to it.

    RAISES for a point that projects off-screen rather than clamping, which is
    _layer_sample's contract and for its reason: a clamp returns a number, and a
    number is what an arm reads, so a fixture whose framing drifted would go on
    passing against whatever pixel sat at the edge. This gate has already been
    bitten once by a read point that moved onto the wrong surface.
    """
    px, py = project(world)
    x, y = int(round(px)), int(round(py))
    if not (0 <= x < w and 0 <= y < h):
        raise AssertionError(f"world {world} projects to ({x},{y}), outside the {w}x{h} frame")
    o = (y * w + x) * 3
    return (pix[o], pix[o + 1], pix[o + 2])


def _decal_near(got, want, eps=DECAL_CODE_EPS):
    return all(abs(int(g) - int(v)) <= eps for g, v in zip(got, want))


def _decal_probe_rows(output):
    """The --decal-probe lines, split into the per-frame and per-decal halves."""
    frames, decals = [], []
    for line in output.splitlines():
        # `if "=" in kv`, the guard the file's four other k=v readers carry: a
        # bare token added to either line would otherwise raise mid-suite
        # instead of leaving a field unread.
        if line.startswith("decal-probe decal "):
            decals.append(dict(kv.split("=", 1) for kv in line.split()[2:] if "=" in kv))
        elif line.startswith("decal-probe frame="):
            frames.append(dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv))
    return frames, decals


def run_decal_gate(workdir):
    """Marks projected through a box, culled into the froxel grid (spec 11.73).

      decals-identity   a decal-free scene is the pre-decal frame exactly, and the
                        fixture WITH its decals is not -- the second half is what
                        stops the first passing on a build that projects nothing
      decals-albedo     the poster's interior reads its painted code through the
                        albedo view, with clear wall as the in-frame control
      decals-orient     each of the poster's four quadrants lands in the world
                        corner it was painted for -- the arm that says the mark
                        is a PICTURE and not just a coloured patch
      decals-angle-fade the oblique plate inside the poster's box reads its own
                        substrate: a projector must refuse a surface it grazes
      decals-surface    the scorch's roughness reads its PAINTED code through
                        renderMode 8, against a no-surface-map twin and a floor
                        control that must not move
      decals-normal     flattening the relief ALONE -- the map still bound --
                        moves the lit frame, which is the only arm that reaches
                        the decal's tangent frame
      decals-opacity    a half-opaque poster reads the MIDPOINT between its own code
                        and the wall's, not something darker -- the premultiplied
                        accumulation, which the opaque arms above cannot see
      decals-feather    a point inside the poster's edge ramp reads the substrate
                        mixed at the coverage the box geometry owes, so a mark
                        fades out of its volume instead of stopping dead
      decals-overlap    a THIRD mark over the poster wins where it covers it, and
                        the poster still reads its own code beside it -- the only
                        arm where the accumulator is non-empty at all
      decals-mask       two runs agree on the froxel digest, the live decals claim
                        a fraction of the grid rather than all of it, and a decal
                        behind the camera claims no froxel and moves no pixel
      decals-schema     a decal missing position, size, direction or image is
                        refused by name, and a degenerate size is refused too
      decals-capture    a decal reaches a reflection PROBE's capture, read as the
                        poster's reflection in a mirror floor -- the property the
                        materialArray tenancy bought over a unit-6 alias
      decals-config     the snapshot's nine decal rows survive a dump and a
                        restore, and a perturbed one CHANGES THE FRAME -- the
                        config group's own fixture authors no decal, so without
                        this the rows are dead to the whole suite

    NO ARM HERE IS SAFE ALONE, the probe-set rule. decals-albedo alone passes on
    a build with no facing test, no edge and no mask -- it only asks that a mark
    landed. decals-angle-fade alone passes on a build that projects NOTHING, since
    a surface taking no decal is exactly what it asserts; its pairing with
    decals-albedo is what makes it a claim about refusal rather than absence. And
    decals-identity's first half passes on any build where the feature is inert,
    which is why its second half asserts the decals moved the frame at all.

    decals-overlap is the only arm that runs the accumulation with anything in it.
    The committed fixture's two decals are disjoint in two axes, so `over`
    composited against a zero accumulator every time -- which is the one input
    under which paint order, the premultiplied sum and a plain overwrite are all
    the same picture. It APPENDS its third mark rather than the fixture carrying
    one, because two arms here assert the scene's exact decal count and four
    mutations index decals[0]: the roads rule, and load-bearing rather than
    stylistic. What it does NOT assert is the colour space of that blend -- decal
    over decal composites in stored codes where decal over substrate composites in
    linear, which is stated as a choice in decals_ubo.glsl and asserted here as it
    stands.

    decals-opacity exists because every other colour arm reads an OPAQUE interior,
    where the premultiplied accumulation and the naive one agree exactly -- which
    is how a real double-multiply survived the first mutation round with all six
    arms green. Anything that reads only a == 1 is blind to how a mark meets the
    surface under it, which is most of what a decal is.

    decals-orient exists for the same class of reason and cost more: every image
    in this fixture used to be invariant under mirror, flip, transpose AND
    180-degree rotation, so a projector laying every picture on backwards and
    upside down -- which is what shipped -- painted a frame no arm could tell
    from a correct one. A single-colour patch cannot test a projector. The
    quadrants are what make the fixture able to fail.

    ONE DELIBERATE ABSENCE: no arm covers the half-texel UV inset that guards
    against the material array's GL_REPEAT wrap. One was written and removed on
    measurement -- deleting the inset moves 0 px on this fixture, because its
    images carry a transparent margin and a wrapped blend between two
    transparent texels is still transparent. Making it observable needs an image
    opaque to its own border, where the affected band is half a texel: at the
    canonical 64-texel array size over a two-unit box that is 0.016 world units,
    which is sub-pixel at this framing. The inset stays because it is correct and
    free; an arm that cannot fail would be worse than the gap it papers over.

    Every read is a BYTE through --render-mode 6, the layer fixture's rule: the
    albedo view returns linearToSRGB(factor * sRGBToLinear(blend)), so a correct
    renderer hands back the exact code the generator painted and the assertion
    argues about nothing else -- not the BRDF, not exposure, not the tonemap.
    """
    if not os.path.exists(asset(DECAL_FIXTURE)):
        print(f"  decals-identity SKIP  {DECAL_FIXTURE} not found")
        return []
    gen = _decal_gen()
    if gen is None:
        return []
    failures = []
    cam = _cscn_camera(DECAL_FIXTURE)
    wall_substrate = _decal_substrate(gen, "decal_wall_mat")
    oblique_substrate = _decal_substrate(gen, "decal_oblique_mat")
    floor_substrate = _decal_substrate(gen, "decal_floor_mat")

    # -- identity: the feature costs a decal-free scene nothing ---------------
    def strip(d):
        d.pop("decals", None)

    pix_off, w, h, out_off = _decal_run(workdir, "off", strip, ["--render-mode", "6"])
    pix_flag, _, _, out_flag = _decal_run(workdir, "flag", None,
                                          ["--render-mode", "6", "--no-decals"])
    pix_on, _, _, out_on = _decal_run(workdir, "on", None, ["--render-mode", "6"])
    if pix_off is None or pix_flag is None or pix_on is None:
        failures.append("decals-identity")
        print(f"  decals-identity FAIL  render error\n{out_off or out_flag or out_on}")
    else:
        off = os.path.join(workdir, "decal_off.ppm")
        flag = os.path.join(workdir, "decal_flag.ppm")
        on = os.path.join(workdir, "decal_on.ppm")
        same, _ = compare(off, flag)
        moved, _ = compare(off, on)
        # The anti-vacuity: the decals have to have DONE something, or the
        # identity above is between two frames that never had a decal in them.
        ok = same == 0 and moved > 500
        if not ok:
            failures.append("decals-identity")
        print(f"  decals-identity {'PASS' if ok else 'FAIL'}  --no-decals is the "
              f"stripped scene at {same} px (want 0), while the decals themselves "
              f"move {moved} px (want > 500)")

    project = _projector(cam, w or 400, h or 300)

    # -- albedo: the painted codes, and clear substrate as the control --------
    if pix_on is not None:
        poster = _decal_byte(pix_on, w, h, project, gen.POSTER_READ)
        wall = _decal_byte(pix_on, w, h, project, gen.WALL_CLEAR)
        # The scorch too: it is the decal whose frame comes from the canonical
        # perpendicular fallback and the only one carrying a surface map, so it
        # is the one most likely to be wrong -- and its colour went unasserted
        # while both the suite and docs/verification.md quoted it as a reading.
        scorch = _decal_byte(pix_on, w, h, project, gen.SCORCH_READ)
        floor = _decal_byte(pix_on, w, h, project, gen.FLOOR_CLEAR)
        ok = (_decal_near(poster, gen.POSTER_TR) and _decal_near(wall, wall_substrate) and
              _decal_near(scorch, gen.SCORCH_CODE) and _decal_near(floor, floor_substrate))
        if not ok:
            failures.append("decals-albedo")
        print(f"  decals-albedo {'PASS' if ok else 'FAIL'}  poster reads {poster} "
              f"(want {gen.POSTER_TR}), scorch {scorch} (want {gen.SCORCH_CODE}); "
              f"controls: wall {wall} (want {wall_substrate}), floor {floor} "
              f"(want {floor_substrate}), all +/-{DECAL_CODE_EPS}")

    # -- orient: the picture arrives the way round it was painted -------------
    if pix_on is not None:
        # Which PNG quadrant each world corner must return. A projector that
        # mirrors, flips, rotates or transposes gets a DIFFERENT wrong answer
        # for each, because the generator asserts the four codes are separated
        # in every channel.
        want = {"upper_left": gen.POSTER_TL, "upper_right": gen.POSTER_TR,
                "lower_left": gen.POSTER_BL, "lower_right": gen.POSTER_BR}
        got = {k: _decal_byte(pix_on, w, h, project, v) for k, v in gen.QUAD_READS.items()}
        wrong = [k for k in want if not _decal_near(got[k], want[k])]
        # Anti-vacuity: the four reads must be four DIFFERENT bytes, or they are
        # all landing on one quadrant and agreeing with it by accident.
        distinct = len({tuple(v) for v in got.values()}) == 4
        ok = not wrong and distinct
        if not ok:
            failures.append("decals-orient")
        print(f"  decals-orient {'PASS' if ok else 'FAIL'}  four quadrants land in their "
              f"own corners ({len(want) - len(wrong)}/4 correct, {len(set(map(tuple, got.values())))} "
              f"distinct of 4)" +
              (f"; wrong: {', '.join(f'{k}={got[k]} want {want[k]}' for k in wrong)}"
               if wrong else ""))

    # -- angle fade: the oblique plate refuses the mark -----------------------
    if pix_on is not None:
        ob = _decal_byte(pix_on, w, h, project, gen.OBLIQUE_READ)
        ok = _decal_near(ob, oblique_substrate)
        if not ok:
            failures.append("decals-angle-fade")
        print(f"  decals-angle-fade {'PASS' if ok else 'FAIL'}  the oblique plate "
              f"inside the poster's box reads {ob} (want its own substrate "
              f"{oblique_substrate} +/-{DECAL_CODE_EPS}, i.e. no mark)")

    # -- surface: the scorch's map moves the lit frame ------------------------
    # renderMode 8 is vec4(metallicMap, roughnessMap, 0, 1), and its return sits
    # AFTER the wet-sand seam where the surface half lands -- so the G channel is
    # the decal's roughness as the BRDF will see it, read as a byte rather than
    # inferred from a pixel count.
    def drop_surface(d):
        for dec in d.get("decals", []):
            dec.pop("surface", None)

    def flat_normal(d):
        # The surface map STAYS; only its relief is switched off. That is what
        # separates the normal half from the roughness half -- popping the map,
        # which is what this arm used to do, drops both and cannot tell them
        # apart while claiming in its own message to be testing the normal.
        for dec in d.get("decals", []):
            dec["normalStrength"] = 0.0

    pix_r, _, _, out_r = _decal_run(workdir, "rough", None, ["--render-mode", "8"])
    pix_rn, _, _, out_rn = _decal_run(workdir, "roughnone", drop_surface,
                                      ["--render-mode", "8"])
    if pix_r is None or pix_rn is None:
        failures.append("decals-surface")
        print(f"  decals-surface FAIL  render error\n{out_r or out_rn}")
    else:
        got = _decal_byte(pix_r, w, h, project, gen.SCORCH_READ)[1]
        bare = _decal_byte(pix_rn, w, h, project, gen.SCORCH_READ)[1]
        ctrl = _decal_byte(pix_r, w, h, project, gen.FLOOR_CLEAR)[1]
        ctrl_bare = _decal_byte(pix_rn, w, h, project, gen.FLOOR_CLEAR)[1]
        # renderMode 8 writes roughness raw, no encode -- so the painted code IS
        # the expected byte, give or take the 0.04 floor and rounding.
        want = gen.SCORCH_ROUGH
        ok = (abs(got - want) <= 4 and abs(got - bare) > 40 and ctrl == ctrl_bare)
        if not ok:
            failures.append("decals-surface")
        print(f"  decals-surface {'PASS' if ok else 'FAIL'}  the scorch's roughness reads "
              f"{got} (want its painted {want} +/-4) against {bare} with no surface map "
              f"(want > 40 apart), and the floor control is {ctrl} either way "
              f"(unmoved: {ctrl == ctrl_bare})")

    # -- surface-normal: the relief, isolated from the roughness --------------
    lit_full, _, _, out_lit = _decal_run(workdir, "lit", None, None)
    lit_flat, _, _, out_flat = _decal_run(workdir, "flat", flat_normal, None)
    if lit_full is None or lit_flat is None:
        failures.append("decals-normal")
        print(f"  decals-normal FAIL  render error\n{out_lit or out_flat}")
    else:
        full = os.path.join(workdir, "decal_lit.ppm")
        flat = os.path.join(workdir, "decal_flat.ppm")
        moved, _ = compare(full, flat)
        ok = moved > 200
        if not ok:
            failures.append("decals-normal")
        print(f"  decals-normal {'PASS' if ok else 'FAIL'}  flattening the scorch's relief "
              f"alone -- its surface map still bound -- moves {moved} px of the lit frame "
              f"(want > 200; a build that never composes the decal normal reads 0)")

    # -- opacity: the premultiply, which every opaque read is blind to --------
    def half(d):
        d["decals"][0]["opacity"] = 0.5

    pix_half, _, _, out_half = _decal_run(workdir, "half", half, ["--render-mode", "6"])
    if pix_half is None:
        failures.append("decals-opacity")
        print(f"  decals-opacity FAIL  render error\n{out_half}")
    else:
        got = _decal_byte(pix_half, w, h, project, gen.POSTER_READ)
        # The albedo view encodes with pow(1/2.2), and the blend happens in
        # LINEAR -- so the expected byte is the encode of the half-and-half of
        # the two decoded values, not the average of the two bytes.
        want = tuple(
            int(round((((c / 255.0) ** 2.2 + (s_ / 255.0) ** 2.2) * 0.5) ** (1 / 2.2) * 255.0))
            for c, s_ in zip(gen.POSTER_TR, wall_substrate))
        ok = _decal_near(got, want, eps=4)
        if not ok:
            failures.append("decals-opacity")
        print(f"  decals-opacity {'PASS' if ok else 'FAIL'}  a half-opaque poster reads "
              f"{got} (want {want} +/-4, the linear midpoint of {gen.POSTER_TR} and "
              f"{wall_substrate}); multiplying by coverage twice reads darker")

    # -- feather: the edge ramp, which was inert until the fixture was fixed --
    if pix_on is not None:
        got = _decal_byte(pix_on, w, h, project, gen.FEATHER_READ)
        # SOLVED for, not compared byte to byte. The read point lands within a
        # pixel of where it is asked for, and a pixel here is 0.03 of coverage --
        # so a byte tolerance loose enough to survive that is loose enough to
        # miss a real error. Inverting the blend states what the arm means: the
        # coverage the geometry owes is what came back.
        got_c = []
        for k, (m, s_) in enumerate(zip(gen.POSTER_BR, wall_substrate)):
            mark_lin, wall_lin = (m / 255.0) ** 2.2, (s_ / 255.0) ** 2.2
            if abs(wall_lin - mark_lin) < 0.1:
                continue  # this channel cannot resolve a coverage
            got_lin = (got[k] / 255.0) ** 2.2
            got_c.append((wall_lin - got_lin) / (wall_lin - mark_lin))
        measured = sum(got_c) / len(got_c) if got_c else 0.0
        want_c = gen.FEATHER_COVERAGE
        # +/- 0.06 is two pixels of the read's own position. A build with no
        # ramp reads 1.0 and one with no decal reads 0.0, both far outside it --
        # which is the anti-vacuity, and it is arithmetic rather than a bar.
        ok = abs(measured - want_c) <= 0.06 and len(got_c) >= 2
        if not ok:
            failures.append("decals-feather")
        print(f"  decals-feather {'PASS' if ok else 'FAIL'}  inside the edge ramp reads "
              f"{got}, i.e. coverage {measured:.3f} over {len(got_c)} channels (want "
              f"{want_c:.3f} +/-0.06 from the box geometry; no ramp reads 1.000, no "
              f"decal reads 0.000)")

    # -- overlap: a mark over a mark, and which one wins ----------------------
    def overlap(d):
        # APPENDED, which is what makes it the later paint: decalAccumulate walks
        # ascending and composites over, so the generator asserts this slot is
        # past the poster's rather than the arm assuming it.
        d["decals"].append({
            "position": list(gen.OVERLAP_POS),
            "size": list(gen.OVERLAP_HALF),
            "direction": [0.0, 0.0, -1.0],
            "image": gen.OVERLAP_IMAGE,
            "opacity": 1.0,
            "angleFade": gen.ANGLE_FADE,
            "feather": gen.FEATHER,
        })

    pix_ov, _, _, out_ov = _decal_run(workdir, "overlap", overlap, ["--render-mode", "6"])
    if pix_ov is None:
        failures.append("decals-overlap")
        print(f"  decals-overlap FAIL  render error\n{out_ov}")
    else:
        over = _decal_byte(pix_ov, w, h, project, gen.OVERLAP_READ)
        under = _decal_byte(pix_ov, w, h, project, gen.OVERLAP_CONTROL)
        # The control is in the SAME poster quadrant, which is the anti-vacuity:
        # a variant whose third mark covered the whole poster reads the overlap's
        # code at both points and passes half of this on its own.
        ok = _decal_near(over, gen.SCORCH_CODE) and _decal_near(under, gen.POSTER_BL)
        if not ok:
            failures.append("decals-overlap")
        print(f"  decals-overlap {'PASS' if ok else 'FAIL'}  the later mark reads {over} "
              f"(want {gen.SCORCH_CODE}) where it covers the poster, and the poster still "
              f"reads {under} (want {gen.POSTER_BL}) beside it, +/-{DECAL_CODE_EPS}; "
              f"reversing the composite reads {gen.POSTER_BL} at both")

    # -- mask: determinism, and a decal off screen claims nothing -------------
    _, _, _, out_p1 = _decal_run(workdir, "probe1", None, ["--decal-probe", "1"])
    _, _, _, out_p2 = _decal_run(workdir, "probe2", None, ["--decal-probe", "1"])

    def behind(d):
        # Both decals moved behind the camera, which looks down -Z from z=5.5.
        for dec in d.get("decals", []):
            dec["position"] = [dec["position"][0], dec["position"][1], 40.0]

    pix_behind, _, _, out_behind = _decal_run(workdir, "behind", behind,
                                              ["--render-mode", "6", "--decal-probe", "1"])
    f1, d1 = _decal_probe_rows(out_p1 or "")
    f2, _ = _decal_probe_rows(out_p2 or "")
    fb, _ = _decal_probe_rows(out_behind or "")
    if not f1 or not f2 or not fb or pix_behind is None:
        failures.append("decals-mask")
        print(f"  decals-mask FAIL  no probe rows\n{(out_p1 or '')[:400]}")
    else:
        digest_a, digest_b = f1[-1]["digest"], f2[-1]["digest"]
        bits_live = int(f1[-1]["mask_bits"])
        bits_behind = int(fb[-1]["mask_bits"])
        behind_ppm = os.path.join(workdir, "decal_behind.ppm")
        off = os.path.join(workdir, "decal_off.ppm")
        px_behind, _ = compare(off, behind_ppm)
        ok = (digest_a == digest_b and 0 < bits_live < DECAL_GRID_CELLS and
              bits_behind == 0 and px_behind == 0 and int(f1[-1]["live"]) == 2)
        if not ok:
            failures.append("decals-mask")
        print(f"  decals-mask {'PASS' if ok else 'FAIL'}  digest {digest_a} == "
              f"{digest_b} across two runs, {bits_live} froxel bits live "
              f"(want 0 < n < {DECAL_GRID_CELLS}; marking every cell reads "
              f"{2 * DECAL_GRID_CELLS}), and behind the camera {bits_behind} bits "
              f"(want 0) rendering {px_behind} px from the decal-free frame (want 0)")

    # -- schema: the four required keys, refused BY NAME ----------------------
    def drop_position(d):
        d["decals"][0].pop("position", None)

    def drop_image(d):
        d["decals"][0].pop("image", None)

    def zero_size(d):
        d["decals"][0]["size"] = [1.0, 0.0, 1.0]

    _, _, _, out_nopos = _decal_run(workdir, "nopos", drop_position, ["--decal-probe", "1"])
    _, _, _, out_noimg = _decal_run(workdir, "noimg", drop_image, ["--decal-probe", "1"])
    _, _, _, out_zero = _decal_run(workdir, "zerosize", zero_size, ["--decal-probe", "1"])
    _, clean = _decal_probe_rows(out_p1 or "")
    named = [
        ("position", out_nopos, "needs position, size and direction"),
        ("image", out_noimg, "needs an image"),
        ("size", out_zero, "size must be positive"),
    ]
    misses = [k for k, o, want in named if want not in (o or "")]
    # The unmutated twin must load silently, or "a warning appeared" says nothing.
    clean_quiet = "decal needs" not in (out_p1 or "")
    ok = not misses and clean_quiet and len(clean) == 2
    if not ok:
        failures.append("decals-schema")
    print(f"  decals-schema {'PASS' if ok else 'FAIL'}  refused by name: "
          f"{len(named) - len(misses)}/{len(named)}"
          f"{' (missing ' + ','.join(misses) + ')' if misses else ''}, "
          f"and the unmutated fixture loads with no refusal and {len(clean)} decals")

    # -- capture: decals reach a reflection probe, which is the design claim ---
    #
    # The whole texture story rests on this. A unit-6 alias -- the plan the
    # roadmap booked -- is bound only for `SUBMIT_PASS_SHADE && !alpha_pass &&
    # !capturing`, so it would have had decals absent from probe captures; the
    # materialArray tenancy has no such exclusion. Nothing asserted the
    # difference, and the arm was specced and never written.
    def with_probe(d, drop_decals=False):
        # A mirror floor and a probe that can see the wall. The probe is what
        # carries the poster into the reflection; the floor is what shows it.
        d["probes"] = [{"position": [0.0, 1.2, 0.0],
                        "boxMin": [-3.0, -0.1, -3.1],
                        "boxMax": [3.0, 4.0, 3.1],
                        "boxFade": 0.05}]
        d["materials"] = {"decal_floor_mat": {"roughness": 0.02, "metallic": 1.0}}
        if drop_decals:
            d.pop("decals", None)

    pix_cap, _, _, out_cap = _decal_run(workdir, "cap", with_probe, PROBE_SKY, frames=8)
    pix_capless, _, _, out_capless = _decal_run(
        workdir, "capless", lambda d: with_probe(d, True), PROBE_SKY, frames=8)
    if pix_cap is None or pix_capless is None:
        failures.append("decals-capture")
        print(f"  decals-capture FAIL  render error\n{(out_cap or out_capless)[:400]}")
    else:
        # The reflection, and a floor control the wall cannot reach into.
        refl = _decal_byte(pix_cap, w, h, project, gen.MIRROR_READ)
        refl_bare = _decal_byte(pix_capless, w, h, project, gen.MIRROR_READ)
        moved = sum(abs(int(a) - int(b)) for a, b in zip(refl, refl_bare))
        # Anti-vacuity: a probe has to have been captured at all, or the two
        # frames are two mirrors of the same empty wall and agree for a reason
        # that has nothing to do with decals.
        captured = "probe" in (out_cap or "").lower()
        ok = moved > 12 and captured
        if not ok:
            failures.append("decals-capture")
        print(f"  decals-capture {'PASS' if ok else 'FAIL'}  the poster's reflection in a "
              f"mirror floor reads {refl} with the decal and {refl_bare} without "
              f"(sum |delta| {moved}, want > 12); a probe was captured: {captured}")

    # -- config: the snapshot rows, which the config group's fixture cannot see -
    #
    # CONFIG_FIXTURE is cornell_rooms and it authors no decal, so _decal_at
    # returns NULL at every index and the whole "decals" section is empty in
    # every arm over there -- config-perturb included, which is the one arm that
    # can see a row whose apply silently does nothing. And config-coverage
    # matches on the TRAILING member name, so `position`, `opacity`, `enabled`
    # and the rest are all already satisfied by Light and Probe rows: the nine
    # decal rows would pass that census if they were deleted outright.
    # Dumped from a session already in the state the reference frames were taken
    # in -- albedo view, exposure pinned. The snapshot OWNS THE LOOK and applies
    # after the CLI, so a dump taken in another mode would restore that mode and
    # overrule the flag, and the comparison would be against a different picture.
    _, dump, _ = _config_run(workdir, "decals",
                             ["-t", os.path.join(ROOT, "assets", "textures"), "--render-mode", "6",
                              "--no-auto-exposure", "-E", "1.0"],
                             model=DECAL_FIXTURE, frames=4)
    carried, restored_px, keys = [], None, 0
    if os.path.exists(dump):
        with open(dump) as fh:
            snap = json.load(fh)
        rows = snap.get("decals") or []
        keys = len(rows[0]) if rows else 0
        carried = sorted(rows[0].keys()) if rows else []
        # Perturb through the SNAPSHOT rather than the .cscn: that is the path
        # under test, and opacity 0 is a change every pixel of the mark can see.
        for r in rows:
            r["opacity"] = 0.0
        edited = os.path.join(workdir, "config_decals_edited.json")
        with open(edited, "w") as fh:
            json.dump(snap, fh)
        shot = os.path.join(workdir, "config_decals_restored.ppm")
        # No look flags here on purpose: the snapshot carries them, which is the
        # claim -- "give somebody the JSON and they see your pixels".
        subprocess.run([RENDER, "--config", edited, "-t", os.path.join(ROOT, "assets", "textures"),
                        "-x", "-f", "4", "-W", "400", "-H", "300", "-S", shot],
                       capture_output=True, text=True)
        on = os.path.join(workdir, "decal_on.ppm")
        off = os.path.join(workdir, "decal_off.ppm")
        if os.path.exists(shot):
            moved, _ = compare(on, shot)
            same_as_off, _ = compare(off, shot)
            restored_px = (moved, same_as_off)
    # Nine rows, and the restore must land: opacity 0 takes the frame all the way
    # back to the decal-free one, which no half-applied restore reaches.
    ok = (keys >= 9 and restored_px is not None and restored_px[0] > 500 and
          restored_px[1] == 0)
    if not ok:
        failures.append("decals-config")
    print(f"  decals-config {'PASS' if ok else 'FAIL'}  the dump carries {keys} decal rows "
          f"({', '.join(carried) if carried else 'none'}); restoring one with opacity 0 "
          f"moves {restored_px[0] if restored_px else '?'} px from the marked frame (want "
          f"> 500) and lands {restored_px[1] if restored_px else '?'} px from the "
          f"decal-free one (want 0)")

    return failures
