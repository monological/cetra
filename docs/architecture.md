# Architecture

How the engine is put together: its objects, who owns what, the frame and the render pipeline.

## Architecture Overview

Two layers over the scene graph. Rendering subsystems are split by owner: **PostFX**
is embedded on the `Engine`; **shadows, IBL, sky, reflection probe, and particle
systems** live on the `Scene`.

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

Material feature toggles (refraction, clearcoat, sheen, specular, parallax/POM,
SSS, OIT, energy compensation) are runtime bools on the `Engine`.

## Key Data Structures

**Every public struct's fields play one of three roles, and the header says which** (spec
11.108). Under **ENGINE-OWNED** sit derived state, GL names and resources: read them, never
write; what an app may change among them has a function. Under **BY FUNCTION** sit the fields
the engine must react to -- validate, order, propagate, re-derive -- each naming its function
beside it. Under **SETTINGS** sits everything else: plain stores, written directly at any time,
which is how the GUI and the config snapshot already reach them. The test is whether the LIBRARY
has to react, not who writes: a setter that only stored a value was ceremony and was deleted, and
no plain-store setter survives on any struct. The small structs (Camera, Light, Mesh, SceneNode,
Scene, Exposure, Wind, InputState, the two controllers) are laid out in that order under the three
banners; the feature-organised ones (Engine, PostFX, ShadowSystem, Water, Material, the sky's two)
keep their feature order under one banner at the top that states the rule by kind and lists the
by-function fields. **Two things make a plain write safe where a setter used to react**: the frame
re-applies its GL state from the fields at the frame top (wireframe's polygon mode and cull
switch), and a consumer notices its own inputs changing (the shadow pass rebuilds on a new
cascade count, the water bakes compare against what they last used). Where neither holds, the
field is a function.

**Engine:** window; SSAA scale + MSAA sample count; scene MSAA HDR FBO with the
multi-target G-buffer (see Render Pipeline); lazy resolve targets (mip'd opaque color
for refraction, single-sample scene depth for soft particles); camera + mode; scenes
array; program cache (uthash name->program); material-feature toggles
(energy comp / refraction / clearcoat / specular / sheen / parallax / SSS / OIT);
matrices (`view_matrix`, `projection_matrix` un-jittered, `draw_projection` TAA-jittered,
`prev_view_proj` for motion vectors); the overlays and the run's counts (plain fields since
11.108: `show_gui`, `show_fps`, `show_wireframe`, `show_xyz`, `exit_after_frames`,
`screenshot_every`) and `clear_color` (11.107); headless flag; embedded
`PostFX* postfx`; bone-overlay + shadow-catcher programs; async loader; text renderer. By
function: the sample count and the two scales (clamp + rebuild), the typical size
(`engine_set_typical_size`, spec 13.57: PostFX's tuning lengths and the camera rig follow it), the
screenshot path (owned), the callbacks and the clock (installs).

**Scene:** root SceneNode (`scene_set_root`); lights; **particle_systems (owned)**; cameras;
materials; skeletons + animations -- the registries, each an `scene_add_*`; texture pool;
`shadow_system`; IBL + sky + reflection probe + GI + water + rain, installed by a plain pointer write and
owned from then on; `material_textures` (the `MaterialTextureArray` — NOT a mask array, see the
rename above); late-pass counters (transparent/transmissive/OIT); the xyz program
(`scene_set_xyz_program`, the ONE program the gizmo overlay draws every node with -- nothing is
propagated into nodes since 11.108, so a node created after the overlay was switched on is drawn
like any other); the origin-shift callback (`scene_set_origin_callback`); settings:
`root_transform`, `ambient_radiance`, the skybox / ground-projection / shadow-catcher fields, the
origin-shift threshold.

**SceneNode:** parent/children; `original_transform` (local, a plain field; `node_set_position`
for its translation column); `global_transform` (computed); `prev_global_transform` (motion
vectors); meshes (`node_add_mesh`, which uploads); optional `light` / `camera` /
**`particle_system`** (all borrowed, each a `node_set_*`); `serial` (spec 13.38, ENGINE-OWNED: creation
order, never reused, so anything remembering a node across frames knows it after a free has
handed its address to another); settings `capture_hidden` and `draw_distance` (spec 13.38, see
the scene passes), and `capture_always` (spec 13.42), which keeps a node in a GI capture however
few pixels of a face it spans. Nothing per node says whether the gizmo overlay draws it: the switch is
`engine->show_xyz` and the program the scene's. **`create_node` zeroes the struct since 13.38**:
it used to `malloc` and set fields one by one, and never set `capture_hidden`.

**Mesh:** the content under SETTINGS -- draw mode, positions/normals/tangents (vec4, w = handedness — there is NO bitangent stream)/UV0/UV1/
colors(RGBA)/indices, Material, the skinning input (`bone_ids`, `bone_weights`, `skeleton`);
the upload's output under ENGINE-OWNED -- VAO + per-attribute VBOs + EBO; AABB; `is_skinned`;
per-bone bind-space boxes and the wind vertex maxima (spec 11.53); `gpu_vertex_count` and
`draw_refusal` (spec 11.107); `upload_count` (spec 13.38, what says an in-place edit uploaded
again changed the geometry). Two settings since 13.38:
- `lod_scale` multiplies the view's LOD bias for this mesh, so an app with meshes far larger
  than a room -- silent's trees -- scales theirs and leaves the engine's bias, and the GUI's
  slider, at 1.
- `wind_y0`/`wind_y1` are the heights a vegetation lean is measured over, read through
  `mesh_wind_range` (equal = the mesh's own box) and uploaded by `wind_upload_mesh`. **The meshes
  of one body state one range**: `tree_gen` gives a tree's wood and its leaves the wood's
  extent, since each measured from its own box leaned by its own amount and the leaves slid
  along the branches. It replaced two unreferenced "anchor" vertices that also enlarged the box
  LOD reads.

**`AABB` has five operations since 11.54** — `aabb_empty` / `aabb_is_empty` / `aabb_add_point` /
`aabb_union` / `aabb_expand`, `static inline` beside the typedef in `aabb.h` -- its own header,
on cglm alone, since spec 13.33 gave an audio zone a box and `audio.h` would otherwise have taken
GL and the material stack through `mesh.h`, which includes it. Before them every
consumer hand-rolled all five and the empty sentinel was a convention two files shared by hand, which
had already cost a redundant flag beside an uninitialised box. **There are TWO conventions for
"nothing here" and the difference is load-bearing**: an ACCUMULATOR starts empty at ±FLT_MAX, a
STORED GEOMETRY BOUND does not. `Mesh.aabb` is the second kind — five readers consume it with no
emptiness guard, so seeding it with the sentinel gives `aabb_transform` extents of −inf and poisons
the scene box, the GI fit, the probe proxy and the camera framing. cglm's `box.h` is compiled in,
layout-compatible and used **nowhere**: it lacks `add_point` and `expand`, and its transform and
frustum test are different arithmetic at exactly the edge `wind_cull_fixture` parks meshes on.

**Material:** scalar PBR (albedo, emissive + strength, metallic, roughness, AO,
opacity); `alpha_mode` (OPAQUE/MASK/BLEND) + cutoff; glTF-extension params (IOR,
transmission, thickness, film thickness, clearcoat, specular, sheen, parallax scale,
subsurface + profile, KHR_texture_transform, doubleSided); 13 texture pointers (each a
`material_set_<x>_tex`, retain/release); mask-array layer indices (engine-owned); ShaderProgram
pointer (`material_set_program`); `emissive_light` (spec 11.49 — whether
this material's emissive is a LAMP or decoration, authored in a `.cscn` as
`emissiveLight: "light"` / `"off"`. Per-MATERIAL and not per-mesh, because it is a
statement about what the surface IS). The scalars are the `MATERIAL_PARAMS` table's rows,
written directly or by name; there is no per-field setter. **`wind_response` scales a body's whole
sway -- the lean, every branch's sway and the leaf flutter** -- so the materials of one tree take
ONE response, or the leaves ride off their branches; a leaf's extra life is `wind_flutter`
(spec 13.38, default 1), mode 2's flutter about each card's stem, which nothing it hangs from has
to follow.

**Light:** type (directional/point/spot/area); position, direction and up (`light_set_position`
/ `light_set_direction` / `light_set_up`: an authored copy the walk carries into a world copy, and
the setter writes both for a light on no node); color; **`specular`**, the share of the light the
reflective lobes take (spec 13.30), 1 physical and 0 diffuse only -- for a light standing in for a
lit surface whose reflection SSR and the probes already draw. It was a `vec3` nothing read until
then. Clustered lights carry it in their own array ahead of the packed lights, four to a row,
because every slot of a packed light is a cached light's and the block's upload ends at the last
packed light; ambient; intensity + `units` (`light_set_intensity_units` converts); **`range`** — where the
inverse-square falloff is windowed to zero and the cull radius, 0 = unbounded. **There is no
constant/linear/quadratic attenuation triple** and never has been in the photometric era; spot
cutoffs (stored as COSINES of the half-angles, not radians); area size; `cast_shadows`, and for
a cached light `shadow_follow`, its kept faces drawn again at any move; the
shadow indices and the emissive source id are the engine's.

**Camera:** position and look-at (`camera_set_position` / `camera_set_look_at`, which re-derive
the orbit); the orbit (theta/phi/distance) and the aspect, DERIVED, written by the orbit moves
and the frame; settings: up, FOV, near/far, `fit_clip` (spec 13.57: near and far fitted each
frame, see The Typical Size), the ortho height and switch, `max_distance`, the two speeds.

## Memory Ownership

- Engine owns scenes, shader programs, PostFX, and the surface hooks an app makes (spec 13.29;
  a material borrows one). A post pass borrows its program, which its owner registers with
  `engine_add_program`; a hook's two shadow programs are registered the same way when it is made.
- Scene owns root node, materials (shared), texture pool, particle systems, shadow /
  IBL / sky / probe. **`create_scene` makes the root** (named "root"; spec 11.107), so an
  app attaches under `scene->root_node` and never builds one; `scene_set_root` frees the root
  it replaces with its whole subtree, which is how the importer swaps in a file's own.
- **A material belongs to the first scene that registers it**, and it need not be registered
  by hand: the frame's draw-list build registers every material it walks past, before anything
  reads the registry (the build is the walk with the scene in hand, which a SceneNode is not),
  and the importer registers what it builds before any frame. A mesh carrying another scene's
  material is refused by name rather than drawn from a registry that will free it.
- SceneNode owns children (recursive) and meshes; **borrows** light / camera /
  particle_system (freed by the Scene, not the node).
- **`free_node` UNLINKS from its parent first**, then frees the subtree, so any node may be freed
  and not only a root — a caller does not need `node_remove_child` beforehand. It did not always:
  the old contract was "free_node does not unlink", which held while every caller freed whole trees
  from the root and became a dangling pointer in the parent's array the moment a quadtree started
  detaching and re-attaching patches every frame. `node_add_child` doubles its capacity rather than
  growing by one, for the same reason: a quadtree re-parents its whole selection when the camera
  crosses a band.
- **A node created mid-run has no previous frame**, and the identity `prev_global_transform` it was
  created with is not one — a motion vector taken from it reports the object as having travelled from
  the world origin. `prev_valid` is what the transform walk seeds from `global_transform` on first
  visit. One node doing that is a smear; a region's worth arriving at once is most of the frame.
- TexturePool owns all textures (cached).
- ShaderProgram owns attached shaders.
- Game owns the physics world + entity manager (freed before the engine).
- **A model can be brought into a game's scene through a LIBRARY scene** (spec 13.17, silent's
  cat): load it as a scene of its own, hand that to the engine with `engine_add_scene` AFTER
  `game_set_scene` -- the engine owns it and the game never draws it -- and move the model's nodes
  under the game's. The library keeps the skeletons and clips the animator borrows, and is freed
  with the engine, after the entity manager. A material belongs to the scene that first
  registers it, so each of the import's is moved: `scene_release_material` from the library and
  `scene_add_material` to the game's, which then frees it -- textures and all, where a copy
  through `MATERIAL_PARAMS` would leave them behind. Importing
  straight into an existing scene is not offered: it resets the shared texture pool's directory
  and assigns lights and cameras outright, on the path every golden goes through.

## Patterns

- **Factory pattern:** `create_*()` / `free_*()` for all objects.
- **Hash caching:** programs and textures via uthash.
- **Recursive transforms:** `global = parent->global * local`.
- **Fat G-buffer / MRT:** one forward pass writes color + normals + motion + albedo +
  SSS, each target gated on whether a post pass consumes it.
- **Temporal accumulation:** shared ping-pong primitive reused by AO, SSGI, SSR,
  SSS, exposure, TAA, and the composited fog layer, all gated on `taa_resolving`.
  `run_temporal_accum` sets `texelSize` per call from the resolution that call
  runs at — the program is shared across half-res and full-res consumers, so
  seeding it per program silently mis-sizes the neighbourhood clamp.
  **Fog carries a second, separate accumulator, gated the opposite way.** Its
  history is a 3D volume pair indexed by frame parity, it reprojects through its
  own stored previous camera rather than the velocity buffer (a froxel is a
  volume of air, not a surface), and it runs whether or not TAA does — the
  accumulation is what averages its binary per-cell shadow taps, so gating it
  would ship visibly stair-stepped shadows. The two filter different noise: the
  volume one smooths what the volume itself generates, the 2D one cancels the
  jitter the composite inherits from the aux depth (which is why it is pointless
  without TAA). See `specs/9.5.1`.
  **A fog cell with no history takes several samples** (spec 13.45): one entering the volume
  as the camera turns has nothing to blend, and its one jittered sample beside neighbours
  averaged over many frames drew a hard-edged band at the leading edge of a turn. It averages
  `fog_history_miss_samples` (4) of the sequence's points, in a branch of the inject's own
  `main` rather than a second pass, which cost 2.8 ms of GPU on any frame the camera moved,
  whatever it drew: each layer attached is a render pass on this GPU. A still frame has no
  such cell and pays nothing. See `docs/shader-subsystems.md`, Atmosphere.
  **Every history of LIGHT follows the exposure** (spec 13.20). The buffer is pre-exposed,
  so a history carries the exposure of the frame that wrote it, and unscaled an exposure
  change reached it only as fast as its blend decayed: stepping outdoors in silent, the fog
  volume's 0.9 took tens of frames to forget the room, longest where the fog was deepest, and
  the frame settled as an expanding sphere. Each side of a `PingPong` remembers the
  pre-exposure it was written at (the fog volume, `froxel_prev_pre`), and a history is read
  at this frame's -- the scene pass publishes it as `postfx->pre_exposure` -- times that
  ratio, on `.rgb` only and before any clamp or luma weight reads it. TAA, TAAU, SSGI, SSR,
  SSS, the fog layer and the fog volume take it; AO, specular occlusion and contact shadows
  are unitless and do not; the cloud march and the GI volume store absolute light and need
  nothing. A 4x step is 0 px from a run at the new exposure on its first frame without TAA;
  under it, what is left sits on edges, where TAA's inverse-luma weights depend on absolute
  brightness. `--no-history-rescale` (render, silent) and `--exposure-at` are the A/B.
- **SoA particle pools** with swap-remove compaction.
- **Scene-citizen subsystems:** node borrows, scene owns (mirrors Light/Camera).
- **Clustered forward light culling:** every LOCAL analytic light — point, spot, area — reaches a
  fragment through the 16x8x24 froxel grid's index list (`light_cluster.c/h`), not through a
  per-node selection. **Directionals do NOT go through the grid**: they reach every fragment
  unconditionally from a fixed `dirLights[MAX_DIR_LIGHTS]` array (4) in `lights_ubo.glsl`, and
  `pbr_frag`'s loop runs `k < numDir + clusterCount` over the two populations in sequence. The
  N-nearest-lights-per-node heap and `PBR_MAX_LIGHTS` were deleted by spec 9.1 and neither
  symbol exists.
- **Texture pooling:** single load, cache by filepath.

## Render Pipeline

The engine is a **forward renderer with a multi-target HDR G-buffer**, then a
screen-space post chain. GL 4.1 means every pass is a fullscreen raster pass
(`post_vert.glsl` is the shared fullscreen-triangle vertex shader) -- no compute.

**Scene FBO (MSAA, RGBA16F, multi-render-target):**
- att0 HDR color
- att1 view-space normals (+ SSR marker)
- att2 motion vectors (xy) + linear view-Z + roughness
- att3 albedo (for SSGI)
- att4 skin irradiance / SSS mask
- att5/att6 OIT accumulation + revealage (lazily allocated). The same two output
  locations, on a second lazily-allocated FBO, carry the absorbance moments
  `b1..b4` and `b0` under `--oit-moments` (spec 11.17)
- att7 ambient specular, for the split spec-occ composite. **R11G11B10F, and CORE-allocated
  rather than lazy** -- it comes up in the same pass as att0-att4 (`_gbuffer_attachments`,
  `engine.c:82-125`), so the six core rows span draw-buffer slots 0-4 and 7. The format is
  positive HDR only with no alpha, at half RGBA16F's bandwidth

Each G-buffer target is only written when a post pass that consumes it is active.

**Per-frame loop** (`engine_run`, or `game_run` in the game framework):
1. **Resolution sync** -- apply any scheduled render-scale switch, then rebuild
   the scene target + post chain if the derived sizes disagree with what they
   were last built at. Skips the frame entirely (keeping the frame clock and
   the `--frames` limit running) when the window is 0x0 or a rebuild failed.
2. **App update callback** -- input, physics, the game framework's fixed step.
3. **Origin shift**, then the **sky cycle tick**.
4. **App PRE-RENDER callback, then the engine's transform walk**
   (`scene_latch_prev_transforms` + `scene_propagate_transforms`), **then the
   camera rig's pose onto the camera** (spec 12.19; a NULL rig writes nothing,
   which is the normal state of an app that poses its own), **then the camera's
   view and projection matrices**, derived by the engine from the pose it now
   holds (spec 11.107). See below -- this is a one-statement-wide window and all
   three of its edges are load-bearing.
