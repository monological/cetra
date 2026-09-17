# Cetra as a Game Engine — Roadmap: What We Have and What We Need

A snapshot of where Cetra stands as a foundation for shipping an actual game (as
opposed to a rendering library). Grounded against the current code via a full
subsystem sweep. Companion to `rendering-roadmap.md` — **that** doc owns the
graphics pipeline in depth; **this** doc owns everything else (physics, gameplay,
assets, core, and the gaps between "engine" and "shippable game").

_Last updated: 2026-09-17._

---

## Verdict

The hard, specialized tech is done to a high standard. The renderer is
AAA-caliber and the physics is best-in-class. Of the glue **this document set
out to track**, what remains is Steamworks, near ship — but read the next
paragraph before taking that as a statement about how much is left to build.
Gamepad input
landed in spec 11.109 (still owed a real-pad check), audio in spec 12.0 (heard
on macOS, owed on Linux and Windows), animation blending in spec 12.1
(gate-verified on a generated puppet; a real character is owed a look), the
game UI in spec 12.2 (menus, a HUD, and the settings half of serialization;
owed another display, a real pad through a menu, and the two settings paths this
machine cannot write) and save games in spec 12.3 (entities, spawned objects,
per-section versions and a migration chain; owed a drift tolerance over many
steps, and a migration that changed for a reason nobody anticipated, which only
time produces) and two-bone foot planting in spec 12.4 (a closed-form solver on
12.1's pose seam, a ramp and steps to plant on, twelve gate arms; owed the
foot LOCKING it is a prerequisite for, without which a walking character still
slides, and a look at the plant fraction as a silhouette rather than a number),
whose deferred review items spec 12.5 then closed, and foot LOCKING itself in spec 12.9
(a runtime contact label, a world-space point held at the toe, hysteresis and an inertialized
transition; the slide falls from 0.0783 m to 0.0034 m and the `ik` group goes from thirteen arms
to nineteen). What 12.9 exposed was worth more than the feature: `gametest`'s player travelled at
about ten times the stride its own animation implies, so no contact was ever labelled and the
demo showed nothing. **Spec 12.10 is the answer to that** -- the engine measures what a clip's
feet imply about the ground, the blend space says what a mixture of them implies, and a game
divides to get a playback rate; the demo's travel speed comes from its clips instead of a
constant, and the `ik` group goes from nineteen arms to twenty-one. **Root motion is the other
answer to the same problem, and spec 12.18 is it** -- the clip states its own displacement and
the character goes exactly that far, where stride matching scales playback until the clip keeps
up with the body. The two are complements rather than rivals and both are live: root motion
wherever a clip states a travel, stride matching everywhere else, which today is every imported
rig in the tree. **Spec 12.19 closed the camera framework** -- one rig type behind every camera in
every app, `MouseDragController` deleted, and what the CONTROLS mean turned from a file static
somebody read into a field with a name and an arm that reddens when it is wrong. A sequencer and a
dialogue system to drive one are what remain of that row. **Spec 12.20 closed the animation
state machine**, the row this document had hedged on -- what plays and when is a table now, resolved
against whatever rig is loaded, and the survey that preceded it found FOUR live defects in the one
hand-rolled machine it replaced, none of which any of the suite's existing arms could see. Three of
them were invisible for one structural reason worth carrying: headless, this engine runs exactly one
fixed step per rendered frame, so anything that only breaks at another ratio cannot be reached by any
arm in the suite.
11.109 was the pivot from the renderer era to the game-platform
era, which is numbered from 12.0; it stays the last renderer-era spec, and the
major bump marks the change in the *kind* of work, as every prior one did.

**Specs 12.6 and 12.7 are a different kind of spec from the six above, and the
distinction is worth keeping.** Each of those six closed a named gap in the
table below. These two closed none. They put the game layer onto subsystems the
renderer era had built and never once driven from a game — the procedural
terrain, erosion, the water surface, the derived-data cook — by giving
`gametest` a plate floating 180 units above an eroded crater, a luminous pool at
the bottom of it, and swimming when you land. What that bought is not a feature
but EXERCISE, and it is worth recording what exercise alone found: a clustered
light index pool that zeroed a froxel's entire list on overflow instead of
clamping it (fixed in the engine, `light_cluster.c`), a cook recipe one byte
over the name limit that had therefore cached nothing at all, and a Perlin
permutation table whose one-deep memo cost eleven seconds of every launch to
four interleaved seeds. A demo app that only ever shows what already works is
not paying for itself.

**What this document measures, and what it does not.** Part 2 is headed
*"grep-confirmed absent"*, and a grep can only find a system somebody already
thought to name. Part 3 is a path to a playable **vertical slice**, which is a
much narrower thing than a finished engine. Read together the two have
repeatedly been taken to mean "one week of Steamworks from done", and they do
not mean that — the six rows Part 2 marks done are six rows it happened to list,
not a census. A sweep in 2026-09 for what neither part covered found three kinds
of gap, and Part 2 now carries all three:

- **Authoring.** There is no editor of any kind, and nothing in this document
  had ever said so. A scene is hand-written `.cscn` JSON or built in C.
- **Shipping.** No packaging, no bundle, no signing, no installer, no crash
  reporting. `./build.sh` makes `out/bin/<app>` and nothing a player could be
  handed — which gates Steamworks rather than following it. (Fullscreen was on
  this list until spec 12.15, which is the one item of the three that has since
  closed.)
