# Cetra Graphics Library

A C11 PBR rendering engine on OpenGL 3.3+ / 4.1 (GL 4.1 is the hard ceiling on
macOS -- no compute shaders). Forward-rendered into a fat HDR MSAA G-buffer,
followed by a large screen-space post-processing stack. Scene-graph core, with an
optional game framework layer (fixed-timestep loop + Jolt physics + entities).

## Planning Workflow

When a new plan is approved (ExitPlanMode), before writing any implementation code, do these
steps in order:

1. **Branch off master.** If currently on `master`, create a new feature branch off it. If NOT
   on `master`, STOP and ask the user how to proceed -- do not branch off or commit onto an
   unexpected branch.

   **ALWAYS prefix the branch name with the spec number it implements.** The branch name is the
   spec's `specs/` filename with the `.md` dropped, so the two cannot drift apart and anyone on
   the branch can find the document that explains it. A branch named for what the work felt like
   at the start stops describing what it turned out to be, and once the name has drifted nothing
   connects the branch to its spec. Rename the branch whenever the spec is renamed; if the work
   turns out to be two specs, split the branch so each keeps its own number.

   **When the work wants a worktree, use the `EnterWorktree` tool, not `git worktree add`.**
   It puts the checkout in `.claude/worktrees/<name>` and switches the session into it;
   `ExitWorktree` leaves, keeping or removing. `git worktree add` at a path of your own
   choosing leaves the session in the ORIGINAL tree, so every subsequent edit lands on
   master while the branch you just made sits untouched -- and a path inside the repo needs
   a `.gitignore` entry it does not have, so the checkout shows up as untracked in the
   parent's `git status`.

   **A worktree gets these instructions automatically.** `AGENTS.md` and `CLAUDE.md` are
   TRACKED, so a fresh checkout contains them and nothing needs symlinking back. They were
   gitignored until they were not, and the old instruction to `ln -s` them into the worktree
   is gone with that -- doing it now would leave a symlink shadowing a real tracked file.
   `CLAUDE.md` is committed as a symlink to `AGENTS.md`, so there is still exactly one copy
   of this content and an edit through either name is the same edit.
2. **Copy the plan into `specs/`** as a markdown file.
3. **Commit the plan** (the `specs/` file, on the new branch) as its own commit, before touching
   any code.
4. **Create a task list** with the task tool -- one item per major step of the plan. ALWAYS update
   the task list as you go and complete items. Do not forget to update the list as you go.
5. **Start the implementation.**

**If the work has a look, the plan needs a step where somebody looks at it, and that step is not
the last one.** A plan made of instruments produces instruments. Spec 12.9 put "build the
instrument first" at phase 0 and "watch it" at phase 6, and what shipped was six green phases and
a demo that could not show the feature -- found in five minutes by the person who asked for it,
after seven review agents and nineteen passing arms had not. Run the app at the end of each phase
that changes what it draws.

## Build System

```bash
./build.sh        # Build library + all apps (CMake + Ninja)
```

The build script runs CMake to configure and Ninja to build. Output goes to `out/`
(`out/bin/<app>`).

**Using the engine from another project** (spec 11.105): `add_subdirectory` on a git submodule,
or FetchContent. `README.md`, "Using cetra in your project", has the CMakeLists. There is no
install; the comment at the end of `cetra/CMakeLists.txt` says why the old install rule was
deleted. Three lines make the out-of-tree build work, and each could be mistaken for its
neighbour: the output directories use `PROJECT_BINARY_DIR`, which is `out/` in this tree and the
engine's own subdirectory of a parent's build (the draco include path stays `CMAKE_BINARY_DIR`,
because draco writes there); `CETRA_BUILD_APPS` defaults to `PROJECT_IS_TOP_LEVEL`; and
`shader_strings.h` is included only from `program.c`. To check the path, build a throwaway
consumer in a scratch directory: the README's CMakeLists with `add_subdirectory` pointed at the
checkout, plus a `main.c`. Then look for `build/bin` or `build/lib` appearing, and for the
generated header in `ninja -t deps` for the consumer's object. On this Mac: 669 steps, 36 s cold.

**clangd / editor diagnostics:** the compilation database lands in `out/`, which is
neither place clangd looks (beside the file, its ancestors, or a `build/` child), so a
committed `.clangd` at the repo root points it there. Without it clangd guesses flags,
fails to find `cglm/cglm.h`, turns every `vec3` into an unknown type, and buries the real
diagnostics under an error-limit cutoff — a wall of red that says nothing about the code.
If you see that, check `.clangd` survived before believing any of it. The BUILD is
unaffected either way and always was.

**Compiler:** clang with `-Wall -g -std=gnu11 -pthread`, plus `-DGL_SILENCE_DEPRECATION` **on
macOS only** (`CMakeLists.txt:239`, a `PLATFORM_ID:Darwin` generator expression — the same
scoping trick as the define below, and for the same reason).
`CMAKE_C_STANDARD 11` leaves `C_EXTENSIONS` at its default of ON, so the emitted
flag is `gnu11`, not `c11` -- GNU extensions are available.

The `cetra` target compiles with **`-D_POSIX_C_SOURCE=200809L` and `-D_DEFAULT_SOURCE` on LINUX
ONLY**, through a `$<$<PLATFORM_ID:Linux>:...>` generator expression
(`cetra/CMakeLists.txt:109-111`). The scoping is the thing to know, because the define does real
damage where it is not wanted: on macOS it pins `__DARWIN_C_LEVEL` to the POSIX level and hides
the Darwin extensions -- BSD types like `u_int` (which `<sys/sysctl.h>` is declared in terms of)
and constants like `_SC_NPROCESSORS_ONLN` stop being declared at all. That is why it is not set
there. `util.c` still defines `_DARWIN_C_SOURCE` before its first include for `get_cpu_cores`, but
that is now belt-and-braces against the define coming back rather than a fix for anything the
build currently does. Note the failure mode is an "undeclared identifier" compile error -- and
wrapping the call in `#if defined(...)` turns that error into a silently wrong answer at runtime.

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
   camera's view and projection matrices**, derived by the engine from the pose
   the hook left (spec 11.107). See below -- this is a one-statement-wide window
   and all three of its edges are load-bearing.
5. **GI probe captures**, while the volume is dirty.
6. **Shadow depth pass** (`render_shadow_depth_pass`) -- gated on
   `scene->shadow_system->enabled`; runs before the scene FBO is bound.
7. Bind scene MSAA FBO, set supersampled viewport + draw buffers, clear.
8. Async texture uploads (<=5/frame), mask-array build, POM height resolve.
9. App render callback -> `engine_render_scene`, or `engine_render_scene` itself
   when the app passed no callback (spec 11.107).
10. `engine_present_frame` -> PostFX chain, then the app's **overlay hook**
    (`engine_set_overlay`, spec 12.2 -- a general post-tonemap draw, which the
    game UI installs itself into; the engine never learns what a `UISystem` is),
    then the debug GUI, which is a developer's overlay and belongs on top of the
    game's. With nothing installed the hook is a NULL compare, which is why all
    33 goldens are 0 px with it in place.
11. Screenshot capture (headless/CI), swap buffers.

**Step 4 is where an app puts anything the frame's geometry depends on** (spec
11.96): its camera, and any node it adds, removes or moves. It exists because
the walk used to be each app's own call in its RENDER callback, i.e. after the
shadow pass -- so every shadow in every app was drawn from LAST frame's
transforms and every LOD level chosen from stale positions. `apps/render` also
stepped its skeletal pose there, so a rig's shadow lagged its body for the same
reason; both halves moved together. **The camera is read right after the hook
returns**: the engine derives the view and projection matrices there (spec
11.107), so an app writes a pose and nothing else. Every app used to rebuild
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
TAA off, thirty of them static scenes under a static camera, the thirty-first
a rig posed purely by frame index, measured, and the last two menus over a sim
that has advanced thirty steps. The last pair are the only ones with motion in
them at all, and TAA being off is what keeps even those blind to it -- which is
what the `transform` and `shadow-lag` gate groups exist for.

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
measurement in 11.33): SSR shades only surfaces the catcher marked with a NEGATIVE
normals-buffer alpha, whose magnitude is already the catcher's edge falloff, so there is
no room in that channel for a second reflective surface. `--no-ssr` moves **0 px** on a
water frame. Water's reflection is its own split-sum lookup inside `water_frag`, which
already selects its mip from the per-texel roughness -- so giving SSR per-texel roughness
buys water nothing, and would break the catcher, which writes a zero there.

**PostFX chain** (`postfx_run`): MSAA resolve -> OIT composite -> G-buffer resolves ->
contact shadows -> GTAO + SSGI sweep -> split spec-occ composite -> the TAA seam
(TAA at full scale; the TAAU upscaling resolve under `--render-scale`, which
brings the render-res frame to post res) -> SSGI denoise -> SSR (hi-z) ->
SSGI composite -> atmosphere (fog + aerial) -> SSS -> motion blur -> DoF ->
auto-exposure metering -> bloom -> tonemap + finishing (sharpen / color-grade /
vignette / gamma / grain) -> GUI. Everything before the seam runs at RENDER res
(post size x `render_scale`, 1 by default), everything after composites onto a
post-res canvas; at scale 1 the two are the same buffer. Debug render modes
take a passthrough blit and skip the whole chain.

**Defaults:** on = bloom, GTAO + specular occlusion, SSR, auto-exposure, NEUTRAL
tonemap, normals G-buffer, OIT + moment weighting. Off (present, lazily
allocated) = SSGI, fog, DoF, motion blur.

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

## Key Files

**Core**
| Module | Purpose |
|--------|---------|
| `engine.c/h` | Window, input, render loop, camera modes, G-buffer/MSAA, program cache, embedded PostFX. **Window MODES since 12.15** -- windowed, exclusive fullscreen and borderless, each at the monitor's CURRENT video mode, so no display's resolution is ever changed and there is nothing to restore after a crash; `--render-scale` stays the performance lever. Four things about it are easy to get backwards. `engine_set_window_mode` writes none of `win_width`/`win_height`/`fb_width`/`fb_height`: it calls GLFW, the framebuffer-size callback (their only writer after init) fires, and the frame top rebuilds -- so the switch adds no resize path, and a version that set them directly has two writers again. **It is IDEMPOTENT, and that is a fix rather than a nicety**: `settings_apply` pushes every value on every edit, so holding a volume slider reached it once a frame, and replaying a windowed placement moved the window to a rectangle latched long before -- the fix is both halves, the windowed rectangle kept current while the window is windowed so a replay lands where it already is, and an early-out when the mode and the monitor are both unchanged. It is REFUSED where there is no window, which is load-bearing rather than defensive: a frame whose size came from whatever display the machine has would not be reproducible -- which is why `engine_window_placement` is split out as a pure function of (mode, monitor rect, saved rect), that being the only half an arm can reach. And creation goes through the same switch: the window is made HIDDEN, moved, then shown, so a fullscreen launch never flashes a window first. `engine_monitor_index` is the one statement of what a stored display name MEANS -- NULL, `""` and any unrecognised name all give the primary -- and `engine_monitor_names` is the list, so no app keeps its own array, its own cap or its own unnamed-display fallback |
| `scene.c/h` | Scene graph, hierarchical transforms, owns lights/materials/particles/shadow/IBL/sky |
| `render.c/h` | Scene traversal + the ordered scene passes (opaque/skybox/transparent/OIT/particles) |
| `occlusion.c/h` | CPU masked occlusion culling (spec 11.98): authored proxies rasterised into a 256x144 fixed-point buffer from THIS frame's camera, one conservative test per item per frame. No GL in the module; conservative by four stated roundings, so a violated authoring contract (a box poking out of its mesh) is the only path to a wrong pixel — and the probe checks that too |
| `program.c/h`, `shader.c/h`, `uniform.c/h` | Shader program compile, uniform setup |
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
| `cluster.h`, `cluster_build.cpp` | Nanite's CPU half on GL 4.1 (spec 11.63): meshoptimizer's own `demo/clusterlod.h` behind a C API, in the one C++ TU the JoltC split already precedented. ~128-triangle clusters, grouped, each group simplified with its BOUNDARY LOCKED, re-split, re-grouped differently so the locked seams move. **Cracks are structurally impossible** — every cluster at every level indexes the ORIGINAL vertex buffer, since meshoptimizer only ever drops vertices and never introduces or moves one. That property is unconditional and holds either way; `simplify_permissive = false` buys something narrower and unrelated, namely refusing collapses ACROSS UV/normal seams, which a prop with seams needs. The cut is quantised by DISTANCE BAND, which is what keeps instancing alive: a per-instance cut means two trees at different distances can never share a draw. **On triangles it is PARITY with `lod.c`'s chain, not a win** (−2.7% / −0.1% / −1.4% at three framings) — boundary locking costs about what error-driven selection gains, and the deliverable is the seal |

**Lighting environment (on Scene)**
| Module | Purpose |
|--------|---------|
| `light.c/h` | Directional / point / spot / area lights |
| `shadow.c/h` | Cascaded shadow maps (CSM), PCF + optional PCSS |
| `ibl.c/h` | Split-sum IBL: irradiance + GGX prefilter + BRDF LUT |
| `sky.c/h` | Hillaire physically-based atmosphere (transmittance/multiscatter/sky-view LUTs + the per-frame aerial-perspective volume) |
| `probe.c/h` | ONE parallax-corrected reflection probe: capture, GGX prefilter, the box-projected lookup. Still the whole feature at count 1, where it binds into the IBL prefilter unit exactly as it always did |
| `probe_set.c/h`, `probe_atlas.c/h` | N of them (spec 11.70), blended PER FRAGMENT: a per-froxel 8-bit mask on the light grid says which probes reach a cell, each proxy box gives a weight, and the leftover weight falls to the global environment through the same expression the no-probe path uses. Storage is octahedral roughness ROWS (mips would filter across the tile gutters) TENANTING `giAtlasTex` on unit 14 — an atlas is a pool, so the second consumer cost a region and not a declaration. **The box fade runs OUTWARD from the proxy faces, and that is correctness rather than taste**: a floor lies ON the bottom face of the box that box-projects it, so an inward fade weighs every floor in every scene at exactly zero and hands the one surface the feature exists for back to the environment — which renders as a plausible frame, because a floor reflecting nothing and a floor reflecting a dim room are the same picture. **Per-DRAW selection is the refused alternative** and the fixture is built to refuse it: two rooms over ONE floor mesh, which is a single draw, so a per-draw design lights half of it with the wrong room's reflections while every per-room measurement still passes. Cap 8, set by VRAM (~4.3 MB per probe column) and not by the mask, which is one byte per froxel |
| `light_cluster.c/h` | Clustered forward light culling: 16x8x24 frustum grid, std140 light UBOs |
| `ltc.c/h` | Linearly Transformed Cosines tables for rectangular area lights (on Engine) |
| `emissive_light.c/h` | Derives LTC area panels from emissive meshes (spec 11.49, `--emissive-lights`) |

**Post-processing (on Engine)**
| Module | Purpose |
|--------|---------|
| `postfx.c/h` | The whole HDR post chain: bloom, tonemap, exposure, TAA, GTAO, SSGI, SSR, SSS, fog, motion blur, DoF, OIT composite |
| `lut.c/h` | Adobe `.cube` colour-grading LUT reader (spec 11.58); refuses by name, never truncates |

