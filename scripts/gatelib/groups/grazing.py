"""The grazing group: punctual grazing (leak wall base)."""

import os

from gatelib.harness import asset
from gatelib.images import _linear_luma
from gatelib.lighting import _dir_render


# The wall-base line in cornell_leak at the framing below, as frame fractions.
# Nothing occludes this stretch of the left wall's base; the punctual near-side
# work darkens it to ~0.52 (the wedge spec 10.5 records).
LEAK_CAM = ["--cam-eye", "-0.832,0.062,1.232", "--cam-target", "-1.068,0.045,0.070"]
LEAK_LINE = ((0.08, 0.77), (0.48, 0.59))
LEAK_TOL = 0.05


# The punctual grazing gate: cornell_leak's wall base, which nothing occludes,
# must read fully lit. Its own gate rather than a dir-shadow sub-gate: it
# measures the PUNCTUAL path (the wedge was near-side fallout fixed in the
# reference implementation before the cascade port could copy it, spec 10.5
# phase 2a), so it must not vanish behind a missing cascade fixture.
def run_grazing_gate(workdir):
    leak = asset("cornell_leak.gltf")
    if not os.path.exists(leak):
        print("  grazing      SKIP  (missing cornell_leak.gltf)")
        return []

    shadow = _dir_render(workdir, "cornell_leak.gltf", "leak", LEAK_CAM)
    ref = _dir_render(workdir, "cornell_leak.gltf", "leak_ns", LEAK_CAM + ["--no-shadows"])
    if not (shadow and ref):
        print("  grazing      ERROR while rendering cornell_leak")
        return ["grazing"]

    w, h, pix = shadow
    _, _, refpix = ref
    worst = 1.0
    (x0, y0), (x1, y1) = LEAK_LINE
    for i in range(41):
        t = i / 40.0
        px = (x0 + (x1 - x0) * t) * w
        py = (y0 + (y1 - y0) * t) * h
        lit = _linear_luma(refpix, w, h, px, py)
        if lit > 1e-4:
            worst = min(worst, _linear_luma(pix, w, h, px, py) / lit)
    ok = worst >= 1.0 - LEAK_TOL
    print(f"  grazing      {'PASS' if ok else 'FAIL'}  wall-base term {worst:.4f} "
          f"(want >= {1.0 - LEAK_TOL})")
    return [] if ok else ["grazing"]