- **Depth behind a shipped seam.** A row reading "done" means the seam exists
  and is verified, not that the feature is deep enough to ship a game on. The UI
  has six element kinds and no text input; text is byte-indexed with no UTF-8
  decoder anywhere, so a French menu is broken today; input has no rebinding;
  audio has no reverb or occlusion.

None of that changes the verdict on the hard parts. It changes the estimate.

The graphics API (OpenGL 4.1, no Vulkan/Metal/DirectX, no compute shaders) is
**not** a blocker for shipping on Steam. GL 4.1 runs on Windows/Linux/macOS and
Steam imposes no backend requirement. The real risk is scope: finishing an engine
*and* building a game, plus perf-budgeting the heavy render stack for mid-range
hardware.

---

## Part 1 — What we have today

### Rendering (mature — see `rendering-roadmap.md` for detail)

Far beyond what an indie game needs. In brief: PBR (GGX + clearcoat, sheen,
transmission/volume, thin-film iridescence, anisotropy, parallax occlusion),
GTAO + SSGI, Hi-Z SSR, cascaded shadow maps with PCSS soft shadows, split-sum
IBL + parallax-corrected reflection probes, Hillaire physically-based sky,
screen-space subsurface scattering, bloom + auto-exposure + 4 tonemappers,
volumetric fog, DOF, motion blur, weighted-blended OIT, and MSAA + SSAA + TAA.
All on GL 4.1 as fullscreen raster passes (no compute). HDR MRT G-buffer exposes
view-space normals, linear view-Z, motion vectors, and albedo.

### Physics & gameplay framework (strong)

Built on **Jolt Physics** via the JoltC C wrapper (`cetra/src/game/`).

- **Rigid bodies** — static/kinematic/dynamic; box/sphere/capsule/cylinder
  shapes; forces, impulses, torque; velocity/damping/gravity-factor; sensors;
  sleep. (`physics.c`, ~1640 lines)
- **Queries** — raycasts (unfiltered, layer-filtered, ignore-body) and shape
  sweeps. _Known limitation: raycast hit normals are currently zeroed._
- **Collision events** — threaded event queue drained on the main thread;
  begin/stay/end with contact point, normal, penetration depth, mapped to
  entities.
- **Constraints** — fixed, distance, hinge, slider, 6DOF; spring settings;
  velocity/position **motors** on hinge and slider (doors, elevators).
- **Character controller** — Jolt `CharacterVirtual`: ground/slope/step-up/
  floor-stick, moving platforms, pushes dynamic objects, constraint-aware
  (walks hinged doors open). (`character.c`)
- **Game loop** — a real fixed-timestep accumulator loop (`game.c`) wiring
  entities ↔ physics ↔ scene graph, with pause, FPS, interpolation alpha, and
  deterministic teardown ordering.
- **Entity/component layer** — lightweight entity-with-components (not a
  data-oriented ECS): one component per type per entity, bitmask queries via
  linear scan. Component slots: `MESH_RENDERER`, `RIGID_BODY`, `CHARACTER`,
  `ANIMATOR` (implemented in 12.1), `AUDIO_SOURCE` (12.0). `MESH_RENDERER` is a
  bare enum line and always has been -- an entity's visual is its `node`.

### Animation & assets (good)

- **Skeletal animation** — GPU linear-blend skinning, 128 bones, 4 influences/
  vertex; animated shadow casters; previous-frame bone matrices for TAA motion
  vectors. LERP/SLERP keyframe interpolation, playback speed, looping, multiple
  named clips.
- **Retargeting** — semantic Mixamo→custom-rig bone matching with rest-pose
  compensation (rotation-only; no root motion/scale/IK). Two modes since spec
  12.11: a local delta, and a GLOBAL-space reconciliation that is the only one
  which handles rigs whose rest orientations differ — it arms only when a source
  skeleton is supplied, so an animation-only file still needs `-s <tpose>`. The
  per-clip summary counts corrections actually computed rather than bones
  matched, which it used to conflate.
- **Blending** (spec 12.1) — a pose is a blendable value, and over it: a
  phase-synced 1D blend space, a crossfade when what plays changes, one
  bone-masked override layer, and clip events through a callback. Poses are
  per scene NODE, so two rigs animate independently in one frame.
- **Spring bones** — Verlet secondary motion (hair/cloth/straps) with length +
  swing-angle constraints. No collision.
- **Asset import (Assimp)** — FBX/glTF/GLB/OBJ; full PBR material extraction
  incl. KHR extensions; embedded textures; skeletons; animations; lights;
  cameras. Per-format UV-flip and FBX pivot handling.
- **Async loading** — worker pool for texture decode with main-thread GL upload
  handoff, sized from the machine (cores-2, clamped to [2,8]). File paths and
  compressed embedded images both stream; requests for a key already decoding
  attach to it rather than decoding twice. Loading the `abandoned_window` scene
  went 11.2s → 4.7s this way (§5.7). _Textures only; geometry/skeleton/animation
  parse is still synchronous._

### Particles & VFX (shipped)

A general Niagara-style system, not a one-off effect: **System → Emitter →
composable spawn/init/update Modules → pluggable Renderer**, over SoA pools with
swap-remove compaction. Particle systems are scene citizens — a `SceneNode`
borrows one, the `Scene` owns it, and the framework auto-ticks and auto-renders
it. Two sim backends behind a vtable: CPU, and **GPU via transform feedback**
(GL 4.1 has no compute). Curl-noise/drift/collider modules, soft depth-faded
billboards. Demos: `spores`, and the `abandoned_window` dust. (§5.0–5.3)

### Wind (shipped)

