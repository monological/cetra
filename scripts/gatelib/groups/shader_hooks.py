"""The shader-hooks group: an app's own shaders at named points (spec 13.29)."""

import os

import numpy as np

from gatelib.harness import NO_HALOS, asset, asset_ref, render
from gatelib.images import LSB, _ppm_array, compare
from gatelib.scenes import _camera_block, _cscn_camera, _projector, _world_pixel_box, cscn_copy


def _frac_pixel_box(w, h, rect):
    """A rect in the frame's 0..1, GL's (y up), as a pixel box (x0, y0, x1, y1), rows top down."""
    x0, v0, x1, v1 = rect
    return (int(round(x0 * w)), int(round((1.0 - v1) * h)),
            int(round(x1 * w)), int(round((1.0 - v0) * h)))


def _box_codes(img, box):
    """Every pixel's mean code across its channels inside a pixel box, as a flat array."""
    x0, y0, x1, y1 = box
    return img[y0:y1, x0:x1].mean(axis=2).ravel()


def _ring_mean(img, box, inner, outer):
    """Mean code over the pixels `inner` to `outer` px outside a pixel box, every channel."""
    h, w, _ = img.shape
    x0, y0, x1, y1 = box
    ys, xs = np.mgrid[0:h, 0:w]
    d = np.maximum.reduce([x0 - xs, xs - (x1 - 1), y0 - ys, ys - (y1 - 1)])
    ring = (d >= inner) & (d < outer)
    return float(img[ring].mean()) if ring.any() else 0.0


def _rgb_at(img, project, p):
    """The pixel a world point lands on, as its three codes."""
    x, y = project(p)
    return img[int(y), int(x)]


# App shaders at named points (spec 13.29), on assets/scenes/shader_hooks_fixture.cscn. The
# positions here mirror the generator rather than being read from the asset, the TSL rule above:
# they are what the prediction uses, and a prediction read from a broken asset agrees with it.
HOOKS_FIXTURE = "shader_hooks_fixture.cscn"
HOOKS_MARKS = {"beforeDof": (0.55, 0.80, 0.62, 0.88),
               "beforeBloom": (0.68, 0.80, 0.75, 0.88),
               "afterTonemap": (0.81, 0.80, 0.88, 0.88)}
HOOKS_MARK_CODES = (64, 128, 192)       # what the after-tonemap mark writes, in codes
HOOKS_DOF = ["--dof", "--dof-focus", "8.6", "--dof-range", "1.0"]
HOOKS_HALO_MIN = 6.0      # codes a bloomed mark lifts the wall 3-10 px out, at the least
HOOKS_SPREAD_MIN = 20.0   # codes a defocused mark spreads 1-3 px past its edge, at the least
HOOKS_STILL_MAX = 1.0     # codes a mark drawn after a stage may move what that stage left
# The static quad's open stretch, right of the post in front of it, and the post's front face
# where it crosses the quad, both in world metres (the generator's STATIC_* and post box).
HOOKS_STATIC_OPEN = ((-3.4, 2.0, -3.8), (-2.8, 3.2, -3.8))
HOOKS_POST_FACE = ((-4.1, 2.2, -2.9), (-3.8, 3.0, -2.9))
HOOKS_NOISE_STD_MIN = 20.0   # codes of spread a frame of fresh noise keeps under TAA
HOOKS_NOISE_CORR_MAX = 0.2   # correlation two consecutive frames' noise may share
HOOKS_LATE_SEEN_MIN = 20.0   # mean codes the quad moves the open stretch from the bare frame
HOOKS_LATE_HIDDEN_MAX = 1.0  # mean codes the frame may move behind the post, the quad hidden
# The stripes box (x -4.6..-3.4, y 0..1.2, z -1.0..0.2): the generator's pair of 0.3 m by world
# X, dark where fract(x / 0.3) < 0.5. Sampled at quarter and three-quarter phase, on the front
# face and on the top, each (x, dark).
HOOKS_STRIPE_X = [(-4.425, True), (-4.275, False), (-4.125, True), (-3.975, False),
                  (-3.825, True), (-3.675, False)]
