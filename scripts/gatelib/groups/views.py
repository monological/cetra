"""The views group: several scene views in one window (spec 14.38): each its own camera, rectangle,
settings, buffers and shadows, composited in list order.
"""

import functools
import os
import re

import numpy as np

from gatelib.arms import Arms
from gatelib.harness import RENDER, _run, asset
from gatelib.images import _DISPLAY_TO_LINEAR, _ppm_array
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


def _view_probe(said):
    """--view-probe's lines: per grid view, per frame, (drawn, accumulated, finished, draws); and
    the frames the run drew, or None."""
    rows = {}
    for m in re.finditer(r"^view-probe frame=(\d+) view=(\d+) drawn=(\d) accumulated=(\d+) "
                         r"finished=(\d) draws=(\d+)", said or "", re.M):
        frame, view, drawn, held, finished, draws = map(int, m.groups())
        rows.setdefault(view, {})[frame] = (drawn, held, finished, draws)
    end = re.search(r"^view-probe end frames=(\d+)", said or "", re.M)
    return rows, int(end.group(1)) if end else None


def _draws(rows, view):
    """The frames a view drew over the run, from its last probe line."""
    frames = rows.get(view, {})
    return frames[max(frames)][3] if frames else None


def _linear(img):
    """Display codes back to the radiance the linear tone curve encoded, 0..1."""
    return np.asarray(_DISPLAY_TO_LINEAR)[img.astype(np.intp)]


