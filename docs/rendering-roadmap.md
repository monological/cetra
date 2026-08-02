# Cetra Real-Time Rendering Roadmap

A working roadmap of advanced real-time rendering techniques for Cetra, ranked by
leverage, with a GL-4.1-feasible implementation approach for each. Grounded against
the current code (file references throughout). This is a planning doc — turn sections
into tasks as you go.

**Progress:**
- Tier 1 — **TAA + velocity buffer (4.1) — ✅ shipped** (M0–M3, merged to `master`). Laid down
  the velocity G-buffer + history reprojection that later temporal work builds on.
- Tier 2 — **GTAO (4.2) — ✅ shipped** (2023 visibility bitmask, replacing SSAO). Added linear
  view-Z reconstruction (aux buffer `.z`, fixing grazing-angle depth banding) and temporal AO
  accumulation reusing the velocity buffer. The unset mask bits are the substrate for SSGI (4.3).
- Tier 2 — **SSGI (4.3) — ✅ shipped** (M0–M3 on the `ssgi` branch). One-bounce indirect diffuse
  riding the GTAO bitmask sweep; albedo G-buffer + additive pre-bloom composite; RGB temporal
  accumulation (YCoCg clamp, inverse-luma feedback) + 3-iteration edge-aware à-trous denoise.
  Off by default (`--ssgi` / GUI). Same branch also shipped auto-exposure with eye adaptation.
- Tier 2 — **Reflection probes (4.4) — ✅ shipped** (M0–M4 on the `probe` branch). One-shot scene
  capture into a prefiltered local cubemap (full pipeline: shadows, IBL, skybox), consumed as
  parallax-corrected local specular (Lagarde AABB proxy, rebinding the prefilter slot — the
  fragment stage is at the sampler limit) and as the SSR miss fallback (invView threaded into
  postfx). `--probe` / `--probe-pos` / `--probe-debug` + GUI group. Byte-identical off; capture
  and consumption run-to-run deterministic. Fixed two latent engine bugs found along the way:
  the prefilter chain was mipmap-incomplete (env specular sampled as black everywhere) and the
  BRDF LUT baked with blending on (undefined alpha made it nondeterministic per run).

- Tier 3 — **Screen-space refraction (4.9) — ✅ shipped** (`refraction` branch). Real
  see-through glass per KHR_materials_transmission/volume: mid-frame opaque resolve into a
  mipped target, transmissive late-pass draws sampling it through the refracted ray
  (thickness bends, roughness blurs, thin glass per spec). Generated glass fixture as the
  end-to-end test; `--no-refraction`. Closes the cheap-wins arc.
- Tier 4 — **AgX tonemap (4.16) — ✅ shipped** (`agx` branch). Third tonemap mode via the
  single toneSelect dispatch; highlights roll to white without hue skew, shadows lift
  slightly vs ACES. GUI three-way combo + `--tonemap` flag. Default remains Neutral.
- Tier 3 — **Bloom pyramid (4.13) — ✅ shipped** (`bloompyramid` branch). Jimenez dual-filter:
  13-tap downsample chain + additive tent upsample on one packed-float mip pyramid (hi-z
  build idiom). The tight screen-hugging halo becomes a wide ring-free falloff at the same
  energy; `--no-bloom` added; blur_iterations retired.
- Tier 3 — **Energy compensation (4.8) — ✅ shipped** (`energycomp` branch). Multi-scatter GGX
  via the existing split-sum LUT (Ess = A+B, specular × `1 + F0*(1/Ess - 1)` in both analytic
  and IBL paths — no new LUT, no new sampler). Rough metals recover their brightness; default
  on with `--no-energy-comp` / GUI escape. First of the cheap-wins run: 4.8 → 4.13 → 4.16 → 4.9.
- Tier 2 — **Volumetric fog (4.5) — ✅ shipped** (M0–M3 on the `fog` branch) as a screen-space
  shadowed raymarch rather than the froxel grid (single key-light rig + no transparents = the
  grid's wins never activate; froxels remain the upgrade path if local lights arrive). Half-res
  march to each pixel's aux linear-Z (dome-scaled far for sky), exponential height density with
  energy-conserving per-segment integration, per-caster shadowed in-scatter (hoisted HG phase)
  via `shadow_publish_to_postfx`, composited pre-bloom as `scene·T + inscatter`. Off by default
  (`--fog` / GUI); byte-identical off; deterministic headless; temporally accumulated under TAA.

- Tier 2 — **Cascaded shadow maps (4.6) — ✅ shipped** (M0–M4 on the `csm` branch). Runtime
  cascade count (1–3) with count-strided layers so count 1 stays byte-identical to the classic
  single map; λ=0.75 splits, sphere-of-slice fit + texel snap (orbit-stable), hard selection in
  pbr/catcher, per-step fall-through in the fog march, per-cascade PCSS geometry and
  bias/kernel normalization to the scene-fit reference. Viewer defaults to 3;
  `--shadow-cascades 1` bridge, `--csm-debug` tint view. See §4.6 for the shipped shape.

- Tier 2 — **Physically-based sky (4.7) — ✅ shipped** (M0–M5 on the `sky` branch). Hillaire
  transmittance + multi-scatter + sky-view LUTs (no compute) rendered into the existing IBL
  environment cubemap, so a movable sun drives skybox/IBL/prefilter/probe/fog and a coupled
  key light + shadows through one `sky_update_sun` path; `sky_apply_sun_to_light` owns the
  sun→light policy (transmittance tint, below-horizon fade). GUI elevation/azimuth/disc
  sliders re-bake live. Off by default (`--sky`; `-e` conflict is a hard error); aerial
  perspective deferred (invisible at prop scale; `--fog` covers depth haze). See §4.7.

- Tier 3 — **Clearcoat (4.10) — ✅ shipped** (`clearcoat` branch). glTF KHR_materials_clearcoat: a
  second GGX lobe (F0=0.04) over the base, energy-conserving (coat Fresnel attenuates the base),
  analytic + IBL (reuses prefilter/BRDF LUT, no new sampler), with a coat normal map for
  orange-peel/carbon-fibre. Shipped on a **sampler-array foundation**: the seven scalar masks
  collapsed into one `sampler2DArray` (17→11 declared samplers, engine units relocated in-spec),
  freeing units the coat normal rides. Off by default (`--no-clearcoat`; factor 0 = byte-identical).
  See §4.10.
- Tier 3 — **KHR_materials_specular + _sheen (4.10.1) — ✅ shipped** (`materials-sheen-specular`
  branch). Activated the two reserved-but-dead material slots: specular re-parameterizes the
  dielectric F0 (color tint + weight, folded into F0 for byte-identity), sheen adds a Charlie
  retroreflective cloth lobe (velvet/satin, analytic + IBL). Zero new samplers. Off by default
  (`--no-specular` / `--no-sheen`; non-carrying materials byte-identical). See §4.10.1.

Next up: 4.18 Forward+ (clustered shading), the last Tier-4 item. (4.11 POM, 4.15 motion blur, 4.12 SSS, 4.17 OIT shipped.)

---

## 0. Platform constraints (read first — they shape every choice)

Requested context (`engine.c:231-235`): **OpenGL 4.1 core**, forward-compatible.
Runtime on this machine reports:

```
OpenGL 4.1 Metal - 90.5 | GLSL 4.10
Renderer: Apple M1 Max | Apple
```

4.1 is Apple's hard OpenGL ceiling (Intel and Apple Silicon alike; Apple Silicon runs
GL *on top of Metal*, but exposes only 4.1). This is the binding constraint.

**Not available in GL 4.1** — every technique below must avoid these:
- **Compute shaders** (need GL 4.3) → no compute-based light culling, prefix sums, or histogram passes.
- **SSBOs** (need GL 4.3) → large/variable data goes through UBOs (small, fixed) or **texture buffers** (`samplerBuffer`).
- **Bindless textures** → material texture sets must be atlased or bound per-draw.
- **Hardware ray tracing** → doesn't exist in OpenGL at any version (RT is Vulkan/DXR/Metal only).

**Available in GL 4.1** — lean on these:
- **Tessellation shaders** (since 4.0) → real displacement mapping is on the table.
- **Texture buffer objects** (`samplerBuffer`) and fp32 textures / MRT / UBOs / transform feedback / `GL_TEXTURE_RECTANGLE`.

