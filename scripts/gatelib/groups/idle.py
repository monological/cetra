"""The idle group: idle mode (a frame only when something asks: still, timed, thread, input;
14.1).
"""

import os

from gatelib.harness import asset, render
from gatelib.images import compare
from gatelib.probes import _canvas_run, _canvas_shot


def _idle_run(workdir, tag, *args, frames=50, idle=True):
    """apps/canvas under --idle unless told otherwise: its picture's path, and from its idle lines
    each frame's delta_time and wall clock, the run's frames drawn and seconds waited, the size of a
    texture it loaded, the frames its late work was begun and landed in, and how often a full list
    of timed wakes was warned about, as {"deltas": [...], "at": [...], "frames": n, "waited": s,
    "loaded": (w, h), "begun": n, "landed": n, "wakes_full": n}; None and an empty dict for a run
    that failed."""
    out = os.path.join(workdir, f"idle_{tag}.ppm")
    stdout = _canvas_shot(out, frames, *(["--idle"] if idle else []), *args, log=True)
    if stdout is None:
        return None, {}
    said = {"deltas": [], "at": [], "wakes_full": 0}
    for line in stdout.splitlines():
        p = line.split()
        if "timed wakes are held, the most" in line:
            said["wakes_full"] += 1
        elif p[:3] == ["canvas", "idle", "frame"] and len(p) == 6:
            said["deltas"].append(float(p[5]))
        elif p[:3] == ["canvas", "idle", "at"] and len(p) == 5:
            said["at"].append(float(p[4]))
        elif p[:3] == ["canvas", "idle", "frames"] and len(p) == 6:
            said["frames"], said["waited"] = int(p[3]), float(p[5])
        elif p[:3] == ["canvas", "idle", "loaded"] and len(p) == 5:
            said["loaded"] = (int(p[3]), int(p[4]))
        elif p[:3] == ["canvas", "idle", "work"] and len(p) == 5:
            said[p[3]] = int(p[4])
    return out, said


