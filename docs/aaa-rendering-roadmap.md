# Cetra AAA Rendering Roadmap — Master Plan

## Context

Cetra is a C11 forward-PBR engine on a strict GL 4.1 core ceiling (macOS — no compute, no SSBO).
It already has a strong screen-space stack (TAA, GTAO+SSGI, hi-Z SSR, WBOIT, Hillaire sky, CSM+PCSS,
split-sum IBL, parallax-corrected probes). The gap to "AAA look" is not more post effects — it is
**global illumination fidelity, light-count scaling, and volumetric atmosphere**, all architectural.

This master plan is a tiered roadmap of SIGGRAPH-grade features, each feasible as fullscreen raster
passes + LUTs + probe bakes on GL 4.1. The plan will be committed to `specs/` as the umbrella spec;
each tier item later gets its own subplan (feature branch + spec) before implementation.

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

### A6. Moment shadow maps (Peters & Klein 2015) — Effort L — parked sketch
Filterable 4-moment shadows: RGBA16F array beside the DEPTH24 CSM array, moment transform +
separable pre-blur, Hamburger 4MSM single-tap in `calculateShadow` via `include/msm.glsl`. Costs 4×
shadow memory and PCSS contact-hardening; opt-in `--msm`, PCF/PCSS stay default. Bonus: gives fog
single-tap prefiltered shadow sampling. **Do not schedule until the rest of Track A lands.**

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
it. That one stands, and B3.1 is what recovers it.

The second — **total scatter is not resolution-independent** — was **fixed by 11.14, and B3 was not
the cause.** The composition rule was blamed for treating the two widths as interchangeable; the
actual fault was the screen-space blur capping its kernel in PIXELS, so the delivered world width
fell as 1/height. Moving that cap into world-scatter-per-unit-depth makes the projection cancel
algebraically. `deficit = sqrt(1 - k^2)` was measured against Penner's integral afterwards and
kept.

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

### B3.1. Shadow-penumbra scattering (Penner's second LUT) — Effort M
The half of Penner 2011 that B3 deliberately left out, and the one that recovers what B3 could not.
B3 owns the *attached* terminator; a cast shadow's edge is still a hard multiply outside the
diffuse, so on a convex character the two boundaries coincide and the shadow wins. Penner's second
table pre-integrates the diffusion profile against the shadow's own penumbra gradient, which is
what lets light bleed across a shadow edge instead of being multiplied to zero at it. Needs the
penumbra width the shadow lookup already estimates for PCSS, so the input is largely in hand.
**Owns foundations:** none. **Depends on:** B3 (shipped), PCSS (shipped).

### B3.2. Skin under an area light — Effort M
Skin lit by an area/LTC panel gets **no subsurface scattering at all** today: the SSS accumulation
path `continue`s past panels, so `sssDiffuse` never sees them. Self-consistent rather than broken —
LTC integrates the whole panel analytically and has no single `L` to feed a diffusion profile — but
a softbox portrait is *the* canonical skin setup, so the gap is exactly where the feature matters
most. Same shape of gap as the IBL one: B3 also does nothing on an IBL-lit face, since the ambient
tap has no `L` either. Both want a representative direction and a solid-angle-aware width.
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

### B6. Moment-based OIT (Münstermann 2018) — Effort L — parked sketch
**Power moments** (not trig — avoids complex arithmetic in GLSL 330): 4 power moments RGBA16F +
optical depth b0, new attachments on the OIT FBO. Transparents draw **twice** (moments bracket, then
color bracket weighted by reconstructed transmittance — the `engine_begin/end_oit_pass` bracket
splits in two; main engine-side surgery). Hamburger 4-moment solver in `include/mboit.glsl`;
`oit_moments` toggle keeps WBOIT fallback; off → 0px. ~+16 MB at 4× MSAA 1080p. Best customer: hair
card tips (B8). ~~**Park until Tier 2 lands.**~~ Tier 2 landed with 11.13; unparked.

### B7. Lens flare / cinematic finishing — Effort S/M — sketch
Quarter-res Chapman-style ghost chain off the bloom bright buffer (scaled/flipped UV ghosts +
chromatic offset + lens-color gradient + halo + starburst), additive pre-tonemap composite, optional
dirt mask. Finishing in tonemap uniforms: frame_index-hashed deterministic grain, vignette, edge
chromatic aberration — all default-off (tonemap output stays byte-identical). Runs at post res.

