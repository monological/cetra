# Modules

What each source file under `cetra/src/` is for.

## Key Files

**Core**
| Module | Purpose |
|--------|---------|
| `engine.c/h` | Window, input, render loop, camera modes, G-buffer/MSAA, program cache, embedded PostFX. See `docs/architecture.md`. |
| `scene.c/h` | Scene graph, hierarchical transforms, owns lights/materials/particles/shadow/IBL/sky |
| `render.c/h` | Scene traversal + the ordered scene passes (opaque/skybox/transparent/OIT/particles) |
| `occlusion.c/h` | CPU masked occlusion culling (spec 11.98): authored proxies rasterised into a 256x144 fixed-point buffer from THIS frame's camera, one conservative test per item per frame. See `docs/architecture.md`. |
| `program.c/h`, `shader.c/h`, `uniform.c/h` | Shader program compile, uniform setup. See `docs/shaders.md`. |
| `shader_hook.c/h` | An app's SURFACE HOOK (spec 13.29): GLSL deciding a lit surface's albedo, coverage, normal, roughness, metallic, AO and emission, and optionally an OFFSET moving its vertices. See `docs/shader-subsystems.md`. |
| `shader_params.c/h` | An app shader's own named vec4 uniforms (`ShaderParams`, spec 13.29), and `shader_clock_upload`, the `time` and `frame` every app shader reads. No GL in the header, so a scene file's description carries them |
| `common.h` | Vertex-attribute + sampler-unit + render-mode enums |

**Materials & geometry**
| Module | Purpose |
|--------|---------|
| `material.c/h` | PBR materials, 14 texture slots, glTF extension params, mask-array layers |
| `mesh.c/h` | Geometry data + GL buffer management (UV0/UV1/colors/skinning) |
| `geometry.c/h` | Procedural primitives (box, plane, circle, rect, bezier) |
| `texture.c/h` | Texture loading + pool caching |
| `material_texture_array.c/h` | Packs per-texel material images -- masks and layer maps -- into a `sampler2DArray` |
| `transform.c/h`, `intersect.c/h` | Transform decomposition, ray/AABB intersection |
| `cluster.h`, `cluster_build.cpp` | Nanite's CPU half on GL 4.1 (spec 11.63): meshoptimizer's own `demo/clusterlod.h` behind a C API, in the one C++ TU the JoltC split already precedented. See `docs/architecture.md`. |

**Lighting environment (on Scene)**
| Module | Purpose |
|--------|---------|
| `light.c/h` | Directional / point / spot / area lights. See `docs/lighting.md`. |
| `shadow.c/h` | Cascaded shadow maps (CSM), PCF + optional PCSS; the per-frame punctual pool, sixteen layers, where an area light is a five-face cube in its own frame ending at its range (spec 13.27); and the cached point- and area-light TILES (specs 13.16 and 13.27): blocks of six faces a view kept by light identity, invalidated per face by a moved caster's box, carried across an array rebuild by blit, with a store cell of the still casters for each face that sees a mover, from a pool on top of the budget (spec 13.26). See `docs/lighting.md`. |
| `ibl.c/h` | Split-sum IBL: irradiance + GGX prefilter + BRDF LUT |
| `sky.c/h` | Hillaire physically-based atmosphere (transmittance/multiscatter/sky-view LUTs + the per-frame aerial-perspective volume) |
| `probe.c/h` | ONE parallax-corrected reflection probe: capture, GGX prefilter, the box-projected lookup. Still the whole feature at count 1, where it binds into the IBL prefilter unit exactly as it always did |
| `probe_set.c/h` | N of them (spec 11.70), blended PER FRAGMENT: a per-froxel 16-bit mask on the light grid says which probes reach a cell, each proxy box gives a weight, and the leftover weight falls to the global environment through the same expression the no-probe path uses. See `docs/lighting.md`. |
| `lighting_atlas.c/h` | The ONE unit-14 texture every resident GI volume and reflection probe lives in (spec 13.24), owned by the SCENE: GI slots side by side, each as large as the largest volume in the world, then the probe columns, stacked where the GI slots are tall enough. See `docs/lighting.md`. |
| `residency.c/h` | Which of a world's items hold the few resident slots, and each item's life in one (spec 13.24): the nearest to the camera by distance to their boxes, and a state -- out, capture, upload, loaded -- with the texels kept while it is out. See `docs/lighting.md`. |
| `light_cluster.c/h` | Clustered forward light culling: 16x8x24 frustum grid, std140 light UBOs. See `docs/lighting.md`. |
| `ltc.c/h` | Linearly Transformed Cosines tables for rectangular area lights (on Engine) |
| `emissive_light.c/h` | Derives LTC area panels from emissive meshes (spec 11.49, `--emissive-lights`) |
| `rain.c/h`, `rain_render.c/h` | Rain (spec 13.9), a Scene subsystem (`scene->rain`) owned like water. See `docs/lighting.md`. |

