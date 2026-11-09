"""The ortho group: orthographic post and shading (spec 11.104)."""

import math
import os

from gatelib.harness import NO_HALOS, asset, render
from gatelib.images import _absorb_box_rgb, _alphacov_mean, _box_luma, _read_ppm, compare
from gatelib.probes import _import_log
from gatelib.profiling import LOD_FIXTURE, LOD_SWEEP, _CLUSTERED_LINE, _lod_run
from gatelib.scenes import (
    FOGVOL_BOXES, FOGVOL_DARKEN_MAX, FOGVOL_FIXTURE, FOGVOL_FRAMES, MASK_FIXTURE, MASK_LIT_FLOOR,
    MASK_REF_BOX, _cscn_camera, _projector,
)


# Orthographic post and shading (spec 11.104).
#
# Every existing golden and arm is perspective, so all of them prove the
# perspective path is untouched and NONE can see whether an orthographic frame
# is RIGHT; taa-ortho above compares ortho against ortho and cancels a
# common-mode error. These arms are the ortho path's own evidence. Every bar
# sits against a reading taken on the build BEFORE the fix, recorded beside it,
# because a bar with no measured before-number is not admissible.

# ortho-view-*: the lit-surface view vector. mask_fixture is three untextured
# facing quads under one directional light and no environment, so with every
# depth-reading pass off the frame is a pure function of N, L and V. Under a
# parallel projection V is one direction for every fragment, so the picture
# CANNOT depend on where along the axis the eye sits -- and a V that fans from
# the eye point differs by tens of degrees between these two eyes on every lit
# pixel. The height reproduces the authored 45-degree framing at z = 0, where
# the quads are, so MASK_REF_BOX lands on the same surface it always did.
ORTHO_VIEW_CAM = _cscn_camera(MASK_FIXTURE)
# The second eye sits this far from the target along the authored axis; the
# first is the authored eye itself.
ORTHO_VIEW_NEAR_DIST = 0.6
# Vignette is on by default and is a radial gradient that would sit in the
# flat-quad spread either way; dither would add a code of its own.
ORTHO_VIEW_FLAGS = ["--no-ssao", "--no-ssr", *NO_HALOS, "--no-vignette", "--no-dither",
                    "--no-shadows", "--no-auto-exposure", "-E", "1.0"]
# px between the two eyes. Broken build: 107,665. Fixed: 0, since nothing but V
# moves and V no longer depends on the eye. A second arm reading the code
# SPREAD across one flat-on quad was tried and dropped: the broken build reads
# 1 code there too, because a fanning V hardly moves a Lambert term and the
# specular lobe at 70 degrees off-normal is under a code -- the eye arm carries
# the claim by five orders of magnitude and the spread carries nothing.
ORTHO_VIEW_EYE_MAX = 0

# ortho-ao: parity, because a flat plane cannot see this -- a depth-scaled
# reconstruction of a plane is still a plane, every horizon reads open, and AO
# is 255 either way. A perspective camera at distance L differs from ortho by
# 1/(1 + dz/L) across a scene of depth dz, so the eye is pulled to L = 150 along
# the authored axis (under the app's 40r far plane; cascades stop at 8r, hence
# --no-shadows on both legs) and the perspective FOV is what frames the same
# window there. Both legs then put GTAO's reach at the same screen size:
# 0.5*P00*r/L for perspective and 0.5*P00*r for ortho, equal at the target plane.
ORTHO_AO_FIXTURE = "dir_shadow_fixture.cscn"
ORTHO_AO_CAM = _cscn_camera(ORTHO_AO_FIXTURE)
ORTHO_AO_DIST = 150.0
# The resting sphere's contact point, and the half-width of the ground box read
# around it; the ring the sphere throws is where the two legs must agree.
ORTHO_AO_REST = (1.6, 0.0, 0.0)
ORTHO_AO_HALF = 1.0
ORTHO_AO_FLAGS = ["--no-shadows", "--no-auto-exposure", "-E", "1.0", "--ssao-debug"]
# A code below this in the AO debug buffer counts as occluded.
ORTHO_AO_DARK = 240
# Ortho's occluded fraction against perspective's: a chain that silently does
# nothing under ortho reads 0 here. The perspective floor guards the ratio
# against a perspective leg whose AO is itself dead -- and it is LOW, because
# the rings under two spheres are 0.37% of this frame on the telephoto leg
# (measured), where a dead chain reads exactly 0.
ORTHO_AO_MIN_RATIO = 0.5
ORTHO_AO_PERSP_FLOOR = 0.001
# The ring box's mean DEFICIT (255 minus its mean code) under ortho as a
# multiple of the same box's deficit under perspective. A ratio rather than a
# mean absolute difference because the box is mostly open floor around a thin
# ring, and the difference diluted to 2.4 codes on the broken build -- under a
# bar any fixed build would also clear. The broken build's ortho deficit is 0.
ORTHO_AO_RING_MIN_RATIO = 0.5
ORTHO_AO_RING_MAX_RATIO = 2.0

