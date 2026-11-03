# Cetra Real-Time Rendering Roadmap

The engine's first rendering roadmap: eighteen techniques, ranked by what they unlock, each with a
GL 4.1 approach. Every item is built. What this document still holds is the follow-up work each
item left open, the GL 4.1 constraints that decide which techniques are possible, and the options
for real ray tracing (§5). Active planning is in `docs/aaa-rendering-roadmap.md`.

Each item below says what was built, its spec or branch, and the doc that describes it now, then
lists what is still open under it. §6 indexes them.

---

## 0. Platform constraints (read first — they shape every choice)

The engine requests an **OpenGL 4.1 core**, forward-compatible context. 4.1 is Apple's ceiling on
Intel and Apple Silicon alike (Apple Silicon runs GL on top of Metal and exposes only 4.1), and it
is the binding constraint on every technique.

**Not available in GL 4.1**, so every technique here avoids them:
- **Compute shaders** (GL 4.3): no compute light culling, prefix sums or histogram passes.
- **SSBOs** (GL 4.3): large or variable data goes through UBOs (small, fixed) or texture buffers
  (`samplerBuffer`).
- **Bindless textures**: material texture sets are atlased, array-packed or bound per draw.
- **Hardware ray tracing**: no OpenGL version has it.

**Available in GL 4.1**: tessellation shaders (since 4.0, so real displacement is possible),
texture buffer objects, fp32 textures, MRT, UBOs, transform feedback.

**Beyond OpenGL on a Mac** (a new backend, not a version bump): Metal (compute and ray tracing;
hardware RT on M3 and later, compute-unit RT before that) and Vulkan through MoltenVK. GL and Metal
can share textures through IOSurface (`CGLTexImageIOSurface2D` to an `MTLTexture`). See §5.

So every technique is a fragment-shader, multi-pass one. All of them predate compute; compute
made some faster, not possible.

---

## 1. Current pipeline architecture (grounded)

The render pipeline, the G-buffer, the scene passes and the PostFX chain are described in
`docs/architecture.md`, Render Pipeline. The shaders and the sampler model are in
`docs/shaders.md`; shadows, the sky, IBL and probes in `docs/lighting.md`.

---

## 2. Feature inventory (what already exists)

Every technique in §4 exists. `docs/shaders.md`, Inventory, lists the shaders by subsystem, and
`docs/shader-subsystems.md` describes each subsystem.

---

## 3. Roadmap & dependency ordering

The order was by what unlocks what: TAA came first because its velocity buffer and history
reprojection are what temporal denoising, upsampling and motion blur run on. All four tiers are
built:

- **Tier 1**, infrastructure: TAA and the velocity buffer (4.1).
- **Tier 2**, the largest visual gains: GTAO (4.2, 4.2.1), SSGI (4.3), reflection probes (4.4),
  volumetric fog (4.5), cascaded shadows (4.6), the physically based sky (4.7).
- **Tier 3**, cheap material and correctness wins: energy compensation (4.8), refraction (4.9),
  clearcoat (4.10, 4.10.1), POM (4.11), screen-space SSS (4.12), the bloom pyramid (4.13).
- **Tier 4**, camera and finishing: auto-exposure (4.14), motion blur (4.15), AgX (4.16), OIT (4.17).
- **Architectural**: clustered forward lighting (4.18).

The open follow-ups have no order among themselves; each is taken when something needs it.

Effort key: **S** about days, **M** 1-2 weeks, **L** 3+ weeks.

---

## 4. Techniques

### Tier 1 — Infrastructure

#### 4.1 Temporal Anti-Aliasing + motion-vector buffer — **M** — DONE

Halton(2,3) sub-pixel jitter, a velocity G-buffer, and a resolve that reprojects history by
velocity under a YCoCg neighbourhood clamp, Catmull-Rom history sampling and an inverse-luminance
blend (the blend is what suppresses specular fireflies the clamp cannot at one sample). Merged to
master before specs were numbered; TAAU builds on it (spec 11.7). See `docs/architecture.md`, TAA
and the sample count.

A skinned mesh's previous pose is uploaded as three affine rows a bone (`uPrevBoneRows` in
`include/skin.glsl`): a second `mat4[128]` beside `boneMatrices` does not fit the vertex stage's
uniform budget and the program fails to link.

- **Open: surfaces with no velocity.** The skybox, the shadow catcher and translucent surfaces
  write zero velocity, so they reproject as static and smear a little under fast motion. S.

---

### Tier 2 — Biggest visual jumps

#### 4.2 Ground-truth AO / near-field GI (GTAO or Visibility Bitmask) — **M** — DONE

