"""The clearcoat group: clearcoat (the coat normal, its map and its storage; spec 11.86)."""

import os

from gatelib.harness import NO_HALOS, asset, render
from gatelib.images import _linear_luma, _read_ppm
from gatelib.probes import _texcomp_probe
from gatelib.scenes import _projector


# Clearcoat, read through --clearcoat-debug (spec 11.86).
#
# The camera is PINNED here where the golden's is auto-framed, and the two are
# not in tension: a golden wants comparability with its sheen and specular
# siblings, an arm wants pixels on the weave. At 3.2 units the spheres project
# ~402 px across on the calibrated framebuffer, which is what lets the
# high-frequency read below resolve a twill cell at all.
CC_FIXTURE = "clearcoat_fixture.gltf"
CC_CAM = {"eye": (0.0, 0.5, 3.2), "target": (0.0, 0.5, 0.0), "fovy_deg": 50.0}
# (name, world x). The fixture recentres to sit on y = 0, so the sphere centres
# land at y = 0.5. The MIDDLE one is the same base material with no coat, and it
# is the control every arm here leans on.
CC_SPHERES = [("car_paint", -1.2), ("car_paint_nocoat", 0.0), ("carbon", 1.2)]
CC_RADIUS = 0.5
# Sampled over the inner 55% of each projected disc, where the geometric normal
# still faces the camera within ~34 degrees. Further out the sphere's own sweep
# dominates every read and the silhouette begins mixing in backdrop.
CC_DISC_FRAC = 0.55
CC_GRID = 24
# Mean BLUE byte over a coated disc. Measured 237.8 on a correct build against
# 17.5 with the Z rebuild removed -- which is 11.85's defect exactly, since BC5
# samples (r, g, 0, 1) and a raw *2-1 decode turns the absent channel into
# z = -1, a coat normal pointing into the surface.
CC_NORMAL_BLUE_MIN = 200.0
# Second difference of the red byte, meaned over the disc: how much the coat
# normal varies at the WEAVE's scale rather than at the sphere's. Measured 5.99
# with the map against 0.40 with it ignored. A plain variance cannot do this job
# -- the sphere's own normal sweep dominates it and reads large whether or not a
# map was ever bound. The stride is a FRACTION of the projected radius, not a
# pixel count, so the read survives a 1x framebuffer.
CC_WEAVE_MIN = 2.0
CC_WEAVE_STRIDE_FRAC = 0.03
# What --no-clearcoat must be worth on a coated sphere, as mean absolute
# per-sample change over its own mean. Measured 0.0420 on the dielectric and
# 0.5870 on the near-black metal, where the coat is most of what is visible.
#
# The coat-free sphere is NOT asserted at exactly zero, and the reason is worth
# recording rather than tightening: it moves 3.1e-05, which is 5 bytes of 1344
# differing by exactly 1. That is the last-bit rounding this repo already
# documents -- the branch is not taken on that material either way, but the
# uniform still changes and the driver schedules the arithmetic differently.
# The bar sits three orders below the dielectric's move and 32x above the
# residual, so it still catches a coat that leaks onto a material with none.
CC_OFF_MIN_DELTA = 0.02
CC_OFF_MAX_CONTROL = 0.001


def _cc_disc(project, cx):
    """Projected centre and FULL radius in pixels of the sphere at world x = cx."""
    px, py = project((cx, 0.5, 0.0))
    ex, _ = project((cx + CC_RADIUS, 0.5, 0.0))
    return px, py, abs(ex - px)


def _cc_byte(pix, w, h, x, y, c):
    x = max(0, min(w - 1, int(round(x))))
    y = max(0, min(h - 1, int(round(y))))
    return pix[(y * w + x) * 3 + c]


def _cc_disc_points(project, cx):
    """The sample grid over one sphere's inner disc, plus the weave stride."""
    px, py, rad = _cc_disc(project, cx)
    inner = rad * CC_DISC_FRAC
    pts = []
    for iy in range(CC_GRID):
        for ix in range(CC_GRID):
            ox = (ix + 0.5) / CC_GRID * 2.0 - 1.0
            oy = (iy + 0.5) / CC_GRID * 2.0 - 1.0
            if ox * ox + oy * oy <= 1.0:
                pts.append((px + ox * inner, py + oy * inner))
    return pts, max(2, int(round(rad * CC_WEAVE_STRIDE_FRAC)))


