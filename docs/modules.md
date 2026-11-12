# Modules

What each source file under `cetra/src/` is for, one line each. A file's header says the rest; the
linked doc explains the subsystem. `cetra/src/ext/` is vendored and not listed.

## Core

| Module | Purpose |
|--------|---------|
| `engine.c/h` | The window, the frame loop, idle mode, input and the input script, the G-buffer, the program cache and the embedded post chain. See `docs/architecture.md`. |
| `engine_internal.h` | Private: the pass brackets and frame plumbing the renderer, shadows, captures and post chain call between them, and the lit-surface variant cache. |
| `input.c/h` | The engine's input state: keys, the ordered typed-key list, the pointer and its latched press and release, and the platform's shortcut modifiers. |
| `scene.c/h` | The scene graph and its transform walk, and what a scene owns: lights, materials, particles, shadows, IBL, sky, probes, GI, water, rain. See `docs/architecture.md`. |
| `camera.c/h` | A camera's pose, its projection, the same pose as an orbit, and `fit_clip`. See `docs/architecture.md`. |
| `render.c/h` | The ordered scene passes (opaque, skybox, catcher, water, transparent and OIT, particles) and the variant resolver. See `docs/architecture.md`. |
| `draw_list.c/h` | One frame's drawable meshes flattened out of the graph once, in the walk's order, with their bounds, LOD and cull bits, for every pass. |
| `occlusion.c/h` | CPU masked occlusion culling: authored occluders rasterised into a 256x144 buffer from this frame's camera, one conservative test per item. See `docs/architecture.md`. |
| `program.c/h`, `shader.c/h`, `uniform.c/h` | Shader compile and link, the runtime `#include` resolver and splice, and cached uniform setup. See `docs/shaders.md`. |
| `ubo.c/h` | std140 uniform-block plumbing: the binding-point registry and the blocks' layouts. |
| `shader_hook.c/h` | An app's surface hook: GLSL deciding a lit surface's properties, and optionally an offset moving its vertices. See `docs/shader-subsystems.md`. |
| `shader_params.c/h` | An app shader's named vec4 uniforms (`ShaderParams`, no GL in the header) and `shader_clock_upload`, the `time` and `frame` every app shader reads. See `docs/shader-subsystems.md`. |
| `common.c/h` | The vertex-attribute, sampler-unit and render-mode enums, and the axis gizmo's vertices. See `docs/shaders.md`. |
| `gui.c/h` | The Dear ImGui debug panels, drawn over the engine's public fields and calls. |
| `profiler.c/h` | Per-pass GPU and CPU time and submission counts for one frame, behind `--profiler`. See `docs/verification.md`. |
| `cscene.c/h` | The `.cscn` scene file parsed into a plain struct; applying it is the app's. |

## Materials and geometry

