# Cetra as a Game Engine — Status: What We Have and What We Need

A snapshot of where Cetra stands as a foundation for shipping an actual game (as
opposed to a rendering library). Grounded against the current code via a full
subsystem sweep. Companion to `rendering-roadmap.md` — **that** doc owns the
graphics pipeline in depth; **this** doc owns everything else (physics, gameplay,
assets, core, and the gaps between "engine" and "shippable game").

_Last updated: 2026-07-20._

---

## Verdict

The hard, specialized tech is done to a high standard. The renderer is
AAA-caliber and the physics is best-in-class. What's missing is the unglamorous
but well-understood glue: audio, gamepad input, save/serialization, an in-game
UI/menu layer, animation blending, and (near ship) Steamworks.

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
  `ANIMATOR`, `AUDIO_SOURCE` (last three are declared but unimplemented).

### Animation & assets (good)

- **Skeletal animation** — GPU linear-blend skinning, 128 bones, 4 influences/
  vertex; animated shadow casters; previous-frame bone matrices for TAA motion
  vectors. LERP/SLERP keyframe interpolation, playback speed, looping, multiple
  named clips.
- **Retargeting** — semantic Mixamo→custom-rig bone matching with rest-pose
  compensation (rotation-only; no root motion/scale/IK). Sophisticated for what
  it is.
- **Spring bones** — Verlet secondary motion (hair/cloth/straps) with length +
  swing-angle constraints. No collision.
- **Asset import (Assimp)** — FBX/glTF/GLB/OBJ; full PBR material extraction
  incl. KHR extensions; embedded textures; skeletons; animations; lights;
  cameras. Per-format UV-flip and FBX pivot handling.
- **Async loading** — 4-thread pool for texture decode with main-thread GL
  upload handoff. _Textures only; geometry/skeleton/animation parse is
  synchronous._

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
- **Input** — keyboard + mouse only, with an edge-detecting polling layer for
  games (`game/input.c`).

### Apps (working demos)

`render` (flagship model/scene viewer + CI screenshot driver), `gametest` (a
genuinely playable physics sandbox: WASD character with jump, spawnable crates,
raycasts, a motorized hinge door you push open), `tree` (procedural trees),
`shapes` / `pcb` (2D primitive demos), `splash` (SDF text showcase).

### Build & dependencies

CMake + Ninja (`build.sh`), clang C11 / C++17. System deps: GLFW3, GLEW, cglm,
Assimp. Vendored: JoltC (Jolt Physics), cimgui/Dear ImGui, stb, cwalk, uthash,
log.c. Python3 generates `shader_strings.h` from `shaders/*.glsl` at build time.

---

## Part 2 — What we need (definitive gaps)

Grep-confirmed absent across the repo (excluding vendored code). Effort estimates
are rough and assume a single experienced dev.

| System | Status | Why it matters | Rough effort |
|---|---|---|---|
| **Audio** | Absent (only an unimplemented `AUDIO_SOURCE` enum) | No game ships silent. Needs SFX, music, 3D positional audio. | ~1 week (drop in miniaudio) |
| **Gamepad input** | Absent (kb/mouse only) | Steam players expect controller support. GLFW already exposes `glfwGetGamepadState`. | ~1–2 days |
| **Save / serialization** | Absent (no state or scene persistence) | Save games, settings, level format. No JSON/binary serializer yet. | ~1–2 weeks |
| **Game UI / menus** | Absent (ImGui is dev-only; SDF text exists) | Main menu, HUD, pause, inventory, settings screens. | ~2–3 weeks |
| **Animation blending** | Absent (one clip at a time) | Smooth locomotion (idle↔walk↔run), layered actions. Needed for believable characters. | ~1–2 weeks |
| **Particle system** | Absent | Dust, smoke, embers, weather, impacts. (Actively being designed.) | ~1–2 weeks first pass |
| **Steamworks integration** | Absent | Achievements, cloud saves, overlay, input API. Needed near ship. | ~1 week |
| **IK** | Absent | Foot planting, look-at/aim. Quality-of-life, not blocking. | ~1 week (two-bone) |
| **Particles/VFX authoring, scripting, navmesh/AI, networking** | Absent | Only needed depending on genre. Scripting (Lua) speeds iteration; navmesh/AI for enemies; networking for multiplayer. | genre-dependent |

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

1. **Gamepad input** (~days) — cheapest win, unblocks "feels like a game."
2. **Audio** (~week) — miniaudio: SFX + music + basic 3D positional via the
   existing `AUDIO_SOURCE` component slot.
3. **Animation blending** (~1–2 weeks) — a small blend layer over the existing
   single-clip animator; unlocks real locomotion.
4. **Game UI layer** (~2–3 weeks) — retained-mode menu/HUD built on the SDF text
   renderer (or a thin immediate-mode game UI distinct from debug ImGui).
5. **Save/settings serialization** (~1–2 weeks) — start with settings + a simple
   scene/entity save format.
6. **Particle/VFX system** (~1–2 weeks first pass) — atmosphere and impact FX.
7. **Steamworks** (~week, near ship) — achievements, cloud, overlay.

Fill in genre-specific systems (scripting, AI/navmesh, networking) only as the
actual game design demands them (YAGNI).

---

## Bottom line

Cetra has the two hardest engine pieces — a high-end renderer and best-in-class
physics with a working character controller and game loop — which is the 60% most
solo engine projects never finish. Shipping a game on it is realistic. The
deciding factors are scope discipline and finishing the well-understood glue
above, not the graphics API.
