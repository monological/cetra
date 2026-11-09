"""The draw-count, GPU-timing and shading-report helpers several groups read."""

import json
import re
import os

from gatelib.harness import RENDER, _run, asset


# Milliseconds of slack on every ORDERING comparison in this file. The
# quantities are printed at three decimal places, so this is the tolerance for
# the rounding and nothing else -- the defects these arms catch are measured in
# whole multiples, not in fractions of a millisecond. It is a constant rather
# than a literal because cpu-bound in the submission gate wants the same value,
# and a comment claiming two sites agree is not the same as their agreeing.
GPU_ORDER_SLACK = 0.05


# The report's row format is the assertion surface, so a shifted format has to
# report as a named FAIL rather than as missing rows -- the arms below would
# otherwise blame the renderer for a parser change. Anchored on the " ms"
# suffix; the non-greedy name cannot swallow the number.
_GPU_ROW = re.compile(r"^(.*?)\s+(-?\d+\.\d+) ms$")


# The SHADING block, read on its own rather than through _report_tables. Its rows
# are bare integers where every block that parser knows ends in " ms" or carries
# exactly len(SUBMIT_COLS) columns, so teaching it this shape would mean either a
# third row format in the shared parser or every SHADING row landing in
# `unparsed` -- and four gates assert that `unparsed` is zero.
_SHADING_SAMPLES = re.compile(r"^samples shaded\s+(\d+)$", re.M)
_SHADING_BUDGET = re.compile(r"^sample budget\s+(\d+)$", re.M)
_SHADING_COMPLEXITY = re.compile(r"^depth complexity\s+([\d.]+)$", re.M)


def _shading_block(text):
    """{"shaded", "budget", "complexity"} or None if the block did not print."""
    s = _SHADING_SAMPLES.search(text)
    b = _SHADING_BUDGET.search(text)
    c = _SHADING_COMPLEXITY.search(text)
    if not s or not b or not c:
        return None
    return {"shaded": int(s.group(1)), "budget": int(b.group(1)),
            "complexity": float(c.group(1))}


GPU_FIXTURE = "dir_shadow_fixture.cscn"


def _gpu_cmd(out, extra, profile, fixture=GPU_FIXTURE, size=("800", "600"), frames="45"):
    """The one command line every profiled run uses.

    Built in one place because gpu-off's whole claim is that its two renders
    differ in exactly one flag; two hand-written lists could drift and the arm
    would then compare two different pictures and blame the instrument. The
    fixture and size are parameters for the same reason -- a submission gate
    with its own copy of this list would silently stop tracking a flag added
    here.

    -f 45 is enough to close a latch window (0.5s at this fixture's frame time
    is ~frame 38) and profiler_report publishes whatever is left over
    regardless, so the run length is not load-bearing beyond that.

    THAT LAST PARAGRAPH IS TRUE FOR A COUNT AND FALSE FOR A CLOCK, which is why
    `frames` is a parameter now. profiler_report publishes the LAST LATCHED
    WINDOW (profiler.c), so a run of one window long publishes the FIRST one --
    and the first window is warm-up: program compilation, pipeline creation,
    first-use allocation, clock ramp. Every submission arm here reads integers
    and does not care. An arm reading milliseconds is measuring startup mixed
    with steady state, in a proportion that changes run to run.
    Measured on overdraw_tiles: at 45 frames five identical renders spread 6.4%
    with a 36% outlier; at 200 frames four spread 3.0% with none. And the
    quantity itself moves -- the depth prepass reads +8% to +15% inside the
    warm-up window against +4% to +5% at steady state, because compiling its
    second program is part of what the short run is timing.
    """
    return ([RENDER, "-m", asset(fixture), "-x", "-f", frames,
             "-W", size[0], "-H", size[1], "--no-auto-exposure", "-E", "1.0", "-S", out]
            + (["--profiler"] if profile else []) + extra)


# Every banner-delimited block the report prints. One parser rather than one per
# gate: the row formats are a shared assertion surface, and a second copy would
# let a format change fail in one gate and pass in another.
#
# unparsed is counted PER BLOCK. Folding them together made a SUBMISSION format
# change fail gpu-parse, which claims to be about the GPU table -- an arm going
# red for something it does not name is the failure this suite exists to avoid.
_REPORT_BANNERS = {"GPU TIMING": "gpu", "CPU TIMING": "cpu", "SUBMISSION": "submit"}

# The SUBMISSION block is a matrix, one row per pass: a %-28s name field then one
# right-aligned integer per counter. Column names contain spaces, so the header
# cannot be split reliably -- the order is fixed here instead, and submit-parse
# asserts the header still says what this expects.
SUBMIT_COLS = ("meshes seen", "meshes culled", "draws", "instances", "material switches",
               "triangles")
_SUBMIT_NAME_WIDTH = 28

