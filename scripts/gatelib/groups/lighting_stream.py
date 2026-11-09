"""The lighting-stream and gi-cook groups:

  lighting-stream     lighting data that streams (residency, kept texels, tiles; spec 13.24)
  gi-cook             converged lighting kept on disk (GI sweeps, probe columns; spec 13.42)
"""

import json
import math
import re
import os

from gatelib.harness import RENDER, _import_fixture_gen, _probe_rows, _run, asset
from gatelib.images import _read_ppm, compare
from gatelib.lighting import (
    LSTREAM_FIXTURE, LSTREAM_PROP_ROOM, LSTREAM_TILE_LIGHTS, _lstream_cam_eye, _lstream_pose,
    _lstream_rooms,
)
from gatelib.probes import _COOK_ROW
from gatelib.scenes import CORNELL_FIXTURE, _cscn_camera, _projector, cscn_copy
from gatelib.world import _origin_diff


LSTREAM_W, LSTREAM_H = 400, 300
# The walk: room 0 to room 9 on this frame, and back on the next.
LSTREAM_AWAY_FRAME = 20
LSTREAM_BACK_FRAME = 70
LSTREAM_WALK_FRAMES = 121
# Rows of --stream-probe: in room 0, in room 9 settled, in room 0 again.
LSTREAM_STOPS = (10, 60, 120)
# A streamed item placed in a different slot from the one it would have had samples its texels
# at UVs rounded differently, so two runs with different histories agree to a code, not a bit.
# The round trip is exact (home slots); the walk against a run resident from frame 0 is this.
LSTREAM_EQUAL_PEAK = 1
LSTREAM_EQUAL_FRACTION = 0.005
# Classification has to bring the misaligned twin at least this much nearer its aligned twin,
# in mean frame grey, than the same grid with classification off.
LSTREAM_CLASSIFY_RATIO = 0.5
LSTREAM_CLASSIFY_MIN = 0.5  # grey codes the unclassified grid must be off by, or nothing leaks
LSTREAM_GI_SLOTS = 8
LSTREAM_PROBE_SLOTS = 16
# A volume past its reach leaves the frame as no volume does, but for the probe columns sitting at
# other UVs in an atlas with a GI slot in it: measured 3 px of the 800x600 frame at a code, against
# 486 px with the edge carried out without limit and a zero weight read as black (spec 13.25).
LSTREAM_REACH_FRACTION = 60 / (800 * 600)
# The least budget a frame can have: each kind's first capture goes through and no other, since
# asking again costs more than a microsecond -- one GI probe and one reflection-probe face a
# frame, on every run alike. Frames enough for room 0's eight volumes of 48 probes and its
# sixteen probes' 96 faces at that pace, and for the walk's return to room 0 from the frame it
# came back.
LSTREAM_PACED_BUDGET_MS = "0.001"
LSTREAM_PACED_FRAMES = 600
LSTREAM_PACED_EVERY = 10
LSTREAM_PACED_AWAY = 50  # frames the paced walk spends in room 9
LSTREAM_SINGLE_ARGS = ["--sky", "--sun-elevation", "-10", "--probe-scene"]
LSTREAM_SINGLE_FRAMES = 20
# Two per-frame shadowed spots and probes, with a GI volume of the flag's (spec 13.44). Paced, a
# capture runs every frame, and its rest puts one spot away.
LSTREAM_REST_FIXTURE = "cornell_rooms.cscn"
LSTREAM_REST_LIGHT = "RoomsLampB"
LSTREAM_REST_FRAMES = 60
# Each build of the punctual array the engine logs: its layers, its edge and its light layers.
LSTREAM_ARRAY_BUILD = re.compile(
    r"Punctual shadow array: (\d+) layer\(s\) at (\d+)\^2 .*? (\d+) light layer\(s\)")


def _lstream_builds(text):
    """Every build of the punctual array a run logged, as (layers, edge, light layers)."""
    return [tuple(int(g) for g in m) for m in LSTREAM_ARRAY_BUILD.findall(text)]


def _lstream_cam_at(frame, x):
    eye, target = _lstream_pose(x)
    return ["--cam-at", f"{frame}:" + ",".join(f"{c:g}" for c in eye + target)]


def _lstream_run(workdir, tag, extra, frames, mutate=None, every=None, fixture=LSTREAM_FIXTURE):
    """Render the fixture. Returns (PPM paths by frame number, output) -- the frames
    --screenshot-every wrote when `every` is set, else the last one alone -- or (None, error)."""
    src = asset(fixture)
    scene = src
    if mutate is not None:
        scene = os.path.join(workdir, f"stream_{tag}.cscn")
        cscn_copy(src, scene, mutate)
    out = os.path.join(workdir, f"stream_{tag}.ppm")
    cmd = [RENDER, "-m", scene, "-x", "-f", str(frames), "-W", str(LSTREAM_W), "-H",
           str(LSTREAM_H), "-S", out] + extra
    if every:
        cmd += ["--screenshot-every", str(every)]
    r = _run(cmd, capture_output=True, text=True)
    text = r.stdout + r.stderr
    if r.returncode != 0 or not os.path.exists(out):
        return None, text
    shots = {frames: out}
    if every:
        stem = out[:-4]
        for n in range(every, frames, every):
            path = f"{stem}_{n:06d}.ppm"
            if os.path.exists(path):
                shots[n] = path
    return shots, text


