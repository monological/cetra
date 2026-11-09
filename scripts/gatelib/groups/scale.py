"""The scale group: scale invariance (lights x1000, exposure /1000)."""

import os

from gatelib.harness import ROOT, render
from gatelib.images import LSB, compare
from gatelib.scenes import _scale_emitters, cscn_copy


SCALE = 1000.0

# Scale-invariance gates. Scaling every emitter by K and the exposure by 1/K is
# a no-op on a correctly normalised pipeline, so the two frames must match.
SCALE_GATES = [
    ("punctual", "assets/scenes/cornell_point.cscn", []),
    ("area", "assets/scenes/area_scale_fixture.cscn", []),
    ("subsurface", "assets/scenes/sss_scale_fixture.cscn", []),
    # Fog's in-scatter radiance is a real emitter, so the scaler lifts
    # fog.ambient alongside the lights.
    ("fog", "assets/scenes/froxel_scale_fixture.cscn", ["--fog"]),
    # The sky is an emitter through its radiance scale (spec 13.7): its LUTs,
    # its disc, its zenith march and the sun light it couples all take it. The
    # sun is turned into frame so the disc is read, and fog is on because the
    # zenith march reaches a frame through the fog ambient and nowhere else.
    # The circumsolar sky here stays under the stored ceiling at x1000 (60,000
    # nits, 60 on the relative scale), the one place the legs may differ by
    # design: a circumsolar sky past 60 relative would clip in the x1000 leg.
    ("sky", "assets/scenes/aerial_fixture.cscn", ["--fog", "--sun-azimuth", "180"]),
    # Local exposure (spec 13.19) builds its grid from the EXPOSED frame and centres it on the
    # exposure key, so a scene a thousand times brighter under an exposure a thousand times
    # smaller is the same picture. Every contrast and the blurred base live, so a read of
    # absolute luminance anywhere in it moves this row: the grid built from absolute luminance
    # moved it 329,781 px, peak 135 codes, at 13.19.
    ("local-exposure", "assets/scenes/cornell_point.cscn",
     ["--local-exposure", "--le-highlights", "0.5", "--le-shadows", "0.7", "--le-blend", "0.6"]),
]


def scaled_copy(src, dst, factor):
    """Multiply every authored intensity by `factor` and divide exposure by it."""
    def scale(d):
        _scale_emitters(d, factor)
        post = d.setdefault("post", {})
        post["exposure"] = post.get("exposure", 1.0) / factor

    cscn_copy(src, dst, scale)


def run_scale_gates(workdir):
    failures = []
    for name, rel, extra in SCALE_GATES:
        src = os.path.join(ROOT, rel)
        if not os.path.exists(src):
            print(f"  {name:<12} SKIP  (missing {rel})")
            continue

        big = os.path.join(workdir, f"{name}_{int(SCALE)}x.cscn")
        scaled_copy(src, big, SCALE)
        a = os.path.join(workdir, f"{name}_a.ppm")
        b = os.path.join(workdir, f"{name}_b.ppm")

        for scene, out in ((src, a), (big, b)):
            err = render(scene, out, extra)
            if err:
                print(f"  {name:<12} ERROR while rendering {scene}")
                failures.append(name)
                break
        else:
            ae, pae = compare(a, b)
            ok = pae <= LSB
            detail = f"{ae} px, peak {pae:.6f}"
            if ok and ae:
                detail += " (1 LSB -- invisible)"
            print(f"  {name:<12} {'PASS' if ok else 'FAIL'}  {detail}")
            if not ok:
                failures.append(name)
    return failures
