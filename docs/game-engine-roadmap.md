# Cetra as a Game Engine — Roadmap: What We Have and What We Need

What stands between Cetra and a shippable game, as opposed to a rendering library. Companion to
`aaa-rendering-roadmap.md`: that doc owns the graphics pipeline; this one owns everything else
(physics, gameplay, assets, core, and the gaps between "engine" and "shippable game").

---

## Verdict

The specialised tech is done: the renderer is high-end and the physics is Jolt. What remains is
not hard in the way those were; it is wide.

**Part 2 is not a census.** Its rows are the gaps somebody thought to name, and a row marked done
means the seam exists and is verified, not that the feature is deep enough to ship a game on. Three
kinds of gap sit outside the vertical slice of Part 3:

- **Authoring.** There is no editor of any kind. A scene is hand-written `.cscn` JSON or built in C.
- **Shipping.** No packaging, bundle, signing, installer or crash reporting. `./build.sh` makes
  `out/bin/<app>` and nothing a player could be handed, which gates Steamworks rather than
  following it.
- **Depth behind a shipped seam.** The UI has no key-capture element, input has no rebinding, the
  font atlas cannot page past one sheet, and audio has no reverb or low-pass occlusion.

The graphics API (OpenGL 4.1, no Vulkan, Metal, DirectX or compute) is not a blocker for Steam: GL
4.1 runs on Windows, Linux and macOS and Steam imposes no backend. The real risks are scope,
finishing an engine and building a game, and perf-budgeting the heavy render stack for mid-range
hardware.

---

## Part 1 — What we have today

The engine as it is lives in `docs/`:

- **Rendering**: `docs/architecture.md`, `docs/shaders.md`, `docs/shader-subsystems.md`,
  `docs/lighting.md`; the plan for it is `aaa-rendering-roadmap.md`.
- **The game layer** (fixed-step loop, Jolt physics, entities, characters, brains, navigation,
  input and the action table, audio and zones, settings, saves, particles):
  `docs/game-framework.md`.
- **Animation** (skinning, blending, retargeting, root motion and stride matching, the state
  graph, IK and foot locking, look-at, ragdolls): `docs/animation.md`.