**Post-processing (on Engine)**
| Module | Purpose |
|--------|---------|
| `postfx.c/h` | The whole HDR post chain: bloom, tonemap, exposure, TAA, GTAO, SSGI, SSR, SSS, fog, motion blur, DoF, OIT composite |
| `lut.c/h` | Adobe `.cube` colour-grading LUT reader (spec 11.58); refuses by name, never truncates |
| `crt.c/h` | The CRT (spec 13.28): the window-sized picture, its resample to a `crt_lines` signal in linear light, and the television drawn into the window. See `docs/shader-subsystems.md`. |
| `loading_screen.c/h` | The engine's loading screen (spec 13.34): CETRA over ENGINE as a 1970s station ident on a worn tape, on a CRT of its own -- the letters turn in, ENGINE lights, a light runs along a striped rule to a sparkle, the tape glitches, and it holds under a teletext LOADING sign, glitching now and then, until it is let go. See `docs/architecture.md`. |
| `local_exposure.c/h` | Local exposure's passes (spec 13.19): the frame at half res, the bilateral grid gathered and blurred, the blurred luminance, all into one RG32F atlas the tonemap slices. See `docs/shader-subsystems.md`. |

**Assets & animation**
| Module | Purpose |
|--------|---------|
| `import.c/h` | Assimp-based FBX / OBJ / glTF-GLB loading |
| `animation.c/h` | Skeletal animation: clips, skeletons, and the POSE seam (spec 12.1) -- `animation_sample_pose` evaluates ONE clip into per-bone TRS with its retargeting and hierarchy in per-call scratch, `pose_blend` mixes two, `animation_state_apply_pose` turns one into `bone_matrices[128]`. See `docs/animation.md`. |
| `animator.c/h` | What PLAYS on a skeleton (spec 12.1), and how one thing becomes another: a phase-synced 1D blend space (every entry on one clock, so a foot planted 40% through the walk is planted 40% through the run), a crossfade that drops the outgoing source on completion so the settled pose is the incoming one exactly, one bone-masked override layer that releases itself, and clip events dispatched AFTER the pose is applied -- so a handler may play something. See `docs/animation.md`. |
| `rigging.c/h` | Semantic bone matching + cross-rig retargeting (not GPU skinning) |
| `springbone.c/h` | Procedural secondary motion (Verlet) on un-animated bone chains |
| `ik.c/h` | Two-bone IK for foot planting and LOCKING (specs 12.4 and 12.9): the law of cosines over a hip/knee/ankle chain, spliced where spring bones are and **after** them, between the pose's globals being accumulated and the skinning matrices being built -- the one place per-bone MODEL-space positions exist. See `docs/animation.md`. |
| `ragdoll.c/h`, `ragdoll_jolt.cpp/h` | A rig falling over (spec 12.16), and the first thing in this engine where animation and physics meet. See `docs/animation.md`. |
| `look_at.c/h` | A head turned toward a point (spec 13.17), in the animation state's `look_at` slot, solved right AFTER the IK -- which may drop the pelvis the whole neck hangs from -- inside the non-ragdoll branch, writing the globals only, for `ik.h`'s reason. See `docs/animation.md`. |
| `nav_graph.c/h` | Places and the links between them (spec 13.17): an authored graph, NOT a navmesh, so an agent goes only where someone put a place. See `docs/game-framework.md`. |
| `async_loader.c/h` | Background texture streaming (pthread pool sized `get_cpu_cores() - 2`, clamped to [2, 8]). See `docs/architecture.md`. |

**Procedural terrain (`cetra/src/procedural/`, the subsystem `apps/forest` is built on)**
| Module | Purpose |
|--------|---------|
| `terrain.c/h` | fbm heightfield, tiled mesh + collider build, per-vertex slope/altitude tint. See `docs/terrain.md`. |
| `erosion.c/h` | Mei virtual-pipes hydraulic + thermal erosion over a field (spec 11.59). See `docs/terrain.md`. |
| `heightmap.c/h` | `.r16` read + write and 16-bit PNG read (spec 11.59). Deliberately not through `texture.c`, whose format table cannot request 16-bit at all |
| `terrain_stream.c/h` | The stored field paged off disk (spec 11.69, D4): an fp32 tiled `.cts` file and one rectangular WINDOW resident per pyramid level, anchored on the camera and the player. See `docs/terrain.md`. |
| `terrain_tex.c/h` | The four ground materials a layered terrain blends between — grass, rock, silt, gravel (spec 11.60). See `docs/terrain.md`. |
| `terrain_quadtree.c/h` | A CDLOD quadtree over the terrain's domain (spec 11.63), replacing the fixed `tiles x tiles` grid: fine patches near the camera, coarse ones away, so the patch count tracks how many LEVELS there are and not how much ground they cover. See `docs/terrain.md`. |