First-class directional wind as a scene object (direction/strength/speed/gust/
turbulence), applied as world-position offset in the vertex shader with a
per-mesh height mask, and displaced at both current and previous time so motion
vectors stay correct under TAA. Per-material `wind_response` opts geometry in.
Authored in `.cscn`, not hardcoded. (§5.5)

### Audio (shipped, spec 12.0)

A game-layer subsystem over **miniaudio** (vendored single-header, its own
CoreAudio/ALSA/WASAPI backends): one output device wrapped as an `AudioSystem`,
2D fire-and-forget SFX and music, held voices from a file (WAV/MP3/FLAC) or a
procedural tone, 3D positional sound with the camera as listener, and mixer
buses (master/music/sfx/ui). Owned by the `Game` like the physics world, freed
after the entity manager; the `AUDIO_SOURCE` entity component (dormant until now)
is implemented and synced from its entity each frame. The device is a seam: a
windowed run opens the OS device, a headless run opens **none** and renders
offline, so the whole layer above it is deterministic — which is what the
`audio` gate group asserts on (onset, panning, distance falloff, bus routing,
file decode), with procedural tones and no committed audio. _Heard on macOS
(CoreAudio); Linux and Windows are still owed the same listen — see
`docs/verification.md`._

A text scene-description format (`cscene.c`) layered over imported models:
environment, lights, post-processing, wind, dust, material overrides and camera,
using a presence-flag pattern so unspecified fields keep engine defaults. This is
authoring/description, **not** save-game state. (§6.0)

### Core, scene, text, input (functional)

- **Engine** — GLFW 4.1 core context, 4× MSAA, HDR framebuffer, **headless mode
  with PPM screenshots** (great for CI/verification), Dear ImGui debug panels,
  shader hot-reload, delta-time clamping.
- **Scene graph** — hierarchical transforms, k-nearest-light selection (max-heap),
  frustum culling, ray-picking, motion-vector-ready previous transforms.
- **Geometry** — procedural circle, rect (incl. rounded corners), Bézier curves,
  box, cylinder, sphere, subdivided plane. _No capsule or torus generators._
- **Text** — SDF text rendering (glow/plasma effects, 3D world-space text).
  _Word-wrapping is a TODO._
- **Input** — keyboard, mouse and up to four gamepads through GLFW's standard
  layout (the bundled SDL controller database, plus a mapping file loadable at
  runtime), polled once a frame with per-frame edges, dead zones, hot-plug, and
  an action table a game binds keys, buttons and axes to (`game/input.c`, spec
  11.109). A reader seam lets a text script stand in for a pad, which is what
  the `gamepad` gate group verifies with. _The real device path -- GLFW's
  joystick backends -- has not been exercised on this machine; two recipes for
  doing so are in `docs/verification.md`._

### Apps (working demos)

`render` (flagship model/scene viewer, `.cscn` driver + CI screenshot harness),
`gametest` (the game layer's whole surface in one app, and the closest thing in
the tree to a vertical slice: a skinned puppet on an `ANIMATOR` blending
idle/walk/run from the character's own post-solve speed, feet planted by
two-bone IK on a ramp and three steps, menus and a HUD, save and load, audio, a
scripted-pad path, spawnable crates, raycasts and a motorized hinge door — all
of it on a plate floating above a crater you can fall into and swim in),
`spores` (curl-noise particle room on the game loop, headless-capable), `tree`
(procedural trees), `shapes` (2D primitive demo), `splash` (SDF text
showcase).

### Build & dependencies

CMake + Ninja (`build.sh`), clang `gnu11` / `gnu++17` (CMake's `C_EXTENSIONS`
defaults to ON, so the emitted flag is `gnu11`, not `c11`). The `cetra` target
also compiles with `-D_POSIX_C_SOURCE=200809L` for JoltC, which on macOS hides
Darwin extensions — a file needing them defines `_DARWIN_C_SOURCE` first (see
`util.c`). System deps found via pkg-config: GLFW3, GLEW, cglm, Assimp.
Vendored: JoltC (Jolt Physics), cimgui/Dear ImGui, stb, cwalk, uthash, log.c.
Python3 generates `shader_strings.h` from `shaders/*.glsl` at build time.

---

## Part 2 — What we need (definitive gaps)

Grep-confirmed absent across the repo (excluding vendored code). Effort estimates
are rough and assume a single experienced dev.