GTAO by the 2023 visibility bitmask, replacing SSAO, with positions rebuilt from a linear view-Z in
the aux buffer and temporal accumulation through the velocity buffer. The bitmask's unset bits are
what SSGI (4.3) gathers through. Merged to master before specs were numbered. See
`docs/shaders.md`, Inventory (AO / GI).

The linear view-Z must be read with nearest filtering: it is not screen-linear, and linear
filtering bends flat surfaces and breaks fine geometry into speckle.

- **Open: the aux resolve averages view-Z at silhouettes.** With MSAA above one sample (the
  headless default) the aux attachment is resolved by a blit, which averages `.z` across an edge.
  S.
- **Open: concentric AO banding on large flat ground**, from the 32-sector by two-slice sweep's
  quantisation in `gtao_frag`. Unconfirmed since SSR's rewrite (10.9). S/M.

#### 4.3 Screen-Space GI (one-bounce indirect diffuse) — **M/L** — DONE

One bounce of indirect diffuse gathered in the GTAO bitmask sweep, denoised by temporal
accumulation and an edge-aware a-trous filter, composited before bloom. Off by default (`--ssgi`).
Built on the `ssgi` branch, which also built the first auto-exposure (4.14). The DDGI volumes
(aaa A4, spec 9.7) are the world-space answer to the same need.

#### 4.4 Reflection probes (SSR fallback) — **M** — DONE

