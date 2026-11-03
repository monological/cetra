# Architecture

How the engine is put together: its objects, who owns what, the frame and the render pipeline,
and the engine-level systems around them (the window, input, texture streaming, the cook, the
loading screen).

## Architecture Overview

Two layers over a scene graph. Rendering subsystems are split by owner: **PostFX** is embedded on
the `Engine`; **shadows, IBL, sky, reflection probes and particle systems** live on the `Scene`.

```
Engine (window, camera, HDR MSAA G-buffer, program cache, PostFX, matrices)
   |__ Scene (root node, lights, materials, texture pool,
   |          shadow_system, IBL + sky + reflection probe, particle_systems)
   |     |__ SceneNode (local + global transform, meshes, children,
   |     |             optional light / camera / particle_system)
   |     |     |__ Mesh (positions/normals/tangents/UV0/UV1/colors/indices,
   |     |               skinning weights, Material, AABB)

Game framework (optional, cetra/src/game/) wraps the Engine:
   Game (fixed-timestep loop) --> Jolt PhysicsWorld + EntityManager + Character controllers
```

Material feature toggles (refraction, clearcoat, sheen, specular, parallax/POM, SSS, OIT, energy
compensation) are runtime bools on the `Engine`. The game layer is `docs/game-framework.md`.

## Key Data Structures

**Every public struct's fields play one of three roles, and the header says which** (spec
11.108):
- **ENGINE-OWNED**: derived state, GL names and resources. Read them, never write; what an app may
  change among them has a function.
- **BY FUNCTION**: fields the engine must react to (validate, order, propagate, re-derive), each
  naming its function beside it.
- **SETTINGS**: everything else. Plain stores, written directly at any time, which is how the GUI
  and the config snapshot reach them.

