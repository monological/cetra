# Cetra AAA Rendering Roadmap — Master Plan

## Context

Cetra is a C11 forward-PBR engine on a strict GL 4.1 core ceiling (macOS — no compute, no SSBO).
It already has a strong screen-space stack (TAA, GTAO+SSGI, hi-Z SSR, WBOIT, Hillaire sky, CSM+PCSS,
split-sum IBL, parallax-corrected probes). The gap to "AAA look" is not more post effects — it is
**global illumination fidelity, light-count scaling, and volumetric atmosphere**, all architectural.

This master plan is a tiered roadmap of SIGGRAPH-grade features, each feasible as fullscreen raster
passes + LUTs + probe bakes on GL 4.1. The plan will be committed to `specs/` as the umbrella spec;
each tier item later gets its own subplan (feature branch + spec) before implementation.

**Status: Tiers 1-3 are closed.** Every item is DONE, REJECTED-with-a-measurement, or CLOSED-and-split
— the original plan is finished. **Tier 4 (Tracks C/D/E below) is the new frontier**, and it is shaped
by a different constraint than Tiers 1-3 were: those items could each be built as another gated
fullscreen pass, and Tier 4's cannot. Four structural walls now decide what is reachable at all; they
are stated before Track C because half the Tier 4 items are blocked on one of them. Two have since
fallen (Wall 3 to E4, most of Wall 2 to E5) and one was added after the fact (Wall 4), which is the
section's own record of having mis-framed the geometry problem.

Everything in Tracks C/D/E is a **sketch**, in the sense this document has taught the word: a
pre-implementation guess whose load-bearing claims are wrong often enough that B3 lists four, B6 lists
three, and A6 lists two of three premises stale by the time the code landed. Read each Tier 4 row as
a hypothesis with an effort guess attached, not as a design.

**User decisions locked in:**
- **Environments first** — GI, clustered lighting, clouds, fog lead; skin/hair follow.
- **Strict GL 4.1, single code path** — no dual GL 4.3 compute variants. Individual shaders may bump
  `#version 330 core` → `#version 400 core` only where genuinely needed (e.g. textureGather).
- **Hair stays in the roadmap as a late-tier item.**
- **Just-in-time foundations** — shared infrastructure lands with its first consumer; the spec names
  which feature owns which foundation piece.

## Verified architecture facts (from code exploration)

Ground truth that shapes every design below (library at `cetra/src/`, shaders at `cetra/shaders/`):

- **G-buffer**: 5 MRT attachments on one MSAA FBO, source-of-truth table `_gbuffer_attachments()`
  (`cetra/src/engine.c:60-93`): att0 HDR RGBA16F, att1 view-normal+marker RGBA16F, att2
  motion+linZ+roughness RGBA32F, att3 albedo+metallic RGBA8, att4 SSS-diffuse RGBA16F.
  **Attachments 5-7 free** on the main FBO (5/6 used only on the separate OIT FBO).
  Write-gating via `*_this_frame` flags from postfx `_wants_*` predicates.
  - **RULE, and it is currently violated: nothing categorical or positional may live in a channel
    that gets MSAA-resolved.** An MSAA colour resolve is a box filter
    (`resolve_color_attachment`, `postfx.c:1992`, a `glBlitFramebuffer`), so it is only valid for
    quantities linear in radiance. Two channels in the table above are not. **att4's alpha is a
    categorical SSS profile tag** (`pbr_frag.glsl:2020-2021`) — the average of tag 2 and tag 0 is
    tag 1, which does not mean "half of profile 2", it means *profile 1*, so meaning is destroyed
    rather than degraded; at 4x MSAA a half-covered grass pixel in `apps/tree` is silently shaded
    with the LEAF profile. **att2's `.z` is a view depth**, i.e. a position, and the average of two
    surfaces is a surface that exists nowhere; averaged against the sky's 0 it inflates the SSS
    gather's `basePx` and jumps it to a far coarser LOD. **Both are real defects, and neither is
    the reported "popping fireflies" on flowers** — that attribution stood here until elimination
    disproved it (spec 11.38). The cause was an extrapolated VARYING and these two amplified it,
    the scatter pyramid faithfully spreading a genuinely out-of-range input. Fixing them is still
    right; expecting the specks to go with them is not. Untested suspects under the same rule: **att2's `.w` roughness** (averaging
    roughness is wrong for the reason averaging normals is; the correct operation averages the
    NDF — Toksvig/LEAN — which spec 11.35's far-field handover already does, so the engine gets
    this right in one place and not in the resolve) and **att1's `.a` SSR marker**, a negative
    alpha whose magnitude is the catcher's edge falloff, so part categorical and part continuous.
    The depth half is fixed (11.37 phase 1, from the depth buffer, which resolves by selecting).
    ~~The fix is stencil for the tag~~ — **struck: stencil was built, measured and reverted**
    (11.37 phase 2). It does remove the misfile, +2.174% to 0.000%, and it breaks `sss-scale`,
    the gate holding scatter width independent of resolution. **Per-sample storage cannot serve a
    per-pixel reader**: the SSS passes are fullscreen quads, so they read ONE stencil value per
    pixel, and resolving a per-sample tag selects a single sample and discards coverage — a rim
    pixel 90% covered is dropped if that sample missed, one 10% covered is kept if it hit. The
    alpha rule it replaced was at least monotonic in coverage, and monotonic error cancels across
    resolutions where arbitrary error does not. So it trades a categorical error for an arbitrary
    one. **The tag defect stays open**, measured and guarded by `sss-tag`. Note the industry's
    answer to this exact incompatibility was
    to stop multisampling the G-buffer: it is why MSAA left AAA after ~2013, and this engine's
    interactive path is *already* 1-sample + TAA (`render.c:3029-3040`) and structurally immune —
    the defect lives on the headless default and explicit `--taa --msaa N`. Full recipe and
    feasibility check in `specs/11.37-sss-tag-and-depth-resolve.md`.
  - **The SAME root cause on the INPUT side, and it is not fixable the same way** (spec 11.38).
    The rule above is about the resolve: one box filter over N samples. Its sibling is that an
    MSAA fragment **shades once per pixel at a point that may lie outside the primitive** —
    coverage is per sample, shading is per pixel, so on a partly covered pixel a varying is
    extrapolated and every value leaves the range its vertices bound. Both are the same
    mismatch: shading rate is not coverage rate.
    `RENDER_MODE_EXTRAPOLATION` measures it, exactly and on any asset, because `pbr_vert`
    normalizes `Normal` and `buildTBN` the tangent, so a length above one proves a negative
    barycentric weight. Live at 2,198 px on an `apps/tree` frame and 1,591 on raiden.
    **`centroid` is the textbook fix and was measured and rejected**: it makes derivatives
    inexact (undefined per the GL spec) and every remaining varying feeds one — `dFdx(Normal)`
    the specular AA, `dFdx(WorldPos)` the curvature, UV gradients the hardware's texture LOD.
    Every variant moves 9 to 22 goldens with the diff following the TESSELLATION across whole
    surfaces, up to 310,313 px on the skin fixture. `VertexColor` already carries the qualifier
    at 0 px moved because it is the one varying nothing differentiates. The real answer is the
    one directly above: **do not multisample.** 1 sample + TAA is immune by construction.
- **Temporal primitive**: `PingPong` + `run_temporal_accum()` (`cetra/src/postfx.c:1143`) shared by
  TAA/AO/SSGI/SSR/fog/SSS; invalidation = `valid=false` on any skip; master gate `taa_resolving`.
- **Pass template**: the fog pass is the canonical gated/lazy/half-res/temporal/composited postfx pass
  (fields `postfx.h:177-214`, ensure `postfx.c:655`, run `postfx.c:1197-1298`).
- **PBR texture-unit budget is SATURATED**: 16-unit GL floor; unit 9 (reflectance) is
  reserved-but-unsampled and unit 7 was the last genuinely free slot. **A4 took unit 14** for the GI
  atlas -- the skybox's number, which is safe only because units are per PROGRAM and `pbr_frag` has
  never sampled the skybox cube. The `_Static_assert` chain (`cetra/src/render.c`) now pins that
  sharing as an equality, so moving either one has to come here and decide. Anything else wanting a
  new sampler in `pbr_frag` must first free one.
- ~~**No UBOs exist anywhere**~~ — **STALE, fixed by A1 (spec 9.1).** Three std140 UBOs now carry
  the whole light path (`include/lights_ubo.glsl`), uploaded once per frame rather than per program
  switch per node. `get_closest_lights` and the per-node `uniform Light lights[64]` upload are both
  **deleted**; any item below still costing them into its budget is over-counting. Everything else
  is still by-name `glUniform*` via `UniformManager`, with no value caching.
- **No 3D textures exist anywhere**; `GL_TEXTURE_2D_ARRAY` idiom established (raw `glTexImage3D`,
  no `glTexStorage3D` which is GL 4.2) in `mask_array.c:139` and `shadow.c:146`.
- **No float-from-memory texture helper** (`load_texture_from_memory` is 8-bit only); GPU-bake LUT
  precedent exists (BRDF LUT RG16F 512², `ibl.c:496-527`; sky LUTs `sky.c`).
- **Area lights are fake**: `LIGHT_AREA` shades as a point light; `light.size` only feeds PCSS
  emitter size. Karis sphere-light lobe widening (`pbr_frag.glsl:967-977`) applies to all lights.
- **GTAO** is 2023 visibility-bitmask (2 slices × 8 steps, 32-bit sector mask); bent normal is
  directly derivable from the free sector bits; AO target is R8 today (spare channels discarded).
  Specular occlusion today = scalar heuristic in `tonemap_frag.glsl:154-175`.
- **Probe capture renders the full scene at arbitrary positions** (`reflection_probe_capture`,
  `probe.c:50-263`, camera save/substitute + 6 faces via `ibl_capture_views`) — the DDGI-reusable
  machinery. No octahedral encoding exists yet.
- **Sky**: Hillaire LUTs (transmittance 256×64 once, multiscatter 32×32 once, sky-view 192×108 on
  sun move); `sky_bake()` drives env cube → IBL → key light together. Aerial perspective added by
  9.6 as a 32³ volume — the one sky target rebuilt per frame, since it is the camera's frustum.
- **Fog**: screen-space half-res raymarch with CSM god rays + volumetric spot; HG phase; all logic
  portable to froxels. Light data arrives via `shadow_publish_to_postfx`.
- **TAA**: 8-frame Halton on `draw_projection[2][0/1]`; YCoCg 3×3 clamp + Catmull-Rom history;
  internal-vs-display resolution concept already exists (`fx->width` vs `fx->out_width`,
  downsample only at tonemap) — TAAU inverts it.
- **Shaders**: all `#version 330 core`; build-time `#include` from `cetra/shaders/include/` via
  `gen_shader_header.py` → `shader_strings.h`. New pass = shader file + `create_*_program` +
  PostFX field (postfx programs are not in the engine uthash cache).
- **Scene subsystem pattern**: Scene owns shadow_system (auto), ibl/probe/sky (app-created,
  assigned); subsystems borrow the IBL capture FBO/cube VAO; `*_publish_to_postfx` flattens state.
- **Verification**: headless deterministic renders (`-x -f N --no-springs --no-auto-exposure -E 1.0
  -S out.ppm`), raiden GLB as byte-identical cross-build baseline, `magick compare -metric AE`.

## Global decision: pbr_frag texture-unit ledger

Binding for all features (three new links in the `_Static_assert` chain, `render.c:34-47`):
- **unit 7** → `ltcTex` (LTC 2-layer array: M-inverse + magnitude/Fresnel — packed in 10.7.1)
- **unit 9** → `charliePrefilteredMap` (Charlie sheen env cubemap — took the unit the LTC pack freed in 10.7.1)
- **unit 14** → `giAtlasTex` (DDGI octahedral atlas, sampler2D — legal since sampler units are
  per-program and pbr_frag never sampled the skybox cube)
- Contact shadows + bent-normal spec-occ consume **zero** pbr_frag units (postfx-only).
- Clustered forward consumes **zero** units (UBOs, not data textures).
- Pre-integrated skin consumes **zero** units (spec 11.13). The fit did disappoint — the analytic
  form cost 9x the achievable error — but the escalation was to a 16-row `const` array in GLSL, not
  to a sampled texture, so neither the ledger nor unit 14 was ever at risk. Worth generalising: the
  "no LUT" constraint this ledger imposes is a constraint on *texture* lookups, and a table small
  enough to live in uniform/const space is not one.

**The ledger is now FULL — 16 of 16, with no reserved slack left.** As of A7 and 11.17 the map is
0 albedo, 1 normal, 2 masks, 3 clearcoat-normal, 4 height, 5 emissive, 6 scene-colour, 7 LTC,
8 sheen, 9 Charlie env, 10 CSM (the depth array, or A6's RGBA16F moment array in its place under
`--msm` — one sampler either way, which is what took A6 from L to M), 11 IBL irradiance,
12 IBL prefilter, 13 BRDF LUT, 14 skybox/GI atlas (shared by the equality assert), 15 punctual
shadow array. 11.17 established the rule that makes this a hard stop rather than a tight fit: **the
driver counts sampler *declarations*, not uses**, so there is no seventeenth sampler in `pbr_frag`
for a free unit either. Two escapes are already precedented and both are narrow — ride an existing
declaration through a `#define` when the two consumers are mutually exclusive (moments over
`sceneColorTex`), or put the table in `const`/uniform space (11.13). See **Wall 1** below.

## Track A — Direct Lighting & Global Illumination

### A1. Clustered forward light culling (Olsson et al. 2012) — Effort L
Scale past 64 lights and delete the per-node uniform storm. **UBOs, not data textures** (zero
texture units; kills the 13-glUniform-per-light-per-program-per-node hot loop). Three std140 blocks,
each <16KB min guarantee: `LightsBlock` (4 directionals outside clustering + 128 packed
point/spot/area lights, 6 vec4s each), `ClusterBlock` (16×8×24 grid, packed offset|count uints),
`ClusterIndexBlock` (4096 16-bit indices). CPU cluster assignment in new `light_cluster.c`
(frustum cull → tile X/Y + exponential-Z slice ranges, Doom-2016 slicing); 3 `glBufferData` orphans
per frame. **Deletes** `_update_program_light_uniforms` (`render.c:91-153`) and retires
`get_closest_lights` from the render path. Only pbr_frag declares `lights[]` (verified) — migration
scope is one shader. Legacy path kept behind `--clustered` (default off) during the branch for the
0-diff gate; final commit flips default and deletes legacy. Directional-only raiden baseline expected
byte-identical (directionals bypass clustering, unchanged evaluation order).
New: `ubo.c/h`, `light_cluster.c/h`, `include/lights_ubo.glsl`. CLI: `--point-light-grid N`;
ImGui cluster-heatmap debug view.
**Owns foundations:** UBO machinery + std140 packing + binding registry; packed light representation;
CPU light-culling module. **Depends on:** nothing.

### A2. LTC area lights (Heitz/Dupuy/Hill/Neubelt 2016) — Effort M
True polygonal area-light diffuse+specular. Embed the standard 64×64 LTC tables as checked-in C
float arrays (offline fit — not GPU-bakeable; codegen via new `tools/gen_ltc_lut.py`, run once),
uploaded via new `create_texture_2d_float()` helper in texture.c. In pbr_frag's light loop,
`type==3` branches into `evalLTC()` (new `include/ltc.glsl`) — quad edge-integrals, corners built
in-shader from `position ± right*size/2 ± up*size/2`; all other light types keep the exact existing
expression stream (directional baselines stay byte-identical). New `Light.up`/`original_up` frame
(rotated by node transform like `direction`; +3 uniform components/light, folds into PackedLight
when clustered lands — one-line layout append). v1 limits: single-sided, ~~no area shadows~~
(**A7 delivered them**, spec 9.8: one map down the panel normal, so the occlusion is hard where the
source is soft), clearcoat/sheen skip the area branch. Karis sphere-widening untouched (area lights bypass it).
New: `ltc_lut.h` (generated), `include/ltc.glsl`. CLI: `--area-light px,py,pz,dx,dy,dz,w,h,I[,rgb]`
(the flag IS the test asset). **Owns foundations:** float-texture-from-C-array helper; `Light.up`
orientation frame; embedded-table codegen pattern. **Depends on:** A1 preferred (edit final loop once).