**Beyond OpenGL on this Mac** (new-backend projects, not version bumps): **Metal**
(compute + ray tracing; HW-accelerated RT only on M3+, the M1 Max runs Metal RT on
general compute units) and **Vulkan via MoltenVK**. GL↔Metal interop is possible via
**IOSurface** (`CGLTexImageIOSurface2D` ↔ `MTLTexture`). See the appendix.

**Consequence:** everything here is described as a **fragment-shader / multi-pass**
technique. That is fine — all of it predates compute; the compute era just made some of
it faster, not possible-only-then.

---

## 1. Current pipeline architecture (grounded)

- **Forward shading** with **N-nearest-lights-per-object** selection, ~75-light GPU cap
  (`render.c`, `common.h`). No deferred/clustered path.
- **MSAA** framebuffer (**runtime 1× or 4×** — `engine.c`, `set_engine_msaa_samples`), scene
  renders into it; optional **SSAA** via `ss_scale` supersampling of the whole scene+post
  target, box-downsampled at tonemap.
- **G-buffer-lite**: MRT attachments carry **view-space normals + roughness** (attachment 1,
  consumed by SSAO/SSR) and a **per-pixel velocity buffer** (attachment 2, consumed by TAA);
  `engine_set_scene_draw_buffers` toggles them per pass. The single most important existing
  hook for new screen-space effects.
- **Temporal AA** (`taa_resolve_frag`, runtime-selectable, default on at 1× MSAA): sub-pixel
  jitter + velocity-reprojected, neighborhood-clamped history. Off in headless. See 4.1.
- **Post chain** (`postfx.c`, fullscreen passes off `post_vert.glsl`): resolve MSAA →
  **TAA resolve** (if enabled) →
  SSAO (half-res, `ssao_frag` + `ssao_blur_frag`) → SSR (`ssr_frag` + `ssr_composite_frag`)
  → bloom (bright pass + **2-iteration separable Gaussian at half-res**, `R11F_G11F_B10F`;
  *not* a pyramid) → tonemap (`tonemap_frag`: Passthrough / ACES / Khronos PBR Neutral,
  **manual** exposure) → finishing (vignette, grain, sharpen, color grade) → DoF
  (`dof_coc` / `dof_blur` / `dof_composite`, with autofocus).
- **Shading model** (`pbr_frag.glsl`): Cook-Torrance GGX + **anisotropic GGX** +
  **wrap-lighting subsurface** + back-transmission + **sheen** + **thin-film iridescence**
  + IOR, with **PCSS** soft shadows sampled inline.
- **Shadows**: a **single** shadow map (`shadow_depth_vert/frag`) + PCSS. **No cascades.**
- **IBL**: full split-sum (`ibl.c`, `ibl_equirect`/`irradiance`/`prefilter`/`brdf` shaders).
- **Animation**: GPU skinning (`pbr_skinned_vert`), spring-bone secondary motion; plus
  shadow catcher and ground-projection skybox.

---

## 2. Feature inventory (what already exists)

| Present | Where | Note |
|---|---|---|
| PBR Cook-Torrance (metallic-rough) | `pbr_frag.glsl` | Core BRDF |
| Anisotropic GGX | `pbr_frag.glsl:213` | brushed metal / hair |
| Subsurface (wrap-lighting + back transmission) | `pbr_frag.glsl:230` | approximation, upgrade candidate |
| Sheen | `pbr_frag.glsl` | fabric |
| Thin-film iridescence | `pbr_frag.glsl:167` | soap/oil/coatings |
| IOR | `material.h:49` | |
| IBL (split-sum) | `ibl.c`, `ibl_*` | equirect→cubemap, irradiance, prefilter, BRDF LUT |
| PCSS soft shadows | `pbr_frag.glsl`, `shadow.c` | single map |
| **GTAO** (ground-truth AO) | `gtao_frag`, `ao_accum_frag`, `postfx.c` | ✅ 4.2 — 2023 visibility bitmask, linear-Z reconstruction, temporal accumulation |
| SSR | `ssr_frag`, `ssr_composite` | screen-space only |
| Bloom | `bloom_bright`, `bloom_blur` | bright + 2-iter gaussian, half-res |
| Tonemap: ACES / Khronos Neutral / Passthrough | `tonemap_frag`, `postfx.h:19` | manual exposure |
| DoF + autofocus | `dof_*` | CoC gather |
| Vignette / grain / sharpen / color grade | post chain | finishing |
| SSAA supersampling | `engine.c` `ss_scale` | |
| Geometric specular AA | `engine.c` `specular_aa_strength` | partial specular aliasing fix |
| **TAA + velocity buffer** | `taa_resolve_frag`, `postfx.c`, `render.c` | ✅ 4.1 — jitter + YCoCg clamp + Catmull-Rom + inv-luma blend |
| **Motion-vector G-buffer** | `pbr_frag` `VelocityOut`, `RENDER_MODE_VELOCITY` | rigid + skinned deformation velocity |
| **Runtime-selectable AA** | `set_engine_msaa_samples`, `set_engine_taa_enabled` | None / MSAA / TAA / Both; default TAA-only 1× |
| GPU skinning + spring bones | `pbr_skinned_vert`, `springbone.c` | + affine-packed previous-pose for velocity |
| Shadow catcher, ground-projection skybox | `engine.c`, `scene.c` | |

**Absent** (the subject of this doc): reflection & irradiance probes;
volumetrics/fog; cascaded shadow maps; physically-based sky; clearcoat; screen-space refraction;
parallax occlusion mapping; multi-scatter GGX energy compensation; AgX; motion
blur; OIT; Forward+. (TAA / velocity buffer — 4.1; GTAO — 4.2; SSGI one-bounce GI — 4.3;
auto-exposure with eye adaptation — **all now present**.)

---

## 3. Roadmap & dependency ordering

The ordering is not by visual impact alone — it's by **what unlocks what**. TAA is
infrastructure: it introduces the **velocity buffer** and **history reprojection** that
half the other techniques depend on to be affordable.

```
              ┌────────────────────────────────────────────┐
    ✅ DONE    │ 1. TAA + motion-vector (velocity) buffer    │  (infrastructure)
              └───────┬───────────────────────┬─────────────┘
                      │ enables temporal        │ velocity buffer
                      │ denoise + upsample       │ reused by:
        ┌─────────────▼─────────┐   ┌───────────▼──────────┐
 ✅ GTAO │ SSGI (1-bounce GI) ✅  │   │ Motion blur           │
        │ (noisy → TAA cleans)  │   │ Temporal upsampling   │
        └───────────────────────┘   └──────────────────────┘
   (GTAO's unset visibility-mask bits feed SSGI's radiance gather)

   Independent of TAA (can start any time):
     • Cascaded Shadow Maps      • Reflection probes (SSR fallback)
     • Volumetric fog            • PB sky/atmosphere
     • Multi-scatter energy comp • Screen-space refraction
     • Clearcoat • POM • AgX • Auto-exposure • Bloom-pyramid • OIT
     • Forward+ (big; do last)
```

**Suggested sequence:** ~~TAA+velocity~~ ✅ → ~~GTAO~~ ✅ → ~~SSGI~~ ✅ → ~~reflection probes~~ ✅ →
~~volumetric fog~~ ✅ → ~~cheap material/correctness wins (energy comp, refraction, AgX, bloom
pyramid)~~ ✅ → ~~CSM~~ ✅ → the rest as appetite allows. Forward+ only if you
truly need many lights.

Effort key: **S** ≈ days · **M** ≈ 1–2 weeks · **L** ≈ 3+ weeks.

---

## 4. Techniques

### Tier 1 — Infrastructure (do first) — ✅ **complete**

#### 4.1 Temporal Anti-Aliasing + motion-vector buffer  — **M**, highest leverage — ✅ **DONE** (merged to `master`)
- **What.** Jitter the projection sub-pixel per frame; reproject last frame's color via a
  per-pixel velocity buffer; blend with neighborhood-clamped history.
- **Why.** (1) High-quality AA that specifically kills the **specular/shading aliasing**
  your geometric spec-AA only partly addresses. (2) It's the substrate for temporal
  denoising (SSAO/SSR/SSGI at 1 noisy sample/frame), temporal **upsampling** (render at
  ~60–70% res, reconstruct → large perf headroom), and motion blur.
