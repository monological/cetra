"""The fixture-gen group: fixture generators (every gen_*.py reproduces its asset)."""

import re
import os
import shutil
import subprocess
import sys

from gatelib.harness import ASSET_KINDS, ROOT


def run_fixture_gen_gate(workdir):
    """Every gen_*.py still emits the asset committed for it, where it is committed.

      fixture-gen  each generator runs and reproduces its committed outputs, in both
                   content and LOCATION. No GPU and no render, which is why it can
                   afford to cover the whole corpus.

    It exists because gen_water_fixture.py silently stopped emitting its water block: a
    regeneration would have stripped 21 authored keys and taken twenty-odd water arms with
    it, and nothing in the suite could have said so. The same is true of every other
    fixture whose generator nobody has run since committing it.

    Run as a COPY in a scratch directory rather than through an output-directory argument.
    A generator resolves its outputs through fixture_paths, which resolves against ITS OWN
    __file__ -- so mirroring the helper alongside the scripts moves the writes with them,
    and 54 scripts need no convention they have no other use for. The scratch directory has
    to MIRROR the tree, not just hold the scripts: two generators load a module from
    ../../tools and one reads constants out of ../../cetra/shaders/include, both symlinked
    rather than copied because nothing writes outside its own directory, and the kind
    directories are pre-created because asset_path serves reads as well as writes.

    COPIED AND NEVER SYMLINKED, and that is not a preference. sys.path[0] is REALPATHed
    while __file__ is not, so a symlinked script imports its siblings from the real tree
    and every write escapes into the committed corpus; a symlinked image is worse, since
    the first generator to save one truncates the committed file through the link.
    """
    src_dir = os.path.join(ROOT, "assets")

    # ONE WALK, and the arm's whole picture of the corpus comes out of it: which
    # generators exist, which images they read back, and where each committed
    # asset lives. It is deliberately not a set of globs per kind -- this arm
    # asserts that a generator reproduces what is committed, and that claim does
    # not depend on the directory layout. Indexing by BASENAME is what makes it
    # layout-agnostic: a generator emits a filename, and where the committed twin
    # sits is this index's problem rather than the gate's.
    #
    # The walk stops at the self-contained bundles. abandoned_window, ivy_arcade
    # and raiden carry their own scenes and their own producers, and this arm has
    # never run any of them: ivy_arcade/gen_ivy_mat.py writes into its own
    # textures/ and reads across into abandoned_window's, and build_arcade.py
    # needs Blender. Pruning them keeps this arm's assertions about the corpus
    # fixture-gen governs instead of failing on a gap it was never asked to
    # close -- the gap is real and belongs in docs/verification.md, not hidden
    # behind a name a loop happens not to match.
    # A .fbm is assimp's sidecar convention -- the textures belonging to one FBX,
    # named after it. Nothing generated reads them, so they are pruned with the
    # bundles: collecting them would hand every generator's mirror nine images it
    # never asked for, and a flat mirror makes two sidecars with one basename
    # clobber each other. Pruned by SUFFIX, so the next .fbm needs no edit here.
    self_contained = {"abandoned_window", "ivy_arcade", "raiden", "__pycache__"}
    found, helpers, inputs, committed_at, ambiguous = set(), [], [], {}, []
    for walk_root, walk_dirs, walk_files in os.walk(src_dir):
        walk_dirs[:] = [d for d in walk_dirs
                        if d not in self_contained and not d.endswith(".fbm")]
        for f in sorted(walk_files):
            path = os.path.join(walk_root, f)
            if f.startswith("gen_") and f.endswith(".py"):
                found.add(path)
            elif f.endswith(".py"):
                # A module the generators import rather than one this arm runs
                # (fixture_paths). It has to reach the mirror or every generator
                # dies on the import, so it is collected here and copied below.
                helpers.append(path)
            elif f.endswith(".png") and not f.endswith("_golden.png"):
                inputs.append(path)
            # A basename in two places would make "the committed twin" a choice,
            # and this arm would quietly compare against whichever the walk
            # reached first. Refuse instead: it is the confusion a move creates.
            if f in committed_at:
                ambiguous.append(
                    f"{f}: committed at {os.path.relpath(committed_at[f], src_dir)} "
                    f"and {os.path.relpath(path, src_dir)}")
            committed_at[f] = path
    gens = sorted(found)
    helpers = sorted(helpers)
    inputs = sorted(inputs)
    # Every module this arm ships, by import name. A generator that cannot find
    # one of these has a broken fixture; anything else it cannot find (numpy,
    # PIL) is a property of the machine. The two are told apart below.
    own_modules = {os.path.basename(p)[:-3] for p in gens + helpers}
    # Byte equality is the contract a .gltf, .cscn, .ies, .cube or .glsl has -- all five are
    # text a generator writes deterministically. It is NOT the contract a .png has, whose
    # bytes come out of PIL and zlib and move with those libraries rather than with the
    # fixture. Binary outputs are held to being emitted and non-empty, and the count is
    # printed so the weaker check does not read as coverage it is not.
    #
    # .cube joined late and that is the point of listing them here rather than defaulting
    # to text: 11.58 committed 1.2 MB of tables, verified their regeneration by hand, and
    # left them in the BINARY bucket -- so the printed "N binary emitted non-empty" read
    # as PNG coverage while four tables had none at all. .glsl (spec 13.29) repeated it,
    # with the shaders every shader-hooks arm runs.
    text_ext = (".gltf", ".cscn", ".ies", ".cube", ".glsl")

    drifted, missing_dep, compared, binary = [], [], 0, 0
    for gen in gens:
        name = os.path.basename(gen)
        run_root = os.path.join(workdir, "gen", name[:-3])
        run_dir = os.path.join(run_root, "assets")
        os.makedirs(run_dir, exist_ok=True)
        # The kind directories exist before a generator runs, because asset_path
        # serves READS as well as writes and must not mkdir on a read -- a
        # generator asking where a texture lives would otherwise create the
        # directory as a side effect of looking.
        for kind in set(ASSET_KINDS.values()):
            os.makedirs(os.path.join(run_dir, kind), exist_ok=True)
        tools_link = os.path.join(run_root, "tools")
        if not os.path.exists(tools_link):
            os.symlink(os.path.join(ROOT, "tools"), tools_link)
        # ...and cetra, for the same reason one level further out:
        # gen_purkinje_fixture reads the shipped ramp edges and the rod tint out
        # of the shader includes rather than restating them, so its straddle
        # asserts are claims about the REAL ramp instead of about its own copy.
        # Symlinked, not copied: nothing here writes into it, and a mirror of the
        # engine tree per generator would be absurd.
        cetra_link = os.path.join(run_root, "cetra")
        if not os.path.exists(cetra_link):
            os.symlink(os.path.join(ROOT, "cetra"), cetra_link)
        # ALL the generators, not just the one under test: gen_layer_vt_fixture
        # imports gen_layer_fixture for its shared constants, and a mirror
        # without the sibling raised ModuleNotFoundError -- which the classifier
        # below filed as a machine dependency, so the vt fixture had ZERO drift
        # coverage while the pass line read one generator short. Unrun siblings
        # cannot read as outputs: the mtime stamps only count writes, and -B
        # keeps their import from minting a __pycache__ the scan would count.
        #
        # MIRRORED AT THEIR OWN SUBPATHS, not flattened onto run_dir. A generator
        # resolves everything from its own location, so where it sits inside the
        # mirror is what its ../.. reaches resolve through -- flattened, a script
        # from assets/generators/ would land beside assets/ and climb one level
        # too far, past the symlinks above and out of the scratch tree entirely.
        for path in gens + helpers + inputs:
            dest = os.path.join(run_dir, os.path.relpath(path, src_dir))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copy2(path, dest)
        stamps = {}
        for stamp_root, _stamp_dirs, stamp_files in os.walk(run_dir):
            for f in stamp_files:
                p = os.path.join(stamp_root, f)
                stamps[os.path.relpath(p, run_dir)] = os.stat(p).st_mtime_ns

        script = os.path.join(run_dir, os.path.relpath(gen, src_dir))
        proc = subprocess.run([sys.executable, "-B", script],
                              capture_output=True, text=True,
                              cwd=os.path.dirname(script))
        if proc.returncode != 0:
            tail = (proc.stderr or proc.stdout).strip().splitlines()
            last = tail[-1][:90] if tail else "no output"
            # A generator needing numpy or PIL is a property of this machine, not of the
            # fixture, so it is reported apart from a real drift rather than as one. A
            # missing module this arm SHIPS is a property of the fixture and must fail
            # loudly -- tested against the set of modules actually copied into the mirror
            # rather than against a gen_ prefix, which said the same thing while only
            # covering the generators. fixture_paths is a sibling every generator imports
            # and matches no prefix, so the narrower test filed all 54 as a machine fault
            # and left `drifted` empty.
            absent = re.search(r"No module named '([\w.]+)'", proc.stderr or "")
            is_dep = ("ModuleNotFoundError" in (proc.stderr or "")
                      and not (absent and absent.group(1).split(".")[0] in own_modules))
            (missing_dep if is_dep
             else drifted).append(f"{name}: exited {proc.returncode} ({last})")
            continue

        # Keyed by subpath, to match the stamps: a generator writes beside itself
        # or into a sibling kind directory, and a flat listing would miss both.
        # The emitted NAME is still what the committed index is keyed by, so the
        # comparison below stays a basename lookup.
        seen_now = {}
        for wrote_root, _wrote_dirs, wrote_files in os.walk(run_dir):
            for f in wrote_files:
                p = os.path.join(wrote_root, f)
                seen_now[os.path.relpath(p, run_dir)] = os.stat(p).st_mtime_ns
        script_rel = os.path.relpath(gen, src_dir)
        wrote = sorted(rel for rel, mtime in seen_now.items()
                       if rel != script_rel and stamps.get(rel) != mtime)
        if not wrote:
            drifted.append(f"{name}: wrote nothing")
            continue
        for rel in wrote:
            # The subpath says where it landed; the basename is what the
            # committed index is keyed by, and the two differ once a generator
            # emits into a sibling directory rather than beside itself.
            f = os.path.basename(rel)
            committed, regenerated = committed_at.get(f), os.path.join(run_dir, rel)
            if not committed or not os.path.exists(committed):
                drifted.append(f"{f}: emitted but not committed")
                continue
            # WHERE it landed, not only what it holds. Indexing by basename is
            # what makes this arm survive a re-split, and it is also what made
            # it blind to one: spec 12.8 shipped with asset_path unrouted, so
            # every generator wrote a flat file at the corpus root while this
            # arm found the committed twin under its kind directory, compared
            # the bytes, and passed. Content equality alone cannot tell a
            # generator that works from one that has stopped updating the
            # corpus at all.
            want = os.path.relpath(os.path.dirname(committed), src_dir)
            got = os.path.dirname(rel) or "."
            if want != got:
                drifted.append(f"{f}: emitted into {got}, committed in {want}")
            if f.endswith(text_ext):
                compared += 1
                with open(committed) as f_committed, open(regenerated) as f_regenerated:
                    if f_committed.read() != f_regenerated.read():
                        drifted.append(f"{f}: DRIFTED from the committed asset")
            else:
                binary += 1
                if os.path.getsize(regenerated) == 0:
                    drifted.append(f"{f}: emitted empty")

    # THE ARM'S OWN REACH IS UNDER TEST, and nothing above can see it fail.
    # Every check here is driven by `gens` and `committed_at`, both of which come
    # out of one walk -- so a walk that finds nothing runs no generator, compares
    # no asset, collects no drift, and prints PASS with the whole corpus
    # uncovered. These two assertions are what make the counts mean something.
    #
    # Counts rather than hardcoded numbers: a literal would have to be edited by
    # whoever adds the 55th generator, which is the kind of chore that gets done
    # by lowering the number to match.
    # `ambiguous` is an INDEPENDENT claim about the corpus, not a rung on the
    # coverage ladder -- chained, a tree with both a duplicated basename and
    # dead discovery reported only the duplicate, and the second problem cost a
    # second round trip to find. The two below do subsume each other: no
    # generators means nothing compared, so the elif picks the sharper message.
    if ambiguous:
        drifted.insert(0, "; ".join(sorted(set(ambiguous))[:4]))
    if not gens:
        drifted.insert(0, "no generators found under assets/; this arm ran nothing")
    elif not compared or not binary:
        drifted.insert(0, f"compared {compared} text and {binary} binary assets; "
                          "a run that verifies nothing is not a pass")

    ok = not drifted
    detail = (f"{len(gens)} generators, {compared} text assets byte-identical, "
              f"{binary} binary emitted non-empty" if ok else "; ".join(drifted[:6]))
    if missing_dep:
        detail += f"; {len(missing_dep)} skipped for a missing module"
    print(f"  fixture-gen  {'PASS' if ok else 'FAIL'}  {detail}")
    return [] if ok else ["fixture-gen"]
