"""The light-pool group: the clustered light index pool (64 KB, its overflow warning; spec
13.51).
"""

import re
import subprocess

from gatelib.harness import RENDER, asset
from gatelib.probes import _import_log
from gatelib.profiling import _CLUSTERED_LINE


# The light index pool (spec 13.51). Mirrors of UBO_CLUSTER_INDICES_BLOCK_SIZE (one slot a byte),
# GL 4.1's guaranteed block size the pool used to be held to, and LC_OVERFLOW_REPEAT.
LIGHT_POOL_SLOTS = 65536
LIGHT_POOL_GUARANTEED = 16384
LIGHT_POOL_REPEAT = 600
LIGHT_POOL_FIT = "light_pool_fit.cscn"
LIGHT_POOL_OVER = "light_pool_over.cscn"
_POOL_OVERFLOW_LINE = re.compile(r"Cluster index pool overflowed: needs (\d+) of (\d+) slots")


def run_light_pool_gate(workdir):
    """The clustered light index pool at 64 KB, and its overflow warning (spec 13.51).

      pool-fit         sixteen lights round the eye, each reaching past it, store more indices
                       than a 16 KB pool holds, with no overflow
      pool-overflow    forty-eight overflow the 64 KB pool: the warning names the demand
                       against 65536, and repeats once LC_OVERFLOW_REPEAT builds have passed

    Both read the log, so a small frame serves. WHAT THE OLD BUILD READS: the 16 KB pool stores
    16384 of pool-fit's indices and warns; the once-a-session warning prints one line over
    pool-overflow's 602 frames, and named neither the demand nor the pool's size.
    """
    del workdir # the log is the reading; nothing is written
    failures = []

    # --- pool-fit -------------------------------------------------------------
    text = _import_log(asset(LIGHT_POOL_FIT), ["-W", "160", "-H", "120"])
    m = _CLUSTERED_LINE.search(text)
    if not m:
        print("  pool-fit     ERROR the clustered: line did not print")
        failures.append("pool-fit")
    else:
        indices, overflowed = int(m.group(3)), "overflowed" in text
        ok = LIGHT_POOL_GUARANTEED < indices <= LIGHT_POOL_SLOTS and not overflowed
        print(f"  pool-fit     {'PASS' if ok else 'FAIL'}  {m.group(2)} lights store {indices} "
              f"indices (want over {LIGHT_POOL_GUARANTEED}, a 16 KB pool's whole, and at most "
              f"{LIGHT_POOL_SLOTS}), overflowed={overflowed}")
        if not ok:
            failures.append("pool-fit")

    # --- pool-overflow --------------------------------------------------------
    # One camera build a frame, so the second warning lands on build REPEAT + 2.
    frames = str(LIGHT_POOL_REPEAT + 2)
    r = subprocess.run([RENDER, "-m", asset(LIGHT_POOL_OVER), "-W", "160", "-H", "120", "-x",
                        "-f", frames], capture_output=True, text=True)
    warnings = _POOL_OVERFLOW_LINE.findall(r.stdout + r.stderr)
    if not warnings:
        print("  pool-overflow ERROR no overflow warning printed")
        failures.append("pool-overflow")
    else:
        needed, size = int(warnings[0][0]), int(warnings[0][1])
        ok = len(warnings) == 2 and size == LIGHT_POOL_SLOTS and needed > LIGHT_POOL_SLOTS
        print(f"  pool-overflow {'PASS' if ok else 'FAIL'}  {len(warnings)} warnings over "
              f"{frames} frames (want 2), the first needing {needed} of {size} slots (want over "
              f"{LIGHT_POOL_SLOTS} of {LIGHT_POOL_SLOTS})")
        if not ok:
            failures.append("pool-overflow")

    return failures