def run_idle_gate(workdir):
    """Idle mode (spec 14.1): with it on, the engine draws a frame only when something asks for
    one, and waits for events in between. Headless there are no events, and a run ends at the
    first frame after which nothing asks for another and no work is out; apps/canvas prints each
    frame's delta_time and, at exit, the frames drawn and the seconds waited. The noise floor is
    0 px: two runs of one sheet are identical.

      idle-still     a still sheet under --idle -f 50 draws 1 frame and ends, and its picture is
                     the sheet drawn for one frame without idle mode, 0 px.
      idle-timed     two timed wakes, 0.2 s and 0.3 s, each asked for on the frame before: 3
                     frames, from the first to the last at least the 0.5 s asked for, and each
                     delta_time under the wake it followed. The wait is kept out of the clock: a
                     frame after a wait advances what animates by a frame and not by the wait.
      idle-wakes-kept every timed wake asked for is kept (spec 14.9): 0.6 s and 0.2 s asked on
                     one frame draw a frame at or after each and only then end, where one deadline
                     ended after the first; 0.3 s and 0.3005 s are one wake, drawn at or after the
                     later; and 65 asked on one frame are warned about once, the last of them, past
                     the 64 held, moving the latest later rather than being dropped -- the run's
                     last frame is at or after its 1 s.
      idle-thread    the board filled on a thread started on the frame a 0.5 s wake draws, between
                     engine_begin_work and engine_end_work: the loop waits rather than drawing, 3
                     frames, and the last is the board drawn without idle mode, 0 px. A headless
                     run does not end while work is out. The thread is started past the first
                     tenth of a second: something a hidden window is sent then wakes a waiting
                     loop, and a board ready inside it was drawn with the empty event removed.
      idle-pointer   a scripted drag across the shapes sheet: a frame each frame the script
                     plays and one for its last, none after, and the last is the same drag
                     without idle mode at that frame, 0 px. The drag must move the sheet, or two
                     still frames would agree.
      idle-3d        apps/render --idle on a 3D fixture draws every one of its frames -- a scene
                     with anything in it keeps the loop drawing -- and its last is the fixture
                     without idle mode, 0 px.
      idle-ui        a UI panel fading in over 0.25 s with a button in it taking focus: frames
                     while the fade plays and the focus eases, then the end, within 60 frames of
                     the 200 allowed. The ease snaps to its target within a quarter of a code;
                     an exponential approach that never arrived held the loop to the last frame.
      idle-gui       a click handed to the debug GUI on the first frame: at least 4 frames, as
                     ImGui takes the press and the release a frame apart and shows the second a
                     frame later, then the end, within 10. With nothing to ask for them the run
                     ended at the first, and with no frame after the one that took the release, at
                     the third.
      idle-load      a 2048x1608 texture asked for on the frame a 0.5 s wake draws: the loop waits
                     while it decodes rather than drawing, 3 frames, the third delivering it. The
                     loader's thread wakes the loop when the decode is done.
      idle-type      the palette opened, typed into, moved in and closed by a script (spec 14.2):
                     a frame for each of its eight frames, and the end within 30, since a caret
                     headless does not blink and nothing else is left to ask.
      idle-editor    the code editor on a 24 MB text (spec 14.4): its budget wraps the rows it
                     guessed a frame's share at a time and asks for each frame, so the run draws
                     over 30 frames more than one on a line of text -- whose frames are the UI's
                     own eases -- and ends on its own once every row is wrapped, short of the 400
                     allowed.
      idle-work-waits headless, a frame waits for the work out (spec 14.7): the board's thread,
                     begun as the first frame draws and made slow on purpose, lands in the second
                     at a pause of 0 ms and of 300 ms alike, while a script keeps every frame
                     drawing. Before, a script's frames drew on through the work.
      engine-close-hook the window's close, which the app may refuse (spec 14.7), asked by an
                     input script's close as the close button asks it: refused the first time and
                     taken the second, the run ending on the frame of the second; with no hook
                     the first ends it; and a close over four frames asked once, on the first.
    """
    failed = []

    def note(name, ok):
        if not ok:
            failed.append(name)

    still, said = _idle_run(workdir, "still", "--sheet", "shapes")
    plain, _ = _idle_run(workdir, "still_plain", "--sheet", "shapes", frames=1, idle=False)
    if not still or not plain or "frames" not in said:
        ok, detail = False, "a run failed"
    else:
        ae, _ = compare(plain, still)
        ok = said["frames"] == 1 and ae == 0
        detail = f"{said['frames']} frame(s) drawn of 50 (want 1), {ae} px from one plain frame"
    print(f"  idle-still          {'PASS' if ok else 'FAIL'}  {detail}")
    note("idle-still", ok)

    _, said = _idle_run(workdir, "timed", "--sheet", "shapes", "--idle-wake", "0.2,0.3")
    d = said.get("deltas", [])
    if "frames" not in said or len(d) != 3:
        ok, detail = False, f"a run failed or printed {len(d)} frames"
    else:
        span = d[1] + d[2] + said["waited"]
        ok = said["frames"] == 3 and span >= 0.5 and d[1] < 0.2 and d[2] < 0.3
        detail = (f"{said['frames']} frames (want 3), {span:.3f} s from the first to the last "
                  f"(want >= 0.5), delta_time {d[1]:.4f} and {d[2]:.4f} s after the waits "
                  f"(want under 0.2 and 0.3), {said['waited']:.3f} s waited")
    print(f"  idle-timed          {'PASS' if ok else 'FAIL'}  {detail}")
    note("idle-timed", ok)

    # Each run's frame 1 asks; its wall clock is taken just before, so a frame at or past the ask's
    # time plus a wait is at or past that wait.
    _, two = _idle_run(workdir, "wakes_two", "--sheet", "shapes", "--idle-wake", "0.6+0.2")
    _, near = _idle_run(workdir, "wakes_near", "--sheet", "shapes", "--idle-wake", "0.3+0.3005")
    many = [round(0.02 + 0.01 * k, 2) for k in range(64)] + [1.0]
    _, full = _idle_run(workdir, "wakes_full", "--sheet", "shapes", "--idle-wake",
                        "+".join(str(t) for t in many), frames=200)
    if any("frames" not in s or len(s["at"]) != s["frames"] for s in (two, near, full)):
        ok, detail = False, "a run failed"
    else:
        after = [[t - s["at"][0] for t in s["at"][1:]] for s in (two, near, full)]
        last = after[2][-1] if after[2] else 0.0
        ok = (two["frames"] == 3 and after[0][0] >= 0.2 and after[0][1] >= 0.6
              and near["frames"] == 2 and after[1][0] >= 0.3005
              and full["wakes_full"] == 1 and last >= 1.0)
        detail = (f"0.6+0.2: {two['frames']} frames (want 3) at "
                  f"{', '.join(f'{t:.3f}' for t in after[0])} s; 0.3+0.3005: {near['frames']} "
                  f"(want 2) at {', '.join(f'{t:.4f}' for t in after[1])} s (want >= 0.3005); "
                  f"65 times: warned {full['wakes_full']} (want 1), {full['frames']} frames, the "
                  f"last at {last:.3f} s (want >= 1)")
    print(f"  idle-wakes-kept     {'PASS' if ok else 'FAIL'}  {detail}")
    note("idle-wakes-kept", ok)

    late, said = _idle_run(workdir, "thread", "--sheet", "board", "--edit-thread", "--idle-wake",
                           "0.5")
    board, _ = _idle_run(workdir, "board_plain", "--sheet", "board", frames=2, idle=False)
    if not late or not board or "frames" not in said:
        ok, detail = False, "a run failed"
    else:
        ae, _ = compare(board, late)
        ok = said["frames"] == 3 and said["waited"] > 0.5 and ae == 0
        detail = (f"{said['frames']} frames (want 3), {said['waited']:.3f} s waited for the wake "
                  f"and the thread (want > 0.5), {ae} px from the board drawn plainly (want 0)")
    print(f"  idle-thread         {'PASS' if ok else 'FAIL'}  {detail}")
    note("idle-thread", ok)

    script = os.path.join(workdir, "idle_drag.txt")
    with open(script, "w") as f:
        f.write("2 at=600,400\n3-10 down by=12,-6\n")
    dragged, said = _idle_run(workdir, "drag", "--sheet", "shapes", "--input-script", script)
    played = 12  # the script's frames 0 to 10, played after frames 1 to 11, and the frame after
    plain_drag, _ = _idle_run(workdir, "drag_plain", "--sheet", "shapes", "--input-script",
                              script, frames=played, idle=False)
    if not dragged or not plain_drag or not plain or "frames" not in said:
        ok, detail = False, "a run failed"
    else:
        ae, _ = compare(plain_drag, dragged)
        moved, _ = compare(plain, dragged)
        ok = said["frames"] == played and ae == 0 and moved > 1000
        detail = (f"{said['frames']} frames (want {played}), {ae} px from the drag without idle "
                  f"mode (want 0), {moved} px moved from the still sheet (want > 1000)")
    print(f"  idle-pointer        {'PASS' if ok else 'FAIL'}  {detail}")
    note("idle-pointer", ok)

    fixture = asset("mask_fixture.cscn")
    shot = os.path.join(workdir, "idle_3d.ppm")
    shot_plain = os.path.join(workdir, "idle_3d_plain.ppm")
    last = os.path.join(workdir, "idle_3d_000012.ppm")
    failure = (render(fixture, shot, ["--idle", "--screenshot-every", "12"], frames=12)
               or render(fixture, shot_plain, [], frames=12))
    if failure:
        ok, detail = False, f"a run failed: {failure[-300:]}"
    else:
        ae, _ = compare(shot_plain, shot)
        ok = os.path.exists(last) and ae == 0
        detail = (f"frame 12 {'drawn' if os.path.exists(last) else 'never drawn'} of 12, "
                  f"{ae} px from the fixture without idle mode (want 0)")
    print(f"  idle-3d             {'PASS' if ok else 'FAIL'}  {detail}")
    note("idle-3d", ok)

    _, said = _idle_run(workdir, "ui", "--sheet", "shapes", "--ui", "--ui-enter", frames=200)
    if "frames" not in said:
        ok, detail = False, "the run failed"
    else:
        ok = 15 <= said["frames"] <= 60
        detail = (f"{said['frames']} frames of 200 (want 15 to 60: the 0.25 s fade's 15 at the "
                  f"least, and an end)")
    print(f"  idle-ui             {'PASS' if ok else 'FAIL'}  {detail}")
    note("idle-ui", ok)

    _, said = _idle_run(workdir, "gui", "--sheet", "shapes", "--gui-click", frames=200)
    if "frames" not in said:
        ok, detail = False, "the run failed"
    else:
        ok = 4 <= said["frames"] <= 10
        detail = f"{said['frames']} frames of 200 (want 4 to 10)"
    print(f"  idle-gui            {'PASS' if ok else 'FAIL'}  {detail}")
    note("idle-gui", ok)

    _, said = _idle_run(workdir, "load", "--sheet", "shapes", "--idle-wake", "0.5", "--idle-load",
                        os.path.relpath(asset("fire_hearth_color.png")), frames=200)
    if "frames" not in said:
        ok, detail = False, "the run failed"
    else:
        ok = said["frames"] == 3 and said.get("loaded") == (2048, 1608)
        detail = (f"{said['frames']} frames (want 3), {said['waited']:.3f} s waited for the wake "
                  f"and the decode, delivered at {said.get('loaded')} (want (2048, 1608))")
    print(f"  idle-load           {'PASS' if ok else 'FAIL'}  {detail}")
    note("idle-load", ok)

    script = os.path.join(workdir, "idle_type.script")
    with open(script, "w", encoding="utf-8") as f:
        f.write("2 key=mod+k\n4 type=zo\n6 key=down\n8 key=escape\n")
    _, said = _idle_run(workdir, "type", "--sheet", "shapes", "--palette", "--input-script", script,
                        frames=200)
    if "frames" not in said:
        ok, detail = False, "the run failed"
    else:
        ok = 9 <= said["frames"] <= 30
        detail = f"{said['frames']} frames of 200 (want 9 to 30: the script's, and an end)"
    print(f"  idle-type           {'PASS' if ok else 'FAIL'}  {detail}")
    note("idle-type", ok)

    # The code editor on 24 MB: its budget wraps the guessed rows a frame's share at a time, asking
    # for each frame, and once every row is wrapped nothing asks and the run ends. The UI's own
    # eases keep a run drawing a while whatever the text, so a small text's run is the baseline.
    line = "    (sig IO_L1N_T0L_N1_DBC_44 :io :bank 44 :byte 0 :strobe :domain VCCO_44)\n"

    def editor_run(tag, megabytes):
        path = os.path.join(workdir, f"idle_editor_{tag}.ckt")
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(line * max(1, int(megabytes * 1024 * 1024) // len(line)))
        _, probe = _canvas_run(workdir, f"idle_editor_{tag}", "--idle", "--editor", path,
                               frames=400)
        frames = probe.get(("idle", "frames"))
        settled = probe.get(("editor", "settled"))
        return (int(frames[0]) if frames else None), (int(settled[0]) if settled else None)

    small, _ = editor_run("small", 0.001)
    frames, settled = editor_run("big", 24)
    ok = (frames is not None and small is not None and frames > small + 30 and frames < 400
          and settled == 1)
    detail = (f"{frames} frames of the 400 allowed against {small} for a line of text, every row "
              f"wrapped {settled} (want over 30 more than the line's, which a 24 MB text's budget "
              f"takes at 192 KB a frame, the end before 400, and 1)")
    print(f"  idle-editor         {'PASS' if ok else 'FAIL'}  {detail}")
    note("idle-editor", ok)

    # Headless, a frame waits for the work out (spec 14.7): the board's thread, begun as the first
    # frame draws and made slow on purpose, lands in the second while a script keeps every frame
    # drawing.
    script = os.path.join(workdir, "idle_work.script")
    with open(script, "w", encoding="utf-8") as f:
        f.write("0-20 idle\n")

    def work_run(delay):
        _, said = _idle_run(workdir, f"work_{delay}", "--sheet", "board", "--edit-thread",
                            "--work-delay", str(delay), "--input-script", script)
        return said.get("begun"), said.get("landed")

    runs = {delay: work_run(delay) for delay in (0, 300)}
    ok = all(b is not None and landed == b + 1 for b, landed in runs.values())
    detail = (", ".join(f"{d} ms: begun at frame {b}, landed at {landed}"
                        for d, (b, landed) in runs.items())
              + " (want each landed the frame after it was begun)")
    print(f"  idle-work-waits     {'PASS' if ok else 'FAIL'}  {detail}")
    note("idle-work-waits", ok)

    # The window's close, which the app may refuse (spec 14.7): a script's close asks the hook as
    # the close button would, as the frame it is on ends.
    def close_run(tag, lines, *flags):
        script = os.path.join(workdir, f"close_{tag}.script")
        with open(script, "w", encoding="utf-8") as f:
            f.write(lines)
        _, probe = _canvas_run(workdir, f"close_{tag}", *flags, "--input-script", script,
                               frames=30)
        said = probe.get(("close", "asked"))
        return (int(said[0]), int(said[2]), int(said[4])) if said and len(said) == 5 else None

    twice = close_run("twice", "5 close\n9 close\n", "--refuse-close")
    alone = close_run("alone", "5 close\n9 close\n")
    held = close_run("held", "3-6 close\n", "--refuse-close")
    first = close_run("first", "3-6 close\n")
    # The script's frame N ends as the run's frame N + 2, as its typing reaches that frame.
    ok = (twice == (2, 1, 11) and alone == (0, 0, 7) and held == (1, 1, 30)
          and first == (0, 0, 5))
    detail = (f"asked, refused and frames drawn: refused then taken {twice} (want (2, 1, 11)), "
              f"no hook {alone} (want (0, 0, 7)), a close over frames 3 to 6 {held} (want "
              f"(1, 1, 30)) and with no hook {first} (want (0, 0, 5), its first frame)")
    print(f"  engine-close-hook   {'PASS' if ok else 'FAIL'}  {detail}")
    note("engine-close-hook", ok)

    return failed
