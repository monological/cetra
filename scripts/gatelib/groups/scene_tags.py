"""The scene-tags group: tags in 3D (spec 14.39) -- an object and a class on each surface, a table
the views borrow and each view's class rows and fade, which hide, paint, fade and outline them.
"""

import functools
import os

import numpy as np

from gatelib.arms import Arms
from gatelib.groups.views import _differ, _draws, _view_probe, _views_run
from gatelib.harness import asset
from gatelib.scenes import _projector

# The fixture's camera, as its scene file has it, and where its pieces stand (gen_tags_fixture.py).
TAGS_CAM = {"eye": (0.0, 1.3, 1.25), "target": (0.0, 0.05, 0.05), "fovy_deg": 40.0}
TAGS_STRIP_XS = (-0.3, -0.1, 0.1, 0.3)
TAGS_STRIP_TOP, TAGS_STRIP_Z = 0.12, -0.15
TAGS_PART_XS = (-0.36, -0.12, 0.12, 0.36)
TAGS_PART_TOP, TAGS_PART_Z = 0.22, 0.3
TAGS_CAP_TOP = 0.26
TAGS_W, TAGS_H = 400, 300

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
      scene-tag-capture    two views of the rooms under a GI volume, the left one hiding the red
                           wall and a ceiling panel by its tags, the volume dirtied at frame 10 so
                           it captures again inside the left view's draw: the right is its view
                           alone without tags, 0 px -- the captures see the wall's bounce -- and
                           the left differs.

    Falsified by hand at 14.39: the instance block's tag left unfilled, the instanced parts read
    whatever the block last held -- part0 and part1 black, part2 and part3 another object's colour
    -- and instanced is 27087 px off; the walk's change test removed, the rested view never draws
    again after frame 40; the paint's metal and gloss left on, the strip reads (139, 3, 3), red
    metal, against the slab's (225, 10, 10).

    scene-tag-capture holds the outcome, not one guard: with the capture's bind of no tags removed,
    and with its programs resolved with the tags bit as well, it still passes. The reflection
    probes are captured at the first frame, before any view binds its tags, and the GI captured
    again from frame 10 reaches the other view too little to move a code.
    """
    arm = Arms(20)
    scene = asset("tags_fixture.cscn")
    have = os.path.exists(scene)

    def run(tag, extra, frames=10):
        return _views_run(workdir, f"tags_{tag}", scene, TAGS_W, TAGS_H, extra, frames)

    @functools.cache
    def reach():
        return run("reach", TAGS_FLAGS + ["--render-mode", "15"], 3)

    @arm("scene-tag-off")
    def _():
        if not have:
            return None, "no tags_fixture.cscn"
        plain, said = run("plain", [])
        inert, said2 = run("inert", ["--tag-table", "--tag-class", "2=none"] + TAGS_FLAGS)
        if plain is None or inert is None:
            return False, f"a run failed: {said if plain is None else said2}"
        diff = _differ(plain, inert)
        return diff == 0, f"{diff} px between the fixture and its tags that change nothing (want 0)"

    @arm("scene-tag-reach")
    def _():
        if not have:
            return None, "no tags_fixture.cscn"
        img, said = reach()
        if img is None:
            return False, f"the run failed: {said}"
        project = _projector(TAGS_CAM, img.shape[1], img.shape[0])
        checks = [(f"strip{k + 1}", (x, TAGS_STRIP_TOP, TAGS_STRIP_Z), k + 1)
                  for k, x in enumerate(TAGS_STRIP_XS)]
        checks += [(f"part{k}", (x, TAGS_PART_TOP, TAGS_PART_Z + 0.04), 10 + k)
                   for k, x in enumerate(TAGS_PART_XS)]
        checks += [("cap", (TAGS_PART_XS[0], TAGS_CAP_TOP, TAGS_PART_Z), 10),
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
    def _():
        if not have:
            return None, "no tags_fixture.cscn"
        img, said = reach()
        single, said2 = run("single", TAGS_FLAGS + ["--render-mode", "15", "--no-instancing"], 3)
        if img is None or single is None:
            return False, f"a run failed: {said if img is None else said2}"
        diff = _differ(img, single)
        return diff == 0, f"{diff} px between instanced and one draw an object (want 0)"

    @arm("scene-tag-node")
    def _():
        if not have:
            return None, "no tags_fixture.cscn"
        still = ["--views", "1x1", "--view-turn", "0", "--accumulate", "16", "--view-probe"]
        _, moved = run("node_moved", TAGS_FLAGS + still + ["--node-tag", "40:part1=55:3"], 60)
        _, same = run("node_same", TAGS_FLAGS + still + ["--node-tag", "40:part1=11:3"], 60)
        rows_moved, _ = _view_probe(moved)
        rows_same, _ = _view_probe(same)
        drawn_moved = sorted(f for f, row in rows_moved.get(0, {}).items() if row[0] and f >= 40)
        drawn_same = sorted(f for f, row in rows_same.get(0, {}).items() if row[0] and f >= 40)
        rested = _draws(rows_same, 0)
        ok = bool(drawn_moved) and drawn_moved[0] <= 41 and not drawn_same and rested is not None
        return ok, (f"a new tag at frame 40 draws the rested view again from frame "
                    f"{drawn_moved[0] if drawn_moved else None} (want 40 or 41); the same tag "
                    f"written draws {len(drawn_same)} frames after it (want 0)")

    @functools.cache
    def without(name):
        return run(f"without_{name}", ["--remove-node", name])

    @arm("scene-tag-hidden")
    def _():
        if not have:
            return None, "no tags_fixture.cscn"
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
    def _():
        if not have:
            return None, "no tags_fixture.cscn"
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
        project = _projector(TAGS_CAM, plain.shape[1], plain.shape[0])
        strip = (TAGS_STRIP_XS[1], TAGS_STRIP_TOP, TAGS_STRIP_Z)
        slab_top = (0.0, 0.1, -0.4)
        copper, painted = _code_at(plain, project, strip), _code_at(red, project, strip)
        slab_painted = _code_at(red, project, slab_top)
        reds = max(abs(a - b) for a, b in zip(painted, slab_painted)) <= 3
        top = (TAGS_PART_XS[3], TAGS_PART_TOP, TAGS_PART_Z)
        front = (TAGS_PART_XS[3], 0.16, TAGS_PART_Z + 0.06)
        lit_top, lit_front = _code_at(lit, project, top), _code_at(lit, project, front)
        flat_top, flat_front = _code_at(flat, project, top), _code_at(flat, project, front)
        shaded = max(abs(a - b) for a, b in zip(lit_top, lit_front))
        evened = max(abs(a - b) for a, b in zip(flat_top, flat_front))
        black = _code_at(dim, project, (TAGS_PART_XS[2], TAGS_PART_TOP, TAGS_PART_Z))
        ok = reds and shaded > 20 and evened <= 1 and max(black) <= 2
        return ok, (f"the strip {copper} unpainted, {painted} under red, the slab in the same red "
                    f"{slab_painted} (want within 3 codes); part3's top "
                    f"and front {shaded} codes apart unpainted (want > 20), {evened} painted flat "
                    f"(want <= 1); part2 dimmed all the way {black} (want black)")

    @arm("scene-tag-order")
    def _():
        if not have:
            return None, "no tags_fixture.cscn"
        img, _ = run("order", TAGS_FLAGS + ["--node-tag", "strips=0:2", "--tag-class",
                                            "2=colour=FF2020FF", "--tag", "3=colour=20C040FF",
                                            "--tag-class", "1=hidden", "--tag", "20=colour=FFFFFFFF"])
        no_slab, _ = without("slab")
        if img is None or no_slab is None:
            return False, "a run failed"
        project = _projector(TAGS_CAM, img.shape[1], img.shape[0])
        own = _code_at(img, project, (TAGS_STRIP_XS[2], TAGS_STRIP_TOP, TAGS_STRIP_Z))
        beside = _code_at(img, project, (TAGS_STRIP_XS[1], TAGS_STRIP_TOP, TAGS_STRIP_Z))
        slab_at = (0.5, 0.1, -0.4)
        slab, bare = _code_at(img, project, slab_at), _code_at(no_slab, project, slab_at)
        ok = own[1] > 2 * max(own[0], own[2]) and beside[0] > 3 * max(beside[1], beside[2]) and \
            slab == bare
        return ok, (f"strip 3 {own} (want green), strip 2 {beside} (want red); the slab, tagged "
                    f"white in a hidden class, {slab} where the floor shows {bare} (want equal)")

    @arm("scene-tag-limit")
    def _():
        if not have:
            return None, "no tags_fixture.cscn"
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

    @arm("scene-tag-capture")
    def _():
        rooms = asset("cornell_rooms.cscn")
        # A GI volume, converged at load before any view draws, dirtied at frame 10 so it captures
        # again inside the views' frames, 64 probes a frame.
        flags = ["--no-auto-exposure", "-E", "1.0", "--gi-volume", "--gi-rate", "64",
                 "--change-at", "10:gi"]
        # The red wall, which bounces into the GI the captures gather, and a ceiling panel the
        # right view shows, so that view differs; the panel's own light no capture of irradiance
        # takes, its derived area light carrying it.
        # The LEFT view hides, the first drawn: the world's lighting work runs inside the first
        # view's draw, after it has bound its tags, so that is where a capture would take them.
        # Both views draw with tags, borrowing one empty table, so the programs a capture draws
        # with carry them whichever view resolved them last; the left's class row alone hides.
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

    return arm.failed
