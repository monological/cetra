"""Where the suite finds its assets and binaries, and how it runs them."""

import importlib.util
import re
import os
import subprocess
import sys

from gatelib.images import _read_ppm


# scripts/, and the repository above it. Every path the suite reads from its own tree is joined
# from one of these, never from a module's __file__, so a helper keeps its paths when it moves.
SCRIPTS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(SCRIPTS)

# The committed corpus is split by KIND (spec 12.8), and a call site says a
# filename rather than a path so it never learns the taxonomy -- the ~150 sites
# that used to join ROOT/assets/<name> pass the same bare name they always did,
# and the ~60 module constants holding one need no change at all.
#
# The taxonomy itself is IMPORTED and not restated. It shipped as two tables,
# one here and one in fixture_paths, and they had drifted apart before either
# was used in anger -- this side was missing .glb and every image format, that
# side was missing .cscn and .r16. The generators' copy is the one that
# survives, because it is the only one a generator can reach: fixture-gen
# sandboxes a generator by copying assets/ into a scratch tree with no scripts/
# in it, so a dependency pointing the other way would break the sandbox.
# fixture_paths imports nothing but os and has no side effects.
sys.path.insert(0, os.path.join(ROOT, "assets", "generators"))
from fixture_paths import KIND as ASSET_KINDS  # noqa: E402  (the one taxonomy)
from fixture_paths import asset_ref, asset_subpath  # noqa: E402


def asset(name):
    """Absolute path to a committed asset, routed to its kind directory.

    Assembled through a list rather than as a join of the root, the corpus
    directory and the name. That three-argument shape is what ~150 call sites
    were swept FROM, and written here it makes this function match its own
    sweep -- once into unbounded recursion, and once (having fixed that) into a
    docstring rewritten to say the opposite of what it meant.
    """
    # An absolute path is already an answer -- several arms build a mutated
    # fixture in the workdir and hand the result straight back here. It has to
    # return BEFORE the split: os.path.join discards its earlier arguments when
    # one is absolute, but splitting "/a/b" yields a leading empty segment, and
    # join then treats that as an ordinary relative piece and builds
    # assets/a/b instead.
    if os.path.isabs(name):
        return name
    return os.path.join(ROOT, "assets", *asset_subpath(name).split("/"))


# Where the app binaries come from. `out/bin` is build.sh's default (Debug, no
# -O at all); `out/release/bin` is `./build.sh --release`.
#
# Worth knowing before choosing, because the two apps answer very differently
# (spec 11.65, measured):
#   render  0.41 s -> 0.36 s, a 12% saving. Its startup is a GL context and ~37
#           driver-side shader compiles, which an optimiser cannot touch.
#   forest  6.58 s -> 1.36 s, a 4.8x saving. Its startup is CPU work -O2 does
#           reach: procedural 1K texture bakes, 20 cluster-DAG builds, erosion.
# At ~284 render and ~55 forest launches that is ~17 s against ~287 s, so
# essentially the whole prize is forest's.
#
# Auto-selected by FRESHNESS, which is the one rule that cannot test a stale
# binary: release is used only when it is at least as new as the debug tree it
# would replace. Build debug after release and the mtime ordering flips back on
# its own, so the dangerous case -- passing on a binary nobody just built --
# cannot arise without also making debug older, which is the same hazard the
# plain default has. An explicit --bin-dir or CETRA_BIN_DIR overrides it, and
# the resolved directory is printed on every run so a result is never ambiguous
# about what it tested.
def _default_bin_dir():
    debug = os.path.join(ROOT, "out", "bin")
    release = os.path.join(ROOT, "out", "release", "bin")
    try:
        # forest is the app the choice is FOR -- 4.8x against render's 1.1x -- so
        # it is the one whose freshness decides.
        if os.path.getmtime(os.path.join(release, "forest")) >= os.path.getmtime(
                os.path.join(debug, "forest")):
            return release
    except OSError:
        pass
    return debug


def _bin(name):
    """An app binary in BIN_DIR, spelled the way the host names executables."""
    return os.path.join(BIN_DIR, name + (".exe" if sys.platform == "win32" else ""))


BIN_DIR = os.environ.get("CETRA_BIN_DIR") or _default_bin_dir()
RENDER = _bin("render")

# Every pass that spreads a highlight's light across the frame after shading: bloom, and the
# aperture's diffraction glare (spec 13.4). An arm that reads a surface on its own pins all of
# them off, or it measures the surface plus a halo from whatever is brightest in view -- and
# the next such pass is added here rather than found arm by arm.
NO_HALOS = ["--no-bloom", "--no-glare"]