The test is whether the library has to react, not who writes; no plain-store setter exists on any
struct. The small structs (Camera, Light, Mesh, SceneNode, Scene, Exposure, Wind, InputState, the
two controllers) are laid out under the three banners in that order. The feature-organised ones
(Engine, PostFX, ShadowSystem, Water, Material, the sky's two) keep their feature order under one
banner at the top that states the rule by kind and lists the by-function fields.

**Two things make a plain write safe**: the frame re-applies its GL state from the fields at the
frame top (wireframe's polygon mode and cull switch), and a consumer notices its own inputs
changing (the shadow pass rebuilds on a new cascade count, the water bakes compare against what
they last used). Where neither holds, the field is a function.

**Engine:** window; `ss_scale` (SSAA) and `msaa_samples`; the scene MSAA HDR FBO with the
multi-target G-buffer (see Render Pipeline); lazy resolve targets (mip'd opaque colour for
refraction, single-sample scene depth for soft particles); the camera (borrowed,
`engine_set_camera`); the scenes; the program cache (uthash, name to program); the
material-feature toggles; matrices (`view_matrix`, `projection_matrix` un-jittered,
`draw_projection` TAA-jittered, `prev_view_proj` for motion vectors); the overlays and the run's
counts as plain fields (`show_gui`, `show_fps`, `show_wireframe`, `show_xyz`,
`exit_after_frames`, `screenshot_every`) and `clear_color`; the headless flag; the embedded
`PostFX* postfx`; the bone-overlay and shadow-catcher programs; the async loader; the text
renderer. By function: the sample count and the two scales (clamp and rebuild), the typical size
(`engine_set_typical_size`, see The Typical Size), the screenshot path (owned), the callbacks and
the clock (installs).

**Scene:** the root SceneNode (`scene_set_root`); the registries, each a `scene_add_*`: lights,
particle systems (owned), cameras, materials, skeletons and animations; the texture pool;
`shadow_system`; IBL, sky, reflection probe, GI, water and rain, installed by a plain pointer write
and owned from then on; `material_textures`, the scene's `MaterialTextureArray`
(`docs/shaders.md`, Texture / Sampler Model); late-pass counters (transparent, transmissive,
OIT); the xyz program (`scene_set_xyz_program`), the one program the gizmo overlay draws every
node with, so a node created after the overlay was switched on is drawn like any other; the
origin-shift callback (`scene_set_origin_callback`). Settings: `root_transform`,
`ambient_radiance`, the skybox, ground-projection and shadow-catcher fields, the origin-shift
threshold.

**SceneNode:** parent and children; `original_transform` (local, a plain field;
`node_set_position` writes its translation column); `global_transform` (computed);
`prev_global_transform` (motion vectors); meshes (`node_add_mesh`, which uploads); optional
`light`, `camera` and `particle_system`, all borrowed, each a `node_set_*`. `serial` is
engine-owned: creation order, never reused, so anything remembering a node across frames knows
it after a free has handed its address to another. Settings: `capture_hidden`, `draw_distance`
(see Scene passes) and `capture_always`, which keeps a node in a GI capture however few pixels of
a face it spans. Nothing per node says whether the gizmo overlay draws it: the switch is
`engine->show_xyz` and the program is the scene's. `create_node` zeroes the struct.

**Mesh:** settings are the content: draw mode, positions, normals, tangents (vec4, w =
handedness; there is no bitangent stream, see `docs/shaders.md`, Vertex Attributes), UV0, UV1,
RGBA colours, indices, the Material, and the skinning input (`bone_ids`, `bone_weights`,
`skeleton`). Engine-owned is the upload's output: VAO, per-attribute VBOs and EBO; the AABB;
`is_skinned`; per-bone bind-space boxes and the wind vertex maxima; `gpu_vertex_count` and
`draw_refusal`; `upload_count`, which says an in-place edit uploaded again changed the geometry.
Two more settings:
- `lod_scale` multiplies the view's LOD bias for this mesh, so an app with meshes far larger than
  a room (silent's trees) scales theirs and leaves the engine's bias, and the GUI's slider, at 1.
- `wind_y0`/`wind_y1` are the heights a vegetation lean is measured over, read through
  `mesh_wind_range` (equal means the mesh's own box) and uploaded by `wind_upload_mesh`. **The
  meshes of one body state one range**: `tree_gen` gives a tree's wood and its leaves the wood's
  extent, since each measured from its own box leans by its own amount and the leaves slide along
  the branches.

**AABB** has five operations, `aabb_empty` / `aabb_is_empty` / `aabb_add_point` / `aabb_union` /
`aabb_expand`, `static inline` beside the typedef in `aabb.h`. That header depends on cglm alone, so
`audio.h` can give a zone a box without taking GL and the material stack through `mesh.h`.
**There are two conventions for "nothing here", and the difference matters**: an accumulator
starts empty at ±FLT_MAX, a stored geometry bound does not. `Mesh.aabb` is the second kind: five
readers consume it with no emptiness guard, so seeding it with the sentinel gives
`aabb_transform` extents of −inf and poisons the scene box, the GI fit, the probe proxy and the
camera framing. cglm's `box.h` is compiled in and layout-compatible but unused: it lacks
`add_point` and `expand`, and its transform and frustum test are different arithmetic at exactly
the edge `wind_cull_fixture` parks meshes on.

**Material:** scalar PBR (albedo, emissive and strength, metallic, roughness, AO, opacity);
`alpha_mode` (OPAQUE/MASK/BLEND) and cutoff; glTF-extension parameters (IOR, transmission,
thickness, film thickness, clearcoat, specular, sheen, parallax scale, subsurface and profile,
KHR_texture_transform, doubleSided); 13 texture pointers, each a `material_set_<x>_tex` with
retain/release; mask-array layer indices (engine-owned); the program (`material_set_program`).
The scalars are the `MATERIAL_PARAMS` table's rows, written directly or by name; there is no
per-field setter.
- `emissive_light` says whether the material's emissive is a lamp or decoration (`.cscn`:
  `emissiveLight: "light"` / `"off"`, spec 11.49). It is per material, not per mesh, because it is
  a statement about what the surface is.
- **`wind_response` scales a body's whole sway** (the lean, every branch's sway and the leaf
  flutter), so the materials of one tree take one response, or the leaves ride off their
  branches. A leaf's extra life is `wind_flutter` (default 1), mode 2's flutter about each card's
  stem, which nothing it hangs from has to follow.

**Light:** type (directional, point, spot, area); position, direction and up
(`light_set_position` / `light_set_direction` / `light_set_up`: an authored copy the walk carries
into a world copy; the setter writes both for a light on no node); colour; ambient; intensity and
`units` (`light_set_intensity_units` converts); spot cutoffs, stored as cosines of the
half-angles; area size; `cast_shadows`, and for a cached light `shadow_follow`, its kept faces
drawn again at any move. The shadow indices and the emissive source id are the engine's.
- **`specular`** is the share of the light the reflective lobes take (spec 13.30): 1 is physical,
  0 diffuse only, for a light standing in for a lit surface whose reflection SSR and the probes
  already draw. Clustered lights carry it in their own array ahead of the packed lights, four to a
  row, because every slot of a packed light is a cached light's and the block's upload ends at the
  last packed light.
- **`range`** is where the inverse-square falloff is windowed to zero, and the cull radius; 0 is
  unbounded. There is no constant/linear/quadratic attenuation.

**Camera:** position and look-at (`camera_set_position` / `camera_set_look_at`, which re-derive
the orbit); the orbit (theta, phi, distance) and the aspect, derived, written by the orbit moves
and the frame; settings: up, FOV, near/far, `fit_clip` (see The Typical Size), the ortho height
and switch, `max_distance`, the two speeds. There is no camera mode: what moves a camera is a
`camera_rig.h` rig (`docs/camera.md`).

## Memory Ownership

- **Engine** owns scenes, shader programs, PostFX, and the surface hooks an app makes (a material
  borrows one; `docs/shader-subsystems.md`, Shader hooks). A post pass borrows its program, which
  its owner registers with `engine_add_program`; a hook's two shadow programs are registered the
  same way when it is made.
- **Scene** owns the root node, materials, the texture pool, particle systems, and the shadow,
  IBL, sky and probe systems. `create_scene` makes the root (named "root"), so an app attaches
  under `scene->root_node` and never builds one; `scene_set_root` frees the root it replaces with
  its whole subtree, which is how the importer swaps in a file's own.
- **A material belongs to the first scene that registers it**, and need not be registered by
  hand: the frame's draw-list build registers every material it walks past, before anything reads
  the registry, and the importer registers what it builds before any frame. A mesh carrying
  another scene's material is refused by name rather than drawn from a registry that will free it.
- **SceneNode** owns its children (recursively) and its meshes, and borrows its light, camera and
  particle system, which the Scene frees.
- **`free_node` unlinks from its parent first**, then frees the subtree, so any node may be freed,
  not only a root, with no `node_remove_child` beforehand. `node_add_child` doubles its capacity
  rather than growing by one. Both are for the terrain quadtree, which detaches and re-attaches
  patches every frame and re-parents its whole selection when the camera crosses a band.
- **A node created mid-run has no previous frame.** The identity `prev_global_transform` it is
  created with is not one: a motion vector taken from it reports the object as having travelled
  from the world origin. The transform walk seeds `prev_valid` from `global_transform` on first
  visit. One node getting this wrong is a smear; a region's worth arriving at once is most of the
  frame.
- **TexturePool** owns all textures (cached by path). **ShaderProgram** owns its attached shaders.
  **Game** owns the physics world and entity manager, freed before the engine.
- **A model can be brought into a game's scene through a library scene** (spec 13.17, silent's
  cat). Load it as a scene of its own, hand that to the engine with `engine_add_scene` after
  `game_set_scene` (the engine owns it and the game never draws it), and move the model's nodes
  under the game's. The library keeps the skeletons and clips the animator borrows, and is freed
  with the engine, after the entity manager. Each of the import's materials is moved too:
  `scene_release_material` from the library and `scene_add_material` to the game's, which then
  frees it, textures and all; a copy through `MATERIAL_PARAMS` would leave the textures behind.
  Importing straight into an existing scene is not offered: it resets the shared texture pool's
  directory and assigns lights and cameras outright, on the path every golden goes through.

## Patterns

- **Factory pattern:** `create_*()` / `free_*()` for all objects.
- **Hash caching:** programs and textures via uthash. **Texture pooling:** one load, cached by
  file path.
- **Recursive transforms:** `global = parent->global * local`.
- **Fat G-buffer / MRT:** one forward pass writes colour, normals, motion, albedo and SSS, each
  target gated on whether a post pass consumes it.
- **SoA particle pools** with swap-remove compaction.
- **Scene-citizen subsystems:** the node borrows, the scene owns (as for lights and cameras).
- **Clustered forward light culling:** every local analytic light (point, spot, area) reaches a
  fragment through the 16x8x24 froxel grid's index list (`light_cluster.c/h`); there is no
  per-node light selection. **Directionals do not go through the grid**: they reach every fragment
  from a fixed `dirLights[MAX_DIR_LIGHTS]` array (4) in `lights_ubo.glsl`, and `pbr_frag`'s loop
  runs `k < numDir + clusterCount` over the two populations in sequence.

**Temporal accumulation** is one ping-pong primitive reused by AO, SSGI, SSR, SSS, exposure, TAA
and the composited fog layer, all gated on `taa_resolving`. `run_temporal_accum` sets
`texelSize` per call from the resolution that call runs at: the program is shared across half-res
and full-res consumers, so seeding it per program mis-sizes the neighbourhood clamp.

**The fog volume carries a second accumulator, gated the opposite way** (spec 9.5.1). Its history
is a 3D volume pair indexed by frame parity, it reprojects through its own stored previous camera
rather than the velocity buffer (a froxel is a volume of air, not a surface), and it runs whether
or not TAA does, because it is what averages the binary per-cell shadow taps; gated, the shadows
would be stair-stepped. The two filter different noise: the volume one smooths what the volume
generates, the 2D one cancels the jitter the composite inherits from the aux depth, which is why
the 2D one is pointless without TAA. **A fog cell with no history takes several samples** (spec
13.45), `fog_history_miss_samples` (4), in a branch of the inject's own `main`; see
`docs/shader-subsystems.md`, Atmosphere.

**Every history of light follows the exposure** (spec 13.20). The buffer is pre-exposed, so a
history carries the exposure of the frame that wrote it, and unscaled an exposure change reaches
it only as fast as its blend decays (stepping outdoors in silent, the fog settled as an expanding
sphere over tens of frames). Each side of a `PingPong` remembers the pre-exposure it was written at
(the fog volume: `froxel_prev_pre`), and a history is read at this frame's, which the scene pass
publishes as `postfx->pre_exposure`, times that ratio, on `.rgb` only and before any clamp or luma
weight reads it.
- TAA, TAAU, SSGI, SSR, SSS, the fog layer and the fog volume take it.
- AO, specular occlusion and contact shadows are unitless and do not.
- The cloud march and the GI volume store absolute light and need nothing.

Under TAA a step still leaves something on edges, where TAA's inverse-luma weights depend on
absolute brightness. `--no-history-rescale` (render, silent) and `--exposure-at` are the A/B.

## Render Pipeline

The engine is a **forward renderer with a multi-target HDR G-buffer**, then a screen-space post
chain. GL 4.1 has no compute, so every post pass is a fullscreen raster pass over
`post_vert.glsl`, the shared fullscreen-triangle vertex shader.

### The G-buffer

The scene FBO is MSAA, RGBA16F, multi-render-target:
- att0 HDR colour
- att1 view-space normals (+ SSR marker)
- att2 motion vectors (xy) + linear view-Z + roughness
- att3 albedo (for SSGI)
- att4 skin irradiance / SSS mask
- att5/att6 OIT accumulation + revealage (lazily allocated). The same two output locations, on a
  second lazily allocated FBO, carry the absorbance moments `b1..b4` and `b0` under
  `--oit-moments` (spec 11.17)
- att7 ambient specular, for the split spec-occ composite: R11G11B10F (positive HDR, no alpha,
  half RGBA16F's bandwidth), and allocated with the core targets rather than lazily
  (`_gbuffer_attachments` in `engine.c`), so the six core targets span draw-buffer slots 0-4 and 7

Each target is written only when a post pass that consumes it is active.

### The per-frame loop

`engine_run`, or `game_run` in the game framework:

1. **Resolution sync**: apply any scheduled render-scale switch, then rebuild the scene target and
   post chain if the derived sizes disagree with what they were last built at. The frame is
   skipped entirely (the frame clock and the `--frames` limit keep running) when the window is
   0x0 or a rebuild failed.
2. **App update callback**: input, physics, the game framework's fixed step.
3. **Origin shift**, then the **sky cycle tick**.
4. **App pre-render callback, then the engine's transform walk** (`scene_latch_prev_transforms` +
   `scene_propagate_transforms`), **then the camera rig's pose onto the camera** (a NULL rig
   writes nothing, the normal state of an app that poses its own camera), **then the camera's
   view and projection matrices**, derived from the pose it now holds (`_engine_derive_camera`).
   See the rules below.