5. **The lighting update** (`scene_update_lighting`, spec 13.24): the cached shadow tiles'
   assignment, then the **GI probe captures** while a resident volume is dirty, then the
   **reflection probes**, each once, after the volumes its box touches have swept (specs
   13.13 and 13.24). Each ranks its world list from the camera first, once a frame, and
   streams from it; `scene_capture_ready` is the one statement of what a kept capture waits
   on.
6. **Shadow depth pass** (`render_shadow_depth_pass`) -- gated on
   `scene->shadow_system->enabled`; runs before the scene FBO is bound. Then the **rain's
   occlusion layer** (`shadow_render_rain_layer`, spec 13.9) whenever the scene rains, shadows
   on or off: switching shadows off does not put a roof over the street.
7. Bind scene MSAA FBO, set supersampled viewport + draw buffers, clear.
8. Async texture uploads (<=5/frame), mask-array build, POM height resolve. Headless, what the
   app's start-up asked for is already in: `engine_run` waits for it before frame 1 (spec 14.8).
9. App render callback -> `engine_render_scene`, or `engine_render_scene` itself
   when the app passed no callback (spec 11.107).
10. `engine_present_frame` -> PostFX chain, then the app's **overlays**
    (`engine_add_overlay`, specs 12.2 and 14.0 -- general post-tonemap draws, up
    to eight, lowest order first; the game UI adds itself at `UI_OVERLAY_ORDER`,
    over a 2D canvas, and the engine never learns what a `UISystem` is),
    then the **CRT** when it is on (spec 13.28), then the debug GUI, which is a
    developer's overlay and belongs on top of the game's -- and outside the
    television. With nothing added the list is empty, which is why every golden
    is 0 px with it in place. The engine's **loading screen**, when it
    is shown (spec 13.34), goes over all of it but the GUI -- and a frame it covers
    whole draws only the screen: step 9 settles the scene's panels and variants and
    draws no camera pass, step 10 runs no post chain and no GUI, and steps 5 to 8 go
    on loading.