HOOKS_STRIPE_FRONT = (0.6, 0.2)    # y, z on the front face
HOOKS_STRIPE_TOP = (1.2, -0.4)     # y, z on the top
HOOKS_STRIPE_SPLIT = 100           # codes between a dark and a light stripe in the albedo view
# Front-face centres of the boxes the hooks paint flat: the two hooks and the two params.
HOOKS_FLAT = {"key_a": ((-2.5, 0.4, 0.3), 0), "key_b": ((-1.5, 0.4, 0.3), 2),
              "param_a": ((-0.4, 0.4, 0.3), 0), "param_b": ((0.6, 0.4, 0.3), 1)}
# The card: x 1.6..2.8, y 0..1.4, its faces at z -0.62 and -0.58. Its hook holes a 3x2 grid of
# UV cells, u running with x and v down from the top, so a cell (u, v) is at x = 1.6 + 1.2u,
# y = 1.4(1 - v). Hole centres, and points between the holes.
HOOKS_CARD_HOLES = [(u, v) for u in (1 / 6, 1 / 2, 5 / 6) for v in (0.25, 0.75)]
HOOKS_CARD_SOLID = [(1 / 3, 0.25), (2 / 3, 0.25), (1 / 3, 0.75), (2 / 3, 0.75)]
HOOKS_SUN = (-0.3, -1.0, -0.7)     # the fixture's sun, which way it travels
# Looking down on the floor behind the card from behind it, where its shadow falls: from the
# front the card stands between the eye and its lower row of holes' shadows.
HOOKS_CARD_CAMERA = {"eye": [2.0, 6.0, -3.2], "target": [2.0, 0.0, -0.9], "fov": 45.0}
HOOKS_HOLE_LIGHT_MIN = 15.0  # codes a hole's shadow is brighter than the solid card's
HOOKS_HOLE_SOLID_MAX = 3.0   # codes the floor may move between the holes, solid either way
# The dome: a 1.6 m grid centred (4.2, 0.005, -0.4), raised 0.6 m at its middle.
HOOKS_DOME_CENTRE = (4.2, 0.005, -0.4)
HOOKS_DOME_HEIGHT = 0.6
# A low sun for the dome's arm, so its shadow clears the dome's own footprint.
HOOKS_LOW_SUN = (-0.3, -0.45, -0.7)
# Looking up past the dome from close in front: the frame's bottom edge clears the flat grid's
# own box, so only the offset's declared bound keeps the raised dome from being culled.
HOOKS_CULL_CAMERA = {"eye": [4.2, 0.4, 1.8], "target": [4.2, 1.4, -0.4], "fov": 45.0}
HOOKS_DOME_SEEN_MIN = 30.0   # codes, in its largest channel, the raised dome differs from the
                             # flat one where it rises: blue against the grey behind it
HOOKS_DOME_SHADOW_MIN = 15.0  # codes the raised dome's apex shadow darkens the floor
HOOKS_MARK_BRIGHT_MIN = 250   # codes every channel of a bright HDR mark's centre reaches