5. **The lighting update** (`scene_update_lighting`, spec 13.24): the cached shadow tiles'
   assignment, then the GI probe captures while a resident volume is dirty, then the reflection
   probes, each once, after the volumes its box touches have swept. Each ranks its world list from
   the camera once a frame and streams from it; `scene_capture_ready` is the one statement of what
   a kept capture waits on. See `docs/lighting.md`.
6. **Shadow depth pass** (`render_shadow_depth_pass`), gated on `scene->shadow_system->enabled`,
   before the scene FBO is bound. Then the **rain's occlusion layer** (`shadow_render_rain_layer`)
   whenever the scene rains, shadows on or off: switching shadows off does not put a roof over the
   street.
7. Bind the scene MSAA FBO, set the supersampled viewport and draw buffers, clear.
8. Async texture uploads (at most five a frame), mask-array build, POM height resolve. **Headless,
   everything the app's start-up asked for is already in**: `engine_run` waits for the loader
   before frame 1 (spec 14.8), so frame N is the same picture on a busy machine as on an idle
   one. A window draws its first frame at once, and a texture asked for after the run began still
   streams; `textures-in` holds it.
9. The app's render callback, which calls `engine_render_scene`, or `engine_render_scene` itself
   when the app passed none.
10. `engine_present_frame`: the PostFX chain, then the app's **overlays** (`engine_add_overlay`, up
    to eight, lowest order first: general post-tonemap draws; the game UI adds itself at
    `UI_OVERLAY_ORDER`, over a 2D canvas, and the engine never learns what a `UISystem` is), then
    the **CRT** when it is on, then the debug GUI, a developer's overlay that belongs on top of the
    game's and outside the television. The **loading screen**, when shown, goes over all of it but
    the GUI (see The loading screen).
11. An input script's `close`, asked here unless the run is ending already, so a close taken makes
    this frame the last (see Closing); then the screenshot (headless/CI), the buffer swap, then the
    poll for events, or under idle mode a wait until something asks for the next frame; then the
    scripted pointer's frame.

### Step 4 and the transform walk

**Step 4 is where an app puts anything the frame's geometry depends on** (spec 11.96): its camera,
and any node it adds, removes or moves. Written in the render callback it is a frame late: every
shadow is drawn from last frame's transforms, every LOD level chosen from stale positions, and a
skeletal pose stepped there leaves the rig's shadow lagging its body.