**Particle system (general, Niagara-style)**
| Module | Purpose |
|--------|---------|
| `particle_pool.c/h` | SoA particle pool with swap-remove compaction |
| `particle_emitter.c/h` | Emitter: pool + phase module lists + spawn transform + renderer |
| `particle_module.c/h` | Composable spawn / init / update modules |
| `particle_sim.c/h` | Sim backend vtable (CPU backend now; GL 4.3 compute seam) |
| `particle_renderer.c/h` | Pluggable renderer (instanced billboard) |
| `particle_system.c/h` | System (emitters + backend); scene citizen (engine auto-ticks + auto-renders) |
| `noise.c/h` | 3D Perlin + divergence-free curl noise |

**Game framework (optional, `cetra/src/game/`)**
| Module | Purpose |
|--------|---------|
| `game.c/h` | Fixed-timestep loop, init/update/render/shutdown callbacks, ticks particles + physics. See `docs/game-framework.md`. |
| `physics.c/h` | Jolt Physics: rigid bodies, raycasts, sweeps, 5 constraint types + motors. See `docs/game-framework.md`. |
| `entity.c/h`, `component.h` | ECS-lite entities + components. See `docs/game-framework.md`. |
| `brain.c/h` | The BRAIN component (spec 13.17): a small utility AI. See `docs/game-framework.md`. |
| `character.c/h` | Character controller on Jolt `CharacterVirtual`. See `docs/game-framework.md`. |
| `animator_component.c/h` | The `ANIMATOR` component (spec 12.1): an `Animator` the entity owns, its pose bound to the entity's node, ticked once per RENDERED frame from `game_pre_render` with the sim clock's delta -- never per fixed step, because the tick begins with the prev-pose latch and two in one frame would lose a step's motion vector. See `docs/game-framework.md`. |
| `input.c/h` | The game layer's input, polled once a frame before the fixed steps: keys, mouse, up to four gamepads in GLFW's standard layout behind a reader seam (GLFW, or a scripted pad from a text file), and the action table a game reads instead of key codes (spec 11.109). See `docs/game-framework.md`. |
| `settings.c/h` | What a PLAYER chose, persisted across runs (spec 12.2): the four bus volumes, window mode and vsync, as ONE descriptor table walked in both directions -- `config_snapshot.c`'s technique, because a writer and a reader maintained separately drift silently. See `docs/game-framework.md`. |
| `save.c/h` | Where the player WAS, persisted across runs (spec 12.3). See `docs/game-framework.md`. |
| `audio.c/h` | The game layer's audio (spec 12.0): one output device wrapping miniaudio's high-level engine, wrapped as an `AudioSystem` the `Game` owns like the physics world. See `docs/game-framework.md`. |

**The 2D canvas (spec 14.0, item 1 of `docs/eda-roadmap.md`)**
| Module | Purpose |
|--------|---------|
| `canvas.c/h`, `canvas_edit.c`, `canvas_record.h` | Exact 2D shapes after tone mapping, at display resolution, kept on the GPU and edited in place: segments, arcs, circles, rounded boxes, polylines, filled polygons with holes, and text. See `docs/canvas.md`. |
| `triangulate.c/h` | An outline with holes into triangles, a port of Mapbox's earcut v2.2 with its ISC notice (spec 14.0). See `docs/canvas.md`. |

