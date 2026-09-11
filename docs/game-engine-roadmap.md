# Cetra as a Game Engine — Roadmap: What We Have and What We Need

A snapshot of where Cetra stands as a foundation for shipping an actual game (as
opposed to a rendering library). Grounded against the current code via a full
subsystem sweep. Companion to `rendering-roadmap.md` — **that** doc owns the
graphics pipeline in depth; **this** doc owns everything else (physics, gameplay,
assets, core, and the gaps between "engine" and "shippable game").

_Last updated: 2026-09-10._

---

## Verdict

The hard, specialized tech is done to a high standard. The renderer is
AAA-caliber and the physics is best-in-class. What's missing is the unglamorous
but well-understood glue: save/serialization, an in-game UI/menu layer,
and (near ship) Steamworks. Gamepad input landed in spec
11.109 (still owed a real-pad check), audio in spec 12.0 (heard on macOS,
owed on Linux and Windows) and animation blending in spec 12.1 (gate-verified
on a generated puppet; a real character is owed a look). 11.109 was the pivot from the renderer era to the game-platform
era, which is numbered from 12.0; it stays the last renderer-era spec, and the
major bump marks the change in the *kind* of work, as every prior one did.

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
  compensation (rotation-only; no root motion/scale/IK). Sophisticated for what
  it is.
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
  box, cylinder, subdivided plane. _No sphere/capsule/torus generators._
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
`gametest` (a genuinely playable physics sandbox: WASD character with jump,
spawnable crates, raycasts, a motorized hinge door you push open), `spores`
(curl-noise particle room on the game loop, headless-capable), `tree`
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
| **Save / serialization** | Partial — `.cscn` describes scenes, but nothing persists runtime state | The level/authoring half exists (§6.0). Still missing: save games, settings persistence, and any entity/physics state serializer. | ~1–2 weeks |
| **Game UI / menus** | Absent (ImGui is dev-only; SDF text exists) | Main menu, HUD, pause, inventory, settings screens. | ~2–3 weeks |
| **Animation blending** | **Done, spec 12.1** -- a blend layer over the single-clip animator: a pose became a blendable value, and over it a phase-synced 1D blend space, a crossfade whose settled pose is the incoming clip's own bit for bit, one bone-masked override layer that releases itself, and clip events dispatched after the pose is applied. Poses are per node, so two rigs animate independently; the `ANIMATOR` component ticks one once per rendered frame from the sim clock. Thirteen gate arms on a generated puppet (endpoints to the pixel, an analytic 21.60/45.00/68.40-degree nlerp midpoint, crossfade timing, mask and release, two rigs, one shared phase, event counts, and the committed walk clip binding all twenty bones by name), plus the corpus's first skinned golden. Still owed: a real character, judged by eye (`docs/verification.md`). | Smooth locomotion (idle↔walk↔run), layered actions. Needed for believable characters. | done (~1 week) |
| **Steamworks integration** | Absent | Achievements, cloud saves, overlay, input API. Needed near ship. | ~1 week |
| **IK** | Absent | Foot planting, look-at/aim. Quality-of-life, not blocking. | ~1 week (two-bone) |
| **VFX authoring, scripting, navmesh/AI, networking** | Absent | Only needed depending on genre. The particle system exists but emitters are built in code — an authoring/preset layer would come before heavy VFX work. Scripting (Lua) speeds iteration; navmesh/AI for enemies; networking for multiplayer. | genre-dependent |

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
4. **Game UI layer** (~2–3 weeks) — retained-mode menu/HUD built on the SDF text
   renderer (or a thin immediate-mode game UI distinct from debug ImGui).
5. **Save/settings serialization** (~1–2 weeks) — start with settings + a simple
   entity/state save format; `.cscn` already covers scene description.
6. **Steamworks** (~week, near ship) — achievements, cloud, overlay.

Fill in genre-specific systems (scripting, AI/navmesh, networking) only as the
actual game design demands them (YAGNI).

---

## Bottom line

Cetra has the two hardest engine pieces — a high-end renderer and best-in-class
physics with a working character controller and game loop — which is the 60% most
solo engine projects never finish. Shipping a game on it is realistic. The
deciding factors are scope discipline and finishing the well-understood glue
above, not the graphics API.
