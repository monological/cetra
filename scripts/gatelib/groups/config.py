"""The config group: the session dumped to JSON and restored from it (spec 11.71)."""

import json
import re
import os

from gatelib.harness import ROOT, asset
from gatelib.images import compare
from gatelib.probes import CONFIG_FIXTURE, _config_run, _config_variant


# A look nothing defaults to, so a restore that silently did nothing cannot pass by
# landing on the same frame. Every one of these is a different mechanism: two effect
# switches, a tonemap enum, and three finishing-stack floats.
CONFIG_LOOK = ["--no-ssr", "--no-ssao", "--tonemap", "agx", "--sharpen", "0.8",
               "--vignette", "--grain", "0.05"]

# GUI controls whose widget writes a LOCAL, not the field. Each entry names the field
# it actually drives, which is the whole value of the list: a control added with a new
# local fails config-coverage until somebody writes down what it sets.
CONFIG_GUI_LOCALS = {
    "fft": "wave_model",
    # The Time of Day slider edits a float mirror because cycle_hour is a
    # double (an accumulator; see config_snapshot.c's CFG_DOUBLE). Named
    # tod_hour rather than hour: this map is keyed on the bare local, so a
    # generic name silently captures the next local that shares it.
    "tod_hour": "cycle_hour",
    # A checkbox over a float: the sky is relative or photometric (spec 13.7).
    "photometric_sky": "radiance_scale",
    "interp": "lut_interp",
    # The camera panel edits the RIG since spec 12.19, and the rig's aim and arm
    # are what produce the pose the snapshot carries as camera.eye/target. So
    # these three land on the pose by the only route there is; there is no
    # separate row for them and carrying one would carry the same fact twice,
    # which is what the deleted camera.orbit block was.
    "dist": "position",
    "yaw": "position",
    "pitch": "position",
    "mode": "meter_mode",
    "msaa": "msaa_samples",
    "msm_size_idx": "msm_size",
    "pending_scale": "render_scale",
    "pending_size": "typical_size",
    "rm": "current_render_mode",
    "so": "spec_occlusion_mode",
    "tm": "tonemap_mode",
}

# Controls a snapshot deliberately cannot carry, with the reason. Not an escape hatch:
# both reasons are about IDENTITY, which is what a snapshot needs and these lack.
CONFIG_GUI_UNCARRIED = {
    "sss_profiles": "profile SLOTS are assigned in material-block order at load, and "
                    "material->subsurface_profile indexes them",
    # A rig's anchor is whatever the app points it at -- a player's position, a
    # scene centre -- and it is rewritten every frame by the app that owns it, so
    # a restored value would not survive to the first draw. Same for the lift
    # above it. The pose they produce IS carried, as camera.eye and camera.target.
    "anchor": "the app writes it every frame; the pose it produces is carried instead",
    "look_lift": "the app writes it every frame; the pose it produces is carried instead",
}


# Values a restore is allowed NOT to reproduce, each with the reason. Two are a
# setter doing its job, which is itself worth pinning -- a perturbation that came
# back unchanged would mean the table had gone round the setter. The third is a
# field nothing can carry.
#
# The list is the whole point of config-perturb: it inverts the default from
# "uncovered unless an arm names it" to "covered unless this names it".
CONFIG_PERTURB_EXCEPTIONS = {
    "engine.msaa_samples": "engine_set_msaa_samples validates; 5 is not a sample count",
    "engine.render_scale": "clamped to [0.5, 1], and forced to 1 headless without jitter",
    "camera.near_clip": "render.c recomputes it every frame from the camera-to-target distance",
    # This fixture runs without --clouds, so flipping the switch asks for a layer
    # whose noise bake -- a one-shot at startup -- never ran. Refused by name, and
    # config-clouds is the arm that reads both halves of that.
    "sky.clouds.enabled": "the session baked no cloud noise, so the row refuses rather than "
                          "storing a flag every consumer ignores",
    # Perturb flips sky.cycle ON and gives cycle_day_seconds a non-zero value,
    # so the restored session runs a clock -- and a running clock OWNS these
    # five, deriving them every frame from the hour it is advancing. That is
    # the feature working, the same ownership the GUI states by disabling its
    # sun sliders while the cycle runs; with the cycle off (every other scene
    # and every other arm here) all five round-trip normally. Noted as one
    # group because they have one cause: spec 11.81's tick.
    "sky.cycle_hour": "the clock advances it every frame while the cycle runs",
    "sky.sun_elevation": "derived from cycle_hour by the tick while the cycle runs",
    "sky.sun_azimuth": "derived from cycle_hour by the tick while the cycle runs",
    "sky.stars_hour": "advanced in lock-step with the sun while the cycle runs",
    "lights[sky_sun].intensity": "sky_apply_sun_to_light rewrites the coupled key light every "
                                 "tick while the cycle runs",
    # The moon's, same cause and one step further: the cycle derives its angles
    # from cycle_hour MINUS the lag, and advances the lag itself at the synodic
    # rate. So all three are the tick's while the clock runs, exactly as the
    # sun's two above are -- and lights[sky_moon].intensity joins them for the
    # sun light's reason, since the sky rewrites its coupled lights every tick.
    #
    # That last entry is here because the moon light is created UNCONDITIONALLY
    # on any sky scene. It was gated on moon_enabled for one commit, and this
    # comment then said no such light existed to perturb -- which was true when
    # written and false the moment the gating came off. The suite caught it as
    # a DROPPED value; the comment could not catch itself.
    "sky.moon_elevation": "derived from cycle_hour and the lag by the tick while the cycle runs",
    "sky.moon_azimuth": "derived from cycle_hour and the lag by the tick while the cycle runs",
    "sky.cycle_moon_offset": "the tick advances the lag at the synodic rate while the cycle runs",
    "lights[sky_moon].intensity": "sky_update_moon rewrites the coupled moon light every "
                                  "frame, cycle or not",
}

