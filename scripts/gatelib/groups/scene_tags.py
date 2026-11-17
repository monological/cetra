"""The scene-tags group: tags in 3D (spec 14.39) -- an object and a class on each surface, a table
the views borrow and each view's class rows and fade, which hide, paint, fade and outline them.
"""

import functools
import os

import numpy as np

from gatelib.arms import Arms
from gatelib.groups.views import _differ, _draws, _linear, _view_probe, _views_run
from gatelib.harness import _import_fixture_gen, asset
from gatelib.scenes import _cscn_camera, _projector

TAGS_W, TAGS_H = 400, 300
# The fixture's top, on which the strips and parts stand.
TAGS_SLAB_TOP = 0.1

# The tags every arm gives the fixture: the strips 1 to 4 by piece, the parts 10 to 13 of class 3
# by node, the slab 20 of class 1. The cap, part0's child, takes part0's.
TAGS_FLAGS = ["--piece-tags", "strips=1", "--node-tag", "slab=20:1"]
for _k in range(4):
    TAGS_FLAGS += ["--node-tag", f"part{_k}={10 + _k}:3"]


def _tag_colour(tag):
    """The tags render mode's colour for a tag, as codes: pbr_frag's hash, written as it is."""
    if tag == 0:
        return (0, 0, 0)
    h = (tag * 2654435761) & 0xFFFFFFFF
    return tuple(round(((h >> s) & 255) / 255.0 * 0.75 * 255 + 0.25 * 255) for s in (24, 16, 8))


def _code_at(img, project, point):
    x, y = project(point)
    return tuple(int(c) for c in img[int(round(y)), int(round(x))])


def _radiance_at(img, project, point):
    """The pixel at `point` under the linear curve: its radiance at the pinned exposure."""
    return _linear(np.array([[_code_at(img, project, point)]]))[0, 0]


def _drawn_from(said, view, frame):
    """The frames from `frame` on that `view` drew, off --view-probe."""
    rows, _ = _view_probe(said)
    return sorted(f for f, row in rows.get(view, {}).items() if row[0] and f >= frame)