- **Current state.** **Shipped in 4 milestones (M0–M3).** Runtime-selectable AA as two
  independent toggles → all four combos **None / MSAA / TAA / Both**
  (`set_engine_msaa_samples`, `set_engine_taa_enabled`). The windowed render app defaults to
  **TAA-only at 1× MSAA** (4× MSAA is bandwidth-heavy through GL-on-Metal on the M1); headless
  forces **TAA off / MSAA 4×** so screenshots stay deterministic.
- **What was built.**
  - **Velocity G-buffer** on `GL_COLOR_ATTACHMENT2` (`RGBA16F`, motion in `.xy`), written only
    in the opaque PBR pass and resolved to a single-sample texture — mirrors the existing
    normals-resolve blit (`postfx.c`, `resolve_color_attachment`). *(Still `RGBA16F`, not the
    planned `RG16F`; velocity only uses `.xy`, so a narrower format is a pending perf nit.)*
  - **Sub-pixel jitter**: Halton(2,3) indexed by `total_frames`, applied to a **local** draw
    projection in `render_current_scene`; `engine->view_proj` stays un-jittered for frustum
    culling and motion vectors (computed once/frame). No jitter in headless.
  - **Previous-frame data**: `engine->prev_view_proj` (snapshotted at frame end), per-node
    `prev_global_transform` (`scene.c`), and skinned **previous bone matrices** packed as **3
    affine rows/bone** (12 floats vs 16) so a second full set fits the `4096`-component vertex
    uniform budget alongside `boneMatrices[128]` — `animation_snapshot_prev_pose`, once/frame.
  - **TAA resolve pass** (`taa_resolve_frag.glsl`, after MSAA color resolve, before SSAO):
    reproject history by velocity, **YCoCg 3×3 neighborhood clamp**, **Catmull-Rom** history
    sampling (sharpness), and an **inverse-luminance-weighted blend** (feedback 0.9) that
    suppresses specular fireflies the neighborhood clamp can't remove at 1× MSAA. Ping-pong
    `taa_history_texture[2]` (`RGBA16F`), blit back into `hdr_fbo` so downstream post is
    unchanged.
  - **Velocity debug view** (`RENDER_MODE_VELOCITY`, `renderMode==9`): motion vectors ×400,
    static geometry resolves to flat 0.5 gray.
- **Dependencies.** None. **Is** a dependency for 4.3, 4.4, 4.15 — the velocity buffer and
  history reprojection are now available for them.
- **Gotchas hit.** Skinned-mesh velocity was the fiddly part (affine-row packing to fit the
  uniform budget); a naive second `mat4[128]` won't link. Specular fireflies **accumulate**
  under TAA at 1× MSAA (the clamp bounds edges, not lone bright pixels) — the
  inverse-luminance blend was the fix. Skybox / translucent / shadow-catcher write zero
  velocity (treated static) → minor smear under fast motion, an accepted v1 limitation.
- **Refs.** Karis, "High-Quality Temporal Supersampling" (SIGGRAPH 2014 Advances course);
  Pedersen, "Temporal Reprojection AA in INSIDE" (GDC 2016); Salvi, "TAA" notes.

---

### Tier 2 — Biggest visual jumps

#### 4.2 Ground-truth AO / near-field GI (GTAO or Visibility Bitmask)  — **M** — ✅ **DONE** (merged to `master`)
- **What.** Horizon-based AO that computes a physically-grounded occlusion integral, with
  optional colored near-field bounce (multi-bounce GTAO).
- **Why.** A clear quality tier above classic SSAO — correct cosine-weighted occlusion,
  less haloing, and near-field color bleeding for near-free.
- **Current state.** **Shipped** — replaced SSAO with the **2023 Visibility Bitmask** GTAO.
- **What was built.**
  - `gtao_frag.glsl`: per-pixel slice sweep marking each occluder's finite angular slab (front +
    a `THICKNESS` back horizon) into a 32-bit per-slice mask, mapped into the surface-normal
    hemisphere; `visibility = 1 − popcount/32` (SWAR popcount, GLSL 330). Thin occluders shadow
    only a slab, and the **unset bits are the SSGI substrate** (4.3).
  - **Linear-Z reconstruction:** the non-linear DEPTH24 buffer staircased flat surfaces into
    grazing-angle AO banding, so positions are reconstructed from a stored **linear view-Z** in
    the aux buffer's spare `.z` channel (`NEAREST`-filtered — view-Z isn't screen-linear).
  - **Temporal AO accumulation** (`ao_accum_frag.glsl`): reuses the velocity buffer to reproject
    and blend the AO over frames, with per-frame slice jitter — removes motion flicker.
  - A2C hair is excluded (its zero-normal marker → depth-derivative noise flickers).
- **Gotchas hit.** View-Z can't be linear-filtered (bends flat surfaces, mangles fine geometry
  into speckle) → `NEAREST` aux buffer. The AO-debug view amplifies sub-threshold wobble
  invisible in beauty — test the real lit scene. Known limitation: the aux MSAA color-resolve
  averages `.z` at silhouette edges (latent only in the non-default GTAO+MSAA>1 modes).
- **Refs.** Jimenez et al., "Practical Realtime Strategies for Accurate Indirect Occlusion"
  (GTAO, SIGGRAPH 2016); "Screen Space Indirect Lighting with Visibility Bitmask" (2023).

#### 4.3 Screen-Space GI (one-bounce indirect diffuse)  — **M/L** ✅ **DONE**
- **What.** Trace short rays in screen space against the depth buffer, gather radiance from
  hit pixels → one bounce of indirect diffuse (color bleeding, contact GI).
- **Why.** Your lighting is direct + IBL; there is no *local* bounce. SSGI adds the "light
  bouncing off the red wall onto the floor" that sells interiors.
- **Shipped.** Rides the GTAO visibility-bitmask sweep instead of a separate march: per
  occluder sample, the sectors it newly covers weight its lit radiance (current-frame
  `hdr_texture`, firefly-clamped) into a half-res GI target (`gtao_frag` MRT attachment 1).
  Albedo G-buffer (scene attachment 3) feeds the additive pre-bloom composite
  `(1-metal) x albedo x GI x intensity`. Denoise: RGB temporal accumulation (YCoCg
  neighborhood clamp + inverse-luma feedback, velocity-reprojected, TAA frames) then 3
  edge-aware à-trous iterations (linear-depth + normal weights). Off by default:
  `--ssgi`, `--ssgi-debug`, `--albedo-debug`, GUI toggle + intensity. AO/GI reach scales
  with scene radius (1%); sub-resolution pixels gate themselves off. SSGI-off stays
  byte-identical.
- **Deps satisfied.** TAA velocity (4.1), GTAO sweep + linear-Z aux (4.2).
- **Refs.** Ritschel et al. SSDO; Mara/McGuire deep-G-buffer GI; Silvennoinen SSGI talks.