**The camera is read right after the hook returns.** The engine applies the installed camera rig
and then derives the view and projection matrices, so an app writes a rig's anchor and input, or
poses the camera itself with no rig installed, and nothing else. The rig's update stays with the
app, because that is where the input is and `camera_rig.h` must not know whether it came from an
action table, a mouse or a script. Anything reading the camera inside the hook reads last frame's
pose, which is why `--trace-camera` prints from the render callback and the render app's near-clip
recompute reads the rig's own arm.

**The window is one statement wide, and each edge matters.** It lands:
- after the origin shift, which rewrites root-child locals and the whole tree's cached globals;
- after the sky cycle tick, which rewrites a sun's `original_direction` that the walk then rotates
  by the owning node;
- before the GI capture, whose probes a converged volume never re-bakes, so a capture from stale
  transforms is wrong permanently rather than for a frame.

So the update callback (step 2) is not the place.

**The walk is idempotent and the latch is not**, which is why they are two functions. A latch
(`prev := global`) run twice in a frame makes every node's previous pose its current one, zeroes
every motion vector and stops TAA reprojecting. The engine owns the latch and calls it once;
anyone may call the walk again after a late graph change. No golden can see this failure: every
golden is a single frame with TAA off and nothing moving that TAA would reproject. The
`transform` and `shadow-lag` gate groups exist for it.

### Idle mode

**Idle mode draws a frame only when something asks for one** (`Engine.idle`, off by default, spec
14.1), for an app that is mostly still, as pcb's schematic and editor are; every game and every
other app leaves it off. Five things about it are easy to get backwards.

- **A scene with anything to draw keeps the loop drawing**, every frame, as without idle mode: an
  item, an IBL, a sky, water, falling rain, a fire, a particle system. A still 3D view drawn on
  demand, and stopping TAA's accumulation, are item 3 of `docs/eda-roadmap.md`; until then idle
  mode only waits when no 3D scene is drawn. A game asks for every frame through `game_run`.
- **What asks.** In a window any wake draws (an event, a request), so no callback marks anything.
  Beyond that:
  - `engine_request_frame`, from any thread;
  - `engine_request_frame_after`, for a time. Every time asked is kept until it comes
    (`EngineWake.wakes`, 64, sorted; two within a millisecond become one at the later), so a
    caller asks once and a frame is never early. Passed wakes are dropped at the end of every
    frame's events, idle or not, since the caret asks for its blink every frame in any app;
  - `engine_end_work`, when a thread's result is in;
  - whatever in the engine wants a frame, which asks during the frame: the scene with content; the
    UI while an ease or an entrance plays, a program reads `uTime`, or a text field's caret is due
    to blink (a timed wake); the debug GUI while it takes ImGui's queued input; the loader when a
    decode is done or five delivered leave more; the loading screen. Only the input script is asked
    at the frame's end, since its frame plays after the wait.

  **A change made inside a frame is drawn by that frame**; the calls are for changes made anywhere
  else. A request posts `glfwPostEmptyEvent` only while the loop is waiting, and the two order
  their atomics so neither misses the other.
- **The wait is kept out of every clock.** The engine's clock is the wall clock less
  `time_waited`, so a timed change (a blink) is timed by `glfwGetTime` instead. **`total_frames`
  does not advance across a wait**, since a gap in it is how the shadow tiles and the fog's
  histories tell lost history.
- **Headless, a run ends at the first frame after which nothing asks for another and no work is
  out** (an `engine_begin_work` not yet ended, a decode in flight, a timed wake), and that frame is
  its screenshot; `--frames` stays the most it draws. On macOS a hidden window is sent something in
  its first tenth of a second that wakes a waiting loop; headless asks again after any wake, but
  the `idle` arms still start their late work past it.
- **Headless, a frame waits for the work out before anything else that asks** (spec 14.7), an
  input script's frames included, so what one frame began has landed by the next on any machine
  and a scripted test needs no wait of its own. A window draws what asks at once.
  - Only `engine_begin_work`'s work is waited for, never the loader's decodes, whose count falls
    only as a frame delivers them, so a wait on it would never end.
  - **`engine_end_work` stores its request before its count falls, and wakes the loop once, after
    both.** The request first keeps a run from ending before the result is drawn; a wake after the
    count has fallen is the one a waiting loop needs.
  - Waiting for work is its own state (`IDLE_WAIT_WORK`), in which a timed wake is no deadline: one
    already passed, as the code editor's "wake now" is, would spin the loop at full CPU until the
    work ended.
  - **So counted work must end on its own, with no frame drawn**: begun as it starts, never frames
    ahead, and ended by the worker, never by a frame draining what it posted. Work begun before the
    timed wake whose frame starts its thread waits for itself forever. Work that never ends is
    reported once after ten seconds, and the run goes on waiting.

### Scene passes

`engine_render_scene`, in order.

**Occlusion first** (spec 11.98, `occlusion.c/h`). Right after the draw-list build, the authored
occluder proxies are rasterised from this frame's camera into a 256x144 fixed-point CPU masked
depth buffer, and every item's `occluded` bit is settled once, by one conservative test. It is
camera-pass-only (the one `CullView` constructor initialises the switch false and one site sets
it), skipped under captures, and for a scene with no occluders the whole off state is two integer
compares. The module has no GL. It is conservative by four stated roundings, so a violated
authoring contract (a box poking out of its mesh) is the only path to a wrong pixel, and the
probe checks that too.

