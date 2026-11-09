"""The ragdoll group: a rig falling over (build, shapes, scale, simulation; spec 12.16)."""

import re
import subprocess

from gatelib.harness import GAMETEST, _PROBE_TIMEOUT, asset


# "ragdoll <case> <label> <key> <numbers...>" from gametest --ragdoll-probe,
# through the shared four-field reader.
_RAGDOLL_PROBE = re.compile(r"^ragdoll ([\w-]+) (\w+) (\w+)((?:\s+-?[\d.]+)+)$", re.M)

# The twelve the build names, in build order. Spelled out rather than counted so
# a bone that stops resolving fails by NAME.
_RAGDOLL_BONES = ["hips", "spine", "chest", "head", "arm_l", "forearm_l", "arm_r",
                  "forearm_r", "thigh_l", "shin_l", "thigh_r", "shin_r"]


def _ragdoll_probe(case, rig=None):
    """{(case, label, key): [floats]} from one gametest --ragdoll-probe run, or None.

    Keyed by CASE as well, which is what keeps the spawn count down: several cases
    share one block in the probe -- asking for `build` also prints the shapes rows,
    asking for `settles` also prints pose and frees -- so the shared reader's
    case filter was throwing away rows this had already paid a whole process for.
    Four spawns where there were seven, and every arm reads the same numbers.
    """
    extra = ["--ragdoll-probe", case]
    if rig:
        extra += ["--puppet", asset(rig)]
    try:
        r = subprocess.run([GAMETEST] + extra, capture_output=True, text=True,
                           timeout=_PROBE_TIMEOUT)
    except subprocess.TimeoutExpired:
        return None
    out = {(c, label, key): [float(v) for v in nums.split()]
           for c, label, key, nums in _RAGDOLL_PROBE.findall(r.stdout + r.stderr)}
    if r.returncode != 0 or not out:
        return None
    return out