**Assets & animation**
| Module | Purpose |
|--------|---------|
| `import.c/h` | Assimp-based FBX / OBJ / glTF-GLB loading |
| `animation.c/h` | Skeletal animation: clips, skeletons, and the POSE seam (spec 12.1) -- `animation_sample_pose` evaluates ONE clip into per-bone TRS with its retargeting and hierarchy in per-call scratch, `pose_blend` mixes two, `animation_state_apply_pose` turns one into `bone_matrices[128]`. A bone no clip drives copies its bind MATRIX rather than a decomposed rebuild, which is what keeps the single-clip path bit-identical. Prev-pose for TAA motion vectors. **`animation_stride_speed` (spec 12.10) is the one thing here that asks what a clip MEANS rather than what it holds**: the ground speed its own feet imply, in MODEL units, from a loop sampled at 60 Hz. A foot is standing while it is in the lower HALF of its own vertical range, and the answer is the MEDIAN horizontal velocity of the toe over those samples -- a median rather than a fit over a window, because the samples need not be contiguous and a window has to be found before it can be used, which is a thing that can be found wrong. It was: at a tight band a real stance split into three separate runs on a rig whose hip translation the retarget had replaced by its bind position, and the two feet of `strut_walk` came out 23 per cent apart where the median has them 0.7 per cent apart. It **REFUSES** when the clip does not walk, and that is a result and not a gap -- a straight-leg pendulum has both feet lowest at the same instant swinging opposite ways, so their low samples carry both directions and median to nothing. The two feet must agree in direction and magnitude, and the published answer is the one weighted by how long each stood |
| `animator.c/h` | What PLAYS on a skeleton (spec 12.1), and how one thing becomes another: a phase-synced 1D blend space (every entry on one clock, so a foot planted 40% through the walk is planted 40% through the run), a crossfade that drops the outgoing source on completion so the settled pose is the incoming one exactly, one bone-masked override layer that releases itself, and clip events dispatched AFTER the pose is applied -- so a handler may play something. No state machine: a game decides what plays, this fades and blends it. **Since 12.10 an entry also carries what it implies about the GROUND** -- a `stride`, in the units of the caller's own param axis, which `animation_stride_speed` measures from a clip and a rig. `animator_stride_speed` blends it, and that call is the whole reason this lives here: the space keeps ONE clock, so the blended pose lays down `sum(w * stride * seconds)` of ground per turn of the loop while the clock turns the loop at `sum(w / seconds)`, and the answer is the PRODUCT. It collapses to the weighted mean of the strides only when every clip is the same length, which a walk-and-run space is exactly not, and a game reaching for the mean is wrong by an amount that reads as a tuning problem. Divide the speed a game wants to travel at by that and write it to `speed`: that is stride matching, and it is what makes foot locking work at a speed other than one. **A stride of 0 means the entry lays down no ground**, which is true of a standing clip and is also what an unmeasured entry carries; the two are deliberately not distinguished, because an idle at 0 is what lets a speed axis reach zero and only the CALLER knows which of its entries were supposed to move. **The phase sync is a trap for clips of very different LENGTH**: a two-tick rest pose beside an 86-tick walk turns the shared clock 43 times too fast for the walk, which is a held frame with no phase to lose ruining one that has -- give it the other's period |
| `rigging.c/h` | Semantic bone matching + cross-rig retargeting (not GPU skinning) |
| `springbone.c/h` | Procedural secondary motion (Verlet) on un-animated bone chains |
| `ik.c/h` | Two-bone IK for foot planting and LOCKING (specs 12.4 and 12.9): the law of cosines over a hip/knee/ankle chain, spliced where spring bones are and **after** them, between the pose's globals being accumulated and the skinning matrices being built -- the one place per-bone MODEL-space positions exist. Writes `global_transforms` and nothing else, because a bone no clip drives has `driven[]` clear and a rotation written into the Pose is discarded, and this rig's locomotion never bends a knee. The bend plane comes from a POLE carried in the hip's own frame rather than from the current triangle, since a bind pose that is exactly straight leaves that triangle degenerate on nearly every frame. **The pole is asked of the RIG before the caller since 12.11**: a bind knee sits off the hip-ankle line by exactly the amount and in exactly the direction its author meant it to bend, so the perpendicular part of that offset IS the pole, and `ik_add_foot`'s `knee_forward` is the FALLBACK for a rig that binds straight. It had to be, because the argument is in the hip's frame and every caller in this tree passes the `+Z` that suits the generated puppet -- on a humanoid whose thigh binds 178 degrees away from it, an ordinary difference between two riggers, the same vector points backwards and the knee folds under on every stance frame with every chain resolved, every length sane and nothing warning. Whether a foot is planted at all is decided HERE and never by the caller: only here are the globals still the clip's pose, and a caller reading them sees its own last solve, so a planted foot reads as planted and the feet weld to the ground. **Foot LOCKING since 12.9, and it is a layer in FRONT of the solve rather than a change to it** -- the solver still receives a point and neither knows nor cares whether it came from a live ray or a contact frozen forty frames ago. A contact is labelled from the ankle's lift and vertical speed at the seam (the toe joint passes back through its own bind clearance mid-swing on a retargeted clip, which is why detection is not there), pinned at the TOE in WORLD space, and held until the label ends or the clip has carried the foot past `unlock_distance` -- which is the LARGER of the two thresholds, and that gap is the whole hysteresis. The stance foot travels 0.0034 m against planting's 0.0783, over the same ticks. Three things about it are easy to get backwards: `ik_set_world` is what a caller owes for the world half, and without it a held point rides along with the character and the lock does nothing; the contact is captured AFTER the solve, since a point taken from the clip's pose is somewhere the foot is not yet and the lock spends its first ticks dragging the foot onto it; and a locked foot takes its weight UNFADED, because `plant_fraction` answers "is this foot down" from height alone and the contact label is both better and stricter. `docs/foot-locking.md` carries the method, what shipped and where the two differ. **Everything in here is MODEL space and a fraction of the leg, and `ik-scale` is what says so** (spec 12.10): the same walk on a node scaled 1 and 2 must come out identical, because a scale reaches the world matrix and nothing the solver decides. 12.9 shipped a live bug through exactly that gap -- a world distance against a model threshold, halving the unlock distance at the app's own `PLAYER_SCALE 2` -- and no arm could see it, every other case in the probe running at scale 1. **And a lock inside its unlock distance holds the foot still WHATEVER the body is doing**, so slide does not measure whether locking is working: at half the clip's speed the foot does not travel while the leg is hauled a third of a metre from the pose, which is this method's own T-Rex warning arriving sideways. The distance between the solved ankle and the one the animator asked for is what sees it, and `ik-slide-speeds` reads that. **Two target setters since 12.5, and the difference is not cosmetic**: `ik_foot_set_target` takes a point for the ANKLE, `ik_foot_set_ground` takes the SURFACE and adds `sole_offset` -- the ankle's clearance above its own sole, derived once from the bind pose at `ik_add_foot` rather than by each caller. Folding that into one setter was tried and is wrong: most call sites pass arbitrary geometry (a reach target, a full extension, an exact distance for a closed form), and a sole clearance displaced every one of them by the offset. A target and a ground are different claims and only the caller knows which it holds. `max_pelvis_drop` is likewise a FRACTION of the asking foot's leg length since 12.5, not metres, so it carries to a rig of another size -- the same units `plant_fraction` uses, and 12.9 halved it to 0.31, set from the deepest thing the demo world asks rather than from taste. **The exponential soft-clamp on extension the method prescribes is REFUSED here**, and the refusal is about the RIG: a soft band must begin below full extension to smooth anything, and near full extension the knee angle goes as the square root of the shortening, so on a bind pose that is exactly straight 1 per cent of band costs 16.2 degrees of permanent bend. Re-measure on a rig that binds bent rather than inheriting that |
| `ragdoll.c/h`, `ragdoll_jolt.cpp/h` | A rig falling over (spec 12.16), and the first thing in this engine where animation and physics meet. Twelve capsules built from the BIND pose -- length from a bone to its child, radius from `Mesh.bone_aabb`, the per-bone boxes 11.53 added for culling -- started from the LIVE pose so the heap continues the character's motion, and written back at the seam `ik_solve` occupies. **It REPLACES the pose rather than correcting it**, which is why it runs last of the three: a foot planted on ground a falling body is no longer standing on is not a correction worth keeping. Globals only, for `ik.h`'s reason. **Two bones cannot be resolved by name and are WALKED instead**: `categorize_bone` gives one category to a whole chain -- `BCAT_SPINE` covers spine and chest alike, `BCAT_HEAD` covers `Head` and `HeadTop_End` -- and the semantic matcher answers only when exactly one bone matches, so on a segmented spine or a Mixamo head it correctly refuses. The name does not answer the question; the hierarchy does, and `find_category_under` asks it three times. **The body count is therefore a MEASUREMENT, not a constant** -- a one-segment spine yields eleven -- which is why `ragdoll-build` asserts structure (every bone resolved, constraints == bodies - 1) and reports the number. Everything is MODEL space scaled once, 12.10's rule, and the node scale has to come back OUT of the basis on the way back: `to_model` carries 1/scale and a Jolt body carries none, so their product skins a character at half size and reads as an explosion rather than a scale bug. `ragdoll_jolt.cpp` is the THIRD one-TU C++ escape after `cluster_build.cpp` and `physics_cook.cpp`, in `cetra/src/` root because the build globs C++ only there. It refuses rather than asserts: parents must precede children (`CreateRagdoll` indexes the parent's body while building the child), only part 0 may be a root, and a degenerate capsule is named -- `CapsuleShape` asserts its dimensions and Jolt's default Trace is a breakpoint. `SwingTwistConstraintSettings` is not a free choice: it is the only type `DriveToPoseUsingMotors` accepts, so it is what keeps a getting-up spec possible. And **removal precedes release** -- `~Ragdoll` destroys its bodies without removing them, so dropping the last reference while they are added takes the process down |
| `async_loader.c/h` | Background texture streaming (pthread pool sized `get_cpu_cores() - 2`, clamped to [2, 8]) |

**Procedural terrain (`cetra/src/procedural/`, the subsystem `apps/forest` is built on)**
| Module | Purpose |
|--------|---------|
| `terrain.c/h` | fbm heightfield, tiled mesh + collider build, per-vertex slope/altitude tint. Since 11.59 the height has two SOURCES behind one pure function: the fbm, or a Catmull-Rom sample of a stored `TerrainField`. Since 11.60 `terrain_ground_classes` is the ONE statement of what the ground is made of — rock / silt / channel from slope and the erosion masks — read by the vertex tint, by `terrain_bake_splat` and (through the exported channel band) by the scatter, so the three cannot drift. They were three copies of four thresholds, and the two paint copies are never live in the same frame, so a drift would produce no contradiction to notice |
| `erosion.c/h` | Mei virtual-pipes hydraulic + thermal erosion over a field (spec 11.59). CPU, Eulerian, double-buffered, threaded like the cloud noise bake — **bit-identical at any worker count**, which is the constraint that rules out droplet erosion. Produces the flow / deposit / wear masks, which are the point as much as the silhouette |
| `heightmap.c/h` | `.r16` read + write and 16-bit PNG read (spec 11.59). Deliberately not through `texture.c`, whose format table cannot request 16-bit at all |
| `terrain_stream.c/h` | The stored field paged off disk (spec 11.69, D4): an fp32 tiled `.cts` file and one rectangular WINDOW resident per pyramid level, anchored on the camera and the player. **Windows rather than 11.67's page table, and the choice is about ACCESS PATTERN not scale** — every heavy consumer walks a contiguous square and `sample_plane` runs ~30k times per patch build, so a 4-compare containment test beats a lookup per corner of a 4x4 footprint straddling up to four tiles; and a window's position is a pure function of the anchor path, which deletes the eviction ordering, want-guard and hysteresis rather than re-implementing them. The coarse tail stays whole, so a miss is ANSWERABLE: a query whose clamped footprint escapes a window retries one level coarser (twice the world each step) and gets exactly what that level returns unstreamed. **The file stores the pyramid already in memory, never re-filtered**, or that equality dies while level 0 stays green. **A window is `n*tile + 1` nodes** for the same reason a level is `2^k + 1`; sized `n*tile` the far edge of the world is unreachable and everything built there comes from the fall. Masks fall to a coarse plane in the file and that answer is an APPROXIMATION with no unstreamed twin — the height's is exact |
| `terrain_tex.c/h` | The four ground materials a layered terrain blends between — grass, rock, silt, gravel (spec 11.60). Pure CPU, `vegetation_tex.h`'s malloc-and-return contract, and **every field is periodic**: a ground tiled across a kilometre from unbounded noise prints a hard grid at every boundary. Packed for the material texture array (albedo + height, normal + roughness + AO), because that packing is a contract with `layers.glsl` and a second place that knows it is a second place to get it wrong. Threaded on `cetra_bake_bands` like the cloud and erosion bakes, rows within a layer — `veg_noise_seed` rewrites a file static, so the seeding happens once before the split |
| `terrain_quadtree.c/h` | A CDLOD quadtree over the terrain's domain (spec 11.63), replacing the fixed `tiles x tiles` grid: fine patches near the camera, coarse ones away, so the patch count tracks how many LEVELS there are and not how much ground they cover. Patches are CPU-built off `terrain_height_at` and cached — the alternative, one shared unit grid displaced by a height TEXTURE, makes the drawn surface a different function from the one the collider and the scatter agree through. **`TERRAIN_SPLIT_FACTOR` is a proof, not a knob**, and it is written down beside itself: two separate bounds (adjacent patches never more than one level apart, and a seam vertex at factor 1 from the fine side while the coarse side is still at 0) both require f ≥ 2√2 |

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
| `game.c/h` | Fixed-timestep loop, init/update/render/shutdown callbacks, ticks particles + physics |
| `physics.c/h` | Jolt Physics: rigid bodies, raycasts, sweeps, 5 constraint types + motors. **Jolt's default `Trace` is `DummyTrace`, which is `JPH_ASSERT(false)`**, so any condition Jolt merely wants to REPORT takes a breakpoint instead. The one met so far is `SHAPE_MESH` over a large run of exactly coplanar triangles — the tree builder cannot split them, falls back to a random split, and traces. Spec 11.63's island hit it with a perfectly flat sea floor and fixed it at the geometry. **Two things this entry claimed for a spec cycle and got wrong, both narrowing it.** The trace is `JPH_IF_DEBUG`-wrapped (`AABBTreeBuilder.cpp:205`), so the death is **debug-build only** — a release build indexes the same degenerate soup silently and badly, which is the worse half. And `JPH::Trace` / `JPH::AssertFailed` are **assignable `JPH_EXPORT extern` pointers**, so a handler absolutely can be installed; what JoltC lacks is a C entry point for doing it, and a C++ TU has been precedented since `cluster_build.cpp`. Nothing installs one today, so the geometry fix is what is load-bearing — but it is the right place because flat collision geometry is bad regardless, not because it was the only place reachable. **What JoltC does not bind is a longer list than it looks, and spec 12.16 shortened it**: `Ragdoll/`, `Vehicle/` and `SoftBody/` are all vendored and compiled, and a grep for them across `JoltC/` returns a single enum tag. Ragdoll is now reached through `ragdoll_jolt.cpp`; vehicles and soft body are still unbound and fixable exactly the same way. **Before costing anything against "Jolt does not do that", check whether JoltC merely fails to expose it** — that distinction was months of imagined work on the roadmap |
| `entity.c/h`, `component.h` | ECS-lite entities + components. `RIGID_BODY`, `CHARACTER`, `ANIMATOR` (12.1) and `AUDIO_SOURCE` (12.0) are implemented; `MESH_RENDERER` is a bare enum line -- an entity's visual is its `node` |
| `character.c/h` | Character controller on Jolt `CharacterVirtual` |
| `animator_component.c/h` | The `ANIMATOR` component (spec 12.1): an `Animator` the entity owns, its pose bound to the entity's node, ticked once per RENDERED frame from `game_pre_render` with the sim clock's delta -- never per fixed step, because the tick begins with the prev-pose latch and two in one frame would lose a step's motion vector. A paused sim passes 0 and the rig holds, reading zero deformation velocity |
| `input.c/h` | The game layer's input, polled once a frame before the fixed steps: keys, mouse, up to four gamepads in GLFW's standard layout behind a reader seam (GLFW, or a scripted pad from a text file), and the action table a game reads instead of key codes (spec 11.109). Every device's state is one struct held twice, this frame's and the previous frame's, and every edge -- a key's, a pad button's, an action's -- is the two compared; a pad that appears has its previous state set to its current one, which is the whole connect rule and reaches an action for free. An action is evaluated on read, so there is no cache, no cap and no ordering to get wrong |
| `settings.c/h` | What a PLAYER chose, persisted across runs (spec 12.2): the four bus volumes, window mode and vsync, as ONE descriptor table walked in both directions -- `config_snapshot.c`'s technique, because a writer and a reader maintained separately drift silently. It could not be rows in that file, whose owner enum has no Game and no AudioSystem. `settings_default_path` resolves the platform's own per-user location (`%APPDATA%`, `~/Library/Application Support`, `$XDG_CONFIG_HOME`), which nothing else in this engine does -- every other `fopen` writes relative to cwd; `CETRA_SETTINGS_DIR` overrides it, which is what keeps a gate and a golden bake hermetic. Input bindings are NOT carried: `input_bind` takes a borrowed const table, so persisting a rebinding wants the remapping UI it would exist for. **Window mode APPLIES since 12.15**, through `engine_set_window_mode`, and the file gained a monitor NAME to go with it -- an index renumbers when a display is unplugged and silently moves the game to another screen. The mode is `EngineWindowMode` itself, not a mirror of it: `settings.h` includes `engine.h`, which is legal and always was (everything under `game/` is public and `game.h` already did it), and a spec cycle was spent asserting otherwise and paying four static asserts to route around a wall that was not there. **Check an include boundary before conceding to it.** `borderless` was APPENDED to the mode labels rather than slotted between the existing two: the index is the value and the value is written as a name, so a label inserted in the middle re-points every file already on disk -- the one surviving assert holds the label count against `ENGINE_WINDOW_MODE_COUNT`. vsync goes through `engine_set_vsync` rather than `glfwSwapInterval` here, which was a second writer of a value the engine did not keep and therefore could not put back after a mode change dropped it |
| `save.c/h` | Where the player WAS, persisted across runs (spec 12.3). The THIRD descriptor table after `config_snapshot.c` and `settings.c`, and neither could have carried it: that file's owner enum has no Entity, no RigidBody, no CharacterController and no Animator, and settings are what a player chose rather than what they did. Entities match by NAME (an id is `next_id++`, so creation order decides it and a flag changes that), components are keyed by name and never by `ComponentType`, whose `1u << type` renumbers on a removal. A row may carry a GET/SET pair instead of an offset, which is what reaches a body's velocity inside Jolt -- and the pose is pushed back INTO the body, since `entity->position` is a copy `sync_physics_to_entities` rewrites every step. **A restored world is deliberately NOT bit-exact**: Jolt's solver state stays out, or the format would pin to `JPH_VERSION_ID` and break every save on a physics upgrade. One rule orders the failures -- file-level problems refuse the file, record-level problems drop the record and count it in `SaveLoadResult`, so one unbuildable crate never costs a player the rest. A spawned entity carries the recipe that made it (`save_register_spawner`, `save_note_spawn`), and a registered name is permanent: retiring one leaves a tombstone. Versions are PER SECTION with a migration chain that runs on the parsed tree before the descriptor walk, so a migration written today still behaves in three years |
| `audio.c/h` | The game layer's audio (spec 12.0): one output device wrapping miniaudio's high-level engine, wrapped as an `AudioSystem` the `Game` owns like the physics world. 2D fire-and-forget SFX and music, held voices from a file (WAV/MP3/FLAC) or a procedural tone, mixer buses, and 3D positional sound whose listener is the camera and whose sources are `AUDIO_SOURCE` components synced from their entities. The device is the seam: a windowed run opens the OS device, a headless run opens NONE and renders offline through `audio_system_read_pcm`, so everything above the device is deterministic and the `audio` gate group asserts on it with no hardware. miniaudio (single-header, vendored) carries its own CoreAudio/ALSA/WASAPI backends |