# ortho-dof: the depth inverse, with no parity needed. dof_fixture's checker
# panel sits at the focus plane (z = 0, six units from the authored eye); its
# 8 px cells are 2.6 px at 800x600 and mip to grey at the suite's default size,
# which is why this arm alone renders larger. Focused, the sharp image passes
# through; refocused to 30 the panel must blur. A depth inverse that assumes
# w = -z under glm_ortho returns a distance of about 1 for the panel, so the
# in-focus render is fully blurred and the two DoF renders agree.
ORTHO_DOF_FIXTURE = "dof_fixture.cscn"
ORTHO_DOF_CAM = _cscn_camera(ORTHO_DOF_FIXTURE)
# Last-wins over render()'s shared size, the way every larger capture in this
# file asks for one.
ORTHO_DOF_SIZE = ["-W", "800", "-H", "600"]
# Panel interior at the ortho framing: x +-0.6 of a 5.83 half-width, y 0.55..1.45
# about the target's 1.0 over a 4.37 height, inset a third to stay clear of
# the blur bleeding in from the edge.
ORTHO_DOF_BOX = (0.38, 0.38, 0.62, 0.62)
ORTHO_DOF_FLAGS = [*NO_HALOS, "--no-ssao", "--no-ssr", "--no-auto-exposure", "-E", "1.0"]
ORTHO_DOF_ON = ["--dof", "--dof-focus", "6", "--dof-range", "1.5", "--dof-max-coc", "8"]
# A focused panel is not byte-identical to the no-DoF frame even when the CoC is
# 0: the pass gathers at half resolution and resamples back, which the
# PERSPECTIVE control on the same fixture measures at 6.1 codes. The fixed
# orthographic build reads 6.7, the broken one 31.4 -- the same as its refocused
# leg, which is the signature. Bar 12: 1.8x the fixed reading, 0.38x the broken.
ORTHO_DOF_SHARP_MAX = 12.0
ORTHO_DOF_BLUR_MIN = 20.0

# ortho-cluster: the froxel wedge. Under ortho a cluster is a box of constant
# width; a bound that scales it by depth puts a tile edge at ndc*depth*half_w,
# about 28x too far out at this framing. The prediction was that every light
# would then touch every tile. THE MEASUREMENT SAYS THE OPPOSITE, and it is
# worse: the innermost tile's edge lands at 70 world units, so it swallows every
# light, and the tiles beyond it start where no light is -- the broken build
# assigns 65 indices where the matched perspective assigns 709, and most of the
# frame is unlit by clustered lights. So the bar is TWO-SIDED. 25 lights on a
# 5x5 grid, spacing 4, at y = 1; the two legs frame the same window from the
# same eye, so their counts should be near each other.
ORTHO_CLUSTER_FLAGS = ["--point-light-grid", "5,4,5", "--cam-eye", "0,20,20",
                       "--cam-target", "0,0,0", "--no-shadows"]
ORTHO_CLUSTER_HEIGHT = "30"
ORTHO_CLUSTER_FOV = "55.9"
ORTHO_CLUSTER_MIN_RATIO = 0.5
ORTHO_CLUSTER_MAX_RATIO = 2.0