# Switches that OWN other rows, as (block, key). The main perturb pass holds each at its value,
# so the rows it owns are checked there, and a second restore flips it alone, which checks the
# switch. Perturbed with the rest, a switch would excuse every row it owns.
CONFIG_PERTURB_HELD = (
    ("camera", "fit_clip"),  # a fitted camera's planes are the engine's every frame (spec 13.57)
)


def _config_perturb(node, top=True):
    """Move every scalar in a snapshot to a different value, in place.

    Type-aware: ints stay ints (a float into an int row truncates and reads as a
    dropped value), and a float already inside [0,1] is moved WITHIN that band so
    a percentage or a blend weight is not pushed out of range and legitimately
    refused. Strings are left alone -- an enum's next label would need the
    vocabulary, and the enums are already covered by config-roundtrip.
    """
    n = 0
    if isinstance(node, dict):
        for k, v in list(node.items()):
            if top and k in ("version", "source"):
                continue
            if k in ("name", "index"):
                continue
            if isinstance(v, bool):
                node[k] = not v
                n += 1
            elif isinstance(v, int):
                node[k] = v + 1
                n += 1
            elif isinstance(v, float):
                node[k] = round(v * 0.5 + 0.25, 6) if 0.0 <= v <= 1.0 else round(v * 1.37 + 0.11, 6)
                n += 1
            elif not isinstance(v, str):
                n += _config_perturb(v, False)
    elif isinstance(node, list):
        for v in node:
            n += _config_perturb(v, False)
    return n


def _config_diff_scalars(asked, got, path="", top=True, out=None):
    """Dotted paths where `got` does not carry the value `asked` holds."""
    if out is None:
        out = []
    if isinstance(asked, dict):
        for k, v in asked.items():
            if top and k in ("version", "source"):
                continue
            if k in ("name", "index") or not isinstance(got, dict) or k not in got:
                continue
            _config_diff_scalars(v, got[k], f"{path}.{k}" if path else k, False, out)
    elif isinstance(asked, list) and isinstance(got, list):
        # Carry the element's identity into the path. Without it every light
        # reports as a bare `lights.intensity`, so one exception excuses the
        # whole array -- a build that stopped restoring intensity entirely
        # would pass. The `name` key is skipped as a VALUE above and used as a
        # LABEL here.
        for i, (a, g) in enumerate(zip(asked, got)):
            label = a.get("name") if isinstance(a, dict) else None
            elem = f"{path}[{label if label else i}]"
            _config_diff_scalars(a, g, elem, False, out)
    elif isinstance(asked, bool):
        if asked != got:
            out.append(path)
    elif isinstance(asked, (int, float)) and isinstance(got, (int, float)):
        if abs(asked - got) > max(1e-4, abs(asked) * 1e-4):
            out.append(path)
    return out


def _config_missing(*paths):
    """True if any capture is absent -- a run that refused rather than rendered.

    Guarded because several arms here deliberately drive the refusal paths, and
    handing a missing file to compare() aborts the whole GROUP on an exception:
    the arm that failed is reported and every arm after it silently never runs.
    """
    return any(not os.path.exists(p) for p in paths)


def _config_px(a, b):
    """compare()'s pixel count, or -1 when either leg refused to render."""
    return -1 if _config_missing(a, b) else compare(a, b)[0]


# Every ImGui call in gui.c is either a VALUE widget whose target must be a table
# row, or it is not. Both lists are explicit, and an ig* call on neither is a
# failure -- which is the whole point: the value list used to be the only one, so
# a control written with an unlisted widget type was silently not counted, and
# the census still printed a plausible number. Five of eight shapes tested were
# invisible that way, including igDragFloat and igInputFloat.
CONFIG_VALUE_WIDGETS = ("Checkbox", "SliderFloat", "SliderInt", "SliderFloat2", "SliderFloat3",
                        "SliderAngle", "ColorEdit3", "ColorEdit4", "DragFloat", "DragFloat2",
                        "DragFloat3", "InputFloat", "InputInt", "Combo_Str_arr")