| Module | Purpose |
|--------|---------|
| `material.c/h` | PBR materials: the scalar parameters (`MATERIAL_PARAMS`), 13 texture slots, glTF extension parameters and layers. See `docs/shaders.md`. |
| `mesh.c/h` | Geometry arrays and their GL upload: UV0/UV1, colours, skinning, the AABB and the LOD chain. |
| `mesh_builder.c/h` | Growable vertex arrays for geometry whose size is unknown until built, handed to a Mesh outright. |
| `geometry.c/h` | Procedural primitives: box, plane, circle, rect, bezier. |
| `lod.c/h` | A mesh's LOD chain built by meshoptimizer into one index buffer; refused for skinned meshes. |
| `cluster.h`, `cluster_build.cpp` | Cluster LOD DAGs (meshoptimizer's `clusterlod.h` behind a C API), crack-free by construction. See `docs/architecture.md`. |
| `aabb.h` | The axis-aligned box and its five operations, on cglm alone; states the empty-accumulator and stored-bound conventions. See `docs/architecture.md`. |
| `texture.c/h` | Texture loading through one `TextureDesc`, and the pool that caches by resolved path. See `docs/shaders.md`. |
| `texture_compress.c/h` | Block compression: BC5 for normals, BC4 for masks, DXT for colour on request. See `docs/shaders.md`. |
| `material_texture_array.c/h` | Packs a scene's per-texel material images (masks, layer maps, splats, decals) into one `sampler2DArray`. See `docs/shaders.md`. |
| `transform.c/h`, `intersect.c/h` | Transform decomposition; ray and AABB intersection. |
| `decal.c/h` | Decals packed for the shader; the froxel masks beside them are `light_cluster.c`'s. See `docs/shader-subsystems.md`. |
| `layers_vt.c/h`, `layers_vt_pages.h` | The layered-surface composite cache and its page table. See `docs/shader-subsystems.md`. |
| `roads.c/h` | Roads on layered surfaces: world-XZ polylines that override splat weights, sent as segments in a uniform block. See `docs/shader-subsystems.md`. |
| `wind.c/h` | The scene's wind field, which wind-responsive materials sway by, and the bound that keeps a swaying mesh cullable. |

## Lighting environment (on Scene)

| Module | Purpose |
|--------|---------|
| `light.c/h` | Directional, point, spot and area lights. See `docs/lighting.md`. |
| `shadow.c/h` | Cascaded shadow maps, the per-frame punctual pool, and the cached point- and area-light tiles. See `docs/lighting.md`. |
| `ibl.c/h` | Split-sum IBL: irradiance, GGX prefilter, BRDF LUT. See `docs/lighting.md`. |
| `sky.c/h` | Hillaire's atmosphere: transmittance, multiscatter and sky-view LUTs, and the aerial-perspective volume. See `docs/lighting.md`. |
| `sky_clouds.c` | The volumetric clouds: the noise fields baked on the CPU, the march and the cloud shadow map. See `docs/shader-subsystems.md`. |
| `moon_surface.c/h`, `moon_map.h` | The moon's surface baked once on the CPU, over the generated maria map. See `docs/shader-subsystems.md`. |
| `probe.c/h` | One parallax-corrected reflection probe: capture, GGX prefilter, the box-projected lookup; at a count of one it binds into the IBL prefilter unit. See `docs/lighting.md`. |
| `probe_set.c/h` | Any number of reflection probes, the nearest 16 resident, blended per fragment. See `docs/lighting.md`. |
| `gi_volume.c/h` | DDGI irradiance volumes: a world list, the nearest resident, swept under the capture budget. See `docs/lighting.md`. |
| `lighting_atlas.c/h` | The one unit-14 texture every resident GI volume and reflection probe lives in, owned by the scene. See `docs/lighting.md`. |
| `residency.c/h` | Which of a world's items hold the resident slots, and each item's life in one. See `docs/lighting.md`. |
| `capture_key.c/h` | The cook key for a light capture: everything a capture of a box reads, folded at rest. See `docs/lighting.md`. |
| `light_cluster.c/h` | Clustered forward light culling on a 16x8x24 froxel grid, with std140 light blocks. See `docs/lighting.md`. |
| `ltc.c/h`, `ltc_lut.h` | Linearly Transformed Cosines tables for area lights; the tables are generated. |
| `emissive_light.c/h` | Derives LTC area panels from emissive meshes (`--emissive-lights`). See `docs/lighting.md`. |
| `ies.c/h` | IES photometric profiles read and resampled into the light block. See `docs/shader-subsystems.md`. |
| `rain.c/h`, `rain_render.c/h` | Rain: the physics and state (no GL), and its drawing, surfaces and medium. See `docs/lighting.md`. |
| `fire.c/h`, `fire_internal.h`, `fire_sim.c`, `fire_render.c/h` | Fire: grid, flame and flipbook kinds whose light is derived from what they draw; the GPU simulation and the late-draw composite. |
| `water.c/h` | The water surface, its own program, and the surface query. See `docs/shader-subsystems.md`. |
| `water_bounds.h` | Where a water is: its rectangle less its dry shapes, the same rule as `include/water_bounds.glsl`. See `docs/shader-subsystems.md`. |
| `shore_runup.c/h`, `shore_chain.c/h` | The shore's run-up on the CPU, twin of the shader's, and the swash film's Lagrangian chains. See `docs/shader-subsystems.md`. |

## Post-processing (on Engine)

| Module | Purpose |
|--------|---------|
| `postfx.c/h` | The HDR post chain: bloom, tonemap, exposure, TAA, GTAO, SSGI, SSR, SSS, fog, motion blur, DoF, OIT composite, app passes. See `docs/architecture.md`. |
| `exposure.c/h` | The camera's exposure, the one place that decides it, and the metering modes. See `docs/shader-subsystems.md`. |
| `glare.c/h` | Diffraction glare by FFT. See `docs/shader-subsystems.md`. |
| `local_exposure.c/h` | Local exposure's bilateral grid and blurred luminance, in one atlas the tonemap slices. See `docs/shader-subsystems.md`. |
| `lut.c/h` | Adobe `.cube` colour-grading LUT reader; refuses a bad file by name, never truncates. |
| `crt.c/h` | The CRT: the window-sized picture resampled to a line signal and drawn as a television. See `docs/shader-subsystems.md`. |
| `loading_screen.c/h` | The engine's loading screen, a station ident on its own CRT. See `docs/architecture.md`. |

## Assets and animation

| Module | Purpose |
|--------|---------|
| `import.c/h` | Assimp-based FBX, OBJ and glTF/GLB loading. |
| `async_loader.c/h` | Background texture decoding on a thread pool; mips, compression and upload stay on the main thread. See `docs/architecture.md`. |
| `animation.c/h` | Clips, skeletons and the pose: sampling one clip, blending two, applying one to the bone matrices. See `docs/animation.md`. |
| `animator.c/h` | What plays on a skeleton: a phase-synced blend space, crossfades, an override layer, clip events and root motion. See `docs/animation.md`. |
| `anim_graph.c/h` | A state machine over the animator: states, transitions and parameters as data. See `docs/animation.md`. |
| `rigging.c/h` | Semantic bone matching and cross-rig retargeting. |
| `springbone.c/h` | Procedural secondary motion (Verlet) on un-animated bone chains. |
| `ik.c/h` | Two-bone IK for foot planting and locking. See `docs/animation.md`. |
| `look_at.c/h` | A head turned toward a point, solved after the IK. See `docs/animation.md`. |
| `ragdoll.c/h`, `ragdoll_jolt.cpp/h` | A rig falling over as Jolt capsules built from its bind pose. See `docs/animation.md`. |
| `nav_graph.c/h` | An authored graph of places and links, A* over it, and a follower. See `docs/game-framework.md`. |

## Procedural (`cetra/src/procedural/`)

| Module | Purpose |
|--------|---------|
| `terrain.c/h` | The heightfield (fbm or a stored field), its mesh and collider, and the ground classes. See `docs/terrain.md`. |
| `terrain_quadtree.c/h` | A CDLOD quadtree of terrain patches. See `docs/terrain.md`. |
| `terrain_stream.c/h` | The stored field paged off disk as one window per pyramid level. See `docs/terrain.md`. |
| `terrain_tex.c/h` | The four ground materials a layered terrain blends between. See `docs/terrain.md`. |
| `erosion.c/h` | Hydraulic and thermal erosion over a field, identical at any worker count. See `docs/terrain.md`. |
| `heightmap.c/h` | `.r16` read and write and 16-bit PNG read, apart from `texture.c`, which cannot ask for 16 bits. |
| `tree_gen.c/h` | Procedural trees: a skeleton of curved branches, then one bark mesh and one leaf mesh. |
| `rock.c/h` | Noise-displaced icosphere rocks. |
| `vegetation_tex.c/h` | Bark and foliage textures on the CPU, and the 2D noise they share (seeded globally, one thread). |
| `sand.c/h` | Sand ripple textures on the CPU. |
| `stochastic_tex.c/h` | The bake half of by-example stochastic texturing (Heitz and Neyret 2018). |
| `foam_pattern.c/h` | The water's tiling foam web, baked once with mips. |
| `water_waves.c/h` | The Gerstner wave sum on the CPU, the same as `ocean.glsl`'s. See `docs/shader-subsystems.md`. |

## Particle system

| Module | Purpose |
|--------|---------|
| `particle_system.c/h` | A system of emitters and a backend; a scene citizen the engine ticks and draws. See `docs/game-framework.md`. |
| `particle_emitter.c/h` | An emitter: pool, module lists, spawn transform, renderer. |
| `particle_pool.c/h` | Structure-of-arrays particle pool with swap-remove compaction. |
| `particle_module.c/h` | Composable spawn, init and update modules. |
| `particle_sim.c/h`, `particle_sim_tf.c` | The simulation backend seam, with a CPU backend and a transform-feedback GPU backend. |
| `particle_renderer.c/h` | The instanced billboard renderer. |
| `noise.c/h` | 3D Perlin and divergence-free curl noise. |

## Game framework (`cetra/src/game/`)

| Module | Purpose |
|--------|---------|
| `game.c/h` | The fixed-timestep loop and its callbacks, owning physics, entities and audio. See `docs/game-framework.md`. |
| `physics.c/h` | Jolt physics: rigid bodies, ray casts, sweeps, constraints and motors. See `docs/game-framework.md`. |
| `character.c/h` | A character controller on Jolt's `CharacterVirtual`, with a crouch. See `docs/game-framework.md`. |
| `entity.c/h`, `component.h` | Entities and their components. See `docs/game-framework.md`. |
| `animator_component.c/h` | The `ANIMATOR` component, ticked once per rendered frame. See `docs/game-framework.md`. |
| `brain.c/h` | The `BRAIN` component, a small utility AI. See `docs/game-framework.md`. |
| `input.c/h` | The game layer's input: keys, mouse, gamepads behind a reader seam, and the action table. See `docs/game-framework.md`. |
| `audio.c/h` | Audio on miniaudio: sounds, voices, buses, 3D sources and zones. See `docs/game-framework.md`. |
| `settings.c/h` | What a player chose, persisted through one descriptor table. See `docs/game-framework.md`. |
| `save.c/h` | Where the player was, persisted through registered descriptor tables. See `docs/game-framework.md`. |

## Jolt bridges (C++)

| Module | Purpose |
|--------|---------|
| `physics_cook.cpp/h` | Jolt shape serialise and restore, for the cook. See `docs/architecture.md`. |
| `character_jolt.cpp/h` | A character's capsule built and changed while it stands, and its supporting volume. See `docs/game-framework.md`. |
| `jolt_handles.hpp` | The casts between JoltC handles and Jolt objects every C++ file shares. |

## The 2D canvas

| Module | Purpose |
|--------|---------|
| `canvas.c/h`, `canvas_edit.c`, `canvas_record.h` | Exact 2D shapes and text drawn after tone mapping, kept on the GPU and edited in place. See `docs/canvas.md`. |
| `triangulate.c/h` | Polygons with holes into triangles (earcut). See `docs/canvas.md`. |

## Text and UI

| Module | Purpose |
|--------|---------|
| `text.c/h` | Fonts: the SDF atlas, kerning, the pen, and UTF-8. See `docs/ui.md`. |
| `glyph_cache.c/h` | Coverage bitmaps at the exact pixel size, for the code editor. See `docs/ui.md`. |
| `embedded_file.h` | A file built into the program by `cetra_embed_files`. See `docs/shader-subsystems.md`. |
| `ui.c/h` | The retained UI: elements, layout, focus, input routing and a run of presses, theme. See `docs/ui.md`. |
| `ui_draw.c` | The UI's draw list, drawn as an engine overlay after tone mapping. See `docs/ui.md`. |
| `ui_text.h` | Private: the UI's one text walk for measuring, breaking, drawing and caret placement. See `docs/ui.md`. |
| `ui_internal.h`, `ui_hooks.h` | Private: what the UI's files share, and what a test reads of the system. See `docs/ui.md`. |
| `ui_field.c/h` | The text field, on stb_textedit. See `docs/ui.md`. |
| `ui_table.c/h` | The table element: columns sized across every row, rows read from the app's source, a header and a cursor. See `docs/ui.md`. |
| `ui_typing.c/h` | Private: the platform's editing conventions, shared by the field and the editor. See `docs/ui.md`. |
| `fuzzy.c/h` | `fuzzy_match`: an in-order match, a score and the matched positions. See `docs/ui.md`. |
| `text_doc.c/h`, `text_doc_edit.h` | The code editor's text, apart from the editors showing it; the edit half is private. See `docs/ui.md`. |
| `ui_editor.c/h`, `ui_editor_internal.h`, `ui_editor_hooks.h` | The code editor element; its private halves. See `docs/ui.md`. |
| `ui_editor_vim.c/h` | The code editor's vim mode, held to vim itself by an oracle. See `docs/ui.md`. |

## Cameras and apps

| Module | Purpose |
|--------|---------|
| `camera_rig.c/h` | What moves a camera and what the controls mean under it. See `docs/camera.md`. |
| `app.c/h` | App helpers: `CameraDrag` and `PlaneDrag` turn a pointer into rig input, plus a three-point light rig. See `docs/camera.md`. |
| `gait.c/h` | A first-person walker's stride, footfalls, eye lag and head bob. See `docs/camera.md`. |
| `frame_script.c/h` | The frame-range script grammar the scripted pad and the input script share. |

## Support

| Module | Purpose |
|--------|---------|
| `cook.c/h` | The derived-data cook: a content-addressed cache over deterministic bakes. See `docs/architecture.md`. |
| `config_snapshot.c/h` | The live session's settings as JSON, dumped and restored through one table. See `docs/verification.md`. |
| `json_util.c/h` | cJSON helpers several readers share, and `json_add_float` at a binary32 round-trip width. See `docs/architecture.md`. |
| `spectrum.c/h` | CIE 1931 colour matching and conversion to the engine's linear Rec.709. |
| `thread.c/h` | Threads, mutexes and condition variables over pthreads or Win32, and `cetra_bake_bands`. |
| `compat.h` | `strcasecmp` on MSVC. |
| `util.c/h` | Path handling and GL error checks. |