def run_ragdoll_gate(workdir):
    """A rig falling over (spec 12.16): what the ragdoll builds, and what it does.

      ragdoll-build    every humanoid bone resolves on BOTH rigs, and the tree the build
                       produced is a tree: one root, every parent earlier in build order
                       than its child, one constraint per body but the root -- structure
                       first, since a rig's spine length decides the COUNT and a single
                       expected number would be wrong on one of the two
      ragdoll-shapes   capsules are DERIVED, not uniform: a thigh is thicker than a
                       forearm and longer than a head, by ratio so it holds on any rig
      ragdoll-mass     no body is orders of magnitude lighter than the heaviest, by the
                       r^2*L a capsule's mass goes as -- the root especially, since every
                       limb and the whole spine hang off it
      ragdoll-scale    the same rig at node scale 1 and 2 gives capsules in exactly 1:2
      ragdoll-settles  dropped from 3 m it falls, lands, and comes to rest rather than
                       jittering or sliding forever
      ragdoll-pose     while active a bone's global comes from its BODY and not the clip
      ragdoll-roundtrip the pose a ragdoll is STARTED from is the pose it reports back
                       before a single step has run -- placing a body from a bone and
                       reading a bone back from a body are inverses, and the pose goes
                       through Jolt in between
      ragdoll-rigid    after 300 steps every joint is still where it belongs: a bone
                       sits its bind distance from the one it hangs from
      ragdoll-frees    building and destroying leaves the world's body count where it
                       started
      ragdoll-animator the path a ragdolled character is actually DRAWN through: an
                       animator ticked while a ragdoll owns the pose still writes the
                       bones, and writes exactly what applying the ragdoll by hand
                       would

    Unlike the display group, the SIMULATION is reachable here: a headless game carries
    a real physics world, so these arms step Jolt and read the bodies back rather than
    asserting only the arithmetic.

    ragdoll-frees is the one that looks like bookkeeping and is not. ~Ragdoll destroys
    its bodies and does NOT remove them, so releasing a ragdoll still added to the world
    destroys bodies the broadphase is still indexing -- which costs nothing visible
    until the body pool runs out, some number of deaths later.

    ragdoll-roundtrip and ragdoll-rigid are here because the six arms above were all
    green over a ragdoll that had never once held together. It fell, it settled, it took
    its pose from its bodies and it freed cleanly, limb by limb, as a cloud of pieces
    that never touched -- a left-handed capsule basis is a reflection, Jolt keeps an
    orientation as a quaternion, no quaternion is a reflection, and the nearest rotation
    came back instead: legs end for end, arms at right angles, every number in range.
    Nothing that measures ONE body can see it. These two measure the relationship
    between two, which is the only thing a ragdoll actually is.

    The counts are REPORTED and the structure is asserted, which is deliberate. The
    first draft of this spec said thirteen bodies, the list was twelve, and a rig with a
    one-segment spine yields eleven -- an arm pinned to a constant would have been
    edited to match whatever came out, which is writing the arm against the code.
    """
    del workdir # the probe builds its own world; nothing is rendered or written
    failed = []

    def note(name, ok):
        # Bookkeeping only; each verdict line names its arm as a literal, which
        # is what gate-arm-docs reads.
        if not ok:
            failed.append(name)

    # Four spawns for eight arms. A `build` run also prints the shapes rows and
    # a `settles` run also prints pose, roundtrip, rigid and frees, so what
    # decides the count is how many distinct (rig, world) setups the group needs
    # and not how many arms read them.
    rigs = [("puppet", None), ("t_pose", "t_pose.fbx")]
    build = {}
    for tag, rig in rigs:
        p = _ragdoll_probe("build", rig)
        if not p:
            print(f"  ragdoll-build SKIP  no probe output for {tag}")
            return ["ragdoll-build"]
        build[tag] = p

    ok, detail_bits = True, []
    for tag, _ in rigs:
        p = build[tag]
        missing = [b for b in _RAGDOLL_BONES if p.get(("build", b, "bone"), [-1])[0] < 0]
        # The structure, read back from the system rather than restated: which
        # slot each body ended up hanging from. Constraints are counted from that
        # column, so this is an assertion about what the build produced and not
        # the arithmetic identity "bodies - 1 == bodies - 1" it used to be.
        parents = {b: int(p[("build", b, "bone")][1])
                   for b in _RAGDOLL_BONES if p.get(("build", b, "bone"), [-1])[0] >= 0}
        roots = [b for b, par in parents.items() if par < 0]
        # Parent-first, which is the ordering CreateRagdoll indexes on: a slot's
        # parent must sit earlier in the build order than the slot itself.
        unordered = [b for b, par in parents.items()
                     if par >= 0 and par >= _RAGDOLL_BONES.index(b)]
        bodies = int(p[("build", "count", "bodies")][0])
        cons = int(p[("build", "count", "constraints")][0])
        if missing or len(roots) != 1 or unordered or cons != bodies - 1:
            ok = False
        detail_bits.append(f"{tag} {bodies} bodies / {cons} constraints"
                           + (f", UNRESOLVED {missing}" if missing else "")
                           + (f", ROOTS {roots}" if len(roots) != 1 else "")
                           + (f", OUT OF ORDER {unordered}" if unordered else ""))
    print(f"  ragdoll-build {'PASS' if ok else 'FAIL'}  "
          f"{'; '.join(detail_bits)} (want all twelve resolved, exactly one root, every "
          f"parent earlier than its child, and constraints == bodies - 1)")
    note("ragdoll-build", ok)

    # Ratios, not absolute sizes: the two rigs are different heights and a bar in
    # metres would hold on one of them by accident.
    p = build["t_pose"]
    thigh_r = p[("shapes", "thigh_l", "capsule")][0]
    fore_r = p[("shapes", "forearm_l", "capsule")][0]
    thigh_h = p[("shapes", "thigh_l", "capsule")][1]
    head_h = p[("shapes", "head", "capsule")][1]
    ok = thigh_r > fore_r * 1.2 and thigh_h > head_h * 1.2
    print(f"  ragdoll-shapes {'PASS' if ok else 'FAIL'}  thigh radius {thigh_r:.4f} vs "
          f"forearm {fore_r:.4f}, thigh half-height {thigh_h:.4f} vs head {head_h:.4f} "
          f"(want each at least 1.2x, or the capsules are not derived from the rig)")
    note("ragdoll-shapes", ok)

    # A capsule's mass goes as r^2 * L, and the SPREAD is what decides whether the
    # thing reads as a body. ragdoll-shapes compares a thigh against a forearm and a
    # head and cannot see this: on an imported character those three were all sane
    # while the hips -- the root everything hangs off -- came out at a eleven
    # thousandth of a shin, because the bone carried no skin of its own and the
    # fallback sized it off the few centimetres to the next joint. Every arm was
    # green and the character melted.
    ok, bits = True, []
    for tag, _ in rigs:
        p = build[tag]
        mass = {}
        for b in _RAGDOLL_BONES:
            row = p.get(("shapes", b, "capsule"))
            if row:
                r, h = row
                mass[b] = r * r * 2.0 * (r + h)
        if not mass:
            ok = False
            bits.append(f"{tag} NO CAPSULES")
            continue
        lo = min(mass, key=mass.get)
        hi = max(mass, key=mass.get)
        spread = mass[hi] / mass[lo]
        if spread > 100.0:
            ok = False
        bits.append(f"{tag} {spread:.1f}x ({lo} to {hi})")
    print(f"  ragdoll-mass {'PASS' if ok else 'FAIL'}  heaviest body over lightest: "
          f"{'; '.join(bits)} (want under 100x; a body four orders of magnitude light is "
          f"not a small limb, it is a joint with no inertia, and what that looks like is a "
          f"character that melts)")
    note("ragdoll-mass", ok)

    p = _ragdoll_probe("scale", "t_pose.fbx")
    if not p:
        print("  ragdoll-scale SKIP  no probe output")
        note("ragdoll-scale", False)
    else:
        worst = 0.0
        for bone in _RAGDOLL_BONES:
            row = p.get(("scale", bone, "ratio"))
            if not row:
                continue
            worst = max(worst, abs(row[0] - 2.0), abs(row[1] - 2.0))
        ok = worst < 1e-4
        print(f"  ragdoll-scale {'PASS' if ok else 'FAIL'}  node scale 1 -> 2 moves every "
              f"capsule by 2.0, worst error {worst:.6f} (want < 1e-4: everything is measured "
              f"in model space and scaled once, and 12.10 shipped a live bug through exactly "
              f"this gap)")
        note("ragdoll-scale", ok)

    # One run for the last three arms: they are one fall, measured at three
    # points, and the probe prints all of it from the same stepped world.
    sim = _ragdoll_probe("settles")
    if not sim:
        print("  ragdoll-settles SKIP  no probe output")
        for name in ("ragdoll-settles", "ragdoll-pose", "ragdoll-frees"):
            note(name, False)
        return failed

    drop = sim[("settles", "hips", "drop")][0]
    speed = sim[("settles", "hips", "speed")][0]
    ok = drop > 1.0 and speed < 0.2
    print(f"  ragdoll-settles {'PASS' if ok else 'FAIL'}  the hips fell {drop:.4f} m "
          f"(want > 1: it has to actually fall) and are moving {speed:.4f} m/s after 300 "
          f"steps (want < 0.2: at rest rather than sliding)")
    note("ragdoll-settles", ok)

    moved = sim[("pose", "hips", "moved")][0]
    ok = moved > 0.5
    print(f"  ragdoll-pose {'PASS' if ok else 'FAIL'}  applying the ragdoll moved the hips "
          f"bone {moved:.4f} m from where the clip left it (want > 0.5: the body is the "
          f"pose now, not a correction to it)")
    note("ragdoll-pose", ok)

    trips = {b: sim[("roundtrip", b, "off")][0] for b in _RAGDOLL_BONES
             if ("roundtrip", b, "off") in sim}
    worst_bone = max(trips, key=trips.get) if trips else None
    worst = trips[worst_bone] if trips else 1.0
    ok = len(trips) == len(_RAGDOLL_BONES) and worst < 1e-3
    print(f"  ragdoll-roundtrip {'PASS' if ok else 'FAIL'}  before any step, the started pose "
          f"comes back {worst:.6f} m out at worst ({worst_bone}), over {len(trips)} bodies "
          f"(want < 1e-3 on all twelve: the two conversions are inverses, and the pose goes "
          f"through a quaternion in between, which is where a reflected basis is lost)")
    note("ragdoll-roundtrip", ok)

    drifts = {b: sim[("rigid", b, "drift")][1] for b in _RAGDOLL_BONES
              if ("rigid", b, "drift") in sim}
    worst_bone = max(drifts, key=drifts.get) if drifts else None
    worst = drifts[worst_bone] if drifts else 1.0
    ok = len(drifts) == len(_RAGDOLL_BONES) - 1 and worst < 0.02
    print(f"  ragdoll-rigid {'PASS' if ok else 'FAIL'}  after 300 steps the worst joint has "
          f"drifted {worst:.6f} of its own limb ({worst_bone}), over {len(drifts)} joints "
          f"(want < 0.02 on all eleven: a bone stays its bind distance from the one it hangs "
          f"from, or the character has come apart while every other arm stayed green)")
    note("ragdoll-rigid", ok)

    before, during, after = (int(v) for v in sim[("frees", "world", "bodies")])
    ok = after == before and during > before
    print(f"  ragdoll-frees {'PASS' if ok else 'FAIL'}  world bodies {before} -> {during} "
          f"-> {after} (want it to rise and come back exactly: ~Ragdoll destroys bodies "
          f"without removing them, so a missed removal is invisible until the pool runs "
          f"out)")
    note("ragdoll-frees", ok)

    # ---- ragdoll-animator
    #
    # Every arm above calls ragdoll_apply ITSELF. The game reaches it through
    # animator_update -> animation_state_apply_pose, and spec 12.20 stood the
    # animator down while a ragdoll owns the pose without noticing that the apply
    # it returned before was where the ragdoll got written. Nine green arms and a
    # character T-posing through the floor, found by looking at it.
    p = _ragdoll_probe("animator")
    moved = p.get(("animator", "hips", "moved")) if p else None
    agrees = p.get(("animator", "bones", "agrees")) if p else None
    if moved is None or agrees is None:
        ok, detail = False, "the probe failed or measured nothing"
    else:
        ok = moved[0] > 0.5 and agrees[0] < 1e-5
        detail = (f"through animator_update alone the hips left bind by {moved[0]:.4f} m "
                  f"(want > 0.5: a return before the apply leaves the rig at bind) and every "
                  f"bone matches applying the ragdoll directly to {agrees[0]:.6f} (want 0)")
    print(f"  ragdoll-animator {'PASS' if ok else 'FAIL'}  {detail}")
    note("ragdoll-animator", ok)

    return failed