# ortho-lod: projected size is distance-independent under ortho, so the LOD
# sweep must select the SAME level at every eye and only the height may move
# it. The inverse arm at a huge height is what stops "always level 0" passing.
ORTHO_LOD_HEIGHT = "8"
ORTHO_LOD_HEIGHT_FAR = "800"

# ortho-fog-volume: a site the code phases missed and a review found afterwards
# (spec 11.104 as-built), with its own instrument.
#
# The froxel fog's history reprojection mapped a cell into the previous volume
# by xy * focal / depth, the perspective form, so under ortho every cell read
# its history from near the centre column, and over sixty frames the two boxes
# on the fog volume fixture blend toward the gap between them. The statistic is
# fogvol-density's own, box over gap, read under an orthographic camera at the
# authored framing with the bar the perspective arm holds. No eye-invariance
# arm can see this, or the fog's phase direction, or the aerial LUT's path
# length: each is a function of a froxel's VIEW-space position, which does not
# move with the eye under a parallel projection.
ORTHO_FOG_CAM = _cscn_camera(FOGVOL_FIXTURE)
ORTHO_FOG_FLAGS = ["--no-auto-exposure", "-E", "1.0", "--no-dither", "--no-ssao"]
# Ortho's box/gap against perspective's from the same eye. The phase direction
# fans from the eye on the broken build, so the two boxes, off-axis where the
# fan is largest, weight the sun's in-scatter differently from the gap.
ORTHO_FOG_PARITY_MIN = 0.8
ORTHO_FOG_PARITY_MAX = 1.25

# Water's optical path took length(ViewPos), the sight line from the eye
# point, for the ray scale and the from-below path, and its short-band fade
# took the same length as a distance; all three are fixed and NONE has an arm.
# The view-eye design was written for them and measured 246,444 px on the
# broken build, then 226,641 with the path fixed and 384,957 with an
# orthographic branch on the projected grid as well -- because the water plane
# CROSSES the near plane under a tilted parallel view, so the eye's position
# along the axis decides how much of the plane is clipped, and because the
# grid itself is deferred (spec 11.104): its far-capped rows spike and its
# uniform lattice moires against the wave field. An arm whose premise the
# fixture refutes is not admissible, so the path fix carries a hand argument
# in the spec instead.


def _vec_str(v):
    return ",".join(f"{c:g}" for c in v)


def _ortho_height_matching(fovy_deg, dist):
    """The orthographic view height that frames what a fovy camera frames at dist."""
    return 2.0 * dist * math.tan(math.radians(fovy_deg) * 0.5)


def _box_absdiff(pa, pb, w, h, box):
    """Mean |a - b| of the red channel over a fractional box, in raw codes."""
    x0, y0 = int(w * box[0]), int(h * box[1])
    x1, y1 = int(w * box[2]), int(h * box[3])
    s = n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            o = (y * w + x) * 3
            s += abs(pa[o] - pb[o])
            n += 1
    return s / n if n else 0.0


def _dark_fraction(pix, w, h, below):
    n = w * h
    return sum(1 for i in range(n) if pix[i * 3] < below) / n if n else 0.0