11. An input script's `close`, asked here (spec 14.7) unless the run is ending already, so one
    taken makes this frame the last; then screenshot capture (headless/CI), swap buffers, then the
    poll for events -- or under **idle mode** (`Engine.idle`, spec 14.1, off by default) a WAIT
    until something asks for the next frame (see below) -- then the scripted pointer's frame.

**Idle mode draws a frame only when something asks for one** (spec 14.1), for an app that is
mostly still, as pcb's schematic and editor are; every game and every other app leave it off. Five
things about it are easy to get backwards.
- **A scene with anything to draw keeps the loop drawing**, every frame, as without it: an item,
  an IBL, a sky, water, falling rain, a fire, a particle system. A still 3D view drawn on demand,
  and stopping TAA's accumulation, are roadmap item 3's; until then idle mode only ever waits
  when no 3D scene is drawn. A game asks for every frame through `game_run`.
- **What asks**: in a window any wake draws -- an event, a request -- so no callback marks
  anything. Then `engine_request_frame` from any thread, `engine_request_frame_after` for a time --
  every time asked kept until it comes since spec 14.9, 64 at once, two within a millisecond one at
  the later, so a caller asks once and a frame is never early --
  `engine_end_work` when a thread's result is in, and whatever in the engine wants a frame, which
  asks DURING the frame: the scene with content, the UI while an ease or an entrance plays or a
  program reads `uTime` or a text field's caret is due to blink (spec 14.2, a timed wake), the
  debug GUI while it takes ImGui's queued input, the loader when a decode is done or five
  delivered leave more, the loading screen. Only the input script is asked at the frame's end,
  since its frame plays after the wait. **A change made inside a frame is
  drawn by that frame**; the calls are for changes made anywhere else. A request posts
  `glfwPostEmptyEvent` only while the loop is waiting, and the two order their atomics so neither
  misses the other.