def _lstream_rows(text):
    """--stream-probe's rows by frame: each list's resident slots, every item's fields by index,
    and the cached lights as (name, distance, whole) in the order printed."""
    rows, cur = {}, None
    for rec in _probe_rows(text, "stream"):
        kind = rec.get("kind")
        if "frame" in rec:
            cur = rows.setdefault(int(rec["frame"]), {"gi": {}, "probe": {}, "tile": []})
            if "slots" in rec:
                cur[kind + "_slots"] = [int(s) for s in rec["slots"].split(",") if s]
            if kind == "probes":
                cur["probe_captures"] = int(rec["captures"])
        elif cur is None:
            continue
        elif kind in ("gi", "probe"):
            cur[kind][int(rec["idx"])] = rec
        elif kind == "tile":
            cur["tile"].append((rec["light"], float(rec["dist"]), rec["whole"] == "1"))
    return rows


def _lstream_nearest(boxes, eye, k):
    """The k items nearest the eye by distance to their boxes, ties to the lower index -- the
    residency the engine should arrive at, worked out here from the fixture rather than read
    back from the run it is checking."""
    def dist(box):
        lo, hi = box
        return math.sqrt(sum(max(lo[c] - eye[c], eye[c] - hi[c], 0.0) ** 2 for c in range(3)))
    order = sorted(range(len(boxes)), key=lambda i: (dist(boxes[i]), i))
    return set(order[:k])


def _lstream_patch_mean(shot, x, y, half=4):
    """Mean 8-bit RGB of a square patch centred on a pixel."""
    w, h, pix = shot
    acc = [0.0, 0.0, 0.0]
    n = 0
    for dy in range(-half, half + 1):
        for dx in range(-half, half + 1):
            px, py = int(x) + dx, int(y) + dy
            if 0 <= px < w and 0 <= py < h:
                i = (py * w + px) * 3
                for c in range(3):
                    acc[c] += pix[i + c]
                n += 1
    return [a / max(n, 1) for a in acc]


def _lstream_mean_grey(path):
    pix = _read_ppm(path)[2]
    return sum(pix) / len(pix)


