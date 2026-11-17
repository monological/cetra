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

    Falsified by hand at 14.39: the instance block's tag left unfilled draws the four parts
    reach's part0 colour, each of them; the walk's change test removed, a still view never sees
    a node's new tag and rests through frame 40.
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

    return arm.failed