# Every sample coordinate and every stored golden in this suite was measured
# against a framebuffer TWICE the requested -W/-H, because that is what a HiDPI
# context hands back. Most arms already read fractionally and do not care
# (_beach_pixel says so in its own docstring); two cannot:
#
#   - ao-ring's window is absolute -- AO_RING_ROWS (745, 815) against an arm
#     rendering -W 800 -H 600. At 1x the buffer is 800x600 and every sampled row
#     is past the bottom edge, which is an IndexError that takes the whole run
#     down rather than failing one arm.
#   - cloud-shadow reads a --sky-debug tile that sky.c draws at ABSOLUTE pixel
#     offsets. At 1x that tile is not displaced, it is off the frame entirely, so
#     no rescaling of the sample box could recover it.
#
# The second is why the REQUEST is scaled rather than the readings. A HiDPI
# machine multiplies by one and is byte-identical to before; a 1x machine asks
# for double and gets the same framebuffer, so absolute coordinates land, the
# debug tile is on screen, and an absolute pixel-count threshold means the same
# thing on both. Nothing downstream learns which platform it is on.
#
# Note this was never only a portability question: a Mac on a 1x external monitor
# breaks the suite in exactly the same way, which is why the scale is MEASURED
# below and not keyed off sys.platform.
CALIBRATED_FB_SCALE = 2
_FB_SCALE = None