| System | Status | Why it matters | Rough effort |
|---|---|---|---|
| **Audio** | **Done, spec 12.0** -- miniaudio wrapped as a game-layer `AudioSystem`: one device, 2D SFX and music, 3D positional sound with the camera as listener, mixer buses, and the `AUDIO_SOURCE` component implemented. The layer above the device is gate-verified off offline PCM (onset, pan, distance, bus routing, decode), and heard on macOS. Still owed: a listen on Linux and Windows (`docs/verification.md`). | No game ships silent. Steam players expect it, and the offline-render path doubles as the deterministic test seam. | done (~1 week) |
| **Gamepad input** | **Done, spec 11.109** -- GLFW's standard layout behind a reader seam, an action table, hot-plug, a loadable mapping file; the layer above the seam gate-verified by a scripted pad. Still owed: one run with a real controller, or the Linux uinput recipe (`docs/verification.md`), since no pad was at hand. | Steam players expect controller support. Steam itself presents a virtual Xbox pad to a GLFW game, which is what shipped titles rely on; Steam Input's own API is a second reader behind the same seam, booked with Steamworks. | done (~2 days) |
| **Save / serialization** | **Done, spec 12.3** — `game/save.c`, a third descriptor table walked both ways, beside `.cscn` (what exists) and the config snapshot (how it is tuned). Entities matched by NAME, components keyed by name rather than by a positional enum, a GET/SET row pair for state that lives inside Jolt, spawned objects carrying the recipe that made them, per-SECTION versions with a migration chain that runs on the parsed tree, and an atomic write. File-level problems refuse the file; record-level problems drop the record and count it, so one unbuildable object never costs a player the rest. Eight gate arms, no GPU. Still owed: a drift tolerance measured over many steps, the Linux and Windows paths, and unknown-field preservation for the day Steam Cloud syncs between patch levels. | done (~1 week) |
| **Game UI / menus** | **Done, spec 12.2** -- a retained tree of elements over a pure two-pass layout, a geometric focus model, and a theme whose every zero means inherit, drawn through a general post-tonemap overlay hook so a menu is neither graded nor rescaled and IS captured by a headless screenshot. The element list is CLOSED (panel, label, button, toggle, slider, selector) with three escape hatches under it: a custom draw callback, a custom fragment program, and the raw draw-list primitives. Input is settled at one depth -- a `ui` flag on an action and one suppression switch -- and Escape now opens a menu in every app instead of quitting. Ten gate arms assert layout, wrapping, navigation, hit-testing, capture, the stack, theme resolution, the closed list and the settings round-trip with no GPU at all, plus two menu goldens. Still owed: a 4K/HiDPI display other than this one, a real controller through a menu, and the Linux and Windows settings paths. | Main menu, HUD, pause, settings screens. Inventory is an app's own screen built from these elements. | done (~1 week) |
| **Animation blending** | **Done, spec 12.1** -- a blend layer over the single-clip animator: a pose became a blendable value, and over it a phase-synced 1D blend space, a crossfade whose settled pose is the incoming clip's own bit for bit, one bone-masked override layer that releases itself, and clip events dispatched after the pose is applied. Poses are per node, so two rigs animate independently; the `ANIMATOR` component ticks one once per rendered frame from the sim clock. Thirteen gate arms on a generated puppet (endpoints to the pixel, an analytic 21.60/45.00/68.40-degree nlerp midpoint, crossfade timing, mask and release, two rigs, one shared phase, event counts, and the committed walk clip binding all the rig's bones by name), plus the corpus's first skinned golden. Still owed: a real character, judged by eye (`docs/verification.md`). | Smooth locomotion (idle↔walk↔run), layered actions. Needed for believable characters. | done (~1 week) |
| **Steamworks integration** | Absent | Achievements, cloud saves, overlay, input API. Needed near ship. | ~1 week |
| **IK** | **Planting and LOCKING done, specs 12.4, 12.5 and 12.9; look-at/aim untouched** — two-bone foot PLANTING: a closed-form solver on 12.1's pose seam, writing globals only; a ramp of known slope and three steps always in the world, since flat ground is the one case where planting is correctly a no-op; and thirteen gate arms, of which the load-bearing one ticks a walk cycle and measures the ankle's travel, because every other arm poses the rig once and all of them passed while the feet were welded to the floor. Six feet land on their target to 0.00000000 m across flat ground and both fixtures. Spec 12.9 added the LOCK above it: a runtime contact label, a world-space point held at the toe, two thresholds with hysteresis, and an inertialized transition -- 0.0034 m of slide against planting's 0.0783 over the same ticks, and six more arms. What it does NOT do is rescue a character whose travel speed has nothing to do with its clips, which is what `gametest` is, and that is the next thing a game on this engine needs rather than more IK. Spec 12.5 closed the eight review items 12.4 deferred — a `sole_offset` derived once from the bind pose, so a caller states a GROUND rather than an ankle target and no call site re-derives the clearance; `max_pelvis_drop` as a FRACTION of leg length rather than metres, so it carries to a rig of another size; and the probe memoisation, whose stated hazard turned out not to exist. That was quality, not coverage: the five debts `docs/verification.md` records against this path all stand, and its verdict line stays Owed. Look-at/aim untouched. | Foot planting, look-at/aim. Quality-of-life, not blocking. | planting and locking done (~2 weeks); look-at still open |
| **Ragdoll, vehicles, soft body / cloth** | **Ragdoll DONE, spec 12.16; vehicles and soft body still unbound.** The estimate below was right and the work confirmed it: a C++ TU behind a C header, twelve capsules derived from the rig with no authored asset, and the bones written at the pose seam `ik_solve` already occupied. What it cost beyond the binding was the mapping the row predicted — and one thing it did not: two of the twelve bones cannot be resolved by NAME, because the semantic matcher gives a whole spine one category and refuses ambiguity by design, so the chain is walked instead. **Nine gate arms, and the last three were each added after the ones before them were green over something visibly broken** — which is the part of this row a later binding job should read, because none of the three failures was in the binding. A character that came apart: a left-handed capsule basis is a reflection, Jolt keeps orientations as quaternions, no quaternion is a reflection, and the C side round-trips perfectly either way, so crossing a language boundary changed what a frame IS. A character that melted: shapes DERIVED from an arbitrary rig, where per-bone boxes are per MESH and a rig arriving as fifteen of them measures nothing for most of its body, giving eleven uniform sticks and a pelvis the size of a pea. And a "settles connected" claim that was simply false. **Every arm that measures ONE body was green through all three**; the three added measure a RELATIONSHIP between two, which is the only thing a ragdoll is. Getting up is explicitly out and the constraint type chosen is the one that keeps it possible. The original entry follows, since it is still true of the other two: **Present in Jolt, UNBOUND in JoltC** — `cetra/src/ext/JoltC/JoltPhysics/Jolt/Physics/` carries `Ragdoll/`, `Vehicle/` and `SoftBody/`, all vendored and compiled. What is missing is the C binding: ragdoll appears nowhere in `JoltC/Functions.h`; vehicle appears only as the enum TAG `JPC_CONSTRAINT_SUB_TYPE_VEHICLE` with no constructor behind it; and every soft-body creation entry point is **commented out** (`Functions.h:1506-1524`), leaving `JPC_Body_IsSoftBody` as the one live symbol. The escape is precedented twice in this tree — `cluster_build.cpp` and `physics_cook.cpp` are C++ TUs that reach past JoltC, and `AGENTS.md` records the same shape for `JPH::Trace`. **So this is a binding job, not a solver job**, and the estimate below should be read that way; writing any of these three from scratch would be months. Ragdoll additionally wants a bridge from `animation.c`'s `Skeleton` to `JPH::Skeleton` + `RagdollSettings`, and that mapping is the real work — the solver is done. **That estimate held for the binding and was wrong for the mapping.** The binding was a day. Making the derivation survive rigs nobody wrote it against took two further rounds, both found by watching rather than by any arm, and neither had anything to do with Jolt. Read the two estimates below as the BINDING only. | Death animations and hit reactions (ragdoll), any driveable thing (vehicle), rope/banner/capes (soft body). Spring bones cover hair and straps already and do not collide. | ragdoll done (~1 day to bind, ~3 to make the derivation survive an arbitrary rig); vehicles and soft body ~2-4 days each to bind |
| **Editor / scene authoring** | Absent, and never previously listed here. Ten apps, none of them an editor: a scene is hand-written `.cscn` JSON or built in C. No placement UI, no gizmo-driven authoring, no asset browser, no play-in-editor, no undo, no prefabs. The ~185 ImGui controls and the config snapshot (spec 11.71) TUNE what already exists, which is a different job from authoring what is there — the snapshot's own header says it cannot create a light, bind a texture or place a probe. | In most engines this is the single largest remaining line item. It decides whether anyone but the author can build a level, and it is what turns iteration time from a rebuild into a drag. | months; the largest single item in this table |
| **Packaging and distribution** | Absent. No CPack, no install rule (the old one was removed deliberately — see the comment at the end of `cetra/CMakeLists.txt`), no `.app` bundle or `Info.plist`, no code signing or notarization, no installer, no crash reporting, no telemetry, no update path. | Nothing reaches a player without it. It **gates** Steamworks rather than following it: achievements on a build nobody can install is the wrong order. | ~1-2 weeks |
| **Display and window modes** | **Done, spec 12.15.** Three modes -- windowed, exclusive fullscreen, borderless -- applied from the settings screen, persisted, and restored on the next run, with a monitor chosen by NAME rather than index so unplugging a display cannot silently move the game. Neither non-windowed mode changes a video mode: both take the monitor as it is, which removes resolution-restore-on-crash and stored-mode validation from the problem entirely and leaves `--render-scale` the performance lever it already was. The windowed geometry the old comment said the engine did not keep, it now keeps, refreshed while the window is windowed rather than sampled once on the way out -- a player drags a window, so a rectangle latched at departure restores it to somewhere it left hours ago. vsync moved onto the engine in the same change, having been consumed once at init and unrecoverable after -- which made it impossible to put back when taking a monitor drops it. Five gate arms, no GPU, over `engine_window_placement`: a pure function of mode, monitor rectangle and saved geometry, split from the effect because the switch is REFUSED under headless and there is otherwise nothing an arm can reach. **Review caught two defects worth carrying**: a mirrored enum and four static asserts routing around an include boundary that did not exist (`game/` is public and `game.h` already includes `engine.h`), and a switch that was not idempotent -- `settings_apply` pushes every value on every edit and the UI fires it once a frame while a slider is held, so dragging the volume slider moved the window to the corner. Still owed: a second display. This machine has one, so the name lookup cannot be told from one that always answers 0. | Table stakes on every platform, and it was the one settings row that lied to the player. | done (~1 day) |
| **Text encoding and localization** | Absent, and the ceiling is structural rather than a missing feature. `text.c` treats every string as BYTES — `(unsigned char)charset[i]` (:106), `(unsigned char)mesh->text[i]` (:495), `(unsigned char)*p` (:667) — and there is no UTF-8 decode anywhere in the tree. A codepoint above 127 therefore renders as two or more wrong glyphs: a French or German menu is broken **today**, and CJK, RTL and combining marks are out of reach entirely. Closing it is three things: a decoder, an atlas that can page (a CJK face does not fit one sheet), and a string table. | Any market outside English, and the bug is live rather than pending. | ~1-2 weeks for the decoder and atlas paging; the string table is ongoing content |
| **UI element vocabulary** | The closed list is six (`ui.h:248-257`): panel, label, button, toggle, slider, selector. Absent from it: **text input**, scroll view, list/grid, drag-and-drop, tabs, modal dialog, tooltip. The three escape hatches mean an app can draw any of them by hand, which is the deliberate design — but it means an inventory screen, a name-entry field, a key-rebinding row or a quest log is app code rather than an element, and every game re-writes them. | Real games are mostly these screens. The closed list was the right call for 12.2; what it does not yet say is which elements come next. | ~1 week per element family |
| **Input rebinding** | Absent, and stated as such: `input_bind` takes a BORROWED const table, and `settings.c`'s header records that bindings are deliberately not carried because persisting them wants the remapping UI they would exist for. That UI needs a key-capture element the closed list above does not have, so these two rows are one gap approached from two sides. | Accessibility requirement on most platforms, and expected by any player on a non-QWERTY layout. | ~3-4 days, after a capture element |
| **Camera framework** | **Done, spec 12.19.** `cetra/src/camera_rig.c/h`: a rig is an ANCHOR, an ARM and an AIM, with no mode enum -- a follow is an anchor that moves, a viewer orbit one that does not, first person a distance of zero, and a pinned pose derives all three. Arm response, an occlusion PROBE seam, a pose BLEND, a decaying SHAKE, and a Catmull-Rom RAIL. `MouseDragController` is deleted and every camera in every app -- render, spores, tree, forest, gametest and the 2D canvas -- goes through it; the engine has ONE slot, so the stand-down guard five apps each hand-rolled is gone. **What the CONTROLS mean is now a field with a name**, `steers_controls`, defaulting to false: 12.17's decision kept, and turning it off reproduces 104.85 degrees of error, to the second decimal the number 12.17 recorded for the scheme it was reverting. Player settings (FOV, look sensitivity, invert-Y, motion reduction) ride `GameSettings`. **Still absent: a sequencer or cutscene track, and a dialogue system to drive one** -- a rail is an anchor a caller drives, with nothing in this tree that scripts one over time. Also new: a scripted POINTER (`--pointer-script`), mirroring `--pad-script`, because nothing in the suite had ever pressed a mouse; 17 arms in the `camera` group, five needing no window. | The scripted camera moment is now possible rather than authored; a sequencer is what would author it. | Done; a sequencer is its own project |
| **Animation state machine** | **Done, spec 12.20.** `cetra/src/anim_graph.c/h`: a table of STATES, each naming a playback source the app registered plus the parameters that drive its blend axis and rate, and a table of TRANSITIONS over a CLOSED condition vocabulary -- one table, one order, first match wins. The row's own diagnosis is what it answered: *"`gametest` then hand-rolled one across `player_medium`, the locomotion space, a jump one-shot and a swim source, which is the evidence that every game on this engine writes the same thing."* That hand-rolled machine is deleted, along with its enum, its jump latch, the chaser's own two-state copy and **twelve clip-presence guards**. **The line count is honestly a wash** -- about 120 lines leave the app against ~650 of new engine -- and the case is the other three things. **Binding resolves every state against the rig actually loaded**: a source whose clip is missing refuses to register, which makes the states naming it unreachable and PRUNES every row into them, reported once by name. That is the twelve guards collapsed into one decision, and it is why `--puppet` can no longer be handed a rig that walks into a state it cannot play. **`animator.h:27`'s non-goal is kept rather than amended** -- *"There is no state machine here"* is still true of `animator.c`; this is a layer over it, and everything it does goes through that file's existing public calls, which `graph-identity` holds to 0 px of pose and 0 of clock over 120 ticks. And **the decision became assertable**: twenty arms, eleven of which need no window, no GL, no rig and no clip, because a graph binds to a NULL animator and a state may name no source. **Four live defects came out of the survey, none of which any of 552 existing arms could see**, and the roadmap should carry them because three were invisible for the same structural reason. The jump-to-fall hand-off read a per-frame EDGE from the fixed step, so it was dropped on a frame that ran no step and doubled on one that ran two -- and **headless is always exactly one step per frame**, which is why the whole suite was blind to it. A ragdolled player kept its last source running: clock turning, the walk's own footstep events firing from a body nobody was drawing, root motion accumulating undrained, with no revive path to stop it. The medium test guarded air on its clip existing and water on nothing, so a rig with its own walk and no stroke was labelled swimming while its walk cycle played, had its rate pinned and its root-motion branch skipped, and decayed to a standstill on a loop whose only fixed point was zero. And the waterline was a hard compare against a plane the buoyancy drive aims at, so a floating character crossed it about once a second **forever**, crossfading between its stroke and its walk -- fixed by two thresholds, which is hysteresis a bool cannot express and the reason the vocabulary carries comparisons at all. **Still absent**: any authoring surface. A graph is a C table in an app -- no file format, no editor, no hot reload, no way for a designer to add a state. Also absent: sub-state machines, 2D blend spaces, additive layers, and the override layer, which stays the app's one line. | Locomotion, combat and reaction logic all want it. The demo was the proof it gets written either way. | done; the estimate below was for the TABLE, and the table was the small half -- the conversion, the four defects it turned up and the docs were the rest, which is the same shape 12.16 recorded for the ragdoll binding |
| **Root motion** | **Done, spec 12.18.** A clip states how far its root travels, the animator hands that to the character, and the character goes exactly that far; `gametest` now travels at the speed its own clips carry rather than at a constant, and has two authored moves that stride matching cannot express. The estimate below was right. Three things it found are worth carrying. **Extraction is per ENTRY, never from the blended pose** — the weights and the fade move too, so differencing a blended root reads a knob turn or a crossfade as a stride across the room; `anim-root-fade` is the arm that tells the two designs apart, and the first draft was the wrong one. **The feedback runs the other way now**: the blend knob has to come from the STICK, because the travel comes from the clip the knob selects, so a knob fed by the achieved speed starts at zero, selects the standing clip, travels nothing and stays there — which also costs the wall-stops-the-walk behaviour stride matching gets for free. And **the benefit it is famous for is unmeasurable in this tree**: root motion's point is that a stance foot cannot slide, and the only clips carrying a root curve are the four authored for this spec, on a rig whose pendulum walk has no stance at all. Six of the eight committed clips are in place — `strut_walk` states 0.000071 m over its whole loop — while the landing and the jump-rise each carry about 9 cm of hip between their ends, which the game never sees because a retargeted channel is refused. The MEASURING is what mattered rather than the answer: the claim reached four documents off a sample of one, and only asking all eight found the two exceptions. What is NOT done: a retargeted clip still takes its position from the bind pose, so an imported character can never carry root motion until the retarget carries translation with a proportion transfer. | Anything whose displacement is choreographed rather than steered. | done (~1 day; the docs took longer than the engine) |
| **Audio depth** | The seam shipped in 12.0 and is deep for what it covers. Absent behind it: reverb zones, occlusion and obstruction, a DSP/filter graph, interactive or stem-based music, voice and dialogue playback, ducking. miniaudio carries some of this already, so parts are configuration rather than construction. | A room that sounds like a room, and music that responds to play. | genre-dependent |
| **Accessibility** | Absent. No subtitle or caption system, no colourblind palettes, no UI scaling beyond the per-element font size, no remapping (above), no hold-to-toggle. **Motion reduction reaches the CAMERA since spec 12.19** -- a settings toggle that is exactly the no-shake path rather than a quieter one -- and not the POST STACK, whose motion blur is untouched. | Increasingly a platform requirement, and cheapest to design in rather than retrofit. | ~1 week for the basics, once rebinding and a caption element exist |
| **VFX authoring, scripting, navmesh/AI, networking** | Absent | Only needed depending on genre — though **scripting is arguably mis-filed here**: iteration speed is not genre-dependent, and today every gameplay change is a C rebuild. The particle system exists but emitters are built in code, so an authoring/preset layer would come before heavy VFX work. Navmesh/AI for enemies; networking for multiplayer. | genre-dependent |

