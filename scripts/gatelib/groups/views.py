"""The views group: several scene views in one window (spec 14.38): each its own camera, rectangle,
settings, buffers and shadows, composited in list order.
"""

import functools
import os
import re

import numpy as np

from gatelib.arms import Arms
from gatelib.harness import RENDER, _run, asset
from gatelib.images import _ppm_array
from gatelib.scenes import cscn_copy

# The window every grid arm draws, in window points; its halves and quarters are the views.
VIEWS_W, VIEWS_H = 800, 600


def _views_run(workdir, tag, scene, w, h, extra, frames=30):
    """apps/render headless at w x h points with `extra`: the frame as an (h, w, 3) array of codes
    and the run's output, or None and the output's tail for a run that failed."""
    out = os.path.join(workdir, f"views_{tag}.ppm")
    cmd = [RENDER, "-m", scene, "-x", "-f", str(frames), "-W", str(w), "-H", str(h), "-S", out]
    r = _run(cmd + extra, capture_output=True, text=True)
    said = r.stdout + r.stderr
    if r.returncode != 0 or not os.path.exists(out):
        return None, said[-300:]
    return _ppm_array(out), said


def _differ(a, b):
    """Pixels whose codes differ anywhere, between two arrays of one shape."""
    return int(np.any(a != b, axis=2).sum())


def _cells(img, cols, rows):
    """A frame cut into its grid's views, left to right, top to bottom."""
    h, w = img.shape[0] // rows, img.shape[1] // cols
    return [img[r * h:(r + 1) * h, c * w:(c + 1) * w] for r in range(rows) for c in range(cols)]