#### 4.4 Reflection probes (SSR fallback)  — **M** — ✅ DONE
- **Shipped.** One probe, captured once at load by rendering the scene six times through the
  full pipeline (`render_current_scene` with substituted matrices; shadows + IBL + skybox
  included) into an RGB16F cubemap, GGX-prefiltered with the shared ibl toolkit (promoted from
  `ibl.c` statics). Consumed at both ends: parallax-corrected local specular in `pbr_frag`
  (Lagarde AABB proxy; the probe rebinds the prefilter unit — the fragment stage is at the
  driver's 16-sampler limit — and the box fade feathers the parallax correction), and a
  fallback in `ssr_frag` at every miss site + the faded tail of partial hits (view matrix
  threaded into `postfx_run`; premultiplied contract unchanged). Auto-placed at the scene
  center with the proxy box floor-locked to the scene bounds; `--probe`, `--probe-pos x,y,z`,
  `--probe-debug` (capture as background), GUI group with intensity/box-fade. Static capture,
  bind pose, no runtime recapture. Byte-identical off; deterministic on.
- **Found along the way.** The prefilter chain was mipmap-incomplete (no GL_TEXTURE_MAX_LEVEL
  above its 4x4 tail) so env specular sampled as black on every surface since the IBL pass
  shipped; and the BRDF LUT baked with blending enabled, feeding its undefined RG-only alpha
  into the blend equation — nondeterministic per run. Both fixed on the `probe` branch.
- **Revised after live testing (same day).** A single parallax box cannot place both a
  near-field hero model and the far dome (scene-sized box streaks the dome; dome-sized box
  ghosts the model; the model's own capture bakes grid moire). Default on dome stages is now
  **environment-only**: prefilter the global env into a floor-locked dome-sized box with the
  parallax origin at the env capture point — SSR supplies the model's reflection. Scene capture
  (2x-supersampled, camera-side auto placement) stays for interiors via `--probe-scene`. The
  fallback also exposed SSR's fixed world-unit thresholds: ray start bias now grows with view
  distance, march reach/thickness scale with the scene.
- **Later.** Multi-probe (struct is self-contained), relight, `--probe-box` override.
- **Refs.** Lagarde & Zanuttini, "Local Image-Based Lighting with Parallax-Corrected
  Cubemaps" (SIGGRAPH 2012).

#### 4.5 Volumetric fog / lighting  — **M/L** — ✅ **shipped** (`fog` branch)
- **What.** Froxel (view-frustum voxel) grid accumulating in-scattered light per cell →
  god rays, light shafts, atmospheric depth, height fog.
- **Why.** Completely absent, and it's one of the highest "mood per week" additions — it
  makes every shadowed shaft and every lit interior read volumetrically.
- **Current state.** Shipped as a **screen-space shadowed raymarch** instead of the froxel
  grid (`fog_frag.glsl` + `shadow_publish_to_postfx`): with one shadow-casting key rig and
  no transparents, the grid's wins (many lights amortized, fog on transparents, authored
  volumes) never activate, while slice-rendered 3D textures are exactly the GL-on-Metal
  driver edge that bit in 4.4. Half-res march to aux linear-Z, exponential height density,
  per-caster single-tap shadowing with hoisted HG phase, `scene·T + inscatter` composite
  pre-bloom, temporal accumulation under TAA. The froxel design below remains the upgrade
  path if local lights arrive (the composite, publish plumbing, and policy all carry over).
- **Froxel upgrade — ✅ shipped** (spec 9.5, `froxel-volumetric-fog` branch). A1's clustered
  lighting removed the "wins never activate" objection: the screen-space march cannot afford
  the light list (per light, per step, per pixel) but a volume evaluates lighting once per
  cell. Three passes over a 160×90×64 RGBA16F pair — inject+light, an O(n²) front-to-back
  integrate that reads one volume and writes the other (no read-write hazard without
  `glTextureBarrier`, GL 4.5), and a full-res composite that is one trilinear tap on the same
  `scene·T + inscatter` blend. Slices are drawn one layer at a time via
  `glFramebufferTextureLayer` — the cascade/mask-array/cube-face idiom — so no geometry shader
  was needed. The screen-space march was deleted once the froxel path was verified against it —
  carrying both meant every fog parameter change had to land in two shaders.
- **Dependencies.** Shadow map(s); much smoother with TAA and CSM.
- **Refs.** Wronski, "Volumetric Fog" (SIGGRAPH 2014); Hillaire, "Physically Based &
  Unified Volumetric Rendering in Frostbite" (SIGGRAPH 2015).

#### 4.6 Cascaded Shadow Maps  — **M** — ✅ **shipped** (`csm` branch)
- **What.** Split the view frustum into 3–4 depth ranges, each with its own shadow map.
- **Why.** Your single map spreads limited resolution across the whole scene — fine for a
  single asset, poor for anything large/outdoor. CSM is the standard fix and pairs with your
  existing PCSS filter.
- **Current state.** Single map + PCSS (`shadow_depth_*`, `shadow.c`).
- **GL 4.1 approach.** Render N cascades to a texture array (or atlas), pick cascade per
  fragment by view depth in `pbr_frag`, keep PCSS per-cascade; add cascade-boundary
  dithering/blend. Stabilize by snapping texel-sized to kill shimmer.
- **Dependencies.** None. Synergizes with volumetrics.
- **Refs.** Engel, "Cascaded Shadow Maps" (ShaderX / GPU Gems); Valient CSM stabilization.
- **Shipped shape.** Runtime `cascade_count` (1–3) on ShadowSystem; layers stride by the
  RUNTIME count (`layer = slot*count + cascade`) so count 1 keeps the classic layer
  indices and stays byte-identical to the single-map path — the branch's central gate.
  λ=0.75 log/uniform splits over [camera near, min(far_plane, camera far)];
  sphere-of-slice fitting (rotation-invariant, so stable under orbit) with texel
  snapping in a rotation-only light view space. **The OUTERMOST cascade is the
  classic camera-independent scene-fit map** — camera-fit cascades are never
  scene-complete (their boxes clip casters), so every pure-camera-fit set leaks
  a boundary that moves with the camera; anchoring the last cascade to the scene
  makes the union's floor scene-complete. `pbr_frag` walks from the fragment's
  cascade outward taking the max occlusion; `catcher_frag` (a soft scene-scale
  grounding shadow) samples ONLY the scene-fit map — no selection, no seams,
  exactly the pre-cascade floor behavior; fog marches per-step cascade
  fall-through ending in the scene map. `cascadeParams[layer] = (width, orthoNear, orthoFar, biasNorm)`
  where `.w` undoes each cascade's depth-range stretch of the 0..1 app-tuned bias and
  grows with the texel ratio vs the scene-fit reference; the catcher's 5×5 kernel is
  likewise world-normalized to the scene-fit width. PCSS reads the sampled cascade's
  ortho geometry from cascadeParams (the single-map globals died). Probe capture
  forces count 1 (six capture cameras need the camera-independent scene-fit map).
  Viewer defaults to 3 cascades; `--shadow-cascades 1` restores the classic map,
  `--csm-debug` tints by cascade. MAX_LIGHTS dropped 70 → 64 to fit the cascade
  matrix/param arrays under the 4096-component GL 4.1 fragment uniform limit.

#### 4.7 Physically-based sky & atmosphere  — **M** — ✅ **shipped** (`sky` branch)
- **What.** Analytic multiple-scattering sky with a movable sun and aerial perspective.
- **Why.** If you light from HDRIs, a real atmosphere gives a dynamic sun/time-of-day and
  distance haze that a static skybox can't, and it drives the directional light + volumetrics
  coherently.
- **Current state.** Static skybox / IBL environment (`skybox_*`, `ibl.c`).
- **GL 4.1 approach.** Hillaire's model precomputes transmittance + multi-scatter LUTs
  (small fullscreen passes, no compute needed), then a sky-view LUT sampled by the skybox
  pass; aerial perspective applied in a fullscreen pass by depth.
- **Dependencies.** Ties into the directional light and (4.5) volumetrics.
- **Refs.** Hillaire, "A Scalable and Production Ready Sky and Atmosphere" (EGSR 2020);
  Bruneton, "Precomputed Atmospheric Scattering" (2008/2017).
- **Shipped shape.** New module `sky.{c,h}` (sibling of `probe.{c,h}`), behind `--sky`
  (default off → structural byte-identity; `--sky` + `-e` is a hard error). Three LUTs as
  fullscreen raster passes, kilometres throughout for fp16 safety: transmittance 256×64 and
  multiple-scattering 32×32 are sun-INDEPENDENT (baked once); the sky-view LUT 192×108
  (sun-relative azimuth × sqrt horizon-warped latitude) re-bakes per sun move. **The sky
  feeds the existing IBL, not a bespoke path**: M0 extracted `ibl_bake_from_cubemap` (the
  re-bake entry point, delete-before-gen throughout) out of `precompute_ibl`; the sky renders
  six faces into a 256² environment cubemap and hands it straight to that function, so
  irradiance → prefilter (256²/7 mips, `max_reflection_lod` 6) → skybox → probe → fog publish
  all follow the sun with zero downstream changes. `sky_update_sun` is the single "sun moved"
  entry: re-derive `sun_dir`, `sky_bake`, retint the coupled key light. **Sun ↔ key light is
  owned by the sky module** (`sky_apply_sun_to_light`): direction away from the disc, colour
  from a CPU transmittance march (no GPU readback), intensity faded to zero below ~3° so a
  night sky casts no direct light or shadow; the app wires the light once and the GUI's live
  re-bake reuses the same path. GUI Sky sub-group (elevation/azimuth/disc sliders) re-bakes
  live per change (~10 ms at this env size, so no split needed — the sky-view + env render is
  the only variable cost and must be live anyway); an environment-only probe re-prefilters on
  slider release, a scene-captured probe is left as shot. Ground projection is forced off in
  sky mode — `sky_env`/`sky_background` instead shade below-horizon rays as a Lambertian
  virtual ground (albedo 0.3, sun-lit) so the shadow catcher still grounds the model.
- **Trade-off — sun disc is analytic in the background only, never in the env cube.** A 0.53°
  disc is ~3 texels at 256², which aliases the prefilter importance sampler into fireflies,
  and its direct energy already ships as the key light (baking it in would double-count). Cost:
  IBL mirror reflections show no disc — the key light's specular highlight stands in for it.
- **~~Deferred~~ — aerial perspective. SHIPPED in spec 9.6.** The deferral reasoning held for two
  more specs and was the right call: invisible at studio/prop scene scales (<0.1 % extinction over
  ~100 m), with `--fog` already providing sun-driven depth haze. What unblocked it was not B1's 3D
  machinery — that was never the constraint — but building the world-scale scene this note was
  waiting for (`assets/aerial_fixture.gltf`, ridges from 20 to 95 km). The predicted shape was
  right: a units→km knob (`world_units_per_km`, `--world-scale`) plus a transmittance-LUT term,
  now a 32³ volume folded into the fog composite rather than a term in the deleted `fog_frag`.
- **Note.** The engine's frame output is not frame-invariant for a static scene even with
  `--no-auto-exposure` (a per-frame temporal term, present on the `-e` path too); byte gates
  therefore compare at a FIXED frame count (deterministic there), and the M4 re-bake
  equivalence was verified at the IBL-texture level (env/irradiance/prefilter bit-identical
  when re-baked to the same sun) rather than by whole-frame screenshot.

---

### Tier 3 — Cheap, high-ROI material & correctness wins

#### 4.8 Multi-scatter GGX energy compensation (Kulla-Conty)  — **S** — ✅ **shipped** (`energycomp` branch)
- **What.** Add the energy lost by single-scatter GGX back via a precomputed
  directional-albedo LUT.
- **Why.** Rough metals currently darken (energy leaks out of the microfacet model). ~20
  lines + a small LUT; pure correctness.
- **Current state.** Shipped with NO new LUT: the existing split-sum BRDF LUT's A+B is the
  single-scatter directional albedo, and specular (analytic + IBL) scales by
  `1 + F0*(1/Ess - 1)` (Fdez-Agüera), hoisted per pixel and reusing the ambient LUT fetch.
  Default on; `--no-energy-comp` / GUI checkbox; gated on iblEnabled (the LUT only exists
  with an environment — engine-owned LUT is the upgrade path for no-env analytic comp).
- **GL 4.1 approach.** Bake the `E(µ, roughness)` LUT (reuse the `ibl_brdf` LUT pipeline),
  add the multi-scatter term in `pbr_frag` direct + IBL specular.
- **Refs.** Kulla & Conty, "Revisiting Physically Based Shading at Imageworks" (SIGGRAPH
  2017); Fdez-Agüera, "A Multiple-Scattering Microfacet Model" (2019).

#### 4.9 Screen-space refraction (real glass/liquids)  — **M** — ✅ **shipped** (`refraction` branch)
- **What.** For transmissive surfaces, sample the **scene color buffer behind** the surface,
  offset by the refracted vector (normal + IOR), blur by roughness for frosted glass.
- **Why.** You have IOR + thin-film but no *see-through* refraction. This is the big
  material win for glass, gems, liquids, and it reuses data you already have.
- **Current state.** Shipped: `engine_resolve_opaque_color` blits the MSAA color (opaques +
  skybox) into a lazy mipped RGBA16F target between skybox and the late pass; transmissive
  meshes (`transmission > 0`, imported from KHR_materials_transmission with ior/thickness
  scoped to transmissive materials) join the late pass and sample it through
  `refract(-V,N,1/ior)·thickness` — thickness 0 = thin (tint+blur only, per spec), box mips
  = the roughness blur. Diffuse yields via `kD *= 1-transmission`; specular/emissive stay.
  `sceneColorTex` rides dead unit 6 (height bind removed). `--no-refraction` kill switch;
  `assets/glass_fixture.gltf` (generated) is the end-to-end test. v1 limits documented in
  the spec: no G-buffer writes for glass (AO/SSR/TAA-velocity blind), refracted image
  excludes catcher/other transparents, unsorted overlap.
- **GL 4.1 approach.** Render opaque scene → resolve to a color texture → draw transmissive
  objects last, sampling that texture with a refraction offset; roughness selects a blurred
  mip. Matches glTF `KHR_materials_transmission`/`volume` semantics.
- **Dependencies.** A resolved opaque-scene color texture (easy from the post chain).
- **Refs.** glTF `KHR_materials_transmission`; Frostbite/UE transmission notes.

#### 4.10 Clearcoat (second specular lobe)  — **S/M** — ✅ **shipped** (`clearcoat` branch)
- **What.** An additional thin, smooth dielectric GGX lobe over the base BRDF.
- **Why.** Car paint, lacquer, varnish, carbon fiber. You already have anisotropy/SSS/
  sheen/thin-film — clearcoat is the notable gap.
- **Current state.** None.
- **GL 4.1 approach.** Add clearcoat weight/roughness/normal to `Material`; evaluate a second
  GGX lobe with fixed F0≈0.04 and layer over the base per glTF `KHR_materials_clearcoat`.
- **Refs.** Kelemen-Szirmay-Kalos coat; Burley "Physically Based Shading at Disney" (2012).
- **Shipped shape.** Two phases. **(A) Sampler-budget foundation first**: the engine was at the
  GL 4.1 fragment-sampler ceiling (17 declared samplers, and `brdfLUT`/skybox bound *out of spec*
  at units 16/17 — only Apple's lenient driver tolerated it). Rather than channel-pack, the
  material system was moved to the scalable GL-4.1 primitive — a **`sampler2DArray`** (new
  `mask_array.{c,h}`, sibling of `probe`/`sky`): the seven scalar masks (roughness/metallic/ao/
  opacity/microsurface/anisotropy/subsurface) collapse into ONE array sampler indexed per material
  by layer (`layer >= 0 ? maskArray[layer].<chan> : scalar`), dropping declared samplers 17→11,
  freeing units, and relocating the shadow/IBL units below 16 (a `_Static_assert` chain pins the
  whole ordered 0–15 budget). albedo/normal/emissive stay on dedicated native-res units; masks are
  deduped by id and GPU-resampled (blit-via-copy shader) into canonical-size layers (largest
  present dim, cap 2048), built lazily when the async loader drains. c64 byte-identical (its masks
  are all 2048); raiden 0.175% RMSE from upsampling its sub-2048 masks. **Choice of array over
  channel-packing was deliberate** — packing caps at 4 channels/texture, the array scales to any
  new mask type by adding a layer (the closest thing to bindless on GL 4.1). **(B) Clearcoat**:
  a second GGX lobe (fixed F0=0.04/IOR 1.5, coat roughness) in both the analytic loop and the IBL
  split-sum, reusing `prefilteredMap`+`brdfLUT` at the coat roughness — no new IBL sampler. The
  base is attenuated by the coat's Fresnel `(1 − ccWeight·Fc)` (energy-conserving; the coat
  deepens rather than over-brightens the base). Global `clearcoatEnabled` + `--no-clearcoat`, and
  per-material `clearcoatFactor` defaults 0 ⇒ the lobe is skipped and output is byte-identical
  (the coat also keeps the base `N·L` accumulation cosine, so the pre-clearcoat float grouping is
  bit-exact when off). Import reads `AI_MATKEY_CLEARCOAT_FACTOR`/`_ROUGHNESS` (glTF-scoped; FBX ⇒
  0). The **clearcoat normal map** (orange-peel / carbon-fibre weave) lands on freed unit 3 — the
  array foundation's payoff — perturbing a coat normal `Nc` (independent of the base normal map,
  per glTF, but sharing its tangent basis so it must replicate the base's bitangent-handedness
  correction — the B4 review caught the coat dropping it, silently wrong-handing the weave on
  mirrored-UV meshes) that shows in the coat's microfacet term and IBL reflection; imported from
  `aiTextureType_CLEARCOAT` index 2 (a bespoke read, since the index-0 mapping table can't reach
  it). Test asset: `clearcoat_fixture.gltf` (car-paint + carbon spheres, embedded procedural weave;
  regen `gen_clearcoat_fixture.py`). **Trade-off (documented):** the coat shares the base `N·L`
  cosine in the per-light accumulation (not its own `Nc·L`) to preserve byte-identity when off —
  the coat normal's weave still reads through the `D(Nc·H)` microfacet term; a fully-separate coat
  cosine reorders the float accumulation (sub-ULP, but breaks the byte gate).
