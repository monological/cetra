"""The probe-set group: clustered specular probes (selection, blend, tenancy; spec 11.70)."""

import re
import os

from gatelib.harness import RENDER, _run, asset
from gatelib.images import _linear_luma, _read_ppm
from gatelib.scenes import PROBE_SKY, _cscn_camera, _projector, cscn_copy


# Clustered specular probes (spec 11.70).
#
# The fixture is two rooms over ONE floor mesh, joined by a doorway at the front,
# with a bright panel in the left room and a dim one in the right. Every arm
# below reads that shared floor, because it is the geometry the whole design was
# decided by: per-DRAW probe selection, the cheaper answer this spec refused,
# hands one mesh a single probe and lights half of it with the wrong room's
# reflections -- while every per-room measurement still passes.
#
# Probes need a precomputed IBL to exist at all, and the fixture AUTHORS one --
# an environment block asking for a sky with the sun below the horizon, which
# supplies the IBL without lighting the rooms. Deliberately not passed as flags
# here: a fixture whose gate arms configure it into working, while opening it by
# hand renders something else, is the trap beach_fixture is on record for. So
# the arms pass only the exposure pin every other arm passes.
PROBE_FIXTURE = "cornell_rooms.cscn"

# Floor samples, in WORLD space, projected through the fixture's own camera.
#
# z = 0.55 is in FRONT of the partition, which ends at z = 0.30 (DOOR_Z in the
# generator): the doorway is at the front precisely so this band is visible from
# a viewpoint that sees both rooms. The dark and lit reads are mirrored about
# x = 0, so the pair differs in which room it is in and in nothing else.
PROBE_FLOOR_Z = 0.55
PROBE_DARK_X = 1.30
PROBE_LIT_X = -1.30
PROBE_PATCH = 4  # half-width in pixels of the box each sample averages

# There is deliberately NO seam arm, and it is worth saying why rather than
# leaving the gap silent.
#
# One was built and measured: sweep the floor across the doorway and compare the
# peak curvature in the handover band against a leg whose probe boxes ABUT with
# no overlap, so the two hand over discontinuously. It discriminated at 1.68x on
# an earlier lighting of this fixture, and both blend mutations drove it to
# exactly 1.00. It stopped discriminating when the fixture moved to spots: the
# two probes now agree closely at the doorway, in luma and in hue, which is what
# a doorway between two rooms lit to similar levels means -- so blending and
# switching produce nearly the same pixels there and the arm can no longer fail.
# An arm that cannot fail is worse than an absent one. What still holds the
# blend is probe-set-rooms, which both blend mutations also failed.

# How much brighter the dark room's floor gets when the probe serving it
# photographed the LIT room instead. Both legs run the identical multi path over
# the identical atlas, so this is a reading of selection alone.
# The single probe has to reach the frame at all, or every other thing
# probe-set-single checks is satisfied by a build that binds no probe. Measured
# 366,129 px -- and note these are FRAMEBUFFER pixels, so on a 1x display the
# same frame reads ~91,500. The bar clears both.
PROBE_SINGLE_EFFECT_MIN = 20000
# What moving both boxes out of frame takes away, which is what stops
# probe-set-fallback's identity holding for the wrong reason. Measured 345,230.
PROBE_FALLBACK_REMOVED_MIN = 20000

# How much brighter the dark room's floor gets when the probe serving it
# photographed the LIT room instead. Both legs run the identical multi path over
# the identical atlas, so this is a reading of selection alone.
# Measured 1.82 on the rooms arm and 1.62 under --gi-volume, which the tenancy
# arm reads against this same bar.
PROBE_ROOMS_RATIO_MIN = 1.20
# The lit room is the in-frame control: only probe B moved, so room A has to
# read the same in both legs. A bar rather than exact equality because the two
# legs are separate processes; it has measured 0.0000 every time.
PROBE_ROOMS_CONTROL_MAX = 0.01

_PROBE_SET_ROW = re.compile(
    r"probe-set frame=(\d+) count=(\d+) mode=(\w+) atlas=(\d+)x(\d+) "
    r"captures=(\d+) mask_bits=(\d+) digest=([0-9a-f]+)")


def _probe_set_rows(text):
    """The diagnostic's per-frame lines, as dicts in emission order."""
    return [{"frame": int(m[0]), "count": int(m[1]), "mode": m[2],
             "atlas": (int(m[3]), int(m[4])), "captures": int(m[5]),
             "mask_bits": int(m[6]), "digest": m[7]}
            for m in _PROBE_SET_ROW.findall(text)]