- **The wait is kept out of every clock**: the engine's clock is the wall clock less
  `time_waited`, so a timed change -- a blink -- is timed by `glfwGetTime` instead. **`total_frames`
  does not advance** across a wait, since a gap in it is how the shadow tiles and the fog's
  histories tell lost history.
- **Headless, a run ends at the first frame after which nothing asks for another and no work is
  out** (an `engine_begin_work` not yet ended, a decode in flight, a timed wake), and that frame
  is its screenshot; `--frames` stays the most it draws. A hidden window is sent something in
  its first tenth of a second on macOS that wakes a waiting loop; headless asks again after any
  wake, but the `idle` arms still start their late work past it.
- **Headless, a frame waits for the work out before anything else that asks** (spec 14.7), an
  input script's frames included, so what one frame began has landed by the next on any machine
  and a scripted test repeats with no wait of its own. A window draws what asks at once. Only
  `engine_begin_work`'s work is waited for, never the loader's decodes, whose count falls only as a
  frame delivers them, so a wait on it would never end. Those decodes are only ever a texture
  asked for after the run began: what its start-up asked for is in before frame 1 (spec 14.8).
  **`engine_end_work` stores its request
  before its count falls and wakes the loop once, after both**: the request first is what keeps a
  run from ending before the result is drawn, and a wake after the count has fallen is the one a
  waiting loop needs. Waiting for work is its own state (`IDLE_WAIT_WORK`), in which a timed wake is
  no deadline: one that had passed, as the code editor's "wake now" has, spun the loop at full CPU
  until the work ended. **So the counted work must end on its own, with no frame drawn**: begun as
  it starts, never frames ahead, and ended by the worker, never by a frame draining what it posted.
  The canvas app once began its board's work at the sheet's build and started the thread after a
  timed wake, and the frame that would have started it waited for it for good. Work that never ends
  is said once after ten seconds, and the run goes on waiting.