Also worth noting (not "gaps" but design ceilings):

- The entity layer is linear-scan, fine for hundreds/low-thousands of entities,
  not tens of thousands of agents.
- The render stack is heavy; SSGI/SSR/GTAO/volumetrics on GL 4.1 (no compute)
  need perf budgeting for mid-range GPUs.
- Only PPM image export (a debug feature — not a real gap).

---

## Part 3 — Suggested path to a playable vertical slice

Ordered smallest-effort-to-playable first. Each is independent enough to land on
its own branch.

1. **Gamepad input** — done in spec 11.109; the remaining item is a minute with
   a real pad, per `docs/verification.md`.
2. **Audio** — done in spec 12.0 (miniaudio: SFX + music + 3D positional through
   the `AUDIO_SOURCE` component), heard on macOS; the remaining item is a listen on
   Linux and Windows, per `docs/verification.md`.
3. **Animation blending** — done in spec 12.1 (a phase-synced blend space, a
   crossfade, one masked override layer and clip events, with per-node poses so
   two rigs animate at once); the remaining item is a look at a real character,
   per `docs/verification.md`.
4. **Game UI layer** — done in spec 12.2 (a retained tree, a pure two-pass
   layout, geometric focus, a closed element list with three escape hatches under
   it, and settings persistence), with gametest carrying main, pause, settings and
   HUD screens; the remaining items are a 4K display, a real pad through a menu,
   and the Linux and Windows settings paths, per `docs/verification.md`.