A parallax-corrected local cubemap (Lagarde's box proxy), consumed as local specular in `pbr_frag`
and as SSR's miss fallback. Built on the `probe` branch; N probes blended per fragment followed in
spec 11.70 (aaa C4). See `docs/lighting.md`, Reflection probes.

On a stage whose environment is a far dome, the auto-placed probe is environment-only, with SSR
supplying the model's reflection: one parallax box cannot place both a near model and the far
dome, since a scene-sized box streaks the dome and a dome-sized box ghosts the model.
`--probe-scene` captures the scene instead, for interiors.

- **Open: relight.** A captured probe does not follow a moved sun. M.
- **Open: a `--probe-box` override** for the auto-placed probe's proxy box. S.

#### 4.4.1 SSR reflection quality — DONE

Full-resolution stochastic SSR with temporal accumulation and an a-trous denoise (spec 4.4.1); the
trace itself was rewritten in spec 10.9, which removed the grid stripes at the source. See
`docs/shaders.md`, Inventory (SSR).

#### 4.5 Volumetric fog / lighting — **M/L** — DONE

Froxel volumetric fog (spec 9.5, aaa B1): inject and light, front-to-back integrate, and a
full-resolution composite, over a 160x90x64 volume drawn a layer at a time. It replaced the first
version, a screen-space shadowed raymarch (spec 4.5), which could not afford the clustered light
list per step per pixel; the march was deleted rather than kept beside it, so every fog parameter
lives in one shader. See `docs/shader-subsystems.md`, Atmosphere.

#### 4.6 Cascaded Shadow Maps — **M** — DONE

Up to three cascades per light in a texture array, sphere-fitted and texel-snapped so they are
stable under orbit (spec 4.6). See `docs/lighting.md`, Cascades.

The outermost cascade is the camera-independent scene-fit map: a camera-fitted cascade clips
casters, so a set made only of them leaks a boundary that moves with the camera. The catcher
samples only that map, and a capture uses it alone.

#### 4.7 Physically-based sky & atmosphere — **M** — DONE

Hillaire's transmittance, multiple-scattering and sky-view LUTs as raster passes, rendered into the
IBL environment so one sun drives the skybox, IBL, probes, fog and the key light (spec 4.7).
Aerial perspective followed in spec 9.6 (aaa B9). See `docs/lighting.md`, The sky.

The sun disc is drawn analytically in the background only, never into the environment cube: a
0.53 degree disc is a few texels there, which aliases the prefilter's importance sampling into
fireflies, and its direct energy is already the key light, so baking it in would count it twice.
IBL mirror reflections therefore show no disc; the key light's highlight stands in for it.

---

### Tier 3 — Cheap, high-ROI material & correctness wins

#### 4.8 Multi-scatter GGX energy compensation (Kulla-Conty) — **S** — DONE

Specular scaled by `1 + F0*(1/Ess - 1)` (Fdez-Aguera), with `Ess` read from the split-sum BRDF
LUT, so no new LUT (spec 4.8). `--no-energy-comp` turns it off.

- **Open: compensation without an environment.** The LUT fetch and the compensation sit inside
  `pbr_frag`'s `iblEnabled` gate, so a scene with no environment gets no compensation on its
  analytic specular either. The LUT is always bound, so hoisting both out of the gate is the fix;
  it moves every no-environment render. S.

#### 4.9 Screen-space refraction (real glass/liquids) — **M** — DONE

Transmissive surfaces sample a mipped resolve of the opaque scene through the refracted ray,
thickness bending it and roughness choosing the mip, per `KHR_materials_transmission` and
`_volume` (spec 4.9). `--no-refraction` turns it off; `glass_fixture` is the test.

- **Open: glass writes no G-buffer**, so AO, SSR and TAA's velocity see the surface behind it
  (also in `docs/aaa-rendering-roadmap.md`, Known limitations).
- **Open: the refracted image holds no transparent surface**, since the resolve runs before the
  transparent pass, and overlapping transmissive surfaces are not sorted.

#### 4.10 Clearcoat (second specular lobe) — **S/M** — DONE

`KHR_materials_clearcoat`: a second GGX lobe (F0 0.04) over the base, analytic and IBL, attenuating
the base by its Fresnel, with its own normal map (spec 4.10). It came with the move of the scalar
masks into one `sampler2DArray`, now the material texture array; an array was chosen over channel
packing because packing stops at four channels a texture and an array takes any new image as a
layer. See `docs/shaders.md`, Texture / Sampler Model.

- **Open: the coat's own cosine.** In the light loop the coat lobe shares the base's `N.L` and
  its denominator rather than taking `Nc.L`, so the coat normal map shows only through the
  microfacet terms. S.
- **Open: a shared GL state save and restore** for the capture and bake prologues, which each
  write their own (`layers_vt.c`'s `vt_gl_state_save` is one). S.
- **Open: a descriptor table for the material mask slots**, which are wired one by one in
  `material.c`, `material_texture_array.c` and `render.c`. S.

#### 4.10.1 KHR_materials_specular + _sheen (the reserved slots) — **S/M** — DONE

Specular re-parameterises the dielectric F0 by colour and weight; sheen adds a Charlie cloth lobe,
analytic and IBL (spec 4.10.1). Conformance followed in 10.7 (the Charlie E-LUT and reference
visibility) and 10.8 (`f90 = specularFactor` at every F0 consumer).

- **Open: the specular colour texture.** `Material.reflectance_tex` is loaded and owned but has no
  unit and is never bound; it needs a sampler (`docs/shaders.md`, Fitting a new texture in). M.
- **Open: a sheen roughness texture.** Sheen roughness is the factor alone. S/M.

#### 4.2.1 Specular occlusion + edge-aware AO — **S/M** — DONE

GTAO kept off smooth specular, and a depth-bilateral AO blur so a silhouette's occlusion does not
bleed onto the floor (spec 4.2.1). Specular occlusion is now the split composite's cone term
(specs 11.76 and 11.77), and the material AO map dims only the diffuse share under it. See
`docs/shader-subsystems.md`, Specular occlusion.

Lagarde's `saturate(pow(NdotV + ao, exp2(-16r - 1)) - 1 + ao)` was rejected: it occludes the
grazing rim more, which made the shimmer worse.

#### 4.11 Parallax Occlusion Mapping — **S/M** — DONE

A tangent-space height-field march in `pbr_frag` with self-shadowing and a silhouette discard past
the first UV tile (spec 4.11). glTF has no height texture, so a `<name>_height.<ext>` sibling of
the albedo is resolved into `Material.height_tex` and turns POM on.

- **Open: grazing silhouettes step without TAA.** The discard is all-or-nothing per pixel and
  quantised by the march layers; TAA hides it, a headless 4x MSAA frame shows it.
  Alpha-to-coverage on the silhouette and finer adaptive layers are the fix. S/M.
- **Open: a POM fade by distance.** S.
- **Open: a gate on the tiled-UV seam** at tile 0's own [0,1] boundary. S.
- **Open: a glTF height convention**, if a standard emerges.
- **Open: real displacement by tessellation**, which GL 4.1 has. L.

#### 4.12 Upgrade SSS to separable screen-space SSS — **M** — DONE

Skin diffuse irradiance written to its own attachment and blurred in screen space, so diffuse
softens while specular stays sharp, plus back-light transmission (specs 4.12, 4.12.1). The blur
is now a pyramid gather (`sss_gather_frag`); the pre-integrated skin BRDF followed in spec 11.13
(aaa B3). See `docs/shaders.md`, Inventory (SSS).

- **Open: transmission driven by a thickness map.** M.
- **Open: a per-material transmission tint** distinct from the scatter colour. S.

#### 4.13 Bloom quality: dual-filter pyramid — **S** — DONE

Jimenez's 13-tap downsample and additive tent upsample on one mip chain (spec 4.13). Lens flare
and finishing followed in spec 11.21 (aaa B7).

---

### Tier 4 — Camera / finishing polish

#### 4.14 Auto-exposure / eye adaptation — **S/M** — DONE

Metered exposure with eye adaptation, first built on the `ssgi` branch; metering is now a
histogram (spec 11.52, aaa E3). See `docs/shader-subsystems.md`, Tonemap / exposure.

#### 4.15 Motion blur — **M** — DONE

McGuire's reconstruction filter from the velocity buffer: tile max, neighbour max, a 16-tap gather
(spec 4.15). Off by default (`--motion-blur`).

- **Open: a separable tile max** (20+20 taps against one 20x20 pass), if its cost ever matters. S.
- **Open: per-object shutter tuning, and motion-blur-aware TAA.** M.

#### 4.16 AgX tonemapping — **S** — DONE

`POSTFX_TONEMAP_AGX`, Wrensch's fitted form, selectable with `--tonemap agx` (spec 4.16). The
default stays Neutral.

- **To decide: AgX as the default.** The user's call.

#### 4.17 Order-Independent Transparency (weighted-blended) — **M** — DONE

Weighted-blended OIT for alpha-blended, non-transmissive meshes (spec 4.17), since replaced as the
default by moment-based OIT (spec 11.17, aaa B6). See `docs/architecture.md`, Scene passes.

---

### Architectural (bigger; do only if needed)

#### 4.18 Clustered / tiled forward (Forward+) — **L** — DONE

Clustered forward light culling on the CPU over a 16x8x24 froxel grid, in std140 UBOs (spec 9.1,
aaa A1). See `docs/lighting.md`, Clustered light culling.

---

## 5. Beyond real-time (if you ever want actual ray tracing)

GL 4.1 has no ray tracing. Three ways to get it, cheapest first:

- **Offline CPU path tracer: days, best quality, not interactive.** Reuses `intersect.c`
  (ray/AABB/triangle) and headless PPM output. Build a BVH over the scene's triangles, trace on the
  CPU, accumulate samples, write an image. No GL limits at all; the easiest way to a reference
  image.
- **Hybrid GL + Metal RT through IOSurface: weeks, interactive, hardware-assisted.** GL keeps
  rasterisation, post and present; a Metal device builds an `MTLAccelerationStructure` and traces
  reflections, AO or GI in an MSL kernel into an IOSurface-backed texture GL imports
  (`CGLTexImageIOSurface2D`) and composites. The normals and roughness G-buffer is the input such a
  pass wants. Costs: a Metal module (metal-cpp and MSL), a second copy of the geometry and its
  acceleration structure, and a GL-to-Metal sync each frame (macOS has no shared fence). Hardware
  RT on M3 and later, compute-unit RT before.
- **A full Metal or Vulkan/MoltenVK backend: months.** Compute, SSBOs, hardware RT and clustered
  shading done right. A rewrite of the renderer, not a mode.

---

## 6. Summary table

Every item is built; the right-hand column is what is still open under it in §4.

| # | Technique | Built | Open |
|---|---|---|---|
| 4.1 | TAA + velocity buffer | branch | catcher, skybox and translucents write no velocity |
| 4.2 | GTAO (visibility bitmask) | branch | aux view-Z averaged at MSAA silhouettes; banding on flat ground |
| 4.2.1 | Specular occlusion + edge-aware AO | 4.2.1, 11.76 | — |
| 4.3 | SSGI | branch | — |
| 4.4 | Reflection probes | branch, 11.70 | relight; `--probe-box` |
| 4.4.1 | SSR quality | 4.4.1, 10.9 | — |
| 4.5 | Volumetric fog | 4.5, 9.5 | — |
| 4.6 | Cascaded shadow maps | 4.6 | — |
| 4.7 | PB sky / atmosphere | 4.7, 9.6 | — |
| 4.8 | Energy compensation | 4.8 | compensation without an environment |
| 4.9 | Screen-space refraction | 4.9 | glass G-buffer; transparents in the refracted image |
| 4.10 | Clearcoat | 4.10 | the coat's own cosine; shared GL state save; mask slot table |
| 4.10.1 | KHR specular + sheen | 4.10.1, 10.7, 10.8 | specular colour texture; sheen roughness texture |
| 4.11 | Parallax occlusion mapping | 4.11 | silhouette AA; distance fade; seam gate; height convention; tessellation |
| 4.12 | Screen-space SSS | 4.12, 4.12.1, 11.13 | thickness-map transmission; transmission tint |
| 4.13 | Bloom pyramid | 4.13 | — |
| 4.14 | Auto-exposure | branch, 11.52 | — |
| 4.15 | Motion blur | 4.15 | separable tile max; per-object shutter; blur-aware TAA |
| 4.16 | AgX tonemap | 4.16 | to decide: AgX as default |
| 4.17 | OIT | 4.17, 11.17 | — |
| 4.18 | Clustered forward | 9.1 | — |