def _cc_debug_read(pix, w, h, project, cx):
    """(mean rgb, weave contrast) over one sphere's inner disc of a debug frame.

    RAW bytes, never _linear_rgb. A debug mode takes the passthrough blit, so
    these are the shader's own output with no tonemap and no display encode on
    them -- decoding sRGB here would report a stored 0.5 as 0.21, which is the
    trap --cs-debug already carries.
    """
    pts, stride = _cc_disc_points(project, cx)
    sums = [0.0, 0.0, 0.0]
    contrast = 0.0
    for sx, sy in pts:
        for c in range(3):
            sums[c] += _cc_byte(pix, w, h, sx, sy, c)
        contrast += abs(2 * _cc_byte(pix, w, h, sx, sy, 0)
                        - _cc_byte(pix, w, h, sx - stride, sy, 0)
                        - _cc_byte(pix, w, h, sx + stride, sy, 0))
    return [s / len(pts) for s in sums], contrast / len(pts)


def _cc_lit_samples(pix, w, h, project, cx):
    """Per-sample LINEAR luma over one sphere's inner disc of a SHADED frame.

    Per sample and not a mean, because the comparison below has to be of
    absolute differences: the coat adds a tight highlight and takes energy out
    of the base lobe all around it, so differencing two means lets those cancel.
    """
    pts, _ = _cc_disc_points(project, cx)
    return [_linear_luma(pix, w, h, sx, sy) for sx, sy in pts]