def run_scene_tags_gate(workdir):
    """Tags in 3D (spec 14.39), on tags_fixture through apps/render: four strips in one mesh tagged
    1 to 4 by piece, four parts of one mesh, drawn as one instanced run, tagged 10 to 13 by node,
    a cap under part0 that takes its tag, and the slab tagged 20.

      scene-tag-off        tags that change nothing draw nothing changed: the fixture with a
                           table, node tags, piece tags and an empty class row is the fixture
                           without them, 0 px, through the tags variant of every program.
      scene-tag-reach      the tags render mode: each strip, each part and the slab its own
                           tag's colour, exactly, the cap its parent's, the untagged floor black.
      scene-tag-instanced  the same frame with no instanced draws, each part from its node's
                           uniform: 0 px from the instanced one.
      scene-tag-node       a node's tag written at frame 40, after a still view has finished its
                           average and rested: the view draws again from that frame; written to
                           the value it holds, it rests.
      scene-tag-hidden     part1's tag hidden is the fixture without part1, its shadow included,
                           0 px; the slab's class hidden is the fixture without the slab, 0 px.
      scene-tag-paint      a metal strip and the dielectric slab under one red paint read within 3
                           codes of each other, a matte coat on both; a part painted wholly flat is
                           one colour on its lit top and its shaded front, which differ unpainted;
                           a tag dimmed all the way is black. Ambient occlusion off for the first
                           two, since it darkens a surface by its neighbour whatever it is.
      scene-tag-order      strip 3's own green over its class's red, the strip beside it red; a
                           tag shown in a hidden class is hidden.
      scene-tag-limit      tags 0 and 2^22 refused by name; tag 300, past the table's first 256,
                           hides part1 as tag 11 does, 0 px.
      scene-tag-fade       fade 1 with part2 lit is part2 alone, every other object hidden by its
                           tags, the floor untagged in the fading run, 0 px; fade 0.5 with the slab
                           and the floor lit draws
                           part3's top, in radiance, within 4% of the mean of the part solid and the
                           slab behind it, and the slab beside it as it is unfaded, 0 px. Under the
                           weighted OIT: the moments' estimate darkens a lone layer by about 10%.
      scene-tag-ghost-off  a view that paints but ghosts nothing runs no OIT pass in a scene with no
                           translucent mesh; the same view fading runs one.
      scene-tag-shadow     part3 in a class half see-through: the slab where its shadow fell is the
                           slab with part3 removed, exactly, at every point checked.
      scene-tag-capture    two views of the rooms under a GI volume, the left one hiding the red
                           wall and a ceiling panel by its tags, the volume dirtied at frame 10 so
                           it captures again inside the left view's draw: the right is its view
                           alone without tags, 0 px -- the captures see the wall's bounce -- and
                           the left differs.
      scene-tag-outline    part0 outlined: a ring outside it 2 to 4 pixels wide (1.5 points at 2
                           pixels a point), the outline's colour exactly where it is whole; looking
                           down the row of parts, part2 outlined behind part3 rings at the view's
                           hidden strength (0.35, within 0.05) over part3's face, and part3
                           outlined rings at 1.
      scene-tag-outline-still  an outline written at frame 40 onto a view that rested at 17 draws
                           no scene frame after it, and the last frame is the one where the outline
                           was there from the start, 0 px.
      scene-tag-restart    two resting views, the left borrowing the table and the right none: a
                           row written at frame 40 draws the left again from 40 and not the right.
      scene-tag-shared     two resting views borrowing one table: a row written at frame 40 draws
                           both again, and both show it.

    Falsified by hand at 14.39: the instance block's tag left unfilled, the instanced parts read
    whatever the block last held -- part0 and part1 black, part2 and part3 another object's colour
    -- and instanced is 27087 px off; the walk's change test removed, the rested view never draws
    again after frame 40; the paint's metal and gloss left on, the strip reads (139, 3, 3), red
    metal, against the slab's (225, 10, 10).

    scene-tag-capture holds the outcome, not one guard. A capture binds no tags as every pass binds
    its view's, where the pass begins, a capture's view having none: there is no one line to take
    out. Its programs resolved with the tags bit as well, it still passes, since its view's block
    hides nothing; and the GI captured again from frame 10 reaches the other view too little to
    move a code even where a hidden wall would cast no shadow in it.
    """
    arm = Arms(23)
    scene = asset("tags_fixture.cscn")
    gen = _import_fixture_gen("gen_tags_fixture.py", "scene-tags") if os.path.exists(scene) \
        else None
    if gen is not None:
        # Where the fixture's pieces stand, read off its generator and its scene file.
        cam = _cscn_camera("tags_fixture.cscn")
        strip_xs, part_xs = gen.STRIP_XS, gen.PART_XS
        strip_top, strip_z = gen.STRIP_Y + gen.STRIP_HALF[1], gen.STRIP_Z
        part_top, part_z = TAGS_SLAB_TOP + gen.PART_SIZE, gen.PART_Z
        cap_top = gen.CAP_CENTRE[1] + gen.CAP_SIZE[1] / 2.0

    def needs_fixture(body):
        """An arm on tags_fixture, skipped when the fixture or its generator is not there."""
        @functools.wraps(body)
        def guarded():
            return body() if gen is not None else (None, "no tags_fixture or its generator")
        return guarded

    def run(tag, extra, frames=10):
        return _views_run(workdir, f"tags_{tag}", scene, TAGS_W, TAGS_H, extra, frames)

    still = ["--views", "1x1", "--view-turn", "0", "--accumulate", "16", "--view-probe"]
    still2 = ["--views", "2x1", "--view-turn", "0", "--accumulate", "16", "--view-probe"]

    @functools.cache
    def reach():
        return run("reach", TAGS_FLAGS + ["--render-mode", "15"], 3)

    @arm("scene-tag-off")
    @needs_fixture
    def _():
        plain, said = run("plain", [])
        inert, said2 = run("inert", ["--tag-table", "--tag-class", "2=none"] + TAGS_FLAGS)
        if plain is None or inert is None:
            return False, f"a run failed: {said if plain is None else said2}"
        diff = _differ(plain, inert)
        return diff == 0, f"{diff} px between the fixture and its tags that change nothing (want 0)"

    @arm("scene-tag-reach")
    @needs_fixture
    def _():
        img, said = reach()
        if img is None:
            return False, f"the run failed: {said}"
        project = _projector(cam, img.shape[1], img.shape[0])
        checks = [(f"strip{k + 1}", (x, strip_top, strip_z), k + 1)
                  for k, x in enumerate(strip_xs)]
        checks += [(f"part{k}", (x, part_top, part_z + 0.04), 10 + k)
                   for k, x in enumerate(part_xs)]
        checks += [("cap", (part_xs[0], cap_top, part_z), 10),
                   ("slab", (0.5, 0.1, -0.4), 20), ("floor", (0.75, 0.0, 0.0), 0)]
        # Within one code: a quarter plus three quarters of a byte lands on a half for one byte in
        # four, which the GPU's conversion and Python's round settle apart.
        wrong = []
        for name, point, tag in checks:
            got, want = _code_at(img, project, point), _tag_colour(tag)
            if max(abs(g - w) for g, w in zip(got, want)) > 1:
                wrong.append(f"{name} {got} want {want}")
        return not wrong, (f"{len(checks)} surfaces each their tag's colour, within a code"
                           if not wrong else "; ".join(wrong))

    @arm("scene-tag-instanced")
    @needs_fixture
    def _():
        img, said = reach()
        single, said2 = run("single", TAGS_FLAGS + ["--render-mode", "15", "--no-instancing"], 3)
        if img is None or single is None:
            return False, f"a run failed: {said if img is None else said2}"
        diff = _differ(img, single)
        return diff == 0, f"{diff} px between instanced and one draw an object (want 0)"

    @arm("scene-tag-node")
    @needs_fixture
    def _():
        _, moved = run("node_moved", TAGS_FLAGS + still + ["--node-tag", "40:part1=55:3"], 60)
        _, same = run("node_same", TAGS_FLAGS + still + ["--node-tag", "40:part1=11:3"], 60)
        drawn_moved, drawn_same = _drawn_from(moved, 0, 40), _drawn_from(same, 0, 40)
        rested = _draws(_view_probe(same)[0], 0)
        ok = bool(drawn_moved) and drawn_moved[0] <= 41 and not drawn_same and rested is not None
        return ok, (f"a new tag at frame 40 draws the rested view again from frame "
                    f"{drawn_moved[0] if drawn_moved else None} (want 40 or 41); the same tag "
                    f"written draws {len(drawn_same)} frames after it (want 0)")

    @functools.cache
    def without(name):
        return run(f"without_{name}", ["--remove-node", name])

    @arm("scene-tag-hidden")
    @needs_fixture
    def _():
        tag_hidden, said = run("hidden_tag", TAGS_FLAGS + ["--tag", "11=hidden"])
        class_hidden, said2 = run("hidden_class", TAGS_FLAGS + ["--tag-class", "1=hidden"])
        no_part, _ = without("part1")
        no_slab, _ = without("slab")
        if any(x is None for x in (tag_hidden, class_hidden, no_part, no_slab)):
            return False, f"a run failed: {said if tag_hidden is None else said2}"
        d_tag, d_class = _differ(tag_hidden, no_part), _differ(class_hidden, no_slab)
        return d_tag == 0 and d_class == 0, (
            f"part1's tag hidden {d_tag} px from part1 removed, the slab's class hidden {d_class} "
            f"px from the slab removed (want 0, 0)")

    @arm("scene-tag-paint")
    @needs_fixture
    def _():
        plain, _ = run("paint_plain", TAGS_FLAGS + ["--node-tag", "strips=0:2"])
        # The strips and the slab in one paint: a matte coat reads alike on the metal and the
        # dielectric, which unpainted look nothing alike. This and the flat run with ambient
        # occlusion off: it is screen space, laid over the finished pixel, and darkens the slab by
        # the strips and the part's front by the floor whatever the surfaces are.
        red, _ = run("paint_red", TAGS_FLAGS + ["--no-ssao", "--node-tag", "strips=0:2",
                                                "--tag-class", "2=colour=FF2020FF",
                                                "--tag-class", "1=colour=FF2020FF"])
        lit, _ = run("paint_lit", TAGS_FLAGS + ["--no-ssao"])
        flat, _ = run("paint_flat", TAGS_FLAGS + ["--no-ssao", "--tag-class",
                                                  "3=colour=4080C0FF,flat=1"])
        dim, _ = run("paint_dim", TAGS_FLAGS + ["--tag", "12=dim=1"])
        if any(x is None for x in (plain, red, lit, flat, dim)):
            return False, "a run failed"
        project = _projector(cam, plain.shape[1], plain.shape[0])
        strip = (strip_xs[1], strip_top, strip_z)
        slab_top = (0.0, 0.1, -0.4)
        copper, painted = _code_at(plain, project, strip), _code_at(red, project, strip)
        slab_painted = _code_at(red, project, slab_top)
        reds = max(abs(a - b) for a, b in zip(painted, slab_painted)) <= 3
        top = (part_xs[3], part_top, part_z)
        front = (part_xs[3], 0.16, part_z + 0.06)
        lit_top, lit_front = _code_at(lit, project, top), _code_at(lit, project, front)
        flat_top, flat_front = _code_at(flat, project, top), _code_at(flat, project, front)
        shaded = max(abs(a - b) for a, b in zip(lit_top, lit_front))
        evened = max(abs(a - b) for a, b in zip(flat_top, flat_front))
        black = _code_at(dim, project, (part_xs[2], part_top, part_z))
        ok = reds and shaded > 20 and evened <= 1 and max(black) <= 2
        return ok, (f"the strip {copper} unpainted, {painted} under red, the slab in the same red "
                    f"{slab_painted} (want within 3 codes); part3's top "
                    f"and front {shaded} codes apart unpainted (want > 20), {evened} painted flat "
                    f"(want <= 1); part2 dimmed all the way {black} (want black)")

    @arm("scene-tag-order")
    @needs_fixture
    def _():
        img, _ = run("order", TAGS_FLAGS + ["--node-tag", "strips=0:2", "--tag-class",
                                            "2=colour=FF2020FF", "--tag", "3=colour=20C040FF",
                                            "--tag-class", "1=hidden", "--tag", "20=colour=FFFFFFFF"])
        no_slab, _ = without("slab")
        if img is None or no_slab is None:
            return False, "a run failed"
        project = _projector(cam, img.shape[1], img.shape[0])
        own = _code_at(img, project, (strip_xs[2], strip_top, strip_z))
        beside = _code_at(img, project, (strip_xs[1], strip_top, strip_z))
        slab_at = (0.5, 0.1, -0.4)
        slab, bare = _code_at(img, project, slab_at), _code_at(no_slab, project, slab_at)
        ok = own[1] > 2 * max(own[0], own[2]) and beside[0] > 3 * max(beside[1], beside[2]) and \
            slab == bare
        return ok, (f"strip 3 {own} (want green), strip 2 {beside} (want red); the slab, tagged "
                    f"white in a hidden class, {slab} where the floor shows {bare} (want equal)")

    @arm("scene-tag-limit")
    @needs_fixture
    def _():
        far = TAGS_FLAGS + ["--node-tag", "part1=300:3", "--tag", "300=hidden", "--tag",
                            "0=hidden", "--tag", f"{1 << 22}=hidden"]
        img, said = run("limit", far)
        no_part, _ = without("part1")
        if img is None or no_part is None:
            return False, f"a run failed: {said}"
        refused = [t for t in (0, 1 << 22) if f"tag {t} is refused" in said]
        diff = _differ(img, no_part)
        return diff == 0 and len(refused) == 2, (
            f"tags refused by name: {refused} (want [0, {1 << 22}]); tag 300 hidden {diff} px from "
            f"part1 removed (want 0)")

    @arm("scene-tag-fade")
    @needs_fixture
    def _():
        gone, said = run("fade_gone", TAGS_FLAGS + ["--tag", "12=lit", "--tag-fade", "1"])
        # The reference hides every other object by its tags: the floor, untagged in the fading
        # run, given one here.
        hide = ["--node-tag", "floor=30"]
        for t in (1, 2, 3, 4, 10, 11, 13, 20, 30):
            hide += ["--tag", f"{t}=hidden"]
        alone, said2 = run("fade_alone", TAGS_FLAGS + hide)
        if gone is None or alone is None:
            return False, f"a run failed: {said if gone is None else said2}"
        d_gone = _differ(gone, alone)
        # Radiance: the linear curve, nothing spread across pixels; and the weighted OIT, whose one
        # layer composites exactly, where the moments' estimate darkens a lone layer by a tenth.
        raw = ["--tonemap", "linear", "--no-bloom", "--no-ssao", "--no-dither",
               "--no-oit-moments"]
        lit = TAGS_FLAGS + raw + ["--node-tag", "floor=30", "--tag", "20=lit", "--tag", "30=lit"]
        half, _ = run("fade_half", lit + ["--tag-fade", "0.5"])
        solid, _ = run("fade_solid", lit)
        behind, _ = run("fade_behind", lit + ["--remove-node", "part3"])
        if half is None or solid is None or behind is None:
            return False, "a run failed"
        project = _projector(cam, half.shape[1], half.shape[0])
        top = (part_xs[3], part_top, part_z)
        ghost = _radiance_at(half, project, top)
        want = 0.5 * (_radiance_at(solid, project, top) + _radiance_at(behind, project, top))
        off = float(np.max(np.abs(ghost - want) / np.maximum(want, 1e-4)))
        aside = (0.5, 0.1, -0.4)
        spared = _code_at(half, project, aside) == _code_at(solid, project, aside)
        ok = d_gone == 0 and off <= 0.04 and spared
        return ok, (f"fade 1 {d_gone} px from part2 alone (want 0); fade 0.5 part3's top "
                    f"{off * 100:.1f}% from half the part and half the slab (want <= 4%); the lit "
                    f"slab beside it {'as' if spared else 'NOT as'} unfaded")

    @arm("scene-tag-ghost-off")
    @needs_fixture
    def _():
        _, painted = run("ghost_off", TAGS_FLAGS + ["--profiler", "--tag-class",
                                                     "3=colour=4080C0FF"])
        _, fading = run("ghost_on", TAGS_FLAGS + ["--profiler", "--tag-fade", "0.5"])
        off, on = "oit accumulate" in painted, "oit accumulate" in fading
        return not off and on, (f"OIT accumulate painted: {off} (want False), fading: {on} "
                                f"(want True)")

    @arm("scene-tag-shadow")
    @needs_fixture
    def _():
        ghost, _ = run("shadow_ghost", TAGS_FLAGS + ["--node-tag", "part3=13:4", "--tag-class",
                                                     "4=see=0.5"])
        bare, _ = run("shadow_bare", TAGS_FLAGS + ["--remove-node", "part3"])
        if ghost is None or bare is None:
            return False, "a run failed"
        project = _projector(cam, ghost.shape[1], ghost.shape[0])
        # Where part3's shadow fell on the slab: off its far side, along the sun.
        points = [(0.46, 0.1, 0.30), (0.49, 0.1, 0.24), (0.47, 0.1, 0.22), (0.48, 0.1, 0.28)]
        unequal = [p for p in points if _code_at(ghost, project, p) != _code_at(bare, project, p)]
        return not unequal, (f"{len(points) - len(unequal)} of {len(points)} points where the "
                             f"shadow fell read as with part3 removed")

    @arm("scene-tag-capture")
    def _():
        rooms = asset("cornell_rooms.cscn")
        # A GI volume, converged at load before any view draws, dirtied at frame 10 so it captures
        # again once the views have drawn with their tags, 64 probes a frame, at each frame's start.
        flags = ["--no-auto-exposure", "-E", "1.0", "--gi-volume", "--gi-rate", "64",
                 "--change-at", "10:gi"]
        # The red wall, which bounces into the GI the captures gather, and a ceiling panel the
        # right view shows, so that view differs; the panel's own light no capture of irradiance
        # takes, its derived area light carrying it. Both views draw with tags, borrowing one
        # empty table, so the programs a capture draws with carry them whichever view resolved
        # them last; the left's class row alone hides.
        hide = ["--tag-table", "--node-tag", "rooms_left_red=0:7", "--node-tag",
                "rooms_light_a=0:7", "--tag-class", "0/7=hidden"]
        pair, said = _views_run(workdir, "tags_capture_pair", rooms, 800, 600,
                                flags + hide + ["--views", "2x1", "--view-turn", "0"])
        alone, said2 = _views_run(workdir, "tags_capture_alone", rooms, 400, 600,
                                  flags + ["--views", "1x1", "--view-turn", "0"])
        if pair is None or alone is None:
            return False, f"a run failed: {said if pair is None else said2}"
        half = pair.shape[1] // 2
        left, right = pair[:, :half], pair[:, half:]
        d_left, d_right = _differ(left, alone), _differ(right, alone)
        return d_right == 0 and d_left > 0, (
            f"the view without tags {d_right} px from its view alone (want 0), the one hiding the "
            f"wall and the panel {d_left} px (want > 0)")

    purple = (0xB3, 0x88, 0xFF)
    ring_flag = "outline=B388FFFF"

    @arm("scene-tag-outline")
    @needs_fixture
    def _():
        plain, _ = run("outline_plain", TAGS_FLAGS)
        ringed, _ = run("outline_ringed", TAGS_FLAGS + ["--tag", f"10={ring_flag}"])
        if plain is None or ringed is None:
            return False, "a run failed"
        project = _projector(cam, plain.shape[1], plain.shape[0])
        # Leftward from inside part0's left face at its middle: the run of changed pixels just past
        # its silhouette is the ring.
        x0, y0 = project((part_xs[0] - 0.04, 0.16, part_z + 0.06))
        row, x = int(round(y0)), int(round(x0))
        changed = np.any(ringed[row] != plain[row], axis=1)
        while x > 0 and not changed[x]:
            x -= 1
        width = 0
        while x > 0 and changed[x]:
            width, x = width + 1, x - 1
        exact = bool(np.any(np.all(ringed == np.array(purple), axis=2)))
        # Down the row of parts from +x: part3 nearest, part2 wholly behind it.
        side = ["--cam-eye", "2.0,0.16,0.3", "--cam-target", "0,0.16,0.3", "--fov", "30"]
        bare, _ = run("outline_side", TAGS_FLAGS + side)
        hid, _ = run("outline_hidden", TAGS_FLAGS + side + ["--tag", f"12={ring_flag}"])
        seen, _ = run("outline_seen", TAGS_FLAGS + side + ["--tag", f"13={ring_flag}"])
        if bare is None or hid is None or seen is None:
            return False, "a run failed"

        def strength(img):
            """The ring's alpha where it is strongest: (img - bare) / (purple - bare)."""
            b = bare.astype(np.float64)
            c = np.array(purple, dtype=np.float64)
            moved = np.any(img != bare, axis=2)
            if not moved.any():
                return 0.0
            span = c - b[moved]
            ok = np.abs(span) > 40
            a = np.where(ok, (img[moved].astype(np.float64) - b[moved]) / np.where(ok, span, 1),
                         np.nan)
            return float(np.nanmax(np.nanmedian(a, axis=1)))

        a_hid, a_seen = strength(hid), strength(seen)
        ok = 2 <= width <= 4 and exact and abs(a_hid - 0.35) <= 0.05 and a_seen >= 0.95
        return ok, (f"a ring {width} px wide (want 2 to 4), the colour exact somewhere: {exact}; "
                    f"behind part3 at {a_hid:.2f} (want 0.35), in front at {a_seen:.2f} (want 1)")

    @arm("scene-tag-outline-still")
    @needs_fixture
    def _():
        late, said = run("outline_late", TAGS_FLAGS + still + ["--tag", f"40:10={ring_flag}"], 60)
        early, _ = run("outline_early", TAGS_FLAGS + still + ["--tag", f"10={ring_flag}"], 60)
        if late is None or early is None:
            return False, "a run failed"
        after = _drawn_from(said, 0, 40)
        diff = _differ(late, early)
        return not after and diff == 0, (
            f"{len(after)} scene frames drawn after the outline at 40 (want 0); the last frame "
            f"{diff} px from the outline there from the start (want 0)")

    @arm("scene-tag-restart")
    @needs_fixture
    def _():
        _, said = run("restart", TAGS_FLAGS + still2 + ["--tag-table", "--tag-views", "1",
                                                        "--tag", "40:11=colour=FF2020FF"], 60)
        left, right = _drawn_from(said, 0, 40), _drawn_from(said, 1, 40)
        ok = bool(left) and left[0] <= 41 and not right
        return ok, (f"the borrowing view draws again from {left[0] if left else None} (want 40 "
                    f"or 41), the other {len(right)} frames after 40 (want 0)")

    @arm("scene-tag-shared")
    @needs_fixture
    def _():
        img, said = run("shared", TAGS_FLAGS + still2 + ["--tag", "40:11=colour=FF2020FF"], 60)
        before, _ = run("shared_before", TAGS_FLAGS + still2 + ["--tag-table"], 60)
        if img is None or before is None:
            return False, "a run failed"
        left, right = _drawn_from(said, 0, 40), _drawn_from(said, 1, 40)
        half = img.shape[1] // 2
        changed = [_differ(img[:, :half], before[:, :half]),
                   _differ(img[:, half:], before[:, half:])]
        ok = bool(left) and bool(right) and min(changed) > 0
        return ok, (f"both views draw again (left from {left[0] if left else None}, right from "
                    f"{right[0] if right else None}), each shows the row: {changed} px (want > 0)")

    return arm.failed