**Support**
| Module | Purpose |
|--------|---------|
| `text.c/h` | SDF text rendering (stb_truetype) with glow |
| `ui.c/h` | The game UI layer (spec 12.2): a RETAINED tree of elements over a two-pass box layout, a geometric focus model, and a theme whose every zero means "inherit". The element list is CLOSED -- panel, label, button, toggle, slider, selector -- and three escape hatches sit under it, each keeping strictly more than the last: `ui_set_draw` keeps layout, focus and input while the app paints; `ui_set_element_program` keeps all of that and swaps only the fragment stage; the draw-list primitives take neither. Everything is in WINDOW POINTS, top-left origin, +Y down -- the space `text.c`'s ortho and GLFW's cursor already agree on, and deliberately not the engine's `InputState` space. `ui_layout` is a pure function of (tree, width, height): no GL, no clock, no input, which is what lets ten gate arms assert a menu with no GPU |
| `ui_draw.c` | The UI's own draw list: quads, 9-slice, rounded rects and SDF glyph runs batched by (texture, program, clip) into one vertex buffer, drawn through `engine_set_overlay` AFTER tone mapping -- so a menu is never graded, bloomed, grained or rescaled by `--render-scale`, and IS captured by a headless screenshot, which is what makes a menu golden possible. Reuses `Font` and `font_get_glyph` from `text.c` and nothing else of it: that file is one draw call per mesh, hardcodes its SDF constants and clobbers GL state rather than restoring it |
| `app.c/h` | App helpers: the mouse-drag orbit controller for a 3D viewer and its 2D twin, the canvas controller (pan, drag a picked node in the plane, zoom about the cursor; spec 11.108), a three-point light rig, input gating |
| `cook.c/h` | The derived-data cook (spec 11.99): a transparent content-addressed cache over the deterministic startup bakes it wraps — the UE DDC model — plus the `--cook` pre-warm verb on forest and render. THE KEY IS THE IDENTITY (input bytes + recipe version + library version where a library owns the byte format), so a stale artefact is unfindable rather than detected; a miss always bakes live; a corrupt `.cca` is refused by name against a payload hash and treated as a miss. Process-global behind `cook_init`, main-thread-only, `cooked/` gitignored at the repo root, `CETRA_COOK_DIR`/`CETRA_NO_COOK` the env levers. **Never fold a worker count into a key, and never measure a bake without `--no-cook`** — gates and goldens isolate onto per-run cache dirs automatically. What may NOT be cooked is stated in the header charter (GPU resamples, GL handles, the scatter) |
| `physics_cook.h/.cpp` | Jolt shape serialize/restore behind a C header — the one-C++-TU escape (`cluster_build.cpp`'s precedent), because JoltC binds none of Jolt's serialization. Exports `JPH_VERSION_ID` as the mandatory cook-key axis. The stream classes carry istream EOF semantics, and that is load-bearing: Jolt checks `IsEOF()` after a stream's LAST field, so a positional implementation refused every restore ever written (the 11.99 ledger's caught-live row) |
| `config_snapshot.c/h` | The live session as JSON (spec 11.71): ~230 settings dumped and restored. **ONE descriptor table, walked in both directions** — the writer and the reader cannot list different fields because there is only one list, which is the drift `render.c`'s `frame_schedule` comment records having lived with. Materials ride `MATERIAL_PARAMS` rather than rows of their own, so a property added there is carried for free. What it OMITS is as load-bearing as what it carries: GPU handles, the lazy-alloc guards, the seven per-frame PUBLISHED blocks and the nine temporal histories are not configuration, and restoring one corrupts the frame rather than reproducing it |
| `json_util.h/.c` | The cJSON shapes more than one reader needs (spec 12.3): `json_string_or` and `json_int_or`, hoisted out of `config_snapshot.c` the day a third cJSON reader appeared as its own comment invited, plus `json_add_float` — a float at `%.9g` through a RAW item, which is the exact round-trip width for binary32 where cJSON's own `%1.15g` is sized for a double and writes `0.3f` as `0.300000011920929`. `cscene.c` is deliberately NOT a consumer: its `get_float`/`get_bool` are bool-returning PRESENCE tests, because a `.cscn` leaves an unspecified field at its engine default, and that is a different contract |
| `util.c/h` | Path handling, GL error checking |

## Dependencies

**Everything is VENDORED under `cetra/src/ext/` and built from source as a static library.**
There is no pkg-config and no vcpkg in this build -- GLFW (window/input), GLEW (GL extensions),
cGLM (math), Assimp (model import), Jolt Physics (via the `JoltC` C wrapper), Dear ImGui (via
`cimgui`) and meshoptimizer (LOD chains, cluster DAGs) are all `add_subdirectory` targets with
`BUILD_SHARED_LIBS OFF`. The only `find_package` calls in the tree are `OpenGL`, `Threads` and
`Git`, and `Python3` (REQUIRED — `gen_shader_header.py` runs at build time). This is what makes
the three-platform build work from one command; anything that reaches for a system library
instead has to answer for Windows.

**Header-only:** stb_image (textures), stb_truetype + stb_rect_pack (SDF text),
uthash (hash caches).