- **Cameras** (the rig, steering the controls, the walker's gait): `docs/camera.md`.
- **UI and text**: `docs/ui.md`; the 2D canvas: `docs/canvas.md`; procedural terrain:
  `docs/terrain.md`.
- **Build and dependencies**: `docs/building.md`, `README.md`.
- **Apps**: `apps/<app>/README.md`. `gametest` is the closest thing to a vertical slice: the whole
  game layer in one app.

---

## Part 2 — What we need (definitive gaps)

Effort estimates are rough and assume a single experienced dev.

| System | Status | Why it matters | Rough effort |
|---|---|---|---|
| **Audio** | Done, spec 12.0 (zones 13.33, voice pool 13.17). `docs/game-framework.md`, Audio. | | done |
| **Gamepad input** | Done, spec 11.109. `docs/game-framework.md`, Input. | | done |
| **Save / serialization** | Done, spec 12.3. `docs/game-framework.md`, Saves. Still open: a drift tolerance measured over many save-step-reload-step cycles, and preserving unknown fields so a save survives Steam Cloud syncing between patch levels. | A save written by one patch read by another. | unestimated |
| **Game UI / menus** | Done, spec 12.2, widened by the 14.x series. `docs/ui.md`. | | done |
| **Animation blending** | Done, spec 12.1. `docs/animation.md`, What plays. | | done |
| **Steamworks integration** | Absent. Steam presents a virtual Xbox pad to a GLFW game, which shipped titles rely on; Steam Input's own API would be a second reader behind the gamepad seam (`GamepadReadFn`). | Achievements, cloud saves, overlay, input API. Needed near ship, after packaging. | ~1 week |
| **IK** | Planting (12.4, 12.5), locking (12.9) and look-at (13.17) done. `docs/animation.md`. Still open: hand and aim IK. | Reaching, aiming, holding. Quality-of-life, not blocking. | unestimated |
| **Ragdoll, vehicles, soft body / cloth** | Ragdoll done, spec 12.16 (`docs/animation.md`, Ragdolls); getting up is not built, and the ragdoll's `SwingTwistConstraintSettings` is the one type `DriveToPoseUsingMotors` accepts, so it stays possible. Vehicles and soft body are vendored and compiled in Jolt but unbound by JoltC: every soft-body creation entry point is commented out in `JoltC/Functions.h`, and vehicle exists there only as an enum tag. Both are binding jobs through a one-TU C++ file, as the ragdoll was (`docs/game-framework.md`, Physics). Cost the work as the ragdoll's went: the binding was a day, and making shapes derived from a rig survive arbitrary content took three times that. | Getting up after a fall, anything driveable, rope, banners and capes. Spring bones cover hair and straps and do not collide. | ~2-4 days each to bind, plus the content work |
| **Editor / scene authoring** | Absent. No placement UI, gizmo-driven authoring, asset browser, play-in-editor, undo or prefabs. The ImGui panels and the config snapshot (spec 11.71) tune what exists; the snapshot cannot create a light, bind a texture or place a probe. | Usually the largest remaining line item. It decides whether anyone but the author can build a level, and turns iteration from a rebuild into a drag. | months; the largest item here |
| **Packaging and distribution** | Absent. No CPack, no install rule (the comment at the end of `cetra/CMakeLists.txt` says why the old one was removed), no `.app` bundle or `Info.plist`, no signing or notarization, no installer, no crash reporting, no telemetry, no update path. | Nothing reaches a player without it. It gates Steamworks: achievements on a build nobody can install is the wrong order. | ~1-2 weeks |
| **Display and window modes** | Done, spec 12.15. `docs/architecture.md` and `docs/game-framework.md`, Settings. Still owed: a check on a second display, since with one the name lookup cannot be told from one that always answers the primary. | | done |
| **Text encoding and localization** | Partly done. Text is UTF-8 throughout (`text_utf8_next` and its kin, spec 14.3), and the atlas holds printable ASCII, Latin-1, Greek and a short list of symbols, built once at load (`docs/ui.md`, Fonts). Still absent: an atlas that can page (a CJK face does not fit one sheet), shaping for right-to-left scripts and combining marks, and a string table. | Any market outside Western Europe. | ~1-2 weeks for paging; shaping and the string table are more |
| **UI element vocabulary** | The closed list is nine: panel, label, button, toggle, slider, selector, scrolling pane (with virtual rows), text field and code editor (`ui.h`, specs 12.2, 14.2, 14.4); screens may be modal. Absent: a key-capture element, drag-and-drop, tabs, tooltip, grid. The escape hatches let an app draw any of them by hand, which is the design, but an inventory screen or a key-rebinding row is app code rather than an element, and every game rewrites it. | Real games are mostly these screens. | ~1 week per element family |
| **Input rebinding** | Absent. `input_bind` takes a borrowed const table, and `settings.h` records that bindings are not persisted because that wants the remapping UI they would exist for. That UI needs a key-capture element (above), so these two rows are one gap from two sides. A focusable panel with a key hook (spec 14.10) receives every key while it holds the focus, which is where such an element would start. | An accessibility requirement on most platforms, and expected by any player on a non-QWERTY layout. | ~3-4 days, after a capture element |
| **Camera framework** | Done, spec 12.19. `docs/camera.md`. Still absent: a sequencer or cutscene track, and a dialogue system to drive one. A rail is an anchor a caller drives; nothing scripts one over time. | The scripted camera moment is possible; a sequencer is what would author it. | a sequencer is its own project |
| **Animation state machine** | Done, spec 12.20. `docs/animation.md`, The state graph. Still absent: any authoring surface (a graph is a C table in an app: no file format, editor or hot reload), sub-state machines, 2D blend spaces, additive layers, and the override layer, which stays the app's one call. | Locomotion, combat and reactions all want it. | authoring surface open |
| **Root motion** | Done, spec 12.18. `docs/animation.md`, Root motion. Still open: a retargeted channel takes its position from the bind pose, so an imported character cannot carry root motion until the retarget carries translation with a proportion transfer. | Lunges, dodges and turns an animator choreographed, on imported rigs. | unestimated |
| **Audio depth** | The seam covers placed sound heard through zones of walls as a gain (13.33), bus fades (13.53) and per-voice pitch (13.47, 13.56). Absent: reverb, low-pass occlusion and obstruction (a zone turns sound down; it does not muffle it), a DSP or filter graph, interactive or stem-based music, voice and dialogue playback, automatic ducking. miniaudio carries some of this, so parts are configuration rather than construction. | A room that sounds like a room, and music that responds to play. | genre-dependent |
| **Accessibility** | Mostly absent. `GameSettings.reduce_motion` reaches the camera (exactly the no-shake path, spec 12.19) but not the post stack's motion blur. No subtitle or caption system, no colourblind palettes, no UI scaling beyond each element's font size, no remapping (above), no hold-to-toggle. | Increasingly a platform requirement, and cheapest designed in. | ~1 week for the basics, once rebinding and a caption element exist |
| **VFX authoring, scripting, navmesh/AI, networking** | AI started (spec 13.17): a place graph with A* (`nav_graph.h`), a utility-AI `BRAIN` component (`game/brain.h`) and a head look-at, in `docs/game-framework.md` and `docs/animation.md`. Absent: a floor navmesh (an agent goes only where someone put a place), scripting, networking, and particle authoring (emitters are built in code; a `.cscn` authors only its dust). | Genre-dependent, except scripting, which buys iteration speed in any genre: today every gameplay change is a C rebuild. An authoring or preset layer for particles would come before heavy VFX work. | genre-dependent |

Design ceilings and known limitations, not gaps:

- The entity layer is a linear scan: fine for hundreds to low thousands of entities, not tens of
  thousands of agents.
- Raycast hit normals are zeroed (`physics.c`); only the shape sweep returns one.
- A model's geometry, skeleton and clips parse synchronously; only texture decode streams.
- The render stack is heavy: SSGI, SSR, GTAO and volumetrics on GL 4.1 with no compute need
  perf budgeting for mid-range GPUs.
- Headless runs exactly one fixed step per rendered frame, so a defect that appears only at
  another ratio is beyond every gate arm (`docs/verification.md`).

---

## Part 3 — Suggested path to a playable vertical slice

Ordered smallest-effort-to-playable first.

1. **Gamepad input** — done, spec 11.109.
2. **Audio** — done, spec 12.0.
3. **Animation blending** — done, spec 12.1.
4. **Game UI layer** — done, spec 12.2.
5. **Save serialization** — done, spec 12.3. A restored world is deliberately not bit-exact:
   Jolt's solver state stays out of the file, or the format would pin to `JPH_VERSION_ID` and
   break every save on a physics upgrade.
6. **Foot locking** — done, spec 12.9, with stride matching (12.10), root motion (12.18) and the
   work of pointing the stack at a real imported character (12.11 to 12.14). The extension
   soft-clamp is refused: on a bind pose that is exactly straight, 1 per cent of soft band costs
   16.2 degrees of permanent bend (`docs/foot-locking.md`).
7. **Steamworks** (~1 week, near ship) — achievements, cloud, overlay. Do not start here: packaging
   gates it, since there is nothing yet for an achievement to attach to.

**Past the slice, the order depends on a choice this document cannot make**: a game shipped on
this engine, or an engine others build in.

- **A game shipped on it**: packaging first, since a build nobody can install is the hard blocker;
  then the UI elements the game's own screens need, and atlas paging if it ships outside Western
  Europe.
- **An engine others build in**: the editor, scripting and an asset pipeline, a far larger
  programme that changes what is worth doing to the runtime underneath.

Either way the **platform debt is unconditional**: the done rows still owe their Linux and Windows
paths (an audio listen, the settings and save paths) and one run with a real controller, through
the game and through a menu. None of it is new work, only work this machine cannot do;
`docs/verification.md` has the recipes.

Fill in genre-specific systems (navmesh, networking, VFX authoring) only as the game's design
demands them. Scripting is listed with them and probably should not be: it buys iteration speed
regardless of genre.

---

## Bottom line

Cetra has the two hardest engine pieces, a high-end renderer and Jolt physics with a working
character controller and game loop. Shipping a game on it is realistic, and the graphics API is not
what decides it. What is left divides into three:

1. **The vertical-slice glue**: Part 3. One item left, Steamworks, blocked on (2).
2. **Shipping**: packaging, signing, an installer, crash reporting. Weeks, and nothing reaches a
   player before it.
3. **Authoring**: an editor, and a scripting layer beside it. Months, and it decides whether anyone
   but the author can make something here.

Before costing a gap, check what a dependency already does: vehicles and soft body are compiled in
and only unbound. And expect the cost to be in meeting arbitrary content rather than in the
dependency.