def run_lighting_stream_gate(workdir):
    """Lighting data that streams: GI volumes, reflection probes and cached shadow tiles
    resident by distance, captured once and kept (spec 13.24).

      stream-equal       a room walked into renders as it does resident from frame 0: within a
                         code on under half a percent of the frame, the walk having streamed
      stream-once        room 0 -> room 9 -> room 0: room 0 renders the same to the bit after the
                         round trip, its volumes and probes are uploaded and not captured again
      stream-nearest     at three stops the resident volumes and probes are exactly the nearest
                         eight and sixteen, worked out here from the fixture's boxes
      stream-probes-16   probes past the eighth blend: the coverage view in a room whose probes
                         sit past descriptor 8 IS the albedo view, and each room's floor
                         reflects its own wall colour
      stream-tiles       the cached shadow tiles follow the camera: the nearest sixteen bodied
                         lights hold whole blocks at both ends of the walk and no farther one does
      stream-classify    a grid laid with its probes in the walls lands nearer its aligned twin
                         with classification on than off
      stream-hidden      a capture_hidden node and a skinned mesh are absent from what the probes
                         and the GI volume photographed: their digests match the room without them
      stream-reach       a volume a kilometre off lights room 0 not at all: the frame is the one
                         with no volume, but for the atlas's other size (spec 13.25)
      stream-paced       captures under a capture budget spread over hundreds of frames, the
                         probes taking theirs while the volumes still sweep, and land on the
                         lighting taken all at once: equal digests, the same frame (spec 13.32)
      stream-paced-walk  the walk, paced, leaves room 0 with a probe half captured: it is
                         dropped without a column, and what is resident at the end is the
                         unbudgeted walk's, digests and all
      stream-paced-single a world of one probe, paced a face a frame, is pending where the
                         unbudgeted run is captured, and lands on the same frame
      capture-keeps-shadow-array  captures whose rest puts one of two spots' shadows away keep
                         the punctual array the frame laid out: paced, a capture a frame, the
                         array is built once in sixty frames, at the two-light edge, which a copy
                         with one spot casting shows to differ (spec 13.44)

    The walk teleports, which no player does: it is the worst case for every cap at once -- all
    of room 0's items leave and all of room 9's arrive in one frame -- and a walk can only ever
    cross one boundary at a time.

    Falsified by hand at 13.24 (see the spec): re-capturing on readmit reddens stream-once;
    first-come ranking reddens stream-nearest and stream-tiles; the 8-bit mask reddens
    stream-probes-16; forcing the classification flag to 1 reddens stream-classify; dropping the
    capture skip reddens stream-hidden; letting a capture run before its lights are shadowed
    reddens stream-equal. At 13.25: the edge carried out without limit reddens stream-reach. At
    13.44: a capture choosing the edge from its own layers builds the array 120 times in the sixty
    frames of capture-keeps-shadow-array.
    """
    if not os.path.exists(asset(LSTREAM_FIXTURE)):
        print(f"  stream-equal SKIP  {LSTREAM_FIXTURE} not found")
        return []
    failures = []
    rooms, desc = _lstream_rooms()
    gi_boxes = [(v["boxMin"], v["boxMax"]) for v in desc["giVolumes"]]
    probe_boxes = [(p["boxMin"], p["boxMax"]) for p in desc["probes"]]
    first, last = rooms[0], rooms[-1]

    # One block a frame: room 9's two lights then take two frames to be shadowed, so a capture
    # that did not wait for them would photograph one unshadowed. At the default two the
    # capture's own depth pass places both in the frame it needs them, and the wait is untested.
    walk, walk_text = _lstream_run(
        workdir, "walk",
        _lstream_cam_at(LSTREAM_AWAY_FRAME, last) + _lstream_cam_at(LSTREAM_BACK_FRAME, first)
        + ["--stream-probe", "10", "--tile-blocks-per-frame", "1"], LSTREAM_WALK_FRAMES, every=10)
    rows = _lstream_rows(walk_text) if walk else {}

    # -- equal: the walk's room 9 against room 9 from the start ---------------
    away, away_text = _lstream_run(
        workdir, "away", _lstream_cam_eye(last) + ["--tile-blocks-per-frame", "1"],
        LSTREAM_STOPS[1] + 1)
    if walk is None or away is None:
        print(f"  stream-equal ERROR  {(walk_text if walk is None else away_text)[-300:]}")
        failures.append("stream-equal")
    else:
        walked = walk.get(LSTREAM_STOPS[1])
        frac, peak = (_origin_diff(walked, away[LSTREAM_STOPS[1] + 1]) if walked
                      else (1.0, 255))
        stop = rows.get(LSTREAM_STOPS[1], {})
        streamed = (stop.get("gi", {}).get(len(rooms) - 1, {}).get("state") == "swept"
                    and rows.get(LSTREAM_STOPS[0], {}).get("gi", {}).get(len(rooms) - 1, {})
                    .get("slot") == "-1")
        ok = peak <= LSTREAM_EQUAL_PEAK and frac <= LSTREAM_EQUAL_FRACTION and streamed
        print(f"  stream-equal {'PASS' if ok else 'FAIL'}  room 9 walked into vs resident from "
              f"frame 0: {frac:.3%} of the frame, peak {peak} code(s) (want <= "
              f"{LSTREAM_EQUAL_PEAK} on <= {LSTREAM_EQUAL_FRACTION:.1%}); its volume was out at "
              f"frame {LSTREAM_STOPS[0]} and "
              f"swept by frame {LSTREAM_STOPS[1]}: {streamed}")
        if not ok:
            failures.append("stream-equal")

    # -- once: the round trip ---------------------------------------------------
    if walk is None:
        print("  stream-once ERROR  the walk did not render")
        failures.append("stream-once")
    else:
        left, returned = walk.get(LSTREAM_STOPS[0]), walk.get(LSTREAM_STOPS[2])
        frac, _ = _origin_diff(left, returned) if left and returned else (1.0, 255)
        before, gone, back = (rows.get(s, {}) for s in LSTREAM_STOPS)
        gi_same = all(before.get("gi", {}).get(i, {}).get("captures")
                      == back.get("gi", {}).get(i, {}).get("captures") for i in (0, 1))
        evicted = all(gone.get("gi", {}).get(i, {}).get("slot") == "-1" for i in (0, 1))
        probes_same = gone.get("probe_captures") == back.get("probe_captures")
        ok = frac == 0 and gi_same and evicted and probes_same
        print(f"  stream-once {'PASS' if ok else 'FAIL'}  room 0 before and after: {frac:.3%} of "
              f"the frame differs (want 0); its volumes left residency: {evicted}, came back with "
              f"no capture: "
              f"{gi_same}; probe captures {gone.get('probe_captures')} -> "
              f"{back.get('probe_captures')} on the way back (want equal)")
        if not ok:
            failures.append("stream-once")

    # -- nearest: the residency the boxes say -------------------------------------
    stops_ok, detail = bool(rows), []
    for stop, x in zip(LSTREAM_STOPS, (first, last, first)):
        row = rows.get(stop, {})
        eye, _ = _lstream_pose(x)
        want_gi = _lstream_nearest(gi_boxes, eye, LSTREAM_GI_SLOTS)
        want_probe = _lstream_nearest(probe_boxes, eye, LSTREAM_PROBE_SLOTS)
        got_gi = {s for s in row.get("gi_slots", []) if s >= 0}
        got_probe = {s for s in row.get("probes_slots", []) if s >= 0}
        good = got_gi == want_gi and got_probe == want_probe
        stops_ok = stops_ok and good
        got = "ok" if good else f"gi {sorted(got_gi)} probes {sorted(got_probe)}"
        detail.append(f"frame {stop}: {got}")
    print(f"  stream-nearest {'PASS' if stops_ok else 'FAIL'}  the nearest "
          f"{LSTREAM_GI_SLOTS} volumes and {LSTREAM_PROBE_SLOTS} probes resident: "
          + "; ".join(detail))
    if not stops_ok:
        failures.append("stream-nearest")

    # -- probes past the eighth ---------------------------------------------------
    # The room whose probes sit at descriptor 8 and above when the camera is in it: an 8-bit
    # mask drops exactly them.
    probe_room = len(rooms) - 2
    eye, target = _lstream_pose(rooms[probe_room])
    resident = sorted(_lstream_nearest(probe_boxes, eye, LSTREAM_PROBE_SLOTS))
    own = [resident.index(2 * probe_room), resident.index(2 * probe_room + 1)]
    cover, cover_text = _lstream_run(
        workdir, "cover", _lstream_cam_eye(rooms[probe_room]) + ["--render-mode", "14"], 31)
    albedo, _ = _lstream_run(
        workdir, "albedo", _lstream_cam_eye(rooms[probe_room]) + ["--render-mode", "6"], 31)
    # Projected at the size of the frame read, not the size asked for: _scale_argv renders at
    # twice it, and a patch placed in the smaller frame lands on the wall above the floor.
    tints = {}
    for k in (0, 2):
        shots, _ = _lstream_run(workdir, f"tint{k}", _lstream_cam_eye(rooms[k]), 31)
        if shots:
            shot = _read_ppm(shots[31])
            k_eye, k_target = _lstream_pose(rooms[k])
            project = _projector(_cscn_camera(LSTREAM_FIXTURE, eye=k_eye, target=k_target),
                                 *shot[:2])
            px, py = project((rooms[k], 0.0, -1.0))
            tints[k] = _lstream_patch_mean(shot, px, py)
    if cover is None or albedo is None or len(tints) != 2:
        print(f"  stream-probes-16 ERROR  {cover_text[-300:]}")
        failures.append("stream-probes-16")
    else:
        cover_shot, albedo_shot = _read_ppm(cover[31]), _read_ppm(albedo[31])
        project = _projector(_cscn_camera(LSTREAM_FIXTURE, eye=eye, target=target),
                             *cover_shot[:2])
        same = []
        for fx in (rooms[probe_room] - 1.0, rooms[probe_room] + 1.0):
            px, py = project((fx, 0.0, -1.0))
            same.append(_lstream_patch_mean(cover_shot, px, py)
                        == _lstream_patch_mean(albedo_shot, px, py))
        # Room 0's walls are red and room 2's blue: each floor's reflection leans its own way.
        red, blue = tints[0], tints[2]
        own_colour = red[0] > red[2] and blue[2] > blue[0]
        ok = all(same) and min(own) >= 8 and own_colour
        print(f"  stream-probes-16 {'PASS' if ok else 'FAIL'}  room {probe_room}'s probes at "
              f"descriptors {own} (want >= 8): coverage == albedo on both halves of its floor "
              f"{same}; room 0's floor r/b {red[0]:.0f}/{red[2]:.0f}, room 2's "
              f"{blue[0]:.0f}/{blue[2]:.0f} (each its own wall's colour: {own_colour})")
        if not ok:
            failures.append("stream-probes-16")

    # -- tiles ----------------------------------------------------------------------
    tile_ok, tile_detail = bool(rows), []
    for stop in LSTREAM_STOPS[:2]:
        # Ordered here by the distances printed, not in the order printed: the order is the
        # engine's own ranking, which is the thing in question.
        ranked = sorted(rows.get(stop, {}).get("tile", []), key=lambda t: t[1])
        near = all(whole for _, _, whole in ranked[:LSTREAM_TILE_LIGHTS])
        far = not any(whole for _, _, whole in ranked[LSTREAM_TILE_LIGHTS:])
        good = len(ranked) > LSTREAM_TILE_LIGHTS and near and far
        tile_ok = tile_ok and good
        tile_detail.append(f"frame {stop}: nearest {LSTREAM_TILE_LIGHTS} whole {near}, "
                           f"rest without {far}")
    print(f"  stream-tiles {'PASS' if tile_ok else 'FAIL'}  " + "; ".join(tile_detail))
    if not tile_ok:
        failures.append("stream-tiles")

    # -- classify ---------------------------------------------------------------------
    def unclassified(d):
        d["giVolumes"][-1]["classify"] = False

    aligned, _ = _lstream_run(workdir, "aligned", _lstream_cam_eye(rooms[-2]), 31)
    classified, _ = _lstream_run(workdir, "classified", _lstream_cam_eye(rooms[-1]), 31)
    leaky, leaky_text = _lstream_run(workdir, "leaky", _lstream_cam_eye(rooms[-1]), 31,
                                     mutate=unclassified)
    if aligned is None or classified is None or leaky is None:
        print(f"  stream-classify ERROR  {leaky_text[-300:]}")
        failures.append("stream-classify")
    else:
        a = _lstream_mean_grey(aligned[31])
        c = _lstream_mean_grey(classified[31])
        u = _lstream_mean_grey(leaky[31])
        ok = (abs(u - a) >= LSTREAM_CLASSIFY_MIN
              and abs(c - a) <= LSTREAM_CLASSIFY_RATIO * abs(u - a))
        print(f"  stream-classify {'PASS' if ok else 'FAIL'}  mean grey: aligned {a:.2f}, "
              f"misaligned classified {c:.2f}, unclassified {u:.2f} (want the classified within "
              f"{LSTREAM_CLASSIFY_RATIO:g}x the unclassified's error, which is >= "
              f"{LSTREAM_CLASSIFY_MIN})")
        if not ok:
            failures.append("stream-classify")

    # -- hidden -------------------------------------------------------------------------
    room = rooms[LSTREAM_PROP_ROOM]
    probes = (2 * LSTREAM_PROP_ROOM, 2 * LSTREAM_PROP_ROOM + 1)
    look = _lstream_cam_eye(room) + ["--stream-probe", "30"]
    gone, gone_text = _lstream_run(workdir, "props_gone", look + [
        "--remove-node", "hidden_prop", "--remove-node", "skinned_prop"], 31)
    hidden, hidden_text = _lstream_run(workdir, "props_hidden",
                                       look + ["--capture-hide", "hidden_prop"], 31)
    seen, seen_text = _lstream_run(workdir, "props_seen", look, 31)
    if gone is None or hidden is None or seen is None:
        print(f"  stream-hidden ERROR  {gone_text[-300:]}")
        failures.append("stream-hidden")
    else:
        def digests(text):
            row = _lstream_rows(text).get(31, {})
            return ([row.get("probe", {}).get(i, {}).get("digest") for i in probes]
                    + [row.get("gi", {}).get(LSTREAM_PROP_ROOM, {}).get("digest")])

        d_gone, d_hidden, d_seen = digests(gone_text), digests(hidden_text), digests(seen_text)
        drawn, _ = _origin_diff(gone[31], hidden[31])
        ok = (None not in d_gone and d_hidden == d_gone and d_seen[:2] != d_gone[:2]
              and drawn > 0)
        print(f"  stream-hidden {'PASS' if ok else 'FAIL'}  probe and volume digests with the "
              f"props hidden == with them removed: {d_hidden == d_gone}; with hidden_prop "
              f"photographed they differ: {d_seen[:2] != d_gone[:2]}; the camera still draws "
              f"both: {drawn:.2%} of the frame")
        if not ok:
            failures.append("stream-hidden")

    # -- reach --------------------------------------------------------------------------
    # One small volume a kilometre off and no other: every fragment of room 0 is far past its
    # box, so what lights it is the environment's answer, to the bit the same as no volume.
    def far_volume(d):
        d["giVolumes"] = [{"boxMin": [1000.0, 0.0, 0.0], "boxMax": [1004.0, 3.0, 4.0],
                           "spacing": 1.0}]

    def no_volume(d):
        d["giVolumes"] = []

    far, far_text = _lstream_run(workdir, "far_volume", _lstream_cam_eye(first), 31,
                                 mutate=far_volume)
    none, none_text = _lstream_run(workdir, "no_volume", _lstream_cam_eye(first), 31,
                                   mutate=no_volume)
    if far is None or none is None:
        print(f"  stream-reach ERROR  {(far_text if far is None else none_text)[-300:]}")
        failures.append("stream-reach")
    else:
        frac, peak = _origin_diff(far[31], none[31])
        ok = frac <= LSTREAM_REACH_FRACTION and peak <= 1
        print(f"  stream-reach {'PASS' if ok else 'FAIL'}  room 0 under a volume a kilometre off "
              f"vs no volume: {frac:.4%} of the frame, peak {peak} (want <= "
              f"{LSTREAM_REACH_FRACTION:.4%} at a code: past its reach a volume answers nothing, "
              f"and what is left is the atlas's other size)")
        if not ok:
            failures.append("stream-reach")

    # -- paced: budgeted runs land on the same lighting (spec 13.32) ----------------------
    # At one GI probe and one probe face a frame, the eight volumes and sixteen probes take
    # hundreds of frames where the unbudgeted run takes one. Captures run at render time 0 and
    # an opening writes outright, so the frame a probe or a face was taken in cannot matter:
    # what the two hold at the end is the same to the bit. Printed every frame, for the walk.
    look = ["--stream-probe", "1"]
    budget = ["--capture-budget-ms", LSTREAM_PACED_BUDGET_MS]
    paced, paced_text = _lstream_run(workdir, "paced", look + budget, LSTREAM_PACED_FRAMES)
    whole, whole_text = _lstream_run(workdir, "whole", look, LSTREAM_PACED_FRAMES)

    def held(row):
        return ({i: (r.get("state"), r.get("digest")) for i, r in row.get("gi", {}).items()},
                {i: (r.get("state"), r.get("digest")) for i, r in row.get("probe", {}).items()})

    def settled(row):
        gi, probes = held(row)
        return (bool(gi) and all(s not in ("sweeping", "unswept") for s, _ in gi.values())
                and all(s != "capturing" for s, _ in probes.values()))

    def resident(row):
        return {kind: {i: (r.get("slot"), r.get("state"), r.get("digest"))
                       for i, r in row.get(kind, {}).items() if r.get("slot") != "-1"}
                for kind in ("gi", "probe")}

    half = None
    if paced is None or whole is None:
        print(f"  stream-paced ERROR  {(paced_text if paced is None else whole_text)[-300:]}")
        failures.append("stream-paced")
    else:
        prows, wrows = _lstream_rows(paced_text), _lstream_rows(whole_text)
        end = LSTREAM_PACED_FRAMES
        same = bool(prows.get(end)) and held(prows[end]) == held(wrows.get(end, {}))
        early = LSTREAM_PACED_EVERY
        spread = settled(wrows.get(early, {})) and not settled(prows.get(early, {}))
        # Each kind takes a unit a frame, so the probes are not held behind the GI: the first
        # probe lands while volumes are still sweeping.
        landed = next((f for f in sorted(prows)
                       if any(r.get("state") == "loaded" for r in prows[f]["probe"].values())),
                      None)
        beside = landed is not None and any(
            r.get("state") in ("sweeping", "unswept") for r in prows[landed]["gi"].values())
        frac, peak = _origin_diff(paced[end], whole[end])
        ok = same and spread and beside and settled(prows.get(end, {})) and frac == 0
        print(f"  stream-paced {'PASS' if ok else 'FAIL'}  a {LSTREAM_PACED_BUDGET_MS} ms capture "
              f"budget against none: still capturing at frame {early}: {spread} (the unbudgeted "
              f"run is done); the first probe landed at frame {landed} with volumes still "
              f"sweeping: {beside}; at frame {end} every volume and probe digest equal: {same}, "
              f"the frame {frac:.3%} apart, peak {peak} (want 0)")
        if not ok:
            failures.append("stream-paced")
        # The first frame a probe stands half captured, which the walk leaves room 0 just after.
        half = next(((f, i) for f in sorted(prows) for i, r in sorted(prows[f]["probe"].items())
                     if r.get("faces") == "3"), None)

    # The walk, paced, leaving room 0 with a probe half captured and a volume part swept: both
    # are abandoned, their cubes and faces dropped, and both are taken again on the way back.
    # What is resident at the end is what the unbudgeted walk holds at its end.
    if half is None or walk is None:
        print("  stream-paced-walk ERROR  "
              + ("no probe stood half captured in the paced run" if walk else "the walk failed"))
        failures.append("stream-paced-walk")
    else:
        frame, probe = half
        away, back = frame + 1, frame + 1 + LSTREAM_PACED_AWAY
        pwalk, pwalk_text = _lstream_run(
            workdir, "paced_walk",
            _lstream_cam_at(away, last) + _lstream_cam_at(back, first) + look + budget,
            back + LSTREAM_PACED_FRAMES)
        # A row is the residency as the frame before left it: `away`'s is what the teleport
        # meets, the next one's what it did.
        walk_rows = _lstream_rows(pwalk_text) if pwalk else {}
        before = walk_rows.get(away, {}).get("probe", {}).get(probe, {})
        after = walk_rows.get(away + 1, {}).get("probe", {}).get(probe, {})
        part = before.get("faces") in ("1", "2", "3", "4", "5")
        dropped = (part and after.get("slot") == "-1" and after.get("faces") == "0"
                   and after.get("kept") == "0")
        end_row = walk_rows.get(back + LSTREAM_PACED_FRAMES, {})
        want = resident(rows.get(LSTREAM_STOPS[2], {}))
        got = resident(end_row)
        same = bool(want["gi"]) and bool(want["probe"]) and got == want
        ok = pwalk is not None and dropped and settled(end_row) and same
        print(f"  stream-paced-walk {'PASS' if ok else 'FAIL'}  the walk left room 0 at frame "
              f"{away} with probe {probe} {before.get('faces')} faces of 6 in: dropped without a "
              f"column {dropped}; at frame {back + LSTREAM_PACED_FRAMES} the resident volumes "
              f"and probes are the unbudgeted walk's, digests and all: {same}")
        if not ok:
            failures.append("stream-paced-walk")

    # One probe, which a world of one captures with no column: paced, it is still pending a few
    # frames in, and the frame it lands on is the unbudgeted one's.
    single = LSTREAM_SINGLE_ARGS + ["--probe-set-probe", "1"]
    one_paced, one_paced_text = _lstream_run(workdir, "single_paced", single + budget,
                                             LSTREAM_SINGLE_FRAMES, fixture=CORNELL_FIXTURE)
    one_whole, one_whole_text = _lstream_run(workdir, "single_whole", single,
                                             LSTREAM_SINGLE_FRAMES, fixture=CORNELL_FIXTURE)
    if one_paced is None or one_whole is None:
        text = one_paced_text if one_paced is None else one_whole_text
        print(f"  stream-paced-single ERROR  {text[-300:]}")
        failures.append("stream-paced-single")
    else:
        def modes(text):
            return {int(r["frame"]): r.get("mode") for r in _probe_rows(text, "probe-set")
                    if "frame" in r}

        pm, wm = modes(one_paced_text), modes(one_whole_text)
        early = 3
        spread = pm.get(early) == "pending" and wm.get(early) == "single"
        end = LSTREAM_SINGLE_FRAMES
        frac, peak = _origin_diff(one_paced[end], one_whole[end])
        ok = spread and pm.get(end) == "single" and frac == 0
        print(f"  stream-paced-single {'PASS' if ok else 'FAIL'}  one scene probe a face a frame: "
              f"pending at frame {early}: {pm.get(early)} (unbudgeted: {wm.get(early)}); at frame "
              f"{end} {pm.get(end)}, the frame {frac:.3%} apart, peak {peak} (want 0)")
        if not ok:
            failures.append("stream-paced-single")

    # A rest that puts one of two spots' shadows away, as an app puts its flashlight away: each
    # capture's own pass then has one per-frame layer where the frame has two. The frame decides
    # the edge before anything captures and every capture keeps it, so the array is built once, at
    # the two-light edge, where an edge chosen by the capture rebuilt it there and again in the
    # frame, every paced frame. A copy whose second spot never casts measures the one-light edge,
    # so the premise that it differs is read rather than assumed from the budget, and the hook's
    # own count says it ran in every capture.
    if not os.path.exists(asset(LSTREAM_REST_FIXTURE)):
        print(f"  capture-keeps-shadow-array SKIP  {LSTREAM_REST_FIXTURE} not found")
        return failures

    def one_spot(d):
        for light in d["lights"]:
            if light.get("name") == LSTREAM_REST_LIGHT:
                light["cast_shadows"] = False

    rest = ["--gi-volume", "--capture-rest-unshadow", LSTREAM_REST_LIGHT,
            "--stream-probe", str(LSTREAM_REST_FRAMES)]
    rest_shots, rest_text = _lstream_run(workdir, "rest_unshadow", rest + budget,
                                         LSTREAM_REST_FRAMES, fixture=LSTREAM_REST_FIXTURE)
    one_shots, one_text = _lstream_run(workdir, "rest_one_spot", ["--gi-volume"], 2,
                                       mutate=one_spot, fixture=LSTREAM_REST_FIXTURE)
    if rest_shots is None or one_shots is None:
        print(f"  capture-keeps-shadow-array ERROR  "
              f"{(rest_text if rest_shots is None else one_text)[-300:]}")
        failures.append("capture-keeps-shadow-array")
    else:
        builds, alone = _lstream_builds(rest_text), _lstream_builds(one_text)
        rests = re.search(r"capture-rest-unshadow rests=(\d+)", rest_text)
        rests = int(rests.group(1)) if rests else 0
        vol = _lstream_rows(rest_text).get(LSTREAM_REST_FRAMES, {}).get("gi", {}).get(0, {})
        captures = int(vol.get("captures", 0))
        # One GI probe a frame, the volume still sweeping, and the hook holding every capture.
        paced = (vol.get("state") == "sweeping" and captures == LSTREAM_REST_FRAMES
                 and rests >= captures)
        premise = len(alone) == 1 and alone[0][2] == 1
        kept = (premise and len(builds) == 1 and builds[0][2] == 2
                and builds[0][1] != alone[0][1])
        ok = kept and paced
        print(f"  capture-keeps-shadow-array {'PASS' if ok else 'FAIL'}  a capture a frame for "
              f"{LSTREAM_REST_FRAMES} frames, its rest putting '{LSTREAM_REST_LIGHT}' away "
              f"({rests} rests, {captures} GI probes, volume {vol.get('state')}): the array built "
              f"{builds} (want once, as (layers, edge, light layers), with 2 light layers), "
              f"against one spot's edge {alone[0][1] if premise else alone} (want it to differ)")
        if not ok:
            failures.append("capture-keeps-shadow-array")

    return failures