# A timing block's rows that are not passes -- whole-frame quantities that happen
# to share the row format. Filed here with the rest of the report's vocabulary
# rather than beside a threshold, because that is what it is: a statement about
# the shape of the table, read from two different gates.
#
# Named for the predicate it is used in, which is always `not in`. It was three
# hand-written copies of the same tuple until PERIOD arrived and had to be added
# to all of them -- and one copy is in another gate, where a miss would have
# failed as "this pass costs more than the frame" about a row that is not a pass.
NON_PASS_ROWS = frozenset({"TIMED", "FRAME (wall)", "PERIOD"})


def _within(value, ceiling, factor=1.0):
    """Is `value` inside `ceiling`, allowing a multiple of it plus rounding?

    False when either is missing, because a row that did not print is a more
    broken report than a row with the wrong number in it and must not read as
    passing. The caller reports WHICH is missing; this only answers the
    comparison.
    """
    if value is None or ceiling is None:
        return False
    return value <= ceiling * factor + GPU_ORDER_SLACK


def _report_tables(text):
    """Parse every block of a --profiler report.

    Returns {"gpu": {pass: ms}, "cpu": {pass: ms},
             "submit": {pass: {col: int}}, "submit_header": str|None,
             "unparsed": {block: n}}
    """
    out = {"gpu": {}, "cpu": {}, "submit": {}, "submit_header": None,
           "unparsed": {"gpu": 0, "cpu": 0, "submit": 0}}
    block = None
    for line in text.splitlines():
        stripped = line.rstrip()
        if stripped.startswith("===== END"):
            block = None
            continue
        if stripped.startswith("====="):
            block = None
            for banner, key in _REPORT_BANNERS.items():
                if stripped.startswith("===== " + banner):
                    block = key
            continue
        if block is None or not stripped.strip():
            continue
        if block == "submit":
            name = stripped[:_SUBMIT_NAME_WIDTH].strip()
            values = stripped[_SUBMIT_NAME_WIDTH:].split()
            if name == "pass":
                out["submit_header"] = stripped[_SUBMIT_NAME_WIDTH:].strip()
                continue
            if len(values) == len(SUBMIT_COLS) and all(v.isdigit() for v in values):
                out["submit"][name] = dict(zip(SUBMIT_COLS, (int(v) for v in values)))
            else:
                out["unparsed"]["submit"] += 1
            continue
        m = _GPU_ROW.match(stripped)
        if m:
            out[block][m.group(1).strip()] = float(m.group(2))
        elif stripped.strip() != "no passes timed":
            out["unparsed"][block] += 1
    return out


_IMPORT_DEDUP = re.compile(
    r"Import: (\d+) meshes built, (\d+) shared references, (\d+) LOD chains")


