"""The clouds group: cloud layer (steady-state churn, report-only)."""

import os

from gatelib.harness import asset
from gatelib.lighting import DIR_CHURN_SLACK_PX, _taa_churn


# Cloud-layer steady-state churn (spec 11.0). The march's rotating dither is
# resolved by its own ray-direction accumulation; what leaks past it into the
# frame must stay near the no-clouds floor or the sky shimmers under TAA.
# Report-only for its first cycle: the bound (first measurement x1.5, on a
# floor measured fresh each run) becomes blocking once a cycle of history
# shows it stable. NB _taa_churn compares frames THIRTY apart, where the
# non-repeating dither sequence touches ~6x more pixels than adjacent-frame
# churn does -- at RMSE ~3e-4 amplitude (phase 3's adjacent-frame number).
# The ERROR path blocks even while the threshold only reports: broken render
# infrastructure and a drifting churn number are different failures.
CLOUD_CHURN_FACTOR = 9.7  # 153015/23656 measured at phase 5, x1.5 headroom


def run_cloud_churn_gate(workdir):
    fixture = asset("aerial_fixture.gltf")
    if not os.path.exists(fixture):
        print("  clouds       SKIP  (missing aerial_fixture.gltf)")
        return []
    clouded = _taa_churn(workdir, fixture, "cloud", ["--clouds"])
    floor = _taa_churn(workdir, fixture, "cloud_off", [])
    if clouded is None or floor is None:
        print("  clouds       ERROR while rendering the TAA sequences")
        return ["cloud-churn"]
    ok = clouded <= floor * CLOUD_CHURN_FACTOR + DIR_CHURN_SLACK_PX
    print(f"  clouds       {'PASS' if ok else 'REPORT'}  {clouded} px frame-to-frame "
          f"(no-cloud floor {floor} px)")
    return []  # report-only this cycle; flip to failures on the next