### A3. Screen-space contact shadows (Uncharted 4-style) — Effort S
8-step per-pixel march along the key light in a new postfx pass at AO res, slotted between
depth/normal resolve and GTAO. Same-frame application: composited in tonemap as an independent
multiplier beside `aoVisibility()` (`tonemap_frag.glsl:154`) — matches the engine's existing
"screen-space occlusion multiplies HDR in tonemap" contract, costs zero pbr_frag units, no frame
latency. Reuses `include/depth.glsl` reconstruction, 4×4 noise dither, existing `ssao_blur`
bilateral, `run_temporal_accum` with new `cs_history` PingPong. Key-light view-space direction
published via `shadow_publish_to_postfx` extension. Enabling forces `postfx_wants_aux`.
New: `contact_shadow_frag.glsl`. CLI: `--contact-shadows`, `--cs-distance`.
**Owns foundations:** key-light direction publication to postfx (reused by future light-aware post
passes). **Depends on:** nothing.

### A4. DDGI-style irradiance probe volume, rasterized (Majercik 2019 adapted) — Effort XL — **DONE (spec 9.7)**
Shipped. Read `specs/9.7-ddgi-probe-volume.md` rather than the sketch below, which was written
before the code and is wrong in four places kept here for the record: the per-frame budget (the
volume converges then idles, doing literally zero work per frame once dirty_count hits 0 -- a fixed
budget forever would mean 12 full scene renders per frame, each rebuilding the whole 3072-cluster
light grid regardless of the 16^2 viewport); the gutter rewrite (folded into the projection pass, so
`gi_border_frag.glsl` was written and deleted -- and with it a read of the atlas while the atlas was
the render target, undefined in GL 4.1); the `pbr_frag.glsl:1169` citation (it is 1209, and the swap
is not one line -- the SSS tap reuses the same sub-expression); and `cornell_box.glb` (it is
`.gltf`, generated by `assets/gen_cornell_rooms.py`, matching all 13 other fixtures).

The track's long pole and the biggest visual payoff: multi-bounce dynamic diffuse GI. New
Scene-citizen subsystem `GIVolume` (`scene->gi_volume`, mirrors probe/sky ownership). Uniform grid
(default 8×4×8, scene-AABB bounds). Round-robin budget (default 2 probes/frame): render scene 6× at
**16² faces** via `scene_capture_faces()` — the camera-substitution core extracted from
`reflection_probe_capture` (`probe.c:50-263`) into a shared helper; cosine-convolve into **8×8
octahedral irradiance tiles** + **16×16 octahedral visibility tiles** (distance/distance² moments
for Chebyshev leak rejection); blend into ONE RGBA16F 2D atlas with fixed-function constant-alpha
blending (`glBlendColor`, hysteresis 0.97 — no ping-pong hazard); rewrite 1px oct-wrap gutters.
Startup burst converges all probes with hysteresis 0. Sampling (`include/gi_volume.glsl`): 8-probe
trilinear at `WorldPos + N*bias`, backface + Chebyshev tests, replacing the flat
`texture(irradianceMap, N)` diffuse term (`pbr_frag.glsl:1169`); specular IBL untouched. Because
captures run the full forward shader, probes automatically see clustered lights, LTC panels, sky,
emissive — the payoff of rasterized capture.
New: `gi_volume.c/h`, `gi_project_frag.glsl`, `gi_border_frag.glsl`, `include/octahedral.glsl`,
`include/gi_volume.glsl`; test asset `cornell_box.glb` (must be authored). CLI: `--gi-volume`,
`--gi-probes X,Y,Z`, `--gi-rate N`; ImGui probe-sphere debug draw.
**Owns foundations:** octahedral encode/decode include; generalized capture-at-position helper;
atlas+gutter machinery. **Depends on:** A1+A2 soft (probes should capture clustered/area-lit scenes).

