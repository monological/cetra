"""The catcher group: catcher over a real ground (contact fixture)."""

import os

from gatelib.harness import asset
from gatelib.lighting import DIR_CHURN_FACTOR, DIR_CHURN_SLACK_PX, _taa_churn


# The shadow catcher's virtual floor is auto-enabled by every sky-lit scene and
# sits at y=0 -- exactly where a scene that ships its own ground puts it. The
# catcher must never win the depth race against that real ground: when it does,
# it re-stamps its own flat-bias shadow term over already-shaded pixels in
# jitter-dependent patches (long rectangular streaks that flicker under TAA).
# contact_fixture is the suite's only sky+catcher+real-ground scene, so the
# check lives here; the authored-light fixtures have no catcher and cannot
# measure it. Default settings deliberately (PCSS on, full post): the defect
# was invisible to every gate that pins the pipeline down.
def run_catcher_gate(workdir):
    fixture = asset("contact_fixture.cscn")
    if not os.path.exists(fixture):
        print("  catcher      SKIP  (missing contact_fixture.cscn)")
        return []
    shadowed = _taa_churn(workdir, fixture, "catch", [])
    floor = _taa_churn(workdir, fixture, "catch_ns", ["--no-shadows"])
    if shadowed is None or floor is None:
        print("  catcher      ERROR while rendering the TAA sequences")
        return ["catcher-churn"]
    ok = shadowed <= floor * DIR_CHURN_FACTOR + DIR_CHURN_SLACK_PX
    print(f"  catcher      {'PASS' if ok else 'FAIL'}  {shadowed} px frame-to-frame "
          f"(no-shadow floor {floor} px)")
    return [] if ok else ["catcher-churn"]