def run_shader_hooks_gate(workdir):
    """An app's own shader runs where it says it does (spec 13.29).

      post-identity   every pass present but painting nothing renders the frame with no
                      passes: the HDR locations copy the frame back texel for texel, and the
                      picture after the tone map holds what the window would.
      post-order      a mark painted at each location shows what ran after it and nothing that
                      ran before. Before DOF: defocused, and it blooms. Before bloom: sharp, and
                      it blooms. After the tone map: sharp, no bloom, and the codes it wrote.
      late-fresh      the late-draw quad's noise, under TAA, keeps its spread in one frame and
                      shares nothing with the frame before: no history reaches it.
      late-occluded   where the post stands in front of the quad, the frame is the frame
                      without the quad; where nothing does, it is not.
      late-inert      the quad emitting nothing renders the frame with no quad at all: it casts
                      no shadow, writes no G-buffer and reaches no other pass.
      hooks-surface   the stripes box's hook decides its albedo: the albedo view shows its
                      stripes where the generator put them, and the shaded frame lights them,
                      its sunlit top brighter than its front.
      hooks-cache-key two hooks at one feature mask are two programs: one box red, one blue.
      hooks-params    one hook on two materials is told apart by a param: red and green.
      hooks-opacity-shadow the card's hook cuts its holes in the card and in its shadow: from
                      above, the floor under each hole's shadow is lit where the solid card's
                      is not, and between the holes the two agree.
      hooks-offset    the dome's offset raises it, its shadow follows it, and the lean depth
                      prepass on and off render the same frame.
      hooks-offset-cull looking up past the dome, its declared bound keeps it drawn; with no
                      bound it is culled as the flat grid it was built as.

    The marks sit over the wall, which the depth of field blurs and the pieces in front of it do
    not reach, so each one is measured against the same frame rendered with no passes at all.
    """
    src = asset(HOOKS_FIXTURE)
    if not os.path.exists(src):
        print(f"  shader-hooks  SKIP  (missing {HOOKS_FIXTURE})")
        return []

    def scene(name, mutate):
        return cscn_copy(src, os.path.join(workdir, name), mutate)

    def shot(scene_path, tag, extra):
        out = os.path.join(workdir, f"hooks_{tag}.ppm")
        err = render(scene_path, out, extra)
        if err:
            print(f"  shader-hooks  ERROR rendering {tag}: {err.strip()[-300:]}")
            return None
        return out

    # The fixture as authored, with no halos: the late arms read it against its twins, and the
    # surface arms read the same frame for the hooks' shading.
    lit = shot(src, "lit", NO_HALOS)
    verdicts = {}
    verdicts.update(_hooks_post_arms(src, scene, shot))
    verdicts.update(_hooks_late_arms(src, scene, shot, workdir, lit))
    verdicts.update(_hooks_surface_arms(src, shot, lit))
    verdicts.update(_hooks_offset_arms(scene, shot))

    failures = []

    def judged(name):
        ok, detail = verdicts[name]
        if not ok:
            failures.append(name)
        return ok, detail

    ok, detail = judged("post-identity")
    print(f"  post-identity {'PASS' if ok else 'FAIL'}  {detail}")
    ok, detail = judged("post-order")
    print(f"  post-order    {'PASS' if ok else 'FAIL'}  {detail}")
    ok, detail = judged("late-fresh")
    print(f"  late-fresh    {'PASS' if ok else 'FAIL'}  {detail}")
    ok, detail = judged("late-occluded")
    print(f"  late-occluded {'PASS' if ok else 'FAIL'}  {detail}")
    ok, detail = judged("late-inert")
    print(f"  late-inert    {'PASS' if ok else 'FAIL'}  {detail}")
    ok, detail = judged("hooks-surface")
    print(f"  hooks-surface {'PASS' if ok else 'FAIL'}  {detail}")
    ok, detail = judged("hooks-cache-key")
    print(f"  hooks-cache-key {'PASS' if ok else 'FAIL'}  {detail}")
    ok, detail = judged("hooks-params")
    print(f"  hooks-params  {'PASS' if ok else 'FAIL'}  {detail}")
    ok, detail = judged("hooks-opacity-shadow")
    print(f"  hooks-opacity-shadow {'PASS' if ok else 'FAIL'}  {detail}")
    ok, detail = judged("hooks-offset")
    print(f"  hooks-offset  {'PASS' if ok else 'FAIL'}  {detail}")
    ok, detail = judged("hooks-offset-cull")
    print(f"  hooks-offset-cull {'PASS' if ok else 'FAIL'}  {detail}")
    return failures


_NOT_RENDERED = (False, "a frame did not render")