5. **Save serialization** — done in spec 12.3 (entity and component state, spawned
   objects rebuilt from their recipes, per-section versions and a migration chain,
   written atomically). A restored world is deliberately not bit-exact: Jolt's
   solver state stays out of the file, or the format would pin to `JPH_VERSION_ID`
   and break every save on a physics upgrade. The remaining items are a measured
   drift tolerance, the two platform paths this machine cannot write, and a
   migration that changed for a reason nobody anticipated — see
   `docs/verification.md`.
6. **Foot locking** — done in spec 12.9. A contact is labelled at the solve seam, pinned
   at the toe in WORLD space, and held until the clip walks away from it, with two
   thresholds rather than one and an inertialized transition in place of the position
   lerp. A stance foot travels **0.0034 m** across the ground where planting left
   **0.0783 m**, over the same ticks with the body at the speed the clip's own feet imply.
   Six new arms take the `ik` group from thirteen to nineteen, among them `ik-drop`, which
   exercises the pelvis cap that spec 12.5 found nothing had. One thing it did NOT do,
   deliberately and written up in `docs/foot-locking.md`: the extension soft-clamp is refused,
   because on a bind pose that is exactly straight 1 per cent of soft band costs 16.2 degrees of
   permanent bend. It also could not SHOW the feature — `gametest`'s player moved at about ten
   times the stride its animation implies, so no contact was ever labelled and the frame was 0 px
   against `--no-lock`.

   **Spec 12.10 closed that**, which was the nearest thing to a blocker in this list.
   `animation_stride_speed` measures the ground speed a clip's own feet imply and refuses when
   the clip does not walk; `animator_stride_speed` blends it across a space, which no caller can
   do from outside since the answer is not the weighted mean of the strides. `gametest`'s
   locomotion entries sit at the speeds they imply, its knob is metres per second, and full stick
   is derived from the fastest clip — 2.67 m/s on the committed humanoid, where `--no-lock` now
   moves 2217 px. Three new arms; two defects found by watching it rather than by measuring, one
   of them a walk playing at eighteen times speed at any stick short of full, true since 12.9.
   Root motion, the other answer to the same problem, arrived in **spec 12.18** and is the
   inverse of this one: rather than scaling playback until the clip keeps up with the body, the
   clip states how far it travels and the body goes exactly that far. It is what an authored
   lunge, dodge or turn-in-place needs, since those are distances an animator chose and
   rate-scaling one is exactly wrong. What it could NOT do is improve the numbers above: only
   the four clips 12.18 authored carry a root curve, and they sit on the same stanceless
   pendulum, while the committed clips a locomotion space plays are in place -- `strut_walk`
   states 0.000071 m of travel over its whole loop, which 12.18 measured rather than assumed,
   along with the other seven.
   **Specs 12.11 through 12.14 are the debt 12.1 booked, paid by pointing the
   whole stack at a real character rather than the generated puppet.** 12.11 gave
   the loader a second rest pose — global-space retargeting, and `gametest`'s shared
   clips loaded with the rig they were authored on — which took a 122-bone
   third-party humanoid from lying on its face at a measured 0.12 m/s stride to
   walking at 5.53, and derived the IK bend pole from the RIG, since every caller in
   this tree passes a `+Z` that folds a real thigh backwards. 12.12 raked the demo's
   key light 45 degrees off vertical, because hung straight down it lit the plate at
   `N.L` = 1 and a standing figure at nearly zero, so any imported character read as a
   silhouette against its own brightly lit stage. 12.13 pulled the follow camera in
   and argued a larger defect behind it: movement was camera-relative, so forward
   was always away from the camera and you could never see the character's front.
   12.14 gave the derived pole an arm that FAILS against the code it replaced, the
   existing one having run on a rig whose bind knee sits exactly on the hip-ankle
   line and therefore never entering the new path at all. Every one of the four was
   found by looking at the demo, none by a suite.

   **Spec 12.17 reversed 12.13's half of that**, and was found the same way — turning
   the camera and pressing W walked the character across the frame rather than up it,
   reported by the person playing it as *"the wasd keys don't work from the new camera
   reference"*. `cam_yaw` moves only when the player turns the camera — an arrow key
   or the right stick — and never chases the character's facing, that design having
   been tried in 12.6 and abandoned, so orbiting round to see the character's front
   is a thing the player can simply do. That is what 12.13's argument had assumed was
   unreachable. The larger finding is that **12.13 changed the behaviour and told
   nobody**: `cli-reference.md` and `AGENTS.md` both went on describing camera-relative
   movement for the four specs the code was world-aligned, so anyone checking the
   documentation before reporting this would have been told they were right. And
   nothing in the suite reads displacement, so neither flip could be contradicted by
   any arm; `pad-camera-relative` now says which scheme is live.
