"""The gametest and canvas probe runners several groups use."""

import functools
import json
import re
import os
import subprocess

from gatelib.harness import GAMETEST, RENDER, ROOT, _PROBE_TIMEOUT, _bin, _probe_render, _run, asset
from gatelib.scenes import TEXCOMP_FIXTURE


GAMETEST_TRACE_EVERY = 10


def _gametest_probe_text(flag, case, env=None, extra=None, timeout=None):
    """(combined output, returncode) from one gametest --<x>-probe run, given `timeout`
    seconds or _PROBE_TIMEOUT.

    stderr is merged in deliberately: a probe that refuses by name says so there, and an
    arm that sees neither the numbers nor the reason is a worse failure than a red one.
    A timeout reads as a failed run, since that is what a caller can act on.
    """
    try:
        r = subprocess.run([GAMETEST, flag, case] + (extra or []), capture_output=True,
                           text=True, env=env, timeout=timeout or _PROBE_TIMEOUT)
    except subprocess.TimeoutExpired:
        return "", 1
    return r.stdout + r.stderr, r.returncode


def _gametest_probe(flag, rx, case, env=None, extra=None):
    """{(label, key): [floats]} from one gametest --<x>-probe run, or None if it failed
    or measured nothing.

    The shape four probes print -- "<keyword> <case> <label> <key> <numbers...>" -- read
    back through the caller's own compiled regex. Audio is the fifth probe and keeps its
    own reader: its grammar carries a literal rms token, no key, and a fixed pair of
    numbers, so it shares the spawn above and nothing else.

    `extra` is forwarded to the spawn, which has always taken it; this only stopped being
    the end of the line. It is how a caller runs a case against another rig.
    """
    text, code = _gametest_probe_text(flag, case, env, extra)
    out = {(label, key): [float(v) for v in nums.split()]
           for c, label, key, nums in rx.findall(text) if c == case}
    if code != 0 or not out:
        return None
    return out


def _import_log(fixture, extra=()):
    """Two-frame headless render, combined stdout+stderr (log_* goes to stderr)."""
    r = subprocess.run([RENDER, "-m", fixture, *extra, "-x", "-f", "2"],
                       capture_output=True, text=True)
    return r.stdout + r.stderr


# The scene the config arms drive. Cornell rooms because it is the corpus's densest
# fixture for this: two probes, three lights, seven materials and no water, so one
# snapshot exercises both element arrays, the material pass through MATERIAL_PARAMS,
# and the absent-subsystem path at once.
CONFIG_FIXTURE = "cornell_rooms.cscn"


def _config_run(workdir, name, extra, model=CONFIG_FIXTURE, frames=2):
    """One capture plus its snapshot. Returns (ppm, json, stdout+stderr)."""
    ppm = os.path.join(workdir, f"config_{name}.ppm")
    dump = os.path.join(workdir, f"config_{name}.json")
    cmd = [RENDER, "-x", "-f", str(frames), "-S", ppm, "--config-dump", dump]
    if model:
        cmd += ["-m", asset(model), "-W", "400", "-H", "300"]
    r = _run(cmd + extra, capture_output=True, text=True)
    return ppm, dump, r.stdout + r.stderr


def _config_variant(src, dst, mutate):
    """A snapshot with one thing changed. The twin cannot differ in anything else."""
    with open(src) as f:
        d = json.load(f)
    mutate(d)
    with open(dst, "w") as f:
        json.dump(d, f, indent=1)
    return dst


def _texcomp_probe(workdir, extra, scene=None):
    """The texture ledger: (total_mb, formats seen, compressed count).

    `scene` defaults to the texcomp fixture. It is a parameter because the ledger
    is a property of the LOADER, not of that one asset, and the clearcoat group
    needs the same read on a fixture whose only texture arrives through a
    different path entirely -- an embedded data URI at an assimp slot the mapping
    table cannot reach.

    `workdir` is unused and kept: every caller passes it, and this returning
    None on a dead render is load-bearing enough that the signature is not worth
    churning for one argument.
    """
    del workdir
    rows, _ = _probe_render(scene or asset(TEXCOMP_FIXTURE),
                            "--texture-probe", "texture-probe", extra=extra, frames=3)
    total = next((r for r in rows if r.get("kind") == "total"), None)
    if total is None:
        # A dead render prints no probe lines, so an empty ledger is
        # indistinguishable from a compressed-nothing one -- and `ncomp == 0`
        # then PASSES on a build that cannot start.
        return None
    formats = {r["format"] for r in rows if r.get("kind") == "tex" and "format" in r}
    return float(total["mb"]), formats, int(total["compressed_count"])


# The variant a scene resolves to, read off the program builder's own log. There
# is no other way in: a correct variant and the uber-shader are the same picture,
# which is the point of the feature and also why no frame can tell them apart.
_PBR_VARIANT = re.compile(r"pbr variant (pbr[\w-]*): features (\d+) of (\d+) samplers (\d+)")