def run_views_gate(workdir):
    """Several views (spec 14.38): apps/render --views CxR tiles its window with views the app's
    own view drives, hidden, each the camera turned --view-turn degrees more about the target. A
    lone view of the grid (--views 1x1 --view-index K) is what each view is held to: every view is
    drawn through its picture, a lone one too. The noise floor is 0 px: one configuration rendered
    twice is identical, which views-same-camera states.

      views-one         one view filling the window is the window's frame without --views, 0 px.
      views-same-camera two views of one camera side by side are each that view alone at its
                        size, 0 px, and the lone view twice is 0 px.
      views-alone       four cameras, a quarter of the window each, over sky and clouds, over a
                        fogged ground with sun shadows, AO and SSR, and over OIT: each is its view
                        alone, ranking the world from its own eye (--view-focus 0), 0 px. What a
                        view keeps for itself -- its shadows, its clouds' history, its froxels,
                        its aerial volume -- is its own, and none of it is the focus's.
      views-zero        --views 0: no scene pass or chain runs (no scene clear or tone map in the
                        profiler, which a view's run shows), every pixel is --window-color exactly,
                        the probe set still captures its two probes, and the sixteen-texture wall
                        delivers as many textures as with a view.
      views-hidden      a view hidden from frame 10 leaves its half the window's colour, and the
                        other half is its view alone, 0 px. Under --accumulate 16, hidden at 20
                        and shown at 30, the view draws nothing more and the frame is the pair
                        never hidden, 0 px; a node moved at 25 while it was hidden, it draws again
                        when shown, 34.
      views-mask        the QFN package in view 0's bit alone: view 1 is the board without it --
                        its body and its shadow -- 0 px from the board with it removed, and view 0
                        is the board with it, 0 px from its view alone; the two differ by the
                        package.
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

    Still views, under --accumulate 16 unless said: a view draws a TAA frame, averages 16, and
    rests from its 17th; a change at frame 40 of 60 restarts it for 17 more, 34 in all.

      still-converges   --accumulate 32 --idle draws 33 frames and the run ends there, and the
                        average is under half as far from a 64-sample supersampled reference, in
                        radiance, as TAA's frame 200.
      still-rest        under --idle the view rests at 17 and the run ends there, of 200 asked.
      still-camera      one view's camera moved: it draws 34, the other 17.
      still-material    a material written and said with scene_mark_changed: both draw 34;
                        written unsaid, both 17 -- the contract.
      still-node        a node in one view's bit alone moved: that view 34, the other 17; a light
                        moved, both 34.
      still-settings    one view's setting changed: it draws 34, the other, its settings written
                        every frame by the app to what they were, 17.
      still-engine      engine_mark_changed, and engine_set_typical_size at twice the size: both
                        34, and the second's frame differs from the run without it, its lengths
                        rescaled.
      still-forever     over water, and over a surface hook marked animated, a view never rests:
                        40 of 40 frames drawn each.
      still-unfinished  under auto exposure the still board rests at the pinned 17, the meter held
                        while it averages; the key light four times brighter at 40, it rests later
                        than the pinned view does after the same change, the meter adapting.
      still-ui-only     60 frames of a view resting from 17 draw it 17 times, and the last frame
                        is the one it rested on, 0 px: the 43 after it are the window's alone.
      still-first       frames 2 and 4, an average of 1 and of 3, show the TAA frame before them,
                        0 px; frame 5, an average of 4, is shown.
      still-off         accumulation off: 40 of 40 frames drawn.
      still-taau        accumulation below render scale 1 is said once and the view keeps TAA's
                        upscale, drawing 40 of 40.

    The exact background, a frame of nothing but background, vignette and bloom off:

      background-exact  under Neutral, ACES, AgX and the linear curve, three colours each, every
                        pixel reads back the colour asked with the dither off, and within one code
                        with it on; an exposure six times larger moves it 0 px.
      background-log    an exact background under a vignette is said once; without one, not.
      hdr-memory        an environment loaded from its file's bytes is the same file loaded by its
                        path, 0 px, and both differ from the board with no environment.
    """
    arm = Arms(17)
    board = asset("board_fixture.cscn")
    board_flags = ["--no-auto-exposure", "-E", "0.0088", "--contact-shadows"]
    half_w = VIEWS_W // 2

    def lone(tag, scene, flags, w, h, index=0, frames=30, own_focus=False):
        return _views_run(workdir, tag, scene, w, h, flags + [
            "--views", "1x1", "--view-index", str(index)] + (
            ["--view-focus", "0"] if own_focus else []), frames=frames)

    # The board's view 0 alone at half the window, which four arms hold a half to.
    @functools.cache
    def board_half():
        return lone("board_half", board, board_flags, half_w, VIEWS_H)

    # A grid under --accumulate 16 unless said, with each view's frames from --view-probe.
    def still(tag, extra, views="1x1", frames=60, scene=board, flags=None, w=400, h=300):
        img, said = _views_run(workdir, f"still_{tag}", scene, w, h, (
            board_flags + ["--accumulate", "16"] if flags is None else flags) + [
            "--views", views, "--view-probe"] + extra, frames=frames)
        rows, end = _view_probe(said)
        return img, rows, end, said

    rested = 17               # a TAA frame and 16 averaged
    restarted = 2 * rested    # and again from a change: its TAA frame and 16 more

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
                alone, _ = lone(f"alone_{tag}_{k}", scene, flags, half_w, VIEWS_H // 2, index=k,
                                own_focus=True)
                if alone is None:
                    return False, f"the {tag} view {k} alone failed"
                per.append(_differ(cell, alone))
            worst = max(worst, max(per))
            said.append(f"{tag} {per}")
        return worst == 0, f"each view from its view alone, px: {'; '.join(said)} (want all 0)"

    @arm("views-zero")
    def _():
        color = (0.2, 0.4, 0.6)
        flags = ["--window-color", ",".join(str(c) for c in color), "--profiler",
                 "--probe-set-probe", "1", "--no-auto-exposure", "-E", "1.0"]
        rooms = asset("cornell_rooms.cscn")
        img, said = _views_run(workdir, "zero", rooms, 400, 300, flags + ["--views", "0"],
                               frames=5)
        viewed, viewed_said = _views_run(workdir, "zero_view", rooms, 400, 300,
                                         flags + ["--views", "1x1"], frames=5)
        wall = asset("textures_in_fixture.cscn")
        _, wall_zero = _views_run(workdir, "zero_wall", wall, 200, 150,
                                  ["--views", "0", "--texture-probe"], frames=8)
        _, wall_view = _views_run(workdir, "zero_wall_view", wall, 200, 150,
                                  ["--views", "1x1", "--texture-probe"], frames=8)
        if img is None or viewed is None:
            return False, f"a run failed: {said}"
        want = np.array([round(c * 255) for c in color], dtype=np.float64)
        off = int(np.any(img != want, axis=2).sum())

        def scopes(text):
            return len(re.findall(r"^(scene clear|tonemap \+ finishing)\s", text, re.M))

        caps = [int(m) for m in re.findall(r"^probe-set frame=\d+ .*captures=(\d+)", said, re.M)]
        captured = max(caps) if caps else 0
        textures = [len(re.findall(r"^texture-probe tex ", t or "", re.M))
                    for t in (wall_zero, wall_view)]
        ok = (off == 0 and scopes(said) == 0 and scopes(viewed_said) > 0 and captured == 2 and
              textures[0] == textures[1] > 0)
        return ok, (f"{off} px off the window's colour (want 0), {scopes(said)} scene or chain "
                    f"scopes (want 0; a view's run {scopes(viewed_said)}, want > 0), {captured} "
                    f"probes captured (want 2), {textures[0]} textures in with no view against "
                    f"{textures[1]} with one (want equal, > 0)")

    @arm("views-hidden")
    def _():
        pair = ["--views", "2x1", "--view-turn", "0"]
        hidden, _ = _views_run(workdir, "hidden", board, VIEWS_W, VIEWS_H,
                               board_flags + pair + ["--view-hide-at", "1:10"])
        alone, _ = board_half()
        hide = ["--view-turn", "0", "--view-hide-at", "1:20", "--view-show-at", "30"]
        back, back_rows, _, _ = still("hidden_back", hide, views="2x1")
        never, _, _, _ = still("hidden_never", ["--view-turn", "0"], views="2x1")
        _, moved_rows, _, _ = still("hidden_moved", hide + ["--change-at", "25:node:qfn_body"],
                                    views="2x1")
        if hidden is None or alone is None or back is None or never is None:
            return False, "a run failed"
        left, right = _cells(hidden, 2, 1)
        lit = int(np.any(right != 0, axis=2).sum())
        dl, db = _differ(left, alone), _differ(back, never)
        unchanged, moved = _draws(back_rows, 1), _draws(moved_rows, 1)
        ok = lit == 0 and dl == 0 and db == 0 and unchanged == rested and moved == restarted
        return ok, (f"hidden half {lit} px off the window's colour (want 0), the other {dl} px "
                    f"from its view alone (want 0); shown again unchanged, {unchanged} draws "
                    f"(want {rested}) and {db} px from never hidden (want 0); a node moved while "
                    f"it was hidden, {moved} draws (want {restarted})")

    @arm("views-mask")
    def _():
        masked, _ = _views_run(workdir, "mask", board, VIEWS_W, VIEWS_H, board_flags + [
            "--views", "2x1", "--view-turn", "0", "--view-mask", "1:2",
            "--node-mask", "qfn_body:1"])
        # Removed, not masked: a reference masked out would cast the shadow the arm asks after
        # exactly as wrongly as the view under test.
        without, _ = lone("mask_without", board, board_flags + ["--remove-node", "qfn_body"],
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

    # ---- still views: --accumulate 16 unless said, so a view rests at its 17th frame -- a TAA
    # frame, then the 16 it averages -- and a change at frame 40 is one it has rested through.
    def two_views(tag, change, extra=()):
        """Each of a pair's draws over 60 frames with `change` at frame 40."""
        img, rows, _, _ = still(tag, ["--change-at", change] + list(extra), views="2x1")
        return img, (_draws(rows, 0), _draws(rows, 1))

    @arm("still-converges")
    def _():
        # Radiance, not display codes, is what a pixel averages, as the engine's own
        # supersampling does: the linear curve under an exposure nothing clips at, and the
        # reference box-filtered 8x8 after decoding. Shadows, AO and bloom off, whose kernels are
        # sized in pixels and would differ at eight times the size for reasons of their own.
        flags = ["--no-auto-exposure", "-E", "0.003", "--tonemap", "linear", "--no-ssao",
                 "--no-bloom", "--no-dither", "--msaa", "1", "--no-shadows"]
        w, h = 200, 150
        ref, _ = _views_run(workdir, "conv_ref", board, w * 8, h * 8, flags + ["--views", "1x1"],
                            frames=3)
        avg, rows, end, _ = still("conv_avg", ["--idle"], flags=flags + ["--accumulate", "32"],
                                  frames=200, w=w, h=h)
        taa, _ = _views_run(workdir, "conv_taa", board, w, h, flags + [
            "--views", "1x1", "--taa", "--headless-jitter"], frames=200)
        if ref is None or avg is None or taa is None:
            return False, "a run failed"
        fh, fw = ref.shape[0] // 8, ref.shape[1] // 8
        ref = _linear(ref).reshape(fh, 8, fw, 8, 3).mean(axis=(1, 3))
        rms = [float(np.sqrt(np.mean(((_linear(img) - ref) * 255.0) ** 2))) for img in (avg, taa)]
        draws = _draws(rows, 0)
        ok = draws == 33 and end == 33 and rms[0] < 0.5 * rms[1]
        return ok, (f"{draws} frames drawn, the run ending at {end} (want 33: a TAA frame and 32 "
                    f"averaged); {rms[0]:.3f} RMS from a 64-sample reference against TAA's "
                    f"{rms[1]:.3f} at frame 200 (want under half)")

    @arm("still-rest")
    def _():
        _, rows, end, _ = still("rest", ["--idle"], frames=200)
        draws, frames = _draws(rows, 0), rows.get(0, {})
        finished = frames[max(frames)][2] if frames else 0
        ok = draws == rested and end == rested and finished == 1
        return ok, (f"{draws} frames drawn and the run ended at {end} of 200 asked (want "
                    f"{rested} each), resting {finished == 1}")

    @arm("still-camera")
    def _():
        _, draws = two_views("camera", "40:camera:1")
        ok = draws == (rested, restarted)
        return ok, f"draws {draws} after one view's camera moved (want ({rested}, {restarted}))"

    @arm("still-material")
    def _():
        _, said = two_views("material", "40:material")
        _, silent = two_views("material_silent", "40:material-silent")
        ok = said == (restarted, restarted) and silent == (rested, rested)
        return ok, (f"draws {said} with scene_mark_changed, {silent} without (want both "
                    f"{restarted}, then both {rested}: an unsaid write is the app's to say)")

    @arm("still-node")
    def _():
        masks = ["--view-mask", "1:2", "--node-mask", "qfn_body:1"]
        _, node = two_views("node", "40:node:qfn_body", masks)
        _, light = two_views("light", "40:light", masks)
        ok = node == (restarted, rested) and light == (restarted, restarted)
        return ok, (f"draws {node} after a node in view 0's bit alone moved (want ({restarted}, "
                    f"{rested})), {light} after a light moved (want both {restarted})")

    @arm("still-settings")
    def _():
        # The app writes every grid view's settings each frame, the unchanged view's to what they
        # were: that view resting is the write of a setting's own value restarting nothing.
        _, changed = two_views("settings", "40:settings:1")
        ok = changed == (rested, restarted)
        return ok, (f"draws {changed} after one view's setting changed, the other's written to "
                    f"what they were (want ({rested}, {restarted}))")

    @arm("still-engine")
    def _():
        _, marked = two_views("engine", "40:engine")
        typical_img, typical = two_views("typical", "40:typical:0.002")
        plain, _, _, _ = still("engine_plain", [], views="2x1")
        if typical_img is None or plain is None:
            return False, "a run failed"
        rescaled = _differ(typical_img, plain)
        ok = marked == (restarted, restarted) and typical == (restarted, restarted) and rescaled > 0
        return ok, (f"draws {marked} after engine_mark_changed, {typical} after "
                    f"engine_set_typical_size at twice the size (want both {restarted} each), "
                    f"which moved {rescaled} px (want > 0)")

    @arm("still-forever")
    def _():
        def animated(d):
            d["shaderHooks"]["stripes"]["animated"] = True
        hooked = cscn_copy(asset("shader_hooks_fixture.cscn"),
                           os.path.join(workdir, "views_hooks_animated.cscn"), animated)
        said = []
        ok = True
        for tag, scene in (("water", asset("water_fixture.cscn")), ("hook", hooked)):
            _, rows, _, _ = still(f"forever_{tag}", [], scene=scene, frames=40,
                                  flags=["--no-auto-exposure", "-E", "1.0", "--accumulate", "16"])
            finished = any(f[2] for f in rows.get(0, {}).values())
            draws = _draws(rows, 0)
            ok = ok and draws == 40 and not finished
            said.append(f"{tag} {draws} of 40 drawn, resting {finished}")
        return ok, f"{'; '.join(said)} (want 40 each, never resting)"

    @arm("still-unfinished")
    def _():
        auto = [f for f in board_flags if f not in ("--no-auto-exposure", "-E", "0.0088")]

        def first_rest(rows, after):
            frames = rows.get(0, {})
            done = [f for f in sorted(frames) if f > after and frames[f][2]]
            return done[0] if done else None

        _, still_rows, _, _ = still("unfinished", [], flags=auto + ["--accumulate", "16"])
        brighter = ["--change-at", "40:light-bright"]
        _, auto_rows, _, _ = still("unfinished_auto", brighter, flags=auto + ["--accumulate", "16"],
                                   frames=300)
        _, pinned_rows, _, _ = still("unfinished_pinned", brighter, frames=300)
        settled = first_rest(still_rows, 0)
        auto_rest, pinned_rest = first_rest(auto_rows, 40), first_rest(pinned_rows, 40)
        ok = (settled == rested and pinned_rest is not None and auto_rest is not None and
              auto_rest > pinned_rest)
        return ok, (f"under auto exposure the still board rested at {settled} (want {rested}); "
                    f"after the light brightened at 40 it rested at {auto_rest}, the pinned view "
                    f"at {pinned_rest} (want later, and at all)")

    @arm("still-ui-only")
    def _():
        img, rows, _, _ = still("ui_only", [])
        stop, _, _, _ = still("ui_only_stop", [], frames=rested)
        if img is None or stop is None:
            return False, "a run failed"
        scenes = _draws(rows, 0)
        d = _differ(img, stop)
        ok = scenes == rested and d == 0
        return ok, (f"{scenes} scene passes in 60 frames (want {rested}), the last frame {d} px "
                    f"from the one the view rested on (want 0)")

    @arm("still-first")
    def _():
        shots = {f: still(f"first_{f}", [], frames=f)[0] for f in (1, 2, 4, 5)}
        if any(img is None for img in shots.values()):
            return False, "a run failed"
        held = [_differ(shots[f], shots[1]) for f in (2, 4)]
        shown = _differ(shots[5], shots[1])
        ok = held == [0, 0] and shown > 0
        return ok, (f"frames 2 and 4 {held} px from the TAA frame before them (want 0: an average "
                    f"of 1 and 3), frame 5 {shown} px (want > 0: an average of 4 is shown)")

    @arm("still-off")
    def _():
        _, rows, _, _ = still("off", [], flags=board_flags, frames=40)
        draws = _draws(rows, 0)
        return draws == 40, f"{draws} of 40 frames drawn with accumulation off (want 40)"

    @arm("still-taau")
    def _():
        _, rows, _, said = still("taau", ["--taa", "--headless-jitter", "--render-scale", "0.5"],
                                 frames=40)
        line = "A view averages only at render scale 1"
        draws, says = _draws(rows, 0), (said or "").count(line)
        ok = draws == 40 and says == 1
        return ok, (f"{draws} of 40 frames drawn below render scale 1 (want 40), said {says} "
                    f"time(s) (want 1)")

    # ---- the exact background: the board's camera turned up into empty space, so the whole
    # frame is background, with the vignette and bloom off -- a stage that changes a pixel by
    # where it is, and light added onto a bright background, as a camera's would.
    def sky(tag, extra, exposure="0.0088"):
        return _views_run(workdir, f"bg_{tag}", board, 64, 48, [
            "--no-auto-exposure", "-E", exposure, "--cam-eye", "0,1,0",
            "--cam-target", "0,2,0.001", "--no-vignette", "--no-bloom"] + extra, frames=3)

    colours = [(26, 27, 38), (128, 128, 128), (232, 228, 216)]

    def exact(colour):
        return ["--background-exact", ",".join(f"{c / 255:.6f}" for c in colour)]

    @arm("background-exact")
    def _():
        worst_off, worst_on, said = 0, 0, []
        for curve in ("neutral", "aces", "agx", "linear"):
            for colour in colours:
                want = np.array(colour, dtype=np.float64)
                tag = f"{curve}_{'_'.join(str(c) for c in colour)}"
                off, _ = sky(f"{tag}_off", ["--tonemap", curve, "--no-dither"] + exact(colour))
                on, _ = sky(f"{tag}_on", ["--tonemap", curve] + exact(colour))
                if off is None or on is None:
                    return False, f"a {curve} run failed"
                worst_off = max(worst_off, int(np.abs(off - want).max()))
                worst_on = max(worst_on, int(np.abs(on - want).max()))
        bright, _ = sky("exposure_a", exact(colours[0]))
        dim, _ = sky("exposure_b", exact(colours[0]), exposure="0.05")
        if bright is None or dim is None:
            return False, "an exposure run failed"
        moved = _differ(bright, dim)
        ok = worst_off == 0 and worst_on <= 1 and moved == 0
        return ok, (f"four curves by three colours: off by at most {worst_off} code(s) with the "
                    f"dither off (want 0), {worst_on} with it on (want <= 1); an exposure six "
                    f"times larger moved it {moved} px (want 0)")

    @arm("background-log")
    def _():
        line = "exact background has a stage over it"
        _, vignetted = _views_run(workdir, "bg_log", board, 64, 48, [
            "--no-auto-exposure", "-E", "0.0088", "--cam-eye", "0,1,0", "--cam-target",
            "0,2,0.001", "--no-bloom"] + exact(colours[0]), frames=5)
        _, clean = sky("log_clean", exact(colours[0]))
        if vignetted is None or clean is None:
            return False, "a run failed"
        said, quiet = vignetted.count(line), clean.count(line)
        return said == 1 and quiet == 0, (f"said {said} time(s) under a vignette (want 1), {quiet} "
                                          f"without one (want 0)")

    @arm("hdr-memory")
    def _():
        env = asset("abandoned_window/abandoned_window_env.hdr")
        flags = board_flags + ["-e", env]
        path, _ = _views_run(workdir, "hdr_path", board, 400, 300, flags)
        memory, said = _views_run(workdir, "hdr_memory", board, 400, 300, flags + ["--hdr-memory"])
        bare, _ = _views_run(workdir, "hdr_none", board, 400, 300, board_flags)
        if path is None or memory is None or bare is None:
            return False, "a run failed"
        loaded = "Loaded HDR from memory" in said
        d, lit = _differ(path, memory), _differ(path, bare)
        ok = loaded and d == 0 and lit > 0
        return ok, (f"the environment from its bytes {d} px from the same file by its path (want "
                    f"0), loaded from memory {loaded}; {lit} px from the board without it (want > "
                    f"0)")

    return arm.failed