### B8. Physically based hair (Karis/Marschner) — Effort XL — LATE-tier sketch
Dedicated `hair_frag.glsl`/`create_hair_program`: three-lobe specular (R/TT/TRT) from shifted
anisotropic lobes along the strand tangent (card UV V-direction), Karis azimuthal scale factors,
absorption color, wrapped-diffuse faked multiple scattering, CSM as-is. Material: `hair_roughness`,
`hair_shift`, `hair_tint`, `hair_backlit` + hair program flag. Transparency: **alpha-to-coverage on
the existing 4× MSAA** (masked-foliage precedent); soft tips opt into MBOIT. Import needs: hair flag
from glTF extras, card tangents derived at import, flow map as a mask_array layer. Test content: a
card-based groom (check raiden's hair; else CC0 asset). Last by decree.

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
and 11.2 (below). **Tiers 1 and 2 are complete**, Tier 2 closing with B3 pre-integrated skin
(11.13). Tier 3 is unparked.

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
| 12 | B3 Pre-integrated skin | S→**M** | **DONE (11.13):** opt-in `curvature_scale`, 16-row const table, `.cscn` material overrides. Not zero infra: fit tool, fixture, 3 gates, golden. Shadow limits the wrap (→ B3.1). **Inert for realistic content since 11.14** fixed the blur it was compensating for. |
| 12.1 | SSS blur width | M→**L** | **DONE (11.14):** the blur capped its kernel in PIXELS, so delivered world scatter fell as 1/height — 2.5% of its low-res value at 4K, a defect live since SSS shipped and named in `specs/4.12-sss.md` with nothing behind it. Fixing the units alone was correct arithmetic and an unshippable image: a resolution-independent world width means an unbounded PIXEL width, and 12 fixed taps then resolve individually as rings. Replaced the separable blur with a scale-space pyramid — one trilinear tap per profile Gaussian per channel — so reach stops depending on the tap budget. Now 2.5% drift across a 4x sweep, and 91% of Penner's integral against the old kernel's 129%. New `sss-scale` and `sss-band` gates; first SSS-ON golden. |

**Tier 3 — polish & late-tier (unparked: Tiers 1-2 have landed):**
| # | Item | Effort | Why here |
|---|------|--------|----------|
| 13 | SSS is dead in procedural scenes | S | **Bug, found in 11.14.** `scene_has_subsurface()` walks `scene->materials`, which only `import.c` ever fills, so a scene that builds materials in code never runs the SSS pass. `apps/tree` authors `subsurface` 0.6 / 0.45 and two profiles and has never rendered any of it. Fixing it switches subsurface ON for the tree for the first time, so it wants a look pass, not just a one-line fix. |
| 14 | B3.1 Shadow-penumbra scattering | M | Recovers what B3 measured it could not do: the cast shadow owns the terminator on a convex character. Cheapest real gain in the character tier, and the PCSS penumbra estimate is already computed. |
| 15 | B3.2 Skin under an area light | M | Skin gets no SSS at all under an LTC panel today, and a softbox portrait is the canonical skin setup. Shares its shape with the IBL gap. |
| 16 | B6 Moment-based OIT | L | |
| 17 | B7 Lens flare / finishing | S/M | |
| 18 | A6 Moment shadow maps | L | |
| 19 | B8 Hair | XL | Wants B3.1 and B3.2 settled first — hair shares the shadow-scattering and area-light problems and is far more expensive to iterate on. |

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
3. Tier order as above; 5↔6 swappable; Tier 3 items are parked sketches whose subplans get written
   only when scheduled.

## Files most touched across the roadmap

- `cetra/shaders/pbr_frag.glsl` — A1 loop restructure, A2 LTC branch, A4 GI ambient, B3 skin diffuse
- `cetra/src/postfx.c/h` — A3, A5, B1, B4, B5 (pass order, targets, temporal)
- `cetra/src/render.c` — A1 (delete per-node upload), unit assert chain, material uniforms
- `cetra/src/engine.c` — G-buffer table, frame loop hooks (A4 probe updates, B2 cloud pass)
- `cetra/src/sky.c/h` — B9 aerial perspective + sky-published fog colors, B2 clouds
- `cetra/src/probe.c` — A4 capture-core extraction
- `apps/render/src/render.c` — CLI flags for every feature