**Compiled straight into the `cetra` target:** cwalk (path handling), cJSON (the config
snapshot's reader/writer), log.c (logging), progressbar.c.

There is **no audio backend** -- `COMPONENT_AUDIO_SOURCE` is a declared-but-unimplemented
component slot.

## Shaders

GLSL sources live in `cetra/shaders/`. During build, `gen_shader_header.py` converts
every `.glsl` into a C string literal in **`out/generated/shader_strings.h`** --
the BUILD tree, not the source tree, so a read-only checkout builds and two build dirs
never fight over one file. Edit the `.glsl` sources, never the header.
`post_vert.glsl` is the shared fullscreen-triangle vertex shader for all post passes.

**The lit surface is not one program but a family of VARIANTS** (spec 11.93), and this is the
first thing to know before editing `pbr_frag.glsl`. `program.c` splices
`#define CETRA_PBR_FEATURES <mask>` in after the `#version` line
(`shader_source_with_defines`, plus a `#line 2` so every body line number survives), and the
shader gates six optional features on it. `render.c`'s resolver recomputes each material's mask
EVERY FRAME — the mask is a pure function of six material fields and three scene facts, all of
which a GUI slider moves with nothing marked dirty — and `engine_pbr_variant` compiles and
caches one program per distinct mask. `apps/forest` resolves to two: `pbr-32` for its layered
terrain, `pbr-0` for everything else.

Three rules the next editor needs:

- **The polarity is SUBTRACTIVE and that is structural, not stylistic.** No defines at all means
  the uber-shader, because `pbr_features.glsl` defaults the mask to `PBR_FEAT_ALL`. So a
  resolver that fails to run, or a mask never assembled, yields the SLOW program — never a fast
  one missing a feature its material needed. Wrong-and-slow is recoverable; wrong-and-pretty is
  what ships.
- **The C-to-GLSL contract is a shared NUMBER, not a shared name.** The first shape had C emit
  macro names (`#define CETRA_NO_SHEEN 1`) for the shader to `#ifdef`, and its failure was
  untypeable and silent: rename either side and the guard never fires, the picture stays
  correct, and the optimisation just stops happening.
- **What a gate is worth depends on WHERE it is.** Every feature gate is a dynamically uniform
  branch an unusing scene already skips, so guarding one at runtime is worth ~0.05 ms; removing
  the same code at compile time was worth 24. The cost is OCCUPANCY — live values, and declared
  samplers — which every fragment pays whether or not the branch is taken. A runtime `if` around
  a gated feature is therefore close to free and close to pointless.

**Two FAMILIES share that fragment source**, differing only in the vertex stage: `pbr` and
`pbr_skinned` (spec 11.95). One mask means the same thing in both, which is why a second family
cost a builder argument rather than a second set of gates. `instanced` still falls out of the
link — a skinned program has no `InstanceBlock`, so 11.28's rule that it never carries more than
one instance holds without anything asserting it. **Until 12.1 nothing in the golden corpus was skinned**, which made
`pbr-variant-skinned` the only instrument that could see that family at all; `puppet_golden` is
now the first picture in the corpus that goes through `pbr_skinned_vert` and `skin.glsl`.

**`include/noise.glsl` owns three hashes, and two shaders are exempt from using them.** `ign` is
interleaved-gradient noise for SCREEN-space dither; `hash21(vec2 p, vec2 k)` and `hash13(vec3 p,
vec3 k)` are the sin-fract value hash, with **`k` as a parameter** because water keys off
`(127.1, 311.7)` and tonemap's grain off `(12.9898, 78.233)` and both are right — sharing the form
costs neither a value, sharing a constant would have changed one. The exemptions are
`ssr_frag.glsl:211` and `contact_shadow_frag.glsl:232`, both inlining IGN in the explicit
multiply-add form: the include spells it as a `dot()`, which rounds differently, and the hash's low
bits steer a traced ray. **Migrating was measured at 31,800 px** — the same function re-associated,
under a configuration recorded nowhere but as "a no-TAA render", so read it as evidence that the
transformation is hostile rather than as a reusable price. Contact shadows are golden-covered, so
that one is a 0 px bet against it. `particle_sim_vert`'s `hash33` and `stochastic.glsl`'s
`_stochasticHash` are declined for arity and for dragging `stochasticLut` plus `dFdx` into any vertex
program that included them.

**Replacing the sin-fract form with an integer hash is REJECTED** (roadmap E10, row 38). The
diagnosis is sound and worse than the roadmap said — the `* 43758.5` amplifies argument error by the
same factor, so pinning the output to one 8-bit code demands 33 to 42 bits of precision from a 24-bit
type, and a 64x64 integer lattice hashes to 1047 distinct values of 4096 at the origin and 53 at
world offset 1e7. It is rejected anyway: the benefit is cross-driver reproducibility on a one-driver
machine, **no consumer has a correct pattern to regress against** (grain, wind phase, foam bubbles,
curl noise and tile offsets are all arbitrary by design), and **zero of the 27 goldens depend on any
of the four hashes** — both water goldens are Gerstner with no bed, so `foam` is identically 0 and
the bubble hash is never evaluated. Note `ign` is not one of the four and never was.

**THREE include files are compiled by BOTH languages, and there are rules.** `shore_constants.glsl`
(from `shore_runup.h`) and `wind_bounds.glsl` (from `wind.c`) exist because a number the GPU and the
CPU must agree on is the half that drifts -- the swash solver runs host-side, and the wind bound that
makes a swaying mesh cullable is a CPU bound on GPU arithmetic. Both are `#define`s only: **the `f`
suffix is load-bearing**, since an unsuffixed literal is a `double` in C and promotes every
expression it touches to a precision the shader does not have (11.53 shipped without it and the bound
evaluated in double for a spec cycle), and nothing in them may use a type, a function or a qualifier
-- `const`, `static` and `float[3](...)` each belong to one side only. Numbers only. If you are about
to invent this technique, you are not: read `shore_constants.glsl`'s header first.

**`pbr_features.glsl` (from `program.h`) is the third**, and it generalises the technique past a
shared constant to a shared VOCABULARY: the lit-surface variant bits (spec 11.93), which C emits as
a mask and the shader reads back through the same names. It carries no `f` suffixes -- these are mask
bits rather than amplitudes, and an integer means the same thing to both preprocessors -- and since
11.95 it also holds `CETRA_HAS`, a function-like macro over those numbers, plus the `#ifndef`
default that makes an un-spliced source the uber-shader. That is still "numbers only" in the sense
that matters: no type, no function, no qualifier. It lives there rather than in `pbr_frag` so a
CHUNK can gate its own sampler declaration -- `ltc.glsl` decides when `ltcTex` exists, which is a
thing only that file should know. C sees both macros and wants neither; they are inert on that side.

**Never regenerate it into `cetra/src/`.** That was the old location, it is gitignored,
and a quoted `#include` finds the including file's own directory first -- so an in-tree
copy silently shadows the build-tree one and the engine renders whatever that stale file
says. `cetra/CMakeLists.txt:8-12` refuses to configure while one exists; if you hit that
error, delete `cetra/src/shader_strings.h`. The build regenerates on any `.glsl` edit
(`GLOB_RECURSE ... CONFIGURE_DEPENDS`, so shared chunks under `shaders/include/` retrigger
it too) -- there is no manual step, and `./build.sh` is the whole answer.
**Only `program.c` includes it** (spec 11.105). It used to be included from `shader.h`, which
`engine.h` pulls in, so every file that used the engine compiled 2 MB of string literals and a
`.glsl` edit rebuilt 66 objects. Now it rebuilds one.

Inventory by subsystem. **`docs/shader-subsystems.md` carries the per-subsystem detail** — how
each one works, what was rejected, and the failure modes that render a plausible frame. Read the
entry there before changing anything marked with a dagger.

- **PBR:** `pbr_vert`, `pbr_frag`, `pbr_skinned_vert` (GPU skinning)
- **Shadows:** `shadow_depth_vert/frag`, `catcher_vert/frag` (shadow-catcher ground)
- **IBL:** `ibl_cubemap_vert`, `ibl_equirect_frag`, `ibl_irradiance_frag`, `ibl_prefilter_frag`, `ibl_charlie_prefilter_frag` (sheen env), `ibl_brdf_frag`
- **Atmospheric sky:** `sky_transmittance/multiscatter/view/env/background/debug_frag`
- **Day/night cycle †:** `sky_cycle_tick`, split into `sky_cycle_advance` + `sky_slicer_pump`;
  called from `engine.c` before the GI sweep and the shadow pass
- **The moon †:** `include/moon.glsl` + one guarded block in `include/sky_radiance.glsl`, over the
  surface `moon_surface.c` bakes
- **Night floor †:** one term at the end of `sky_view_frag.glsl`'s march
- **Stars †:** `include/stars.glsl` + one term in `include/sky_radiance.glsl`
- **Skybox:** `skybox_vert/frag`
- **Bloom:** `bloom_bright_frag`, `bloom_downsample_frag`, `upsample_tent_frag`
- **Tonemap / exposure †:** `tonemap_frag`, `lum_measure_frag`, `lum_histogram_frag`,
  `lum_reduce_frag` (measure -> bin -> collapse; there is no `lum_adapt_frag`, the blend is on the
  CPU where the readback already lands)
- **Purkinje / scotopic shift †:** `include/purkinje.glsl` + `tools/gen_scotopic_weights.py`.
  **NOT in the finishing stack** — it splices into `sceneToToned` between the sanitize and the
  tone curve
- **DoF:** `dof_coc/tile/dilate/gather/composite_frag`
- **Motion blur:** `motion_blur_tilemax/neighbormax/_frag`
- **TAA / temporal:** `taa_resolve_frag`, `taau_resolve_frag` (render-to-post upscale,
  `--render-scale`), `temporal_accum_frag`
- **AO / GI:** `gtao_frag`, `ssao_blur_frag`, `ssgi_accum/atrous/composite_frag`
- **Specular occlusion †:** `spec_occ_composite_frag` + `include/spec_occ.glsl`. Three modes,
  default `split`; the default's term is computed in `gtao_frag`, not here
- **SSR:** `ssr_frag`, `ssr_hiz_frag`, `ssr_accum_frag` (temporal), `ssr_atrous_frag`
- **SSS:** `sss_gather_frag`, `sss_pyr_seed_frag`, `sss_pyr_down_frag` — a pyramid gather, which
  REPLACED the separable `sss_blur_frag`. That name survives in one stale comment at
  `froxel_composite_frag.glsl:79` and nowhere else
- **IES profiles †:** `include/lights_ubo.glsl`'s `IesBlock` + `iesProfile`. REPLACES a spot's
  analytic cone rather than multiplying it
- **Contact shadows †:** `contact_shadow_frag` — marches the key directional and every clustered
  punctual with no shadow map
- **Water †:** `water_vert/frag` + `water_spectrum_frag` + `water_fft_frag` + `water_foam_frag` +
  `include/ocean.glsl`
- **OIT:** `oit_resolve_frag` + `include/mboit.glsl` (the absorbance moments and their
  reconstruction, shared by the generation and accumulate sub-passes)
- **Atmosphere †:** `froxel_inject/integrate/composite_frag` + `include/froxel.glsl`, and
  `aerial_lut_frag`. `froxel_inject_frag` is where every medium meets
- **Volumetric clouds:** `cloud_march_frag`, `sky_background_clouds_frag`, `sky_env_clouds_frag`
  (the background and env-cube variants), `cloud_noise_debug_frag`
- **Cloud shadow †:** `cloud_shadow_frag` — a 256² R16F sun-transmittance map through the deck
- **Clustered specular probes †:** `probe_project_frag` + `include/probe_specular.glsl`
- **Clustered decals †:** `include/decals_ubo.glsl`. Declares no sampler; spliced into `pbr_frag`
  in TWO halves
- **Layered surfaces †:** `include/layers.glsl` + `include/triplanar.glsl`. Declares no sampler;
  gated on `layerCount`, so an unlayered material is an exact identity
- **Roads †:** `roadReshapedWeights` in `layers.glsl` + `include/roads_ubo.glsl`
- **Composite cache †:** `layers_vt_bake_frag` + `sampleCachedSurface` in `layers.glsl`, and
  `layers_vt_feedback_vert/frag` for the vote pass
- **Particles:** `particle_vert/frag`
- **Text:** `text_vert/frag`
- **Game UI:** `ui_vert/frag` — one program for every menu primitive, selected per vertex by a
  `mode` attribute: a flat fill, a textured quad, or an SDF glyph. Rounded corners and borders are
  an exact signed-distance evaluation rather than geometry, so a radius costs no vertices; the
  glyph branch takes `fwidth` with no constant smoothing term, which is what keeps text the same
  weight at any size
- **Debug / util:** `shape_vert/geo/frag`, `bone_vert/frag`, `xyz_vert/frag`, `mask_copy_frag`

Three rules from that inventory stay HERE, because each one is violated from a different file:

- **`tonemap_frag`'s finishing stack has an ORDER and it is stated at the top of the file**:
  `sharpen -> grade -> vignette -> gamma -> LUT -> grain -> dither`. Two of those positions are
  contracts rather than taste. **Dither must stay last** (spec 11.24) because it is the quantization
  stage and anything appended after it reaches the 8-bit target undithered. **The LUT must stay
  after `displayEncode`** (spec 11.58) because a `.cube` is authored against display-encoded values;
  it sits before the grain because grain is sensor noise laid over a finished look. The `sampler3D`
  is on unit 11 -- this program's unit space is its own, separate from `pbr_frag`'s ledger, and a
  `sampler3D` sharing a unit with a `sampler2D` is `INVALID_OPERATION` at draw, so it appends rather
  than reusing the free unit 7. Note `lutCoord`'s half-texel inset is **trilinear-only**:
  `lutTetrahedral` addresses texels by integer index, so the default path is an exact identity
  whether that remap is right or wrong -- which is why the identity gate arm needs a trilinear leg
  and why its first draft was green with the remap deleted
- **`punctualAngular` is the one place a punctual light's ANGULAR falloff is decided** (spec 11.57)
  -- profile where there is one, `spotConeFactor` otherwise. It exists because that falloff is
  evaluated in **five** places: `pbr_frag`'s main loop, `pbr_frag`'s hand-duplicated `renderMode 7`
  debug copy (which no golden can see), the froxel fog's clustered point walk, the froxel fog's spot
  shaft, and `contact_shadow_frag`'s fold weight. A profile applied to some but not all means the
  beam disagrees with the pool it casts. The spot shaft is the one that does NOT come from the
  cluster list -- it is published standalone so it can carry its shadow map -- **so the decision is
  split from its data source**: `punctualAngularOf` takes the values, `punctualAngular` is the
  cluster-list adapter over it, and the shaft calls the former. It shipped restating the rule as
  its own ternary while this paragraph and the function's own comment already claimed all five went
  through one place, which is worse than not centralising: the next reader trusts the comment and
  edits one site.
  **A profile is refused on an area panel and a directional**, at authoring and again at pack.
  Not because anything would read it in `pbr_frag` -- panels `continue` before the call -- but
  because `contact_shadow_frag` folds the angular term into its DENOMINATOR before it classifies
  the light, so a profiled panel was weighted by a distribution its LTC shading never applies.
  Same error class 11.56 measured at 23% on a pixel that should lose 1%.
  **`--ies-profile` skips the same two types**, which is what lets the flag be pointed at a whole
  scene blindly: a mixed room takes the profile on its practicals and leaves its sun and its
  softboxes alone.
  **And `iesProfile`'s out-of-range answer is `1.0`, which is the neutral for a MULTIPLIER and
  wrong for a replacement** -- so the "is this a profile" bounds test lives in `punctualAngularOf`,
  not there. Composed the other way a stale index made a light an unbounded omnidirectional
  emitter that could never return exact zero, taking `pbr_frag`'s early-out with it.
- **`shadowMisc.y` is the LIVE punctual layer, not `Light.shadow_layer`** (spec 11.56).
  `shadow_layer` is maintained only while the depth pass runs, so switching the shadow system
  off at runtime leaves every mapped light claiming a layer nothing draws. `light_cluster.c`
  packs `shadow_live_punctual_layer` instead, which applies `enabled` and the
  `punctual_layer_count` bound -- the same bound `punctual_shadow.glsl` puts on the lookup.
  Anything asking "does this light have a map" must go through that helper and not the raw
  field, or a light silently keeps a map it lost.

## OpenGL Vertex Attributes (common.h)

```c
#define GL_ATTR_POSITION      0    // vec3
#define GL_ATTR_NORMAL        1    // vec3
#define GL_ATTR_TEXCOORD      2    // vec2  (UV0)
#define GL_ATTR_TANGENT       3    // vec4  (xyz tangent, w = bitangent handedness)
//                            4       free FOR MESH VAOs -- see below
#define GL_ATTR_COLOR         5    // vec4  (vertex color)
#define GL_ATTR_BONE_IDS      6    // ivec4 (skinning)
#define GL_ATTR_BONE_WEIGHTS  7    // vec4  (skinning)
#define GL_ATTR_TEXCOORD2     8    // vec2  (UV1: lightmap/AO or a UV1 splat, OR wind data
                                   //        under wind_mode >= 1 -- mutually exclusive uses)
#define GL_ATTR_MORPH        12    // vec3  (CDLOD: parent Y, window start, 1/span)
#define GL_ATTR_MORPH_NORMAL 13    // vec3  (CDLOD: the parent surface's normal)
```

**There is no `GL_ATTR_BITANGENT` and no bitangent stream.** Slot 4 held one until it turned out
to be dead weight -- the fragment shader reconstructs `B = cross(N, T)` regardless and only ever
read the stored vector for its SIGN, which now rides in `tangent.w`. So a mesh uploads four floats
of tangent, not three plus three.

**Slot 4 is free for MESH VAOs, which is not the same as unclaimed.**
`particle_sim_vert.glsl` binds location 4 as `iLife` on the GPU-sim VAO, and the billboard
renderer holds **9-11** for per-instance centre/params/colour as bare literals rather than
defines. Separate VAOs, so there is no runtime conflict either way -- but reusing any of 4 or 9-11
for mesh geometry would collide with that convention. 14-15 are genuinely unclaimed; GL 4.1
guarantees at least 16.

The last two are OPTIONAL and their absence is the OFF state rather than a degenerate one: an
unbound attribute reads (0,0,0), so the span reciprocal is 0, the morph factor is 0, and every mesh
outside a terrain quadtree is an exact identity with no flag, no material field and nothing to switch
off — all 24 goldens are 0 px. The morph WINDOW rides the attribute instead of a per-level uniform
array indexed by a baked level, which costs eight bytes a vertex of per-patch constant and buys
exactly that.

**And the morph is a DISPLACER, which is a bigger commitment than an attribute.** Anything added to
`cetra_local_position` (`object_position.glsl`) needs a BOUND on the CPU side — `draw_list.c`'s
`_item_bounds` — or a mesh that moves becomes uncullable or, worse, gets culled while on screen. And
it has to reach ALL FIVE programs that include the chunk (pbr, pbr_skinned, depth prepass, shadow
depth, shadow absorb), which `engine_upload_displacement_uniforms` is the one call for. A program
that misses a uniform displaces its vertices somewhere else, and under the prepass's one-sided LEQUAL
that DELETES the surface rather than shading it wrong — spec 11.62 shipped exactly that, reaching one
program of five, with the full gate suite green.

## Texture / Sampler Model

`Material` carries **13 texture pointers**: albedo, normal, roughness, metalness, AO,
emissive, height, opacity, microsurface, anisotropy, sheen, reflectance,
clearcoat-normal.

**Storage is BLOCK-COMPRESSED where the caller says what a texture IS** (spec 11.85).
`TextureUse` follows `TextureAlpha`'s pattern for the same reason: the loader knows only
`is_srgb`, which is false for a tangent normal and false for a roughness mask alike, and those
two want opposite formats. `import.c`'s `texture_mappings` table is the one place that knows
which is which. **Normals take BC5, single-channel masks BC4, and colour takes DXT only under
`--texture-compress-colour`** — off by default because BC5 and BC4 cost a fraction of a code
while DXT quantises endpoints to RGB565 and is a judgement. Raiden: **81.4 MB → 74.9 → 37.3**. **It is a VRAM win and NOT a speed win, measured both
ways**: frame time moves +0.3% against a 2.2 s spread and the encode costs nothing at load
(-0.4%, even with colour on). The bandwidth argument is real and unobservable here -- nothing
in this engine is texture-fetch bound. Note the GPU profiler **cannot price it at all**: on a
light frame the pass sits under the timer's jitter, on a heavy one half the samples retire
unavailable and print 0.000, so wall clock is the only instrument that answers.
**BC7 does not exist on this platform** — BPTC is core in GL 4.2, Apple stops at 4.1 and
exposes no extension, and the upload returns `GL_INVALID_ENUM`; RGTC reads as absent for the
opposite reason (core in GL 3.0, no extension string) and works. Verified by probe.
**The two families have different GUARANTEES and only one is queried**: RGTC being core means
BC4/BC5 exist on any context this engine can create, while S3TC is an extension that may be
absent on another driver — so the colour path asks once, warns by name, and degrades to
uncompressed rather than uploading nothing. The default path cannot fail on a conformant
driver; the opt-in path cannot fail silently.
**Three consequences worth knowing.** `pbr_frag` **rebuilds a normal's Z** from XY
unconditionally, because no block format carries three channels — a no-op on a well-formed map
and a repair on a JPEG-damaged one (raiden moves 52,353 px). The mip chain is built **on the
CPU**, because `glGenerateMipmap` cannot fill a compressed chain; sRGB is averaged in LINEAR
space, which `texture_mean_rgb` depends on. And **`texture_pool_publish` is the one path from
decoded pixels to a pooled Texture** (spec 11.86) — create, bind, sampler state, formats,
upload, insert. 11.85 shared the UPLOAD alone and left the other seven steps written out three
times, where they had already drifted: `TextureAlpha` reached one producer of three, and the
async path recovered `is_srgb` by testing an internal format that has no sRGB variant below
three channels, so a greyscale albedo mipped in the wrong space on the streamed path only.
**The moon surface is outside all of this** — `moon_surface.c` is pure CPU pixel generation with
no GL call in it at all, and `sky.c:1444-1454` uploads those pixels with `glTexImage2D` +
`glGenerateMipmap`
itself — the claim is about the POOL, not the engine. **The material texture array is NOT
compressed and that is measured**: its packed maps and splat are three independent quantities
in one image, which BC4 and BC5 cannot hold, and it contains no normal maps at all.

**A loader is told three facts as one `TextureDesc`** — `is_srgb`, `TextureAlpha`, `TextureUse`
— through `texture_load_file` / `texture_load_memory` / `texture_load_memory_owned`. They are
three because they genuinely disagree: reflectance is colour-ish and LINEAR, a decal is colour
with a real opacity alpha loaded linear because it lives in the material array, and
`apps/tree`'s sand albedo is baked non-sRGB on purpose so the stochastic transform operates on
stored codes. `texture_desc(is_srgb)` states the historical inference and is **not** a zero
initialiser — `TEXTURE_ALPHA_OPACITY` is the zero of its enum, so a zeroed desc claims a linear
image has an opacity in alpha and dilates a height map. Before 11.86 this was **seven** entry
points, four of which existed only to default a field and two of which had no callers at all.

At draw time they bind through the material sampler units (`common.h`,
`TEXUNIT_MATERIAL_MAX = 8`). Six per-texel masks (roughness/metallic/ao/opacity/
microsurface/anisotropy) are packed into a single `sampler2DArray`. There is **no
subsurface layer**, and the hair strand map is not a seventh: it rides the
anisotropy slot (spec 11.20).

**Since 11.60 that array is not only masks**, and 11.61 renamed it
`MaterialTextureArray` for that reason. The rename is worth knowing about because 11.60 argued
AGAINST it and was wrong: it cited `anisotropy_tex` still carrying the strand map (11.20) as
precedent for documenting a second tenant rather than renaming. **That precedent does not
transfer** — `anisotropy_tex`'s declared contract, per-texel grain direction, is STILL TRUE of a
strand map, so the name merely narrowed. An albedo is not a mask under any reading, so this one
became untrue, and it took sixteen lines of header comment to explain that a name meant something
other than what it said. **A name that needs a paragraph is a rename in prose form.** It now also holds a layered
material's **layer maps** (albedo + height, and packed normal/roughness/AO) and its **splat**, up to
`MATERIAL_MAX_LAYERS` of each, so N material layers cost the 16/16 ledger **nothing**. **Since 11.73
it also holds DECAL images** — an albedo and an optional packed surface map per mark, up to
`DECAL_MAX` of each — the third TENANT of this array (masks, layers, decals; the wall-dodge
roll call below counts a different list and reaches six) and the proof it generalises past
materials: a decal belongs to no material at all. What the
array really is is a scene's unique per-texel material IMAGES, of which the masks were the first
kind; `material_texture_layer_for` dedups by GL id and knows nothing about what a layer means. Two consequences
worth carrying: every layer shares one canonical **width and height** (two dimensions since 11.60 —
a square at the longest side halved forest's array for nothing), and the array is **linear RGBA8**,
so a layer albedo is stored un-decoded and `pbr_frag` decodes after the blend — and a decal's
albedo the same way, for the same reason and at its own splice. That last used to be why
the transparent-texel dilate was gated on `is_srgb` — a layer's alpha is a HEIGHT, not an
opacity — and **11.73 broke that correlation and replaced it**: the gate is a `TextureAlpha`
the caller states, defaulting to the old inference, because a decal image is colour with a
real opacity alpha loaded LINEAR. For two specs only the FILE loader took it while the memory
and async paths inferred, a latent trap for the first app to build a decal image procedurally;
**11.86 closed it** by carrying the whole `TextureDesc` to all three, so a streamed decal can
state its alpha. The dilate still HAPPENS in three places, deliberately — the async one runs
on a worker thread, which is the point of it — but the decision is `texture_wants_dilate`.
Five are scalar; **anisotropy is a vector field** -- `.rg` a grain direction
stored as a coherence-weighted DOUBLED angle, `.b` a per-strand identity. The
doubling is not decoration: the array resamples and mips every layer, and a
direction equals its own reverse, so a raw vector averages to nothing exactly
where the surface is minified (spec 11.20). Anything added here has to survive
being averaged.

```
0: albedo   1: normal   2: masks (sampler2DArray)   3: clearcoat normal   4: height (POM)
5: emissive 6: scene color (refraction resolve, engine-bound)
7: LTC tables, 2-layer sampler2DArray (engine-bound)   8: sheen color
9: Charlie sheen environment cubemap (engine-bound)
```

`Material.reflectance_tex` is loaded and owned but has no
unit and is never bound (KHR specular color is deferred). Engine-bound units 9-15 hold
the Charlie sheen environment (9), the CSM depth array or its MSM replacement (10),
the three IBL textures (11 irradiance, 12 prefilter, 13 BRDF LUT), the skybox/GI
atlas (14) and the punctual shadow array (15).

**The uber-shader is full at 16/16 and the driver counts sampler DECLARATIONS, not uses**,
so there is no seventeenth sampler in `pbr_frag` even for a unit nothing binds. That second
clause was tested rather than assumed in spec 11.95 and it HELD: a `pbr-0` variant, whose
every read of `ltcTex` had already been folded away by 11.93's constant branches, still
reported 16 active samplers. A declaration outlives a dead read. **A VARIANT is not full,
and that is 11.95's whole result** -- `pbr-0` spends **12**, because gating the reads with
the PREPROCESSOR rather than a branch lets the declaration go too (`sheenTex`,
`charliePrefilteredMap`, `ltcTex`, and `heightTex` where neither parallax nor layers are
carried). Ask which units a variant needs before assuming the ledger; the answer for the
common case is four short of the ceiling.
Five escapes are precedented: ride an existing declaration when the consumers are provably
mutually exclusive (11.17's moments over `sceneColorTex`, and 11.66's composite-cache
pair over `albedoTex`/`normalTex` — the strongest exclusivity in the file, since the
same `layerCount > 0` bit that arms the cache is the one that skips both reads, so it
rests on the MATERIAL rather than on a pass or a routing convention), alias a unit
that is IDLE in the pass that needs it (11.41's cloud shadow map, a `#define` over
unit 6 in the opaque pass), bake the transform over the original so the second texture
is never read (11.46), keep the table in `const`/uniform space (11.13's skin curvature
rows, 11.57's whole 2D IES distribution in a UBO block of its own, 11.67's
virtual-texture PAGE TABLE on binding 7 -- an RVT indirection is a table too,
so real paging costs the ledger nothing -- and 11.68's ROAD SEGMENTS on
binding 8, which widens the rule past tables: a road is GEOMETRY the shader
evaluates rather than an image it samples, so a whole content feature arrived
for 1104 bytes and no unit, and 11.70's PROBE descriptors plus one 8-bit
froxel mask per cluster on binding 9, 3760 bytes for a whole spatial selection
structure, and 11.73's DECAL transforms plus a 16-bit froxel mask on binding
10, 7440 bytes -- which takes `pbr_frag` to TEN uniform blocks of GL 4.1's
guaranteed twelve, so two remain and the next taker should know that is the
budget), or **become
another tenant of a texture that is already declared** (11.60's N material
layers in the `sampler2DArray`, which is the cheapest of the five and the one to reach
for first: an array is a POOL, and a consumer that fits its shape costs a layer index
rather than a declaration. `material_texture_layer_for` dedups by GL id and knows
nothing about what a layer means. 11.70 generalises this past arrays: its probe columns
are a REGION of the GI atlas's plain 2D texture on unit 14, side by side with GI's own,
which works for the same reason and needs only that the incumbent's coordinates come
from a uniform rather than a constant -- `giAtlasSize` already did). 11.67 adds the refusal-backed variant of the first escape: units 3/4
(`clearcoatNormalTex`/`heightTex`) carry the page pair because a layered
material REFUSES both maps -- exclusivity made true by authoring policy where
units 0/1's was provable, with the IES refusals as the precedent and the dead
POM march as the independent justification. Note 11.66 also shows the tenancy escape REFUSED on size grounds: a
composite atlas in `materialArray` would have promoted every other layer in the scene
to its resolution through the canonical-size rule, so it owns plain 2D textures
instead.

**The SIXTH escape is different in kind from the five above, because it RETURNS units
rather than routing around them** (spec 11.95): gate the declaration itself on the
variant's feature mask, `#if CETRA_HAS(...)`, and a program that provably cannot sample a
texture stops paying for it. Reach for this FIRST -- it is the only one that leaves the
ledger emptier than it found it, and the other five all start by conceding the unit is
gone. Four conditions make it work, and a want that fails them needs one of the five
instead. The feature must have a BIT (a scene or material fact the CPU can answer, in
`pbr_features.glsl`) -- which is why `clearcoatNormalTex` is still ungated, clearcoat being
decided by a uniform. Every read must move to the preprocessor WITH the declaration, since
a surviving read references a name that no longer exists -- a compile error, which is the
good failure. The chunk that declares gates ITSELF (`ltc.glsl` decides when `ltcTex`
exists; a guard at the include site would take the functions `pbr_frag` calls with it). And
the units come back PER VARIANT, not globally: a scene whose materials use everything still
declares sixteen, so this buys headroom for the common case rather than an unconditional
slot. Note the win is the unit and not the clock -- freeing `heightTex` measured 0 ms, and
the two BODIES removed on the way (the LTC library, the layered blend) are where the
milliseconds were.

This is the roadmap's Wall 1, and **the list of what it blocks is shorter than it looks —
this paragraph named the cloud shadow map in `pbr_frag` as blocked for a spec cycle after
11.41 had already shipped it** by the second escape above. **And this paragraph named
decals for another cycle after that**, on the roadmap's own reasoning that a flat 2D atlas
would dodge the wall by aliasing unit 6 in the opaque pass — which was already false when
it was written, since 11.41 holds that unit for the whole of that pass. Spec 11.73 shipped
them as `materialArray` TENANTS instead, the sixth wall-dodge in the roll call below, and got
something the alias could not have given: decals in the LATE PASS, where unit 6 carries the
refraction resolve and could not have carried an atlas as well. **The captures half of that
claim is weaker and was overstated for a while** — `!capturing` guards the cloud-shadow bind
because a time-varying dapple freezes into an idle probe, and a static decal has no such
reason, so an alias could have been bound there deliberately. It also nearly shipped false in
the other direction: probes were captured at load BEFORE decals were applied, so no capture
contained one until `decals-capture` was written. What it genuinely still
blocks is light cookies and sampling the froxel volume from the transparent pass — the
latter being the only booked consumer that cannot dodge, since a `sampler3D` in the one
pass where refraction is live has nothing to alias. **Check whether a want actually needs a NEW declaration before
booking it against this wall**: C2 (spec 11.49) added a whole lighting feature to
`pbr_frag` for one `float` uniform and no unit at all, and C3 (spec 11.57) added a
2D per-luminaire distribution for zero — the roadmap had deferred its asymmetric
half for want of a sampler unit nobody was going to spend, since the table was
never going to be a texture. **D9 (spec 11.60) is the third and the largest**: a whole
multi-layer material system, and the roadmap had it booked as needing its own surface
program to get a fresh ledger. It needed neither a program nor a unit — the array it
wanted was already bound on every draw. **D4 (spec 11.63) is the fourth**: a CDLOD terrain
whose textbook form displaces a shared grid by a height TEXTURE, which is a sampler in
every geometry program and a different surface from the one the collider agrees through.
It needed no unit either — the heights are in the VERTEX BUFFER, on two attribute slots
that were free, and the sixth escape is the one nobody counts. **C4 (spec 11.70) is the
fifth**, and it is the tenancy escape used on something that is not a `sampler2DArray`:
N reflection probes needed somewhere every fragment could reach all of them at once, and
the roadmap's own entry booked it as "reusing A4's" atlas — which turned out to be true of
the DECLARATION and not of the texture, since A4's is mip-less with 8x8 tiles where
specular wants roughness-varying radiance. So the two became tenants of ONE physical
texture on unit 14, side by side, with the GI region's coordinates untouched because
`giAtlasSize` was already a uniform. **D1 (spec 11.73) is the sixth, and the one the
roadmap had booked AGAINST this wall by name**: clustered decals, the first consumer that
genuinely wanted a picture rather than a table, a distribution or a polyline. It is still
the tenancy escape — a decal image is one of the scene's unique per-texel material images,
so it costs an array LAYER and no declaration — and what it bought over the flat-atlas
alias the roadmap had planned is coverage rather than thrift: marks in the late pass and
in probe captures, which an opaque-pass-only alias forecloses. **Ask how many BYTES a
feature needs before asking for a unit, ask what is already DECLARED before asking for a
program, and ask whether the data belongs on the VERTEX at all before asking for either.**

**`water_frag` reached the same ceiling in 11.42, and 11.45 took it back off** — it declares
**11**, not 16, and this paragraph claimed the ceiling for three specs after it was freed. It is a
separate ledger — a program gets sixteen, not the engine — so its units alias `pbr_frag`'s freely,
and its tenants are chosen around a rule the material ledger never has to think about: **two sampler
TYPES against one image unit is an `INVALID_OPERATION` at draw**, so the cascade array cannot go on
unit 10 where water binds `cascadePrev1` as a `sampler2D`. It takes 11 (whose other tenant is the
IBL irradiance CUBE) and the foam target takes 15 (the punctual shadow ARRAY). Aliasing across
programs is fine; aliasing across types within one is not.

**What freed it is the rule to reach for before booking anything against Wall 1** (`ocean.glsl:63-78`):
*a cap counted in declarations is a cap on how many DISTINCT shapes of data a program reads, and six
identical fields were only ever one shape.* Six RGBA16F cascades became one `sampler2DArray` — same
memory, same filtering, same reads, **five units back**. So N textures of one shape cost one
declaration, whatever N is: it is why terrain material layers (roadmap D9) are three declarations
rather than 2N, and why that item does not need virtual texturing to exist. One consequence to carry:
an array has ONE mip policy for every layer, so consolidating textures with different mip needs costs
memory even when it changes no read.

## Render Modes (common.h)

```c
RENDER_MODE_PBR              // Full PBR shading
RENDER_MODE_NORMALS          // Normal visualization
RENDER_MODE_WORLD_POS        // Position heatmap
RENDER_MODE_TEX_COORDS       // UV visualization
RENDER_MODE_TANGENT_SPACE    // TBN basis
RENDER_MODE_FLAT_COLOR       // Solid color debug
RENDER_MODE_ALBEDO           // Albedo only
RENDER_MODE_SIMPLE_LIGHTING  // Unlit/simple lighting
RENDER_MODE_METALLIC_ROUGH   // Metallic/roughness channels
RENDER_MODE_VELOCITY         // Motion-vector visualization
RENDER_MODE_HDR_HOTSPOTS     // Shaded HDR magnitude as a heat ramp (mode 10)
RENDER_MODE_SSS_HOTSPOTS     // The same ramp over attachment 4's SSS diffuse (11)
RENDER_MODE_EXTRAPOLATION    // Per-varying MSAA extrapolation: R normal, G tangent, B UV (12)
```

**The shader numbering runs one further than the enum.** `pbr_frag.glsl:1038` handles
`renderMode == 13` as the clearcoat-normal view, and `--clearcoat-debug` reaches it by assigning
the literal 13 (`apps/render/src/render.c:1356`) — there is no `RENDER_MODE_CLEARCOAT` in
`common.h` to name it with. Anything switching on this enum has to cope with a value past its
last member.

## Camera Modes

```c
CAMERA_MODE_FREE   // WASD + mouse look
CAMERA_MODE_ORBIT  // Mouse drag to orbit target
```

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
`screenshot_every`, `camera_mode`) and `clear_color` (11.107); headless flag; embedded
`PostFX* postfx`; bone-overlay + shadow-catcher programs; async loader; text renderer. By
function: the sample count and the two scales (clamp + rebuild), the screenshot path (owned),
the callbacks and the clock (installs).

**Scene:** root SceneNode (`scene_set_root`); lights; **particle_systems (owned)**; cameras;
materials; skeletons + animations -- the registries, each an `scene_add_*`; texture pool;
`shadow_system`; IBL + sky + reflection probe + GI + water, installed by a plain pointer write and
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
**`particle_system`** (all borrowed, each a `node_set_*`). Nothing per node says whether the
gizmo overlay draws it: the switch is `engine->show_xyz` and the program the scene's.

**Mesh:** the content under SETTINGS -- draw mode, positions/normals/tangents (vec4, w = handedness — there is NO bitangent stream)/UV0/UV1/
colors(RGBA)/indices, Material, the skinning input (`bone_ids`, `bone_weights`, `skeleton`);
the upload's output under ENGINE-OWNED -- VAO + per-attribute VBOs + EBO; AABB; `is_skinned`;
per-bone bind-space boxes and the wind vertex maxima (spec 11.53); `gpu_vertex_count` and
`draw_refusal` (spec 11.107).

**`AABB` has five operations since 11.54** — `aabb_empty` / `aabb_is_empty` / `aabb_add_point` /
`aabb_union` / `aabb_expand`, `static inline` in `mesh.h` beside the typedef. Before them every
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
written directly or by name; there is no per-field setter.

**Light:** type (directional/point/spot/area); position, direction and up (`light_set_position`
/ `light_set_direction` / `light_set_up`: an authored copy the walk carries into a world copy, and
the setter writes both for a light on no node); color; specular;
ambient; intensity + `units` (`light_set_intensity_units` converts); **`range`** — where the
inverse-square falloff is windowed to zero and the cull radius, 0 = unbounded. **There is no
constant/linear/quadratic attenuation triple** and never has been in the photometric era; spot
cutoffs (stored as COSINES of the half-angles, not radians); area size; `cast_shadows`; the
shadow indices and the emissive source id are the engine's.

**Camera:** position and look-at (`camera_set_position` / `camera_set_look_at`, which re-derive
the orbit); the orbit (theta/phi/distance) and the aspect, DERIVED, written by the orbit moves
and the frame; settings: up, FOV, near/far, the ortho height and switch, `max_distance`, the two
speeds.

## Lighting Environment

- **Shadows:** cascaded shadow maps (up to 3 cascades, up to 3 shadow-casting lights
  into a `GL_TEXTURE_2D_ARRAY`), per-cascade bounding-sphere ortho fitting, 3x3 PCF by
  default with optional PCSS contact-hardening. Stored on `scene->shadow_system`.
- **IBL:** split-sum -- 32^3 irradiance map, GGX prefilter (1024 base, 9 mips), 512^2
  BRDF LUT. Environment comes from an HDR file (`-e/--env`) or is baked procedurally
  from the sky. Extracts bright light lobes from the HDR to aim analytic shadow lights.
- **Sky:** Hillaire (EGSR 2020) physically-based atmosphere -- transmittance (256x64),
  multiple-scattering (32x32), and sky-view (192x108, re-baked when the sun moves) LUTs.
  Feeds the skybox, IBL, reflection probe, and fog; couples the directional key light
  via atmospheric transmittance.
- **Reflection probes:** local prefiltered cubemaps with parallax-corrected AABBs
  (Lagarde 2012); consumed as PBR specular and as the SSR ray-miss fallback. **Needs a
  precomputed IBL to be created at all**, so `--probe-scene` on a scene with no `-e` and
  no `--sky` logs "requires an HDR environment" and skips — and a `.cscn` authoring
  `probes[]` without an `environment` block is refused the same way, which is a trap
  worth knowing because the file still parses and the scene still renders.
  **Up to eight since spec 11.70**, authored as a top-level `probes: [{position, boxMin,
  boxMax, intensity, boxFade, envOnly}]` and blended per fragment; `--probe` beside an
  authored array is warned-ignored, and so is `environment.probe_scene`, because both
  ask for ONE auto-placed probe and a file with its own array has answered that already.
  Captured at load, published only once every probe succeeds, so no probe is ever
  photographed into another's capture. `--probe-set-res N` is the atlas row-0 size
  (default 512, any value in 64..2048 -- it briefly had to be a power of two, and
  the row table that removed that is the reason it no longer does), `--probe-set-probe N` the diagnostic, `--probe-set-debug` the atlas
  overlay — the only way to tell a bad column from a bad lookup, since both are the same
  picture from outside.
- **Derived area panels:** `--emissive-lights` (spec 11.49) fits a rectangle to each
  emissive mesh and registers it as a real `LIGHT_AREA` in the cluster list. Off by
  default. The fit is cached in LOCAL space and epoch-gated on the scene graph while
  PLACEMENT runs per frame from the owning node's transform, so a lamp on a moving node
  costs four vector transforms rather than a refit -- three corners through the node's 4x4, and
  the panel normal through its normal matrix. Reconcile, not rebuild: a panel whose
  mesh survives keeps the same `Light` object, so a `light_overrides` entry naming it
  persists across a graph change.

**Two captures, opposite requirements, and this is the thing to know before touching
either.** Both `gi_volume.c` and `probe.c` reach the scene through `scene_capture_faces`
and both raise `engine->capturing`, but a DDGI capture's output is IRRADIANCE that gets
added to the analytic direct term, so a derived emitter must be ABSENT from it or its
light arrives twice (measured: floor GI lift 1.31x). A reflection probe's output is
RADIANCE, what a mirror sees, so the emitter must be PRESENT. `engine->capturing_irradiance`
names what a capture is FOR rather than that one is running, and only `gi_volume.c` raises
it — gating on `capturing` alone would settle the specular question by accident. That
specular double count is real, measured at 1.0386, and deliberately left in: every fix
costs more than it buys, and a gate arm bounds it instead.

## Game Framework

`cetra/src/game/` layers a classic fixed-timestep game loop over the Engine, with an
optional Jolt physics world and an ECS-lite entity system.

```c
GameConfig config = {.engine = {.title = "My Game", .headless = headless}}; // zero = default
Game* game = create_game(&config); // creates and initialises the engine
game->engine->exit_after_frames = frames; // the run's settings go on the engine
game_set_scene(game, scene);
game_set_init(game, on_init);      // + on_update / on_pre_render / on_shutdown; on_render
                                   // only for what the app does AROUND the draw, since the
                                   // loop draws the scene itself when none is set
game_run(game);                    // fixed-timestep loop
free_game(game);
```

Each fixed step, in order: sync kinematic bodies -> user `on_update` ->
**`scene_update_particle_systems`** -> character controllers ->
`physics_world_update` (4 collision sub-steps) -> collision events -> sync physics ->
sync character controllers to entities -> sync transforms. Physics and entities are optional (the `spores` app uses the loop for
particles only; `gametest` exercises the full stack). Physics is **Jolt** (rigid bodies,
raycasts/sweeps, fixed/distance/hinge/slider/6DOF constraints with motors, and a
`CharacterVirtual` controller).

**Input is polled once a frame, BEFORE the fixed steps** (`game->input`, spec 11.109), so a
press or release is an edge per FRAME: a frame that runs two fixed steps hands both the
same press, and a frame that runs none drops it. A game reads ACTIONS rather than key codes
-- a table of `InputAction` rows, each a name and up to six sources (a key, a mouse button,
a pad button, or a pad axis, each with a scale), bound with `input_bind` and read with
`input_action_value` / `_down` / `_pressed` / `_released`, or two at once as a ground-plane
move with `input_action_move`. Every action is one float in -1..1 from whichever source is
largest in magnitude, so a stick and a key pair are the same action and not two code paths.
The keyboard, mouse and per-slot pad queries are still there for a game that wants a
specific device. **Gamepads come through GLFW's standard layout** (any controller the
bundled SDL database knows reads as fifteen buttons and six axes) with a radial stick dead
zone and a trigger dead zone, both rescaled; hot-plug is logged by name; a slot that
disconnects releases everything it held, and a button already down when a pad appears is not
a press. **A pad is read through a seam** (`GamepadReadFn`): the default asks GLFW, and
`input_set_pad_script` replays a text script on slot 0 instead, which is how the whole layer
above the seam is verified with no controller present -- the `gamepad` gate group is five
such scripts through `apps/gametest`. GLFW's real device path is what the seam sits on, and
nothing in the suite exercises it; `docs/verification.md` carries the two recipes for
closing that. A controller released after the build is picked up through
`input_load_gamepad_mappings` (an SDL mapping file, any time after the engine exists).
**The wheel is the engine's**: its scroll callback accumulates `engine->input.scroll_dx/dy`
after the GUI gate and zeroes them before each end-of-frame poll, so any frame's update hook
reads one frame's delta; the game layer's `input_scroll` is that, read at the poll.