- **Follow-ups:** ~~the reserved `sheen`/`reflectance` slots make KHR_materials_sheen / _specular
  clean adds~~ ✅ done in **4.10.1**; a shared `gl_state_save/restore` helper for the capture/bake
  prologue (5th copy now) and a shared material-mask-slot descriptor table are noted extraction
  candidates.

#### 4.10.1 KHR_materials_specular + _sheen (the reserved slots)  — **S/M** — ✅ **shipped** (`materials-sheen-specular` branch)
- **What.** Activated the two long-reserved-but-dead material slots. **KHR_materials_specular**
  re-parameterizes the dielectric F0 (`specularColorFactor` tints it, `specularFactor` weights it) —
  not a new lobe, a modulation at the single F0 site. **KHR_materials_sheen** adds a retroreflective
  Charlie cloth lobe (velvet / satin): a new `distributionCharlie` (Estevez-Kulla) + `visibilityAshikhmin`,
  layered over the base like clearcoat (analytic + IBL), `sheenColorFactor` + `sheenRoughnessFactor`.
- **Zero new samplers.** sheen-color / specular-color textures ride the already-reserved units 8/9;
  scalar factors are uniforms. Import verified on real assets (Assimp 6.0.5 populates
  `AI_MATKEY_SPECULAR_FACTOR` / `$clr.specular` and `$clr.sheen.factor` / sheen roughness); the
  specular-color key is glTF-gated via the KHR-specific `specularFactor` presence.