# Converged lighting kept on disk (spec 13.42), on the lighting-stream fixture: room 0's eight
# resident volumes and sixteen probes, swept and captured unbudgeted in these frames.
GICOOK_FRAMES = 12
GICOOK_SITES = ("gi-volume/1", "probe-column/1")
# Room 0's coloured walls: what the material, node and texture changes touch, textured so that a
# texture's bytes are among what the key must see.
GICOOK_MATERIAL = "room0_colour"
GICOOK_TEXTURE = "gicook_room0.png"
GICOOK_COLOURS = ((200, 40, 30), (30, 40, 200))  # the texture, and its bytes changed
# A paced run sweeps room 0's first volume a probe a frame over its 48, so a material changed on
# this frame lands in the middle of that sweep.
GICOOK_UNSTABLE_FRAME = 20
_GICOOK_UNSTABLE = re.compile(r"gi-cook volume=(\d+) result=unstable")


def _gicook_png(path, rgb):
    """A 4x4 PNG of one colour, through the emissive fixture's writer."""
    gen = _import_fixture_gen("gen_emissive_fixture.py", group="gi-cook")
    with open(path, "wb") as f:
        f.write(gen.png_rgb(4, 4, [[rgb] * 4] * 4))


def _gicook_textured(workdir):
    """The fixture with room 0's coloured walls textured from GICOOK_TEXTURE, written beside the
    copied model in the first colour. Returns the scene's path."""
    _gicook_png(os.path.join(workdir, GICOOK_TEXTURE), GICOOK_COLOURS[0])
    with open(asset("stream_rooms.gltf")) as f:
        g = json.load(f)
    g["images"] = [{"uri": GICOOK_TEXTURE}]
    g["textures"] = [{"source": 0}]
    for m in g["materials"]:
        if m.get("name") == GICOOK_MATERIAL:
            m["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}
    gltf = os.path.join(workdir, "gicook_textured.gltf")
    with open(gltf, "w") as f:
        json.dump(g, f)

    def mutate(d):
        d["models"] = [{"path": gltf}]

    return cscn_copy(asset(LSTREAM_FIXTURE), os.path.join(workdir, "gicook_textured.cscn"), mutate)


def _gicook_rows(text, result):
    """{(site, name): key} of a run's GI and probe cook rows with this result."""
    return {(site, name): key for site, name, res, _bytes, key in _COOK_ROW.findall(text)
            if site in GICOOK_SITES and res == result}


def _gicook_count(rows):
    """(volumes, probe columns) among a run's GI and probe cook rows."""
    volumes = sum(1 for site, _name in rows if site == GICOOK_SITES[0])
    return volumes, len(rows) - volumes


def run_gi_cook_gate(workdir):
    """Converged lighting kept on disk (spec 13.42): a GI volume's opening sweep and a reflection
    probe's column stored by the cook under a key folded from the scene at rest, and loaded by the
    next run of the same scene -- the lighting-stream fixture, room 0's walls textured, with
    --gi-cook.

      gi-cook-hit       a second run against the first's directory captures nothing: every volume
                        and probe the first stored it loads by the same key, it captures no GI
                        probe and no reflection probe, and its frame is the first's to the bit
      gi-cook-pure      the scene paced a probe a frame keys every capture as the unbudgeted run
                        did, though each lands on another frame
      gi-cook-miss      against the warm directory, each kind of change sweeps volumes and
                        captures columns anew and stores them: a node taken out, a material's
                        roughness, a light's intensity, a light moved 30 cm, the sun, and a
                        texture's bytes changed under the same file name
      gi-cook-unstable  a material changed in the middle of a paced sweep: the volume being swept
                        says so, and is not stored

    Every run has its own cook directory under the workdir rather than the suite's shared one, as
    in the cook group: what is in it must be what this group put there.
    """
    if not os.path.exists(asset(LSTREAM_FIXTURE)):
        print(f"  gi-cook-hit SKIP  {LSTREAM_FIXTURE} not found")
        return []
    failures = []
    textured = _gicook_textured(workdir)

    def run(tag, extra, frames=GICOOK_FRAMES, mutate=None):
        shots, text = _lstream_run(workdir, f"gicook_{tag}",
                                   extra + ["--stream-probe", str(frames)], frames,
                                   mutate=mutate, fixture=textured)
        if shots is None:
            print(f"  gi-cook      ERROR {tag}: {text[-300:]}")
        return shots, text

    def cook(tag):
        return ["--gi-cook", "--cook-dir", os.path.join(workdir, f"gicook_{tag}")]

    paced = ["--capture-budget-ms", LSTREAM_PACED_BUDGET_MS]

    def rougher(frame):
        return ["--material-at", str(frame), GICOOK_MATERIAL, "roughness", "0.5"]

    # --- gi-cook-hit --------------------------------------------------------
    cold, cold_text = run("cold", cook("a"))
    warm, warm_text = run("warm", cook("a"))
    if cold is None or warm is None:
        return ["gi-cook"]
    stored = _gicook_rows(cold_text, "cooked")
    loaded = _gicook_rows(warm_text, "hit")
    again = _gicook_rows(warm_text, "cooked")
    row = _lstream_rows(warm_text).get(GICOOK_FRAMES, {})
    gi_captures = sum(int(r["captures"]) for r in row.get("gi", {}).values())
    probe_captures = row.get("probe_captures", -1)
    volumes, columns = _gicook_count(stored)
    ae, _peak = compare(cold[GICOOK_FRAMES], warm[GICOOK_FRAMES])
    ok = (volumes > 0 and columns > 0 and loaded == stored and not again
          and gi_captures == 0 and probe_captures == 0 and ae == 0)
    print(f"  gi-cook-hit {'PASS' if ok else 'FAIL'}  the cold run stored {volumes} volumes and "
          f"{columns} probe columns; the warm run loaded {len(loaded)} by the same "
          f"keys: {loaded == stored}, stored {len(again)}, captured {gi_captures} GI probes and "
          f"{probe_captures} reflection probes, and its frame is {ae} px from the cold run's "
          f"(want both stored, all loaded, then 0, 0, 0 and 0 px)")
    if not ok:
        failures.append("gi-cook-hit")

    # --- gi-cook-pure -------------------------------------------------------
    paced_shots, paced_text = run("paced", cook("p") + paced, frames=LSTREAM_PACED_FRAMES)
    keyed = _gicook_rows(paced_text, "cooked")
    apart = sorted(name for (site, name), key in stored.items() if keyed.get((site, name)) != key)
    ok = paced_shots is not None and bool(stored) and keyed == stored
    print(f"  gi-cook-pure {'PASS' if ok else 'FAIL'}  paced a probe a frame over "
          f"{LSTREAM_PACED_FRAMES} frames, {len(stored) - len(apart)} of the unbudgeted run's "
          f"{len(stored)} captures stored under the same key"
          + (f"; apart: {', '.join(apart[:8])}" if apart else "") + " (want all)")
    if not ok:
        failures.append("gi-cook-pure")

    # --- gi-cook-miss -------------------------------------------------------
    def brighter(d):
        d["lights"][0]["intensity"] *= 1.5

    def moved(d):
        d["lights"][0]["position"][0] += 0.3

    def sun(d):
        d["environment"]["sun"]["azimuth"] = 30.0

    changes = [("node", ["--remove-node", GICOOK_MATERIAL], None),
               ("material", rougher(0), None),
               ("light", [], brighter),
               ("light-moved", [], moved),
               ("sun", [], sun)]
    swept = {}
    for name, extra, mutate in changes:
        _shots, text = run(f"miss_{name}", cook("a") + extra, mutate=mutate)
        swept[name] = _gicook_count(_gicook_rows(text, "cooked"))
    # Last, since it leaves the texture changed for every run after it.
    _gicook_png(os.path.join(workdir, GICOOK_TEXTURE), GICOOK_COLOURS[1])
    _shots, text = run("miss_texture", cook("a"))
    swept["texture"] = _gicook_count(_gicook_rows(text, "cooked"))
    ok = all(volumes > 0 and columns > 0 for volumes, columns in swept.values())
    print(f"  gi-cook-miss {'PASS' if ok else 'FAIL'}  volumes and columns stored anew: "
          + ", ".join(f"{name} {v}/{c}" for name, (v, c) in swept.items())
          + " (want both above 0 for every change)")
    if not ok:
        failures.append("gi-cook-miss")

    # --- gi-cook-unstable ---------------------------------------------------
    shaken, text = run("unstable", cook("u") + paced + rougher(GICOOK_UNSTABLE_FRAME),
                       frames=LSTREAM_PACED_FRAMES)
    unstable = sorted(set(_GICOOK_UNSTABLE.findall(text)), key=int)
    kept = sorted(int(name.split("-")[1]) for site, name in _gicook_rows(text, "cooked")
                  if site == GICOOK_SITES[0])
    leaked = [v for v in unstable if int(v) in kept]
    ok = (shaken is not None and unstable == ["0"] and not leaked
          and len(kept) == volumes - len(unstable))
    print(f"  gi-cook-unstable {'PASS' if ok else 'FAIL'}  roughness changed on frame "
          f"{GICOOK_UNSTABLE_FRAME} of a paced sweep: unstable {unstable or 'none'}, stored "
          f"{kept or 'none'} (want volume 0 alone unstable, the one being swept then, and every "
          f"other volume stored, each having begun after it)")
    if not ok:
        failures.append("gi-cook-unstable")

    return failures