def run_views_gate(workdir):
    """Several views (spec 14.38): apps/render --views CxR tiles its window with views the app's
    own view drives, hidden, each the camera turned --view-turn degrees more about the target. A
    lone view of the grid (--views 1x1 --view-index K, --view-picture so it goes through a picture
    as a view among others does) is what each view is held to. The noise floor is 0 px: one
    configuration rendered twice is identical, which views-same-camera states.

      views-one         one view filling the window is the window's frame without --views, 0 px:
                        the lone view writes the window directly, as every app's one view does.
      views-same-camera two views of one camera side by side are each that view alone at its
                        size, 0 px, and the lone view twice is 0 px.
      views-alone       four cameras, a quarter of the window each, over sky and clouds, over a
                        fogged ground with sun shadows, AO and SSR, and over OIT: each is its view
                        alone, 0 px. What a view keeps for itself -- its shadows, its clouds'
                        history, its froxels, its aerial volume -- is its own.
      views-zero        --views 0: no scene pass or chain runs (no scene clear or tone map in the
                        profiler), every pixel is --window-color exactly, and the probe set still
                        captures its two probes.
      views-hidden      a view hidden from frame 10 leaves its half the window's colour, and the
                        other half is its view alone, 0 px; hidden at 10 and shown at 20, the frame
                        is the pair never hidden, 0 px.
      views-mask        the QFN package in view 0's bit alone: view 1 is the board without it --
                        its body and its shadow -- 0 px from a lone view whose package is in no
                        view, and view 0 is the board with it, 0 px from its view alone; the two
                        differ by the package.
      views-samples     a 1-sample view beside a 4-sample one: each is its view alone at its count,
                        0 px, and the two differ.
      views-resize      one view's rectangle changed at frame 10: one rebuild of render targets
                        more than the run without, and the other view still its view alone, 0 px.
      views-programs    four views with glare and local exposure link as many programs as one.
      views-order       a view laid over the right half of another: where they overlap the later
                        is drawn, its view alone there, 0 px, and where they do not, the first.
      views-focus-log   water drawn by a view other than the focus is said once over the run, and
                        the focus drawing it alone says nothing.
      views-kept-log    a view hiding a box within a kept lamp's reach is said once; the same view
                        hiding nothing says nothing.
      capture-overlay   light lines drawn in one view and in the focus, and not in the other: the
                        other is its view alone without them, 0 px, though the room's reflection
                        probes are captured while the focus shows lines; the lined view differs.
    """
    arm = Arms(17)
    board = asset("board_fixture.cscn")
    board_flags = ["--no-auto-exposure", "-E", "0.0088", "--contact-shadows"]
    half_w = VIEWS_W // 2

    def lone(tag, scene, flags, w, h, index=0, frames=30):
        return _views_run(workdir, tag, scene, w, h, flags + [
            "--views", "1x1", "--view-index", str(index), "--view-picture"], frames=frames)

    # The board's view 0 alone at half the window, which four arms hold a half to.
    @functools.cache
    def board_half():
        return lone("board_half", board, board_flags, half_w, VIEWS_H)

    @arm("views-one")
    def _():
        plain, _ = _views_run(workdir, "one_plain", board, VIEWS_W, VIEWS_H, board_flags)
        one, _ = _views_run(workdir, "one_grid", board, VIEWS_W, VIEWS_H,
                            board_flags + ["--views", "1x1"])
        if plain is None or one is None:
            return False, "a run failed"
        d = _differ(plain, one)
        return d == 0, f"{d} px from the window's frame without --views (want 0)"

    @arm("views-same-camera")
    def _():
        pair, _ = _views_run(workdir, "same_pair", board, VIEWS_W, VIEWS_H,
                             board_flags + ["--views", "2x1", "--view-turn", "0"])
        alone, _ = board_half()
        again, _ = lone("board_half_again", board, board_flags, half_w, VIEWS_H)
        if pair is None or alone is None or again is None:
            return False, "a run failed"
        left, right = _cells(pair, 2, 1)
        floor, dl, dr = _differ(alone, again), _differ(left, alone), _differ(right, alone)
        ok = floor == 0 and dl == 0 and dr == 0
        return ok, (f"left {dl} px, right {dr} px from the view alone (want 0), the view alone "
                    f"twice {floor} px")

    @arm("views-alone")
    def _():
        cases = [("sky", asset("aerial_fixture.cscn"),
                  ["--clouds", "--no-auto-exposure", "-E", "1.0"]),
                 ("fog", asset("contact_fixture.cscn"),
                  ["--fog", "--no-auto-exposure", "-E", "1.0"]),
                 ("oit", asset("oit_sphere_fixture.gltf"),
                  ["--no-auto-exposure", "-E", "0.08", "--oit-moments"])]
        worst, said = 0, []
        for tag, scene, flags in cases:
            grid, _ = _views_run(workdir, f"alone_{tag}_grid", scene, VIEWS_W, VIEWS_H,
                                 flags + ["--views", "2x2"])
            if grid is None:
                return False, f"the {tag} grid failed"
            per = []
            for k, cell in enumerate(_cells(grid, 2, 2)):
                alone, _ = lone(f"alone_{tag}_{k}", scene, flags, half_w, VIEWS_H // 2, index=k)
                if alone is None:
                    return False, f"the {tag} view {k} alone failed"
                per.append(_differ(cell, alone))
            worst = max(worst, max(per))
            said.append(f"{tag} {per}")
        return worst == 0, f"each view from its view alone, px: {'; '.join(said)} (want all 0)"

    @arm("views-zero")
    def _():
        color = (0.2, 0.4, 0.6)
        img, said = _views_run(workdir, "zero", asset("cornell_rooms.cscn"), 400, 300, [
            "--views", "0", "--window-color", ",".join(str(c) for c in color), "--profiler",
            "--probe-set-probe", "1", "--no-auto-exposure", "-E", "1.0"], frames=5)
        if img is None:
            return False, f"the run failed: {said}"
        want = np.array([round(c * 255) for c in color], dtype=np.float64)
        off = int(np.any(img != want, axis=2).sum())
        scopes = len(re.findall(r"^(scene clear|tonemap \+ finishing)\s", said, re.M))
        caps = [int(m) for m in re.findall(r"^probe-set frame=\d+ .*captures=(\d+)", said, re.M)]
        captured = max(caps) if caps else 0
        ok = off == 0 and scopes == 0 and captured == 2
        return ok, (f"{off} px off the window's colour (want 0), {scopes} scene or chain scopes "
                    f"(want 0), {captured} probes captured (want 2)")

    @arm("views-hidden")
    def _():
        pair = ["--views", "2x1", "--view-turn", "0"]
        hidden, _ = _views_run(workdir, "hidden", board, VIEWS_W, VIEWS_H,
                               board_flags + pair + ["--view-hide-at", "1:10"])
        back, _ = _views_run(workdir, "hidden_back", board, VIEWS_W, VIEWS_H,
                             board_flags + pair + ["--view-hide-at", "1:10", "--view-show-at",
                                                   "20"])
        never, _ = _views_run(workdir, "hidden_never", board, VIEWS_W, VIEWS_H, board_flags + pair)
        alone, _ = board_half()
        if hidden is None or back is None or never is None or alone is None:
            return False, "a run failed"
        left, right = _cells(hidden, 2, 1)
        lit = int(np.any(right != 0, axis=2).sum())
        dl, db = _differ(left, alone), _differ(back, never)
        ok = lit == 0 and dl == 0 and db == 0
        return ok, (f"hidden half {lit} px off the window's colour (want 0), the other {dl} px "
                    f"from its view alone (want 0); shown again, {db} px from never hidden "
                    f"(want 0)")

    @arm("views-mask")
    def _():
        masked, _ = _views_run(workdir, "mask", board, VIEWS_W, VIEWS_H, board_flags + [
            "--views", "2x1", "--view-turn", "0", "--view-mask", "1:2",
            "--node-mask", "qfn_body:1"])
        without, _ = lone("mask_without", board, board_flags + ["--node-mask", "qfn_body:0"],
                          half_w, VIEWS_H)
        alone, _ = board_half()
        if masked is None or without is None or alone is None:
            return False, "a run failed"
        left, right = _cells(masked, 2, 1)
        dl, dr, part = _differ(left, alone), _differ(right, without), _differ(alone, without)
        ok = dl == 0 and dr == 0 and part > 100
        return ok, (f"the view showing it {dl} px from its view alone, the view hiding it {dr} px "
                    f"from the board without it (want both 0); the package and its shadow "
                    f"{part} px (want > 100)")

    @arm("views-samples")
    def _():
        pair, _ = _views_run(workdir, "samples", board, VIEWS_W, VIEWS_H, board_flags + [
            "--views", "2x1", "--view-turn", "0", "--view-samples", "1:1"])
        one, _ = lone("samples_one", board, board_flags + ["--view-samples", "0:1"], half_w,
                      VIEWS_H)
        four, _ = board_half()
        if pair is None or one is None or four is None:
            return False, "a run failed"
        left, right = _cells(pair, 2, 1)
        dl, dr, apart = _differ(left, four), _differ(right, one), _differ(one, four)
        ok = dl == 0 and dr == 0 and apart > 0
        return ok, (f"4 samples {dl} px, 1 sample {dr} px from each alone (want 0); the counts "
                    f"differ by {apart} px (want > 0)")

    @arm("views-resize")
    def _():
        pair = ["--views", "2x1", "--view-turn", "0"]
        moved, said = _views_run(workdir, "resize", board, VIEWS_W, VIEWS_H, board_flags + pair + [
            "--view-rect", f"1:{half_w},0,{half_w // 2},{VIEWS_H // 2}", "--view-rect-at", "10"])
        still, still_said = _views_run(workdir, "resize_still", board, VIEWS_W, VIEWS_H,
                                       board_flags + pair)
        alone, _ = board_half()
        if moved is None or still is None or alone is None:
            return False, "a run failed"
        rebuilt = said.count("Render targets rebuilt") - still_said.count("Render targets rebuilt")
        dl = _differ(_cells(moved, 2, 1)[0], alone)
        ok = rebuilt == 1 and dl == 0
        return ok, (f"{rebuilt} rebuild(s) more than the run without (want 1), the other view "
                    f"{dl} px from its view alone (want 0)")

    @arm("views-programs")
    def _():
        flags = board_flags + ["--glare", "--local-exposure", "--programs-linked"]
        linked = []
        for grid in ("1x1", "2x2"):
            img, said = _views_run(workdir, f"programs_{grid}", board, 400, 300,
                                   flags + ["--views", grid], frames=3)
            m = re.search(r"^programs linked (\d+)", said or "", re.M)
            if img is None or not m:
                return False, f"the {grid} run failed or printed no count"
            linked.append(int(m.group(1)))
        return linked[0] == linked[1], (f"{linked[1]} programs linked for four views, {linked[0]} "
                                        f"for one (want equal)")

    @arm("views-order")
    def _():
        quarter = half_w // 2
        over, _ = _views_run(workdir, "order", board, VIEWS_W, VIEWS_H, board_flags + [
            "--views", "2x1", "--view-rect", f"1:{quarter},0,{half_w},{VIEWS_H}"])
        first, _ = board_half()
        second, _ = lone("order_second", board, board_flags, half_w, VIEWS_H, index=1)
        if over is None or first is None or second is None:
            return False, "a run failed"
        fb = over.shape[1] // VIEWS_W  # framebuffer pixels per point
        q = quarter * fb
        under = _differ(over[:, :q], first[:, :q])
        overlap = _differ(over[:, q:q + half_w * fb], second)
        ok = under == 0 and overlap == 0
        return ok, (f"the overlap {overlap} px from the later view alone, the rest {under} px "
                    f"from the first (want both 0)")

    @arm("views-focus-log")
    def _():
        water = asset("water_fixture.cscn")
        flags = ["--no-auto-exposure", "-E", "1.0"]
        line = "A view other than the focus draws water or rain"
        img, plain = _views_run(workdir, "focus_plain", water, 400, 300, flags)
        img2, grid = _views_run(workdir, "focus_grid", water, 400, 300, flags + ["--views", "1x1"])
        if img is None or img2 is None:
            return False, "a run failed"
        said, quiet = grid.count(line), plain.count(line)
        return said == 1 and quiet == 0, (f"said {said} time(s) by a view not the focus (want 1), "
                                          f"{quiet} by the focus alone (want 0)")

    @arm("views-kept-log")
    def _():
        def kept(d):
            d["lights"][0]["shadow_cache"] = True
        point = cscn_copy(asset("cornell_point.cscn"),
                          os.path.join(workdir, "views_point_kept.cscn"), kept)
        line = "A view hides a node within a kept lamp's reach"
        flags = ["--no-auto-exposure", "-E", "1.0", "--views", "1x1"]
        img, hiding = _views_run(workdir, "kept_hiding", point, 400, 300, flags + [
            "--view-mask", "0:2", "--node-mask", "cornell_tall_box:1"], frames=10)
        img2, open_ = _views_run(workdir, "kept_open", point, 400, 300, flags, frames=10)
        if img is None or img2 is None:
            return False, "a run failed"
        said, quiet = hiding.count(line), open_.count(line)
        return said == 1 and quiet == 0, (f"said {said} time(s) with the box hidden (want 1), "
                                          f"{quiet} with nothing hidden (want 0)")

    @arm("capture-overlay")
    def _():
        rooms = asset("cornell_rooms.cscn")
        flags = ["--no-auto-exposure", "-E", "1.0"]
        # --show-lights puts them on the focus too, the hidden view driving the grid, whose
        # switches a capture once took.
        pair, _ = _views_run(workdir, "overlay_pair", rooms, VIEWS_W, VIEWS_H, flags + [
            "--views", "2x1", "--view-turn", "0", "--show-lights", "--view-lines", "0"])
        alone, _ = lone("overlay_alone", rooms, flags, half_w, VIEWS_H)
        if pair is None or alone is None:
            return False, "a run failed"
        left, right = _cells(pair, 2, 1)
        dr, lined = _differ(right, alone), _differ(left, alone)
        ok = dr == 0 and lined > 0
        return ok, (f"the view without lines {dr} px from its view alone (want 0), the lined view "
                    f"{lined} px (want > 0)")

    return arm.failed
