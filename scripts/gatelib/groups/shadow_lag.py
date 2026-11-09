"""The shadow-lag group: a shadow tracks this frame's pose (spec 11.96)."""

import os
import tempfile

from gatelib.harness import RENDER, _run, asset
from gatelib.images import compare


def run_shadow_lag_gate(workdir):
    """A caster's shadow is drawn from THIS frame's pose, not last frame's.

    The pixel half of spec 11.96, and the corpus had nothing that could show it:
    every golden is a static scene under a static camera, so a shadow one frame
    behind is pixel-identical to a correct one. The two things in the tree that
    move and cast -- the wind quads, the abandoned-window curtains -- displace
    vertices in the shader, which was never stale.

      shadow-lag-holds   at frame 15 the moving scene and a PARKED twin are
                         identical. The clip has not started, so they are the
                         same scene, and anything here makes the arm below
                         unreadable.
      shadow-lag-tracks  at frame 16 they DIFFER. That is the first displaced
                         frame, and the caster is out of shot, so the only thing
                         that can differ is its shadow: a correct one has left
                         the parked position, a lagging one is still on it.

    BOTH ARMS READ AGAINST THE TWIN rather than against an earlier frame of the
    same run. Comparing consecutive frames also discriminates, but only while the
    motion onset falls exactly between the two sampled frames -- retime the clip
    or change the frame rate and it silently stops testing anything. Against a
    parked twin the read is ABSOLUTE: at the first displaced frame a correct
    shadow has left the parked position and a lagging one is still sitting on it,
    whatever the sampling.

    What it covers is the SKINNED pose reaching the depth pass -- the caster
    moves by a bone, because a .cscn cannot express a moving object and a
    schedule flag would run before the shadow pass and pass against the bug. The
    node-transform half is the transform-walk group's, off the CPU probe.

    Falsified by restoring the pre-fix ordering (update_animation after the
    shadow pass rather than before): tracks reads 0 px where it reads 8617, and
    the shadow's onset slips from frame 16 to 17.

    FALSIFY WITH AN EXPLICIT --bin-dir. This suite auto-selects out/release/bin
    whenever its forest is no older than the debug one, and a mutation confined
    to apps/render never re-links forest -- so a debug-only rebuild leaves the
    selector on a stale RELEASE binary and three consecutive falsification runs
    come back green against code that was never built.
    """
    del workdir # frames go to a temp path the arm indexes; nothing is compared to a golden
    moving = asset("shadow_lag_fixture.cscn")
    parked = asset("shadow_lag_still.cscn")
    if not os.path.exists(RENDER) or not all(os.path.exists(p) for p in (moving, parked)):
        print("  shadow-lag-holds SKIP  (render or shadow_lag fixtures not present)")
        return []
    arms = ["shadow-lag-holds", "shadow-lag-tracks"]

    def _frames(scene, tag, tmp):
        base = os.path.join(tmp, f"{tag}.ppm")
        r = _run([RENDER, "-m", scene, "-x", "-f", "18", "--no-auto-exposure", "-E", "1.0",
                  "-W", "400", "-H", "300", "--screenshot-every", "1", "-S", base],
                 capture_output=True, text=True)
        got = {n: base[:-4] + f"_{n:06d}.ppm" for n in (15, 16)}
        ok = r.returncode == 0 and all(os.path.exists(p) for p in got.values())
        return got, ok

    with tempfile.TemporaryDirectory() as tmp:
        mv, ok_m = _frames(moving, "moving", tmp)
        st, ok_s = _frames(parked, "parked", tmp)
        if not (ok_m and ok_s):
            for arm in arms:
                print(f"  {arm} ERROR a render failed or wrote no numbered frames")
            return arms
        holds = compare(mv[15], st[15])[0]
        tracks = compare(mv[16], st[16])[0]

    failures = []
    ok = holds == 0
    print(f"  shadow-lag-holds {'PASS' if ok else 'FAIL'}  at frame 15 the moving scene and the "
          f"parked twin differ by {holds} px (want 0: the clip has not started, so the two are the "
          f"same scene and anything here would make the arm below unreadable)")
    if not ok:
        failures.append("shadow-lag-holds")

    # AGAINST THE PARKED TWIN, not against frame 15 of itself, and that is the
    # whole arm. The shadow's own sequence advances one step per frame in a
    # correct build AND a lagging one, so a difference of differences is blind to
    # this -- an earlier version compared consecutive frames and could not tell
    # them apart. Read absolutely: at the first displaced frame a correct shadow
    # has LEFT the parked position and a lagging one is still sitting on it.
    ok = tracks > 100
    print(f"  shadow-lag-tracks {'PASS' if ok else 'FAIL'}  at frame 16 they differ by {tracks} px "
          f"(want > 100: the caster is out of frame so this is its shadow alone, and a shadow drawn "
          f"from the previous frame's pose is still exactly where the parked twin puts it)")
    if not ok:
        failures.append("shadow-lag-tracks")

    return failures
