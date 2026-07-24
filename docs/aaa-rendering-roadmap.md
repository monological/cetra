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
- **PBR texture-unit budget is nearly saturated**: 16-unit GL floor; in `pbr_frag.glsl` only **unit 7
  is truly free**, unit 9 (reflectance) is reserved-but-unsampled, unit 14 (skybox) is not sampled by
  pbr_frag. `_Static_assert` chain at `cetra/src/render.c:34-47`.
- **No UBOs exist anywhere** — all uniforms are by-name `glUniform*` via `UniformManager`.
  Light upload = `uniform Light lights[64]` (21 components/light, 13 GL calls/light, re-uploaded per
  program switch per node; `_update_program_light_uniforms` `cetra/src/render.c:91-153`).
  Per-node k-nearest light selection: `get_closest_lights` (`cetra/src/scene.c:386-447`).
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
  sun move); `sky_bake()` drives env cube → IBL → key light together. No aerial perspective.
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
- **unit 7** → `ltcMatTex` (LTC M-inverse LUT)
- **unit 9** → `ltcAmpTex` (LTC magnitude/Fresnel LUT — repurposes the reserved-unsampled reflectance unit)
- **unit 14** → `giAtlasTex` (DDGI octahedral atlas, sampler2D — legal since sampler units are
  per-program and pbr_frag never sampled the skybox cube)
- Contact shadows + bent-normal spec-occ consume **zero** pbr_frag units (postfx-only).
- Clustered forward consumes **zero** units (UBOs, not data textures).
- Pre-integrated skin uses an **analytic fit, no LUT** — partly to keep this ledger intact. Its
  documented LUT fallback would collide with the DDGI atlas on unit 14; if the fit ever disappoints,
  the fallback packs into shared atlas space instead (flagged in both subplans).

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
when clustered lands — one-line layout append). v1 limits: single-sided, no area shadows,
clearcoat/sheen skip the area branch. Karis sphere-widening untouched (area lights bypass it).
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

### A4. DDGI-style irradiance probe volume, rasterized (Majercik 2019 adapted) — Effort XL
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

### A5. Bent-normal specular occlusion from GTAO (Jimenez 2016) — Effort M
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
`glBlendFunc(GL_ONE, GL_SRC_ALPHA)` idiom (B9 adds its aerial tap here later). Kills fog half-res
buffer/history/tent-upsample. Old fog kept behind `fog_volumetric` toggle for one release. Fog
sun/ambient stay **app-set** in this item — moving them to sky-published lands in B9, so the froxel
pass keeps a clean A/B against the old screen-space pass (changing the color source at the same time
would confound the parity gate).
New: `volume_vert/geo.glsl`, `froxel_inject/integrate/composite_frag.glsl`,
`include/froxel.glsl`. CLI: `--fog-volumetric[=0]`.
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

### B3. Pre-integrated skin shading (Penner 2011) — Effort S
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

### B4. TAAU temporal upscaling (Karis-style) — Effort L
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
route through `postfx_resize` + full history invalidation; no per-frame dynamic resolution v1.
New: `taau_resolve_frag.glsl`. CLI: `--render-scale 0.67`.
**Owns foundations:** the render-res/post-res split — bokeh, lens flare, tonemap inherit it.
**Depends on:** sequence after B1 so the froxel composite is written against the post-res
convention once.