**Audio is a Game subsystem like the physics world** (`game->audio`, spec 12.0): one
output device wrapping miniaudio's high-level engine, built with `create_audio_system`,
installed with `game_set_audio_system`, and freed by `free_game` AFTER the entity manager --
because `AUDIO_SOURCE` components hold sounds that live in the engine, so the components must
tear down while it is still alive (the physics ordering). A game plays 2D one-shots and
music, or holds a `Sound` from a file or a procedural tone and places it in the world; the
listener is the camera, pointed each frame from `game_pre_render` (after the app has posed
it), and a positional source is an `AUDIO_SOURCE` component synced from its entity in the
same pass. **The device is a seam matching the gamepad reader's**: a windowed run opens the
OS device and mixes on miniaudio's own thread; a headless run opens NONE (`noDevice`) and
renders offline through `audio_system_read_pcm`, so the whole layer above the device is a
deterministic function of the frames pulled -- which is what the `audio` gate group asserts
on (onset, pan, distance falloff, bus routing, file decode) with procedural tones and no
committed audio. GLFW's analogue here is the OS device path; nothing in the suite exercises
it, and `docs/verification.md` says which OS it has been heard on (none, at writing).

**The UI is NOT a Game subsystem** (`cetra/src/ui.h`, spec 12.2), and that asymmetry with audio
and physics is the thing to know about it. `create_ui_system` takes an `Engine`, draws through
`engine_set_overlay`, and takes its input as a plain struct of VALUES rather than a
`GameInputState` -- so a menu works with no game framework at all, and the dependency points
from the optional layer to the engine rather than backwards. What the framework adds is only
where the input pass goes: `game_set_frame_input` runs a hook immediately after `input_update()`
and BEFORE the fixed steps, which is the one window in the frame where a UI can take input
without losing one in each direction. A menu opened after the steps would let the same frame
also walk the character, and the frame that closed it would drop a step of real input.

