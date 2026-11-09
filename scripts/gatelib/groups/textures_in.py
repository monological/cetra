"""The textures-in group: a headless run starts with its textures in (spec 14.8)."""

import os

from gatelib.harness import asset, render
from gatelib.images import compare


# --- a headless run's start-up textures (spec 14.8) -------------------------
TEXTURES_IN_FIXTURE = "textures_in_fixture.cscn"


def run_textures_in_gate(workdir):
    """A headless run starts with every texture its start-up asked for (spec 14.8).

      textures-in  the sixteen-image wall at frame 1 against itself at frame 30: 0 px.

    The loader uploads at most five textures a frame, so a run that starts without waiting
    for its textures shows five of the wall's sixteen at frame 1 and eleven tiles blank, on
    any machine: before 14.8 this read 222,938 px. That is the failure local_exposure's
    golden met by chance -- a room half loaded at frame 30 on a busy machine -- read here
    with no load needed to provoke it. Frame 30 is the run's own reference rather than a
    stored image, and the scene is still, with its exposure pinned, so the two frames can
    differ by the textures alone.
    """
    scene = asset(TEXTURES_IN_FIXTURE)
    if not os.path.exists(scene):
        print(f"  textures-in  SKIP  ({TEXTURES_IN_FIXTURE} not present)")
        return []
    first = os.path.join(workdir, "textures_in_f1.ppm")
    late = os.path.join(workdir, "textures_in_f30.ppm")
    err = render(scene, first, [], frames=1) or render(scene, late, [])
    if err:
        print(f"  textures-in  ERROR render failed: {err.strip()[-200:]}")
        return ["textures-in"]
    moved, _ = compare(first, late)
    ok = moved == 0
    print(f"  textures-in  {'PASS' if ok else 'FAIL'}  frame 1 against frame 30: {moved} px "
          "(want 0)")
    return [] if ok else ["textures-in"]
