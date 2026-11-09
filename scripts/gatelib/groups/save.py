"""The save group: save serialization (entities, spawners, drops, migrations; spec 12.3)."""

import json
import re
import os

from gatelib.probes import _gametest_probe, _gametest_probe_text


# "save <case> <label> <key> <numbers...>" from gametest --save-probe: the four-field
# shape the anim, ui and ik probes print too, read back through _gametest_probe.
_SAVE_PROBE = re.compile(r"^save ([\w-]+) (\w+) (\w+)((?:\s+-?[\d.]+)+)$", re.M)


def _save_probe_env(case, workdir):
    """The environment one --save-probe case runs in.

    Each case writes into its OWN directory under the run's workdir, through the
    same CETRA_SETTINGS_DIR the settings path honours. Two reasons: a probe must
    never write a real player's save, and the cases share one slot name, so they
    would otherwise read each other's file.
    """
    env = dict(os.environ)
    env["CETRA_SETTINGS_DIR"] = os.path.join(workdir, "save-" + case)
    return env


def _save_probe(case, workdir):
    """{(label, key): [numbers]} from one gametest --save-probe run, or None."""
    return _gametest_probe("--save-probe", _SAVE_PROBE, case,
                           env=_save_probe_env(case, workdir))


def _save_probe_file(case, workdir):
    """The JSON a --save-probe case left on disk, or None."""
    try:
        with open(os.path.join(workdir, "save-" + case, "probe.json"), "r",
                  encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def run_save_gate(workdir):
    """Save serialization (spec 12.3), asserted where it is a pure function of a
    file and a world. No window and no frame: the probe builds a scene, a physics
    world and a handful of entities of its own, so every number below is one this
    file can state in closed form.

      save-roundtrip a write, the live values ZEROED, and a read: all eight app
                     fields come back. Zeroed rather than merely left alone,
                     because against values that still hold the answer a reader
                     that stores nothing at all would pass.
      save-format    what the writer actually put on disk: a version per SECTION
                     rather than one for the file, and a component keyed by NAME.
                     The ComponentType enum is positional, so a number there
                     would re-point every existing save the day a member is
                     retired -- which is the most expensive silent corruption
                     this format has, and the reason it is asserted against a
                     file that really contains an entity.
      save-entities  a pose and a velocity survive being moved away and zeroed.
                     The pose proves it went into the BODY: entity->position is
                     a copy sync_physics_to_entities rewrites every step, so a
                     restore that wrote only that would read back as nothing.
      save-spawned   a crate destroyed outright comes back from its recipe, at
                     the position it had. Its size and colour live nowhere else.
      save-missing   a record naming an entity the scene no longer has drops
                     exactly one and the rest of the file still loads.
      save-unknown-spawner a recipe this build does not have drops one and counts
                     it, rather than costing the file -- one unbuildable crate
                     must not cost a player everything else in the save.
      save-floor     a file below the version floor applies NOTHING; the live
                     value is the one that was already there.
      save-migrate   a key whose MEANING changed (percent at version 1, a
                     fraction at version 2) is repaired by one registered step.
                     This is the only case a tagged format cannot detect itself.
      save-unsaved   an entity marked `unsaved` (spec 13.49) has no record in the
                     file, and a load leaves it where it is even against a file
                     written before the mark, whose record drops as missing.
      save-names     two saved entities with one name are said by name when the
                     file is written, and only the first is kept, since a load
                     gives every record by a name to the first entity that has
                     it; a unique name is not said, and is kept.
      save-enum      an index rides as its NAME: it reads back, and a build whose
                     table lost that name leaves the field and counts one, with
                     the file still loading. An index the table does not hold
                     costs that key and not the write.
      save-peek      one section read into a caller's object with nothing live
                     moved, the file's version and wall clock with it; a table by
                     ADDRESS is refused, since it would write the live value.
    """
    failed = []

    def note(name, ok):
        # Bookkeeping only. Each verdict LINE below spells its arm's name out as
        # a literal, because that source text is what gate-docs reads to tell
        # the documented list from the one that actually runs.
        if not ok:
            failed.append(name)

    def close(a, b, tol=1e-4):
        return abs(a - b) <= tol

    # ---- roundtrip
    p = _save_probe("roundtrip", workdir)
    if not p:
        ok, detail = False, "the probe produced nothing"
    else:
        want = {"box_count": 7.0, "player_yaw": 1.25, "chaser_yaw": -0.5, "heart_timer": 0.25,
                "catch_cooldown": 1.5, "door_open_pending": 1.0, "door_open_velocity": -6.0,
                "player_touching_door": 1.0}
        got = {k: p[("read", k)][0] for k in want if ("read", k) in p}
        ok = (p.get(("read", "ok"), [0])[0] == 1.0 and len(got) == len(want)
              and all(close(got[k], want[k]) for k in want))
        detail = (f"{len(got)} of {len(want)} fields back from a zeroed struct, "
                  f"box_count {got.get('box_count', -1):.0f} (want 7), "
                  f"player_yaw {got.get('player_yaw', -1):.3f} (want 1.25), "
                  f"door_open_velocity {got.get('door_open_velocity', 0):.1f} (want -6)")
    print(f"  save-roundtrip        {'PASS' if ok else 'FAIL'}  {detail}")
    note("save-roundtrip", ok)

    # ---- format: read back what the writer actually put on disk.
    # The entities case is run HERE rather than at its own arm below, because
    # this arm needs a file that contains an entity -- the roundtrip case writes
    # none, so reading that one could never see a component at all.
    p_entities = _save_probe("entities", workdir)
    doc = _save_probe_file("entities", workdir)
    if doc is None:
        ok, detail = False, "cannot read what the probe wrote"
    else:
        sections = [k for k in ("world", "gametest", "entities") if k in doc]
        versioned = [k for k in sections if isinstance(doc[k], dict) and "version" in doc[k]]
        entry = (doc.get("entities", {}).get("list") or [{}])[0]
        named = "rigid_body" in entry
        # Every component key must be a name. A digit where a name belongs is
        # the enum leaking onto disk.
        numeric = [k for k in entry if k.isdigit()]
        ok = (len(versioned) == 3 and len(sections) == 3 and "version" in doc
              and named and not numeric)
        detail = (f"{len(versioned)} of {len(sections)} sections carry their own version "
                  f"(want 3), the file carries one too, and the component rides as "
                  f"'rigid_body' ({named}) with {len(numeric)} numeric keys (want 0)")
    print(f"  save-format           {'PASS' if ok else 'FAIL'}  {detail}")
    note("save-format", ok)

    # ---- entities (the run the format arm above already made)
    p = p_entities
    if not p:
        ok, detail = False, "the probe produced nothing"
    else:
        pos = p.get(("read", "position"), [])
        vel = p.get(("read", "velocity"), [])
        count = p.get(("read", "count"), [0])[0]
        ok = (count == 1.0 and len(pos) == 3 and len(vel) == 3
              and close(pos[0], 1.0) and close(pos[1], 2.0) and close(pos[2], 3.0)
              and close(vel[0], 4.0) and close(vel[1], -5.0) and close(vel[2], 6.0))
        detail = (f"{count:.0f} entity restored to {pos} (want [1, 2, 3], moved to -9 first) "
                  f"with velocity {vel} (want [4, -5, 6], zeroed first)")
    print(f"  save-entities         {'PASS' if ok else 'FAIL'}  {detail}")
    note("save-entities", ok)

    # ---- spawned
    p = _save_probe("spawned", workdir)
    if not p:
        ok, detail = False, "the probe produced nothing"
    else:
        before = p.get(("before", "found"), [1])[0]
        after = p.get(("after", "found"), [0])[0]
        spawned = p.get(("after", "count"), [0])[0]
        pos = p.get(("after", "position"), [])
        ok = (before == 0.0 and after == 1.0 and spawned == 1.0 and len(pos) == 3
              and close(pos[0], 2.0) and close(pos[1], 8.0) and close(pos[2], -3.0))
        detail = (f"destroyed (found {before:.0f}), then {spawned:.0f} rebuilt from its recipe "
                  f"(found {after:.0f}) at {pos} (want [2, 8, -3])")
    print(f"  save-spawned          {'PASS' if ok else 'FAIL'}  {detail}")
    note("save-spawned", ok)

    # ---- drops: one file, two policies
    p = _save_probe("drops", workdir)
    if not p:
        ok, detail = False, "the probe produced nothing"
        missing = unknown = -1.0
        loaded = 0.0
    else:
        missing = p.get(("read", "missing"), [-1])[0]
        unknown = p.get(("read", "unknown_spawner"), [-1])[0]
        loaded = p.get(("read", "ok"), [0])[0]
        ok = missing == 1.0 and loaded == 1.0
        detail = (f"an absent entity dropped {missing:.0f} (want 1) and the file still "
                  f"loaded (ok {loaded:.0f}, want 1)")
    print(f"  save-missing          {'PASS' if ok else 'FAIL'}  {detail}")
    note("save-missing", ok)

    ok = unknown == 1.0 and loaded == 1.0
    detail = (f"an unregistered recipe dropped {unknown:.0f} (want 1) and the file still "
              f"loaded (ok {loaded:.0f}, want 1)")
    print(f"  save-unknown-spawner  {'PASS' if ok else 'FAIL'}  {detail}")
    note("save-unknown-spawner", ok)

    # ---- floor
    p = _save_probe("floor", workdir)
    if not p:
        ok, detail = False, "the probe produced nothing"
    else:
        refused = p.get(("read", "ok"), [1])[0]
        yaw = p.get(("read", "player_yaw"), [0])[0]
        ok = refused == 0.0 and close(yaw, 3.0)
        detail = (f"refused (ok {refused:.0f}, want 0) and player_yaw left at {yaw:.1f} "
                  f"(want 3, the live value; the file said 99)")
    print(f"  save-floor            {'PASS' if ok else 'FAIL'}  {detail}")
    note("save-floor", ok)

    # ---- migrate
    p = _save_probe("migrate", workdir)
    if not p:
        ok, detail = False, "the probe produced nothing"
    else:
        steps = p.get(("read", "steps"), [0])[0]
        scaled = p.get(("read", "scaled"), [0])[0]
        ok = steps == 1.0 and close(scaled, 0.75)
        detail = (f"{steps:.0f} step run (want 1) turning 75 percent at version 1 into "
                  f"{scaled:.3f} at version 2 (want 0.750)")
    print(f"  save-migrate          {'PASS' if ok else 'FAIL'}  {detail}")
    note("save-migrate", ok)

    # ---- unsaved
    p = _save_probe("unsaved", workdir)
    doc = _save_probe_file("unsaved", workdir)
    if not p or doc is None:
        ok, detail = False, "the probe produced nothing"
    else:
        kept = p.get(("read", "kept"), [])
        marked = p.get(("read", "marked"), [])
        restored = p.get(("read", "restored"), [0])[0]
        dropped = p.get(("read", "dropped"), [0])[0]
        names = [e.get("name") for e in doc.get("entities", {}).get("list", [])]
        ok = (len(kept) == 3 and len(marked) == 3
              and all(close(a, b) for a, b in zip(kept, (1.0, 2.0, 3.0)))
              and all(close(a, -9.0) for a in marked)
              and restored == 1.0 and dropped == 1.0
              and names == ["probe_kept"])
        detail = (f"kept back at {kept} (want [1, 2, 3]), marked left at {marked} (want -9s), "
                  f"{restored:.0f} restored and {dropped:.0f} dropped (want 1 and 1); written "
                  f"again, the file holds {names} (want ['probe_kept'])")
    print(f"  save-unsaved          {'PASS' if ok else 'FAIL'}  {detail}")
    note("save-unsaved", ok)

    # ---- names: what the writer SAYS, so the arm reads the run's whole output
    text, code = _gametest_probe_text("--save-probe", "names",
                                      env=_save_probe_env("names", workdir))
    doc = _save_probe_file("names", workdir) or {}
    names = [e.get("name") for e in doc.get("entities", {}).get("list", [])]
    twins = text.count("saved entity is named 'twin'")
    solos = text.count("saved entity is named 'solo'")
    wrote = "save names wrote ok 1" in text
    ok = (code == 0 and wrote and twins == 1 and solos == 0
          and names.count("twin") == 1 and names.count("solo") == 1)
    detail = (f"the write named 'twin' {twins} time(s) (want 1, for the second of two) and "
              f"'solo' {solos} (want 0), and still wrote ({wrote}) one record each for 'twin' "
              f"and 'solo' ({names.count('twin')}/{names.count('solo')}, want 1/1)")
    print(f"  save-names            {'PASS' if ok else 'FAIL'}  {detail}")
    note("save-names", ok)

    # ---- enum
    p = _save_probe("enum", workdir)
    doc = _save_probe_file("enum", workdir)
    if not p or doc is None:
        ok, detail = False, "the probe produced nothing"
    else:
        room = p.get(("read", "room"), [-1])[0]
        patched = p.get(("patched", "room"), [-1])[0]
        counted = p.get(("patched", "dropped"), [0])[0]
        loaded = p.get(("patched", "ok"), [0])[0]
        outside = p.get(("outside", "wrote"), [0])[0]
        summary = doc.get("summary", {})
        # Read back as 2 here, under a table whose index 2 is another name, can only
        # have been written as the name.
        ok = (room == 2.0 and patched == 1.0 and counted == 1.0 and loaded == 1.0
              and outside == 1.0 and "room" not in summary and "played" in summary)
        detail = (f"'attic' back as {room:.0f} (want 2); under a table without it the field "
                  f"stayed {patched:.0f} (want 1), {counted:.0f} counted (want 1), the file "
                  f"loaded ({loaded:.0f}); index 7 wrote the file ({outside:.0f}) without its "
                  f"key ({'room' not in summary})")
    print(f"  save-enum             {'PASS' if ok else 'FAIL'}  {detail}")
    note("save-enum", ok)

    # ---- peek
    p = _save_probe("peek", workdir)
    if not p:
        ok, detail = False, "the probe produced nothing"
    else:
        read_ok = p.get(("read", "ok"), [0])[0]
        played = p.get(("read", "played"), [0])[0]
        at = p.get(("read", "at"), [])
        live = p.get(("live", "played"), [-1])[0]
        version = p.get(("stamp", "version"), [0])[0]
        age = p.get(("stamp", "age"), [-99])[0]
        refused = p.get(("refused", "ok"), [0])[0]
        yaw = p.get(("refused", "player_yaw"), [0])[0]
        ok = (read_ok == 1.0 and close(played, 42.5) and len(at) == 3
              and all(close(a, b) for a, b in zip(at, (1.0, 2.0, 3.0)))
              and live == 0.0 and version == 1.0 and -1.0 <= age <= 5.0
              and refused == 1.0 and close(yaw, 3.0))
        detail = (f"peeked played {played:.1f} at {at} (want 42.5 at [1, 2, 3]) with the live "
                  f"one left at {live:.1f} (want 0); version {version:.0f}, stamped {age:.0f} s "
                  f"before the write returned (want 0..5); the address table refused "
                  f"({refused:.0f}) with player_yaw left at {yaw:.2f} (want 3)")
    print(f"  save-peek             {'PASS' if ok else 'FAIL'}  {detail}")
    note("save-peek", ok)

    return failed