**Step 4 is where an app puts anything the frame's geometry depends on** (spec
11.96): its camera, and any node it adds, removes or moves. It exists because
the walk used to be each app's own call in its RENDER callback, i.e. after the
shadow pass -- so every shadow in every app was drawn from LAST frame's
transforms and every LOD level chosen from stale positions. `apps/render` also
stepped its skeletal pose there, so a rig's shadow lagged its body for the same
reason; both halves moved together. **The camera is read right after the hook
returns**: the engine applies the installed camera rig and then derives the view
and projection matrices there (specs 11.107 and 12.19), so an app writes a rig's
anchor and input -- or poses the camera itself, with no rig installed -- and
nothing else. The rig's UPDATE stays with the app, because that is where the
input is and `camera_rig.h` is not allowed to know whether it came from an
action table, a mouse or a script. Anything reading the camera INSIDE the hook
reads last frame's pose, which is why `--trace-camera` prints from the render
callback and why the render app's near-clip recompute reads the rig's own arm. Every app used to rebuild
both matrices at the end of its own hook, and the three that did so only
through the drag controller skipped it whenever the GUI held the pointer. A
pose written from `render` is a frame late.

**The window is one statement wide and its edges are not stylistic.** It has to
land AFTER the origin shift, which rewrites root-child locals and the whole
tree's cached globals; AFTER the sky cycle tick, which rewrites a sun's
`original_direction` that the walk then rotates by the owning node; and BEFORE
the GI capture, whose probes a converged volume never re-bakes, so a capture from
stale transforms is wrong permanently rather than for a frame. Spec 11.94
recorded the update callback at step 2 as the right hook; the first two edges
refute that.

**The walk is idempotent and the latch is not**, which is why they are two
functions. `apply_transform_to_nodes` used to fuse `prev := global` into the
traversal, so a second walk in one frame made every node's previous pose its
current one, zeroed every motion vector and stopped TAA reprojecting. The engine
owns the latch and calls it once; anyone may call the walk again after a late
graph change. **No golden can see that failure** -- all 33 are single frames with
TAA off, thirty-two of them static scenes under a static camera and the last a rig
posed purely by frame index, measured. None has motion in it that TAA would
reproject -- which is what the `transform` and `shadow-lag` gate groups exist for.

**Scene passes** (`engine_render_scene`, in order). Before any of them, right after the draw-list
build, the **occlusion pass** (spec 11.98) rasterises this frame's authored occluders into a CPU
masked depth buffer and settles every item's `occluded` bit once — camera-pass-only (the one
`CullView` constructor initialises the switch false and one site sets it), skipped under captures,
and the whole off state for a scene with no occluders is two integer compares. Then:
opaque + alpha-masked (writes the G-buffer) -> skybox (procedural sky / IBL cubemap /
probe debug) -> **shadow catcher** -> refraction resolve (mip'd opaque color, if
transmissive) -> **water** (`--water`, spec 11.32) -> transparent pass (with an **OIT**
accumulate sub-pass, preceded under `--oit-moments` by a moment generation sub-pass over
the same meshes) -> **particles**
(soft, uses a mid-frame scene-depth resolve) -> stash `view_proj`
as `prev_view_proj` for next-frame motion vectors.

**A node's draw distance is the camera's alone** (spec 13.38, `SceneNode.draw_distance`,
inherited by every node under it that sets none). Each item's `beyond` byte is settled at the
list's build from the camera's eye, against the sphere round the item's world box that LOD
reads, the way `occluded` is settled by the occlusion pass. Only the camera pass's `CullView` arms it (`distance`, set in
`_scene_pass_prepare` and not for a capture re-entering there). Three things about it are easy
to get backwards:
- **The items stay in the list.** Cutting them at build would change which items the list
  holds every time the camera moved, and that is a graph change to the kept shadow faces.
- **Shadows, captures, kept tiles and the rain's layer still take what it cuts.** A tree past
  the camera's reach still shades the ground, and a reflection is not missing one for an eye
  standing far off.
- **An occluder past its distance is not rasterised.** The camera does not draw it, so it must
  hide nothing; `dd-occluder` is what holds that.

The catcher's position is load-bearing (spec 11.18). It writes depth, and drawing
it before the transparent and particle passes is what lets those sort against the
floor -- it drew last until 11.18, and translucency behind the plane composited
over the shadow instead of being hidden by it. Everything that must sort against
the ground has to stay after it.

**Water is the second surface with that property, and the two are mutually
exclusive** (spec 11.32). It sits after the refraction resolve because it samples it,
and before the transparent pass because it writes DEPTH -- it is a single-layer
surface in the opaque sense, not translucent geometry, and takes no part in OIT.
Writing the aux attachment is what makes fog and aerial perspective land at the
water's own depth rather than at the bed's; the late pass writes no aux, which is
why a translucent surface is still fogged wrong and water is not.
**The catcher is suppressed whenever water is active**: the catcher quad sits at
y = 0, which is where a water plane usually is, and the resulting coplanar depth
tie resolves per pixel into a wandering band that reads as a hole in the water.
Water is the more specific ground plane, so it wins.
**Water is not SSR-traced, and that is deliberate** (spec 11.32 phase 1b, confirmed by
measurement in 11.33): SSR shades only surfaces marked with a NEGATIVE normals-buffer alpha,
and `--no-ssr` moves **0 px** on a water frame. Water's reflection is its own split-sum
lookup inside `water_frag`, which already selects its mip from the per-texel roughness.
**The marker is a RANGE since spec 13.9, and every test of it is a range test**: (-1, 0) is
the catcher, whose magnitude is its edge falloff, and below -1 is wet ground, as
`-(1 + film)`, whose roughness SSR reads per texel from the aux buffer. Every reader asks
`include/ssr_marker.glsl`: a sign test anywhere (the tonemap's AO guard, `spec_occ`) hands wet
ground the catcher's treatment. Wet ground's
reflection REPLACES its share of the environment's rather than lerping the pixel, and **the
replacement is made in the split composite, BEFORE TAA, from the frame before's trace** (spec
13.21). Subtracted after TAA, it took this frame's share out of a frame TAA had already resolved,
and every thin ripple and wet edge went below zero; `rain-ssr-taa` and `rain-ssr-taau` are what
see that, since every other rain arm runs without TAA, where the two agree. **Where the frame
before did not reach -- the strip a turn reveals at its leading edge -- wet ground takes the
nearest of its reflections** (spec 13.46): the environment's share alone flashed that strip on
every turn in silent. The borrow fades back to that share past a tenth of the frame off its edge,
so a cut does not lend one edge column to the whole ground, and `rain-ssr-edge` holds it -- see
`docs/shader-subsystems.md`, Rain.