def _probe_set_captured_rows(text):
    """The rows from once the set was captured, and whether it was ever pending past frame 0.

    The engine captures an installed set in the first frame (spec 13.13), after the app's
    update hook has printed frame 0, so that row alone may read "pending". A set still
    pending at any later frame was never captured.
    """
    rows = _probe_set_rows(text)
    late = any(r["mode"] == "pending" and r["frame"] > 0 for r in rows)
    return [r for r in rows if r["mode"] != "pending"], late


def _probe_run(workdir, tag, mutate=None, extra=None, frames=30, fixture=None):
    """Render the two-room fixture, optionally through a mutation.

    Returns (pixels, w, h, output) or (None, None, None, error).
    """
    src = asset(fixture or PROBE_FIXTURE)
    if not os.path.exists(src):
        return None, None, None, "missing fixture"
    scene = src
    if mutate is not None:
        scene = os.path.join(workdir, f"probe_{tag}.cscn")
        cscn_copy(src, scene, mutate)
    out = os.path.join(workdir, f"probe_{tag}.ppm")
    cmd = [RENDER, "-m", scene, "-x", "-f", str(frames), "-W", "400", "-H", "300",
           "-S", out] + PROBE_SKY + (extra or [])
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None, None, None, r.stdout + r.stderr
    w, h, pix = _read_ppm(out)
    return pix, w, h, r.stdout + r.stderr


def _probe_floor_luma(pix, w, h, project, x, z=PROBE_FLOOR_Z):
    """Mean linear luma of a patch centred on a world floor point."""
    px, py = project((x, 0.0, z))
    vals = [_linear_luma(pix, w, h, px + dx, py + dy)
            for dy in range(-PROBE_PATCH, PROBE_PATCH + 1)
            for dx in range(-PROBE_PATCH, PROBE_PATCH + 1)]
    return sum(vals) / len(vals)


def _probe_b_into_room_a(d):
    """The wrong-room leg: probe B keeps its box and photographs room A.

    Deliberately NOT "delete probe B", which would compare the multi path
    against the single-probe path and measure the storage change along with the
    selection. Moving only the capture point leaves everything else identical,
    so what the arm reads is which room a probe photographed and nothing else.
    """
    d["probes"][1]["position"] = list(d["probes"][0]["position"])


def _probe_boxes_offstage(d):
    """Both probes keep their captures and move their INFLUENCE out of frame.

    Every fragment then sits outside every box, so the blend's weight is zero
    everywhere and the whole frame must fall to the global environment -- which
    is the same expression the no-probe branch evaluates.
    """
    for p in d["probes"]:
        p["boxMin"] = [100.0, 100.0, 100.0]
        p["boxMax"] = [101.0, 101.0, 101.0]


