"""The import group: import."""

import math
import os
import subprocess

from gatelib.harness import RENDER, asset
from gatelib.probes import _import_log


def run_range_gate():
    """glTF KHR_lights_punctual `range` must survive import unchanged."""
    fixture = asset("point_import_fixture.gltf")
    if not os.path.exists(fixture):
        print("  gltf-range   SKIP  (missing point_import_fixture.gltf)")
        return []
    r = subprocess.run([RENDER, "-m", fixture, "-x", "-f", "2"],
                       capture_output=True, text=True)
    got = None
    for line in (r.stdout + r.stderr).splitlines():
        if "<Light name=" in line and "range=" in line:
            field = _light_field(line, "range")
            got = float(field) if field is not None else None
    if got is None:
        print("  gltf-range   ERROR (no light imported)")
        return ["gltf-range"]
    ok = abs(got - 25.0) < 1e-3
    print(f"  gltf-range   {'PASS' if ok else 'FAIL'}  authored 25.0, imported {got}")
    return [] if ok else ["gltf-range"]


# Mirrors gen_light_import_fixture.py's spot variant and has to match it, or the
# gate predicts a cone the asset does not author.
CONE_INNER_DEG = 20.0
CONE_OUTER_DEG = 30.0


def run_cone_gate():
    """A glTF spot's cone half-angles must arrive as COSINES, and in order.

      gltf-cone   an imported 20/30 degree cone reads cos 0.9397 / 0.8660, inner
                  above outer

    import.c wrote assimp's RADIAN half-angles straight into the engine's cosine
    fields until 11.57. Nothing caught it because nothing in the corpus authored a
    spot through glTF -- cornell_leak is the only spot anywhere and it comes
    through a .cscn, which has always converted.

    The ORDERING is asserted beside the values because it is the half that breaks
    the shader rather than merely detuning it: cosine decreases with angle, so
    correct data has inner > outer, and spotConeFactor's
    epsilon = max(cutOff - outerCutOff, 1e-4) collapses to 1e-4 when that
    inverts -- turning a soft edge into a hard step. Falsified by hand at 11.57:
    the raw radians read 0.3491 / 0.5236, wrong AND inverted.
    """
    fixture = asset("spot_import_fixture.gltf")
    if not os.path.exists(fixture):
        print("  gltf-cone    SKIP  (missing spot_import_fixture.gltf)")
        return []
    line = None
    for text in (_import_log(fixture, ["--no-scene-file"]),):
        for candidate in text.splitlines():
            if "<Light name='coned_lamp'" in candidate:
                line = candidate
    if line is None:
        print("  gltf-cone    ERROR (no spot light imported)")
        return ["gltf-cone"]

    inner = _light_field(line, "cutOff")
    outer = _light_field(line, "outerCutOff")
    if inner is None or outer is None:
        print("  gltf-cone    ERROR (light print carries no parseable cone)")
        return ["gltf-cone"]
    # outerCutOff is the last field, so it arrives with the closing '>'.
    got_inner = float(inner)
    got_outer = float(outer.rstrip(">"))
    want_inner = math.cos(math.radians(CONE_INNER_DEG))
    want_outer = math.cos(math.radians(CONE_OUTER_DEG))
    ok = (abs(got_inner - want_inner) < 1e-4 and abs(got_outer - want_outer) < 1e-4
          and got_inner > got_outer)
    print(f"  gltf-cone    {'PASS' if ok else 'FAIL'}  {CONE_INNER_DEG:.0f}/{CONE_OUTER_DEG:.0f} "
          f"degree cone imported as cos {got_inner:.4f}/{got_outer:.4f} "
          f"(want {want_inner:.4f}/{want_outer:.4f}, inner above outer -- radians would "
          f"read 0.3491/0.5236 and inverted)")
    return [] if ok else ["gltf-cone"]


def _fbx_light_line(output):
    """The unit_lamp <Light ...> print, or None."""
    for line in output.splitlines():
        if "<Light name='unit_lamp'" in line:
            return line
    return None


# The light_print format is the assertion surface, so a shifted format must
# report as a named FAIL, not a parser traceback: both extractors return None
# on any mismatch.
def _light_field_vec3(line, key):
    try:
        inner = line.split(key + "=(")[1].split(")")[0]
        return [float(v) for v in inner.split(",")]
    except (IndexError, ValueError):
        return None


def _light_field(line, key):
    try:
        return line.split(key + "=")[1].split(",")[0]
    except IndexError:
        return None


def run_fbx_unit_gate():
    """FBX unit scale must bake cm to metres, and the light must survive.

    Pins two things at once (spec 11.2, paying 11.1's recorded debt): the
    engine's mirror of assimp's UnitScaleFactor * 0.01 conversion (a
    vendored-assimp change that moved it would land here first), and the FBX
    light-import path itself -- the fixture's light must arrive at its exact
    metre position, at the intensity the conversion chain produces, and
    actually reach the clusterer (a culled light is the exact silently-black
    symptom of spec 11.1's c64). A second render with --no-unit-scale is the
    mechanical twin: the same file read raw must place the light at 200 cm.
    """
    fixture = asset("fbx_unit_fixture.fbx")
    if not os.path.exists(fixture):
        print("  fbx-unit     SKIP  (missing fbx_unit_fixture.fbx)")
        return []

    out = _import_log(fixture)
    light = _fbx_light_line(out)
    if light is None:
        print("  fbx-unit     ERROR (no FBX light imported)")
        return ["fbx-unit"]

    problems = []
    if "file declares 0.01x, applied 0.01x" not in out:
        problems.append("unit-scale log line missing or wrong")
    pos = _light_field_vec3(light, "global_position")
    if pos is None:
        problems.append("light print carries no parseable position")
    elif abs(pos[0]) > 1e-3 or abs(pos[1] - 2.0) > 1e-3 or abs(pos[2]) > 1e-3:
        problems.append(f"position {pos} != (0, 2, 0) m")
    intensity = _light_field(light, "intensity")
    if intensity is None or not intensity.endswith(" cd"):
        problems.append("light print carries no candela intensity")
    elif abs(float(intensity[:-3]) - 50.0) > 1e-3:
        problems.append(f"intensity {intensity} != 50 cd")
    clusterable = None
    for line in out.splitlines():
        if "clustered:" in line and "directional +" in line:
            clusterable = int(line.split("directional +")[1].split("clusterable")[0])
            break  # logged once per build; the first match is the record
    if clusterable != 1:
        problems.append(f"clusterable count {clusterable} != 1 "
                        "(0 is the silently-black symptom, 2+ a double import)")

    raw_light = _fbx_light_line(_import_log(fixture, ["--no-unit-scale"]))
    raw_pos = _light_field_vec3(raw_light, "global_position") if raw_light else None
    if raw_pos is None:
        problems.append("--no-unit-scale render imported no parseable light")
    elif abs(raw_pos[1] - 200.0) > 1e-3:
        problems.append(f"--no-unit-scale position y {raw_pos[1]} != 200 cm")

    ok = not problems
    detail = "cm baked to (0, 2, 0) m at 50 cd, clustered" if ok else "; ".join(problems)
    print(f"  fbx-unit     {'PASS' if ok else 'FAIL'}  {detail}")
    return [] if ok else ["fbx-unit"]


def _run_import_gates(workdir):
    del workdir # all three drive the importer directly rather than a render
    return run_range_gate() + run_cone_gate() + run_fbx_unit_gate()