**A menu takes input away from the game with one switch.** `input_set_suppressed` makes every
action whose `ui` flag is false read zero, so a game reads its own action table unchanged and no
call site needs guarding; the UI's own actions carry `ui = true` and keep reading, which is what
lets the key that opened a menu close it. An app raises and lowers it EVERY FRAME from
`ui_captures_input` rather than on edges, so a screen closed by any route -- a button, a
callback, a scene change -- hands input back. Note this is separate from the debug GUI's capture
check, which is applied per SOURCE inside the reader: folding the two together would have let an
ImGui text field silence a gamepad.

**Pausing is the app's, not the layer's.** A menu over a live world needs no support at all,
because pausing stops only the fixed step while the whole render path keeps running -- which is
why the scene behind a pause screen still draws. `apps/gametest` pauses on the EDGES of capture
rather than assigning every frame: assigning made it the only writer that mattered, and the P
key's own pause was put back a frame later.

**A save is not a Game subsystem either** (`cetra/src/game/save.h`, spec 12.3), and for the
same reason the UI is not: `create_save_system` BORROWS the game, and what gets written is what
an app registers. The engine's own half is the sim clock, the entities and their components;
everything else an app hands over, because an entity walk cannot reach a facing angle held in a
file static or the counter that names the next spawned thing -- and a save without those
restores a world whose player faces the wrong way and whose next crate collides with an
existing name. `save_register_table` takes a descriptor table over any struct, and a row may
carry a GET/SET pair instead of an offset, which is how state living inside Jolt or inside a
file static reaches the file at all.

**Two things about a load are worth knowing before reading it.** The first is the failure rule:
file-level problems refuse the file, record-level problems drop the record and count it in
`SaveLoadResult`. A save naming an entity a patched level no longer has is the ordinary
consequence of shipping an update, and refusing the whole file for it would make a released
game unpatchable -- so one unbuildable object never costs a player the rest. The second is that
the entity POSE has to be pushed back into the body rather than stored on the entity:
`entity->position` is a copy `sync_physics_to_entities` rewrites from Jolt every step, so a
restore that wrote only that would be overwritten before the next frame drew, and the load
would appear to have done nothing at all.

## Particle System

A general Niagara-style system: **System -> Emitter -> composable Modules
(spawn/init/update) -> pluggable Renderer**, over SoA pools, with the sim behind a vtable.
**TWO backends ship**: `create_cpu_particle_sim_backend`, and
`create_tf_particle_sim_backend` — a GPU backend where emission and lifecycle stay on the CPU
and the per-particle update runs through transform feedback. It is not hypothetical and it is not
the fallback: `apps/spores` and the `.cscn` particle path both select it. A GL 4.3 COMPUTE
backend behind the same seam would be a third. Particle
systems are **scene citizens**: a `SceneNode` borrows a `ParticleSystem*`, the `Scene`
owns it, and it is auto-ticked and auto-rendered (`render.c`) -- the app just builds and
attaches. **The tick has TWO homes, not one**: `game.c`'s fixed step when the app uses the Game
framework, and the Engine's own per-frame hook (`engine.c:2507-2516`) when it does not, since
with no framework there is nobody else to own the sim. `apps/render` never calls `game_run` and
its window dust still ticks.

```c
ParticleSystem* sys = create_particle_system("spores");
particle_system_set_backend(sys, create_cpu_particle_sim_backend());

ParticleEmitter* em = create_particle_emitter("spore", 20000);
particle_emitter_set_renderer(em, create_billboard_particle_renderer(particle_prog));
particle_emitter_add_module(em, particle_module_spawn_rate(2000.0f));
particle_emitter_add_module(em, particle_module_init_box_location(min, max));
// ... init_lifetime / init_size / init_color, update_curl_noise / drift / integrate
particle_system_add_emitter(sys, em);

scene_add_particle_system(scene, sys);        // scene owns it (ticked + drawn)
SceneNode* node = create_node();
node_set_particle_system(node, sys);             // node transform = emitter spawn frame
node_add_child(scene->root_node, node);
```

The emitter spawns in the node's world frame (transform-at-spawn, then world-space sim).
Ownership invariant: never free a `SceneNode` whose particle system is still registered
on the `Scene`.

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

## Memory Ownership

- Engine owns scenes, shader programs, PostFX.
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

## Example Apps