7. **Steamworks** (~week, near ship) — achievements, cloud, overlay. **Do not
   start here.** Packaging gates it: there is no bundle, no signing and no
   installer, so there is nothing for an achievement to attach to. See Part 2.

**Past the slice, the list stops being ordered**, because the order depends on a
choice this document cannot make: whether the goal is a game shipped **on** this
engine or an engine other people build **in**. The two diverge immediately.

- **A game shipped on it** — packaging first, since a build nobody can install is
  the hard blocker (display modes closed in 12.15, root motion in 12.18, the
  camera framework in 12.19 and the animation state machine in 12.20); then
  whatever UI elements the game's own screens need, and the text encoding the
  `text.c` row records as a live bug rather than a gap.
- **An engine others build in** — the editor, scripting and an asset pipeline,
  which is a far larger programme and changes what is worth doing to the runtime
  underneath it.

Either way the **platform debt is unconditional**: five rows in Part 2 marked
done carry the same three owed items — the Linux paths, the Windows paths, and
one run with a real controller. Nothing ships anywhere until those close, and
none of them is new work, only work nobody has been able to do on this machine.

Fill in genre-specific systems (AI/navmesh, networking, VFX authoring) only as the
actual game design demands them (YAGNI). Scripting is listed with them and
probably should not be: it buys iteration speed regardless of genre.