def _pbr_feature_bits():
    """The feature bits, READ from the file both C and GLSL include.

    Not mirrored. `_ubo_instance_max` established the rule and states the
    reason: a mirror lets the constant move while the arm goes on asserting the
    old value -- and passing, because it is checking the renderer against a
    number it made up. That applies with more force here, because the mask this
    parses is the contract between the C that emits it and the shader that reads
    it back.
    """
    src = open(os.path.join(ROOT, "cetra", "shaders", "include", "pbr_features.glsl")).read()
    bits = {m.group(1).lower(): int(m.group(2))
            for m in re.finditer(r"#define\s+PBR_FEAT_(\w+)\s+(\d+)", src)}
    full = bits.pop("all", None)
    return bits, full


_COOK_ROW = re.compile(r"^cook site=(\S+) name=(\S+) result=(\w+) bytes=(\d+) key=([0-9a-f]{16})",
                       re.M)


def _scribbled():
    """The environment with freed memory written over at once, by macOS's allocator and glibc's: a
    run reading what it freed reads garbage and fails, where with memory left as it was it reads
    what was there and passes. What the arms of an element freed late are run under (spec 14.6).
    Made at the call, after main has set the run's own cook directory in the environment."""
    return dict(os.environ, MallocScribble="1", MALLOC_PERTURB_="85")


# "ik <case> <label> <key> <numbers...>" from gametest --ik-probe. The character
# class admits no letters and no exponent on purpose: a nan or an inf prints a line
# this cannot match, so the key vanishes from the dict and the arm fails on absence.
# Every arm gets its NaN check for free that way.
_IK_PROBE = re.compile(r"^ik ([\w-]+) (\w+) (\w+)((?:\s+-?[\d.]+)+)$", re.M)


def _ik_probe_run(case):
    """{(label, key): [floats]} from one gametest --ik-probe run, or None if it failed
    or measured nothing. UNCACHED: one call is one process.

    One run of ANY case. Seven of them build a rig and nothing else -- no physics world,
    no window, no frame -- because a two-bone solve is a pure function of a hip, a
    target and two segment lengths, and a world would only make exact arithmetic depend
    on contact slop. The other three DO build a Jolt world and the --ik-ground fixture,
    because planting's input is a raycast. run_ik_gate says where the line is drawn.
    """
    return _gametest_probe("--ik-probe", _IK_PROBE, case)


@functools.cache
def _ik_probe(case):
    """_ik_probe_run memoised by case: the arms ask for twelve distinct cases seventeen
    times, and each spawn loads the rig -- three a Jolt world and the ground fixture too.

    The returned dict is SHARED between callers. Every arm reads and none writes, which
    is what makes that safe; an arm needing to mutate one must copy it first.

    ik-repeat calls _ik_probe_run twice instead, and asserts `first is not second` so
    that routing it through here fails rather than going quietly vacuous.
    """
    return _ik_probe_run(case)


def _canvas_shot(out, frames, *args, env=None, log=False):
    """apps/canvas headless at 400x300 points, its picture written to `out`: its stdout, with its
    log after it when `log` is set, or None for a run that failed or hung, which prints the tail of
    its output so an arm reads nothing and fails rather than the group stopping."""
    cmd = [_bin("canvas"), "-x", "-f", str(frames), "-W", "400", "-H", "300", "-S", out, *args]
    tag = os.path.splitext(os.path.basename(out))[0]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=_PROBE_TIMEOUT, env=env)
    except subprocess.TimeoutExpired:
        print(f"    apps/canvas {tag}: no exit in {_PROBE_TIMEOUT} s")
        return None
    if r.returncode != 0 or not os.path.exists(out):
        print(f"    apps/canvas {tag} failed: {(r.stdout + r.stderr)[-400:]}")
        return None
    return r.stdout + (r.stderr if log else "")


def _canvas_run(workdir, tag, *args, frames=2, env=None):
    """apps/canvas under --canvas-probe for `frames` frames: its picture's path and its probe
    lines keyed by (sheet, name), or None and no lines for a run that failed."""
    out = os.path.join(workdir, f"canvas_{tag}.ppm")
    stdout = _canvas_shot(out, frames, "--canvas-probe", *args, env=env)
    if stdout is None:
        return None, {}
    return out, _canvas_probe(stdout)


def _canvas_probe(stdout):
    """apps/canvas's probe lines keyed by (sheet, name), each the rest of its words."""
    probe = {}
    for line in stdout.splitlines():
        p = line.split()
        if len(p) >= 3 and p[0] == "canvas":
            probe[(p[1], p[2])] = p[3:]
    return probe

def _probe_verdicts(probe, failed):
    """verdict(name, case, check) for one probe's arms: (ok, what it saw) for `case` held to
    `check`, which takes its rows, each case run once however many arms read it. A failure is
    appended to `failed`; a row the check reads that the probe did not print is a failure naming
    that row, never a KeyError, which would end the whole run. The caller prints the verdict
    line, whose literal name is what gate-arm-docs reads."""
    rows_of = {}

    def verdict(name, case, check):
        if case not in rows_of:
            rows_of[case] = probe(case)
        rows = rows_of[case]
        try:
            ok, said = check(rows) if rows else (False, "no probe output")
        except KeyError as missing:
            ok, said = False, f"the probe printed no {missing.args[0]} row"
        if not ok:
            failed.append(name)
        return ok, said

    return verdict