| App | Path | Purpose | Headless |
|-----|------|---------|----------|
| render | `apps/render/` | FBX/GLB model viewer, orbit camera, animation retargeting, HDR/IBL | yes |
| spores | `apps/spores/` | Cordyceps spore-room particle demo (curl-noise motes, game loop) | yes |
| forest | `apps/forest/` | A walkable ISLAND since 11.63: ~5000 instanced trees/rocks on a CDLOD terrain quadtree, props and collision RESIDENT per region, sea past the shore, character on a Jolt mesh collider (spec 11.29), wind on the trees since 11.53. `--terrain-extent <f>` grows it past a kilometre; `--no-island` is the flat domain everything before 11.63 measured | yes |
| gametest | `apps/gametest/` | Physics/character/entity demo on the action table: WASD or the left stick and dpad, jump on Space or A, boxes on F or X, a hinge door; `--pad-script` replays a scripted pad, `--trace-player` prints the pose and the commanded move each 30 steps (the `gamepad` gate group reads it), `--print-bindings` lists the table. Audio since 12.0: a beep on jump and spawn and a looping tone carried by the door as an `AUDIO_SOURCE` component, `--mute` to silence it, `--audio-probe <case>` for the headless offline render the `audio` gate reads. Animation since 12.1: the player IS the procedural puppet (`assets/models/puppet.gltf`) on an `ANIMATOR` component, blending idle/walk/run from the character's POST-SOLVE speed so a wall stops the walk, a jump one-shot that returns to the space by itself, a wave (E / LB) on the right arm's subtree, footsteps fired from the clips' own events, and a facing yaw on an inner node since the entity node's local is the loop's to write; `--no-puppet` keeps the red box, `--twin <clip>` stands a second rig beside it, `--anim-probe <case>` is the headless probe the `anim` gate group reads, and `--trace-player` appends its `anim ...` tail after `jump`. Menus since 12.2: a main menu, a pause menu, a settings screen and a non-modal HUD, all closed at startup with **Escape** opening the pause menu and Quit an item inside it; `--no-ui` runs without them, `--ui-screen <name>` opens one so a headless run can photograph it, `--ui-focus <n>` moves focus through the real navigation path, `-W`/`-H` size the window, and `--ui-probe <case>` is the headless probe the `ui` gate group reads -- the only probe in the tree that needs no window at all. Foot planting since 12.4: the player's feet are placed on what a downward ray finds, a ramp of known slope and three steps stand in the world to plant them on, always -- flat ground is the one case where planting is correctly a no-op, so a scene without a slope demonstrates nothing. They were gated behind a flag at first to keep them out of the two menu goldens, which photograph the world through a backdrop only 77 percent opaque; those goldens now include them. `--no-ik` runs without planting, `--no-lock` keeps planting and drops the LOCKING above it (spec 12.9), and `--ik-probe <case>` is the headless probe the `ik` gate group reads. **`--puppet <path>` takes any rigged humanoid, and since 12.11 the clips it is lent carry the rig they were AUTHORED on.** A rig with no locomotion of its own is given `SHARED_CLIPS`, and those are loaded with `t_pose.fbx`'s skeleton as the retarget source -- imported as a whole scene, since that is the only public way to a `Skeleton`, and freed straight after, since a channel copies the source bone's index, its parent's and its local rest by value and the sampler rebuilds the hierarchy from those alone. Without it the loader has one rest pose where it needs two: a 122-bone third-party character resolved every chain, measured a 0.12 m/s stride against the reference rig's 5.01, and lay on its face. With it, 51 of 52 channels corrected and an axis of 0.00 to 5.53 m/s. The loader is drained either side of that import -- one `TexturePool` per `AsyncLoader`, and embedded keys are `"*0"`, `"*1"`, ... PER FILE, so two scenes in flight answer each other's lookups. The generated puppet carries its own idle/walk/run and never enters the branch. **Since 12.10 the travel speed comes from the CLIPS.** `build_locomotion` measures each locomotion clip at init; when the moving ones answer, every blend entry sits at the world speed it implies, the knob is metres per second rather than a fraction of anything, and `player_speed` is the fastest entry's stride times the playback ceiling -- 2.67 m/s on `t_pose.fbx` + `strut_walk`, against which `--no-lock` moves 2217 px where the whole of 12.9 measured 0. `CHASER_FRACTION` and `GROTTO_SWIM_FRACTION` are fractions of that for the same reason: written as speeds they outran a player whose speed had become derived. When a clip REFUSES -- the generated puppet's pendulum walk does -- the axis stays a fraction of `PLAYER_SPEED` 10, the run says so in one line, and the feet slide as they did. **The standing entry is anchored at ZERO rather than measured**, which is a correction and not a shortcut: a breathing idle shifts its weight and measures 0.13 m/s, and an axis starting there divides a standing character's 0 by it and clamps to the rate floor, so the idle breathed at 0.6x while nothing moved. What a standing clip lays down is no ground by definition. **Water and air are their own SOURCES, chosen by one value on one edge** (`PlayerMedium`): before that the water was a swim-only edge check and the air was not handled at all, so the jump one-shot ended after a second and a character ran on the spot in mid-air. A rig with no airborne clip never enters that state, which is what keeps the generated puppet unchanged. **The swim axis is STATED where the ground's is measured**, and the asymmetry is a result: a stride is how fast the ground goes past a foot pressing it, a swimmer's feet press nothing, and `animation_stride_speed` refuses the stroke for exactly that reason. **And the submersion test asks the WATER where its surface is** rather than repeating the constant that put it there -- what it cannot ask for is the wave displacement, that being an inverse FFT evaluated in the shader, with `water.h` publishing the still plane and the height variance but no point sample. Two things about that are worth knowing. **The absolute axis deleted a coupling rather than fixing it**: the knob had to divide by the compile-time constant and never by the `--speed` cap or the gear re-normalised, and an absolute axis has no gear. And **a two-tick rest pose beside an 86-tick walk is a phase-sync trap** -- one clock in the shorter entry's ticks played the walk at eighteen times speed at any stick short of full, true from the day the fallback shipped and invisible to every arm, since no probe plays two entries. A grotto since 12.6, a crater on an island since 12.7: the 50x50 plate hangs in the air above a shaft 180 units deep, and the basin around it is no longer nine boxes but an eroded heightfield through `cetra/src/procedural/` -- a hand-filled `TerrainField`, since `terrain_field_seed` nulls the installed field and writes the analytic fbm, and island shaping is a DOME with no inverse. Three things about that terrain are load-bearing and none is visible. **Detail lives in a HORIZONTAL warp, not in the height**: the wall climbs about 6 units per 1.4 across, a slope near 4, so displacing the surface sideways is worth roughly four times displacing it upward, and a vertical ridge term only ever bends the rate of climb without carving a ledge. **Both warps are gated on the UNWARPED radius**, because horizontal displacement drags whatever stands at the sample point toward the centre, and ungated it hauls wall height into the pool until a floating swimmer can plant on the bottom -- caught four times by a measurement of the pool floor's HIGHEST point, which is the rule, and never once by looking. And **every noise source shares one seed**: `terrain.c` memoises the Perlin permutation table one entry deep and rebuilds it with a Fisher-Yates on each miss, so four seeds interleaved seven times a point cost seven million shuffles and 11.7 s of an 15.7 s build. The land runs 1600 units across for a 53-unit crater and drowns below the waterline at its edge, so the domain's own square boundary is under the sea and the only edge in view is a shoreline. Walking off the plate -- always possible and always unhandled, since a `CharacterVirtual` has no y limit and nothing bounded the world -- now drops you into water you swim in. Both characters float rather than sink, on a critically damped drive toward the surface written at the line that applies gravity, because Jolt's buoyancy is not bound in this engine, `gravity_factor` is rigid-body-only and unreachable from a character, and the gravity handed to `character_controller_update` is world-wide. **The seabed is not decoration**: water colour is the refraction resolve of real geometry attenuated over a depth-buffer path, so a pale floor under shallow water IS the turquoise and `scatter_albedo` only tints it -- with nothing behind the surface every pixel takes the maximum path and the sea goes flat. **The platform key is RAKED 45 degrees off vertical since 12.12, and the lamp is MOVED rather than turned.** Hung straight down it lit the plate at `N.L` = 1 and a standing figure at nearly zero -- only shoulders and scalp caught it -- so an arbitrary `--puppet` read as a silhouette against its own brightly lit stage, which auto-exposure then compounded by metering the plate. **Nothing about that is specific to a metallic asset**: a diffuse surface with a horizontal normal receives the same nothing, and the character that exposed it measures mostly dielectric. Turning the lamp alone is not enough -- at 44.5 above the plate that drags the pool `drop * tan(rake)` off centre, 44.5 units on a plate of half-extent 25 -- so it sits at an offset position aiming back at the centre, with the intensity carrying the square of the longer throw or the plate loses a stop and the meter re-opens. The cone is UNCHANGED: an oblique footprint is an ellipse, so the plate spreads 0.87 stops corner to corner against 0.10, but that is a gradient rather than a cutoff and still finishes off the plate. The angle is 45 because the two costs run opposite ways -- most of the spread is spent by 25 degrees while the figure keeps gaining past 40, turning back over only at 55. The swim clip is its own animator source crossfaded on the water's EDGE, never a fourth entry in the locomotion space: that would blank the trace's three weight columns and fail `anim-trace-idle`, and it is the wrong shape anyway, since swim is a different medium rather than a faster gear. The follow camera is ON by default and the ARROW KEYS turn it; `--no-follow-cam` gives back the fixed orbit, the spelling `--no-puppet`, `--no-chaser` and `--no-ik` already use. It was designed to trail the player's smoothed facing and that shape cannot be made to work: `player_yaw` follows the VELOCITY, so the camera is never in front of you, both directions read as forward, and no sign change fixes it -- movement is camera-relative off `cam_yaw` instead. So nothing in 12.6 is behind a flag, which is the demonstration 12.4 argued should not be hidden -- 12.7's four camera flags are the exception that proves it, since `--cam-eye`, `--cam-target`, `--cam-up` and `--fov` state a POSE rather than hide a feature: this app's camera is derived from the player every frame, so without them a framing cannot be photographed twice and no visual change can be A/B'd at all. The two menu goldens photograph the world and the camera alike, and both were re-baked for 12.7 (`5b15ce2d`) and spec 12.9 measured them at 0 px, so the claim that they have been stale since 12.6 flipped the follow camera on is retired. It owns the corpus's only non-render-app goldens, `menu` and `menu_focus`. Frame-deterministic headless with the rig on the frame, but its two menu goldens are NOT reliably 0 px -- re-run before believing any non-zero from them, and see `docs/verification.md` before spending time on one | yes |
| tree | `apps/tree/` | Procedural recursive tree generator with ImGui sliders, on a domed island in a sea with a seabed under it, at sunset, walkable in first person (`--player`); `--no-water` for dry land. Specs 11.32, 11.35, 11.36 | yes (but NOT frame-deterministic on the orbit path: floor is 9k-31k px depending on framing, see `docs/verification.md`) |
| shapes | `apps/shapes/` | Procedural geometry demo (rect/circle/bezier) | no |
| sprites | `apps/sprites/` | The 2023 particle-globe sketch, behaving as it did: 540 hard-square points on a jittering sphere that spins up over time, raw colours through the passthrough tonemap (spec 11.105) | yes |
| network | `apps/network/` | The 2024 wireframe-sphere sketch: its own two shaders on a material, the sphere's triangles reduced to lines, `time` set each frame from the app (spec 11.105) | yes |
| splash | `apps/splash/` | SDF text-rendering test ("CETRA") | no |

## Verification

**`docs/verification.md` owns this** — how to run the two suites, what a release run moves, the
determinism-by-source table, the per-asset ledger of what is safe to compare, and the cross-build
recipe with the six times it has moved. `scripts/gates.py` asserts analytic properties;
`scripts/goldens.py` compares 33 committed PNGs.

**`assets/` is split by KIND since spec 12.8** — `scenes/`, `models/`, `textures/`, `goldens/`,
`generators/`, `lut/`, `ies/`, `data/`, with `abandoned_window/` and `ivy_arcade/` left
whole because each carries its own scenes and producers. **Nothing that names an asset spells a
directory**: a generator says `asset_path("foo.gltf")` to write one and `asset_ref("foo.png")` to
name one inside a file it emits, `gates.py` says `asset("foo.cscn")`, and all three route through
`fixture_paths.asset_subpath` — ONE statement of the taxonomy, imported rather than restated, so a
new fixture lands in the right place with nothing to register and a later re-split changes one
function rather than two hundred call sites. It shipped as two copies of the map and they had
already drifted before either was used in anger, which is the argument for the import. Three couplings are what
that routing exists to hold, and each renders a plausible frame when broken: a generator resolves
its outputs against its own `__file__`, which is what lets `fixture-gen` sandbox one by copying it;
a `.cscn` resolves its model against the SCENE FILE's directory, so it names `../models/<x>`; and a
glTF image `uri` resolves through the texture pool, whose directory is the model's, so it names
`../textures/<x>` — which works because `find_existing_subpath` probes `base + subpath` VERBATIM
first and only then starts stripping leading segments. Stripping alone can only ever shorten a
name, so a BARE sibling filename can never reach another kind's directory; the explicit `../` is
what does the reaching.

**Look at it before you believe a number.** The suites exist so a change can be re-checked
cheaply later, not so a feature can be declared working without anybody seeing it. Spec 12.4's
defect was found by watching the character walk and reported as "the legs don't move" while
eleven of its twelve arms were green; spec 12.9 repeated that exactly, declaring six phases done
off probe output and shipping a demo that could not show the feature at all. A green suite means
nothing broke that was already covered. It does not mean the thing works.

`docs/verification.md` has the rest -- noise floors, what pins exposure across builds, which
binary directory goes with which suite, and when a bake needs `--no-cook`. Read it when you are
about to quote a measurement, not before every change.

Both run on macOS, Linux and Windows (`./build.sh --target`, see `docs/build-vms.md`). Most gate
arms pass everywhere; timing arms and goldens are the two things that do not travel.

## Headless Mode

**render**, **spores**, **forest** and **tree** support headless capture (this line said three
apps for several specs while the table above already said tree was "yes" -- it takes `-x`, `-f`,
`-S` and `--screenshot-every` like the others), since 11.105 **sprites** and **network** take
`-x`, `-f` and `-S`, and **gametest** has taken `-x`, `-f`, `-S` and `--screenshot-every`
since 11.103, plus `-W` and `-H` since 12.2 -- which it needed before it could own a golden,
since a recipe states the size it was baked at (this list left it out for six specs while the table said "no" for the same
span); with `--pad-script` and `--trace-player` (spec 11.109) it is also the one app a
headless run can PLAY. **`--screenshot-every` was the half of that claim
that was not true until 11.62**: forest and spores parsed `-S` but not it, so capturing a
TRANSITION cost one full process per frame -- which on forest is a terrain bake and a 5,000-prop
scatter per sample. It is the plain field `screenshot_every` on the engine, which every
game-framework app reaches through `game->engine` (it rode `GameConfig` from 11.62 until 11.106
folded that struct down to what the game layer owns, and had a setter until 11.108). The render
app:

```bash
./out/bin/render -m model.glb -a anim.fbx --headless --frames 2000 \
    --screenshot /tmp/run.ppm --screenshot-every 400
```

- `-x, --headless` - Hidden GLFW window (`GLFW_VISIBLE=FALSE`), vsync off. Full pipeline
  still runs (GL context, shaders, skinning, GUI code); nothing appears on screen, no
  focus stolen. Orbit auto-rotation is disabled so screenshots are comparable across runs.
- `-f, --frames N` - Exit cleanly after N frames (deterministic, no `timeout`/kill).
- `-S, --screenshot <path>` - Save the final frame as binary PPM before exit. Convert
  with `magick out.ppm out.png`.
- `--screenshot-every N` - Also save `path_000400.ppm`, ... every N frames. Tile into a
  strip: `magick f1.ppm f2.ppm ... -resize 384x216 +append strip.png`.
- `--headless-jitter` - Keep TAA jitter active under headless (otherwise disabled for
  determinism).