---

## Bottom line

Cetra has the two hardest engine pieces — a high-end renderer and best-in-class
physics with a working character controller and game loop — which is the 60% most
solo engine projects never finish. Shipping a game on it is realistic, and the
graphics API is not what decides it.

What is left is not hard in the way those two were; it is **wide**. It divides
into three, and only the first was ever in this document:

1. **The vertical-slice glue** — Part 3. One item left, and it is blocked on (2).
2. **Shipping** — packaging, signing, an installer, fullscreen, crash reporting.
   Weeks, not months, and nothing reaches a player before it.
3. **Authoring** — an editor, and a scripting layer beside it. Months, and it is
   the item that decides whether anyone but the author can make something here.

The physics row is the pattern worth carrying to the rest of this table: ragdoll,
vehicles and soft body read as absent for a year, and are in fact compiled into
the binary already, missing only a C binding this tree has twice precedented how
to write. **Check what a dependency already does before costing a gap** — the
distance between "absent" and "unbound" was months of imagined work.

Spec 12.16 shipped that row and returned the other half of the lesson, which is
worth carrying with it so the first half is not read as "it was free": binding
Jolt's ragdoll took a day, and making the twelve capsules it needs survive a rig
nobody wrote them against took three times that. **The cost of a gap like this
is rarely in the dependency — it is in meeting arbitrary content.** Both further
rounds were found by watching a character fall over, with every gate arm green.

The real risk remains scope: finishing an engine *and* building a game. This
document now states the size of the first honestly, which it previously did not.
