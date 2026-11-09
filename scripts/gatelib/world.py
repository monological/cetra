"""The forest, region and origin runs several groups share."""

import re

from gatelib.harness import FOREST, _run
from gatelib.images import _read_ppm, compare


# Where the automatic arm places the world, and the drift it allows. The world sits
# outside the threshold from frame 0, so a correct engine shifts immediately and
# then never again -- which is what makes "exactly once" a real assertion rather
# than a restatement of the flag. The threshold is a power of two so the lattice
# derived from it is one too.
ORIGIN_AUTO = 4096.0


def _origin_diff(a, b):
    """Differing pixels as a FRACTION of the frame, and the worst channel step.

    Both numbers come from compare(); this only supplies the denominator, which
    the arms want because a count means nothing without the resolution it was
    taken at -- and these frames render at twice the requested size on a HiDPI
    display, which is the arithmetic that made an early draft here report
    fractions above 1.0 and misread AE as counting channels.
    """
    w, h, _ = _read_ppm(a)
    ae, pae = compare(a, b)
    return ae / float(w * h), int(round(pae * 255.0))


_ORIGIN_TRACE = re.compile(
    r"player t=\s*([\d.]+) pos\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+).*?"
    r"grounded (\d)\s+ground_n\.y\s+[\d.]+\s+terrain\s+(-?[\d.]+)")
# Position, velocity, ground state and ground normal. The last two are what let
# forest-rest tell "standing still" from "fell through the collider".
_FOREST_TRACE = re.compile(
    r"player t=\s*[\d.]+ pos\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)\s+"
    r"vel\s+-?[\d.]+\s+-?[\d.]+\s+-?[\d.]+\s+grounded (\d)\s+ground_n\.y (-?[\d.]+)")


_CLUSTER_PROBE = re.compile(
    r"cluster-probe mesh=(\d+) clusters=(\d+) groups=(\d+) dag_levels=(\d+) bands=(\d+) "
    r"max_index=(\d+) vertex_count=(\d+) foreign=(\d+)((?: band\d+=\d+)+) alias=(\d+)"
    r"((?: digest\d+=\d+)+)")


_REGION_SUMMARY = re.compile(
    r"region-probe side=(\d+) span=([\d.]+) resident=(\d+) of (\d+) loaded=(\d+) freed=(\d+) "
    r"nodes=(\d+)")
_REGION_CELL = re.compile(
    r"region-probe cell rx=(?P<rx>\d+) rz=(?P<rz>\d+) trees=(?P<trees>\d+) "
    r"rocks=(?P<rocks>\d+) nodes=(?P<nodes>\d+) collider=(?P<collider>\d+) "
    r"digest=(?P<digest>[0-9a-f]+) authored=(?P<authored>[0-9a-f]+)")

# A region small enough, and a load radius short enough, that a walk of a few
# hundred frames crosses several boundaries. The shipping radius is larger than a
# kilometre world's own diagonal on purpose -- at this size nothing would ever be
# freed and every arm here would read a scene that never churned.
REGION_CHURN = ["--region-span", "40", "--region-radius", "60"]
# Fast enough to cross several 40-unit regions inside a three-second leg, so a
# 400-frame round trip leaves and comes back rather than shuffling inside one
# cell -- the seam arm needs boundaries CROSSED, not merely approached.
# --trace-player rides along: the seam arm reads the same run the residency arms
# do, so "it crossed a boundary" and "it stayed grounded" are the same walk rather
# than two that merely used the same flags.
REGION_WALK = ["--walk", "40", "--trace-player"]


def _region_run(extra, frames="400"):
    """One forest run with the region probe, parsed into (summary, {cell: row})."""
    # No -S: every region arm reads probe rows and trace lines, never a pixel.
    cmd = ([FOREST, "-x", "-f", frames, "-W", "320", "-H", "200", "--no-fog", "--seed", "1337",
            "--region-probe"] + extra)
    r = _run(cmd, capture_output=True, text=True)
    text = r.stdout + r.stderr
    m = _REGION_SUMMARY.search(text)
    if r.returncode != 0 or not m:
        return None, None, text
    summary = {"side": int(m.group(1)), "span": float(m.group(2)), "resident": int(m.group(3)),
               "total": int(m.group(4)), "loaded": int(m.group(5)), "freed": int(m.group(6)),
               "nodes": int(m.group(7))}
    cells = {}
    for c in _REGION_CELL.finditer(text):
        cells[(int(c["rx"]), int(c["rz"]))] = {
            "trees": int(c["trees"]), "rocks": int(c["rocks"]), "nodes": int(c["nodes"]),
            "collider": int(c["collider"]), "digest": c["digest"], "authored": c["authored"]}
    return summary, cells, text