Then: opaque + alpha-masked (writes the G-buffer) -> skybox (procedural sky / IBL cubemap / probe
debug) -> **shadow catcher** -> refraction resolve (mip'd opaque colour, if transmissive) ->
**water** -> transparent pass (with an **OIT** accumulate sub-pass, preceded under
`--oit-moments` by a moment generation sub-pass over the same meshes) -> **particles** (soft,
using a mid-frame scene-depth resolve) -> stash `view_proj` as `prev_view_proj` for next frame's
motion vectors.

**A node's draw distance is the camera's alone** (spec 13.38, `SceneNode.draw_distance`,
inherited by every node under it that sets none). Each item's `beyond` byte is settled at the
list's build from the camera's eye, against the sphere round the item's world box that LOD reads,
as `occluded` is settled by the occlusion pass. Only the camera pass's `CullView` arms it
(`distance`, set in `_scene_pass_prepare` and not for a capture re-entering there). Three things
about it are easy to get backwards:
- **The items stay in the list.** Cutting them at build would change which items the list holds
  every time the camera moved, which is a graph change to the kept shadow faces.
- **Shadows, captures, kept tiles and the rain's layer still take what it cuts.** A tree past the
  camera's reach still shades the ground, and a reflection is not missing one for an eye standing
  far off.
- **An occluder past its distance is not rasterised.** The camera does not draw it, so it must hide
  nothing; `dd-occluder` holds that.

**The shadow catcher's position matters** (spec 11.18). It writes depth, and drawing it before the
transparent and particle passes is what lets those sort against the floor; drawn last,
translucency behind the plane composited over the shadow instead of being hidden by it. Everything
that must sort against the ground stays after it. How much translucency the floor hides still
depends on whether SSR is on, because SSR is what makes the catcher write depth across its whole
quad rather than only under the shadow.

**Water is the second surface with that property, and the two are mutually exclusive** (spec
11.32; the water itself is `docs/shader-subsystems.md`, Water).
- It sits after the refraction resolve because it samples it, and before the transparent pass
  because it writes depth: a single-layer surface in the opaque sense, not translucent geometry,
  and no part of OIT.
- It writes the aux attachment, which makes fog and aerial perspective land at the water's own
  depth rather than the bed's. The late pass writes no aux, which is why a translucent surface is
  still fogged wrong and water is not.
- **The catcher is suppressed whenever water is active.** The catcher quad sits at y = 0, where a
  water plane usually is, and the coplanar depth tie resolves per pixel into a wandering band that
  reads as a hole in the water. Water is the more specific ground plane, so it wins.
- **Water is not SSR-traced, deliberately.** SSR shades only surfaces marked with a negative
  normals-buffer alpha, so `--no-ssr` changes nothing on a water frame. Water's reflection is its
  own split-sum lookup inside `water_frag`, which selects its mip from the per-texel roughness.

**The SSR marker is a range, and every test of it is a range test** (spec 13.9): (-1, 0) is the
catcher, its magnitude the edge falloff, and below -1 is wet ground, as `-(1 + film)`. Every reader
asks `include/ssr_marker.glsl`; a sign test anywhere (the tonemap's AO guard, `spec_occ`) hands wet
ground the catcher's treatment. How wet ground's reflection replaces its share of the environment,
before TAA, and what it borrows at a turn's leading edge, is `docs/shader-subsystems.md`, Rain.

### The PostFX chain

`postfx_run`: MSAA resolve -> OIT composite -> G-buffer resolves -> contact shadows -> GTAO + SSGI
sweep -> split spec-occ composite -> the TAA seam (TAA at full scale; under `--render-scale` the
TAAU upscaling resolve, which brings the render-res frame to post res) -> SSGI denoise -> SSR
(hi-z) -> SSGI composite -> atmosphere (fog + aerial) -> SSS -> motion blur -> the late draw (an
app's late-draw materials, then fire, then rain's streaks and splashes) -> **an app's passes before
DoF** -> DoF -> **an app's passes before bloom** -> auto-exposure metering -> bloom (+ lens flare)
-> diffraction glare -> local exposure (its grid) -> tonemap + finishing (sharpen / colour grade /
vignette / gamma / grain) -> **an app's passes after the tone map** -> the app's overlay -> the CRT
or the present pass -> GUI.

The three app points are `docs/shader-subsystems.md`, Shader hooks. Everything before the seam runs
at render res (post size times `render_scale`, 1 by default), everything after composites onto a
post-res canvas; at scale 1 the two are the same buffer. Debug render modes take a passthrough
blit and skip the whole chain.

**Defaults.** On: bloom, GTAO and specular occlusion, SSR, auto-exposure, the NEUTRAL tonemap, the
normals G-buffer, OIT with moment weighting. Off (present, lazily allocated): SSGI, fog, DoF,
motion blur, diffraction glare, local exposure, the CRT. SSS and TAA are the two exceptions below.

**Diffraction glare** (`glare.c`, specs 13.4 and 13.5) is off unless asked for: `post.glare` in a
`.cscn`, `postfx->glare_enabled` in an app, `--glare` on render, forest and gametest; `apps/tree`
turns it on for its low sun. **It moves light rather than adding it**: the tonemap takes the moved
light out of the pixel it came from, by `glare_threshold.glsl`, the rule the source pass shares.
The rest is `docs/shader-subsystems.md`, Diffraction glare.

**Local exposure** (`local_exposure.c`, spec 13.19) is an exposure per pixel on top of the
camera's, which still sets the level; off by default (`post.local_exposure`,
`postfx->local_exposure_enabled`), and the identity at its defaults. Two rules cross into the rest
of the chain:
- **It is applied in the tonemap, to the composite with bloom, after the Purkinje shift reads the
  light as it is**, and once per pixel: the sharpen's taps reuse the centre's factor, as they reuse
  AO's.
- **Its strength is relative to the camera's middle grey, not to the room.** The grid is built from
  the exposed frame, centred on `exposure.key`, so it is scale-invariant (a `SCALE_GATES` row) and a
  high key leaves less headroom. Measure the range with `--le-probe` before tuning by eye.

The rest is `docs/shader-subsystems.md`, Local exposure.

**The CRT** (`crt.c`, spec 13.28) is off unless asked for (`postfx->crt_enabled`, `--crt`). With
it on, the post chain, the debug tiles and the app's overlay draw into a window-sized RGBA16F
picture (`postfx_picture_fbo`, PostFX's own, which an after-tonemap pass also draws on), which
`postfx_present_picture` shows through the CRT; the debug GUI draws after it. So the overlay is on
the television and the GUI is not. The picture must be exactly window-sized, since the UI scissors
in window pixels. **Dither stays on the last write**: the tonemap dithers only when it writes the
window, and under the CRT the CRT's pass dithers. Which pass writes the window (the CRT, the
present pass, or the tonemap) is decided once a frame by `postfx_run`. The rest is
`docs/shader-subsystems.md`, The CRT.

**SSS is on but inert.** `engine->sss_enabled` is true, but attachment 4 goes unwritten unless a
material carries `subsurface > 0`, and the pass is skipped when it is unwritten. So `--no-sss` on a
scene with no subsurface material changes nothing because nothing was running, not because the
flag failed; remember that when bisecting.

**OIT is on by default and only costs scenes that use it.** The whole path is gated on
`scene->oit_mesh_count > 0` (ALPHA_BLEND and not transmissive), so a scene with no translucent mesh
allocates nothing, runs no extra pass, and is byte-identical to `--no-oit`. A scene that has one
pays two extra traversals of those meshes and about 50 MB of MSAA targets at 640x400 on a Retina
framebuffer, plus 244 MB of fp32 moment targets at 800x500. `--no-oit` is the unsorted late pass;
`--no-oit-moments` keeps OIT on the McGuire depth curve.

### TAA and the sample count

**The struct default is not what you run.** `taa_enabled` is false on PostFX, but the render app
turns it on for every windowed session (`taa_policy = !args.headless || args.force_taa` in
`apps/render/src/render.c`). Interactive runs have TAA on; headless runs have it off unless
`--taa` / `--headless-jitter`. Check that call site before reasoning about temporal behaviour from
the struct default: anything gated on `taa_resolving` is live in the window and dead in a golden,
so an artifact can be invisible in one and obvious in the other.

**The same policy drops the sample count to 1, and both halves are `EngineConfig` fields**
(`msaa_samples` and `taa`). `create_engine` reads them in the right order: the count before the
scene target is built, the TAA switch after the post chain exists. After creation the switch is the
plain field `postfx->taa_enabled`. Apps that keep four samples do so by decision (spec 11.103):
- `apps/forest`, `apps/tree` (always) and `apps/gametest` run one sample with TAA, like render.
- `apps/spores` keeps four because its particles write no motion vector, and TAA turns 32,000 motes
  into dashes.
- `apps/shapes` keeps four because multisampling is what 2D line art wants and nothing in it moves.
- `apps/sprites` and `apps/network` keep four for the same two reasons, particles and line art, and
  run under the 2D preset, which has TAA off regardless. pcb, in its own repository, does the same.
- `apps/splash` draws to the default framebuffer and never binds the engine's.

**One sample is the only path an alpha-tested material gets**, and that is measured (spec 11.102):
four samples arm alpha-to-coverage, which holds a cutout's fractional coverage where a binary test
under a clamped history cannot, but cost +108% of forest's opaque row and +296% of a dense cutout
scene's frame to recover nothing the eye finds. The one-sample path carries 11.101's jitter
instead.

**A 2D app's defaults are one call**, `engine_set_2d_preset`: bloom, glare, local exposure, GTAO,
SSR, vignette, dither, TAA and shadows off, exposure pinned at unity, the `linear` tone curve (the
identity with the display encode, which passthrough is not), and a white ambient radiance on the
scene, under which an albedo is the colour on screen with no light at all. It runs after
`create_engine`, since the post chain has to exist, and takes the scene for the ambient and shadow
half. It leaves the overlays alone; a 2D app that wants the FPS counter writes `show_fps`. Through
the plain defaults a flat red square comes out pink with a halo: the light needed to reach albedo
through the PBR path pushes red past white, the neutral curve desaturates it, and bloom draws the
glow.

**The jitter is projection-dependent**: it rides the translation element under an orthographic
camera and the z element under a perspective one, because only the second is divided back out.
`--ortho` on the render app is the only way to reach that path headless, and `taa-ortho` guards it.

**Never assume `clip.w = -z`** (spec 11.104). Every site that depends on the projection branches
on `projection[2][3]`: 0 under `glm_ortho`, -1 under `glm_perspective`, an element the jitter
never touches.
- The shader helpers live in `include/depth.glsl`: `viewZFromNdcZ`, `viewPosFromLinZ`,
  `screenLengthAt`, `viewRayVecAt`, `clipWAt`, `projectionIsOrtho`, and `viewDirToCamera` /
  `worldDirToCamera` for the view vector, which under a parallel projection is one direction for
  the whole frame (the view matrix's third rotation row, not negated).
- **`nearPlaneDist()` is the one near-plane recovery.** `P32/(P22-1)` written out by hand is wrong
  under ortho.
- On the CPU, anything that measures from the eye point takes the same branch: LOD by view height,
  the occluder eye as a homogeneous point, the cascade fit as a box.
- **A branch around an unchanged GLSL divide is not byte-identical on this driver** (an `if` whose
  new arm was never taken moved 130 px of water), so the depth inverse is one coefficient form,
  not a wrapped branch.
- The `ortho` gate group and the `ortho_shadow` golden see the orthographic path; every other
  golden is perspective and proves only that it did not move.

**TAAU** (`--render-scale <f>`, 0.5-1, spec 11.7) rides on TAA but never on the auto-enable. Three
ways in: the flag, the GUI "Render Scale" slider, and the `--render-scale-at` diagnostic schedule.
Headless needs `--taa --headless-jitter` for any of them, or the app refuses and renders at full
res. Changing it at runtime rebuilds every render target at the next frame top and resets the
temporal histories (spec 11.8): expect a fraction of a second of re-converging. At 0.67 it roughly
halves frame time (44 to 22 ms at Retina fullscreen); `--sharpen` is the crispness lever when
scaled.

## The Typical Size

**`Engine.typical_size` is the size, in metres, of what the scene holds** (spec 13.57): 1 for
people and rooms, the default, and 0.001 for a circuit board. It is `EngineConfig.typical_size` at
creation and `engine_set_typical_size` after; a `.cscn`'s `typicalSize`, the render app's
`--typical-size`, the snapshot row `engine.typical_size` and the GUI's "Typical Size (m)" all
reach it. Every length in the engine is one of three kinds, and the kind decides what the size
does to it:
- **Physical: what the scene states.** Positions, a light's range, size and body, an SSS profile's
  radius, a material's thickness, fog density per metre. Never rewritten, since a board states them
  in metres at its own size.
- **Tuning: what the engine chose for itself**, every one of them tuned on rooms. Among them:
  GTAO's radius, SSR's thickness and reach, the contact march's start, the depth tolerances of the
  AO blur and the a-trous filters, a soft particle's fade, the cascade and GI capture near planes,
  and the streaming margins. Each is its metre value times the size, read where it is used:
  `engine->typical_size` in C, `ViewParams.typicalSize` in GLSL. PostFX's settable ones are written
  by `postfx_reset_lengths` when the size is set, so an app sets the size first and its own values
  after. The camera rig follows the size too.
- **Fitted: worked out each frame**, which is a `fit_clip` camera's near and far.

**A camera's planes are its owner's, or the fit's.** `Camera.fit_clip` fits them each frame,
inside `_engine_derive_camera` on the pose just written:
- **What it fits to:** the mesh boxes in front of the eye (`scene_for_each_mesh_box`), plus the
  shadow catcher's quad or a bounded water's rectangle when one draws, each box from its owner
  (`render_shadow_catcher_box`, `water_surface_box`).
- **The planes:** far is 1.1 times the farthest depth. Near is 0.9 times the larger of the nearest
  depth and the nearest distance times the frustum corner's cosine. The second bound is what fits a
  close-up, where a box reaching behind the eye takes the first to zero; past the farthest depth it
  means no box reaches the frustum, and is dropped, or near would pass far. Near is never under far
  / `CAMERA_CLIP_RATIO_MAX` (1e4).
- **A sea with no bounds is a square `SEA_REACH` (1000 typical sizes) out from under the eye**, the
  reach a camera has by default.
- **The boxes are at rest**, so wind, a skinned pose, the terrain's morph, a hook's offset or a
  wave's crest can carry geometry past a fitted plane. Particles, fire and rain are not counted.

Four things about it are easy to get backwards.
- **The size writes no camera plane, no light and no SSS profile.** At creation no camera is
  installed yet, and on a runtime change writing the planes would overwrite the app's own. SSS slot
  0's radius is an authored profile once an app replaces the table.
- **A ranking keeps metres, and its margin is times the size.** The cached tiles, the GI volumes and
  the reflection probes rank by distance in metres against `MARGIN * typical_size`. Dividing the
  distance instead would put it in a unit nothing else in the engine uses.
- **A clip w is a length only under perspective.** Under a parallel projection it is the number 1,
  so a floor on it is `1e-4 * (projectionIsOrtho() ? 1.0 : typicalSize)`. Scaled unconditionally,
  every contact-shadow ray under a parallel projection stops at a typical size of 1e4.
- **`scale-twin` holds it, and cannot see the classification.** Every corpus scene renders at 0 px
  against itself shrunk 1024 times, which is exact and not a tolerance. But a physical length
  wrongly multiplied by the size scales with the scene too and twins exactly, so the classification
  has to be checked by reading. A scene already in millimetres is twinned by growing it, since
  shrunk its depths fall under fp16's normal range. `docs/verification.md`, The scale twin, has the
  argument.

## The window

`engine.c/h` owns the window, the render loop, the G-buffer and MSAA targets, the program cache and
the embedded PostFX.

**Window modes** (spec 12.15): windowed, exclusive fullscreen and borderless, each at the monitor's
current video mode, so no display's resolution is ever changed and there is nothing to restore
after a crash; `--render-scale` stays the performance lever. Four things about
`engine_set_window_mode` are easy to get backwards.
- **It writes none of `win_width`/`win_height`/`fb_width`/`fb_height`.** It calls GLFW, the
  framebuffer-size callback (their only writer after init) fires, and the frame top rebuilds. So the
  switch adds no resize path; setting them directly would make two writers.
- **It is idempotent.** `settings_apply` pushes every value on every edit, so holding a volume
  slider reaches it once a frame, and replaying a windowed placement would move the window to a
  rectangle latched long before. So the windowed rectangle is kept current while the window is
  windowed, a replay lands where it already is, and it returns early when the mode and the monitor
  are both unchanged.
- **It is refused where there is no window.** A frame whose size came from whatever display the
  machine has would not be reproducible. That is why `engine_window_placement` is split out as a
  pure function of (mode, monitor rect, saved rect): it is the only half an arm can reach.
- **Creation goes through the same switch.** The window is made hidden, moved, and shown only at
  the end of engine creation (spec 13.39), so a fullscreen launch never flashes a window first and
  no launch shows one standing blank while the programs and the post chain build.

**Exclusive fullscreen is the one mode not shown by `glfwShowWindow`** (spec 13.55). GLFW will not
show a window already on a monitor, so a launch into it takes the monitor while still hidden and is
shown by focusing it, which is how GLFW's own creation shows one. Shown with `glfwShowWindow` it is
never seen; shown borderless and switched after, it loses focus in the switch, and a fullscreen
window that loses focus is minimized. It waits borderless until then, because taking a monitor on
Windows and X11 shows the window early. Creation asks the window system whether the window is
visible, and says so by name when it is not.

`engine_monitor_index` is the one statement of what a stored display name means (NULL, `""` and any
unrecognised name all give the primary), and `engine_monitor_names` is the list, so no app keeps
its own array, cap or unnamed-display fallback.

## Input

**Typed input** (spec 14.2): keys, their repeats and characters go into one ordered list on
`InputState` (`key_events`, 64 a frame, an overflow warned once), cleared with the other latches
before each poll, since 'a' then Backspace is not Backspace then 'a'. Nothing is recorded while
ImGui wants the keyboard; `_engine_key` and `_engine_char` are the halves past ImGui that the input
script shares.

**The pointer** is recorded the same way (spec 14.3): where it last moved or was pressed or
released (`pointer_fb_x`/`_y`, framebuffer pixels, the script's moves included, so a click is where
it lands whatever moves were missed), the left button's press and release latched until the frame
ends (`pointer_pressed`/`_released`), and the modifiers held at the press (`press_mods`), all
cleared in `_engine_clear_latches`. `ui_input_read` hands the whole of it to the UI, so no app
gathers its own. `engine_fb_to_points` is the one conversion from those pixels to window points,
the inverse of the engine's own, used by `ui_input_read` and by an app's own panning. The wheel is
`engine->input.scroll_dx/dy`, one frame's delta.

**The clipboard** is `engine_clipboard_get`/`_set`: GLFW's in a window, a string the engine owns
headless, so a test never touches the user's.

**The input script** (`engine_set_input_script`) moves and presses the pointer and types:
`type=TEXT` (UTF-8, `\s` a space) and `key=NAME` with `+` modifiers. `mod` is the platform's
shortcut modifier, Cmd on macOS and Ctrl elsewhere (`INPUT_MOD_SHORTCUT` in `input.h`, beside
`INPUT_MOD_WORD`, bits of `InputKeyEvent.mods` held to GLFW's by a static assert), so a script is
portable. A scripted key is a press and a release. The grammar is in `docs/cli-reference.md`.

### Closing

**A close the app may refuse** (spec 14.7, `engine_set_close_hook`, an `EngineCloseFunc`) is asked
at the window's close request (its close button, Alt+F4, and on macOS the menu's Quit with its
Cmd+Q, which GLFW sends as one request) and at the input script's `close`, once, as the first frame
of its range ends, so a close taken makes that frame the run's last and its screenshot. Refused,
the flag GLFW set is cleared and a frame asked for, in which the app says why. **A run already
ending asks nothing**: the frame limit, idle mode's end and the app itself (`game_quit`) set the
same flag, so the engine notes whether it was set as the window's events are read
(`Engine.ending`), and a close then is not asked, or a refusal would undo the run's end.

## The loading screen

`loading_screen.c/h` (spec 13.34): CETRA over ENGINE as a 1970s station ident on a worn tape, on a
CRT of its own. The letters turn in, ENGINE lights, a light runs along a striped rule to a sparkle,
the tape glitches, and it holds under a teletext LOADING sign, glitching now and then, until it is
let go. `Engine.loading_palette` picks one of four palettes. Six calls:
- `engine_show_loading_screen`;
- `engine_draw_loading_screen`: draw it now and answer the window's events, at most once a
  refresh, for an app's init before `engine_run`; nothing headless;
- `engine_loading_screen_ready`: the sign becomes PLAY and the screen waits;
- `engine_loading_screen_prompting`: PLAY is up, so a press means play;
- `engine_hide_loading_screen` and `engine_loading_screen_shown`.

PLAY and the switch-off are times on the screen's own clock, set when asked for and never before
the ident has played, and the screen is freed whole once it has switched off. Its clock is the wall
clock windowed, capped at a tenth of a second a draw, and a frame's fixed step headless, so a
headless capture of it is frame-exact. The render app previews it (`--loading-screen`). Three
things about it are easy to get backwards.
- **Hiding is a request.** It lifts only once its ident has played, glitch included, then switches
  off; an app starts its own fade on `shown` going false. Ready is not hiding: the engine does not
  know the game's controls, so the app hides it at a press of its own choosing, read only while it
  is prompting, and an app with no prompt hides it straight away.
- **A frame it covers draws no picture of its own**: step 9 settles the scene's derived panels and
  variants (which the captures read) and draws no camera pass, step 10 runs no post chain and no
  GUI, and steps 5 to 8 (the captures, every upload) go on. Whether a frame is covered is decided by
  the frame's own last draw, so the scene pass and the present cannot disagree within one frame.
- **It moves inside a long frame.** `capture_budget_take` draws it between capture units,
  `engine_pbr_variant` after each new compile, and `draw_tile_cell` after every cached shadow tile
  face, all through `loading_screen_tick`. A tick answers no window events (one could resize the
  targets the frame is drawing into) and puts back what it bound, including the colour mask a
  depth-only capture may have off. A draw whose targets cannot be made puts the screen away rather
  than leaving it up.

**How long it stood still is measured** (spec 13.39): `engine_loading_screen_take_longest_wait` is
the longest time between two chances to draw since it was last asked. A chance is a call to draw
it, a tick, or a frame, drawn or not, and it is counted headless too, so a headless run says what a
window would freeze for.

## Texture streaming

`async_loader.c/h`: a pthread pool sized `get_cpu_cores() - 2`, clamped to [2, 8]. Only the decode
runs on the pool; the mips, the compression, the cook and the upload stay on the main thread. An
app reaches it through `engine_load_material_texture` (a material's texture from a file, set when
it is in), `engine_load_texture` (any texture from a file, handed to a callback) and
`engine_finish_texture_loads` (every load uploaded now, the loading screen drawn between them).
- **A file is resolved once, at submit** (`texture_pool_resolve`, shared with
  `texture_load_file`), and the resolved path is its one key: the pool's, the in-flight registry's,
  and the file the worker reads. A worker resolving again would miss files already pooled and read
  the pool's directory string while the main thread can replace it.
- **Each load carries its pool**, and the drain publishes it there; `async_loader_process_pending`
  takes no pool. A drain publishing into the current scene's pool would put another scene's load
  there and strand its in-flight entry.

## The derived-data cook

`cook.c/h` (spec 11.99) is a transparent content-addressed cache over the deterministic startup
bakes it wraps, after Unreal's DDC, plus the `--cook` pre-warm verb on forest and render.
- **The key is the identity**: input bytes, recipe version, and library version where a library
  owns the byte format. So a stale artefact is unfindable rather than detected. A miss always bakes
  live; a corrupt `.cca` is refused by name against a payload hash and treated as a miss.
- Process-global behind `cook_init`, main-thread-only. `cooked/` is gitignored at the repo root;
  `CETRA_COOK_DIR` and `CETRA_NO_COOK` are the environment levers.
- **Never fold a worker count into a key, and never measure a bake without `--no-cook`.** Gates
  and goldens isolate onto per-run cache directories automatically.
- What may not be cooked is stated in the header's charter (GPU resamples, GL handles, the
  scatter), and so are the two captures that may (spec 13.42): a GI volume's opening sweep and a
  reflection probe's column, keyed by the whole scene they see at rest rather than by a bake's
  arguments (`docs/lighting.md`). `cook_enabled` is for a site whose key costs a walk of the scene,
  which asks before folding it.

`physics_cook.h/.cpp` serialises and restores Jolt shapes behind a C header, in a C++ TU because
JoltC binds none of Jolt's serialisation. It exports `JPH_VERSION_ID` as a mandatory cook-key
axis. **Its stream classes carry istream EOF semantics**: Jolt checks `IsEOF()` after a stream's
last field, so a positional implementation refuses every restore.

## The config snapshot

`config_snapshot.c/h` (spec 11.71) dumps and restores the live session as JSON, about 230
settings; how to use it is `docs/verification.md`, Reproducing a session.
- **One descriptor table, walked in both directions**, so the writer and the reader cannot list
  different fields.
- Materials ride `MATERIAL_PARAMS` rather than rows of their own, so a property added there is
  carried for free.
- **What it omits matters as much as what it carries**: GPU handles, the lazy-allocation guards,
  the seven per-frame published blocks and the nine temporal histories are not configuration, and
  restoring one corrupts the frame rather than reproducing it.

`json_util.h/.c` holds the cJSON shapes more than one reader needs: `json_string_or`,
`json_int_or`, and `json_add_float`, a float at `%.9g` through a raw item, the exact round-trip
width for binary32 (cJSON's own `%1.15g` is sized for a double and writes `0.3f` as
`0.300000011920929`). `cscene.c` deliberately does not use it: its `get_float`/`get_bool` are
bool-returning presence tests, because a `.cscn` leaves an unspecified field at its engine
default, which is a different contract.

## Cluster LODs

`cluster.h` and `cluster_build.cpp` (spec 11.63) are Nanite's CPU half on GL 4.1: meshoptimizer's
own `demo/clusterlod.h` behind a C API, in a C++ TU. About 128-triangle clusters are grouped, each
group simplified with its boundary locked, re-split, and re-grouped differently so the locked seams
move.
- **Cracks are structurally impossible**: every cluster at every level indexes the original vertex
  buffer, since meshoptimizer only drops vertices and never introduces or moves one. That holds
  unconditionally. `simplify_permissive = false` buys something narrower: it refuses collapses
  across UV and normal seams, which a prop with seams needs.
- **The cut is quantised by distance band**, which keeps instancing alive: a per-instance cut means
  two trees at different distances can never share a draw.
- **On triangles it is parity with `lod.c`'s chain, not a win** (−2.7%, −0.1%, −1.4% at three
  framings): boundary locking costs about what error-driven selection gains. What it delivers is
  the seal.