- **Byte-identity (the hard part).** Both gate off a per-material signal (`specular_factor = -1`
  sentinel; `sheenColorFactor == 0`) so non-carrying assets (c64, raiden) are `cmp`-exact vs master.
  The specular WEIGHT is folded into F0 rather than scaling the specular term — a direct specular-term
  weight reassociated the base IBL float grouping and broke the `cmp` gate (the clearcoat B3
  compiler-reassociation lesson, hit again and re-confirmed).
- **Trade-offs (documented v1 simplifications, all follow-ups):** the specular weight folded into F0
  under-dims grazing (Fresnel → 1) and the IBL specular weight-dimming is omitted, both for
  byte-identity; ~~the sheen directional-albedo is analytic with no baked Charlie E-LUT~~ ✅ shipped
  in 10.7 (E baked into the BRDF LUT's blue channel, Charlie alpha squared per spec); the specular
  weight is folded into F0 (under-dims grazing — the deeper fix threads `f90 = specularFactor`
  through Fresnel); the **specular** color texture and the sheen **roughness mask-array layer** are
  deferred (the sheen color texture rides the reserved unit 8 and is wired end-to-end, exercised
  factor-only in the fixtures). Fixtures:
  `gen_specular_fixture.py` (gold/blue/dim spheres), `gen_sheen_fixture.py` (red/blue velvet + gold
  satin, best under a moody env like moon_lab).
- **~~Discovered follow-up — specular occlusion~~ ✅ shipped in 4.2.1.** GTAO's screen-space
  visibility multiplies the *whole* tonemapped color (`tonemap_frag.glsl`), darkening specular too.
  Fixed by a composite-time spec-occ that blends the AO toward *unoccluded* by
  specular-fraction × smoothness (NOT the literal Lagarde `so` — that occludes the grazing rim more;
  see §4.2.1). Benefits base specular + clearcoat + sheen.

#### 4.2.1 Specular occlusion + edge-aware AO — **S/M** — ✅ **shipped** (`specular-occlusion` branch)
- **What.** Two AO-quality fixes. **(a) Specular occlusion** at the tonemap AO-apply: GTAO's
  screen-space visibility multiplied the *whole* composited color, darkening/shimmering the specular
  on model surfaces. `aoVisibility()` blends the AO toward *unoccluded* (1.0) by
  `mix(fresnel, 1, metallic) × (1 − roughness)` — smooth grazing specular goes unoccluded; diffuse/
  rough pixels keep the plain AO. NdotV is reconstructed from the aux linZ (copying gtao's
  `viewPosFromLinZ`); **roughness now rides the previously-unused `aux.w`** (a one-word `pbr_frag`
  export). **(b) Depth-bilateral AO blur**: the 4×4 box that cancels GTAO's noise tile also bled the
  sphere's silhouette occlusion onto the floor ("dripping"); the taps are now Gaussian-weighted on
  relative linZ. Two flags (`--no-spec-occlusion`, `--no-ao-edge-filter`, both default on); both off =
  `cmp`-exact vs master. Debug view 7 = the spec-occ AO visibility.
- **Deliberately NOT Lagarde.** The planned `saturate(pow(NdotV+ao, exp2(-16r-1))-1+ao)` occludes the
  grazing rim *more* (its saturate-to-1 only holds at high NdotV), which a metric confirmed made the
  shimmer worse; the unocclude-smooth-specular model dropped the AO's effect on the specular 74%.
- **4.4.1 SSR reflection quality — ✅ SHIPPED (`ssr-reflection-quality` branch; see
  `specs/4.4.1-ssr-reflection-quality.md`).** The striped "venetian-blind" reflection under a sphere
  on the glossy floor is **SSR, not AO** (isolate by `--no-ssr`). The root cause was NOT "one sharp
  unblurred tap" (the early hypothesis) but **fixed-screen-position Hi-Z/march *grid* artifacts** —
  the half-res march quantized the hard hit/miss coverage edge to a grid, tent-upsampled to a jagged
  edge. Fix = the SVGF denoise pipeline the engine already runs for SSGI, mirrored for SSR: **full-res
  trace** (M1) + **stochastic march** (M3a: jitter the ray per-pixel so the deterministic grid
  scatters into noise) + **temporal accumulation** (M2) + **spatial à-trous denoise** (M3b: resolve
  the noise into a clean reflection in a single frame). Kill-switches `--no-ssr-full-res /
  --no-ssr-temporal / --no-ssr-denoise`, each OFF `cmp`-exact to the prior state. Accepted tradeoff:
  the denoiser leaves the roughness-0.1 near-mirror clean-but-slightly-soft (planar's domain,
  explicitly declined). (Also latent, untouched: concentric AO banding on large flat grounds — a
  `gtao_frag` 32-sector × SLICES=2 sweep quantization issue.)
- **Follow-up — unify spec-occ at the forward material-AO site.** The material `aoMap`
  (`pbr_frag.glsl:1027`) still dims diffuse+specular ambient equally. The forward pass holds the
  separated `specular` term + exact NdotV/F0/roughness (all of which the composite path rebuilds and
  the F0=0.04 assumption approximates), so the *exact* spec-occ belongs there too. Not a v1 blocker
  (baked AO is low-frequency/stable → no shimmer; the visible artifact was fully GTAO), but the two
  occlusion sites are silently divergent until unified.

#### 4.11 Parallax Occlusion Mapping  — **S/M** — ✅ **SHIPPED** (merged to master; see `specs/4.11-parallax-occlusion-mapping.md`)
- **What.** Ray-march a height map in tangent space to fake real surface relief with
  self-occlusion, self-shadowing, and receding silhouettes.
- **Shipped as the full technique** in `pbr_frag`: `parallaxOcclusion()` marches the height field
  along the tangent-space view dir (`transpose(TBN)*V`; adaptive 8–32 layers + last-two-sample
  interpolation; `depth = 1 - height`, white = raised) and rewrites `uv` before every material
  sampler; `parallaxSelfShadow()` marches toward each analytic light to darken the grooves; and a
  silhouette `discard` recedes the relief past the [0,1] tile edge (gated on `uv0` in the first tile
  so tiled UVs aren't clipped mid-surface — the 1:1-UV constraint). All behind a
  `parallaxEnabled && heightTexExists && parallaxScale>0` guard, so every OFF path (`--no-parallax`,
  no height map, scale 0) is `cmp`-exact vs master.
- **Height input:** glTF carries no height/displacement texture (the `KHR_materials_displacement`
  draft is dead; assimp surfaces nothing), so `resolve_height_maps()` resolves a `<name>_height.<ext>`
  **filename-convention** sibling into the existing `Material.height_tex` (dedicated sampler on the
  freed unit 4) and auto-enables POM with a default depth (`--parallax-scale`). Runs once after the
  async texture loader drains (same defer-until-idle idiom as the mask array). `import.c`/assimp stay
  byte-clean.
- **Fixture:** `assets/gen_parallax_fixture.py` → a brick wall (external albedo/normal + procedural
  `_height` PNG); `--parallax`/`--no-parallax`/`--parallax-scale`; the render app now defaults the
  texture dir to the model's own directory (external-texture glTF loads without `-t`).
- **Known limitation — grazing silhouette aliasing.** The silhouette `discard` is all-or-nothing per
  pixel (MSAA can't smooth a discard edge) and is quantized by the discrete march layers, so a grazing
  *still* shows a stepped/"dashed" edge. **Live is clean** — the interactive path is 1× MSAA + TAA, and
  jitter/accumulation/motion carry the edge; the artifact only appears in non-TAA contexts, notably the
  deterministic headless 4× MSAA screenshots (TAA off there for byte gates). Deliberate keep-silhouette
  ship decision (the recession gives real edge thickness; a flat cutoff would read card-flat).
- **raiden byte note:** OFF gates are `cmp`-exact for c64/fixture and C-side for raiden; raiden shows
  ±1 ulp on 2/6.2M px from GLSL FP reassociation of the guarded shader code (benign, isolated to the
  shader, not a behavior leak).
- **Deferred follow-ups:** alpha-to-coverage silhouette AA + finer/adaptive march layers (the real fix
  for the stepping); tiled-UV seam gate (tile-0's own `[0,1]` boundary); a real glTF height-import
  convention if a standard emerges; POM LOD fade at distance. (True displacement via GL 4.1
  tessellation remains a bigger option.)
- **Refs.** Tatarchuk, "Parallax Occlusion Mapping" (GDC 2006).

#### 4.12 Upgrade SSS to separable screen-space SSS  — **M** — ✅ **SHIPPED** (`sss` branch; see `specs/4.12-sss.md`)
- **What.** Post-process diffusion of light in screen space using a separable
  two-pass Gaussian approximating the skin diffusion profile.
- **Why.** The old wrap-lighting SSS was a dormant hack; separable SSS is the film/skin standard.
- **Shipped as proper diffuse separation.** `pbr_frag` accumulates skin diffuse irradiance into a
  guarded `sssDiffuse` local and writes it to a **gated 5th MRT attachment** (`DiffuseOut = subsurface
  * diffuse`, 0 off-skin, so it doubles as the mask). `FragColor`'s expression is left byte-for-byte
  unchanged. `postfx_run_sss` resolves attachment 4, blurs it separably (H/V, depth-aware, per-channel
  **sum-of-three-Gaussians** Jimenez skin profile), and additive-folds `blur - diffuse` into `hdr_fbo`
  (composite `hdr + blur - D`) so the diffuse softens while specular/IBL/SSR stay razor-sharp. A
  Barre-Brisebois **back-light transmission** term adds the thin-region glow (also guarded).
- **Byte-identity:** the whole path is guarded (`sssEnabled` uniform + `subsurface > 0`), so
  `--no-sss` and any non-skin material are `cmp`-exact vs master (verified on c64/raiden with ZERO
  drift — the diffuse accumulator reuses existing sub-expressions and never touches FragColor's
  grouping). The SSS pass runs on non-skin scenes but composites exactly 0 (additive-of-zero,
  byte-exact). Toggle `engine->sss_enabled` (`--no-sss` / GUI); `--sss-radius` / `--sss-color`.
- **Fixture:** `gen_sss_fixture.py` -> a warm mid-value wax sphere (`sss_fixture.gltf`); reads as
  skin/wax under a moody directional env (moon_lab) -- a bright softbox HDR clips the diffuse to white
  (no gradient to scatter), same lesson as the sheen fixture.
- **Deferred → cleared in 4.12.1** (`sss-v2` branch, `specs/4.12.1-sss-followups.md`): per-material
  profiles (profile index in the skin-diffuse alpha + a PostFX profile table), the cross-material bleed
  reject, TAA accumulation of the SSS delta (its own `sss_history`, like fog/SSR), plus two cleanups
  (removed the dead subsurface-texture chain; a g-buffer descriptor table that fixed two latent
  missed-site bugs). The CPU skin-presence gate shipped in 4.12's review (`scene_has_subsurface`).
  Still open: pre-integrated skin BRDF for distant heads; thickness-map-driven transmission;
  per-material transmission tint distinct from scatter color.
- **Refs.** Jimenez et al., "Separable Subsurface Scattering" (2015); Barre-Brisebois & Bouchard,
  "Approximating Translucency" (GDC 2011).

#### 4.13 Bloom quality: dual-filter pyramid  — **S** — ✅ **shipped** (`bloompyramid` branch)
- **What.** Replace the fixed half-res 2-iteration Gaussian with a progressive
  downsample/upsample pyramid (Kawase / dual-filter).
- **Why.** Wide, smooth, stable bloom without the banding/box artifacts of a shallow
  Gaussian — for the same or lower cost.
- **Current state.** Shipped as the Jimenez 13-tap downsample + additive tent upsample on a
  single packed-float mip chain (half-res base → ~8-16 px, built with the hi-z re-attach +
  BASE/MAX_LEVEL idiom). Bright pass (Karis knee + firefly clamp) and tonemap composite
  unchanged; default strength retuned 0.08 → 0.015 for the accumulated energy. `--no-bloom`
  added for headless A/B.
- **GL 4.1 approach.** Build a mip chain (5–6 levels) with a 13-tap downsample and a tent
  upsample that accumulates; standard MRT/fullscreen work, no compute.
- **Refs.** Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare"
  (SIGGRAPH 2014); Froyok "Custom Bloom" writeups.

---

### Tier 4 — Camera / finishing polish

#### 4.14 Auto-exposure / eye adaptation  — **S/M**
- **What.** Measure average scene luminance, adapt exposure over time.
- **Why.** You only have manual exposure (`postfx.h:65`). Auto-exposure makes varied
  lighting "just work."
- **GL 4.1 approach.** No compute histogram — compute log-luminance, generate a mip chain,
  read the top mip (average) on the CPU or in-shader, smooth toward it (adaptation speed).
  Optional metering mask. Feed the existing exposure uniform.
- **Refs.** Lagarde & de Rousiers, "Moving Frostbite to PBR" (exposure section).

#### 4.15 Motion blur  — **M** — ✅ **SHIPPED** (`motion-blur` branch; see `specs/4.15-motion-blur.md`)
- **What.** Per-object + camera blur from the velocity buffer.
- **Why.** Cohesion with TAA's velocity data; sells fast motion and turntables.
- **Dependencies.** Velocity buffer from 4.1 — which was already full-res, un-jittered, and
  camera+object+bone-complete (`aux_texture.xy`), so the whole feature is a self-contained postfx
  pass with **zero** shader/matrix work on the input side.
- **Shipped as the full McGuire pipeline**, three staged passes in `postfx_run_motion_blur` (after
  fog, before DoF): `motion_blur_tilemax` (full-res velocity → max-magnitude per 20px tile) →
  `motion_blur_neighbormax` (3×3-tile max, so blur bleeds past an object's silhouette) →
  `motion_blur_frag` reconstruction (16-tap gather along the tile velocity, weighted by a soft
  depth compare + McGuire velocity cones so foreground blurs over background, interleaved-gradient
  dither for banding). Reads `hdr_texture`+`aux_texture`, writes a scratch, blits back into
  `hdr_fbo`. Off by default (`fx->motion_blur_enabled`, `--motion-blur` / GUI + `--motion-blur-scale`
  shutter); adds itself to `postfx_wants_aux_gbuffer` so velocity is guaranteed when enabled.
- **Byte-identity:** the OFF path is a gated skip touching no shipped shader → `cmp`-exact vs
  master (c64/raiden). A zero-motion early-out (neighbor-max < 0.5px → exact centre texel) makes
  `--motion-blur` on a static scene `cmp`-exact too — the ON-path headless gate. The gather path is
  deterministic (IGN dither is `gl_FragCoord`-based); the animated ON test is visual-only because
  the animation pose is non-deterministic run-to-run (pre-existing time-based playback).
- **Deferred:** separable tile-max (max is separable — 20+20 taps vs the single 20×20 pass) if
  perf ever matters; per-object shutter/segment tuning; motion-blur-aware TAA.
- **Refs.** McGuire et al., "A Reconstruction Filter for Plausible Motion Blur" (I3D 2012).

#### 4.16 AgX tonemapping  — **S** — ✅ **shipped** (`agx` branch)
- **What.** A modern display transform that desaturates toward white on the way up,
  avoiding ACES's hue shifts and blown saturated highlights.
- **Why.** Cheap option beside your ACES/Neutral; it's the current default look (Blender 4+).
- **Current state.** Shipped as `POSTFX_TONEMAP_AGX` (Wrensch's fitted minification: inset
  matrix → log2 EV → sigmoid → outset + linearize, honoring toneSelect's LDR-linear
  contract). GUI is now a three-way combo; `--tonemap <aces|neutral|agx>` selects headless.
  Default stays Neutral; making AgX the default is a user call after living with it.
- **GL 4.1 approach.** Add a third `PostFXTonemapMode`; AgX is a matrix + fitted curve (or a
  3D LUT) in `tonemap_frag`.
- **Refs.** Troy Sobotka, AgX; Blender AgX implementation notes.

#### 4.17 Order-Independent Transparency (weighted-blended)  — **M** — ✅ **SHIPPED** (`oit` branch; see `specs/4.17-oit.md`)
- **What.** Approximate correct layered alpha without sorting via weighted accumulation +
  revealage.
- **Why.** Correct foliage/glass/particle layering; removes sort popping.
- **Shipped:** scoped to `ALPHA_BLEND && transmission==0` (the transmission/refraction late pass and the
  ALPHA_MASK A2C cutout path are untouched). Blend meshes accumulate into a **separate lazily-allocated
  MSAA FBO sharing the scene depth** (accum RGBA16F @5 additive, revealage R16F @6 multiplicative, via
  `glBlendFunci`); `pbr_frag` writes guarded `AccumOut`/`RevealageOut` (locations 5/6) under `oitPass`;
  `postfx_run_oit` resolves + composites over the opaque HDR before TAA/SSR/bloom. Off by default
  (`--oit` opt-in) so `--no-oit` is byte-identical to master. The one review bug: the late-pass filter
  keyed on `oit_enabled` instead of `oit_this_frame`, dropping blend meshes when the accumulate didn't
  run (non-PBR / alloc failure) -- fixed.
- **GL 4.1 approach.** Two extra render targets (accum `RGBA16F`, revealage `R16F`), a resolve pass.
  Standard MRT + indexed blend (`glBlendFunci`), no compute.
- **Refs.** McGuire & Bavoil, "Weighted Blended Order-Independent Transparency" (JCGT 2013).

---

### Architectural (bigger; do only if needed)

#### 4.18 Clustered / tiled forward (Forward+)  — **L**
- **What.** Cull lights into a 3D froxel grid so shading touches only the lights that
  overlap each cluster — lifts the N-nearest / ~75-light cap.
- **Why.** The current per-object nearest-light selection breaks down with many dynamic
  lights. Forward+ is the scalable answer.
- **Current state.** Forward, N-nearest per object (`render.c`, ~75 cap).
- **GL 4.1 approach.** The compute-friendly version isn't available; do light culling on the
  **CPU** per cluster, or build the cluster→light-index lists into a **texture buffer**
  (`samplerBuffer`) each frame, then index it in `pbr_frag`. Fiddlier without compute; treat
  as a real project.
- **Refs.** Ola Olsson et al., "Clustered Deferred and Forward Shading" (HPG 2012);
  Persson, "Practical Clustered Shading."

---

## 5. Beyond real-time (if you ever want actual ray tracing)

Three tiers, cheapest first:

- **Offline CPU path tracer — days, best quality, non-interactive.** Reuses `intersect.c`
  (ray/AABB/triangle already written) and `_save_framebuffer_ppm` (you already render
  headless to PPM). Build a BVH over scene triangles, trace on the CPU, accumulate samples,
  write an image. No GL-version limits at all. Lowest-friction way to get reference-quality
  RT into Cetra. Trade: not interactive.
- **Hybrid GL + Metal RT via IOSurface — weeks, interactive, hardware-assisted.** Keep GL for
  rasterization/post/present; add a Metal device that builds a `MTLAccelerationStructure` and
  traces (reflections/AO/GI) in an MSL kernel, writing to an **IOSurface**-backed texture that
  GL imports (`CGLTexImageIOSurface2D`) and composites. Your normals+roughness G-buffer is
  exactly the input a hybrid RT reflection/AO pass wants. Costs: a Metal module (metal-cpp +
  MSL), a duplicated geometry/acceleration structure, and a per-frame GL↔Metal sync (no shared
  fence on macOS). Best *interactive* RT option short of a full port; HW-accelerated on M3+,
  compute-unit RT on the M1 Max.
- **Full Metal or Vulkan/MoltenVK backend — months.** Everything modern (compute, SSBOs, HW
  RT, clustered shading done right). A rewrite of the renderer, not a mode.

---

## 6. Summary table

| # | Technique | Tier | Effort | Depends on | Payoff |
|---|---|---|---|---|---|
| 4.1 | TAA + velocity buffer | 1 | M | — | ✅ **done** — AA + unlocks temporal denoise/upsample/motion blur |
| 4.2 | GTAO / Visibility-Bitmask AO | 2 | M | depth+normals | ✅ **done** — better occlusion, no haloing, SSGI substrate |
| 4.2.1 | Specular occlusion + edge-aware AO | 2 | S/M | GTAO, aux.w roughness | ✅ **done** — GTAO off specular; no AO silhouette bleed (SSR striping = separate follow-up) |
| 4.3 | SSGI (one-bounce indirect) | 2 | M/L | TAA | ✅ **done** — color bleeding / contact GI |
| 4.4 | Reflection probes | 2 | M | IBL capture | ✅ **done** — robust off-screen reflections |
| 4.5 | Volumetric fog | 2 | M/L | shadows (+TAA) | ✅ **done** — atmosphere, god rays |
| 4.6 | Cascaded shadow maps | 2 | M | — | ✅ **done** — large-scene shadow resolution |
| 4.7 | PB sky/atmosphere | 2 | M | dir light | ✅ **done** — dynamic sun; aerial persp. shipped in 9.6 |
| 4.8 | Multi-scatter GGX energy comp | 3 | S | — | ✅ **done** — correct rough-metal brightness |
| 4.9 | Screen-space refraction | 3 | M | opaque color tex | ✅ **done** — real glass/liquids |
| 4.10 | Clearcoat | 3 | S/M | — | ✅ **done** — car paint / lacquer / carbon-fibre + sampler-array foundation |
| 4.10.1 | KHR specular + sheen | 3 | S/M | reserved slots (have) | ✅ **done** — tinted dielectric specular + velvet/satin cloth |
| 4.11 | Parallax occlusion mapping | 3 | S/M | height slot (have) | ✅ **done** — cheap surface relief |
| 4.12 | Separable SSS | 3 | M | material id | ✅ **done** — film-grade skin |
| 4.13 | Bloom pyramid | 3 | S | — | ✅ **done** — smoother/stabler bloom |
| 4.14 | Auto-exposure | 4 | S/M | — | ✅ **done** — lighting "just works" |
| 4.15 | Motion blur | 4 | M | velocity buffer | ✅ **done** — motion cohesion |
| 4.16 | AgX tonemap | 4 | S | — | ✅ **done** — modern highlight rolloff |
| 4.17 | Weighted-blended OIT | 4 | M | — | ✅ **shipped** — order-independent alpha-blend, `--oit` |
| 4.18 | Forward+ (clustered) | arch | L | — | many dynamic lights |

**Start here:** ~~4.1 (TAA)~~ ✅ → ~~4.2 (GTAO)~~ ✅ → ~~4.3 (SSGI)~~ ✅ → ~~4.4 (probes)~~ ✅ → ~~4.5 (fog)~~ ✅ → ~~4.8 (energy comp)~~ ✅ → ~~4.13 (bloom pyramid)~~ ✅ → ~~4.16 (AgX)~~ ✅ → ~~4.9 (refraction)~~ ✅ → ~~4.6 (CSM)~~ ✅ → ~~4.7 (sky)~~ ✅ → ~~4.10 (clearcoat)~~ ✅ → ~~4.10.1 (specular+sheen)~~ ✅ → ~~4.2.1 (spec-occ + AO edge)~~ ✅ → ~~4.11 (POM)~~ ✅ → ~~4.15 (motion blur)~~ ✅ → ~~4.12 (SSS)~~ ✅ → ~~4.17 (OIT)~~ ✅ → **4.18 (Forward+ clustered) — the last Tier-4 item**.