def run_ortho_gate(workdir):
    """The post chain and the lit surface under an orthographic camera (spec 11.104).

      ortho-view-eye   under a parallel projection the frame cannot depend on where
                       along the axis the eye sits: two eyes, 0 px apart
      ortho-ao         GTAO under ortho matches a far telephoto perspective of the
                       same window: the ring under a resting sphere agrees, and
                       ortho's occluded fraction is not a dead chain's zero
      ortho-dof        the depth inverse: a panel at the focus distance stays sharp
                       and blurs when refocused
      ortho-cluster    the froxel wedge is a box under ortho: index count within 2x
                       of the matched perspective, no pool overflow
      ortho-lod        projected size is distance-independent: one level across the
                       sweep, equal to --no-lod
      ortho-lod-height a larger view volume selects a coarser level, so the arm
                       above cannot pass on a selection that ignores everything
      ortho-fog-volume the froxel history reprojects a cell onto itself under
                       ortho: the fog volume fixture's boxes keep their contrast
                       against the gap, near perspective's from the same eye

    WHAT THE BROKEN BUILD READS, measured before the fix and kept here so every
    bar has a before-number. ortho-view-eye: 107,665 px between the two eyes.
    ortho-ao: ortho's occluded fraction 0.0000 against the telephoto's 0.0037,
    ring deficit 0. ortho-dof: the focused panel moves 31.43 codes, the same as
    the refocused one. ortho-cluster: 65 indices against 709, ratio 0.09.
    ortho-lod: 13826 / 5756 / 1718 across the sweep, the perspective ladder.
    ortho-lod-height: 5756 at both heights. Two arms were designed and did not
    survive this measurement: a flat-quad code spread (broken reads 1) and a
    telephoto/ortho box mean-absolute-difference (broken reads 2.4 codes, mostly
    open floor); both are recorded beside the constants they would have used.
    A site found by review after the code phases was measured the same way
    before its fix: ortho-fog-volume reads box/gap 0.9567 under ortho against
    0.5904 under perspective, ratio 1.62, the boxes all but gone. Water's arm
    is a third that did not survive; the constants' comment says why.
    """
    failures = []

    # --- ortho-view-eye -------------------------------------------------------
    scene = asset(MASK_FIXTURE)
    eye_far, target = ORTHO_VIEW_CAM["eye"], ORTHO_VIEW_CAM["target"]
    axis = [e - t for e, t in zip(eye_far, target)]
    view_dist = math.sqrt(sum(c * c for c in axis))
    eye_near = tuple(t + a / view_dist * ORTHO_VIEW_NEAR_DIST for t, a in zip(target, axis))
    height = _ortho_height_matching(ORTHO_VIEW_CAM["fovy_deg"], view_dist)
    common = ORTHO_VIEW_FLAGS + ["--ortho", f"{height:.4f}", "--cam-target", _vec_str(target)]
    far = os.path.join(workdir, "ortho_view_far.ppm")
    near = os.path.join(workdir, "ortho_view_near.ppm")
    near2 = os.path.join(workdir, "ortho_view_near2.ppm")
    err = (render(scene, far, common + ["--cam-eye", _vec_str(eye_far)]) or
           render(scene, near, common + ["--cam-eye", _vec_str(eye_near)]) or
           render(scene, near2, common + ["--cam-eye", _vec_str(eye_near)]))
    if err:
        print(f"  ortho-view-eye ERROR render failed: {err.strip()[-200:]}")
        failures.append("ortho-view-eye")
    else:
        floor = compare(near, near2)[0]
        moved = compare(far, near)[0]
        w, h, pix = _read_ppm(near)
        lit = _box_luma(pix, w, h, MASK_REF_BOX) >= MASK_LIT_FLOOR
        ok = moved <= ORTHO_VIEW_EYE_MAX and floor == 0 and lit
        print(f"  ortho-view-eye {'PASS' if ok else 'FAIL'}  {moved} px between an eye at "
              f"{view_dist:.1f} and one at {ORTHO_VIEW_NEAR_DIST} along the same axis (want <= "
              f"{ORTHO_VIEW_EYE_MAX}, floor "
              f"{floor}, the quads lit: {lit}; under a parallel projection V is one direction "
              f"everywhere, so the eye's position along it cannot reach a pixel)")
        if not ok:
            failures.append("ortho-view-eye")

    # --- ortho-ao -------------------------------------------------------------
    scene = asset(ORTHO_AO_FIXTURE)
    # The axis runs from the authored eye TO the target, so the far eye sits
    # back along it: target MINUS the unit vector. Added instead, the eye lands
    # under the ground on the far side, both legs render the underside of the
    # floor, and every reading below is a flat 255 that agrees with itself.
    ao_target, ao_eye = ORTHO_AO_CAM["target"], ORTHO_AO_CAM["eye"]
    axis = [t - e for t, e in zip(ao_target, ao_eye)]
    auth_dist = math.sqrt(sum(c * c for c in axis))
    unit = [c / auth_dist for c in axis]
    eye_far = tuple(t - u * ORTHO_AO_DIST for t, u in zip(ao_target, unit))
    height = _ortho_height_matching(ORTHO_AO_CAM["fovy_deg"], auth_dist)
    fov_far = math.degrees(2.0 * math.atan(0.5 * height / ORTHO_AO_DIST))
    common = ORTHO_AO_FLAGS + ["--cam-eye", _vec_str(eye_far), "--cam-target",
                               _vec_str(ao_target)]
    persp = os.path.join(workdir, "ortho_ao_persp.ppm")
    orth = os.path.join(workdir, "ortho_ao_ortho.ppm")
    orth2 = os.path.join(workdir, "ortho_ao_ortho2.ppm")
    err = (render(scene, persp, common + ["--fov", f"{fov_far:.3f}"]) or
           render(scene, orth, common + ["--ortho", f"{height:.3f}"]) or
           render(scene, orth2, common + ["--ortho", f"{height:.3f}"]))
    if err:
        print(f"  ortho-ao     ERROR render failed: {err.strip()[-200:]}")
        failures.append("ortho-ao")
    else:
        floor = compare(orth, orth2)[0]
        w, h, ppx = _read_ppm(persp)
        _, _, opx = _read_ppm(orth)
        project = _projector({"eye": eye_far, "target": ao_target, "ortho_height": height}, w, h)
        rx, ry, rz = ORTHO_AO_REST
        corners = [project((rx + sx * ORTHO_AO_HALF, ry, rz + sz * ORTHO_AO_HALF))
                   for sx in (-1, 1) for sz in (-1, 1)]
        box = (min(c[0] for c in corners) / w, min(c[1] for c in corners) / h,
               max(c[0] for c in corners) / w, max(c[1] for c in corners) / h)
        dark_p = _dark_fraction(ppx, w, h, ORTHO_AO_DARK)
        dark_o = _dark_fraction(opx, w, h, ORTHO_AO_DARK)
        ratio = dark_o / dark_p if dark_p > 0 else 0.0
        # The debug buffer is grey, so its luma is its code.
        deficit_p = 255.0 - _alphacov_mean(ppx, w, h, box)
        deficit_o = 255.0 - _alphacov_mean(opx, w, h, box)
        ring = deficit_o / deficit_p if deficit_p > 0 else 0.0
        ok = (floor == 0 and dark_p >= ORTHO_AO_PERSP_FLOOR and ratio >= ORTHO_AO_MIN_RATIO
              and ORTHO_AO_RING_MIN_RATIO <= ring <= ORTHO_AO_RING_MAX_RATIO)
        print(f"  ortho-ao     {'PASS' if ok else 'FAIL'}  occluded fraction {dark_o:.4f} ortho "
              f"against {dark_p:.4f} telephoto, ratio {ratio:.2f} (want >= {ORTHO_AO_MIN_RATIO} "
              f"with perspective above {ORTHO_AO_PERSP_FLOOR}); the ring box's deficit "
              f"{deficit_o:.2f} against {deficit_p:.2f}, ratio {ring:.2f} (want "
              f"{ORTHO_AO_RING_MIN_RATIO}..{ORTHO_AO_RING_MAX_RATIO}); floor {floor}. A "
              f"reconstruction that multiplies by depth puts GTAO's reach under its early-out "
              f"at 150 units, so ortho reads a flat 255: fraction 0, deficit 0")
        if not ok:
            failures.append("ortho-ao")

    # --- ortho-dof ------------------------------------------------------------
    scene = asset(ORTHO_DOF_FIXTURE)
    dof_dist = math.sqrt(sum((e - t) ** 2 for e, t in zip(ORTHO_DOF_CAM["eye"],
                                                            ORTHO_DOF_CAM["target"])))
    dof_height = _ortho_height_matching(ORTHO_DOF_CAM["fovy_deg"], dof_dist)
    common = ORTHO_DOF_FLAGS + ORTHO_DOF_SIZE + ["--ortho", f"{dof_height:.4f}"]
    base = os.path.join(workdir, "ortho_dof_off.ppm")
    focus = os.path.join(workdir, "ortho_dof_focus.ppm")
    blur = os.path.join(workdir, "ortho_dof_blur.ppm")
    # --dof-focus appears twice on the blur leg; the app's parser is last-wins.
    err = (render(scene, base, common) or
           render(scene, focus, common + ORTHO_DOF_ON) or
           render(scene, blur, common + ORTHO_DOF_ON + ["--dof-focus", "30"]))
    if err:
        print(f"  ortho-dof    ERROR render failed: {err.strip()[-200:]}")
        failures.append("ortho-dof")
    else:
        w, h, bpx = _read_ppm(base)
        _, _, fpx = _read_ppm(focus)
        _, _, dpx = _read_ppm(blur)
        sharp = _box_absdiff(fpx, bpx, w, h, ORTHO_DOF_BOX)
        blurred = _box_absdiff(dpx, bpx, w, h, ORTHO_DOF_BOX)
        ok = sharp <= ORTHO_DOF_SHARP_MAX and blurred >= ORTHO_DOF_BLUR_MIN
        print(f"  ortho-dof    {'PASS' if ok else 'FAIL'}  the panel at the focus distance moves "
              f"{sharp:.2f} codes under DoF (want <= {ORTHO_DOF_SHARP_MAX}) and {blurred:.2f} "
              f"when refocused to 30 (want >= {ORTHO_DOF_BLUR_MIN}); a perspective depth "
              f"inverse under glm_ortho reads the panel at ~1 unit and blurs it in focus")
        if not ok:
            failures.append("ortho-dof")

    # --- ortho-cluster --------------------------------------------------------
    scene = asset(LOD_FIXTURE)
    # The reading is one log line, printed on the first cluster build, so the
    # two-frame log render serves; the size rides along because the count was
    # calibrated at this aspect.
    logs = {}
    for tag, extra in (("persp", ["--fov", ORTHO_CLUSTER_FOV]),
                       ("ortho", ["--ortho", ORTHO_CLUSTER_HEIGHT])):
        text = _import_log(scene, ORTHO_CLUSTER_FLAGS + extra + ["-W", "400", "-H", "300"])
        m = _CLUSTERED_LINE.search(text)
        logs[tag] = (m, "overflowed" in text)
    if not (logs["persp"][0] and logs["ortho"][0]):
        print("  ortho-cluster ERROR the clustered: line did not print on both legs")
        failures.append("ortho-cluster")
    else:
        lp, lo = int(logs["persp"][0].group(2)), int(logs["ortho"][0].group(2))
        ip, io = int(logs["persp"][0].group(3)), int(logs["ortho"][0].group(3))
        ratio = io / ip if ip else 0.0
        ok = (lp == lo == 25 and ORTHO_CLUSTER_MIN_RATIO <= ratio <= ORTHO_CLUSTER_MAX_RATIO
              and not logs["ortho"][1])
        print(f"  ortho-cluster {'PASS' if ok else 'FAIL'}  {lo} lights into {io} cluster "
              f"indices under ortho against {ip} under the matched perspective, ratio "
              f"{ratio:.2f} (want {ORTHO_CLUSTER_MIN_RATIO}..{ORTHO_CLUSTER_MAX_RATIO}, both "
              f"legs 25 lights, no overflow: overflowed={logs['ortho'][1]}); a wedge scaled "
              f"by depth swallows every light into the innermost tiles and reads 0.09")
        if not ok:
            failures.append("ortho-cluster")

    # --- ortho-lod / ortho-lod-height ----------------------------------------
    runs = [_lod_run(workdir, f"ortho{i}", eye, ["--ortho", ORTHO_LOD_HEIGHT])
            for i, eye in enumerate(LOD_SWEEP)]
    off = _lod_run(workdir, "ortho_off", LOD_SWEEP[0], ["--ortho", ORTHO_LOD_HEIGHT, "--no-lod"])
    tall = _lod_run(workdir, "ortho_tall", LOD_SWEEP[1], ["--ortho", ORTHO_LOD_HEIGHT_FAR])
    if any(r is None for r in runs) or off is None or tall is None:
        print("  ortho-lod    ERROR a profiled run failed")
        failures += ["ortho-lod", "ortho-lod-height"]
    else:
        tris = [r["submit"]["opaque"]["triangles"] for r in runs]
        ref = off["submit"]["opaque"]["triangles"]
        ok = all(t == ref for t in tris)
        print(f"  ortho-lod    {'PASS' if ok else 'FAIL'}  triangles across the sweep {tris} "
              f"under --ortho {ORTHO_LOD_HEIGHT} (want all equal to --no-lod's {ref}: projected "
              f"size does not change with distance under a parallel projection; radius over "
              f"distance reads the perspective ladder)")
        if not ok:
            failures.append("ortho-lod")
        # Against the SAME eye at the small height, not against --no-lod: a
        # selection that reads distance and ignores the height gives the same
        # count for both heights at one eye, which is the failure this arm is
        # for, and would still read "fewer than the near eye's level 0".
        t_tall = tall["submit"]["opaque"]["triangles"]
        t_same = tris[1]
        ok = t_tall < t_same
        print(f"  ortho-lod-height {'PASS' if ok else 'FAIL'}  --ortho {ORTHO_LOD_HEIGHT_FAR} "
              f"reads {t_tall} triangles against {t_same} at {ORTHO_LOD_HEIGHT} from the same "
              f"eye (want fewer: a hundred-times-taller view volume is a hundred-times-smaller "
              f"sphere; a selection reading distance alone gives the same count for both)")
        if not ok:
            failures.append("ortho-lod-height")

    # --- ortho-fog-volume -----------------------------------------------------
    scene = asset(FOGVOL_FIXTURE)
    fog_eye, fog_target = ORTHO_FOG_CAM["eye"], ORTHO_FOG_CAM["target"]
    fog_dist = math.sqrt(sum((e - t) ** 2 for e, t in zip(fog_eye, fog_target)))
    fog_height = _ortho_height_matching(ORTHO_FOG_CAM["fovy_deg"], fog_dist)
    ortho_flags = ORTHO_FOG_FLAGS + ["--ortho", f"{fog_height:.4f}"]
    persp = os.path.join(workdir, "ortho_fog_persp.ppm")
    orth = os.path.join(workdir, "ortho_fog_ortho.ppm")
    orth2 = os.path.join(workdir, "ortho_fog_ortho2.ppm")
    err = (render(scene, persp, ORTHO_FOG_FLAGS, frames=FOGVOL_FRAMES) or
           render(scene, orth, ortho_flags, frames=FOGVOL_FRAMES) or
           render(scene, orth2, ortho_flags, frames=FOGVOL_FRAMES))
    if err:
        print(f"  ortho-fog-volume ERROR render failed: {err.strip()[-200:]}")
        failures.append("ortho-fog-volume")
    else:
        floor = compare(orth, orth2)[0]

        def box_over_gap(path):
            w, h, pix = _read_ppm(path)
            luma = {k: sum(_absorb_box_rgb(pix, w, h, b)) / 3.0
                    for k, b in FOGVOL_BOXES.items()}
            return max(luma["left"], luma["right"]) / max(luma["gap"], 1e-6)

        darken_p, darken_o = box_over_gap(persp), box_over_gap(orth)
        parity = darken_o / darken_p if darken_p > 0 else 0.0
        ok = (floor == 0 and darken_o <= FOGVOL_DARKEN_MAX
              and ORTHO_FOG_PARITY_MIN <= parity <= ORTHO_FOG_PARITY_MAX)
        print(f"  ortho-fog-volume {'PASS' if ok else 'FAIL'}  box/gap {darken_o:.4f} under "
              f"ortho against {darken_p:.4f} under perspective from the same eye (want <= "
              f"{FOGVOL_DARKEN_MAX} and the ratio {parity:.3f} within "
              f"{ORTHO_FOG_PARITY_MIN}..{ORTHO_FOG_PARITY_MAX}); floor {floor}. A history "
              f"reprojected by the perspective form reads every cell's past from the centre "
              f"column, and the boxes blend into the gap")
        if not ok:
            failures.append("ortho-fog-volume")

    return failures