def run_probe_set_gate(workdir):
    """N reflection probes, selected and blended per fragment (spec 11.70).

      probe-set-single   one probe keeps the pre-11.70 path: mode=single, no froxel
                         masks built, and two runs byte-identical
      probe-set-rooms    the dark room's floor reflects ITS room. The wrong-room leg
                         moves probe B's capture into room A and nothing else, so the
                         ratio isolates selection; the lit room is the in-frame control
      probe-set-fallback every fragment outside every box IS the no-probe frame
      probe-set-coverage the probe-coverage view (render mode 14) inside a box IS the albedo
                         view, pixel for pixel, and with the boxes moved out of frame the same
                         floor is pure magenta: it shows exactly where reflection falls to the sky
      probe-set-converge captures stop at the probe count and the mask digest repeats
      probe-set-tenancy  the probe atlas and the GI volume share one texture: both
                         effects survive being switched on together
      probe-set-schema   a probe missing its box is refused by name, not defaulted

    NO ARM HERE IS SAFE ALONE, and the pairing is deliberate. probe-set-single
    alone passes on a build that binds no probe at all, since every other thing it
    reads is a diagnostic line or two runs agreeing -- its frame comparison is what
    forces a probe to have reached the pixels. probe-set-rooms alone passes on a
    build that blends nothing, because the dark room is dim anyway -- its control
    read is what forces the lit room to be untouched. And probe-set-fallback alone
    passes on a build where the probes never contribute, which is exactly the
    defect this branch shipped for a day: the fade ran inward from the proxy
    faces, so a floor lying ON the bottom face weighed zero, and the frame looked
    right because a floor reflecting nothing and a floor reflecting a dim room are
    the same picture. Its own anti-vacuity is what closes that.

    Two absences, both deliberate and both recorded above the constants: no seam
    arm (the two probes agree where they hand over on this fixture, so it could
    not fail), and no SSR arm -- SSR shades only what the shadow catcher or wet
    ground marked, the catcher is installed only on scenes authoring no lights of
    their own, this fixture authors two, and it has no rain. The multi-probe SSR
    path is implemented and ungated: the suite's SSR arms are the rainfall group's,
    on a fixture with no probe.
    """
    if not os.path.exists(asset(PROBE_FIXTURE)):
        print(f"  probe-set-single SKIP  {PROBE_FIXTURE} not found")
        return []
    failures = []
    cam = _cscn_camera(PROBE_FIXTURE)

    # -- single: the pre-11.70 path, unmoved -----------------------------------
    def one_probe(d):
        d["probes"] = [d["probes"][0]]

    pix_a, w, h, out_a = _probe_run(workdir, "single_a", one_probe,
                                    ["--probe-set-probe", "10"])
    pix_b, _, _, out_b = _probe_run(workdir, "single_b", one_probe,
                                    ["--probe-set-probe", "10"])
    # The anti-vacuity, and it is not optional: every assertion above is about
    # the diagnostic and about two runs agreeing, and a build that binds no
    # probe at all satisfies all of them perfectly.
    pix_bare, _, _, out_bare = _probe_run(workdir, "single_none",
                                          lambda d: d.pop("probes", None))
    if pix_a is None or pix_b is None or pix_bare is None:
        print(f"  probe-set-single ERROR  {(out_a if pix_a is None else out_b)[-300:]}")
        failures.append("probe-set-single")
    else:
        rows, late = _probe_set_captured_rows(out_a)
        modes = {r["mode"] for r in rows}
        masks = {r["mask_bits"] for r in rows}
        counts = {r["count"] for r in rows}
        identical = pix_a == pix_b
        lit = sum(1 for i in range(0, len(pix_a), 3)
                  if pix_a[i:i + 3] != pix_bare[i:i + 3])
        ok = (bool(rows) and not late and modes == {"single"} and masks == {0}
              and counts == {1} and identical and lit >= PROBE_SINGLE_EFFECT_MIN)
        print(f"  probe-set-single {'PASS' if ok else 'FAIL'}  mode={sorted(modes)} "
              f"count={sorted(counts)} mask_bits={sorted(masks)} want single/1/0; "
              f"two runs identical={identical}; the one probe moves {lit} px against no "
              f"probe, want >={PROBE_SINGLE_EFFECT_MIN}")
        if not ok:
            failures.append("probe-set-single")

    # -- rooms: which room a probe photographed decides the floor --------------
    pix_ok, w, h, out_ok = _probe_run(workdir, "rooms_ok")
    pix_wr, _, _, out_wr = _probe_run(workdir, "rooms_wrong", _probe_b_into_room_a)
    project = _projector(cam, w, h) if pix_ok is not None else None
    if pix_ok is None or pix_wr is None:
        print(f"  probe-set-rooms ERROR  {(out_ok if pix_ok is None else out_wr)[-300:]}")
        failures.append("probe-set-rooms")
        dark_ok = dark_wr = lit_ok = lit_wr = float("nan")
    else:
        dark_ok = _probe_floor_luma(pix_ok, w, h, project, PROBE_DARK_X)
        dark_wr = _probe_floor_luma(pix_wr, w, h, project, PROBE_DARK_X)
        lit_ok = _probe_floor_luma(pix_ok, w, h, project, PROBE_LIT_X)
        lit_wr = _probe_floor_luma(pix_wr, w, h, project, PROBE_LIT_X)
        ratio = dark_wr / dark_ok if dark_ok > 0 else float("inf")
        drift = abs(lit_wr - lit_ok) / max(lit_ok, 1e-6)
        ok = ratio >= PROBE_ROOMS_RATIO_MIN and drift <= PROBE_ROOMS_CONTROL_MAX
        print(f"  probe-set-rooms {'PASS' if ok else 'FAIL'}  dark floor {dark_ok:.4f} -> "
              f"{dark_wr:.4f} when probe B photographs the lit room, ratio {ratio:.4f} "
              f"want >={PROBE_ROOMS_RATIO_MIN}; lit-room control {lit_ok:.4f} vs "
              f"{lit_wr:.4f} drift {drift:.4f} want <={PROBE_ROOMS_CONTROL_MAX}")
        if not ok:
            failures.append("probe-set-rooms")

    # -- fallback: outside every box is the no-probe frame ---------------------
    pix_off, _, _, out_off = _probe_run(workdir, "boxes_off", _probe_boxes_offstage)
    # The probe-free frame is pix_bare, already rendered for probe-set-single's
    # anti-vacuity: same fixture, same mutation, same flags. Rendering it twice
    # is two processes for one image.
    pix_none, out_none = pix_bare, out_bare
    if pix_off is None or pix_none is None:
        print(f"  probe-set-fallback ERROR  {(out_off if pix_off is None else out_none)[-300:]}")
        failures.append("probe-set-fallback")
    else:
        diff = sum(1 for i in range(0, len(pix_off), 3)
                   if pix_off[i:i + 3] != pix_none[i:i + 3])
        # Anti-vacuity: moving the boxes offstage has to have TAKEN something
        # away, or the identity above is between two frames that never had a
        # probe in them and holds for the wrong reason.
        removed = 0 if pix_ok is None else sum(1 for i in range(0, len(pix_off), 3)
                                               if pix_off[i:i + 3] != pix_ok[i:i + 3])
        ok = diff == 0 and removed >= PROBE_FALLBACK_REMOVED_MIN
        print(f"  probe-set-fallback {'PASS' if ok else 'FAIL'}  boxes moved out of frame "
              f"vs no probes at all: {diff} px differ, want 0 (the W=0 remainder is the "
              f"no-probe expression); moving them took {removed} px away, want "
              f">={PROBE_FALLBACK_REMOVED_MIN}")
        if not ok:
            failures.append("probe-set-fallback")

    # -- coverage: the probe-coverage view, against the albedo view ------------
    # The same floor patches in both rooms, each inside its own room's box: through the coverage
    # view they are the albedo view to the pixel, and with the boxes moved out of frame they are
    # pure magenta. The albedo patch must not be magenta itself, or the first half is vacuous.
    covered, w, h, out_cov = _probe_run(workdir, "coverage", None, ["--render-mode", "14"])
    albedo, _, _, out_alb = _probe_run(workdir, "coverage_albedo", None, ["--render-mode", "6"])
    bare_cov, _, _, out_bare_cov = _probe_run(workdir, "coverage_off", _probe_boxes_offstage,
                                              ["--render-mode", "14"])
    if covered is None or albedo is None or bare_cov is None:
        err = out_cov if covered is None else (out_alb if albedo is None else out_bare_cov)
        print(f"  probe-set-coverage ERROR  {err[-300:]}")
        failures.append("probe-set-coverage")
    else:
        project_cov = _projector(cam, w, h)
        idx = []
        for x in (PROBE_LIT_X, PROBE_DARK_X):
            px, py = project_cov((x, 0.0, PROBE_FLOOR_Z))
            px, py = int(round(px)), int(round(py))
            idx += [3 * ((py + dy) * w + px + dx)
                    for dy in range(-PROBE_PATCH, PROBE_PATCH + 1)
                    for dx in range(-PROBE_PATCH, PROBE_PATCH + 1)]
        magenta = bytes((255, 0, 255))
        unlike = sum(1 for i in idx if covered[i:i + 3] != albedo[i:i + 3])
        not_magenta = sum(1 for i in idx if bare_cov[i:i + 3] != magenta)
        albedo_magenta = sum(1 for i in idx if albedo[i:i + 3] == magenta)
        ok = unlike == 0 and not_magenta == 0 and albedo_magenta == 0
        print(f"  probe-set-coverage {'PASS' if ok else 'FAIL'}  inside the boxes {unlike} of "
              f"{len(idx)} floor pixels differ from the albedo view, want 0 ({albedo_magenta} of "
              f"those are magenta themselves, want 0); with the boxes moved out of frame "
              f"{not_magenta} are not pure magenta, want 0")
        if not ok:
            failures.append("probe-set-coverage")

    # -- converge: the sweep runs once and then costs nothing ------------------
    pix_c, _, _, out_c = _probe_run(workdir, "converge", None,
                                    ["--probe-set-probe", "10"], frames=60)
    _, _, _, out_c2 = _probe_run(workdir, "converge2", None,
                                 ["--probe-set-probe", "10"], frames=60)
    rows_c, late_c = _probe_set_captured_rows(out_c)
    rows_c2, _ = _probe_set_captured_rows(out_c2)
    if pix_c is None or not rows_c:
        print(f"  probe-set-converge ERROR  {out_c[-300:]}")
        failures.append("probe-set-converge")
    else:
        caps = {r["captures"] for r in rows_c}
        # The digest is 0 on the frame before the first grid build; every frame
        # that has one must agree, and must agree across processes.
        digs = {r["digest"] for r in rows_c if r["mask_bits"] > 0}
        digs2 = {r["digest"] for r in rows_c2 if r["mask_bits"] > 0}
        # The probe count the run reported, not a literal: the claim is "captures
        # stop AT the probe count", and a fixture that grows a third probe should
        # keep testing that rather than failing for having grown.
        want = rows_c[0]["count"]
        ok = not late_c and caps == {want} and len(digs) == 1 and digs == digs2
        print(f"  probe-set-converge {'PASS' if ok else 'FAIL'}  captures {sorted(caps)} "
              f"want exactly [{want}] (the probe count) across {len(rows_c)} frames, "
              f"captured in the first: {not late_c}; mask digest {sorted(digs)} stable and "
              f"equal across two processes ({sorted(digs2)})")
        if not ok:
            failures.append("probe-set-converge")

    # -- tenancy: one texture, two features ------------------------------------
    #
    # The probe atlas and the GI volume are tenants of ONE texture on unit 14,
    # because pbr_frag has no seventeenth sampler to give either of them. What
    # that has to survive is not "both still change the frame" -- a probe set
    # reading GI's irradiance tiles by mistake changes the frame too, and reads
    # as passing. It is that the probes still tell the two rooms APART with GI
    # live, which is the one thing reading the wrong region destroys.
    pix_gi, _, _, out_gi = _probe_run(workdir, "gi_probes", None, ["--gi-volume"])
    pix_gi_wr, _, _, out_gi_wr = _probe_run(workdir, "gi_wrong", _probe_b_into_room_a,
                                            ["--gi-volume"])
    if pix_gi is None or pix_gi_wr is None or pix_ok is None:
        print(f"  probe-set-tenancy ERROR  {(out_gi if pix_gi is None else out_gi_wr)[-300:]}")
        failures.append("probe-set-tenancy")
    else:
        gi_effect = sum(1 for i in range(0, len(pix_gi), 3)
                        if pix_gi[i:i + 3] != pix_ok[i:i + 3])
        gi_dark_ok = _probe_floor_luma(pix_gi, w, h, project, PROBE_DARK_X)
        gi_dark_wr = _probe_floor_luma(pix_gi_wr, w, h, project, PROBE_DARK_X)
        gi_ratio = gi_dark_wr / gi_dark_ok if gi_dark_ok > 0 else float("inf")
        ok = gi_effect > 0 and gi_ratio >= PROBE_ROOMS_RATIO_MIN
        print(f"  probe-set-tenancy {'PASS' if ok else 'FAIL'}  sharing unit 14: GI moves "
              f"{gi_effect} px with probes live (want >0), and the rooms still read apart "
              f"under it -- dark floor {gi_dark_ok:.4f} vs {gi_dark_wr:.4f}, ratio "
              f"{gi_ratio:.4f} want >={PROBE_ROOMS_RATIO_MIN}")
        if not ok:
            failures.append("probe-set-tenancy")

    # -- schema: a probe without a box is refused, not defaulted ---------------
    def drop_box(d):
        d["probes"][1].pop("boxMax", None)

    _, _, _, out_s = _probe_run(workdir, "schema", drop_box, ["--probe-set-probe", "60"])

    def bad_key(d):
        d["probes"][0]["boxmax"] = [1.0, 1.0, 1.0]

    _, _, _, out_k = _probe_run(workdir, "schema_key", bad_key)
    refused = "needs position, boxMin and boxMax" in out_s
    warned = "is not a recognised probe parameter" in out_k
    rows_s = _probe_set_rows(out_s)
    fell_to_one = bool(rows_s) and {r["count"] for r in rows_s} == {1}
    ok = refused and warned and fell_to_one
    print(f"  probe-set-schema {'PASS' if ok else 'FAIL'}  missing boxMax refused by "
          f"name={refused}, set falls to count 1={fell_to_one}, unknown key warned={warned}")
    if not ok:
        failures.append("probe-set-schema")

    return failures