def render(scene, out, extra, frames=30):
    """One headless frame capture at the suite's shared size.

    THIRTY FRAMES IS INHERITED, NOT DERIVED, and since it is the most repeated
    number in this file that is worth saying where it lives. Nothing in the
    engine produces 30. What the engine actually forces (spec 11.65):

      - Without --taa every shared temporal accumulator -- TAA, GTAO, SSGI, SSR,
        SSS, the composited fog layer, contact shadows -- is not merely off, its
        history is force-invalidated every frame. A gate run passes no --taa.
      - A pinned exposure (--no-auto-exposure -E, or an authored post.exposure)
        skips the metering block entirely, so it imposes no frame requirement.
      - Animation, wind, particles and the water FFT are pure functions of the
        frame INDEX headless, so frame N is pose N -- a floor and a ceiling at
        once for any arm that reads motion, and irrelevant to one that does not.
      - The async texture loader was the one unconditional floor, at five
        uploads per frame plus one more frame for the material texture array.
        Since spec 14.8 a headless run's start-up textures are in before frame
        1, and the array is built in frame 1 before anything draws, so the floor
        for a static, pinned, fog-free capture is one frame -- which is what
        textures-in holds.

    It is NOT lowered, and that is a measurement rather than caution: a frame at
    this size costs ~0.0013 s, so thirty frames cost 0.04 s more than one and
    cutting every call site to the floor would save about five seconds of a
    ten-minute suite. Against that, `frames` is load-bearing for any arm reading
    a pose, and lowering the shared default would move those silently -- the
    arm would still pass, reading a different moment. Five seconds is not worth
    a class of failure this suite has already shipped twice.

    Counts that ARE derived state their reason where they are set: 60 for the
    froxel and cloud accumulators, 150 for auto-exposure and foam history,
    240 and 400 for trace cadence and walk geometry.
    """
    cmd = [RENDER, "-m", scene, "-x", "-f", str(frames), "-W", "400", "-H", "300", "-S", out]
    r = _run(cmd + extra, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return r.stdout + r.stderr
    return None


def _scale_argv(argv):
    """One app command with its framebuffer request brought to the calibrated size.

    Guarded on argv[0] so anything that is not one of our binaries -- magick
    above all -- passes through untouched, which is what makes it safe to route
    every subprocess call in this file through here.
    """
    mult = CALIBRATED_FB_SCALE // (_FB_SCALE or CALIBRATED_FB_SCALE)
    if mult == 1 or not argv or argv[0] not in (RENDER, FOREST):
        return argv
    out = list(argv)
    for i in range(len(out) - 1):
        if out[i] in ("-W", "-H"):
            out[i + 1] = str(int(out[i + 1]) * mult)
    return out


def _run(cmd, **kw):
    """subprocess.run for the app binaries, framebuffer request scaled.

    A wrapper rather than 45 edited argv builders: a site that forgot would read
    the wrong pixels and still report a number, which is the failure this whole
    mechanism exists to prevent.
    """
    return subprocess.run(_scale_argv(cmd), **kw)


def _detect_fb_scale(workdir):
    """Framebuffer pixels per requested pixel, measured on this machine.

    Rendered rather than inferred: the answer is a property of the DISPLAY, not
    the OS, so a Mac on a 1x external monitor answers 1 and must. Falls back to
    the calibrated value if the probe cannot render at all, which leaves the
    suite behaving exactly as it did before this existed.
    """
    global _FB_SCALE
    probe = os.path.join(workdir, "_fbscale.ppm")
    scene = asset("parallax_fixture.gltf")
    r = subprocess.run([RENDER, "-m", scene, "-x", "-f", "1", "-W", "100", "-H", "100",
                        "-S", probe], capture_output=True, text=True)
    if r.returncode == 0 and os.path.exists(probe):
        w, _, _ = _read_ppm(probe)
        _FB_SCALE = max(1, int(round(w / 100.0)))
    else:
        _FB_SCALE = CALIBRATED_FB_SCALE
    return _FB_SCALE


FOREST = _bin("forest")


GAMETEST = _bin("gametest")


# Every --*-probe reader below spawns gametest and reads one case out of the stream.
# This is the half they share; what differs is the grammar, which each keeps.
#
# The timeout is the reason this is one function rather than five. None of the five had
# one, so a gametest that wedged hung the whole suite rather than failing a group -- and
# a fix applied to the reader you happened to be looking at would have left four behind.
_PROBE_TIMEOUT = 300


def _probe_render(scene, flag, prefix, extra=None, frames=30):
    """Run one of the RENDER app's --*-probe flags headless, and parse what it printed.

    Returns ([{key: str}], combined output). The parsing is _probe_rows below,
    which is also what an app other than render goes through -- this one only
    owns the invocation.
    """
    # No -S of its own, and no workdir: callers read probe ROWS off stdout, and the
    # frames these used to write were a readback and a file write nobody opened. One
    # that also wants the frame passes -S in `extra`.
    cmd = [RENDER, "-m", scene, "-x", "-f", str(frames), "-W", "400", "-H", "300", flag]
    r = _run(cmd + (extra or []), capture_output=True, text=True)
    text = r.stdout + r.stderr
    return _probe_rows(text, prefix), text


def _probe_rows(text, prefix):
    """The `<prefix> [tag] k=v ...` lines in `text`, as dicts in emission order.

    Every probe in the tree emits that shape, so the parsing lives HERE rather
    than being written out per subsystem -- which it was six times before this
    existed, and is still three more in this file that predate it.

    Values stay STRINGS. Callers cast what they need: the emissive probe carries
    `center=1,2,3`, which a blanket float() would drop on the floor.

    Split out of _probe_render, which hardcodes the RENDER app, once a probe
    emitted by another app needed the same loop. A row leading with TWO bare
    tokens does not fit -- _water_probe's grid rows are the case, and would read
    the second as a key.
    """
    rows = []
    for line in text.splitlines():
        if not line.startswith(prefix + " "):
            continue
        parts = line.split()[1:]
        rec = {}
        # A leading bare token is the line's TAG, which is how a probe says which
        # kind of row this is. Dispatching on that rather than on whether some
        # field happens to parse is the lesson _water_fft_probe records.
        if parts and "=" not in parts[0]:
            rec["kind"] = parts[0]
            parts = parts[1:]
        for tok in parts:
            if "=" in tok:
                k, v = tok.split("=", 1)
                rec[k] = v
        rows.append(rec)
    return rows


def _import_fixture_gen(filename, group="layers"):
    """A fixture generator, imported for its constants. Writes nothing on import.

    Returns None if its dependencies are absent. The rest of gates.py is
    stdlib-only and these are its only imports of anything else -- the
    generators pull in numpy and Pillow -- so an environment without them would
    otherwise abort a seven-minute run partway through with a
    ModuleNotFoundError, after the GPU time is already spent. The caller SKIPs
    instead, like it does for a missing fixture or an unbuilt binary.

    `group` names the gate in that SKIP line. It was the literal "layers" until
    a second group wanted this, which would have had the decals gate reporting
    itself as the layers one.
    """
    gen_dir = os.path.join(ROOT, "assets", "generators")
    path = os.path.join(gen_dir, filename)
    # The generators' own directory has to be importable for the duration. A
    # generator imports its siblings by bare name -- fixture_paths, and
    # gen_layer_vt_fixture imports gen_layer_fixture -- which resolves for free
    # when the script RUNS, because Python puts a script's directory on the path
    # itself. Loading one by file location gets no such context, so without this
    # every one of them raises ImportError, is reported as a missing dependency
    # and SKIPs the arm that wanted it. That reads as an environment without
    # numpy rather than as a broken path, and the suite still says it passed.
    sys.path.insert(0, gen_dir)
    try:
        spec = importlib.util.spec_from_file_location(filename[:-3], path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    except ImportError as exc:
        print(f"  {group:<17} SKIP  ({filename[:-3]} needs {exc.name})")
        return None
    finally:
        sys.path.remove(gen_dir)
    return mod


def _shared_constant(include, name):
    """A number a both-language include shares with the C, read from the file rather than
    copied: a twin holding its own copy would follow the old one."""
    with open(os.path.join(ROOT, "cetra", "shaders", "include", include)) as f:
        m = re.search(rf"#define {name} +([0-9.]+)f?\b", f.read())
    return float(m.group(1))