def _hooks_post_arms(src, scene, shot):
    """post-identity and post-order, a verdict and its detail each."""
    verdicts = {}

    def no_passes(d):
        d["post"].pop("passes", None)

    def empty_marks(d):
        for p in d["post"]["passes"]:
            p["params"]["markRect"] = [0.0, 0.0, 0.0, 0.0]

    a = shot(scene("hooks_plain.cscn", no_passes), "plain", [])
    b = shot(scene("hooks_blank.cscn", empty_marks), "blank", [])
    if a and b:
        ae, peak = compare(a, b)
        verdicts["post-identity"] = (peak <= LSB, f"{ae} px differ, peak {peak * 255:.2f} codes "
                                     f"(bound 1: the picture after the tone map is fp16)")
    else:
        verdicts["post-identity"] = _NOT_RENDERED

    # Each mark against the same frame without it, the other two still painted, since a bright
    # mark's bloom reaches its neighbours. With bloom, which reads the halo; without, which reads
    # the defocus alone.
    def without(at):
        def mutate(d):
            for p in d["post"]["passes"]:
                if p["at"] == at:
                    p["params"]["markRect"] = [0.0, 0.0, 0.0, 0.0]
        return mutate

    scenes = {"marks": src}
    for at in HOOKS_MARKS:
        scenes[at] = scene(f"hooks_without_{at}.cscn", without(at))
    frames = {}
    for tag, scene_path in scenes.items():
        for bloom, flags in (("bloom", []), ("nobloom", ["--no-bloom"])):
            out = shot(scene_path, f"order_{tag}_{bloom}", HOOKS_DOF + ["--no-dither"] + flags)
            frames[(tag, bloom)] = _ppm_array(out) if out else None
    verdicts["post-order"] = (_NOT_RENDERED if any(f is None for f in frames.values())
                              else _hooks_post_order(frames))
    return verdicts