# Calls that display, lay out, or act -- they carry no settable target.
CONFIG_NONVALUE_WIDGETS = (
    "Begin", "BeginChild", "BeginDisabled", "BeginTable", "BeginTooltip", "Button",
    "CollapsingHeader_TreeNodeFlags", "CollapsingHeader_BoolPtr", "ColorConvertFloat4ToU32",
    "End", "EndChild", "EndDisabled", "EndTable", "EndTooltip", "GetContentRegionAvail",
    "GetCursorScreenPos", "GetIO", "GetStyle", "GetWindowDrawList", "Image", "Indent",
    "IsItemActive", "IsItemDeactivatedAfterEdit", "IsItemHovered", "PopID", "PopItemWidth",
    "PopStyleColor", "PopStyleVar", "ProgressBar", "PushID_Int", "PushID_Str", "PushItemWidth",
    "PushStyleColor_Vec4", "PushStyleVar_Float", "PushStyleVar_Vec2", "RadioButton_Bool",
    "Selectable_Bool", "Separator", "SeparatorText", "SetNextWindowPos", "SetNextWindowSize",
    "SetTooltip", "SameLine", "Spacing", "TableNextColumn", "TableNextRow", "TableSetupColumn",
    "Text", "TextColored", "TextDisabled", "TextUnformatted", "TextWrapped", "TreeNode_Str",
    "TreePop", "Unindent", "ImDrawList_AddRectFilled", "ImDrawList_AddImage",
    "BeginCombo", "EndCombo", "GetDrawData", "GetWindowPos", "GetWindowSize", "Render",
    "SetItemDefaultFocus", "SetNextWindowBgAlpha", "StyleColorsDark", "EndFrame",
)

# Controls whose target is chosen at RUNTIME from a table rather than named in
# the source, so there is no member for the census to match. They are carried --
# by the array the table drives -- and config-perturb is what proves it.
CONFIG_GUI_TABLE_DRIVEN = {
    "v": "the material editor's float/colour buffer: one control per MATERIAL_PARAMS row, "
         "written back through material_param_set, so the materials array carries it",
    "iv": "the same, for that editor's int and enum rows",
}

# gui.c's helpers that wrap one value widget over the member their caller passes as the second
# argument. Their bodies are blanked before the widget scan, and their call sites are counted
# instead -- a helper's own parameter names are not members of anything.
CONFIG_GUI_HELPERS = ("_begin_effect_group", "_length_slider", "_length_drag3")


def _config_gui_source():
    """gui.c with the bodies of CONFIG_GUI_HELPERS' definitions blanked."""
    src = open(os.path.join(ROOT, "cetra", "src", "gui.c")).read()
    for name in CONFIG_GUI_HELPERS:
        m = re.search(r'^static [^\n(]*\b' + name + r'\([^)]*\)\s*\{', src, re.M)
        if not m:
            continue
        depth, end = 1, m.end()
        while depth:
            depth += {"{": 1, "}": -1}.get(src[end], 0)
            end += 1
        src = src[:m.end()] + src[end - 1:]
    return src


def _config_gui_widgets_unclassified():
    """ig* calls in gui.c that neither list mentions. Any is a coverage hole."""
    src = open(os.path.join(ROOT, "cetra", "src", "gui.c")).read()
    called = {m.group(1) for m in re.finditer(r'\big([A-Z]\w*)\s*\(', src)}
    return sorted(called - set(CONFIG_VALUE_WIDGETS) - set(CONFIG_NONVALUE_WIDGETS))


def _config_gui_members():
    """Every struct member a gui.c control writes, by its trailing name."""
    src = _config_gui_source()
    calls = "|".join(["ig(?:" + "|".join(CONFIG_VALUE_WIDGETS) + ")", *CONFIG_GUI_HELPERS])
    controls = (r'\b(?:' + calls + r')'
                r'\s*\(\s*(?:"[^"]*"|[\w\->\.]+)\s*,\s*&?([A-Za-z_][A-Za-z0-9_\->\.\[\]]*)')
    found = [m.group(1) for m in re.finditer(controls, src)]
    return {re.split(r'->|\.', re.sub(r'\[[^\]]*\]', '', e))[-1] for e in found}


def _config_table_members():
    """Every struct member the descriptor table addresses."""
    src = open(os.path.join(ROOT, "cetra", "src", "config_snapshot.c")).read()
    # The member's argument POSITION differs per macro, so the macro name selects
    # it. These must track the macro definitions; when they last drifted, the
    # anti-vacuity floor below caught it rather than letting the census
    # silently shrink to 19 rows and report 150 controls as uncarried.
    index = {"CFG_ROW": 4, "CFG_ROW_ENUM": 3, "CFG_ROW_FN": 4}
    members = set()
    for m in re.finditer(r'\b(CFG_ROW|CFG_ROW_ENUM|CFG_ROW_FN)\(([^()]*)\)', src, re.S):
        args = [a.strip() for a in m.group(2).replace("\n", " ").split(",")]
        i = index[m.group(1)]
        if len(args) > i:
            members.add(args[i].split(".")[-1])
    return members