def run_clearcoat_gate(workdir):
    """Clearcoat, on a fixture that carried no coverage of any kind until 11.86:

      clearcoat-normal    the coat normal points OUT of the surface -- mean blue
                          over a coated disc, through --clearcoat-debug. This is
                          11.85's defect exactly: the map is BC5, which samples
                          (r, g, 0, 1), and a raw *2-1 decode of the channel that
                          is not there gives z = -1. 237.8 correct, 17.5 broken.
      clearcoat-weave     the coat normal MAP arrived at all. Second difference
                          at the weave's own scale, which the sphere's smooth
                          normal sweep cannot counterfeit: 5.99 with the map and
                          0.40 with it ignored. The arm above reads 240 in that
                          state, so neither substitutes for the other -- and this
                          is the only coverage anywhere of the bespoke import
                          read at aiTextureType_CLEARCOAT index 2, which the
                          index-0 mapping table cannot reach.
      clearcoat-uncoated  the coat-free sphere reads EXACTLY black in all three
                          channels. A normalized vector cannot encode there --
                          the darkest reachable is (-1,-1,-1)/sqrt(3) at 0.211 --
                          so it is unambiguous, and it is what stops both arms
                          above passing on a build that writes the geometric
                          normal everywhere.
      clearcoat-storage   the coat map is stored BC5. No pixel arm can replace
                          this one: untagging it makes the picture BETTER, since
                          a stored z needs no rebuild, while silently deleting
                          the saving the tag exists for.
      clearcoat-off       --no-clearcoat moves both coated spheres and leaves the
                          third where it was. A flag that does nothing passes any
                          arm that only inspects what it was supposed to change,
                          which is this suite's most repeated lesson. Read as
                          mean ABSOLUTE per-sample change: the coat adds a tight
                          highlight and takes energy out of the base lobe around
                          it, so differencing two means lets those cancel and
                          understates the dielectric sphere by 2.6x.

    WHY A PINNED CAMERA WHERE THE GOLDEN AUTO-FRAMES. The two want different
    things and both are right. A golden wants to be comparable with the sheen and
    specular goldens it sits beside, which auto-frame; these arms want the weave
    large enough that a twill cell spans more than a few pixels. Nothing is lost
    by disagreeing -- they are separate renders of the same file.

    WHY THE READS ARE RAW. Everything here but clearcoat-off comes out of a debug
    render mode, and a non-PBR mode takes the passthrough blit: no tonemap, no
    display encode, no dither. These bytes are the shader's own output and the
    sRGB decode every other gate applies would be wrong on them.
    """
    scene = asset(CC_FIXTURE)
    if not os.path.exists(scene):
        print(f"  clearcoat-normal SKIP  ({CC_FIXTURE} not present)")
        return []

    failures = []
    framing = ["-W", "800", "-H", "600", "--no-auto-exposure", "-E", "1.0",
               "--cam-eye", "0,0.5,3.2", "--cam-target", "0,0.5,0"]

    dbg = os.path.join(workdir, "clearcoat_debug.ppm")
    err = render(scene, dbg, framing + ["--clearcoat-debug"])
    if err:
        print("  clearcoat-normal ERROR while rendering the coat-normal view")
        return ["clearcoat-normal"]
    w, h, pix = _read_ppm(dbg)
    project = _projector(CC_CAM, w, h)
    # All three discs out of ONE frame, so a projection off by a texel moves
    # them together and the control stays a control.
    reads = {name: _cc_debug_read(pix, w, h, project, cx) for name, cx in CC_SPHERES}
    coated = [n for n, _ in CC_SPHERES if n != "car_paint_nocoat"]

    blues = {n: reads[n][0][2] for n in coated}
    ok = all(b >= CC_NORMAL_BLUE_MIN for b in blues.values())
    print(f"  clearcoat-normal {'PASS' if ok else 'FAIL'}  mean blue " +
          ", ".join(f"{n} {blues[n]:.1f}" for n in coated) +
          f" (want >= {CC_NORMAL_BLUE_MIN:.0f}; a build that does not rebuild "
          f"Z off BC5 reads 17.5)")
    if not ok:
        failures.append("clearcoat-normal")

    weaves = {n: reads[n][1] for n in coated}
    ok = all(v >= CC_WEAVE_MIN for v in weaves.values())
    print(f"  clearcoat-weave {'PASS' if ok else 'FAIL'}  coat-normal contrast at the weave's "
          "scale " + ", ".join(f"{n} {weaves[n]:.2f}" for n in coated) +
          f" (want >= {CC_WEAVE_MIN:.1f}; with the map ignored it reads 0.40 while the "
          "arm above still reads 240)")
    if not ok:
        failures.append("clearcoat-weave")

    bare = reads["car_paint_nocoat"][0]
    ok = max(bare) == 0.0
    print(f"  clearcoat-uncoated {'PASS' if ok else 'FAIL'}  the coat-free sphere reads "
          f"({bare[0]:.2f}, {bare[1]:.2f}, {bare[2]:.2f}) (want exactly 0,0,0; writing the "
          "geometric normal here instead would read a lit-looking normal and pass every "
          "other arm)")
    if not ok:
        failures.append("clearcoat-uncoated")

    probe = _texcomp_probe(workdir, [], scene=scene)
    if probe is None:
        print("  clearcoat-storage ERROR the texture probe failed to render")
        failures.append("clearcoat-storage")
    else:
        mb, formats, ncomp = probe
        ok = formats == {"BC5"} and ncomp == 1
        print(f"  clearcoat-storage {'PASS' if ok else 'FAIL'}  the coat normal is stored "
              f"{sorted(formats)} at {ncomp} compressed of 1, {mb:.3f} MB (want BC5; "
              "untagging it renders BETTER and loses the saving, which no pixel can report)")
        if not ok:
            failures.append("clearcoat-storage")

    lit = os.path.join(workdir, "clearcoat_lit.ppm")
    off = os.path.join(workdir, "clearcoat_off.ppm")
    # No halos: the coated sphere's highlight would otherwise reach the uncoated
    # control, which then moves with the coat for a reason that is not the coat.
    pair = framing + NO_HALOS
    err = render(scene, lit, pair) or render(scene, off, pair + ["--no-clearcoat"])
    if err:
        print("  clearcoat-off ERROR while rendering the lit pair")
        failures.append("clearcoat-off")
        return failures
    wl, hl, plit = _read_ppm(lit)
    _, _, poff = _read_ppm(off)
    proj = _projector(CC_CAM, wl, hl)
    deltas = {}
    for name, cx in CC_SPHERES:
        a = _cc_lit_samples(plit, wl, hl, proj, cx)
        b = _cc_lit_samples(poff, wl, hl, proj, cx)
        base = sum(a) / len(a)
        deltas[name] = (sum(abs(x - y) for x, y in zip(a, b)) / len(a) / base
                        if base > 0.0 else 0.0)
    ok = (all(deltas[n] >= CC_OFF_MIN_DELTA for n in coated)
          and deltas["car_paint_nocoat"] <= CC_OFF_MAX_CONTROL)
    print(f"  clearcoat-off {'PASS' if ok else 'FAIL'}  --no-clearcoat moves " +
          ", ".join(f"{n} {deltas[n]:.4f}" for n in coated) +
          f" (want >= {CC_OFF_MIN_DELTA}) and the coat-free sphere "
          f"{deltas['car_paint_nocoat']:.2e} (want <= {CC_OFF_MAX_CONTROL}, which is "
          "last-bit rounding rather than the coat)")
    if not ok:
        failures.append("clearcoat-off")

    return failures