def _hooks_late_arms(src, scene, shot, workdir, lit):
    """late-fresh, late-occluded and late-inert, a verdict and its detail each. `lit` is the
    fixture as authored, with no halos."""
    verdicts = {}

    # late-fresh: frames 30 and 31 under TAA, with the jitter it resolves.
    fresh = os.path.join(workdir, "hooks_fresh.ppm")
    err = render(src, fresh, ["--screenshot-every", "30", "--taa", "--headless-jitter"], frames=31)
    prev = os.path.join(workdir, "hooks_fresh_000030.ppm")
    if err or not os.path.exists(prev):
        verdicts["late-fresh"] = (False, f"the TAA pair did not render: {(err or '')[-300:]}")
    else:
        now, before = _ppm_array(fresh), _ppm_array(prev)
        h, w, _ = now.shape
        project = _projector(_cscn_camera(HOOKS_FIXTURE), w, h)
        box = _world_pixel_box(project, *HOOKS_STATIC_OPEN, 3 * w // 400)
        a, b = _box_codes(now, box), _box_codes(before, box)
        std = float(a.std())
        corr = float(np.corrcoef(a, b)[0, 1]) if std > 0 and b.std() > 0 else 0.0
        verdicts["late-fresh"] = (
            std >= HOOKS_NOISE_STD_MIN and abs(corr) <= HOOKS_NOISE_CORR_MAX,
            f"spread {std:.1f} codes (needs >= {HOOKS_NOISE_STD_MIN:.0f}), frame-to-frame "
            f"correlation {corr:+.3f} (bound {HOOKS_NOISE_CORR_MAX}) over {a.size} px")

    # The bare twin is the same scene with no static quad in it at all; the dark one, the quad
    # emitting nothing. No halos: the open noise beside the post would bloom onto it, which is
    # light reaching it rather than the quad showing through.
    def bare(d):
        d["models"][0]["path"] = os.path.join(os.path.dirname(src),
                                              asset_ref("shader_hooks_fixture_bare.gltf"))

    def dark(d):
        d["materials"]["hooks_static"]["shaderParams"]["noiseCells"][2] = 0.0

    gone = shot(scene("hooks_bare.cscn", bare), "late_bare", NO_HALOS)
    quiet = shot(scene("hooks_dark.cscn", dark), "late_dark", NO_HALOS)
    if not (lit and gone and quiet):
        verdicts["late-occluded"] = verdicts["late-inert"] = _NOT_RENDERED
        return verdicts

    lit_img, gone_img = _ppm_array(lit), _ppm_array(gone)
    h, w, _ = lit_img.shape
    project = _projector(_cscn_camera(HOOKS_FIXTURE), w, h)

    def mean_abs(corners):
        box = _world_pixel_box(project, *corners, 3 * w // 400)
        return float(np.abs(_box_codes(lit_img, box) - _box_codes(gone_img, box)).mean())

    behind, exposed = mean_abs(HOOKS_POST_FACE), mean_abs(HOOKS_STATIC_OPEN)
    verdicts["late-occluded"] = (
        behind <= HOOKS_LATE_HIDDEN_MAX and exposed >= HOOKS_LATE_SEEN_MIN,
        f"behind the post the frame moves {behind:.2f} codes from the bare one (bound "
        f"{HOOKS_LATE_HIDDEN_MAX:.0f}); in the open {exposed:.1f} (needs >= "
        f"{HOOKS_LATE_SEEN_MIN:.0f})")

    ae, peak = compare(quiet, gone)
    verdicts["late-inert"] = (peak <= LSB, f"the quad at zero against no quad: {ae} px differ, "
                              f"peak {peak * 255:.2f} codes")
    return verdicts


def _hooks_surface_arms(src, shot, lit):
    """hooks-surface, hooks-cache-key and hooks-params, a verdict and its detail each. The albedo
    view (render mode 6) reads what the hook's first call decided; `lit`, the fixture as authored
    with no halos, reads it shaded."""
    albedo = shot(src, "surface_albedo", NO_HALOS + ["--render-mode", "6"])
    if not (albedo and lit):
        return {name: _NOT_RENDERED for name in ("hooks-surface", "hooks-cache-key",
                                                 "hooks-params")}
    albedo_img, lit_img = _ppm_array(albedo), _ppm_array(lit)
    h, w, _ = albedo_img.shape
    project = _projector(_cscn_camera(HOOKS_FIXTURE), w, h)

    def code_at(img, p):
        return float(_rgb_at(img, project, p).mean())

    problems, notes = [], []
    front = [(code_at(albedo_img, (x, *HOOKS_STRIPE_FRONT)), dark) for x, dark in HOOKS_STRIPE_X]
    darks = [c for c, dark in front if dark]
    lights = [c for c, dark in front if not dark]
    split = min(lights) - max(darks)
    notes.append(f"albedo view dark {[round(c) for c in darks]}, light {[round(c) for c in lights]}")
    if split < HOOKS_STRIPE_SPLIT:
        problems.append(f"the stripes split by {split:.0f} codes, not {HOOKS_STRIPE_SPLIT}")
    lit_front = [code_at(lit_img, (x, *HOOKS_STRIPE_FRONT)) for x, dark in HOOKS_STRIPE_X
                 if not dark]
    lit_top = [code_at(lit_img, (x, *HOOKS_STRIPE_TOP)) for x, dark in HOOKS_STRIPE_X
               if not dark]
    notes.append(f"shaded light stripes front {[round(c) for c in lit_front]}, top "
                 f"{[round(c) for c in lit_top]}")
    if min(lit_top) <= max(lit_front):
        problems.append("the sunlit top is not brighter than the front: the stripes are not lit")
    verdicts = {"hooks-surface": (not problems, "; ".join(problems + notes))}

    def flat(names):
        out, bad = [], []
        for name in names:
            p, channel = HOOKS_FLAT[name]
            rgb = tuple(int(c) for c in _rgb_at(albedo_img, project, p))
            out.append(f"{name} {rgb}")
            others = [c for i, c in enumerate(rgb) if i != channel]
            if rgb[channel] < 3 * max(max(others), 1):
                bad.append(name)
        return not bad, ", ".join(out) + (f"; not their own colour: {bad}" if bad else "")

    verdicts["hooks-cache-key"] = flat(["key_a", "key_b"])
    verdicts["hooks-params"] = flat(["param_a", "param_b"])
    return verdicts


def _hooks_floor_under(p, sun):
    """Where the sun travelling along `sun` carries world point `p` onto the floor, y = 0."""
    t = p[1] / -sun[1]
    return (p[0] + sun[0] * t, 0.0, p[2] + sun[2] * t)


def _hooks_offset_arms(scene, shot):
    """hooks-opacity-shadow, hooks-offset and hooks-offset-cull, a verdict and its detail each:
    the card's alpha and the dome's offset, each against a twin without them."""
    verdicts = {}

    def code(img, project, p):
        return float(_rgb_at(img, project, p).mean())

    def channel_gap(a, b, project, p):
        """The largest per-channel difference between two frames at `p`."""
        return float(np.abs(_rgb_at(a, project, p) - _rgb_at(b, project, p)).max())

    # The card, from above, holed and solid.
    def card_view(radius):
        def mutate(d):
            d["camera"] = dict(HOOKS_CARD_CAMERA)
            d["materials"]["hooks_card"]["shaderParams"]["holes"][2] = radius
        return mutate

    holed = shot(scene("hooks_card_holed.cscn", card_view(0.3)), "card_holed", NO_HALOS)
    solid = shot(scene("hooks_card_solid.cscn", card_view(0.0)), "card_solid", NO_HALOS)
    if holed and solid:
        holed_img, solid_img = _ppm_array(holed), _ppm_array(solid)
        h, w, _ = holed_img.shape
        project = _projector(_camera_block(HOOKS_CARD_CAMERA), w, h)

        def floor(u, v):
            return _hooks_floor_under((1.6 + 1.2 * u, 1.4 * (1.0 - v), -0.6), HOOKS_SUN)

        lit = [code(holed_img, project, floor(u, v)) - code(solid_img, project, floor(u, v))
               for u, v in HOOKS_CARD_HOLES]
        same = [abs(code(holed_img, project, floor(u, v)) - code(solid_img, project, floor(u, v)))
                for u, v in HOOKS_CARD_SOLID]
        verdicts["hooks-opacity-shadow"] = (
            min(lit) >= HOOKS_HOLE_LIGHT_MIN and max(same) <= HOOKS_HOLE_SOLID_MAX,
            f"under the holes' shadows the floor is {[round(v) for v in lit]} codes brighter "
            f"than under the solid card's (needs >= {HOOKS_HOLE_LIGHT_MIN:.0f}); between them "
            f"{[round(v, 1) for v in same]} (bound {HOOKS_HOLE_SOLID_MAX:.0f})")
    else:
        verdicts["hooks-opacity-shadow"] = _NOT_RENDERED

    # The dome under a low sun, raised and flat; and the lean prepass on and off.
    def dome_view(height):
        def mutate(d):
            d["lights"][0]["direction"] = list(HOOKS_LOW_SUN)
            d["materials"]["hooks_dome"]["shaderParams"]["dome"][0] = height
        return mutate

    raised_scene = scene("hooks_dome_raised.cscn", dome_view(HOOKS_DOME_HEIGHT))
    raised = shot(raised_scene, "dome_raised", NO_HALOS)
    flat = shot(scene("hooks_dome_flat.cscn", dome_view(0.0)), "dome_flat", NO_HALOS)
    prepassed = shot(raised_scene, "dome_prepass", NO_HALOS + ["--depth-prepass"])
    if raised and flat and prepassed:
        raised_img, flat_img = _ppm_array(raised), _ppm_array(flat)
        h, w, _ = raised_img.shape
        project = _projector(_cscn_camera(HOOKS_FIXTURE), w, h)
        cx, cy, cz = HOOKS_DOME_CENTRE
        body = (cx, cy + 0.75 * HOOKS_DOME_HEIGHT, cz)
        apex_shadow = _hooks_floor_under((cx, cy + HOOKS_DOME_HEIGHT, cz), HOOKS_LOW_SUN)
        seen = channel_gap(raised_img, flat_img, project, body)
        darker = code(flat_img, project, apex_shadow) - code(raised_img, project, apex_shadow)
        ae, peak = compare(raised, prepassed)
        verdicts["hooks-offset"] = (
            seen >= HOOKS_DOME_SEEN_MIN and darker >= HOOKS_DOME_SHADOW_MIN and peak <= LSB,
            f"the raised dome differs from the flat one by {seen:.0f} codes where it rises "
            f"(needs >= {HOOKS_DOME_SEEN_MIN:.0f}); its apex's shadow darkens the floor "
            f"{darker:.0f} (needs >= {HOOKS_DOME_SHADOW_MIN:.0f}); with the prepass {ae} px "
            f"differ, peak {peak * 255:.2f} codes")
    else:
        verdicts["hooks-offset"] = _NOT_RENDERED

    # Looking up past the dome: drawn with its bound, culled without one.
    def cull_view(bound):
        def mutate(d):
            d["camera"] = dict(HOOKS_CULL_CAMERA)
            d["shaderHooks"]["dome"]["offsetBound"] = bound
        return mutate

    bounded = shot(scene("hooks_cull_bounded.cscn", cull_view(HOOKS_DOME_HEIGHT)), "cull_bounded",
                   NO_HALOS)
    unbounded = shot(scene("hooks_cull_unbounded.cscn", cull_view(0.0)), "cull_unbounded",
                     NO_HALOS)
    if bounded and unbounded:
        bounded_img, unbounded_img = _ppm_array(bounded), _ppm_array(unbounded)
        h, w, _ = bounded_img.shape
        project = _projector(_camera_block(HOOKS_CULL_CAMERA), w, h)
        cx, cy, cz = HOOKS_DOME_CENTRE
        top = (cx, cy + 0.9 * HOOKS_DOME_HEIGHT, cz)
        x, y = project(top)
        on_screen = 0 <= x < w and 0 <= y < h
        moved = channel_gap(bounded_img, unbounded_img, project, top) if on_screen else 0.0
        verdicts["hooks-offset-cull"] = (
            on_screen and moved >= HOOKS_DOME_SEEN_MIN,
            f"the dome's top is {'on' if on_screen else 'OFF'} screen; with its bound against "
            f"without, {moved:.0f} codes there (needs >= {HOOKS_DOME_SEEN_MIN:.0f}: drawn, and "
            f"culled without)")
    else:
        verdicts["hooks-offset-cull"] = _NOT_RENDERED
    return verdicts


def _hooks_post_order(frames):
    """post-order's measurement, over each mark's frame with it and without it."""
    h, w, _ = frames[("marks", "bloom")].shape
    problems, notes = [], []
    for at, rect in HOOKS_MARKS.items():
        box = _frac_pixel_box(w, h, rect)
        cx, cy = (box[0] + box[2]) // 2, (box[1] + box[3]) // 2
        centre = tuple(int(c) for c in frames[("marks", "nobloom")][cy, cx])
        halo = (_ring_mean(frames[("marks", "bloom")], box, 3 * w // 400, 10 * w // 400)
                - _ring_mean(frames[(at, "bloom")], box, 3 * w // 400, 10 * w // 400))
        spread = (_ring_mean(frames[("marks", "nobloom")], box, w // 400, 3 * w // 400)
                  - _ring_mean(frames[(at, "nobloom")], box, w // 400, 3 * w // 400))
        notes.append(f"{at}: centre {centre}, halo {halo:+.1f}, spread {spread:+.1f}")
        if at == "afterTonemap":
            if centre != HOOKS_MARK_CODES:
                problems.append(f"{at} wrote {centre}, not {HOOKS_MARK_CODES}")
            if abs(halo) > HOOKS_STILL_MAX or abs(spread) > HOOKS_STILL_MAX:
                problems.append(f"{at} moved its surroundings")
        else:
            if min(centre) < HOOKS_MARK_BRIGHT_MIN:
                problems.append(f"{at} centre {centre} is not the bright mark")
            if halo < HOOKS_HALO_MIN:
                problems.append(f"{at} did not bloom")
        if at == "beforeDof" and spread < HOOKS_SPREAD_MIN:
            problems.append("beforeDof was not defocused")
        if at == "beforeBloom" and abs(spread) > HOOKS_STILL_MAX:
            problems.append("beforeBloom was defocused")
    return not problems, "; ".join(problems + notes)