**PostFX chain** (`postfx_run`): MSAA resolve -> OIT composite -> G-buffer resolves ->
contact shadows -> GTAO + SSGI sweep -> split spec-occ composite -> the TAA seam
(TAA at full scale; the TAAU upscaling resolve under `--render-scale`, which
brings the render-res frame to post res) -> SSGI denoise -> SSR (hi-z) ->
SSGI composite -> atmosphere (fog + aerial) -> SSS -> motion blur -> the late draw (an app's
late-draw materials, then fire, then rain: streaks and splashes, spec 13.9) -> **an app's passes
before DoF** -> DoF -> **an app's passes before bloom** ->
auto-exposure metering -> bloom (+ lens flare) -> diffraction glare -> local exposure (its grid,
spec 13.19) -> tonemap + finishing (sharpen / color-grade /
vignette / gamma / grain) -> **an app's passes after the tone map** -> the app's overlay -> the
CRT (spec 13.28) or the present pass -> GUI. The three app points are spec 13.29's; see App
Shaders below. Everything before
the seam runs at RENDER res
(post size x `render_scale`, 1 by default), everything after composites onto a
post-res canvas; at scale 1 the two are the same buffer. Debug render modes
take a passthrough blit and skip the whole chain.

**Defaults:** on = bloom, GTAO + specular occlusion, SSR, auto-exposure,
NEUTRAL tonemap, normals G-buffer, OIT + moment weighting. Off (present, lazily
allocated) = SSGI, fog, DoF, motion blur, diffraction glare, local exposure, the CRT.

**The glare is OFF unless something asks for it** (specs 13.4 and 13.5, `glare.c`): the star an
aperture draws round light past `glare_threshold`, convolved by FFT on a 512x256 grid (1.5 ms
at 960x540). A scene asks with `post.glare` in its `.cscn`, an app with
`postfx->glare_enabled`; `apps/tree` does, for its low sun over the sea, and the render app,
forest and gametest take `--glare`. Three things about it are easy to get backwards.
- **It MOVES light rather than adding it.** The tonemap takes the moved light out of the pixel
  it came from (`glare_threshold.glsl`, the one rule the source pass shares), and
  `glare-conserves` holds the frame's total at 1.007.
- **Only the HALO is moved.** The star is built on a grid whose cell is several frame pixels,
  so the Airy disk -- about three quarters of the light -- is cut from the kernel and stays where
  it was at full resolution. Moving it redrew every bright edge soft and stepped.
- **Its source thresholds every texel BEFORE the downsample**, never after. A glint is often
  one bright texel among dim ones, and thresholding the block's average put back 58% of what
  the tonemap took, darkening every glint.

Clearwater's 8x far-field lift is deliberately not carried: it imitates a phone lens and is no
part of an aperture's diffraction. `engine_set_2d_preset` switches the glare off.

**Local exposure is an exposure per pixel ON TOP of the camera's** (spec 13.19,
`local_exposure.c`, after Unreal 5's bilateral Local Exposure). The camera's exposure -- manual,
physical or metered -- still sets the level. This splits the frame's log luminance into an
edge-aware BASE and the detail above it, and scales only the base's distance from middle grey:
`highlights` above grey, `shadows` below, `detail` the rest. A window onto a bright day comes down
without the room around it going dark. It is off by default, and at its defaults (every contrast
1) it is the identity, to 0 codes. A scene asks with `post.local_exposure`, an app with
`postfx->local_exposure_enabled`; silent turns it on by day. Five things about it are easy to get
backwards.
- **The base is a BILATERAL GRID, and that is what draws no halo.** Chen, Paris & Durand's grid,
  built by GATHER since GL 4.1 has no compute and no atomics, and sliced at each pixel's own
  luminance. On the gate's window-in-a-wall a plain blur darkens the wall beside the window to
  0.37 of itself; the grid leaves it at 1.000. It shares one atlas with the blurred luminance, so
  the tonemap pays ONE sampler for it (15 of 16 now).
- **Unreal's blurred-luminance blend (0.6) is the WEAKER setting** on a lit room. The blur
  averages the dark walls into the window's base and compresses it less; blend 0 recovered most.
- **Its strength is relative to the camera's middle grey, not to the room.** The grid is built
  from the EXPOSED frame and centred on `exposure.key`, so it is scale-invariant (a `SCALE_GATES`
  row holds that) and a high key leaves less headroom. silent's day key is 0.45, which leaves
  about a stop above grey before white, so its window, 4.2 stops over, needs highlights 0.2 where
  Unreal suggests 0.6-1. MEASURE the range with `--le-probe` before tuning by eye: it prints the
  frame's luminance percentiles in stops from grey, and it corrected a guess of 10 stops to 5.8.
- **It is applied in the tonemap, to the composite WITH bloom, after the Purkinje shift reads
  the light as it is**, and once per pixel: the sharpen's taps reuse the centre's factor, as they
  reuse AO's.
- **A gather costs by the LENGTH of its loops, not by its fetches.** One fragment per (cell, bin)
  walking a cell's 4096 texels was a few thousand long serial loops and cost 18.8 ms at silent's
  3200x1800. Every reduction here is two passes of short loops over many fragments, the meter
  histogram's fix, and the whole feature is 0.8-0.9 ms there and 1.2 ms at 4K.