**Support**
| Module | Purpose |
|--------|---------|
| `text.c/h` | SDF text rendering (stb_truetype) with glow. See `docs/ui.md`. |
| `ui.c/h` | The game UI layer (spec 12.2): a RETAINED tree of elements over a two-pass box layout, a geometric focus model, and a theme whose every zero means "inherit". See `docs/ui.md`. |
| `ui_field.c/h` | The text field (spec 14.2): ImGui's copy of stb_textedit, compiled as C from this one file behind a block of five compatibility names (`ImS8`, `ImGuiDir_Left`/`_Right`, the `LastMoveDirectionLR` and `Stb` members, `IM_ASSERT`), run on a growing UTF-8 buffer. See `docs/ui.md`. |
| `fuzzy.c/h` | `fuzzy_match` (spec 14.2): whether a query's characters appear in a name in order, a score, and the byte offset of each matched character, in a `FuzzyPositions` that says how many it wrote (14.3) -- after fzf's second algorithm, the best placement by a dynamic program over a 32 by 256 table on the stack, not the first. See `docs/ui.md`. |
| `text_doc.c/h` | The code editor's TEXT (spec 14.4), kept apart from the editors showing it, as lite keeps its Doc and vis its Text: two editors of one text each keep their own carets and scroll, and an edit through either -- or the host's -- moves both. See `docs/ui.md`. |
| `glyph_cache.c/h` | The code editor's face as coverage BITMAPS made at the exact pixel size shown (spec 14.4), not the UI's distance fields, which go soft at the 12 to 14 pixels code is read at, worst on a 1x screen. See `docs/ui.md`. |
| `ui_editor.c/h` | The code EDITOR (spec 14.4), the ninth UI element (`ui_editor`, in `ui.h`), on a `TextDoc` it borrows, in the theme's `editor_face`. See `docs/ui.md`. |
| `ui_editor_vim.c/h` | The code editor's VIM MODE (spec 14.5), held to vim 9.2 itself: the `ui-vim-oracle` arm runs a corpus of 7,056 cases and `ui-vim-random` 1,600 seeded sequences of whole commands through the mode and through headless vim fed the same keys as typed, text and cursor compared byte for byte, CRLF, tabs and letters past ASCII among them; both skip without vim on the path. See `docs/ui.md`. |
| `ui_typing.c/h` | The platform's editing conventions (spec 14.2, shared since 14.4), private: word classes and word moves (to a word's end on macOS, the next word's start elsewhere), the word round a point, a character stepped without reading past its run, the paste cleaner, and the state of a run of presses and of a caret's blink. See `docs/ui.md`. |
| `ui_draw.c` | The UI's own draw list: quads -- a whole picture or, since spec 13.43, a part of one (`ui_draw_textured_quad_uv`, the whole-picture quad being that quad over 0..1) -- 9-slice, rounded rects and SDF glyph runs batched by (texture, program, clip) into one vertex buffer, drawn as an engine overlay AFTER tone mapping -- so a menu is never graded, bloomed, grained or rescaled by `--render-scale`, and IS captured by a headless screenshot, which is what makes a menu golden possible. See `docs/ui.md`. |
| `app.c/h` | App helpers, and since 12.19 NEITHER is a camera: `CameraDrag` turns a pointer into what a `camera_rig.h` rig is told (a drag is an aim, a shift-drag an anchor, the wheel and keys a distance), and `PlaneDrag` does the same for a plane under an orthographic camera while keeping its own arithmetic (it was `CanvasController` until spec 14.0, renamed so the 2D canvas could have the name). See `docs/camera.md`. |
| `camera_rig.c/h` | What MOVES a camera, and what the CONTROLS mean under it (spec 12.19). See `docs/camera.md`. |
| `gait.c/h` | A first-person walker's gait, as its eye and its ears know it (spec 13.47): the stride, the footfalls, the eye's lag on the ground's height, the head's bob and sway, and the pace a person keeps on a climb, for a walker with no body to animate. See `docs/camera.md`. |
| `anim_graph.c/h` | What PLAYS, and WHEN (spec 12.20) -- `animator.h:27`'s stated non-goal closed from OUTSIDE rather than by amending it. See `docs/animation.md`. |
| `cook.c/h` | The derived-data cook (spec 11.99): a transparent content-addressed cache over the deterministic startup bakes it wraps — the UE DDC model — plus the `--cook` pre-warm verb on forest and render. See `docs/architecture.md`. |
| `physics_cook.h/.cpp` | Jolt shape serialize/restore behind a C header — the one-C++-TU escape (`cluster_build.cpp`'s precedent), because JoltC binds none of Jolt's serialization. See `docs/architecture.md`. |
| `config_snapshot.c/h` | The live session as JSON (spec 11.71): ~230 settings dumped and restored. See `docs/architecture.md`. |
| `json_util.h/.c` | The cJSON shapes more than one reader needs (spec 12.3): `json_string_or` and `json_int_or`, hoisted out of `config_snapshot.c` the day a third cJSON reader appeared as its own comment invited, plus `json_add_float` — a float at `%.9g` through a RAW item, which is the exact round-trip width for binary32 where cJSON's own `%1.15g` is sized for a double and writes `0.3f` as `0.300000011920929`. See `docs/architecture.md`. |
| `util.c/h` | Path handling, GL error checking |