- `--no-springs` - Disable spring-bone secondary motion. Required for a byte-identical
  render of any rig that has spring bones (see `docs/verification.md`).
- `-b, --show-bones` - Bone X-ray overlay (bind pose green, animated pose red).
- `--check-stretch` - CPU-skins every vertex at frame 60 and reports triangle edges
  whose animated length exceeds 3x bind length, with the offending bone weights. The
  decisive diagnostic for "spike" artifacts (rigid geometry stays ~1x; a mis-bound
  vertex shows up as a 100x+ edge).
- `--anim-clip <name>` - Play this clip rather than the first `-a` one.
- `--anim-space <a>,<b>` + `--anim-blend <t>` - A two-entry blend space, `a` at 0 and `b`
  at 1, and its knob. Parked on an entry it is that clip alone, to the pixel.
- `--anim-switch-to <name>` + `--anim-switch-at <frame>` + `--anim-fade <sec>` - Crossfade
  to a clip on a named frame (default fade 0.25). After it settles the pose is the
  incoming clip's own, as though it had been snapped at the switch.
- `--anim-layer <name>` + `--anim-mask <bone>` - Play a clip on the override layer over
  that bone's subtree; `--anim-layer-once` plays it once and lets it release itself.
- `--anim-probe` - Print the frame's weights and every bone's global pose, `%.9g`, so a
  textual diff is a bit diff. What the `anim` gate group reads.

**Every other flag is in `docs/cli-reference.md`** — the render app's PostFX and environment set
(TAA, water, sky, clouds, contact shadows, decals, IES, LUTs, layers, probes, the profiler), plus
`apps/tree`'s and `apps/forest`'s own. Read it before driving either of those two apps: several of
their defaults are surprising on purpose (tree's sun sits at 0.8 degrees, forest's sea level is
−35), and the reference records why rather than leaving them to be met cold.

**When to use it:** always prefer headless when verifying behavior -- inspecting log
output (asset imports, skeleton extraction, bone mapping, the first-frame animation
dump, GL errors) or visually checking rendered output. It is also the right mode for CI.
A headless run takes a few seconds and produces the same diagnostics as an interactive
session. From code, set `.headless = true` in the `EngineConfig` handed to `create_engine`.

## Reproducing a session: the config snapshot (spec 11.71)

**Reach for this whenever a question is "why does MY frame look like that".** A `.cscn`
plus a command line does not describe a session — the GUI has ~185 controls and dragging
any of them leaves the state on screen and nowhere else. `--config-dump <path>` (and the
**Dump Config** button beside Print Camera) writes ~230 settings across engine, postfx,
exposure, scene, shadow, sky, ibl, gi and water, plus every probe, light and material.
`--config <path>` reads it back.

```bash
./out/bin/render -m assets/scenes/cornell_rooms.cscn        # drag sliders, hit Dump Config
./out/bin/render --config cetra_config.json          # the same frame, no other flag
./out/bin/render --config cetra_config.json -x -f 2 -S /tmp/agent.ppm
```

**It carries its own model and its own framing**, so the last line is the whole handoff:
give somebody the JSON and they see your pixels. Measured on `cornell_rooms` at 400x300
against a 0 px floor: a session under `--no-ssr --no-ssao --tonemap agx --sharpen 0.8
--vignette --grain 0.05` reproduces at **0 px** from the snapshot alone, where the same
fixture without it differs by 87.6% of frame.

**When NOT to reach for it.** It is a *settings* snapshot, not a scene. It cannot create a
light, bind a texture, place a probe or a fog volume, or author a road — everything in a
`.cscn`'s load-time half has no runtime setter, so restoring those is a re-load. The
snapshot NAMES the `.cscn` in its `source` block and applies on top of it. Precedence is
`defaults → .cscn → CLI → snapshot`, and **the snapshot owns the look while the CLI owns
the run**: `-x`, `-f`, `-S`, `--screenshot-every` and `--profiler` are never written and
never applied, so handing one to a headless capture cannot fight itself.

**Three things it deliberately does not carry**, each for want of a stable key rather than
effort: spring-bone params (per `SceneNode`), SSS profile SLOTS (assigned in
material-block order at load), and `camera.near_clip` (`apps/render/src/render.c:2288` — the APP,
not the engine's `render.c` — recomputes it every
frame from the camera-to-target distance). `config-perturb` names all of them.

**A restored sun or cloud goes through the same chain the GUI sliders do** (spec 11.72).
`scene_environment_changed` in `scene.c` is the one place the env cube, the sky-mirroring
probes and the GI volume re-derive from — the GUI's two release sites and the config
restore all call it. It lived as a file static in `gui.c` for a while, with a comment
claiming it was the one chain, while the restore called bare `sky_update_sun`: a strict
SUBSET, since `sky_bake` is `sky_bake_ex(..., false)`, so a restored sun stripped the cloud
deck out of an env cube that had one, refreshed no probe and re-armed no GI.

**`--clouds` rides the `source` block, not a table row alone**, because the noise bake is a
one-shot at startup gated on the layer being on. A row by itself stored `true` against
noise that never existed: every consumer refused it, the sky came out clear, and the dump
wrote `true` back so `config-roundtrip` passed on the wrong frame. `sky.clouds.enabled` now
refuses by name when it finds no baked noise, and `config-clouds` reads both halves.

**A restored sun still does not re-capture a probe SET** — 11.70 defers that and a set
cannot re-capture, since its members' cubes are released into the atlas. An env-only probe
IS refreshed.

**Animation debugging recipe:** (0) `render -m assets/scenes/puppet.cscn --anim-probe` poses a rig whose bind is pure translation and whose clips are authored in closed form, so a wrong axis, order or handedness reads as a NUMBER rather than a silhouette; `-a assets/models/strut_walk.fbx` on it is a real walk cycle bound by exact name -- **and by itself it is the FAILURE, not a baseline** (spec 12.11): that clip is authored on `t_pose.fbx`, the puppet rests in pure translation, and with only one rest pose in hand the loader has nothing to reconcile, so the delta is identity by construction and the rig folds its legs over its head. Add `-s assets/models/t_pose.fbx` and it walks. The pair is the A/B a retarget is read against, and `anim-retarget` is it as an arm: a foot above the hips and `0 retargeted` one way, below and all 22 the other. Then, on the real rig: (1) render the model with no animation as the bind-pose
baseline; (2) play the animation on its native skeleton (e.g. `-m T-Pose.fbx -a clip.fbx`)
to validate the core pipeline; (3) retarget onto the target model and compare frame
strips. Feeding the T-pose itself as the animation (`-a T-Pose.fbx`) must reproduce the
bind pose exactly -- a quick invariant test for the retargeting path. Cross-rig
retargeting needs the source skeleton's rest pose: a clip exported WITH SKIN embeds its own
rig and it is used automatically; for animation-only files pass `-s <tpose.fbx>` explicitly,
or the rotations reach the target raw. **The loader now says which happened** -- the per-clip
summary's third number counts corrections actually computed, so a run that reconciled nothing
prints `0 retargeted` rather than repeating the matched count, and the bone table's `[R]`
means the same thing. It used to say `52 matched, 52 retargeted` over a table of `0.0 deg [R]`
on a load that had done nothing at all.

## Common Operations

**Load an FBX/GLB scene:**
```c
// The loader is REQUIRED -- passing NULL returns NULL rather than loading synchronously.
Scene* scene = create_scene_from_model_path("model.fbx", "textures/", engine->async_loader);
engine_add_scene(engine, scene);
```

**Create a mesh with a material, and add it to the scene graph:**
```c
Mesh* mesh = create_mesh();
mesh_generate_box(mesh, &(Box){.size = {1, 1, 1}});
mesh->material = create_material();
material_set_program(mesh->material, engine_get_program(engine, CETRA_PROGRAM_PBR));

SceneNode* node = create_node();
node_set_name(node, "my_node");
node_add_mesh(node, mesh); // uploads it, and the upload measures the AABB
node_add_child(scene->root_node, node);
```
Attaching uploads a mesh that has vertices and nothing on the GPU, and `mesh_upload`
takes the AABB, the wind and bone extents, and normals if a triangle mesh has none (spec
11.107). Call `mesh_upload` yourself only after editing the arrays in place, and build a
LOD chain before attaching, since it rewrites the index array. What the draw list will
not draw it says once, naming the node and the reason (nothing uploaded, no material, no
program, a program whose setup failed, a draw mode the program's geometry stage cannot
take); nothing is dropped in silence and nothing reaches a GL error at the draw.

**A camera and a light**, each from a description struct -- fill the fields you mean,
zero is the default named in the header (spec 11.106):
```c
CameraDesc cam = {.position = {0, 2, 5}, .fov = glm_rad(45.0f)};
engine_set_camera(engine, create_camera(&cam));

LightDesc key = {.type = LIGHT_DIRECTIONAL, .direction = {-0.3f, -1, -0.5f}, .intensity = 3};
scene_add_light(scene, create_light(&key));
```
After creation a Camera's pose goes through `camera_set_position` / `camera_set_look_at`
(each re-derives the orbit parameters, so a camera moved by pose and then orbited continues
from where it is) and a Light's frame through `light_set_position` / `light_set_direction` /
`light_set_up` (the authored copy the node transform carries); every other field on either is a
plain write. A
light's intensity is the one desc field whose zero is a value: zero emits nothing, which is
how a scene file declines the default rig, so a lit light says how bright. A scene with no
light at all, no environment and zero ambient is warned once as it renders; the zero light
counts as an answer and is not (spec 11.107).

**Everything else on a live object is a field, and the header says which** (spec 11.108; the
three roles are stated once, at the top of "Key Data Structures"). Under a struct's SETTINGS
banner the fields are plain writes at any time:
```c
engine->show_fps = true;
engine->exit_after_frames = 30;
engine->postfx->fog_density = 0.002f;
mesh->draw_mode = MESH_LINES;
node_set_position(node, (vec3){0, 1, 0}); // the translation column of original_transform
```

**Run the low-level engine loop:**
```c
EngineConfig cfg = {.title = "Title", .width = 1920, .height = 1080};
Engine* engine = create_engine(&cfg); // window, GL context, targets, post chain, programs
// Three hooks, in frame order; any may be NULL. pre_render is where the camera
// and any graph change go -- the engine propagates the graph and derives the
// camera's matrices right after it. A NULL render draws the scene itself.
engine_run(engine, update_callback, pre_render_callback, NULL);
free_engine(engine);
```
The frame clears to `engine->clear_color` (0.1 grey by default) where nothing draws.

Everything the window or the first render-target build reads is an `EngineConfig` field
(`headless`, `profiler`, `msaa_samples`, `taa`, ...), which is what makes the old
"before init / after init" ordering rules unwritable. For game-style apps (fixed timestep,
physics, particles) use the game framework instead -- see the Game Framework section.

**Naming** (spec 11.106): subject first -- the first word is the type of the first parameter
(`engine_`, `scene_`, `node_`, `mesh_`, `material_`, `camera_`, `light_`, `game_`), then the
verb, then the object; `create_X` / `free_X` are the one exception. An `x_get_y` that returns
NULL logs; an `x_find_y` is silent and for the optional case (`engine_get_program` versus
`engine_find_program`). A mutator returns void and logs on a NULL argument or OOM; a request
that can be refused for a reason the caller can act on (a bounded array, `node_remove_child`)
returns bool; factories and lookups return NULL on failure. Built-in program names are the
`CETRA_PROGRAM_*` constants in `program.h`.

**Includes**: an app names only `cetra/<header>.h`. `cetra/CMakeLists.txt` allowlists which
headers are public; every other header forwards to `cetra/internal/<header>.h`, which only
in-tree apps can see (`add_cetra_app` adds that tree; a consumer through `add_subdirectory` or
`FetchContent` cannot name it). An app that must include an internal header is a debug harness
and says so in its header comment. The allowlist governs what an app may NAME in an include,
not what a public header's closure reaches.

## Conventions

- All transforms are 4x4 matrices (mat4 from cGLM).
- Positions in world space; transforms propagate parent-to-child.
- Y-up, right-handed coordinate system.
- Angles in radians.
- Colors as vec3 (0.0-1.0) or hex integers.

## Comments

The test for every comment: **does it say something the code at this location
cannot?** Intent, invariants, units, non-obvious whys -- never a restatement of
the code beside it.

- **Declaration comments state the contract, in one line.** What the field
  means or what setting it does, in the codebase's own vocabulary:
  `bool area_lights_enabled; // false = LIGHT_AREA lights are skipped at gather`
  -- not the downstream mechanism that gives it effect.
- **Mechanism is documented where it is implemented.** Never narrate another
  file's causal chain (a header comment describing what the .c or a shader
  does rots silently -- whoever edits the mechanism has no reason to know the
  distant comment exists).
- **Never name the callers.** "(GUI toggle)" on a struct field points the
  dependency arrow backwards and goes stale the moment a second caller
  appears.
- **No self-praise and no verification claims** ("exactly the no-panel path",
  "measured 0 px") -- those are commit-message material, not code.
- **Long comments must earn their length** by recording something the code
  cannot express: a rejected alternative, a measured tradeoff, a failure mode
  being avoided (the `touched` bitset comment in `light_cluster.h` and the
  `_POSIX_C_SOURCE` note in this file are the house standard). Length is
  fine; content-free length is not.

## Reviews

Always run code-quality reviews in BACKGROUND agents, and wait for all of them to finish before
applying any change. Applying fixes while a review is still running means the next one reports
against a file that has moved, and its line numbers stop resolving.

Two are used here, neither of them shipped with this repository:

- **`/thermo-nuclear-code-quality-review`** — an ultra-strict quality audit from the Cursor Team
  Kit, at
  <https://github.com/cursor/plugins/blob/main/cursor-team-kit/skills/thermo-nuclear-code-quality-review/SKILL.md>.
  Install it, or read the SKILL.md and apply its standard by hand; the useful part is that it
  hunts for restructurings that DELETE complexity rather than rearranging it.
- **`/simplify`** — a fan-out of reviewers over the diff for reuse, simplification, efficiency and
  whether a fix sits at the right depth. Quality only; it is not a correctness pass.

Neither substitutes for reading the diff, and neither substitutes for running the thing. A
review that returns nineteen findings against code nobody has watched work is an expensive way
to polish something broken.

**File length is not a review finding in this repo, and any review checklist saying otherwise is
overridden here** -- specifically the 1000-line threshold the thermo-nuclear skill treats as a
presumptive blocker. Decomposition is argued from structure:
two things in one file that change for different reasons, or a function nobody can hold in their
head. A file that is long because the subject is long stays long.

## Edits

**Default to the Edit tool, and the reason is REVIEW, not tooling taste.** An Edit shows the
before and after in the transcript, so a change can be read as it is made; a `python3 - <<'PY'`
heredoc that rewrites a file shows the script and not the result, so the same change arrives
invisible and is only reviewable after the fact by diffing. Prefer Edit even when a script would
be fewer keystrokes.

Python is still the right tool for a mechanical sweep no human is going to read line by line --
renaming a symbol across thirty files, regenerating a fixture, reformatting a table. Use it there.
The test is whether the edit is one somebody would want to see: if it changes behaviour, use Edit.

# ABSOLUTE RULE -- NEVER RUN rm -rf

**NEVER** run `rm -rf` (or any recursive/forced delete) for any reason, including cleaning up
temp or scratch directories mid-task. It triggers a permission prompt that BLOCKS the session,
which kills any unattended run dead until a human comes back. Leave stale temp files and
directories alone -- disk is cheap and a blocked session is not. If a fresh directory is needed,
use a new differently-named path instead of deleting the old one. If something genuinely must be
deleted, ask the user and let them do it.