### A5. Bent-normal specular occlusion from GTAO (Jimenez 2016) — Effort M — **DONE (specs 11.3 + 11.4)**
11.3 built the machinery behind `--spec-occ <off|legacy|bent>` but its look review found
mottling on smooth metal that is architectural, not tunable: the reflection vector queries at
normal-map frequency while the bent normal is a half-res geometry-scale field, and the
tonemap-stage term must guess the specular share of a pre-summed buffer. 11.4 fixed the
placement: ambient specular routes to its own G-buffer attachment (7, R11G11B10F) and a
composite pass occludes exactly that share with the cone term — before TAA, so the reunited
frame stabilizes as one image — with plain AO on everything else. Default is `split`; the
mode's TAA churn measures lower than legacy's, and the four-mode combo stays for A/B.
Directional spec occlusion nearly free from the existing visibility-bitmask GTAO: bent normal =
popCount-weighted average of the UNSET sector bits (the shader's own comment calls this out).
**Widen existing AO targets** rather than second MRT: `AoOut = vec4(ao, bentN*0.5+0.5)`, ssao_fbo
R8→RGBA8, ao_history R16F→RGBA16F, 4-channel bilateral blur + renormalize; AO stays in `.r` so all
existing consumers read identical values. Consumption: replace the scalar `aoVisibility()` heuristic
in tonemap with Jimenez cone-cone intersection (visibility cone from bent normal + `acos(sqrt(1-ao))`
aperture vs GGX reflection cone from roughness). Three-state `specOccMode` uniform (off/legacy/bent)
keeps the 0-diff gate. Follow-up (not v1): bent normal as the DDGI/IBL diffuse sampling direction.
New files: none (edits only). CLI: `--bent-spec-occ`.
**Owns foundations:** bent-normal availability in the AO chain (later: SSGI directionality, SSR
occlusion). **Depends on:** nothing.

### A7. Punctual shadow maps — shadows for all four light types — Effort M — **DONE (spec 9.8)**
Directional lights had cascades; spot lights had exactly one map ("the flashlight", first spot in
scene order); point and area had none, while `.cscn` accepted `cast_shadows` on them and silently
dropped it. Unit 15's single `sampler2D` became one `sampler2DArray` carrying every perspective
map — spot 1 layer, point 6 (cube faces as ordinary layers picked by dominant axis), area 1 down
the panel normal, multiplied into the LTC term. The design was forced rather than chosen:
`pbr_frag` samples all 16 texture units, and `samplerCubeArray` is GLSL 400 against a uniformly-330
shader set, so a second sampler had nowhere to bind. The directional path is untouched, which is
what kept `froxel_fog`, `contact_debug` and `aerial_fixture` at 0 px throughout.
**The limiting factor is per-frame scene traversals, not VRAM** — every layer is re-rendered each
frame, so a point light costs six. Pool of 8, demand-allocated, exhaustion logged by light name.
**Residual:** a one-texel bright hairline where an object rests on a receiver, inherent to the
depth pass front-face culling (the map stores the occluder's far surface, which coincides with the
receiver at the contact). Back-face culling trades it for acne. Untried candidate: receiver-plane
depth bias.

### A6. Moment shadow maps (Peters & Klein 2015) — Effort L — **DONE (spec 11.22)**
Filterable 4-moment shadows: RGBA16F array beside the DEPTH24 CSM array, Hamburger 4MSM single-tap
via `include/msm.glsl`. Shipped opt-in behind `--msm`; PCF stays default and PCSS stays the default
soft-shadow path. **Two of the sketch's three premises were stale.** The moments are resolved *from*
the finished depth array rather than written in the depth pass, so the depth pass, its polygon offset
and its front-face culling are untouched and `--no-msm` is a *provable* 0 px, not a hoped-for one —
that also defeats spec 10.4's "restructures the depth pass" objection to VSM. And it costs **no new
sampler**: `csm.glsl` declares one `sampler2DArray` read as `.r`, so an RGBA16F array *replaces*
unit 10 instead of adding to it, which is what made this an M rather than the L the sketch priced.
The fog bonus was already delivered (`postfx_build_fog_esm`) and is untouched. **75 MB at the 1024²
default**, half the sketch's estimate: the blur scratch is lazy and the shipped blur of 0 never
allocates it. **MSM's one real capability win is that it has no penumbra cap** — `PCSS_MAX_RADIUS_UV`
hard-limits PCSS softness because its blocker search costs taps per unit of radius, and a prefiltered
tap does not. On contact-heavy scenes PCSS still makes the better image, hence the flag.
**Residual:** blur is where four moments run out on a thin caster — 1.0 leaks 0.4159 through the
pillar's band, so the default ships at 0, and the sketch's prediction that one prefiltered tap would
beat 32 stochastic ones is *not* borne out there (churn 7775 px vs PCF's 7545).

## Track B — Atmosphere, Volumetrics, Character & Post

### B1. Froxel volumetric fog (Wronski 2014 / Hillaire 2016) — Effort L
Replace the screen-space fog march with a camera-frustum 3D froxel volume. GL 4.1 trick: **layered
rendering via geometry shader** — one `glDrawArraysInstanced(triangle, depth)` draw, trivial GS sets
`gl_Layer` (GS plumbing proven by `shape_geo.glsl`). Three passes: (1) **inject+light** into
`froxel_scatter` 160×90×64 RGBA16F (exponential slices; ports `fogVisibility()` CSM taps, spot
block, HG phase, height-fog sigma verbatim from fog_frag; per-frame jittered sample + 90/10 blend
vs reprojected previous volume; static jitter headless = deterministic); (2) **integrate** —
deliberately O(n²) front-to-back gather per slice (~30M cheap taps at 64 slices — sidesteps the
read-write hazard a sequential scan would hit without glTextureBarrier, which is 4.2);
(3) **composite** — full-res, 1 trilinear tap, existing
`glBlendFunc(GL_ONE, GL_SRC_ALPHA)` idiom (B9 adds its aerial tap here later). The screen-space
march and its half-res buffer/history/tent-upsample are **deleted**, not toggleable: two fog
implementations meant every parameter change had to land in two shaders, and the A/B that
justified keeping one was already run and recorded. Fog
sun/ambient stay **app-set** in this item — moving them to sky-published lands in B9, so the froxel
pass keeps a clean A/B against the old screen-space pass (changing the color source at the same time
would confound the parity gate).
New: `froxel_inject/integrate/composite_frag.glsl`, `include/froxel.glsl`.
CLI: `--no-fog-volumetric` (the volume is the default).
**Owns foundations:** `create_texture_3d()` (first 3D texture in the codebase); volume-draw
machinery (`create_volume_fbo`, `draw_volume_slices`); `include/froxel.glsl` frustum mapping.
**Depends on:** nothing hard; consumes A1's clustered light list for local-light scattering when
present (degrades to today's sun+spot coverage without it — no blocker).
**As built (spec 9.5):** slices are drawn one layer at a time via `glFramebufferTextureLayer`
rather than the layered-GS sketch above — that is the codebase's existing idiom (shadow cascades,
mask-array layers, IBL cube faces), reuses `post_vert.glsl` unchanged, and avoids adding a
geometry shader to a tree where every shader is `#version 330 core` and none uses `gl_InstanceID`.
`gl_Layer` would in fact have needed no version bump; only `layout(invocations=N)` would. Temporal
reprojection goes through a PostFX-owned copy of the previous camera, since `prev_view_proj`
already holds the current frame's matrix by the time postfx runs.

### B2. Volumetric clouds (Schneider/Vos HZD 2015, Nubis) — Effort XL
**Folded into SkyAtmosphere** (`sky->clouds` sub-struct), not a new scene subsystem — clouds share
sun_dir, transmittance LUT, bake trigger, publish path. **Shared march include with quality knobs**
(`include/clouds.glsl: cloud_march(ro, rd, steps, light_steps, detail_on)`), not a baked LUT (LUTs
smear detail and break parallax). On-screen: half-internal-res march (48-64 adaptive steps, 6 light
steps, dual-lobe HG + powder + altitude gradient) into `cloud_texture` pre-scene; `sky_background_frag`
composites `sky * cloud.a + cloud.rgb`. Temporal via dedicated **ray-direction reprojection** (aux
velocity is scene motion — wrong for clouds); static blue-noise offset headless. Env path:
`sky_env_frag` calls the include at low quality (24 steps) during `sky_bake` so IBL/probes see
clouds; wind-scrolled evolution NOT rebaked into IBL by default. Noise: CPU-generated fixed-seed at
startup (threaded): 128³ RGBA8 Perlin-Worley+Worley octaves, 32³ detail Worley, 128² curl —
**noise.c gains `noise_worley3`** (verified absent). Weather = uniform params v1 (coverage/type/
wind/altitudes), 512² weather-texture hook reserved. Cloud shadows on ground/froxels = deferred
follow-up. Sun radiance at altitude from transmittance LUT; ambient from sky-view LUT samples.
New: `include/clouds.glsl`, `cloud_march_frag.glsl`, `cloud_reproject_frag.glsl`.
CLI: `--clouds`, `--cloud-coverage`. **Owns foundations:** CPU 3D noise generation (Worley +
packing + threaded bake). **Depends on:** B1's `create_texture_3d` (hard).

### B3. Pre-integrated skin shading (Penner 2011) — Effort S — **DONE (spec 11.13), effort was M**
Shipped, opt-in via `Material.curvature_scale` (default 0, so every existing asset and all 16
goldens are byte-identical by construction). The sketch below is preserved, and four of its
load-bearing claims turned out wrong:

- **"extend `include/skin.glsl`" is a build break.** That file is linear-blend *bone* skinning,
  `#include`d by two VERTEX shaders; `dFdx` does not compile in a vertex stage, so following it
  literally breaks the shadow depth pass for every skinned mesh. Landed as a new
  `include/preintegrated_skin.glsl`.
- **"under the existing screen-space SSS" understates the coupling.** Replacing only the att4 tap
  delivers 5% of the intended wrap, because `hdr` still carries Lambert at full strength and the
  new response enters as a signed second difference. The falloff has to replace `NdotL` in BOTH
  `Lo` and the tap.
- **`length(fwidth(N))` measures the normal map, not the mesh.** `N` is post-normal-mapping, and
  pbr_frag already computes that exact quantity as a texture-detail metric for specular AA. Since
  sigma scales linearly in curvature, millimetre normal detail dissolves the terminator entirely.
  Curvature must come from the interpolated geometric normal, and the fwidth ratio itself carries
  up to 40% frame-orientation bias — shipped as an RMS form.
- **"analytic fit, no LUT" cost 9x the achievable error.** The objection to a LUT was texture units,
  of which pbr_frag has none spare; a `const` array in GLSL costs none. Shipped as a 16-row sampled
  table. The polynomial-in-sigma fit could not represent a kink in the coefficient curves, and the
  textbook clamped-wrap form cannot represent the response floor at all (skin facing away from a
  light still returns ~0.012 at sigma 0.3, tending to 1/pi).

Two limits found by measurement. **The cast shadow eats the wrap past the terminator**: on a convex
caster the `NdotL = 0` boundary *is* the self-shadow boundary, and `shadow` multiplies outside the
diffuse, so B3 brightens and reddens the *approach* to the terminator without wrapping light past
it. That one stands as a description of the ANGULAR term — but B3.1 turned out not to be what
recovers it, because the screen-space blur already does (spec 11.19; B3.1 is now **rejected**, see
its section below).

The second — **total scatter is not resolution-independent** — was **fixed by 11.14, and B3 was not
the cause.** The composition rule was blamed for treating the two widths as interchangeable; the
actual fault was the screen-space blur capping its kernel in PIXELS, so the delivered world width
fell as 1/height. Moving that cap into world units was necessary but not sufficient — it banded,
because a resolution-independent world width implies an unbounded PIXEL width no fixed tap budget
can sample — so 11.14 replaced the blur with a scale-space pyramid. `deficit = sqrt(1 - k^2)` was
then measured against Penner's integral and **replaced** by `(1 - k)^2`: the sqrt form assumes
angular and screen-space sigma are interchangeable, and at the pyramid's ceiling it overshoots to
269% of reference.

**Consequence, and it is the honest headline for this row: pre-integrated skin is now inert for
realistic content.** It fires only where the authored radius exceeds the scatter ceiling, under
`--no-sss`, and only on materials that opt in — one fixture today. Fixing the blur removed the
shortfall B3 existed to fill, which is the composition law working correctly, not a regression. The
machinery, the fit tool, the fixture and the gates all remain correct and are what B3.1 and B3.2
build on; but anyone reading this row expecting B3 to be carrying a terminator today should not.

Curvature-aware diffuse falloff under the existing screen-space SSS. **Analytic fit, no LUT** —
pbr_frag units 7/9 go to LTC, and Penner's lookup has well-behaved analytic approximations (~10 ALU
on skin pixels, fully deterministic). Curvature = `length(fwidth(N))/length(fwidth(P)) *
curvature_scale` (silhouette noise smoothed by the downstream SSS blur; artist curvature mask
reserved as future mask_array layer). Wiring: when `sssEnabled && subsurface > 0`, the Lambert term
feeding att4 DiffuseOut is replaced by `skin_diffuse(NdotL_unclamped, curvature, subsurface_color)`;
specular and `subsurfaceTransmission` back-light untouched. Material gains `curvature_scale` via the
4-step scalar recipe.
New: extend `include/skin.glsl`. CLI: `--no-skin-preint`. Test asset: curvature-sweep sphere-row GLB.
**Owns foundations:** none (deliberately). **Depends on:** nothing.

### B3.1. Shadow-penumbra scattering (Penner's second LUT) — Effort M — **REJECTED (11.19)**
The half of Penner 2011 that B3 deliberately left out: a second table pre-integrating the diffusion
profile against the shadow's own penumbra gradient, so light bleeds across a cast-shadow edge
instead of being multiplied to zero at it.

**Not built, because the effect already ships.** `assets/skin_shadow_fixture.cscn` was written to be
the subject — a cast shadow thrown across skin, on the cascade instrument's own geometry — and it
measures the opposite of the premise:

- SSS on vs `--no-sss`: **186,847 px (11.7%)**. Hard black ellipses become soft red bleeds.
- SSS on vs `--no-skin-preint`: **0 px**. The angular term contributes nothing; the screen-space
  blur is delivering all of it.

`shadow` rides *inside* `sssDiffuse`, so the blur that 11.14 repaired scatters across a cast-shadow
edge for free. B3.1 would add an angular term on top, and it would be inert for exactly the reason
row 12 already records B3 as inert — it fires only where the authored radius outruns the scatter
ceiling. A feature whose whole value is a deficit that 11.14 closed.

Reopen only if the pyramid's ceiling becomes a real constraint again, or if a scene needs this under
`--no-sss`. The measurement above is the thing to re-run first.
**Owns foundations:** none. **Depends on:** B3 (shipped), PCSS (shipped).

### B3.2. Skin under an area light — Effort M — **DONE (spec 11.19)**
Skin lit by an area/LTC panel got **no subsurface scattering at all**: the SSS accumulation path
`continue`d past panels, so `sssDiffuse` never saw them. Self-consistent rather than broken — LTC
integrates the whole panel analytically and has no single `L` to feed a diffusion profile — but a
softbox portrait is *the* canonical skin setup, so the gap was exactly where the feature matters
most.

Closed by tapping the panel's diffuse into the skin buffer the way the punctual site taps its
Lambert term. `areaDiff` is already the diffuse in `Lo`, so postfx's `hdr + blur(D) - D` still
cancels. **0 px → 551,295 px (34.5%)** on `skin_area_fixture.cscn`, which reuses the curvature
fixture's geometry and material and swaps only the light. Gated with a directional control arm,
because zero is also what a dead flag would produce.

**Still open, and it is the interesting half:** the pre-integrated wrap stays out of the area path —
it substitutes a widened falloff for `dot(N, L)`, and a rectangle has no single `L`. Smaller hole
than it sounds, since `ff.x` already integrates the clamped cosine over the whole rectangle, so a
wide panel wraps geometrically; what is missing is the widening from transport. Same shape as the
IBL gap, which is also still open — B3 does nothing on an IBL-lit face for the same reason. Both
want a representative direction and a solid-angle-aware width.
**Owns foundations:** none. **Depends on:** A2 LTC (shipped), B3 (shipped).

### B4. TAAU temporal upscaling (Karis-style) — Effort L — **DONE (spec 11.7)**
Shipped as planned below, with three deviations. The buffer split landed as
FOUR sizes, not two: `half_*` (render/2) was separated from `bloom_*`
(post/2), because the old `ssao_*` was a pure alias of `bloom_*` and DoF
sized off it — one name, three meanings, and the likeliest wrong-size bug
in the migration. The froxel fog layer and its history stay at RENDER res
(only the stabilized layer magnifies at the fold), keeping spec 9.5.1's
jitter-cancelling accumulator reading the aux depth 1:1. And `postfx_resize`
was deferred out of 11.7 and landed in spec 11.8, together with a GUI slider
and a real window-resize path, so the scale is runtime-changeable. Measured
42.3 dB at 0.67 against native (bar 32) and 44 -> 22 ms/frame at the native
Retina framebuffer.
Render below display res, reconstruct at display res in the temporal resolve — the perf lever that
funds volumetrics at 4K. New `render_scale` ∈ [0.5, 1.0]; existing integer `ss_scale` remains the
≥1 path. **Separate program (`taau_resolve_frag.glsl`), not a branch** — taa_resolve untouched,
guaranteeing scale==1.0 stays byte-identical. History+output at display res; current frame sampled
at render res with jitter un-applied + Blackman-Harris subpixel weight; Catmull-Rom + YCoCg clamp
carry over (clamp gathered in render-res space, blend scaled by sample-to-pixel distance); Halton
phases 8→16 when scale < 1. **Buffer split**: render res = G-buffer/MSAA, aux, depth, normals,
GTAO/SSGI/SSR, froxel volumes, DoF gather; display ("post") res = TAAU output and everything
downstream (composites, motion blur, bloom pyramid, tonemap — which stops downsampling). Mechanism:
`fx->post_width/post_height` (== width/height when TAAU off, so nothing changes). Scale changes
route through `postfx_resize` + history invalidation (shipped in 11.8); still no per-frame
dynamic resolution.
New: `taau_resolve_frag.glsl`. CLI: `--render-scale 0.67`, `--render-scale-at <frame:scale>`.
**Owns foundations:** the render-res/post-res split — bokeh, lens flare, tonemap inherit it.
**Depends on:** sequence after B1 so the froxel composite is written against the post-res
convention once.

### B5. Bokeh depth of field (Guerrilla-style gather) — Effort M — **DONE (spec 11.6)**
Shipped as planned below, with the tile pass split into tile + dilate (the
motion-blur two-shader idiom) and the near field's "dilated coverage" folded
into the gather's area-normalized weights rather than a separate pass. The
bokeh-chart fixture and the engine's first DoF golden landed with it
(`assets/dof_fixture*`, recipe in the spec).
**Scatter-as-gather N-gon kernel, not circular-separable-complex** — the user wants aperture
*shapes*, which only a sampled kernel gives, and the existing 3-pass scaffold absorbs it with one
pass swapped. (1) keep dof_coc; add 1/8-res max-CoC/min-depth **tile dilate pass** (motion-blur
tilemax precedent); (2) replace dof_blur with a half-res 64-tap Poisson gather warped to an N-gon
(CPU-generated `vec2 kernel[64]` uniform, `dof_blades`/`dof_rotation`; 0 = circle), MRT to near+far
RGBA16F fields with CoC/depth-ordered weights; (3) extended composite: far under sharp scene by
CoC, near alpha-over with dilated coverage. Autofocus untouched.
New: `dof_tile_frag.glsl`, `dof_gather_frag.glsl`; rewritten `dof_composite_frag.glsl`.
CLI: `--dof --dof-blades 6`. Test asset: "bokeh chart" GLB (emissive quads spanning 0.5-50 m).
**Owns foundations:** none. **Depends on:** nothing (adapts to B4's post-res if TAAU lands first).

### B6. Moment-based OIT (Münstermann 2018) — Effort L — **DONE (spec 11.17)**
**Power moments** (not trig — avoids complex arithmetic in GLSL 330): 4 power moments + optical
depth b0, on a second FBO reusing output locations 5/6. Transparents draw **three** times (moments,
then colour weighted by reconstructed transmittance, then the transmissive late pass). Hamburger
4-moment solver in `include/mboit.glsl`; `--no-oit-moments` keeps the WBOIT fallback; off measured
at 0 px against a build of the base commit, WBOIT included. **Both default ON**, which 4.17 had
left as a later call. Best customer: hair card tips (B8) — and the only non-fixture asset the
default flip moves is the raiden rig's hair, 0.12% of frame.

Three sketch estimates were wrong and are worth carrying into B8 and A6:
**memory** is 244 MB at 800x500 on a Retina framebuffer, not ~16 MB — fp32 is worth a third of the
accuracy and the resolve needs its own atlas; **the composite changes**, since moment-weighted
layers carry their own transmittance and sum rather than average; and **there is no seventeenth
sampler in `pbr_frag` for any unit**, free or not, because the driver counts declarations — the
moments ride the refraction sampler through a `#define`, which caps them at four floats and is why
six or eight moments are a re-plan rather than a tweak.

### B7. Lens flare / cinematic finishing — Effort S/M — DONE (spec 11.21)
Quarter-res Chapman-style ghost chain, additive pre-tonemap composite, plus lateral chromatic
aberration on the scene tap. Both default off.

**The sketch overstated the work.** It listed grain, vignette and edge aberration as remaining;
grain, vignette, sharpen and grade had already shipped (`tonemap_frag.glsl`, and
`postfx_apply_film_look` presets four of them). What was genuinely unbuilt was the flare chain and
aberration. Starburst and the dirt mask stay unbuilt — the mask needs an authored texture and a
sampler binding, which is an asset-pipeline question this is not.

**Ghosts read the finished bloom pyramid, not a private bright pass.** Bloom's upsample writes back
onto level 0, so no thresholded buffer survives it; taking one would duplicate the threshold, knee
and firefly clamp, and a second copy of that arithmetic drifts. A mid mip is already thresholded and
blurred, which is what a defocused reflection wants. This does NOT couple the flare to bloom being
on — the pyramid is built whenever any consumer wants one.

**Aberration is denominated in PIXELS at the corner**, falling off as r^2. It first shipped as a raw
UV offset, which put the default at ~450 px and triplicated the frame; the unit was the fix, not the
constant. It sits ahead of the tonemap, not in the finishing block the sketch named — by there the
colour is a scalar and aberration has to resample.

**The fixture's premise was false and measurement caught it.** It declared no lights, so the render
app added its three-point rig and the "black" backdrop measured 0.224; declining the rig took the
opposite-half signal from +3.5% to +125%. A gate calibrated against it would have locked in a 35x
weaker signal and passed. Two dim marks at different radii make the r^2 falloff falsifiable — with
one, a linear ramp passes identically.

**Every gate arm was falsified against a deliberate break before being trusted**, and one arm did
not survive that: "off is off" (`--flare` absent vs strength 0) measured 0 px even with the
composite deliberately broken, because the C side short-circuits and both arms take the same path.
It read as coverage and was not. Replaced by a linearity assertion, which fails at gain 1.000 when
the composite ignores strength.

### B8. Physically based hair (Karis/Marschner) — Effort XL — CLOSED, split (spec 11.20)
The per-texel strand map shipped. The fibre lobes were built, rejected on look at every setting,
and deleted.

**Shipped and live.** `tools/gen_hair_flow.py` derives a strand map from a hair atlas
(structure-tensor orientation, LIC identity) and it rides the **anisotropy** slot that already
existed — so binding a map stretches the ordinary GGX highlight along the painted grain. Encoded as
a **coherence-weighted doubled angle**, the only form that survives the mask array's resample and
mip chain, since a grain direction equals its own reverse. `run_hair_flow_gate` proves the shader
reads it: one quad whose atlas paints strands along the card tangent on one side and across it on
the other splits 1.31x with a map against 1.05x without. Scene files gained a texture vocabulary;
the GUI gained a material editor.

**Why the lobes lost.** Two faults, both structural, in spec 11.20:
1. The branch replaced the whole microfacet term with a bare normalised Kajiya-Kay lobe carrying
   neither `/(4·NdotV·NdotL)` nor the energy compensation — a units mismatch that reads as blown
   white highlights and that no slider fixes. Pairing the scales is necessary and NOT sufficient:
   the lobe still flows through `NdotL` on the CARD normal, a surface-layering stack, and a
   half-vector diffuse, so the unit of substitution is wrong rather than just its scale.
2. Cards carry no normal map, so per-texel *facing* is absent from the asset however good the
   direction map is; the atlas already has hair shading painted in for the lobes to compete with;
   and white hair makes the uncoloured and tinted lobes read identically, hiding the payoff.

**The cheapest lesson, recorded so it is not repeated.** `set_material_anisotropy_tex` had zero
callers — the engine already had an energy-paired per-texel direction channel, taking the same
`.rg`-plus-third-channel layout as KHR_materials_anisotropy — and the first version of this work
built a second one beside it. Nobody looked before building.

Do not re-open the fibre lobes on raiden. What survives is a general anisotropic-specular feature
that hair motivated.

### B9. Aerial perspective (Hillaire 2016) — Effort S
The distance haze that sells outdoor scale: geometry fading into the atmosphere's own colour rather
than a flat fog tint. A 32×32×32 volume marches Rayleigh/Mie via the atmosphere include against the
existing transmittance + multiscatter LUTs, rebuilt every frame (negligible at this size), applied to
scene pixels only as one extra tap in B1's composite. Also switches fog sun/ambient from app-set to
**sky-published** (app override retained) — deferred out of B1 so that item keeps a clean A/B against
the old screen-space fog pass. Split from B1 because it is a different subsystem (sky/atmosphere, not
postfx lighting), carries none of the fog item's risk (purely additive, no legacy pass to retire, no
temporal reprojection), and is S once the 3D machinery exists.
New: `aerial_lut_frag.glsl`. CLI: `--no-aerial`, `--world-scale <units-per-km>`.
**Owns foundations:** `bake_lut_3d` (sky-side layered volume draw) and the units→km world scale.
**Depends on:** B1's `create_texture_3d_float` and `include/froxel.glsl` (hard).

*As built (9.6): B1's `draw_volume_slices` turned out NOT to be reusable — it hardcodes the fog
FBO, the fog volume's dimensions and the `sliceIndex` uniform, and reports failure by clearing
`fog_enabled`. `create_texture_3d_float` and the froxel depth mapping were reused verbatim. Nor was
it "one extra tap": the composite's single blend carries one (inscatter, transmittance) pair and the
fog pass early-returned with fog off, so the two media are combined analytically inside one
composite and `postfx_run_fog` became `postfx_run_atmosphere`.*

## The four walls (Tier 4 preamble)

Tiers 1-3 shared a shape: each item was another gated, lazily-allocated fullscreen pass, and the
engine absorbed a dozen of them without structural change. That is over. Four walls now stand
between the current renderer and the next tier, and naming them is more useful than any single
feature below, because each one decides which features are reachable *at all*.

*Wall 4 was added after 11.29 and it is the one this section got wrong. There were three here for
the whole of Tiers 1-3, and the reason a fourth appeared is worth reading before the list: Wall 2
named the geometry problem as **submission** — draws, objects, triangles — and every remedy under it
reduces submission. That framing survived unchallenged for the length of the roadmap because no
scene in the corpus had overdraw. The first one that did inverted it in a single profiler run.*

**Wall 1 — `pbr_frag` is sampler-saturated (16/16).** Detailed in the ledger section above. It blocks,
today, every feature whose data has to reach the *forward shading* stage as a texture: decals, light
cookies, IES textures, detail/wetness maps, a cloud shadow map, and sampling the froxel volume from
the transparent pass. It does NOT block anything that lives in postfx (which has its own budget and
is nowhere near full), anything that fits in a UBO, or anything small enough to be a `const` table.
Note which Tier 4 items below are postfx-only or UBO-only: those are the cheap ones, and they are
cheap *because* of this wall, not in spite of it.

**Wall 2 — geometry submission does not scale — MOSTLY REMOVED (specs 11.28 / 11.29).** Instancing,
LOD chains and per-cascade shadow culling shipped in E5; on `abandoned_window_shadowed` that is shadow
CPU **−83%**, frame **−38%**, 2,148 draws → 272. What remains of the wall is the third limb, draw
**ordering**, which E5 deferred as unfalsifiable against the corpus it had — see E6 below, where it is
no longer unfalsifiable.

*The original wall text read: "One draw per mesh … no LOD, no occlusion culling, and no instancing
anywhere outside `particle_renderer.c`. The renderer is AAA-caliber per pixel and early-2000s per
object." Two of those three are now false. The occlusion clause was always true and is now Wall 4's
business rather than this one's.*

**Wall 4 — the opaque pass is unshielded.** There is no depth prepass anywhere in the engine, no
occlusion culling of any kind, and opaque draws are issued in draw-list order — a pre-order DFS
flatten, Morton-ordered within a prototype for batch contiguity, which is deliberately
*camera-independent*. A forward renderer with no prepass depends entirely on hardware early-Z to
avoid shading hidden fragments, and early-Z rejects nothing unless nearer geometry wrote depth first.
So every layer of a canopy shades in full, at whatever the uber-shader costs, into five MRT RGBA16F
targets at 4x MSAA.

`apps/forest` measures it, and the numbers are not subtle. At the default 1600x900 on an M1 (a
3200x1800 Retina framebuffer): **opaque 312 ms of a 346 ms timed frame** — 90% of it — against shadow
cascades at 23 ms. The shadow pass pushes **5x the triangles** (130M vs 26M) for **1/14th the time**,
because it is depth-only.

Three probes, and the conditions differ so they are worth stating rather than pooling:

| probe | condition | result |
|---|---|---|
| 3.2x fewer triangles (`--lod-bias 0.2`) | 800x450 fb, MSAA off — fill minimised | 42.6 → 38.6 ms, **9%** |
| 12x more draws (`--no-instancing` 2,617 vs `--no-lod` 219) | same | 44.2 vs 65.6 ms — more draws is **faster** |
| MSAA 4 → 1 | 3200x1800 fb | 312 → 220 ms, **−92 ms** |

Neither triangle count nor draw count is the limit, and MSAA is only 30% of it. Fragments are. (The
draw-count row is not perfectly single-variable — the two configs also differ 25.9M vs 28.7M triangles
— but it fails in the direction that matters: the config with 12x fewer draws is the slower one.)

Two properties of the content make it the worst case rather than merely a bad one, and both are
ordinary authoring choices: the foliage material is `doubleSided` (backface culling off, so every leaf
card rasterizes twice) and `ALPHA_MASK` (a shader that can `discard` cannot have its depth resolved
before it runs, so those draws forfeit early-Z outright).

**Measured, and it is not what the paragraphs above assumed.** 11.30's Phase 0 added the instrument
this section was written without — `GL_SAMPLES_PASSED` around the opaque pass, against the frame's
sample budget — and the reading reorders the story:

| scene | opaque GPU | depth complexity | samples/sec |
|---|---|---|---|
| `apps/forest` | 312 ms | **1.85** | 137M |
| `abandoned_window_shadowed` | 35 ms | **2.78** | 602M |

So the interior has *more* redundant shading than the forest, and the forest shades **4.4x slower per
sample**. Forest's 312 ms is therefore mostly per-sample shader cost, not overdraw — which means a
prepass cannot be worth the 250 ms this entry's last line claims, and that line is now an upper bound
nothing has demonstrated.

Two caveats keep this from settling the question in the other direction. The counter counts samples
that PASS, and **a fragment that `discard`s costs a full shader invocation while passing nothing** —
so it understates the work exactly where alpha-masked foliage dominates, which is forest. It is a
floor on depth complexity, not an estimate: above 1 proves redundant shading, near 1 does not prove
its absence. And 1.85 is a whole-frame average including sky, where no opaque sample passes at all;
within the covered region it is materially higher.

**And that caveat immediately proved load-bearing: the real figure is 3.87.** Forest was running 4x
MSAA with no temporal filter, so a leaf at alpha 0.5 wrote two samples of four and the counter saw
half the fragments that actually shaded. Giving the app TAA instead of MSAA — what the render app has
always done, and what forest was missing — drops the sample budget 4x and the reading rises **1.85 →
3.87**, at the same time as costing 29% less (opaque 551 → 393 ms, frame 578 → 427).

So the withdrawal above is itself partly withdrawn: the prize is roughly twice what Phase 0 recorded,
though still not the whole 312 ms. **Every number in this section was taken on the MSAA path and
understates the true depth complexity by about half.** The lesson worth keeping is not any of the
figures but that three consecutive readings of this scene — 1.85, 2.78, 3.87 — each looked
authoritative and each was an artefact of the configuration it was taken in.

The honest position is that the *prize* is now unmeasured rather than large, and 11.30's Phase 4
exists to measure it. What has not moved is the diagnosis: neither triangles nor draws nor MSAA is
the limit, and the opaque pass shades every layer it is handed because nothing rejects them first.

**This wall was invisible for the length of Tiers 1-3 because nothing measured it**, and the word
"overdraw" appears nowhere in this document or in any spec before this entry. `apps/forest` was built
to exercise E5 and its spec measures triangles, draws and material switches throughout — every one a
submission metric — while carrying `opaque GPU 435 ms` in its own results table, read as a baseline
for comparing LOD settings rather than as one pass taking 435 milliseconds. The timing instrument was
right, present, and pointed at the answer; the model decided what the number meant.

*An earlier draft of this paragraph explained the blindness by asserting the corpus had "no depth
complexity" — props, a character and an interior, where submission genuinely is the bottleneck.
**Measurement withdrew that**: the interior reads 2.78 against forest's 1.85, the highest in anything
tried. The corpus was never short of depth complexity; it was short of an instrument, and the
explanation was a second guess offered in the same breath as admitting the first one had gone
unchecked.*

**Wall 3 — the GPU is unmeasured — REMOVED (spec 11.27).** Per-pass `GL_TIME_ELAPSED` scopes, a
ring of N-frame-deep results, a HUD table and submission counters. It was the cheapest wall to remove
and it gated the honesty of everything in Track E's tail — which it then delivered twice over: E4's
own first draft drew a conclusion the instrument reversed, and Wall 4 above exists because the
instrument said "opaque 312 ms" in a single run.

*Original text: "There is not one `glGenQueries` / `GL_TIME_ELAPSED` / `glQueryCounter` in `cetra/src`
outside the vendored deps, and the HUD shows FPS and nothing else (`gui.c:871`). Every perf number in
every spec in this tree — 44 -> 22 ms at 0.67 scale, the 73.6 ms cloud re-bake — is wall-clock frame
time measured around the whole frame."*

## Track C — Lighting completeness

What the light path still cannot express. Every item here is UBO-only or postfx-only, so **Track C
does not touch Wall 1** — which is exactly why it is the track to start with.

### C1. Translucent shadow maps — Effort M → **L, DONE (spec 11.26)**
Shipped as **deep opacity maps** (transmittance storage), not the four-moment
reconstruction the sketch below assumed. Opt-in behind `--translucent-shadows`; hair casts a
partial shadow, glass stops casting a black one.

**The sampler wall turned out not to reach this**, which is the finding worth carrying. Unit 10
already binds one of two textures and a branch picks, so there are two homes for transmittance
that never coexist; this took the depth-array home, which keeps PCF/PCSS intact and leaves the
moment path alone. `DEPTH_COMPONENT24` is a 24-bit UNORM — better storage for a value in [0,1]
than the moment array's fp16 — and the array's white `CLAMP_TO_BORDER` already reads as
"nothing occludes here".

**Storing `exp(-b0)` rather than the absorbance is what makes the filter legal.** Transmittance
averages linearly and absorbance does not, so a 3x3 box over these layers is exact where
averaging moments carries Jensen's bias. The same arithmetic is why opaque casters stay out of
the map entirely: the error goes as `A²/8` and opaque is the maximum-absorbance case.

Measured against the analytic answer on a new fixture: bands read 0.650 / 0.421 / 0.273 / 0.179
against 0.65^k, worst error **0.0016**; the mask ramp matches `1 - alpha(x)` at **RMS 0.0029**,
where a binary alpha test scores 109x worse. Raiden moves **331,967 px (4.0%)** and the diff is
the groom and the shoulder under it, nothing else — that last figure predates the review pass
and has not been re-measured since.

**Three bugs found by the arms rather than by reading, all three presenting as something they
were not**: an unrestored blend FUNCTION that desaturated the whole frame while the shadows
looked right; a resolve that drew a 4-vertex triangle STRIP as `GL_TRIANGLES, 0, 3` and so
covered half the map, leaving a diagonal edge and values that were exact wherever they appeared;
and two instrument faults (a vignetted lit reference, a GTAO-darkened black point) that each read
as a renderer error.

**Deferred, with reasons:** the four-moment reconstruction (it pairs MSM's linear warp with
MBOIT's, a configuration in neither paper, and 11.17 measured it ill-conditioned exactly where
hair lives); hair *receiving* its own shadow (measured: 195,821 px of card-shaped streaks); the
punctual path (unit 15 has no moment branch and the layers would collapse every punctual map to
its 1024 floor); coloured transmittance (breaks the scalar contract at three sites).

<details><summary>The original sketch, kept for the record — it priced this as M and assumed the moment path</summary>

**Moment transmittance shadow maps — Effort M** *(archived pitch, not a live item: this is what
C1 looked like before it shipped. The four-moment reconstruction it argues for is deferred, with
the measured reason recorded above.)*
**The standout item, and it starts from a defect rather than a feature.** `shadow.c:485-488` excludes
alpha-masked materials from the shadow map entirely unless they opt back in via `foliage_shadows` —
so **hair casts no shadow at all**, by a deliberate choice whose reasoning (strand-scale acne vs
card-shaped streaks at map-texel scale) is sound and whose result is a character standing in light
with nothing under their hair. Alpha-blend materials are not filtered at all, so glass casts a solid
black shadow. Both are the same missing concept: the shadow map stores a binary depth, and these
surfaces need a *transmittance function* of depth.

The engine has now built the machinery for exactly that twice — `include/msm.glsl` (A6/11.22, four
power moments over a depth distribution) and `include/mboit.glsl` (B6/11.17, four power moments over
an absorbance distribution). A transmittance shadow map is the second one pointed at the light's
frustum instead of the camera's. Reuses A6's resolve-from-the-finished-array structure, which is what
made `--no-msm` a provable 0 px; the same structure should make `--no-translucent-shadows` provable
here.
**Carry forward from 11.22 before starting:** four moments run out on a thin caster (blur 1.0 leaks
0.4159), and hair is thin casters — so the honest prior is that this reconstructs a *soft* hair
shadow well and a crisp one badly. Also carry 11.22's gate lesson: an arm measured deep inside an
umbra discriminates nothing, because every reconstruction agrees there.
**Refs.** Münstermann et al., *Moment-Based Order-Independent Transparency* (I3D 2018); Jansen &
Bavoil, *Fourier Opacity Mapping* (I3D 2010); Yuksel & Keyser, *Deep Opacity Maps* (EG 2008).
**Owns foundations:** transmittance-vs-depth storage in the shadow path.
**Depends on:** A6 (shipped), B6 (shipped). **Wall 1:** unaffected (replaces a sampler, as A6 did).

</details>

### C2. Emissive geometry → LTC area lights — Effort S/M
An emissive mesh lights nothing today. It is bright in the frame, it feeds bloom, and its only path
into the lighting solution is a 16²-face DDGI capture that converges over seconds and resolves no
detail. Practicals, screens, strip lights and neon — the entire vocabulary of interior lighting — are
therefore decorative.

`Light` already carries exactly the fit's output: `position`, `direction` (panel normal), `up`,
`size`, and a photometric `intensity` in nits (`light.h:45-73`). So the item is a fit — plane-fit an
emissive mesh's dominant quad, integrate its emissive to a radiance — plus a registration into the
packed light array A1 already uploads. **No new shading code, no new machinery, no new units.**
v1 limits worth writing down now: one rectangle per emissive mesh (a curved neon tube is out), static
fit at load (an animated emitter re-fits per frame or is excluded), and the same single-sided
constraint A2 shipped with.
**Refs.** Heitz, Dupuy, Hill & Neubelt, *Real-Time Polygonal-Light Shading with Linearly Transformed
Cosines* (SIGGRAPH 2016) — already shipped as A2; this item is a producer for it.
**Depends on:** A1 (shipped), A2 (shipped). **Wall 1:** unaffected.

### C3. IES photometric profiles — Effort S
9.9/10.0 made punctual lights genuinely photometric — candela and lux imported as authored, an EV100
camera — and then every one of those lights radiates through a bare analytic cone. IES profiles are
the payoff for work already done, and they are what makes an architectural interior read as lit
rather than as shaded.

**This one fits Wall 1 rather than fighting it.** Most real IES profiles are near rotationally
symmetric about the luminaire axis, so a 32- or 64-tap intensity-vs-angle table per *profile* (not
per light) lives in the existing std140 light UBO alongside the packed lights, costing **zero texture
units**. That is the same escape 11.13 took for the skin table, and it generalises: the ledger
constrains *texture* lookups only. Asymmetric profiles (wall-washers) are the v2 case and do need a
2D table; defer them rather than paying a unit for the symmetric 90%.
**Refs.** Karis, *Real Shading in Unreal Engine 4* (SIGGRAPH 2013 course) — the IES section;
IESNA LM-63 for the file format.
**Depends on:** A1's UBO machinery (shipped), 10.0's photometric units (shipped). **Wall 1:** avoided
by construction.

### C4. Clustered specular probes — Effort L
`scene->probe` is singular (`scene.h:127`) — **one** parallax-corrected reflection probe for an entire
scene. SSR covers what is on screen; everything it misses falls back to that one probe plus the IBL
cube, so a character walking from a lit hall into a side room keeps the hall's reflections. A4 gave
diffuse GI a spatial structure and specular never got one.

The two pieces this needs are both already built and both already own the foundation: **A1's cluster
grid** for probe→cluster assignment (the same CPU pass that culls lights culls probe AABBs) and
**A4's octahedral atlas + `include/octahedral.glsl`** for storage, so N probes cost one sampler, not
N. The unresolved design question is the capture budget: A4's answer — converge, then idle at
literally zero — is available here too and is probably right, but a probe re-capture is a full
6-face scene render at a useful resolution, not A4's 16².
**Refs.** Lagarde & Zanuttini, *Local Image-Based Lighting with Parallax-Corrected Cubemaps*
(SIGGRAPH 2012 talk) — shipped as the single-probe path; McGuire et al., *Real-Time Global
Illumination using Precomputed Light Field Probes* (I3D 2017) for the atlas/visibility structure.
**Depends on:** A1 (shipped), A4 (shipped). **Wall 1:** unaffected (one atlas sampler, reusing A4's).

### C5. Screen-space shadows for local lights — Effort S
A3 marches contact shadows along **one** light — the key. Every other light in the scene lands on
CSM/punctual maps whose texel footprint loses the millimetre-scale contact, which is the specific
artifact A3 exists to fix. Extending the march to the N nearest cluster lights per pixel is the
cheapest remaining grounding win, and it is postfx-only: A3 already publishes the key light's
view-space direction, and A1's cluster grid already answers "which lights touch this pixel".
Cost scales linearly in N, so v1 is N=2 with a per-light distance cap.
**Refs.** the Uncharted 4 contact-shadow technique already cited by A3.
**Depends on:** A1 (shipped), A3 (shipped). **Wall 1:** unaffected (postfx).

## Track D — Surfaces & environment

Where Wall 1 actually bites. D1 and D2's ground-shadow half both need a texture inside `pbr_frag`,
so **D0 is a hard prerequisite for them** and should not be started until one of them is scheduled
(the roadmap's own just-in-time rule: foundations land with their first consumer).

### D0. Free two `pbr_frag` sampler units — Effort M
Not a feature; the unblocking item. The concrete candidate: **unit 11 (IBL irradiance) folds into the
GI atlas on unit 14** as a single octahedral tile. `pbr_frag` already samples that atlas, the
octahedral encode/decode include already exists from A4, and a cosine-convolved environment is
precisely what one atlas tile holds — so the fold costs no new sampler and frees a whole unit. Second
candidate, riskier: share unit 6 (`sceneColorTex`) the way 11.17 shared it for moments, valid only
where the two consumers are provably mutually exclusive.
**The 0-diff gate here is unusually strong and should be demanded**: folding irradiance into an atlas
tile is a pure storage change, so the raiden baseline must be 0 px, and if it is not, the
octahedral resampling is lossy in a way that matters and the item should stop.
**Depends on:** A4 (shipped). **Owns foundations:** the freed units D1/D2 spend.
**Demanded by measurement since 11.39**, not just by the dependency graph: D2 shipped its froxel
half and measured the ground at RMSE 0.0013 against the air's 0.0353, so the dappled light on
terrain that D2 was written to deliver is entirely on the far side of this item. It is now the only
thing standing between the engine and that look, which makes it the highest-leverage entry in D.

### D1. Clustered decals — Effort L
The largest **environment-art** gap in the engine: there is no way to author localised surface detail
onto geometry — no scorch marks, no leaks, no edge wear, no posters, no puddle edges. Forward
rendering rules out the deferred screen-space decal every AAA engine uses, but the clustered form
fits A1 exactly: box decals culled into the same 16x8x24 grid, sampled in `pbr_frag` before the
lighting loop, modifying albedo / normal / roughness in place.
**Hard-blocked on D0** — a decal atlas is a texture in `pbr_frag`, which is the one thing the ledger
has none of. Second cost worth pricing before committing: decals are the first feature that makes the
forward shader's cost data-dependent per pixel in a way clustering cannot bound tightly.
**Refs.** Persson, *Practical Clustered Shading* (SIGGRAPH 2013 course); Wronski,
*Screen-Space Decals* (GDC 2014).
**Depends on:** A1 (shipped), **D0 (hard)**.

### D2. Local fog volumes + cloud shadows — SHIPPED as spec 11.39
Both halves landed. Local fog volumes are a Scene-owned world-space AABB with density, an inward
feather and a σ-weighted tint, authored as a top-level `fogVolumes[]` block and arming the froxel
pass by joining the union at the gate rather than latching `fog_enabled`. Cloud shadows are a
256² R16F sun-transmittance map built by the cloud march from the march's own wind offset, read by
shearing each froxel up to the shell — exact for a horizontal layer, and tiled by `GL_REPEAT`
because the density field is periodic over the shape noise's own 8 km.

**The verdict this item was told to produce: the surface half IS still worth a unit, and the
measurement says so more sharply than the entry below expected.**

| band, aerial fixture with `--clouds --fog` | RMSE on/off |
|---|---|
| sky and air | **0.0353** |
| ground | **0.0013** |

The froxel half shadows *in-scattered light*, so it lands where the sight line crosses the most air
and puts essentially nothing underfoot. The entry below called the froxel half "where the visible
payoff is" — that is right about payoff per line and **wrong about which payoff**. What ships is
weather in the haze; the moving dappled light on terrain, which is the thing the entry actually
promised, is the `pbr_frag` half and is still entirely unbuilt. So D0 is not merely unblocked-by
here, it is the gating dependency for the feature D2 was written to deliver.

Two things measured along the way that contradict the plan and are recorded in the spec: the
predicted wind ghost in the froxel accumulator **does not occur** at any scene scale (the fog
volume's depth, not the shadow tile, sets the ratio), so the history clamp was not built; and the
froxel accumulator's first active frame is not stable run-to-run when the pass is armed by a
volume rather than by `fog_enabled`, which is unexplained and filed.

Original entry follows.

Two gaps in the shipped atmosphere, one item because they share the froxel injection point.
**Local fog volumes**: B1's froxel volume carries one global medium, so a smoky room, a dust shaft or
a mist pocket cannot be authored — only the whole world's fog can change. Per-object density boxes
injected into `froxel_inject_frag` is the standard answer and that shader is already the roadmap's
declared integration point. **Cloud shadows**: B2 shipped clouds and explicitly deferred their
shadows, so the sky has weather and the ground does not know. This is the highest look-per-line item
left in the atmosphere stack — the noise and the march already exist, and a low-res cloud shadow map
sampled in `froxel_inject` (postfx, no wall) buys the moving dappled light that sells an outdoor
scene. Sampling it in `pbr_frag` for direct sun occlusion is the half that needs D0; **the froxel half
does not, and is where the visible payoff is** — do it first and measure whether the surface half is
still worth a unit.
**Refs.** Hillaire, *Physically Based and Unified Volumetric Rendering in Frostbite* (SIGGRAPH 2015
course); Schneider & Vos, *The Real-Time Volumetric Cloudscapes of Horizon Zero Dawn* (SIGGRAPH 2015)
— already shipped as B2.
**Depends on:** B1 (shipped), B2 (shipped); surface half on D0.

### D3. Tessellated water — SHIPPED as specs 11.32 + 11.33 + 11.35, and NOT tessellated
The flagship surface landed: Gerstner and Tessendorf FFT wave models, Beer-Lambert absorption,
caustics and foam from the surface Jacobian, shoaling against a bed provider, an underwater medium,
and a `.cscn` block. It exercises the four subsystems this entry named it for.

**The tessellation rationale did not survive, and that is the entry's lesson.** This item was framed
around unspent GL 4.0 tessellation headroom, and water never needed it. The patch-density problem
tessellation solves is a *screen-space sampling* problem, and it has had screen-space answers since
2004 and 2005 — neither of which needs a pipeline stage.

**The mesh went through both of them, in that order, and the second one is what ships.** 11.33 phase 1
replaced 11.32's uniform world grid with a geometry clipmap: five instanced rings snapped to the
coarsest level's cell, which — against 11.32's own deferral, on arithmetic nobody had done — tile with
no gap, no overlap and no coplanar tie, because each level's cell divides the coarsest exactly. That
worked and nothing recorded about it was false. **What it could not do was reach the horizon**, and the
reason is the same snap: reach and near-field detail are welded to one number, so pushing the extent
out coarsened the finest cell until the swell disappeared. The surface stopped 5° short while a comment
claimed otherwise. 11.35 replaced it with a **projected grid** (Johanson 2004): a fixed lattice in NDC,
one draw, each vertex a ray onto the still plane. Density exact rather than in 2× steps, no
T-junctions, reach decoupled from detail — and the same triangle budget the rings spent.

**The far field then became a filtering problem rather than a mesh-reach one**, which is the part
neither mesh scheme addresses: distant cells cover more than a wave period, so each model drops what
sits under its footprint and hands the removed slope energy to roughness. That is a BRDF answer to a
geometry question, and it is where the remaining detail goes once no mesh can carry it.

**So the tessellation stage is still entirely unspent** — POM silhouettes and D4 terrain remain its
candidate first consumers, and neither inherits a pipeline from here.

**Open items this entry still owns.** Recorded here rather than only in 11.35's history, because a
closed spec is not where anyone looks for work that is still outstanding.

1. **The projector aims at the still plane, not the displaceable slab.** Each lattice ray is
   intersected with the flat plane — which is where the wave field is sampled — so the surface then
   displaces and a wave can lift geometry into view from XZ the lattice never sampled. The nearest
   point a ray can reach is `clearance / tan(top-of-frame angle)`. It bites when the eye is within
   about one wave height of the surface: wading in `apps/tree --player`, a boat deck, a camera at sea
   level. It is **why `water-submerged` sits on a shallow Gerstner framing** rather than the FFT one it
   was written for. The fix is to offset the plane toward the eye by the max vertical displacement,
   which costs zero lattice rows; the blocker is that nothing publishes a displacement bound. Gerstner
   is analytic (`amplitude × Σ 0.44^i`, i<4, = **1.7188**). Spectral needs either the seeded
   realisation's variance (exact, but the inverse FFT's normalisation has to be traced) or a
   fetch-limited JONSWAP estimate (**≈1.2 units**, padded and labelled as padding — *not* a
   Pierson-Moskowitz number: the seeding is JONSWAP+TMA and fetch-limited, so PM is the wrong spectrum
   and is blind to `WATER_FETCH` and `WATER_SEA_DEPTH`). Note the single-plane offset **reduces rather
   than removes** the artifact when the eye is *inside* the slab, because the horizon-row clamp still
   cannot reach a crest projecting above the flat-plane horizon; Johanson's own two-plane construction
   is the complete answer.
2. **Water positions are absolute, so precision tracks distance from the world origin, not the
   camera.** Measured: the same sea at 960 vs 307,200 units out differs by **227,458 px of 480,000**
   (RMSE 0.0146) even though the spectral field is exactly periodic across the 960-unit cascade LCM, so
   those two frames should be identical. Attributed with a flat-surface control — no waves, same
   offset, PAE 4/255 against 77/255 with them — so it is the field lookup, not the view transform.
   `p = camPos.xz + rd.xz * t` quantises to ~0.03 units at 3e5, which is 6% of the medium band's
   texel. **No consumer today**: nothing in this tree exceeds ~1,000 units. And the correct fix is
   engine-wide rather than water's — camera-relative rendering for everything, since water alone would
   be precise standing beside jittering terrain. Full recipe in 11.35's phase 3 as-built.
3. **The water gate corpus has a structural blind spot, and it has already cost one shipped defect.**
   11.35's review round found the shoreline test reading a *cleared* depth buffer as bed geometry and
   discarding every fragment past the last mesh — 40.6% of an `apps/tree` frame — while all 22 water
   arms and all 23 goldens stayed green. It could not be caught: every water arm reads
   `water_fixture`, and the defect's magnitude scales with (horizon distance / far plane), so on a
   14-unit scene the affected strip is sub-degree and the fix moved that golden by 3,207 px of 480,000.
   `water-horizon` is a reach-*invariance* arm, so the bug shifted both its frames equally;
   `water-farfield`'s box is deliberately framed below the top edge so it cannot eat sky. Meanwhile
   **`apps/forest --water` and `apps/tree` inherit the whole surface stack with zero pixel coverage
   between them** — and tree is where the bug was found, by eye. A scene whose far plane is small
   relative to the horizon is the instrument this corpus lacks.

4. **A camera AT the waterline is unhandled, and it is the framing `--player` put in front of people.**
   Wading in `apps/tree` produces a clean horizontal boundary between above-water and below-water
   shading. It should be a wavy, irregular line — the surface crossing the eye plane — and a clean
   sweep means the split is decided by the flat still level rather than by where the waves are.
   **Confirmed by reading, independent of that symptom:** the same question is answered twice with
   different scopes. The normal flip (`water_frag.glsl:248`) is per-PIXEL,
   `cameraSubmerged || !gl_FrontFacing`; the optical-path branch (`:265`) is per-FRAME,
   `cameraSubmerged` alone, and `cameraSubmerged` itself is one bool per frame from
   `cam_world[1] < water->level` (`water.c:945`) against the STILL level. So a fragment whose normal
   was flipped toward the eye — a crest closing over a camera that is still above the still level —
   is then charged its path against the depth buffer BEHIND the surface, which is air there.
   Unifying those two into one `seenFromBelow` is a small change, verified 0 px on `water_fixture`,
   and **the fixture cannot see it at all**: at amplitude 0.06 no crest ever shows its underside, so
   `!gl_FrontFacing` is never true. Another instance of item 3.
   The geometry half is separate and larger: the projector picks its side per frame too
   (`above = waterCamPos.y >= waterLevel`), so the lattice covers one side of the eye plane when a
   straddling camera needs both. Item 1's plane offset does not fix that — it moves the plane, where
   this needs coverage on both sides of it. **Rank above item 1**: it fires whenever a walker enters
   the water, where item 1 needs a specific camera regime.

5. **The water shader does not know how big a world unit is, and 11.36 made that visible.** Clear
   water in `apps/tree` promotes four more world-unit constants in `water_frag.glsl` from harmless to
   conspicuous: `WATER_MAX_BEND` (1.0, whose comment says "a metre or so" — 4.5 cm at 22 units/m, so
   refraction distortion of the now-visible bed is imperceptible), the caustic depth window
   (`0.35 / 9 / 20`, a 1.6 cm–0.9 m ring hugging the shore, and tree is **FFT by default** so
   caustics are live there — expect a bright band with clear bed beyond it), `WATER_SHORT_NEAR/FAR`
   (42/118, whose comment says "Metres:" outright), and `OCEAN_SHOAL_MIN/FULL`. The engine already
   owns the number — `Sky.world_units_per_km`, default 1000 — and **`apps/tree` never initialises
   it**, so its aerial perspective is wrong by the same factor with the opposite sign: 22× too
   *thick*, where the water was 22× too absorbing. Plumbing one float into the water program scales
   all five at once. 11.36 introduced `GROUND_UNITS_PER_METRE`, which is that number, so the app half
   already exists.

**Queued:** `specs/11.36-water-clarity-at-world-scale.md` — the absorption is authored per metre and
stored per world unit, so `apps/tree` (22 units/m) is 4.9–8.6× too absorbing per channel, and
`WATER_MAX_PATH` is a world-unit clamp justified in its own comment as an optical-depth budget. Fixing
either half alone makes the frame worse. This also closes 11.35 phase 6's own open question, which
named the water's optical properties as the blocker on a visible seabed.

**Refs.** Tessendorf, *Simulating Ocean Water* (SIGGRAPH 2001 course); Johanson, *Real-time Water
Rendering: Introducing the Projected Grid Concept* (2004) for the mesh that ships; Asirvatham & Hoppe
(GPU Gems 2) for the clipmap it replaced, whose real home is D4 below.
**Owns foundations:** none of the ones this entry predicted. What it does own is the water subsystem
itself, its bed-provider seam (`WaterHeightFn`, which `apps/forest`'s terrain satisfies directly),
and the CPU wave query buoyancy would consume.

### D4. Terrain — Effort XL
No terrain system exists. Real gap, but only for outdoor scale, and it depends on Wall 2 far more
than on any rendering technique — a clipmap without instancing, LOD and a streaming story is a
mega-mesh with extra steps, which `apps/tree`'s grass already demonstrates the cost of. **Do not
schedule this before Track E's E5** — now satisfied.

**`apps/forest` (11.29) is not this item and must not be read as it.** It is a *consumer* of E5:
fixed tiles, per-tile LOD chains, everything resident, capped at 1 km² by exactly the streaming story
this item owns. It is the "mega-mesh with extra steps" the paragraph above warns about, chosen
knowingly because at that scale the warning does not bite. What it does contribute is a fixture — the
first content in the tree where instancing, LOD and culling all matter at once, and the scene that
found Wall 4.

**This item inherits a working geometry clipmap, and git is where it is kept.** D3 built one for water
and 11.35 removed it — the snap arithmetic and the T-junction stitch are at commit `8d04658`, which is
D4's reference implementation rather than dead code kept live for a hypothetical consumer. **The part
water never used is the part terrain needs**: rings as windows into a mip pyramid of streamed height
data, which is what Asirvatham & Hoppe invented them for and why that paper is this entry's first
reference. Nothing was pre-shaped into a "shared" primitive, because this entry has not chosen between
Hoppe's rings and Strugar's CDLOD, and extracting one would be guessing at that choice.

**And the stitch is a better answer to E5's open T-junction gap than the one recorded there.**
`meshopt_SimplifyLockBorder` fixes cracks by never simplifying a border, which keeps full border
density forever. The water stitch let both sides simplify and made the FINER side agree instead:
evaluate the two even neighbours on the shared edge and average, which is by definition the straight
line the coarser edge draws there — exact, and paid for on one vertex row per patch. It needs a surface
evaluable at arbitrary points, which `terrain_height_at` is by construction (`procedural/terrain.h`, "a
pure function of (params, x, z)"), so terrain can use it where a general mesh could not.
**Refs.** Asirvatham & Hoppe, *Terrain Rendering Using GPU-Based Geometry Clipmaps* (GPU Gems 2);
Strugar, *Continuous Distance-Dependent LOD* (CDLOD, 2009).
**Depends on:** E5 (hard, in practice), D3's tessellation path (soft).
**Inherits:** D3's clipmap implementation at `8d04658`, including the T-junction stitch.

## Track E — Image finishing & the perf floor

### E1. Output dither / debanding — Effort S — **DONE (spec 11.24)**
Shipped on by default, `--no-dither` a provable 0 px. The sky's longest run of one 8-bit value
down a column goes **192 px → 22 px**; 0.5 LSB only reaches 134, which is why the default is the
textbook ±1 and not less. Raiden moves 68.6% of pixels at **PAE exactly 1/255**.

**The sketch's one technical claim was wrong and arithmetic caught it before any shader was
written.** `ign(p) - ign(p + c)` is not a TPDF: `ign` is a sawtooth on a linear ramp, so a
constant offset shifts only its phase and the difference collapses to **four distinct values** —
a staircase. Decorrelation has to come from changing the *scale*, not the offset. Evaluating the
pure function in Python cost minutes and saved shipping a patterned dither.

Two things the plan got right for the right reason. The pattern is **static**: an animated one
would have put ~half the frame 1 LSB apart between consecutive frames, landing in every churn
measurement *including its floor arm*, so both sides inflate and the comparison stops
discriminating — measured 79,593 px vs 79,176, a 0.5% difference. And `include/noise.glsl`
already had `ign()` with a comment instructing new code to use it, so B8's lesson was applied in
advance for once.
**Refs.** Mikkelsen, *Banding in Games: A Noisy Rant* (2010); Jimenez, interleaved gradient
noise (2014), already in the tree.
**Residual:** dither reaches only the finishing block, so `--cs-debug` and the other debug views
are un-dithered by construction (they return from `main()` early — correct, they are data).

### E2. 3D LUT colour grading — Effort S
`tonemap_frag.glsl:52-55` grades with lift/gamma/gain and nothing else. A 32³ `.cube` LUT is the
format a colourist actually hands back, and post shaders are nowhere near their sampler budget, so
this is a self-contained pass with a real workflow payoff. Sequence it *after* the tonemap, and pin
which space the LUT is authored in — the working-space contract from 10.1/10.2 is the thing this
item is most likely to violate quietly.
**Depends on:** 10.1-10.2's working-space contract (shipped). **Wall 1:** unaffected.

### E3. Histogram auto-exposure — Effort M
`lum_measure_frag.glsl:44` writes `log2(lum)` and the mip chain averages it: a flat, unweighted,
whole-frame log-average with no metering mask and no percentile clipping. One bright practical, one
sun disc, one specular highlight drags the entire frame dark — and because exposure multiplies every
pixel, that is also the single largest source of cross-build non-determinism this repo has measured
(99.77% of pixels, per CLAUDE.md). A histogram with low/high percentile rejection fixes the image and
narrows that noise source at the same time.
**Refs.** Lagarde & de Rousiers, *Moving Frostbite to Physically Based Rendering* (SIGGRAPH 2014
course) — the exposure section.
**Depends on:** nothing. **Wall 1:** unaffected.

### E4. GPU timer queries + per-pass HUD — Effort S — **DONE (spec 11.27)**
**Wall 3 removed.** `GL_TIME_ELAPSED` works on the GL-over-Metal driver; `GL_TIMESTAMP` returns 0,
so scopes are flat. At 1600x1200 with fog/DoF/SSGI/TAA: **opaque 10.1 ms, SSR 8.3, atmosphere 4.2,
DoF 2.5, TAA 1.9, GTAO 1.9, bloom 1.0, tonemap 0.5 — 30.5 ms timed against a 54.7 ms wall frame.**
The post chain at 20.3 ms is larger than the forward pass, and SSR alone is within 20% of it.

An earlier draft of this entry read "opaque 24.3 ms of a 40.9 ms frame … which points the next perf
work at Wall 2 rather than at pixels". That denominator was a sum over eight rows with ~21 passes
unscoped, SSGI among them. **Withdrawn** — the numbers above reverse it, and no track ordering is
claimed from them here.

**Wall 3.** `glGenQueries` around each named pass, a ring of N-frame-deep results to avoid the stall,
and an ImGui table. Cheap, and it is the instrument every later perf claim depends on — including the
question this roadmap has never been able to answer: what does the full post chain actually cost,
pass by pass, at 4K.
**Depends on:** nothing. **Owns foundations:** per-pass GPU timing (consumed by E5, D4, any budget
work).

### E5. Instancing + LOD + sorted submission — Effort L — **DONE, two limbs of three (specs 11.28 / 11.29)**
**Wall 2, mostly removed.** Geometry dedup by `aiMesh` index (refcounted `Mesh`), one flattened
pre-order draw list replacing both walkers, per-cascade shadow-layer culling, instanced draws through
a std140 `InstanceBlock`, and LOD chains built at import as index ranges in the one EBO. On
`abandoned_window_shadowed`: shadow CPU **−83%**, TIMED **−41%**, frame **−38%**, 2,148 draws → 272.

**The third limb — draw sorting by program/material — did not ship**, deferred with a stated reason:
`abandoned_window` exports its 483 ivy leaves as one contiguous name-ordered block, so DFS order
already cost ~60 material switches in 553 draws and no other asset interleaved materials at all.
Shipping it would have been an unfalsifiable claim. That reason was true then and `apps/forest` has
since made it false; the limb moves to **E6**, where it belongs with the ordering objectives it
actually competes with.

Four things the specs established that no fixture had shown before, all in 11.29:

- **Scatter ORDER decides whether batching happens at all.** The batcher joins only *consecutive*
  survivors, so with most props frustum-culled a randomly-ordered scatter collapses to runs of one.
  Morton-ordering within each prototype takes the same 2,979 instances from **2,368 draws to 1,287** —
  identical instances, identical triangles, only the draw count moves.
- **LOD fights instancing, inherently.** The batch key is `(mesh, lod)`, so one prototype spanning a
  range of distances splits into separate runs. A CPU-for-GPU trade whose direction depends on framing.
- **The relationship is not monotonic.** At `--lod-bias 0.35` LOD *saves* draws (997 against 1,287),
  because once enough neighbours collapse onto the same coarse level they share runs again.
  Fragmentation peaks when a level boundary cuts through a cluster and falls away on both sides.
- **"meshoptimizer locks mesh borders" was wrong, and was wrong before E5.** `simplifier.cpp:533`
  promotes `Kind_Border` to `Kind_Locked` **only** under `meshopt_SimplifyLockBorder`; `lod.c` passes
  `0`. Borders are weighted, not locked. The belief had propagated into three headers (`lod.h`,
  `rock.h`, `terrain.h`) and spec 11.28, and it was load-bearing twice over: it justified a design
  choice (icosphere over UV sphere, for a border a UV sphere does not really have), and `terrain.h`'s
  crack-free tiling argument does not hold — two neighbours at different LOD levels can T-junction.
  None has been observed; that is luck, not a guarantee. It was never checked against the vendored
  source it was a claim *about*, which was one grep away for the whole of E5.

**Depends on:** E4 (soft — done first, as intended; the LOD thresholds are measured, not guessed).

### E6. Depth prepass + opaque draw ordering — Effort M — **DONE (spec 11.30)**
**Wall 4**, taken directly. Shipped: a depth-complexity instrument, front-to-back opaque ordering (on
by default, `--no-sort-opaque`), one shared object-position chunk with `invariant gl_Position`, and a
position-only depth prepass (`--depth-prepass`, off by default).

**On `apps/forest`, the shipping AA path:** opaque **397 → 234 ms (−41%)** with both, depth complexity
**3.87 → 1.94**. Ordering alone is −36%, the prepass alone −11%, and they are worth more together.

**The interior is where the prepass is actually decisive**: `abandoned_window_shadowed` goes
**31.5 → 11.2 ms (−64%)** and reaches depth complexity **exactly 1.00** — every hidden fragment
rejected, the theoretical floor. Forest reaches only 2.80 because **alpha-masked foliage sits the
prepass out**, and foliage is where its overdraw lives. The limit is the masked exclusion, not the
prepass.

**Four claims this entry made were withdrawn by measurement, and the pattern is the finding.**
It priced E6 at 250 ms, called it the highest-value item by a wide margin, explained Wall 4's
invisibility by asserting the corpus had "no depth complexity", and assumed a prepass would supersede
ordering. Each was written before the instrument existed. The instrument then corrected *itself*
twice — 1.85 was halved by 4x MSAA writing two samples of four for a leaf at alpha 0.5, and a first
attempt counted the prepass's own samples so complexity ROSE when the pass meant to lower it came on.
**Three consecutive readings of one scene — 1.85, 2.78, 3.87 — each looked authoritative and each was
an artefact of its configuration.**

Also shipped from the same work, because forest could not be measured honestly without them: forest
now uses TAA instead of 4x MSAA (−29% on its own, and what the render app always did), and forest is
recorded as **not pixel-deterministic** — 34,991 px run-to-run, the Hillaire sky, a precondition spec
11.29 asserted and never checked.

**What was left is now done (spec 11.31), and it changed the answer.** Masked geometry enters the
prepass by being drawn with its own `pbr` program in a `depthOnly` mode — one source, two
permutations, rather than the shared coverage chunk this row proposed. Forest's complexity falls to
0.72.

**Three of this row's numbers are withdrawn by that work.** Every depth-complexity figure above is
**twice** the truth: the budget came from `engine->msaa_samples`, and this driver returns a 2-sample
target when asked for one sample, so the whole TAA path double-counted. Halve them. The −64% on
`abandoned_window_shadowed` does not reproduce at either AA path — that scene measures the prepass
**costing** 6.7% while reaching a perfect complexity of 1.00. And "worth more together" was an
artefact of the masked exclusion: with it gone the sort and the prepass are substitutes, the sort is
the cheaper of the two, and the prepass has no configuration in this corpus where it pays. Defaults
stay sort-on, prepass-off, now on measurement rather than caution.

**The prepass.** Render opaque depth-only first, then shade with `glDepthFunc(GL_EQUAL)` and
`glDepthMask(GL_FALSE)`. Every hidden fragment is then rejected by the depth test before the uber-shader
runs, at the cost of one extra geometry pass. The shadow pass already proves that pass is cheap here:
23 ms for 130M triangles, against 312 ms for 26M shaded. It is not free and it is not universally a
win — it pays back only above some depth-complexity threshold, and `apps/forest` should be measured on
both sides of it rather than assumed to clear it.

**Two things that will bite, and both are already visible in the tree.** Masked geometry must be
prepassed with the *same* cutoff and the same alpha source or its depth will not match and `GL_EQUAL`
will drop it — and `render.c:509` enables `GL_SAMPLE_ALPHA_TO_COVERAGE` for masked materials under
MSAA, so the prepass has to reproduce the coverage decision, not merely the discard. Second, anything
whose vertex shader displaces (wind, `pbr_vert`'s `windOffset`) must displace *identically* in both
passes or the depths disagree — which makes this the item that finally forces wind to be deterministic
per-pass rather than merely per-frame.

**The ordering.** One sort key now has three objectives competing on it, which is why E5's third limb
belongs here rather than as its own row:

| objective | wants | costs |
|---|---|---|
| batch contiguity (E5, shipped) | identical `(mesh, lod)` adjacent | Morton order, camera-independent |
| material switches (E5's deferred limb) | same program/material adjacent | fights both others |
| front-to-back (new, Wall 4) | near before far | camera-dependent, rebuilt per frame |

A prepass changes the arithmetic: with one, front-to-back matters much less (the depth buffer is
already complete), so the key can stay on batching and materials. Without one, front-to-back is the
only lever on overdraw. **Decide the prepass first; the sort key falls out of it.** `apps/forest` is
the fixture that makes all three falsifiable, which is exactly what 11.28 said it lacked.
**Refs.** The prepass/`GL_EQUAL` idiom is standard; the alpha-to-coverage interaction is the part
worth reading up on rather than deriving.
**Depends on:** E4 (hard — this is a perf item and its whole claim is a measurement). **Wall 1:** unaffected.

### E7. Occlusion culling — Effort L
Named in Wall 2's original text, deferred by 11.28 with one line ("its own latency story on GL 4.1
occlusion queries"), and never given a row until now. Object-level rejection of what is behind other
geometry — the complement to E6, which rejects at the fragment level.

**The latency story is the whole item.** GL 4.1 has `GL_SAMPLES_PASSED` / `GL_ANY_SAMPLES_PASSED` and
nothing else; there is no compute, no indirect draw, no GPU-driven culling under the macOS ceiling.
Reading a query in the frame that issued it stalls the pipeline and costs more than it saves, so the
only viable shape is the classic one: test bounding volumes against *last* frame's depth buffer, accept
one frame of latency, and accept popping on fast camera motion. A software HZB (downsample the depth
pyramid, test AABBs on the CPU) avoids the query round-trip entirely and is worth pricing against the
query path before either is built.

**Sequence it after E6, and expect E6 to shrink it.** A prepass already gets most of the benefit for
opaque geometry, cheaply and with no latency or popping. What occlusion culling adds on top is
skipping the *vertex* and submission cost of hidden objects, which E6 still pays in full — relevant
only once vertex or draw cost is measurable, and today `apps/forest` says it is not (12x more draws is
faster). **On current evidence this item is not yet justified**; it is booked so the gap is visible,
not because a measurement demands it.
**Depends on:** E6 (soft — build the cheap shield first and re-measure). **Wall 1:** unaffected.

### E8. Fixing the wind cull — Effort S
`draw_list.c:94` marks any `wind_response > 0` material `DRAW_UNBOUNDED`, and `draw_item_visible`
(`draw_list.c:201`) then accepts it unconditionally — camera *and* every shadow cascade. Wind-responsive geometry is
therefore never culled by anything. The fix is to expand those meshes' AABB by maximum wind
displacement so they become cullable rather than exempt.

Small, self-contained, and it removes a real correctness-shaped hole in E5's culling. It also unblocks
wind on scattered content: `apps/forest` carries **no wind on 2,000 trees** specifically to avoid this,
which is a visible look compromise made for a fixable engine reason. Pairs naturally with per-instance
wind phase (`windOffset` is object-space with per-mesh uniforms, so every instance of a shared tree
sways identically) — one instance-block field, and the two want the same arms.
**Depends on:** nothing. **Wall 1:** unaffected.

## Sequencing — tiers & rationale

**Tier 1 — the AAA leap (environments):**
| # | Item | Effort | Why here |
|---|------|--------|----------|
| 1 | A1 Clustered forward | L | **DONE** (spec 9.1). The disruptive light-pipeline rewrite goes first so every later item edits the final loop + UBO layout once. NB the CPU-cost claim went unvalidated — see 9.1's as-built notes. |
| 2 | A2 LTC area lights | M | **DONE** (spec 9.2). Signature environment feature; contained M on the now-stable loop; must precede DDGI so probes capture area-lit rooms. |
| 3 | A3 Contact shadows | S | **DONE** (spec 9.3). Post-only depth march along the key light; no shadow.c changes (postfx already had the light dir + view matrix). Default off. |
| 4 | B1 Froxel volumetric fog | L | **DONE** (spec 9.5). Owns the 3D-texture + volume-draw machinery all volumetrics need; consumes A1's light list on day one. Shipped with one-layer-per-draw slices, not the layered-GS sketch — that matches the cascade/mask-array/cube-face idiom and needs no geometry shader. |
| 5 | B9 Aerial perspective | ~~S~~ **M** | **DONE** (spec 9.6). The "S once B1's machinery exists" estimate answered the wrong question: machinery cost was never the blocker. 4.7 deferred this user-approved because it is invisible at prop scales, and B1 did not change that — so it shipped with a world-scale fixture and the units→km knob 4.7 named as the missing piece. Effort M. |
| 6 | A4 DDGI probe volume | XL | **DONE** (spec 9.7). The sequencing paid off exactly as argued — captures run the full forward shader, so probes see the clustered lights and LTC panels for free. The cost model did not: a fixed per-frame budget would have meant 12 scene renders and 12 cluster-grid rebuilds every frame forever, so it converges and then idles at literally zero. Two shadow faults and one area-light defect surfaced en route, none of them A4's; the leak gate is the one item left open. |
| 7 | A7 Punctual shadow maps | M | **DONE** (spec 9.8). Unbooked when it was written — the roadmap had no entry for point or area shadows, only A2's line deferring them. Scheduled here because 9.7 had just moved every per-light-type shadow gate into one binder and left the extension point empty, and because the two shadow faults 9.7 surfaced shared one root cause: a single condition trying to model four light types. |
| 8 | B2 Volumetric clouds | XL | **DONE** (spec 11.0). Shipped in five phases with three corrections to the sketch above: no separate reproject shader (the march blends its own ray-direction history in place, froxel-style, off a cloud-owned previous camera); no curl field in v1 (nothing consumes it); captures skip the screen-space cloud texture entirely rather than re-marching (probe/GI see clouds via the env bake instead). The sketched `cloud_texture` became a sky-owned parity ping-pong; B1's `create_texture_3d` was NOT reusable for the noise (float/clamp -- a tiling RGBA8 sibling was added, as texture.c's own comment predicted). Env re-bake cadence split drag/release: 73.6 ms with clouds vs ~10 without. |

(6↔8 are swappable — no hard dependency either way.)

### Interlude — correctness & conformance series (9.9 → 10.x), shipped between Tier-1 items 7 and 8

Not on the tracks above: a measurement-driven quality series that landed after A7, each item with
its own spec, branch, reviews and gates. It exists because using the engine surfaced defects the
roadmap's feature items could not — a wrong tonemap constant, non-photometric light scales, shadow
banding at real viewing distances, off-spec KHR materials against reference renderers, and temporal
flicker whose root cause was an estimator, not a filter. The series continued past B2 with 11.1
and 11.2 (below). **Tiers 1, 2 and 3 are all complete** — Tier 2 closing with B3 pre-integrated skin
(11.13), Tier 3 with A6 moment shadow maps (11.22) and B8's split (11.20). Tier 4 below is proposed,
not scheduled.

- **9.9–10.0 photometry:** PBR Neutral desaturation blend was inverted vs the Khronos reference;
  then punctual lights went genuinely photometric (candela/lux imported as authored, EV100 camera)
  instead of rescaled to renderer units.
- **10.1–10.2 the working-space contract:** view exposure as a first-class engine contract (1.0 =
  diffuse white in working space), then six reviews' worth of stragglers moved into it. The
  scale-invariance gates in `scripts/gates.py` (lights ×1000 / exposure ÷1000 → 1 LSB) date from
  here.
- **10.3–10.6 shadow quality:** area-light shadows + the horizon term the LTC integrator never had;
  area-map projection fit before density; the cascade policy finished to match the punctual path
  (contact_fixture's large sun-lit ground was the first real receiver); stochastic PCSS (per
  pixel/frame kernel rotation, averaged by TAA). The analytic shadow gates (penumbra width, leak,
  umbra ellipse, acne, churn) grew alongside.
- **10.7/10.7.1 KHR sheen conformance:** Charlie alpha = sheenRoughness² per spec, the sheen E-LUT
  baked into the BRDF LUT's blue channel, a Charlie-prefiltered sheen environment (unit 9 — LTC
  packed into a 2-layer array on unit 7 to free it), reference Charlie-lambda visibility.
- **10.7.2/10.8.1 temporal stability:** SSR's own accumulator (inverse-luma blend, motion-adaptive
  clamp slack), equirect mips before cubemap conversion, a motion-adaptive TAA window, env-cube
  aniso — driven by live flicker reports against an 8k HDR.
- **10.8 KHR specular conformance:** f90 = specularFactor threaded to every F0 consumer, the spec's
  achromatic diffuse trade, clamp-before-factor — checked against the Khronos sample renderer.
- **10.9 SSR trace rewrite:** the estimator itself — one unified hi-z loop with exact per-column
  interval acceptance, receiver-continuity step rejection, and a bounded behind-for-good exit to
  the probe. Raw-trace comb metric −90%, the behind-silhouette ghost gone, fully-dressed churn −69%
  at the acceptance camera. The `froxel_fog_golden` drift it flagged got its owner in 11.0's
  phase-0 bisect: a legitimate catcher z-fight fix (e86fce9), golden re-baked. Post-merge on
  master: reach limits dissolve via a budget-consumed fade instead of an env fallback (built and
  rejected — an invisible catcher mirroring the sky prints as a glowing pool).
- **11.1 import unit scale:** FBX's declared unit (cm) bakes to metres at import via assimp's
  GlobalScale — the photometric model's implicit metre finally holds for imported assets (c64's
  lights sat 1,200 "metres" out and culled to black). The UV V-flip default flipped to the baked
  convention every textured FBX in the tree actually uses. Also bisected and re-baked the
  three area-light goldens, stale since three deliberate un-re-baked changes (e7aa468, abc9d0b,
  7ae16d0).
- **11.2 fixed-step render clock:** the engine's frame clock produces a fixed 1/60 headless
  (frame-index-pure), and particles/animation/game-step-count ride it instead of the wall clock —
  tree went from 308k px run-to-run to 0, spores' step count became exact, and
  abandoned_window's recorded "bistability" turned out to be wall-clock wind phase. Plus the
  first gate to exercise the FBX light-import path (an ASCII FBX fixture pinning the 0.01×
  unit-scale mirror).

**Tier 2 — image quality & performance:**
| # | Item | Effort | Why here |
|---|------|--------|----------|
| 9 | A5 Bent-normal spec-occ | M | **DONE (11.3 + 11.4):** split ambient specular, default `split`, exact occlusion by construction. |
| 10 | B5 Bokeh DoF | M | **DONE (11.6):** near/far gather, N-gon kernel, first DoF golden. |
| 11 | B4 TAAU | L | **DONE (11.7):** render/post/half split, separate upscaling resolve, `--render-scale`; ~2x at 0.67. |
| 12 | B3 Pre-integrated skin | S→**M** | **DONE (11.13):** opt-in `curvature_scale`, 16-row const table, `.cscn` material overrides. Not zero infra: fit tool, fixture, 3 gates, golden. Shadow limits the wrap — and 11.19 measured that the blur, not an angular term, is what recovers it, which is why B3.1 was rejected rather than built. **Inert for realistic content since 11.14** fixed the blur it was compensating for. |
| 12.1 | SSS blur width | M→**L** | **DONE (11.14):** the blur capped its kernel in PIXELS, so delivered world scatter fell as 1/height — 2.5% of its low-res value at 4K, a defect live since SSS shipped and named in `specs/4.12-sss.md` with nothing behind it. Fixing the units alone was correct arithmetic and an unshippable image: a resolution-independent world width means an unbounded PIXEL width, and 12 fixed taps then resolve individually as rings. Replaced the separable blur with a scale-space pyramid — one trilinear tap per profile Gaussian per channel — so reach stops depending on the tap budget. Now 2.5% drift across a 4x sweep, and 91% of Penner's integral against the old kernel's 129%. New `sss-scale` and `sss-band` gates; first SSS-ON golden. |

**Tier 3 — polish & late-tier (unparked: Tiers 1-2 have landed):**
| # | Item | Effort | Why here |
|---|------|--------|----------|
| 13 | SSS is dead in procedural scenes | S | **DONE (11.14).** `scene_has_subsurface()` reads `scene->materials`, which only `import.c` ever filled, so a scene building materials in code never ran the SSS pass — `apps/tree` had authored `subsurface` 0.6 / 0.45 and two profiles and never rendered any of it. Fixed at the registry (`scene_sync_materials`), so mask packing, name lookup and material ownership are fixed with it. Moves 26.7% of the tree's frame; no gate covers that, only a look pass. |
| 14 | B3.1 Shadow-penumbra scattering | M | **REJECTED (11.19):** the premise no longer holds. A cast shadow across skin already scatters — `skin_shadow_fixture.cscn` measures 186,847 px between SSS on and off, hard black ellipses becoming soft red bleeds — because `shadow` rides *inside* `sssDiffuse` and the blur 11.14 repaired carries it. The angular term contributes **0 px** there (`--no-skin-preint` is byte-identical), so Penner's second LUT would be inert for the same reason row 12 records B3 as inert. Reopen only if the pyramid's ceiling binds again or a scene needs this under `--no-sss`. |
| 15 | B3.2 Skin under an area light | M | **DONE (11.19):** the area branch added its diffuse to `Lo` and skipped the SSS tap, so a softbox — the canonical portrait setup — gave skin nothing at all; SSS on measured byte-identical to `--no-sss`. Tapping `areaDiff` into the skin buffer takes it 0 → 551,295 px. New `skin_area_fixture.cscn` (curvature fixture's geometry and material, light swapped) and a two-arm gate carrying the directional control. The pre-integrated wrap stays out: a rectangle has no single `L`, and that half — shared with the IBL gap — is still open. |
| 16 | B6 Moment-based OIT | L | **DONE (11.17):** four power moments, **on by default** with OIT, 4.94x closer to the arithmetic than the weighted-blended weight (0.0157 RMS against 0.0773). No golden moves (none of their scenes has an alpha-blend mesh); the raiden baseline moves 0.12%, the hair silhouette. 4.17's ordering defect came with it onto the default path and was **fixed in 11.18** — the catcher simply drew too late; moving it ahead of the transparent pass repaired the unsorted late pass and the particle depth resolve too, with all 18 goldens at 0 px. Not zero infra: a card-stack fixture with an analytic answer, three gates, the first OIT golden. Two plan corrections worth carrying forward — the old `oit_fixture` **cannot discriminate any two OIT schemes**, because weighted-blended is exact whenever every layer shares a colour; and `pbr_frag` has no seventeenth sampler for ANY unit, free or not, since the driver counts declarations. Six or eight moments would need a third fragment output location and GL 4.1 guarantees eight, all spoken for. |
| 17 | B7 Lens flare / finishing | S/M | **DONE (11.21).** Chapman ghost chain off the bloom pyramid + lateral chromatic aberration, both default off, all 18 goldens 0 px. The sketch overstated the scope: grain, vignette, sharpen and grade had already shipped, so the real work was the flare chain and aberration; starburst and the dirt mask remain unbuilt. Three lessons carry forward. **Units, again** — aberration shipped as a raw UV offset (~450 px, frame triplicated) and the flare repeated it three more times in one file: ghost brightness scaled with ghost COUNT, halo width was a UV radius so the one circular part of a flare rendered as an aspect-dependent ellipse, and width 0 was the slider's BRIGHTEST setting. **A fixture is a claim and must be measured** — this one declared no lights, got the auto three-point rig, and its "black" backdrop measured 0.224, diluting the signal 35x. **A gate arm that cannot fail is not coverage** — "off is off" passed against a deliberately broken composite, because the C short-circuit means both arms take the same path; it was replaced by a linearity assertion that fails at 1.000. Also filed: no golden runner exists, so "all 18 at 0 px" means scraping recipes out of a dozen specs by hand. |
| 18 | A6 Moment shadow maps | L→**M** | **DONE (11.22).** Filterable 4-moment cascades, opt-in `--msm`; PCF/PCSS stay default. Resolved *from* the finished depth array, so the depth pass is byte-identical and `--no-msm` is a provable 0 px — which also retires two of spec 10.4's five reasons for deferring VSM. No new sampler: MSM replaces unit 10 rather than adding to it, and that is what took the effort from L to M. 75 MB, half the sketch's estimate. The one thing it does that PCSS cannot is exceed `PCSS_MAX_RADIUS_UV`, since a prefiltered tap costs the same however wide it blurs. **Two arms that could not fail, both caught here.** The `hole` arm the plan called "the number that decides the item" left `hole-msm` at 0.0084 unchanged to four decimals against a deliberately broken two-moment reconstruction that moved the frame 6339 px — deep inside an umbra every reconstruction agrees, so it was coverage of nothing; the `pillar` arm on the thin caster's band reads 0.4249 vs 0.0000 and does discriminate. Then all six MSM arms turned out to pass silently when `shadow_build_msm` bails, because that frame *is* `--no-msm`; the fix is an inverse arm asserting blurred moments DO leak. Prefiltering a thin caster is still where four moments run out (blur 1.0 leaks 0.4159, so the default is 0), and the sketch's "one prefiltered tap beats 32 stochastic ones" does not hold at that default (7775 px vs 7545). No golden: six analytic arms cover it better than a stored PNG of an off-by-default path would. **The three structural unifications eight reviewers raised against it were measured and declined** — the shared prefilter helper is 6 identical lines wanting 8 parameters across 2 callers, the shared moment kernel is forbidden by two reference constants 60x apart plus a normalisation only one side has, and the PCSS/MSM enum turns out to guard a case already guarded twice over. Recorded in 11.22 as rejections rather than deferrals, since deferred reads as pending and this is the kind of thing a review raises every round. |
| 19 | B8 Hair | XL | **CLOSED, split (spec 11.20).** The strand map shipped and is live: it rides the **anisotropy** slot that already existed, so binding one stretches the ordinary GGX highlight along the painted grain — general enough for brushed metal and satin, which hair merely motivated. The R/TT/TRT fibre lobes were built, swept low and high in all combinations, rejected at every setting, and **deleted**. Two structural faults, neither tunable: the lobe replaced the whole microfacet term without its `/(4·NdotV·NdotL)` or energy compensation AND still flowed through a surface integrand (`NdotL` on the card normal, a coat/sheen stack, a half-vector diffuse); and cards carry no normal map, so per-texel facing is absent from the asset. The lesson worth more than the feature: `set_material_anisotropy_tex` had zero callers, so an energy-paired direction channel was already sitting there and this work built a second one beside it before anyone looked. |

**Tier 4 — completeness, authoring & the perf floor (proposed, nothing scheduled):**
| # | Item | Effort | Why here |
|---|------|--------|----------|
| 20 | E1 Output dither | S | **DONE (11.24).** On by default, `--no-dither` 0 px; sky bands 192 px → 22 px. The sketch's TPDF construction was wrong — a constant `ign` offset only phase-shifts the same sawtooth and collapses to four values — and evaluating the pure function in Python caught it before any shader existed. Static by construction, which measurement confirms costs the churn gates 0.5%. The re-bake reported six of nineteen goldens unreproducible; **11.25 showed that was wrong and all nineteen reproduce** — four recipes were in a ledger nobody opened, and `cloud_fixture` names no file at all. |
| 21 | C1 Translucent shadows | M→**L** | **DONE (11.26).** Shipped as deep opacity maps — transmittance storage, not the moment reconstruction the sketch assumed, which is why it stayed inside the sampler budget. Bands measure 0.0016 against 0.65^k and the mask ramp 0.0025 against 1-alpha, where a binary alpha test scores 126x worse. Raiden moves 4.0%, all of it the groom and the shoulder under it. Three bugs found by the arms, each presenting as something else — a blend function that desaturated the frame while the shadows looked right, a resolve that covered half the map with a diagonal edge, and two instrument faults that read as renderer faults. |
| 22 | D2 Local fog + cloud shadows | M | Highest look-per-line left in the atmosphere stack, and its valuable half (froxel injection) needs nothing from D0. B2 deferred cloud shadows explicitly; this collects the debt. |
| 23 | C2 Emissive → area lights | S/M | A fit plus a registration — `Light` already carries every field the fit produces. Makes practicals and screens light rooms instead of just glowing. |
| 24 | E4 GPU timer queries | S | Wall 3. Cheap, and every perf claim after it is honest in a way the ones before it are not. Sequence before E5/D4 or their thresholds are guesses. |
| 25 | C3 IES profiles | S | Collects the payoff for 9.9/10.0's photometric work. Fits Wall 1 by living in the light UBO — zero texture units. |
| 26 | C5 Contact shadows for local lights | S | A3 generalised from one light to N. Postfx-only. |
| 27 | E3 Histogram exposure | M | Fixes the image *and* narrows the largest measured source of cross-build non-determinism. |
| 28 | E2 3D LUT grading | S | Colourist workflow. Watch the working-space contract. |
| 29 | C4 Clustered specular probes | L | Diffuse GI got a spatial structure in A4; specular still has exactly one probe. Reuses A1's grid and A4's atlas. |
| 30 | E5 Instancing + LOD + sorting | L | **DONE, two limbs of three (11.28 / 11.29).** Wall 2 mostly removed: `abandoned_window_shadowed` shadow CPU −83%, frame −38%, 2,148 draws → 272. Sorting deferred as unfalsifiable against the corpus, which `apps/forest` has since falsified — moved to E6. Established that scatter *order* decides whether batching happens at all (2,368 → 1,287 draws for identical geometry), that LOD fights instancing on the `(mesh, lod)` key non-monotonically, and that "meshoptimizer locks mesh borders" — in three headers and spec 11.28 — was wrong from the start. |
| 31 | **E6 Depth prepass + opaque ordering** | M | **DONE (11.30 + 11.31).** `apps/forest` opaque **306 → 169 ms (−45%)** from the ORDERING alone, depth complexity 1.93 → 1.08. Masked geometry now prepasses too (11.31, via a `depthOnly` mode in `pbr_frag`) and reaches a better 0.72 — and is still **slower** than the sort, because a full extra geometry pass costs more than the shading it saves. The two are substitutes, not complements: 11.30's "worth more together" was an artefact of the masked exclusion. Ordering ships on, the prepass off, with a gate arm asserting the prepass **costs** on a scene with no overdraw. 11.30's own figures were doubled by a budget that trusted `msaa_samples` over the driver, and its −64% interior does not reproduce. Between them these two specs withdrew seven claims — every one from an instrument that had never been checked against a scene with a known answer. |
| 32 | D0 Free two sampler units | M | Foundation only — schedule it **with** D1 or D2's surface half, never before, per the just-in-time rule. |
| 33 | D1 Clustered decals | L | Largest environment-art gap. Hard-blocked on D0. |
| 34 | E8 Fix the wind cull | S | Small, self-contained, closes a real hole in E5's culling — wind geometry is currently exempt from the camera frustum *and* every cascade. Unblocks wind on scattered content, which `apps/forest` gave up to avoid it. |
| 34b | **E9 One sample means one sample** | M | **DONE (11.34).** `apps/forest` opaque **150.9 → 121.6 ms (−19.4%)** against a 0.23% floor, with byte-identical submission integers — the same work, cheaper. One branch in the one allocator plus one at the depth renderbuffer flips the scene, OIT and moment FBOs in lockstep, since they share the depth attachment. The row's original prescription was wrong twice: there is no `sampler2DMS` anywhere in the corpus (11.17 rejected it), and postfx reaches the scene target only through blits, so the GLSL surface was zero files and postfx changed nothing. Priced before built with a new `--msaa <n>` lever, which also decomposed the first confounded A/B: A2C alone costs 202 ms of forest's opaque row (fragment-set explosion, headless-only), a sample ~93 ms on that inflated set. TAA-only edges verified by crops (raiden groom, forest canopy — indistinguishable), all 23 goldens 0 px, and MBOIT's moment-resolve bias (11.17) is now absent on the TAA path for free. |
| 35 | ~~D3 Tessellated water~~ | — | **SHIPPED (11.32, 11.33, 11.35) and it spent no tessellation.** The mesh went through two screen-space schemes instead: clipmap rings (11.33), then a **projected grid** (11.35) after the rings turned out to weld reach to near-field detail — the snap that makes them tile is the same thing that kept the surface 5° short of the horizon while a comment claimed otherwise. The stage this item was scheduled to open is still closed. Reaching the horizon then moved the problem from the MESH to filtering: distant cells cover more than a wave period, so each wave model drops what sits under its footprint and hands the slope energy to roughness — a BRDF answer to a geometry question. See D3. |
| 36 | D4 Terrain | XL | Only after E5; a clipmap without instancing/LOD is a mega-mesh with extra steps. `apps/forest` is a *consumer* of E5, not this — fixed tiles with per-tile chains, fine at 1 km² and explicitly not the answer above it. **Inherits D3's clipmap at `8d04658`** — the rings-over-a-mip-pyramid half water never used is the half terrain needs — and its T-junction stitch, which is a better fix for the crack risk E5 left open than locking borders. |
| 37 | E7 Occlusion culling | L | Booked so the gap is visible, **not because a measurement demands it**, and 11.31 lowers the price further rather than raising it: forest's opaque lane already runs at complexity 1.08 from ordering alone, so there is little redundant shading left to remove, and the one thing that reached 0.72 — the prepass — lost on the clock anyway because the extra submission cost more than the fragments it saved. An occlusion pass is a bigger version of that same trade. `assets/overdraw_layers.gltf` is the instrument to price it with. |

**If only five ever get built: 20 -> 21 -> 22 -> 23 -> 24.** One afternoon, then three items that each
reuse a shipped subsystem rather than building new machinery, then the instrument the rest needs.

**31 (E6) was the one to build next, and it is now built.** An earlier draft of this line read
"nothing else on this table is worth 250 ms", which priced E6 at the whole of the opaque pass before
anything had measured how much of that pass is redundant. The real figures are **−41% on forest and
−64% on the interior** — smaller than the invented number, and arrived at with the crossover, the
worst case and the withdrawn claims all recorded. **37 (E7, occlusion culling) should be re-priced
against them**: it was booked as not-yet-justified on the grounds that E6 would get most of the
benefit for less, and E6 doing so on opaque content is now measured rather than assumed.

### Known limitations not booked as items

Recorded because they are real, understood, and currently nobody's row — not because they are
scheduled.

- **Volumetrics, DoF and AO do not see transparent surfaces.** The transparent pass runs with
  `glDepthMask(GL_FALSE)` (`render.c:975`), so the aux linear-Z target holds whatever opaque surface
  is *behind* the glass. Fog, aerial perspective, DoF and GTAO therefore all treat a transparent pixel
  as the wall behind it. Standard forward-renderer behaviour; the clean fix wants the froxel volume
  sampled inside `pbr_frag`, which is Wall 1. A partial fix (per-draw analytic fog on transparent
  meshes) is available and cheap but will not match the froxel result.
- ~~**No golden runner.**~~ **BUILT (spec 11.25)** — `python3 scripts/goldens.py` checks all
  nineteen in one command, and every render command now lives in one table instead of in whichever
  spec introduced it. **The corpus was never broken**, which is the part worth carrying: 11.17
  recorded three goldens as having no recipe anywhere, 11.21 repeated it and named four, and 11.24
  concluded six of nineteen were unreproducible and wrote that here. All three were wrong. Four of
  the six had their recipe in `assets/area_light_goldens.md`, a per-feature ledger nobody thought
  to open; the two `cloud_fixture` goldens do not name a fixture at all, since **no such file has
  ever existed in this repository** — they are the aerial fixture with `--clouds`. Three specs in
  a row reached a false conclusion from the same cause, which is a better argument for one table
  than any of them made on purpose.
- **Anisotropic filtering is capped at 8x** (`texture.c:89`) and never exposed as a setting.
- **Shadows read last frame's transforms.** `render_shadow_depth_pass` runs at `engine.c:2231`;
  `apply_transform_to_nodes` runs inside the app callback *after* it. A real latent one-frame lag,
  found during 11.28. It is a bug rather than a feature, it is invisible in every golden (which are
  static or frame-locked), and it deserves its own arm — a moving caster whose shadow trails it by a
  frame is the fixture, and nothing in the corpus is one today.
- **`glUseProgram` has no choke point, and the uniform value cache assumes one.** The setters write
  through `glUniform*`, which targets whatever is *bound*, not what the `UniformManager` names — so a
  set under the wrong program updates that program and records the value here, after which the next
  legitimate write is skipped as already-held. Verified clean (zero violations across seven scenes
  under `-DCETRA_CHECK_UNIFORM_BINDING=1`) but only *believed* going forward, because the check costs
  a `glGetIntegerv` per set, about 6% of the frame, and is off by default. Neither Godot nor Unreal has
  this problem, and not by asserting harder: neither lets app code call `glUseProgram` at all, so
  "what is bound" is a mirrored variable and the check is a compare rather than a driver round trip.
  Cetra calls it from ~30 sites. The fix is one function; the cost is touching all 30.
- **Skinned meshes cannot instance.** Blocked on the single global `g_current_animation_state`, so
  `raiden` and every other rig submits one draw per mesh regardless. 11.28's `skinned-nobatch` and
  `skinned-identity` arms hold the line deliberately: a program without an `InstanceBlock` may never
  carry more than one instance.
- **No scene-owned mesh pool.** Refcounting on `Mesh` (11.28) is the interim; a pool is the correct
  ownership model and composes with the refcount rather than replacing it.
- **No screen-size / density culling.** Falls out of the flattened draw list in a few lines once a
  consumer asks. No content demands it — `apps/forest` is dense but its problem is fragments, not
  object count.
- **Transparent geometry is never depth-sorted.** OIT is on by default and order-independent, so this
  costs nothing today; it would only matter under `--no-oit`, where sorting would move pixels
  (including the raiden hair, which has already moved four times) for no measured win.
- **Terrain tiles can crack at LOD boundaries, in principle.** `lod.c` simplifies with options `0`,
  so tile borders are weighted rather than locked and two neighbours at different levels can
  T-junction. None has been observed at any framing tried, which is large tiles and near-straight
  borders rather than a guarantee. The obvious fix is `meshopt_SimplifyLockBorder` on the terrain path
  specifically — deliberately not applied globally, since it would constrain every other chain
  through the same call. **There is a better one, and D4 now records it**: the T-junction stitch D3's
  water clipmap used (commit `8d04658`) lets both sides simplify and makes the finer one agree, which
  needs a height function evaluable at arbitrary points — which terrain has and a general mesh does
  not. Locking a border keeps its full density forever; the stitch does not.
- **The GL 4.1 ceiling itself is the Tier 5 question.** Nothing in Tier 4 needs compute. Lumen-class
  GI, virtual shadow maps, GPU-driven culling, Nanite-style cluster DAGs (which additionally want
  SSBOs, `glMultiDrawElementsIndirect` and 64-bit atomics) and hardware ray tracing all do, and
  `docs/rendering-roadmap.md` §5 already prices the three honest answers (offline CPU path tracer /
  hybrid Metal RT via IOSurface / a full Metal or Vulkan backend). That document owns the question;
  this one should not re-open it.

## Foundations ownership (just-in-time)

| Foundation | Owner | Later consumers |
|---|---|---|
| UBO machinery (`ubo.c/h`, std140, binding registry) | A1 | froxel light data, any bulk constants |
| CPU light culling (`light_cluster.c/h`) + shared exponential Z-slicing constants | A1 | B1 froxel injection |
| Float-texture-from-C-array (`create_texture_2d_float`) | A2 | future LUTs (MSM quantization, etc.) |
| `Light.up` orientation frame | A2 | PackedLight layout |
| Key-light view-space direction publication to postfx | A3 | future light-aware post passes |
| Octahedral encode/decode include (`include/octahedral.glsl`) | A4 — **delivered** | bent-normal storage, oct G-buffer normals |
| Capture-at-position helper (`scene_capture_faces`, render.c) | A4 — **delivered**, and it grew an optional depth cubemap: pass one and it renders straight into the destination faces at native size, because a blit cannot carry depth between differently-sized targets | future probe features |
| Bent normal in the AO chain | A5 | SSGI directionality, SSR occlusion, DDGI sampling |
| `create_texture_3d_float` + `include/froxel.glsl` (slice count parameterized, so a differently-sized volume reuses it) | B1 | B9 aerial perspective, B2 clouds, future volumetrics |
| CPU 3D noise (`noise_worley3`, Perlin-Worley packing, threaded bake) | B2 | ground fog detail, media |
| Render-res/post-res split — **delivered** as four sizes (`width/height` render, `post_*`, `out_*`, `half_*` = render/2), plus the canvas locals every post-seam pass composites onto | B4 | B5, B7, tonemap |
| Transmittance-vs-depth storage in the shadow path | C1 (proposed) | any translucent caster: hair, glass, foliage tips, smoke |
| Freed `pbr_frag` sampler units | D0 (proposed) | D1 decals, D2's surface-shadow half, detail/wetness maps |
| Tessellation pipeline (program creation, patch draw, distance LOD) | **still unowned** — D3 shipped without it | D4 terrain, POM silhouettes |
| Bed-height seam (`WaterHeightFn`) + the CPU Gerstner query | D3 — **delivered** (11.32, 11.33) | Jolt buoyancy, gameplay water tests, any surface that shoals |
| Geometry clipmap: coarsest-cell snap + T-junction stitch | D3 — built (11.33), **removed** (11.35), kept at `8d04658` | D4 terrain, where the streamed-mip-pyramid half water never used is the point |
| Screen-space footprint → detail handover (mip level or dropped octave, energy into roughness) | D3 — **delivered** (11.35) | any procedural surface a projected or adaptive mesh under-samples at distance |
| Per-pass GPU timing | E4 (proposed) | E5 LOD thresholds, D4, all budget work |

## Cross-track integration contracts

- **`froxel_inject_frag` is the single agreed integration point** for A1's clustered light data —
  local-light scattering with it, today's sun+spot coverage without it.
- Fog/cloud ambient defaults to sky-derived values. **A4 has landed and this swap was deliberately
  NOT taken**: `froxel_inject` could now trade its ambient term for one GI-volume tap, but doing so
  changes fog output and would have dragged the fog golden into 9.7's gates. It is a genuine
  one-function substitution and remains available as its own follow-up -- the volume, its uniforms
  and `giSampleIrradiance` are all in place.
- Cluster Z-slicing constants live in one shared header (`include/lights_ubo.glsl`) so froxels reuse
  the same exponential slicing.
- All new screen-space lighting passes size off render res (`engine_render_size`), never post res —
  they inherit the TAAU speedup for free.
- Sky changes propagate into DDGI via probe re-capture + hysteresis (no coupling); optional
  "sky dirty → raise probe budget" hook exposed from `gi_volume.h`.

## Verification policy (every feature, no exceptions)

1. **Off-gate:** feature disabled → **0 differing pixels** (`magick compare -metric AE`) vs the
   raiden cross-build baseline (`-x -f 120 --no-springs --no-auto-exposure -E 1.0`). Measure the
   same-build noise floor first, per CLAUDE.md.
2. **On-reference:** deterministic headless golden committed per feature (fixed frame count converges
   temporal effects; static-jitter paths keep headless byte-deterministic). TAAU alone uses a PSNR
   threshold (≥32 dB vs native) since reconstruction differs by design.
3. Every feature gets a CLI flag in the render app (`parse_args` pattern) + an ImGui toggle
   (`igCheckbox` bound to Engine/PostFX field pattern).
4. New test content needed along the way: `--area-light` CLI flag (A2), ~~cornell-box GLB~~ **(A4:
   shipped as `assets/cornell_box.gltf` plus a `cornell_leak.gltf` variant, both from one generator
   -- `.gltf` with an embedded base64 buffer, matching every other fixture, not `.glb`)**,
   curvature-sweep GLB (B3), bokeh-chart GLB (B5), low-sun fog/cloud goldens (B1/B9/B2).

## Execution workflow

1. **This master plan → `specs/aaa-rendering-roadmap.md`**, committed on a new branch
   `aaa-rendering-roadmap` created off `master` (user-confirmed; the uncommitted
   `remote-build-orchestration` work stays untouched in the working tree).
2. **Per item, later:** its own subplan session → spec in `specs/` → feature branch → implement →
   verify (off-gate + on-reference) → merge. Foundations land inside their owner's branch.
3. Tier order as above; 5↔6 swappable; Tier 3 items were parked sketches whose subplans got written
   only when scheduled, and **Tier 4 items are the same** — a row here is a hypothesis with an effort
   guess, and the subplan session is where it gets checked against the code before any of it is
   believed.

## Files most touched across the roadmap

- `cetra/shaders/pbr_frag.glsl` — A1 loop restructure, A2 LTC branch, A4 GI ambient, B3 skin diffuse
- `cetra/src/postfx.c/h` — A3, A5, B1, B4, B5 (pass order, targets, temporal)
- `cetra/src/render.c` — A1 (delete per-node upload), unit assert chain, material uniforms
- `cetra/src/engine.c` — G-buffer table, frame loop hooks (A4 probe updates, B2 cloud pass)
- `cetra/src/sky.c/h` — B9 aerial perspective + sky-published fog colors, B2 clouds
- `cetra/src/probe.c` — A4 capture-core extraction
- `apps/render/src/render.c` — CLI flags for every feature

Tier 4 adds three that Tiers 1-3 barely touched:

- `cetra/src/shadow.c` — C1 caster filtering + the transmittance resolve (the hair exclusion at
  `shadow.c:485` is C1's starting point)
- `cetra/src/render.c` — E5 submission: instanced draws, LOD selection, draw sorting; D1 decal binding
- `cetra/shaders/tonemap_frag.glsl` — E1 dither, E2 LUT (both at the very end of the chain, where the
  colour is already a display-referred scalar)