def run_config_gate(workdir):
    """The live session dumped to JSON and restored from it (spec 11.71).

      config-coverage   every gui.c control is a row in the descriptor table, or is
                        named in one of two lists here with what it drives or why it
                        cannot be carried. Static: reads both sources, renders nothing
      config-roundtrip  restore a snapshot and dump again: byte-identical to the first
      config-pixels     a run under a non-default look, against its own snapshot
                        restored with no other flag. Floor measured first
      config-standalone the same restore with NO -m and no -W/-H: the source block
                        carries the model and the framing
      config-override   the snapshot beats the .cscn AND the command line, which is
                        what makes it a reproduction rather than another opinion
      config-perturb    move EVERY carried value, restore, and check each one came
                        back. The only arm that can see a row whose apply silently
                        does nothing, which config-roundtrip structurally cannot
      config-sun        a moved sun, which is the deferred re-bake and everything
                        downstream of it, read against --sun-elevation
      config-clouds     a cloudy session restores cloudy, and a file whose source
                        cannot arm the noise bake is refused by name rather than
                        storing a flag every consumer ignores
      config-camera     a pose the scene file does not already hold, read against
                        --cam-eye at the same place
      config-order      it lands AFTER the scene-radius derivation, read as values
                        rather than pixels because that block moves none on a small
                        fixture with AO and SSR off
      config-schema     an unknown key, a wrong-shaped value, a light this scene does
                        not have and a whole absent subsystem are each named

    config-pixels is the arm that matters and it is NOT safe alone: "restored frame
    equals original" is satisfied perfectly by a restore that does nothing, since both
    legs would then render the fixture's own defaults. Its control leg -- the same
    fixture with no snapshot -- is what forces the look to have arrived, and it is
    checked against a floor rather than assumed.

    config-coverage is the arm that keeps the feature true over time. Everything else
    here tests the values that ARE carried; only this one can fail because of a value
    that is not. A control added to gui.c with no table row is invisible to every other
    arm in the suite -- the snapshot still round-trips, still reproduces its own frame,
    and silently omits the thing somebody just added.

    config-order exists BECAUSE the falsification round found the hole: moving the
    apply above the scene-radius block destroys a restored fog range and silently
    raises anything under a derived floor, and every other arm here passed on that
    build. Anything read only as pixels is blind to a field the frame does not use.

    TWO KNOWN GAPS, said out loud rather than left to look like coverage. (There
    were three: the apply used to rebuild the view matrix itself after the pose
    rows landed, which no arm could see because the frame loop rebuilt it again
    before rendering. Spec 11.107 made the frame loop the only builder and the
    deferred call went.)

    Nothing here can see the auto-orbit kill after a restore: render.c already
    passes `!args.headless` when it arms auto-orbit, so it is off in every
    headless run and those lines are unreachable from this group.

    And config-coverage matches a gui.c target by its TRAILING member name, so a
    control writing `sb->enabled` would be satisfied by Water.enabled -- reported
    carried while nothing carries it. The spring-bone sliders were the live
    instance until spec 12.1 moved them out of gui.c (the engine panel reaches no
    animator now, so the app draws them), which took the example away without
    closing the hole: the next control to share a trailing member name with a
    carried one inherits it. Closing it wants (struct, member) keying on both
    sides.
    """
    failures = []

    # -- coverage: static, and the only arm that can see an OMISSION --------------
    gui = _config_gui_members()
    table = _config_table_members()
    known = set(CONFIG_GUI_LOCALS) | set(CONFIG_GUI_UNCARRIED) | set(CONFIG_GUI_TABLE_DRIVEN)
    missing = sorted(gui - table - known)
    # Anti-vacuity: an empty or tiny extraction would pass by finding nothing.
    parsed_enough = len(gui) >= 150 and len(table) >= 200
    # A local named here must still reach a real row, or the note is fiction.
    locals_land = sorted(f for f in CONFIG_GUI_LOCALS.values() if f not in table)
    # An ig* call on neither widget list means the census silently skipped it.
    unclassified = _config_gui_widgets_unclassified()
    # Both exception lists must stay honest: an entry whose control is gone is
    # stale, and one naming something the table DOES carry is simply false.
    stale = sorted(known - gui)
    redundant = sorted(set(CONFIG_GUI_UNCARRIED) & table)
    ok = (not missing and parsed_enough and not locals_land and not unclassified and not stale
          and not redundant)
    detail = f"{len(gui)} controls, {len(table)} rows"
    if missing:
        detail += f", NOT CARRIED: {', '.join(missing)}"
    if locals_land:
        detail += f", local maps to nothing: {', '.join(locals_land)}"
    if unclassified:
        detail += f", UNCLASSIFIED widget: {', '.join(unclassified)}"
    if stale:
        detail += f", exception for a control that is gone: {', '.join(stale)}"
    if redundant:
        detail += f", excused but carried: {', '.join(redundant)}"
    print(f"  config-coverage {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("config-coverage")

    if not os.path.exists(asset(CONFIG_FIXTURE)):
        print(f"  config-roundtrip SKIP  {CONFIG_FIXTURE} not found")
        return failures

    # -- the floor, before anything is compared to anything ----------------------
    pinned = ["--no-auto-exposure", "-E", "1.0"]
    floor_a, _, _ = _config_run(workdir, "floor_a", pinned)
    floor_b, _, _ = _config_run(workdir, "floor_b", pinned)
    floor_px, _ = compare(floor_a, floor_b)

    # -- the tuned original, and the two restores off it -------------------------
    orig_ppm, orig_json, _ = _config_run(workdir, "orig", pinned + CONFIG_LOOK)
    rest_ppm, rest_json, rest_out = _config_run(workdir, "restore", ["--config", orig_json],
                                                model=None)

    same_json = (os.path.exists(orig_json) and os.path.exists(rest_json)
                 and open(orig_json).read() == open(rest_json).read())
    applied = "config snapshot applied" in rest_out
    ok = same_json and applied
    print(f"  config-roundtrip {'PASS' if ok else 'FAIL'}  dump == redump after restore="
          f"{same_json}, applied={applied}")
    if not ok:
        failures.append("config-roundtrip")

    # config-standalone IS the restore above -- no -m, no -W/-H -- so the pixel arm
    # and this one read the same run and cannot disagree about what it was.
    # The control leg -- the same fixture with no snapshot -- is what floor_a
    # already is, byte for byte: same fixture, same size, same pinned exposure.
    # Reusing it rather than rendering a third identical frame saves a process
    # launch, and on this fixture a launch is ~0.7 s release because it captures
    # two probes at load.
    px = _config_px(orig_ppm, rest_ppm)
    ctrl_px = _config_px(orig_ppm, floor_a)
    ok = px >= 0 and ctrl_px >= 0 and px <= floor_px and ctrl_px > 1000
    print(f"  config-pixels   {'PASS' if ok else 'FAIL'}  "
          + (f"restored {px} px (floor {floor_px}), no-config control {ctrl_px} px" if px >= 0
             else "the restored run produced no frame"))
    if not ok:
        failures.append("config-pixels")

    # The restore above already ran with no -m and no -W/-H, so its frame matching is
    # the positive half. The negative half is the point: blank the model out of the
    # source block and the run must REFUSE, or "it found the model" is unfalsifiable
    # -- a build that ignored the block entirely would look identical here.
    blank_json = _config_variant(orig_json, os.path.join(workdir, "config_nomodel.json"),
                                 lambda d: d["source"].pop("model", None))
    _, _, blank_out = _config_run(workdir, "nomodel", ["--config", blank_json], model=None)
    refused = "names no model and none was given" in blank_out
    ok = px >= 0 and px <= floor_px and refused
    print(f"  config-standalone {'PASS' if ok else 'FAIL'}  rendered from the snapshot alone="
          f"{os.path.exists(rest_ppm)}, refused with its model blanked={refused}")
    if not ok:
        failures.append("config-standalone")

    # -- override: the snapshot lands after the .cscn AND after the flags ---------
    # A snapshot saying the opposite of the command line, applied WITH that command
    # line. If it wins, the frame is the one the snapshot alone produces; if the flags
    # win, it is the flag-only frame. Asserting BOTH is what makes this precise --
    # "something moved" would also pass on a build that restored garbage.
    def _override(d):
        d["postfx"]["ssr"]["enabled"] = True
        d["postfx"]["tonemap"] = "neutral"

    over_json = _config_variant(orig_json, os.path.join(workdir, "config_override.json"),
                                _override)
    with_flags, _, _ = _config_run(workdir, "override_flags", pinned + CONFIG_LOOK +
                                   ["--config", over_json])
    alone, _, _ = _config_run(workdir, "override_alone", ["--config", over_json], model=None)
    same_as_alone = _config_px(with_flags, alone)
    moved_off_flags = _config_px(orig_ppm, with_flags)
    ok = same_as_alone >= 0 and same_as_alone <= floor_px and moved_off_flags > 1000
    print(f"  config-override {'PASS' if ok else 'FAIL'}  "
          + (f"with flags == snapshot alone: {same_as_alone} px, and {moved_off_flags} px off "
             f"the flag-only frame" if same_as_alone >= 0 else "a leg produced no frame"))
    if not ok:
        failures.append("config-override")

    # -- perturb: does every carried value actually SURVIVE a restore? ----------
    # config-roundtrip cannot answer this. Both of its legs render the same
    # fixture from the same defaults, so a value the apply DROPS is identical in
    # both dumps and the byte comparison passes -- only a CORRUPTING apply is
    # caught. That left ~210 rows plus both element arrays uncovered for the
    # commonest defect there is: a row whose apply silently does nothing.
    # Measured when this arm was written: 4 rows were dead, at 0 px, silently.
    perturbed_json = os.path.join(workdir, "config_perturb_in.json")
    held_json = os.path.join(workdir, "config_perturb_held.json")
    with open(orig_json) as f:
        orig = json.load(f)
    d, flipped = json.loads(json.dumps(orig)), json.loads(json.dumps(orig))
    moved = _config_perturb(d)
    for block, key in CONFIG_PERTURB_HELD:
        d[block][key] = orig[block][key]
        flipped[block][key] = not orig[block][key]
    for path, doc in ((perturbed_json, d), (held_json, flipped)):
        with open(path, "w") as f:
            json.dump(doc, f, indent=1)
    held = {f"{block}.{key}" for block, key in CONFIG_PERTURB_HELD}
    lost = []
    for tag, asked, path in (("perturb", d, perturbed_json), ("perturb_held", flipped, held_json)):
        _, back, _ = _config_run(workdir, tag, ["--config", path], model=None)
        if not os.path.exists(back):
            break
        with open(back) as f:
            diff = _config_diff_scalars(asked, json.load(f))
        # The second restore asks only whether each held switch carries.
        lost += diff if asked is d else [p for p in diff if p in held]
    unexpected = sorted(set(lost) - set(CONFIG_PERTURB_EXCEPTIONS))
    # An exception that starts surviving is also a finding: the note is then a
    # lie, and the list is the only thing standing between this arm and vacuity.
    stale = sorted(set(CONFIG_PERTURB_EXCEPTIONS) - set(lost))
    ok = os.path.exists(back) and moved > 300 and not unexpected and not stale
    detail = f"{moved} values moved, {len(lost)} not reproduced"
    if unexpected:
        detail += f"; DROPPED: {', '.join(unexpected)}"
    if stale:
        detail += f"; exception no longer needed: {', '.join(stale)}"
    print(f"  config-perturb  {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("config-perturb")

    # -- sun: the deferred re-bake, which is a value AND everything downstream ---
    # Storing the two angles is not restoring the sun: the transmittance and
    # sky-view LUTs, the environment cube, the IBL and the coupled key light are
    # all derived from it. Dropping the re-bake leaves a snapshot that reads back
    # correctly and renders the OLD sun -- measured at the whole frame here.
    # Read against --sun-elevation/--sun-azimuth, the answer it has to reproduce.
    #
    # NOT on cornell_rooms, and the reason is a real limit rather than a fixture
    # preference: a probe SET cannot be re-captured (its capture cubes are released
    # into the atlas), so 11.70 defers relight, and a restored sun there leaves two
    # probes lit by the sun that was up when they were photographed. Measured: the
    # snapshot leg lands 0.144 RMSE from the flag leg on that fixture, where the
    # original is 0.159 -- it moves, but not TO the answer. layer_fixture carries no
    # probe, so the claim there is exact and the arm can demand equality.
    sun_scene = "layer_fixture.cscn"
    sun_base = pinned + ["--sky"]
    sun_orig, sun_orig_json, _ = _config_run(workdir, "sun_orig", sun_base, model=sun_scene)
    if _config_missing(sun_orig_json):
        print(f"  config-sun      SKIP  {sun_scene} not found")
    else:
        def _move_sun(d):
            d["sky"]["sun_elevation"] = 55.0
            d["sky"]["sun_azimuth"] = 120.0

        sun_json = _config_variant(sun_orig_json, os.path.join(workdir, "config_sun_in.json"),
                                   _move_sun)
        sun_cfg, _, _ = _config_run(workdir, "sun_cfg", ["--config", sun_json], model=None)
        sun_flag, _, _ = _config_run(workdir, "sun_flag", sun_base +
                                     ["--sun-elevation", "55", "--sun-azimuth", "120"],
                                     model=sun_scene)
        agree = _config_px(sun_cfg, sun_flag)
        moved = _config_px(sun_cfg, sun_orig)
        ok = agree == 0 and moved > 1000
        print(f"  config-sun      {'PASS' if ok else 'FAIL'}  "
              + (f"snapshot sun == --sun-elevation: {agree} px, and {moved} px off the "
                 f"original sun" if agree >= 0 else "a leg produced no frame"))
        if not ok:
            failures.append("config-sun")

    # -- clouds: the deck survives a restore, and says so when it cannot --------
    # Both halves matter and neither is safe alone. The positive half alone
    # passes on a build that always arms clouds; the negative half alone passes
    # on one that never does.
    #
    # Coverage is pinned at 0.10 rather than left at the 0.45 default for the
    # reason the cloud-shadow arms pin it: at the default the deck is a flat
    # dimming with no pattern in it, so a frame comparison reads a scale factor
    # rather than a sky.
    cloud_base = pinned + ["--sky", "--clouds", "--cloud-coverage", "0.10"]
    cloud_orig, cloud_json, _ = _config_run(workdir, "cloud_orig", cloud_base, model=sun_scene)
    if _config_missing(cloud_json):
        print(f"  config-clouds   SKIP  {sun_scene} not found")
    else:
        cloud_cfg, _, _ = _config_run(workdir, "cloud_cfg", ["--config", cloud_json], model=None)
        # No --clouds on this leg: the snapshot's source block must ask for it.
        clear, _, _ = _config_run(workdir, "cloud_clear", pinned + ["--sky"], model=sun_scene)
        agree = _config_px(cloud_cfg, cloud_orig)
        # Anti-vacuity: a cloudy sky must actually differ from a clear one, or
        # "the restore matches" is true of a build with no clouds at all.
        deck_visible = _config_px(cloud_orig, clear)

        # The negative half: the pre-11.72 file shape, whose source block never
        # carried `clouds`. The layer cannot be armed after startup, so the row
        # must refuse BY NAME rather than store a flag every consumer ignores --
        # which is what silently rendered a clear sky while the dump wrote
        # `enabled: true` back out and config-roundtrip passed on it.
        noarm = _config_variant(cloud_json, os.path.join(workdir, "config_cloud_noarm.json"),
                                lambda d: d["source"].pop("clouds", None))
        _, _, noarm_out = _config_run(workdir, "cloud_noarm", ["--config", noarm], model=None)
        refused = "baked no cloud noise; sky.clouds.enabled ignored" in noarm_out

        ok = agree == 0 and deck_visible > 1000 and refused
        print(f"  config-clouds   {'PASS' if ok else 'FAIL'}  "
              + (f"restored == cloudy original: {agree} px, deck worth {deck_visible} px, "
                 f"unarmed restore refused by name={refused}"
                 if agree >= 0 else "a leg produced no frame"))
        if not ok:
            failures.append("config-clouds")

    # -- camera: a pose the scene file does NOT already hold ---------------------
    # Every other arm restores the fixture's own camera, so the pose path is inert
    # in all of them -- deleting the spherical re-seed and the lookat rebuild moved
    # nothing. The realistic case is the only one that tests it: you moved the
    # camera, then dumped. Read against --cam-eye/--cam-target at the same place,
    # which is the answer the snapshot has to reproduce.
    with open(orig_json) as f:
        base_cam = json.load(f)["camera"]
    eye = [base_cam["eye"][0] + 1.3, base_cam["eye"][1] + 0.6, base_cam["eye"][2] - 1.1]
    target = base_cam["target"]
    cam_json = _config_variant(orig_json, os.path.join(workdir, "config_camera_in.json"),
                               lambda d: d["camera"].update(eye=eye))
    cam_cfg, _, _ = _config_run(workdir, "camera_cfg", ["--config", cam_json], model=None)
    # %.9g and not %.6f, which is the width the snapshot itself carries: the two
    # legs must be handed the SAME pose or this measures a truncation rather than
    # a restore. Six decimals was enough while a Camera stored a pose verbatim;
    # since spec 12.19 the pose round-trips through the rig's asin/atan2, and a
    # 1e-6 difference in the input comes back as 17 pixels.
    cam_flag, _, _ = _config_run(workdir, "camera_flag", pinned + CONFIG_LOOK + [
        "--cam-eye", ",".join(f"{v:.9g}" for v in eye),
        "--cam-target", ",".join(f"{v:.9g}" for v in target)])
    agree = _config_px(cam_cfg, cam_flag)
    moved = _config_px(cam_cfg, orig_ppm)
    ok = agree >= 0 and agree <= floor_px and moved > 1000
    print(f"  config-camera   {'PASS' if ok else 'FAIL'}  "
          + (f"snapshot pose == --cam-eye: {agree} px, and {moved} px off the original pose"
             if agree >= 0 else "a leg produced no frame"))
    if not ok:
        failures.append("config-camera")

    # -- order: the apply must land AFTER the scene-radius derivation ------------
    # Read as VALUES, not pixels, and that is the point: the five fields this block
    # touches are a fog range and three screen-space reaches, none of which moves a
    # pixel on a small fixture with AO and SSR switched off. The falsification round
    # moved the apply above this block and all six other arms passed.
    #
    # Both mechanisms, because they fail differently: fog.far is an unconditional
    # ASSIGNMENT, so an early apply loses it outright; ao.radius is an fmaxf FLOOR,
    # so an early apply only loses values below the floor.
    # The INPUT is named apart from the dump on purpose: _config_run derives its
    # dump path from the arm name, so an input called config_order.json would be
    # the file the restore writes back over -- and the arm would be reading its own
    # input and could never fail. It shipped that way for one falsification round.
    with open(orig_json) as f:
        derived_far = json.load(f)["postfx"]["fog"]["far"]

    def _out_of_band(d):
        d["postfx"]["fog"]["far"] = 1234.5
        d["postfx"]["ao"]["radius"] = 0.001  # under scene_radius * 0.01 on this fixture

    order_json = _config_variant(orig_json, os.path.join(workdir, "config_order_in.json"),
                                 _out_of_band)
    _, back_json, _ = _config_run(workdir, "order", ["--config", order_json], model=None)
    back = {}
    if os.path.exists(back_json):
        with open(back_json) as f:
            back = json.load(f)
    kept_far = abs(back.get("postfx", {}).get("fog", {}).get("far", 0.0) - 1234.5) < 0.01
    kept_radius = abs(back.get("postfx", {}).get("ao", {}).get("radius", 0.0) - 0.001) < 1e-6
    # Anti-vacuity: the authored values must actually differ from what the block
    # derives, or "they survived" is true of a build that never applied them.
    distinct = abs(derived_far - 1234.5) > 1.0
    ok = kept_far and kept_radius and distinct
    print(f"  config-order    {'PASS' if ok else 'FAIL'}  authored fog.far survived={kept_far}, "
          f"ao.radius under the floor survived={kept_radius} (derived far {derived_far:.2f})")
    if not ok:
        failures.append("config-order")

    # -- schema: every way a file can be wrong, each named ------------------------
    # One run covers all of them, so the only reason to carry fewer would be that
    # the rest were untested -- which is what they were: the whole per-array key
    # sweep, its MATERIAL_PARAMS branch, the vector-length check and the version
    # warning had no coverage at all.
    def _wrongnesses(d):
        d["postfx"]["nonsense_key"] = 3                     # unknown key in a real section
        d["postfx"]["ssr"]["strength"] = "not a number"     # scalar of the wrong shape
        d["postfx"]["tonemap"] = "chartreuse"               # unknown enum label
        d["postfx"]["grade"]["lift"] = [0.1, 0.2]           # vector of the wrong LENGTH
        d["water"] = {"level": 2.0}                         # a subsystem this scene lacks
        d["nonsense_section"] = {"x": 1}                    # unknown top-level section
        d["lights"].append({"name": "ghost_light", "intensity": 1.0})
        d["probes"].append({"index": 99, "intensity": 1.0})
        d["materials"].append({"name": "ghost_material", "roughness": 0.5})
        d["probes"][0]["bogus_probe_key"] = 1
        d["lights"][0]["bogus_light_key"] = 1
        d["materials"][0]["bogus_mat_key"] = 1
        d["version"] = 7

    bad_json = _config_variant(orig_json, os.path.join(workdir, "config_bad.json"), _wrongnesses)
    _, _, bad_out = _config_run(workdir, "bad", ["--config", bad_json], model=None)
    named = {
        "unknown key": "'postfx.nonsense_key' is not a known setting" in bad_out,
        "wrong shape": "postfx.ssr.strength is not a value of the right shape" in bad_out,
        "unknown enum": "postfx.tonemap is not a known value" in bad_out,
        "wrong vector length": "postfx.grade.lift is not a value of the right shape" in bad_out,
        "absent subsystem": "this scene has no 'water'" in bad_out,
        "unknown section": "'nonsense_section' is not a known section" in bad_out,
        "absent light": "no light 'ghost_light'" in bad_out,
        "absent probe": "no probe 99" in bad_out,
        "absent material": "no material 'ghost_material'" in bad_out,
        "unknown probe key": "'probes.bogus_probe_key' is not a known setting" in bad_out,
        "unknown light key": "'lights.bogus_light_key' is not a known setting" in bad_out,
        "unknown material key": "'materials.bogus_mat_key' is not a known setting" in bad_out,
        "version": "version 7; expected 1" in bad_out,
    }
    # And it must still have applied the rest rather than refusing the file --
    # asserted as a COUNT, so "applied" cannot pass while a refusal silently took
    # other values with it. THREE values are refused here: the wrong-shaped
    # scalar, the wrong-length vector, and the unknown enum label, which is a
    # refused value as much as the other two. Everything else must still land.
    REFUSED = 3
    applied = re.search(r"config snapshot applied: \S+ \((\d+) fields\)", bad_out)
    clean = re.search(r"config snapshot applied: \S+ \((\d+) fields\)", rest_out)
    count_ok = bool(applied and clean) and int(applied.group(1)) == int(clean.group(1)) - REFUSED
    ok = all(named.values()) and count_ok
    print(f"  config-schema   {'PASS' if ok else 'FAIL'}  "
          f"{sum(named.values())}/{len(named)} named, applied "
          f"{applied.group(1) if applied else '?'} of {clean.group(1) if clean else '?'} "
          f"(want {REFUSED} refused)"
          + ("" if all(named.values()) else
             f", silent: {', '.join(k for k, v in named.items() if not v)}"))
    if not ok:
        failures.append("config-schema")

    return failures