**The CRT is OFF unless something asks for it** (spec 13.28, `crt.c`, after Lottes' CRTS): the
finished picture as a living-room television showed a console's -- a few hundred lines of signal,
each a soft beam, under a slot mask, with composite colour bleed, on a slightly curved tube. An
app asks with `postfx->crt_enabled`; silent takes `--crt`, the render app `--crt` and `--crt-*`.
Four things about it are easy to get backwards.
- **It shows the app's overlay too, and not the debug GUI.** With it on, the post chain, the debug
  tiles and the overlay draw into a window-sized RGBA16F picture (`postfx_picture_fbo`), which
  `postfx_present_picture` shows through the CRT; the GUI draws after it. The picture must be
  exactly window-sized, since the UI's scissor is in window pixels.
- **The dither moves to the last write.** The tonemap dithers only when it writes the window;
  under the CRT the CRT's own pass dithers, so 11.24's "dither last" still holds.
- **It does not keep the frame's mean, and is not meant to.** CRTS's tone curve lifts mid-grey back
  over what the beams and the mask take and compresses whatever is brighter: under the default
  mask a field at 0.18 comes out at 0.18 and one at 0.4 at 0.91 of itself. `crt-mean` holds the
  curve block by block, not the mean.
- **Scanlines fade to fused between 2.5 and 1.5 window pixels a line**, where a beam would beat
  against the pixel grid. So the default 480 lines on a 600-pixel window draw no lines at all,
  only the mask and the bleed.

**SSS is the exception in that list and used to be filed with them.** `engine->sss_enabled` is
**true** (`engine.c:238`) where the other four are false, so it is not off -- it is INERT, because
attachment 4 goes unwritten unless a material carries `subsurface > 0` and the pass is skipped
when it is. The distinction matters when bisecting: `--no-sss` on a scene with no subsurface
material changes nothing because there was nothing running, not because the flag failed.

**OIT is on by default and only bills scenes that use it.** The whole path is
gated on `scene->oit_mesh_count > 0` (ALPHA_BLEND and not transmissive), so a
scene with no translucent mesh allocates nothing, runs no extra pass, and is
byte-identical to `--no-oit`. A scene that HAS one pays two extra traversals of
those meshes and ~50 MB of MSAA targets at 640x400 on a Retina framebuffer, plus
244 MB of fp32 moment targets at 800x500. `--no-oit` is the unsorted late pass;
`--no-oit-moments` keeps OIT on the McGuire depth curve.

4.17 filed an ordering defect against this -- translucency behind the shadow
catcher landing in front of it -- and turning OIT on by default moved that from a
footnote to the default path. **Fixed in spec 11.18** by drawing the catcher
before the transparent pass rather than last, which repaired the unsorted late
pass and the particle depth resolve with it. What remains is that how much
translucency the floor hides depends on whether SSR is on, because SSR is what
makes the catcher write depth across its whole quad rather than only under the
shadow.

**TAA is the exception, and the struct default is not what you actually run.**
`taa_enabled` is false on PostFX, but the render app turns it on for every
windowed session (`apps/render/src/render.c`: `if (!args.headless ||
args.force_taa)`). So interactive runs have TAA **on**, headless runs have it
**off** unless `--taa` / `--headless-jitter`. Check that call site before
reasoning about temporal behaviour from the struct default: anything gated on
`taa_resolving` is live in the window and dead in a golden, so an artifact can
be invisible in one and obvious in the other.
**The same policy drops the SAMPLE COUNT to 1, and both halves are
`EngineConfig` fields** (`msaa_samples` and `taa`, spec 11.106). Until 11.106
they straddled the init call because they had to: the count preceded it, which
builds the scene target, and the TAA switch followed it and was a SILENT no-op
before postfx existed — joined in either direction the app either allocated the
whole G-buffer twice or rendered with no temporal filter at all, neither of which
announced itself. `create_engine` reads the config in the right order, so that
ordering is no longer the app's to get wrong; after creation the switch is the
plain field `postfx->taa_enabled` (spec 11.108). `apps/forest`, `apps/tree` and
(since 11.103) `apps/gametest` run one sample too, tree unconditionally since
11.88.
**The apps that do NOT are each a decision now, not an omission** (spec 11.103):
`apps/spores` keeps four samples because its particles write no motion vector and
TAA turns 32,000 motes into dashes; `apps/shapes` keeps them because
multisampling is what 2D line art wants and nothing in that scene moves;
`apps/splash` is outside the question entirely, drawing to the default framebuffer
without ever binding the engine's. `apps/sprites` and `apps/network` (11.105) keep four
samples for the same two reasons in turn, particles and line art, and run under the 2D
preset, which has TAA off regardless. (`apps/pcb` was a gitignored local app; it is now a
repository of its own with cetra as a submodule, per spec 11.105, and keeps the same
choice.)
**A 2D app's other defaults are one call**, `engine_set_2d_preset`: bloom, GTAO,
SSR, vignette, dither, TAA and shadows off, exposure pinned at unity, the `linear`
tone curve (the identity WITH the display encode, which passthrough is not), and a
white ambient radiance on the scene, under which an albedo is the colour on screen
with no light at all. It runs after `create_engine`, since the post chain has to
exist, and takes the scene for the ambient half. The overlays it leaves alone: both
default off, and a 2D app that wants the FPS counter writes `show_fps` like any other
(spec 11.108). Everything it switches off is ON
by default, which is why a flat red square through the plain defaults came out pink
with a halo: the light needed to reach albedo through the PBR path pushes red past
white, the neutral curve desaturates it, and bloom draws the glow.
**And the jitter is projection-dependent**: it rides the translation element under
an orthographic camera and the z element under a perspective one, because only the
second is divided back out. `--ortho` on the render app is the only way to reach
that path headless, and `taa-ortho` is what guards it.
**And so is everything downstream of it, which is the rule since spec 11.104: never
assume `clip.w = -z`.** Six shader formulas and four CPU sites did, and every one is now
a branch on `projection[2][3]` -- 0 under `glm_ortho`, -1 under `glm_perspective`, an
element the jitter never touches -- with the perspective side the previous expression.
The helpers live in `include/depth.glsl`: `viewZFromNdcZ`, `viewPosFromLinZ`,
`screenLengthAt`, `viewRayVecAt`, `clipWAt`, and `viewDirToCamera` /
`worldDirToCamera` for the view vector, which under a parallel projection is ONE
direction for the whole frame (the view matrix's third rotation row, not negated).
**`nearPlaneDist()` is the one near-plane recovery**; it replaced five hand-rolled copies
of `P32/(P22-1)`, wrong under ortho and written out in four shaders and `postfx.c`.
Anything that measures from the eye POINT on the CPU takes the same branch: LOD by view
height, the occluder eye as a homogeneous point, the cascade fit as a box. **A branch
around an unchanged GLSL divide is not byte-identical on this driver** (measured: 130 px
on water from an `if` whose new arm was never taken), so the depth inverse is one
coefficient form and two goldens were re-baked rather than the arithmetic wrapped. The
`ortho` gate group and the `ortho_shadow` golden are what see the orthographic path;
every other golden is perspective and proves only that it did not move.
**One sample is the only path an alpha-tested material gets**, and that is a
measured choice rather than an oversight: four samples arm alpha-to-coverage,
which holds a cutout's fractional coverage where a binary test under a clamped
history cannot — and costs +108% of forest's opaque row and +296% of a dense
cutout scene's frame to recover nothing the eye finds (spec 11.102, which
refused the promotion; 11.101's jitter is what the one-sample path carries
instead). TAAU (`--render-scale <f>`,
0.5-1, spec 11.7) rides on TAA but never rides the auto-enable. Three ways in:
the flag, the GUI "Render Scale" slider, and the `--render-scale-at` diagnostic
schedule; headless requires `--taa --headless-jitter` for any of them or the
app refuses and renders at full res. Changing it at runtime rebuilds every
render target at the next frame top and resets the temporal histories
(spec 11.8) — expect a visible fraction of a second of re-converging. At 0.67
it roughly halves frame time (measured 44 -> 22 ms at Retina fullscreen);
sharpen (`--sharpen`) is the user-facing crispness lever when scaled.

## The Typical Size (spec 13.57)

**`Engine.typical_size` is the size, in metres, of what the scene holds**: 1 for people and
rooms, the default, and 0.001 for a circuit board. It is `EngineConfig.typical_size` at creation
and `engine_set_typical_size` after; a `.cscn`'s `typicalSize`, the render app's
`--typical-size`, the snapshot row `engine.typical_size` and the GUI's "Typical Size (m)" all
reach it. Every length in the engine is one of three kinds, and the kind decides what the size
does to it:
- **Physical: what the scene states.** Positions, a light's range, size and body, an SSS
  profile's radius, a material's thickness, fog density per metre. Never rewritten, since a
  board states them in metres at its own size.
- **Tuning: what the engine chose for itself**, every one of them tuned on rooms. Among them:
  GTAO's radius, SSR's thickness and reach, the contact march's start, the depth tolerances of the
  AO blur and the a-trous filters, a soft particle's fade, the cascade and GI capture near planes,
  and the streaming margins. Each is its metre value times the size, read where it is used:
  `engine->typical_size` in C, `ViewParams.typicalSize` in GLSL. PostFX's settable ones are
  written by `postfx_reset_lengths` when the size is set, so an app sets the size first and its
  own values after.
- **Fitted: worked out each frame**, which is a `fit_clip` camera's near and far.

**A camera's planes are its owner's, or the fit's.** `Camera.fit_clip` fits them each frame,
inside `_engine_derive_camera` on the pose just written:
- **What it fits to:** the mesh boxes in front of the eye (`scene_for_each_mesh_box`), plus the
  shadow catcher's quad or a bounded water's rectangle when one draws, each box from its owner
  (`render_shadow_catcher_box`, `water_surface_box`).
- **The planes:** far is 1.1 times the farthest depth. Near is 0.9 times the larger of the nearest
  depth and the nearest distance times the frustum corner's cosine. The second bound is what fits
  a close-up, where a box reaching behind the eye takes the first to zero; past the farthest
  depth it says no box reaches the frustum, and is dropped, or near would pass far. Near is never
  under far / `CAMERA_CLIP_RATIO_MAX` (1e4).
- **A sea with no bounds is a square `SEA_REACH` (1000 typical sizes) out from under the eye**,
  the reach a camera has by default. It used to keep the planes the camera had, which after a
  frame fitted without it were the meshes' alone.
- **The boxes are at rest**, so wind, a skinned pose, the terrain's morph, a hook's offset or a
  wave's crest can carry geometry past a fitted plane. Particles, fire and rain are not counted.

Four things about it are easy to get backwards.
- **The size writes no camera plane, no light and no SSS profile.** It once wrote the installed
  camera's planes, which did nothing at creation, with no camera installed yet, and overwrote the
  app's own on a runtime change. It also wrote SSS slot 0's radius, which is an authored profile
  once an app replaces the table.
- **A ranking keeps metres, and its margin is times the size.** The cached tiles, the GI volumes
  and the reflection probes rank by distance in metres against `MARGIN * typical_size`. Dividing
  the distance instead put it in a unit nothing else in the engine uses.
- **A clip w is a length only under perspective.** Under a parallel projection it is the number 1,
  so a floor on it is `1e-4 * (projectionIsOrtho() ? 1.0 : typicalSize)`. Scaled
  unconditionally, every contact-shadow ray under a parallel projection stopped from a typical
  size of 1e4.
- **`scale-twin` holds it, and cannot see the classification.** Every corpus scene renders at 0 px
  against itself shrunk 1024 times, which is exact and not a tolerance. But a physical length
  wrongly multiplied by the size scales with the scene too, and twins exactly: that is how SSS
  slot 0 was missed. A scene already in millimetres is twinned by growing it, since shrunk its
  depths fall under fp16's normal range. `docs/verification.md`, The scale twin, has the argument.

## Camera Modes

**There are none, since spec 12.19.** `CameraMode` and `Engine.camera_mode` are deleted, along
with their config-snapshot row and the GUI radio pair that wrote them. The engine core never read
the field: its only three consumers were the drag controller's auto-orbit gate and arrow-key
branch and that radio, all of which went with `MouseDragController`.

What replaced it is `camera_rig.h`, which has no mode enum either — a follow camera is an anchor
that moves, a viewer orbit one that does not, first person is a distance of zero, and a pinned
pose derives all three. What varies is which fields a caller writes, not which branch runs.