### B5. Bokeh depth of field (Guerrilla-style gather) — Effort M
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
card tips (B8). **Park until Tier 2 lands.**

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
New: `aerial_lut_frag.glsl`. CLI: `--aerial[=0]`.
**Owns foundations:** none (reuses B1's `create_texture_3d` + layered volume draw).
**Depends on:** B1's `create_texture_3d` + volume-draw machinery (hard).

## Sequencing — tiers & rationale

**Tier 1 — the AAA leap (environments):**
| # | Item | Effort | Why here |
|---|------|--------|----------|
| 1 | A1 Clustered forward | L | **DONE** (spec 9.1). The disruptive light-pipeline rewrite goes first so every later item edits the final loop + UBO layout once. NB the CPU-cost claim went unvalidated — see 9.1's as-built notes. |
| 2 | A2 LTC area lights | M | **DONE** (spec 9.2). Signature environment feature; contained M on the now-stable loop; must precede DDGI so probes capture area-lit rooms. |
| 3 | A3 Contact shadows | S | **DONE** (spec 9.3). Post-only depth march along the key light; no shadow.c changes (postfx already had the light dir + view matrix). Default off. |
| 4 | B1 Froxel volumetric fog | L | **DONE** (spec 9.5). Owns the 3D-texture + volume-draw machinery all volumetrics need; consumes A1's light list on day one. Shipped with one-layer-per-draw slices, not the layered-GS sketch — that matches the cascade/mask-array/cube-face idiom and needs no geometry shader. |
| 5 | B9 Aerial perspective | S | Cheap once B1's 3D machinery exists; completes the outdoor atmosphere and lands the sky-published fog colours B1 deferred. |
| 6 | A4 DDGI probe volume | XL | The "why does UE5 look like that" answer; after A1+A2 so rasterized captures see clustered lights and LTC panels. |
| 7 | B2 Volumetric clouds | XL | Biggest sky payoff; needs B1's 3D helpers; landing after A4 means probe captures include clouds automatically. |

(6↔7 are swappable — no hard dependency either way.)

**Tier 2 — image quality & performance:**
| # | Item | Effort | Why here |
|---|------|--------|----------|
| 8 | A5 Bent-normal spec-occ | M | Near-free on the existing GTAO; hands DDGI a better sampling direction as a follow-up toggle. |
| 9 | B5 Bokeh DoF | M | Self-contained palate cleanser between the big lifts. |
| 10 | B4 TAAU | L | After B1's composite settles so the post-res migration happens once; funds Tier 1 at 4K. |
| 11 | B3 Pre-integrated skin | S | Character tier begins; S effort, zero infra. |

**Tier 3 — polish & late-tier (parked until Tiers 1-2 land):**
| # | Item | Effort |
|---|------|--------|
| 12 | B6 Moment-based OIT | L |
| 13 | B7 Lens flare / finishing | S/M |
| 14 | A6 Moment shadow maps | L |
| 15 | B8 Hair | XL |

## Foundations ownership (just-in-time)

| Foundation | Owner | Later consumers |
|---|---|---|
| UBO machinery (`ubo.c/h`, std140, binding registry) | A1 | froxel light data, any bulk constants |
| CPU light culling (`light_cluster.c/h`) + shared exponential Z-slicing constants | A1 | B1 froxel injection |
| Float-texture-from-C-array (`create_texture_2d_float`) | A2 | future LUTs (MSM quantization, etc.) |
| `Light.up` orientation frame | A2 | PackedLight layout |
| Key-light view-space direction publication to postfx | A3 | future light-aware post passes |
| Octahedral encode/decode include | A4 | bent-normal storage, oct G-buffer normals |
| Capture-at-position helper (extracted from probe.c) | A4 | future probe features |
| Bent normal in the AO chain | A5 | SSGI directionality, SSR occlusion, DDGI sampling |
| `create_texture_3d` + layered-GS volume draws + `include/froxel.glsl` | B1 | B9 aerial perspective, B2 clouds, future volumetrics |
| CPU 3D noise (`noise_worley3`, Perlin-Worley packing, threaded bake) | B2 | ground fog detail, media |
| Render-res/post-res split (`post_width/post_height`) | B4 | B5, B7, tonemap |

## Cross-track integration contracts

- **`froxel_inject_frag` is the single agreed integration point** for A1's clustered light data —
  local-light scattering with it, today's sun+spot coverage without it.
- Fog/cloud ambient defaults to sky-derived values; when A4 lands, `froxel_inject` swaps its ambient
  term for one GI-volume tap (one function substitution).
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
4. New test content needed along the way: `--area-light` CLI flag (A2), cornell-box GLB (A4),
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