def _profiled_run(workdir, tag, extra, screenshot=None, fixture=GPU_FIXTURE, size=("800", "600"),
                  frames="45"):
    """One profiled render. Returns the parsed tables, or None on failure.

    The tables carry an "import" entry as well, because the dedup counters are
    logged rather than tabled and an arm that wanted them would otherwise pay
    for a whole extra render to see one line.
    """
    out = screenshot or os.path.join(workdir, f"gpu_{tag}.ppm")
    r = _run(_gpu_cmd(out, extra, True, fixture, size, frames),
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        print(f"  profiler     ERROR {tag} exited {r.returncode}: "
              f"{(r.stdout + r.stderr).strip()[-300:]}")
        return None
    text = r.stdout + r.stderr
    tables = _report_tables(text)
    m = _IMPORT_DEDUP.search(text)
    tables["import"] = ({"built": int(m.group(1)), "shared": int(m.group(2)),
                         "lod_chains": int(m.group(3))} if m else None)
    # None when the block did not print, which is what a caller asserting on it
    # has to notice; _report_tables cannot carry it, since SHADING rows are bare
    # integers where every block it knows ends in " ms" or has SUBMIT_COLS
    # columns, and four gates assert its `unparsed` count is zero.
    tables["shading"] = _shading_block(text)
    return tables


def _timing_delta(base_runs, variant_runs, row="opaque"):
    """(before, after, signed relative delta, run-to-run floor, separated) or None.

    Both arguments are LISTS of profiled runs of one config -- at least two of
    the base, so there is a floor at all. Any run missing the row, or reading
    zero, voids the measurement: a vanished pass reading 0 ms scores as a 100%
    saving, which is the confusion this helper was factored out to prevent.

    One helper because two arms make the same claim in the same shape -- "this
    config moved the row by X against a floor of Y" -- and the second copy had
    already drifted.

    THE ESTIMATOR IS THE FASTEST RUN, NOT THE MEAN OR THE MEDIAN, because this
    noise is mostly ONE-SIDED: contention, scheduling and thermal state can only
    make a render slower than the work it does. So the quickest sample of a
    config is its least-contaminated one, and averaging mixes interference back
    into the number. Measured over five identical renders of overdraw_tiles:
    5.319 / 7.253 / 5.363 / 5.476 / 5.661 ms -- four inside 6% and one 33% high.
    The mean is 5.81, the median 5.48, the minimum 5.32, and only the last is a
    statement about the renderer.

    `separated` IS THE HONEST SEPARATION TEST and the reason this returns five
    things. It is True when the variant's FASTEST run is slower than the base's
    SLOWEST -- the two sets of readings do not overlap at all, so no assignment
    of the noise to either config could account for the gap. It replaced a
    `delta >= 3 * floor` rule at the one call site that had one, and the
    replacement is not a relaxation: it is a claim the multiplier could not make.
    Three times a floor is a statement about a distribution nobody sampled, and
    when the floor drew high it demanded a 15% effect of a renderer whose real
    effect is 5-14%, so a correct build failed one run in five. Non-overlap needs
    no constant, gets STRICTER as the reader adds samples rather than looser, and
    is exactly what "these two configs are not the same speed" means.

    `floor` is still returned and still printed, as the gap between the two
    FASTEST base runs: it is what a reader needs to judge whether a green result
    was comfortable or lucky, and gpu-scale still asserts on it directly.
    """
    b_all = [t.get(row) for t in base_runs]
    v_all = [t.get(row) for t in variant_runs]
    if len(b_all) < 2 or not v_all:
        return None
    if any(x is None or x <= 0.0 for x in b_all + v_all):
        return None
    b_sorted = sorted(b_all)
    b, v = b_sorted[0], min(v_all)
    return b, v, (v - b) / b, (b_sorted[1] - b) / b, v > max(b_all)


# ---------------------------------------------------------------------------
# Submission counters (spec 11.28)
#
# These arms assert on INTEGERS, and that is the whole design. The timing arms
# above have to measure a run-to-run floor before they can claim a drop is
# real; a draw count has no floor to measure, so "553 became 71" is either true
# or the feature does not work. Nothing here can inherit the suite's flakiness.
SUBMIT_FIXTURE = "instancing_fixture.cscn"
SUBMIT_CASCADES = 3   # pinned on the command line below, not inherited from the C default


def _fixture_mesh_nodes(name="instancing_fixture.gltf", prefix=None):
    """Mesh-bearing node count, read from the fixture rather than copied.

    A hand-mirrored constant goes stale the moment the generator changes and
    takes the arm with it -- silently, because the arm would still pass against
    whatever it was told to expect. `prefix` narrows the count to one of a
    fixture's name-classes, which is how the occlusion arms read their
    populations.
    """
    path = asset(name)
    with open(path) as f:
        gltf = json.load(f)
    return sum(1 for n in gltf["nodes"]
               if "mesh" in n and (prefix is None or n.get("name", "").startswith(prefix)))


def _submit_sum_violations(tables):
    """seen == instances + culled, per pass. Returns (rows, the rows that break it).

    Returns rather than prints, unlike its first form, because gate-arm-docs
    reads each group's own source for the arms it runs -- so an arm printed from
    in here is invisible to the checker, and the group carrying it has to opt out
    of being checked at all. The arithmetic still lives once; only the verdict
    line moved to the caller, which is where every other helper in this file
    leaves it.
    """
    rows = dict(tables["submit"])
    bad = [(name, r["meshes seen"], r["instances"], r["meshes culled"])
           for name, r in rows.items()
           if r["meshes seen"] != r["instances"] + r["meshes culled"]]
    return rows, bad


def _submit_sum_detail(tables, label):
    """(ok, detail) for the submission identity. The CALLER prints the verdict.

    Split this way, rather than printing here, because gate-arm-docs reads each
    group's own source for the arm names it runs -- so an arm whose verdict line
    lives in a helper is invisible to the checker, and the group carrying it has
    to opt out of being checked at all. Every other helper in this file already
    works this way; this one did not.
    """
    rows, bad = _submit_sum_violations(tables)
    ok = bool(rows) and not bad
    return ok, (f"{label}: {len(rows)} passes, "
                f"{'identity holds in each' if ok else f'violations {bad}'}")
_CLUSTERED_LINE = re.compile(r"clustered: (\d+) directional \+ (\d+) clusterable lights, "
                             r"(\d+) cluster indices")


LOD_FIXTURE = "lod_fixture.gltf"
# Eye positions marching away from the same target. Not a golden's framing --
# the arm reads triangle counts, and what matters is that the sweep crosses
# enough of the ladder for a level to change.
LOD_SWEEP = ("0,1.5,4", "0,3,30", "0,6,90")


def _lod_run(workdir, tag, eye, extra):
    return _profiled_run(workdir, f"lod_{tag}",
                         ["--cam-eye", eye, "--cam-target", "0,0.5,-6"] + extra,
                         fixture=LOD_FIXTURE, size=("400", "300"))
OCCLUSION_MAT = "occlusion_fixture_mat.cscn"
