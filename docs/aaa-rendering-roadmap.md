# Cetra AAA Rendering Roadmap — Master Plan

## Context

Cetra is a C11 forward-PBR engine on a strict GL 4.1 core ceiling (macOS — no compute, no SSBO).
It already has a strong screen-space stack (TAA, GTAO+SSGI, hi-Z SSR, WBOIT, Hillaire sky, CSM+PCSS,
split-sum IBL, parallax-corrected probes). The gap to "AAA look" is not more post effects — it is
**global illumination fidelity, light-count scaling, and volumetric atmosphere**, all architectural.

This master plan is a tiered roadmap of SIGGRAPH-grade features, each feasible as fullscreen raster
passes + LUTs + probe bakes on GL 4.1. The plan will be committed to `specs/` as the umbrella spec;
each tier item later gets its own subplan (feature branch + spec) before implementation.

**Status: Tiers 1-4 are closed, and Tier 5 ran to completion (11.89-11.94, plus 11.95 for two of
what it left behind).** Of Tier 4's rows only
three remain open, one of which (E7) this document argues against itself. **What is left is not a
feature.** This plan asks throughout what the renderer does not HAVE, and by 11.88 the answer is
"very little" — while `apps/forest` rendered at **7.8 fps against a 16.6 ms budget**. A feature list
has no denominator and so cannot see that; Tier 5 (rows 55-61, spec 11.89) is the answer to the
question this document never asked.

**Tier 5's leftovers are down to one.** Rows 62 and 63 shipped as spec 11.95, taking the frame a
further **177.0 -> 169.8 ms** and the lean variant's sampler ledger from 16 units to **12** — which
is the first mechanism here that gives units BACK rather than routing around a full ledger, and the
answer to a wall this document has booked features against for a dozen specs. Row 61 shipped as
**11.96**, which is the last outright BUG this document carried: shadows and LOD were both read from
last frame's transforms. Still open: **55b** (one denominator), and nothing else in Tier 5.

**What Tier 5 returned: 203.7 -> 177.0 ms, and one phase of six moved the clock.** The frame was
shading-bound the whole way, so 11.90's 10x fewer shadow draws, 11.91's deleted upload and 11.92's
1667 -> 1028 camera draws each removed real work and each left the frame where it found it. Only
11.93, which touched shading, moved it. **Four of the six phases had their booked premise deleted by
measurement** — the instance arena's ceiling was zero, 11.92's prescribed fix would have swapped
every canopy onto a different mesh builder, and 11.94's only frame-time candidate turned out not to
be a stall on this driver. Each is closed with the number that killed it rather than removed, so the
next reader gets the measurement instead of the invitation.

Note the machine: every Tier 5 figure after 11.90 is on a host running ~2.5x slower than the one
11.89 measured, on identical geometry. Ratios travel between them; absolute milliseconds do not.

**Tier 4 (Tracks C/D/E below, plus rows 45-49 added to Track B
after the fact) was the frontier until then**, and it is shaped
by a different constraint than Tiers 1-3 were: those items could each be built as another gated
fullscreen pass, and Tier 4's cannot. Four structural walls now decide what is reachable at all; they
are stated before Track C because half the Tier 4 items are blocked on one of them.

**All four have moved since, and none of them the way this section predicted.** Wall 3 was removed
outright (E4), Wall 2 mostly (E5), Wall 4 mostly (E6/11.31) — and Wall 4 was not in the original
three at all, which is this section's own record of having mis-framed the geometry problem. **Wall 1
is the interesting one: it did not fall, it turned out to bite far more narrowly than stated.** Of
the five things it was said to block, two were never blocked, two are blocked only by a choice of
texture layout, and exactly one is blocked outright. Five escapes from it are now precedented and
four of them were found *after* the wall was written. The lesson the wall's own section draws is the
one to carry into any future entry here: "16/16" is a link-time property and blocking is a
per-pass, per-type, per-byte-count property, and this document asked only the first question for the
length of the roadmap.

Everything in Tracks C/D/E — and B10-B14, which joined Tier 4 later — is a **sketch**, in the sense
this document has taught the word: a
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

Ground truth that shapes every design below (library at `cetra/src/`, shaders at `cetra/shaders/`).

**Read the dates.** This section was written before Track A started, and the roadmap has since built
a good deal of what it records as absent — area lights, bent normals, octahedral encoding, froxel
fog, 3D textures. It is re-checked against the code as of 11.52 and the corrected bullets say what
they now are; treat any un-annotated line-number reference as approximate, since the tree has moved
under them.

- **G-buffer**: **6** MRT attachments on one MSAA FBO, source-of-truth table
  `_gbuffer_attachments()` (`cetra/src/engine.c`, `GBUFFER_ATTACHMENT_COUNT`): att0 HDR RGBA16F,
  att1 view-normal+marker RGBA16F, att2 motion+linZ+roughness RGBA32F, att3 albedo+metallic RGBA8,
  att4 SSS-diffuse RGBA16F, **att7 ambient-specular R11F_G11F_B10F** (spec 11.4's split, resolved in
  postfx). **Attachments 5 and 6 are the free pair**, and both are the OIT locations — this line said
  "5-7 free" until 11.4 took 7, which would have put a new target on an occupied slot.
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
- **3D textures are now everywhere** — froxel scatter/integrate volumes, the 32³ aerial-perspective
  volume, cloud shape and detail noise, with `create_texture_3d_float` and
  `create_texture_3d_rgba8_tiling` in `texture.h`. This bullet read "no 3D textures exist anywhere"
  and was the premise for the 2D-array advice below it. What survives: raw `glTexImage3D`, never
  `glTexStorage3D`, which is GL 4.2 — the `GL_TEXTURE_2D_ARRAY` idiom in `material_texture_array.c` and
  `shadow.c` still holds for that reason.
- **Float-from-memory helpers exist**: `create_texture_2d_float` (LTC tables were the first user),
  `create_texture_3d_float` (froxel fog) and `create_texture_2d_array_float`, all in `texture.h`.
  Only `load_texture_from_memory` is still 8-bit. GPU-bake LUT precedent: the BRDF LUT is
  **RGBA16F** 512² (`ibl_bake_brdf_lut`) — the Charlie sheen terms joined it in 10.7, so the RG16F
  this bullet claimed is two channels short — plus the sky LUTs in `sky.c`.
- **Area lights are real since spec 9.2** — LTC, with `ltcTex` on unit 7, `upArea` in the light UBO
  and an area count gating the table fetches. Spec 11.49 then made emissive geometry a *producer* of
  them. This bullet said they shade as point lights, which stopped being true two tracks ago. Karis
  sphere-light lobe widening still applies to all lights.
- **GTAO** is 2023 visibility-bitmask (2 slices × 8 steps, 32-bit sector mask). The bent normal is
  no longer merely *derivable* — spec 11.3 derives it: the AO target is **RGBA16F** since 11.75 and
  `.gba` carries the encoded bent normal, so the channels are neither spare nor discarded. Specular
  occlusion has its own pass, `spec_occ_composite_frag.glsl`, feeding 11.4's attachment-7 split; it
  was a scalar heuristic in the tonemap when this was written.
  **Since 11.76 the default mode no longer consumes the bent normal at all.** The sector mask is a
  directional visibility function, and the reflection lobe is now tested against it *inside the
  sweep*, where the mask exists — attachment 2, RG16F, carrying the estimator's two sums rather than
  their quotient because everything downstream averages them. Collapsing 32 bits to a direction and
  a scalar and rebuilding a cone from the pair was manufacturing the bands it was blamed for: on
  `cornell_rooms` the AO through the artifact is strictly monotone while the cone term plunged to 0
  and rang. 11.76 kept the cone as `--spec-occ bent` so the old answer stayed computable;
  **11.77 deleted it**, because that argument was false — the gate arm never ran the mode, and
  the cone had been falsified through git against the pre-change renderer instead. The ladder is
  off / legacy / split.
- **Probe capture renders the full scene at arbitrary positions** (`reflection_probe_capture`,
  `probe.c:50-263`, camera save/substitute + 6 faces via `ibl_capture_views`) — the DDGI-reusable
  machinery. Octahedral encoding exists (`include/octahedral.glsl`, used by the DDGI atlas this
  document books onto unit 14 twenty lines below — the two statements contradicted each other).
- **Sky**: Hillaire LUTs (transmittance 256×64 once, multiscatter 32×32 once, sky-view 192×108 on
  sun move); `sky_bake()` drives env cube → IBL → key light together. Aerial perspective added by
  9.6 as a 32³ volume — the one sky target rebuilt per frame, since it is the camera's frustum.
- **Fog is froxel-based since spec 9.5**: `froxel_inject/integrate/composite_frag.glsl` plus
  `include/froxel.glsl`, folded with aerial perspective into one composite by
  `postfx_run_atmosphere`. The screen-space half-res march this bullet described, and its
  `fog_frag.glsl`, are deleted. Light data still arrives via `shadow_publish_to_postfx`.
- **TAA**: Halton on `draw_projection[2][0/1]` — 8 samples at full scale, **16 under TAAU**;
  YCoCg 3×3 clamp + Catmull-Rom history;
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
  per-program and pbr_frag never sampled the skybox cube). Since 11.70 that ONE declaration
  serves two features: the clustered specular probes' octahedral columns are a tenant of the
  same physical texture, side by side with the GI region, whose coordinates are untouched
  because `giAtlasSize` was already a uniform. An atlas is a POOL, and the second consumer
  cost a region rather than a declaration -- the material array's lesson, in a second place.
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
`sceneColorTex`), or put the table in `const`/uniform space. See **Wall 1** below.

**The uniform-space escape has turned out to be the widest of the five, and it was priced as the
narrowest.** 11.13 took it for a 16-row skin table and this entry recorded it as a curiosity. Since
then it has carried A1's entire light path, C3's IES profiles **in fact as of 11.57 — a 3968-float
pool at binding 6, holding fully asymmetric 2D distributions the row had deferred for want of a
sampler unit it was never going to spend**, **11.45's `ShoreFilmBlock`**
(a CPU shallow-water solver's per-column tips, 2.3 KB, read by both the sea's lens and the sand's
wetness so they agree by construction) and **11.46's inverse CDF** (64 entries × 3 channels, 768
bytes). The rule that falls out: ask how many BYTES a feature needs in the shader before asking for a
unit, because the answer is frequently kilobytes and a UBO holds 16 of them.

**There is a FOURTH escape, and spec 11.45 spent it on the other program: CONSOLIDATE identical
declarations into one array.** A cap counted in declarations is a cap on how many distinct *shapes*
of data a program reads, so N textures of the same format and size were never N shapes — they were
one shape N times, and an array of N layers is one declaration holding the same images. `water_frag`
had six `sampler2D` cascade fields (RGBA16F 128², identical in every respect) plus two more for the
previous frame; as two arrays that is 8 declarations down to 2, and the program went **16/16 →
10/16** with all 24 goldens at 0 px and the FFT's own impulse error unchanged at 1.9e-7. The
ping-pong that writes them changed in one line — `glFramebufferTextureLayer` for
`glFramebufferTexture2D`.

**Two costs, both small and both worth knowing before reaching for it.** An array carries ONE mip
policy for every layer, so a band that wanted no chain gets one (read at LOD 0, so it is memory and
not a changed read). And anything that read those textures for *precision* has to keep its own
storage: the FFT impulse test's scratch reported 4.8e-4 at fp16 against 1.9e-7 at fp32 for the same
arithmetic, which is the storage's error masquerading as the transform's.

**How much this offers `pbr_frag` is a separate question and the answer is "less".** Its four
`sampler2DArray`s are `materialArray`, `shadowMaps`, `punctualShadowMaps` and `ltcTex`. The two shadow
arrays are the closest pair — both `GL_DEPTH_COMPONENT24` through the same `init_depth_array` — but
they are sized independently (`punctual_size_for` scales with layer count), so merging them means
one of the two changing resolution. That is a quality trade, not the free consolidation water got,
and it should be priced against D0 rather than assumed cheaper than it. *(D0 has since been
refused -- 11.85 -- so the live comparator is 11.95's declaration gating, which returns units per
variant with no quality trade at all.)*

**There is a FIFTH escape, and spec 11.46 is the worked example: TRANSFORM the data so the shader
never needs the original.** The other four move a lookup somewhere cheaper. This one removes the
second lookup entirely, by noticing that a texture and a processed copy of it are rarely both
needed at draw time.

By-example stochastic texturing (Heitz & Neyret 2018) is the case that proves it, because it looks
like it must cost two units and costs none. The method samples a histogram-transformed copy of a
texture and maps the blended result back through the original histogram — apparently a second
texture plus a lookup table. But **the shader never reads the original**, so the transform is
written back over the source at bake time and takes the slot it already had; and the inverse CDF is
64 entries × 3 channels = 768 bytes, which is the second escape's uniform space. `pbr_frag` gained
a whole texturing method at **16/16 → 16/16**.

The generalisation worth carrying: before spending a unit on a derived texture, ask whether the
derivation could be *baked into* the source instead of sitting beside it. That works whenever the
original is not needed at draw time, which is more often than the reflex suggests. It does not work
when both are read in one pass — which is exactly the condition the first and third escapes exist to
test, so the four compose rather than compete.

**There is a THIRD escape, and it is the one nobody had written down: a unit that is idle in the
pass you need it in.** The two escapes above trade on *consumers* being exclusive. This one trades on
*passes* being exclusive, and it is strictly easier to satisfy. Unit 6 is set unavailable at the top
of the opaque pass and stays unbound and unread for the whole of it — `engine->scene_color_this_frame
= false` at `render.c:1123`, commented "pass 1 never uploads or binds a stale refraction source",
with the resolve not running until `:1360`. So a consumer read only in the opaque pass can take unit
6 by `#define` alias at no cost to anything, and its exclusivity argument is *by pass*, which is
stronger than the argument the two tenants already there rely on.

This has been true since the refraction resolve was written, and it went unnoticed for a whole spec
cycle: **D2's surface half was booked as hard-blocked on D0 when it never needed a free unit at
all — it needed a free pass.** The general lesson for this ledger is that "16/16" describes
declarations, not occupancy, and the two are not the same question. Before booking anything as
blocked on D0, check when its read happens.

**Spent, and it works — this is no longer a proposal.** Spec 11.41 put the cloud shadow map on unit 6
by `#define cloudShadowTex sceneColorTex` in the opaque pass, shipped dappled sunlight on terrain,
the shadow catcher and water, and moved no golden that does not carry clouds. Unit 6 now has three
tenants disjoint by pass (opaque / late / OIT accumulate) and `render.c` picks between them in one
place. The ledger stayed at 16/16 throughout.

Occupancy of unit 6 across the frame, which is the map to reason from:

| pass | unit 6 tenant |
|---|---|
| opaque | **nothing — free** |
| late / transparent | refraction resolve (read only where transmission > 0) |
| OIT accumulate | moment atlas |

**Unit 6 is the ONLY globally idle slot, and "globally" is the load-bearing word.** A full sweep of
every binding site:

| unit | tenant | bound during the opaque pass? |
|---|---|---|
| 0, 1, 2, 5 | albedo, normal, masks, emissive | yes, effectively every draw |
| 3, 4, 8 | clearcoat normal, POM height, sheen | **per-MATERIAL** — only where that material has one (`render.c:241, 246, 257`) |
| **6** | scene colour | **no — explicitly made unavailable** (`render.c:1123`) |
| 7 | LTC tables | yes, unconditional once the tables exist (`ltc.c:53`) |
| 9, 11, 12 | Charlie env, irradiance, prefilter | yes, unconditional once IBL exists (`ibl.c:750-764`) |
| 10, 15 | CSM, punctual shadows | yes, wherever shadows exist |
| 13 | BRDF LUT | yes, every frame |
| 14 | skybox / GI atlas | only with a converged volume (`gi_volume.c:112`) — idle by default |

Units 3, 4 and 8 look free and are not usable. They are per-*material*, so a global texture aliased
there would vanish on any draw that happens to carry a clearcoat, height or sheen map — a
content-dependent hole, which is not the same thing as exclusivity.

**And an alias must match the declared sampler TYPE**, because `#define x sceneColorTex` makes `x`
*be* that `sampler2D`. That constraint decides more than the pass does:

| consumer | pass it reads in | fits the idle slot? |
|---|---|---|
| cloud shadow map | opaque | **yes** — a 2D R16F onto a `sampler2D` |
| decal atlas | opaque | ~~**only as a flat 2D atlas** with computed tile UVs~~ — **moot: D1 shipped (11.73) taking neither the slot nor a flat atlas.** The row asked which consumer fits an idle unit, and the answer was that a decal image is one of the scene's per-texel material images, so it is a TENANT of unit 2's array — a layer index, no declaration, and no exclusivity argument to audit. The slot was never free anyway: 11.41 holds unit 6 for the whole opaque pass, so this table and the cloud-shadow row above it were two candidates for one thing |
| light cookies | opaque **and** late | same type problem, and they want transparent receivers too |
| froxel volume from the transparent pass | late | **no.** A `sampler3D`, in the one pass where refraction is live. Blocked twice over |

So the honest remaining customer for a freed unit is the froxel-in-transparent item — **and since
11.73 it is the ONLY one.** D1 was the other name on this list and it shipped without a unit at all,
which leaves this whole section describing a budget with one booked spender. Note also a SECOND
conditionally-free routing on unit 6: the refraction read is
guarded by `transmission > 0`, so a non-transmissive late-pass draw does not touch it. That is real
capacity and should be treated as a last resort — `pbr_frag.glsl:239-243` already calls the present
arrangement "forced rather than clever", and a third routing on one unit is where it stops being
auditable.

**Unit 11 is the one with a way out, and it is worth only one unit.** `pbr_frag.glsl:1766` already
selects between the irradiance cube and the GI atlas (`giEnabled > 0 ? giSampleIrradiance(...) :
texture(irradianceMap, N).rgb`), so those two consumers are *already* mutually exclusive in use and
differ only in sampler TYPE. Making irradiance a `sampler2D` octahedral tile is what lets one
declaration serve both. Explored in detail under **D0**, including why the entry's "free two" is one,
why its 0-px acceptance bar cannot be met, and why unit 4 (POM height) is a real but premature second.

## Track A — Direct Lighting & Global Illumination

### A1. Clustered forward light culling (Olsson et al. 2012) — Effort L — **DONE (spec 9.1)**
Scale past 64 lights and delete the per-node uniform storm. **UBOs, not data textures** (zero
texture units; kills the 13-glUniform-per-light-per-program-per-node hot loop). Three std140 blocks,
each <16KB min guarantee: `LightsBlock` (4 directionals outside clustering + 128 packed
point/spot/area lights, 6 vec4s each), `ClusterBlock` (16×8×24 grid, packed offset|count uints),
`ClusterIndexBlock` (shipped at **6144** 16-bit indices, not the 4096 sketched). CPU cluster assignment in new `light_cluster.c`
(frustum cull → tile X/Y + exponential-Z slice ranges, Doom-2016 slicing); 3 `glBufferData` orphans
per frame. **Deletes** `_update_program_light_uniforms` (`render.c:91-153`) and retires
`get_closest_lights` from the render path. Only pbr_frag declared `lights[]` when this was
written, so migration scope was one shader. **Four** include `lights_ubo.glsl` now — `pbr_frag`,
`pbr_vert`, `froxel_inject_frag` and `include/froxel.glsl` — which is the consumer set anything
touching the light layout has to move against. Legacy path kept behind `--clustered` (default off) during the branch for the
0-diff gate; final commit flips default and deletes legacy. Directional-only raiden baseline expected
byte-identical (directionals bypass clustering, unchanged evaluation order).
New: `ubo.c/h`, `light_cluster.c/h`, `include/lights_ubo.glsl`. CLI: `--point-light-grid N`;
ImGui cluster-heatmap debug view.
**Owns foundations:** UBO machinery + std140 packing + binding registry; packed light representation;
CPU light-culling module. **Depends on:** nothing.

### A2. LTC area lights (Heitz/Dupuy/Hill/Neubelt 2016) — Effort M — **DONE (spec 9.2)**
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

### A3. Screen-space contact shadows (Uncharted 4-style) — Effort S — **DONE (specs 9.3 + 9.4)**
A per-pixel march along the key light in a new postfx pass, slotted between depth/normal resolve and
GTAO. It shipped 8-step at AO res in 9.3 and 9.4 took it to 16-step at full internal res, which this
line went on claiming for eight specs; C5 marches the map-less local lights through it too. Same-frame application: composited in tonemap as an independent
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

### A5. Bent-normal specular occlusion from GTAO (Jimenez 2016) — Effort M — **DONE (11.3 + 11.4), REPLACED as the default in 11.76, and DELETED in 11.77**
**Read this heading literally: bent-normal specular occlusion is no longer in the engine.** The
entry below is the 11.3/11.4 design, kept as the record of what was built and why it went — it is
no longer reachable by any flag. What changed
is that the cone was the wrong consumer of a bitmask: 11.3's own "look review found mottling that is
architectural, not tunable" was the first sighting of it, 11.75 fixed a hard cliff in the same
function, and 11.76 removed the reconstruction instead of tuning it. Two things this entry asserts
are now false for the default path — **"Widen existing AO targets rather than second MRT"** (the
lobe sums are a second MRT, ~8 MB at 1080p, because they are a different quantity rather than more
channels of the same one) and the `acos(sqrt(1-ao))` aperture (there is no aperture; the lobe is
counted in sectors). The measured attempt to keep the cone and fix its aperture from the bent
normal's LENGTH is written up in spec 11.76 and failed on contact washout — worth reading before
anyone tries it again.
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
**That foundation is now UNCLAIMED, and the reason it was booked is the reason to doubt it.**
Since 11.77 nothing in a normal frame reads the bent normal — debug view 9 is its only consumer.
The listed successors want it because it is a cheap directional signal; 11.76 measured that a
collapsed mean direction loses to the full 32-bit sector mask for exactly that purpose, and the
mask is right there in the same shader. So none of these three should assume the bent normal is
what they want. **This is not an argument for deleting it either** — it removes the reason to
keep it, and what settles that is cost.

**MEASURED, and the answer is keep it for now: ~0.13 ms.** Deleting the production loop and
dropping `AoOut` to R16F moves `gtao sweep` 1.793 → 1.733 ms and `ao denoise` 1.863 → 1.797 ms
(minimum of 3, `cornell_rooms --no-bloom --no-ssr` at 1600x1000). That is ~3.5% of the AO chain
and ~0.3% of the frame, against an 18.9 ms opaque pass — real, but not worth a diff across the
write path, blur, accumulation, upsample and debug view. It scales with AO resolution, so
re-take it before targeting 4K, and take it again if the AO chain is ever being optimised for
other reasons: 3.5% of it feeds nothing.
**The default profiler cannot see this and will mislead you.** It drifted 2.4x run-to-run and
returned a flat 0.000 at 3200x2000 (the heavy-frame unavailable path). Dropping bloom and SSR is
what makes the frame light enough for the queries to retire — spread falls to 1.9%, and two
baseline groups an hour apart agreed to 0.4%.

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
**Residual — resolved by 10.3/10.4, and every part of the diagnosis below was wrong.** The text
said: a one-texel bright hairline, inherent to front-face culling storing the occluder's far
surface; back-face culling trades it for acne; untried candidate, receiver-plane depth bias.

10.3 shipped **near-side storage for every punctual type**, one policy with no per-type split, so
the depth pass culls back faces now. The stated mechanism was tested directly — a negative epsilon
in the shader, then a negative polygon offset — and *neither moved the line by a pixel*, which a
depth tie would have. The real cause was a near-silhouette sliver missing texel centres, so the
comparison hit cleared texels. And the "untried candidate" was tried and shipped:
`include/receiver_plane.glsl`, consumed by this path and by the cascades (10.4 phase 3, 10.5 phase
2).

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

### B1. Froxel volumetric fog (Wronski 2014 / Hillaire 2016) — Effort L — **DONE (spec 9.5, then
9.5.1, 11.11, 11.12, and 11.39/11.40 which added three further media to `froxel_inject_frag`)**
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
CLI: `--fog` (opt-in; fog is OFF by default). The sketch said `--no-fog-volumetric` "the volume is
the default" — that flag never existed, because the screen-space march was deleted rather than kept
behind a toggle. `--no-fog-volumes` is a different feature: spec 11.39's local volumes.
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

### B2. Volumetric clouds (Schneider/Vos HZD 2015, Nubis) — Effort XL — **DONE (spec 11.0)**

**As built, and four of the sketch's claims did not survive.** `cloud_reproject_frag.glsl` was never
written — temporal blending happens INLINE in the march pass. There is no 128² curl field; the noise
is `shape_tex` 128³ plus `detail_tex` 32³ and nothing else. The "hard" dependency on B1's
`create_texture_3d` was not the dependency: the noise needed a *new* tiling sibling,
`create_texture_3d_rgba8_tiling`. And **cloud shadows are not a deferred follow-up — both halves
shipped**, into the froxel fog (11.39) and onto the ground (11.41: `pbr_frag`, the shadow catcher
and water's caustics), with `--no-cloud-shadows` and on by default.

The file set is larger than listed: also `sky_background_clouds_frag.glsl`, `sky_env_clouds_frag.glsl`,
`cloud_noise_debug_frag.glsl`, `include/sky_radiance.glsl`, `cloud_shadow_frag.glsl`,
`include/cloud_shadow.glsl` and `sky_clouds.c`; CLI also gained `--cloud-density` and `--cloud-wind`.
What the sketch got right: `noise_worley3`, dual-lobe HG + powder, and the env bake, so IBL and
probes really do see clouds.

*Original sketch follows.*
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
`--no-sss`, and only on materials that opt in — one fixture when written, **three now**
(`skin_curvature_fixture`, `skin_area_fixture`, `skin_shadow_fixture`, the latter two being the
fixtures B3.1 and B3.2 were written against). Fixing the blur removed the
shortfall B3 existed to fill, which is the composition law working correctly, not a regression. The
machinery, the fit tool, the fixture and the gates all remain correct and are what B3.1 and B3.2
build on; but anyone reading this row expecting B3 to be carrying a terminator today should not.

Curvature-aware diffuse falloff under the existing screen-space SSS. **Analytic fit, no LUT** —
pbr_frag units 7/9 go to LTC, and Penner's lookup has well-behaved analytic approximations (~10 ALU
on skin pixels, fully deterministic). Curvature = `length(fwidth(N))/length(fwidth(P)) *
curvature_scale` (silhouette noise smoothed by the downstream SSS blur; artist curvature mask
reserved as future material-array layer). Wiring: when `sssEnabled && subsurface > 0`, the Lambert term
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

**OPEN, and found by accident in 11.78: the reconstruction loses a near-opaque layer badly.**
Building that spec's fixture wanted an ALPHA_BLEND pane at alpha 1.0 — a byte-identical twin of an
opaque pane, differing only in `alphaMode` — and it renders at about a FIFTH of its value: measured
**38.9 against the 180.2 it owes, and exactly 180.20 under `--no-oit-moments`**. At alpha 0.5 the
loss is 4.4%. The mechanism is `MBOIT_OVERESTIMATION` charging a lone layer a quarter of its own
absorbance while `mboitAbsorbance` blows up as alpha approaches 1, so the weight collapses. This is
the **default-on** path, and it means any near-opaque blend surface — a window pane, a painted panel,
a decal card authored BLEND rather than MASK — renders far too dark. Nothing in the suite sees it:
the OIT arms read a card stack at alpha 0.15, and `oit_sphere_moments` is the only golden on this
path. Arguably a larger visible defect than the one 11.78 fixed, and it is nobody's row yet.

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

### B9. Aerial perspective (Hillaire 2016) — Effort ~~S~~ **M — DONE (spec 9.6)**. The S estimate
assumed B1's machinery made this small; the sequencing table records why that answered the wrong
question.
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

### B10. Night sky — stars — Effort S/M — **DONE (spec 11.79)**

*As built: the row's sketch held — the insertion point, off-by-default and both
predictions (27 goldens 0 px, daylight an exact 0 px) — while the star MODEL was rebuilt
three times against photographed defects, each recorded in the spec's as-built notes: a
fixed angular Gaussian reads as flat white circles (cores are PIXEL-sized off `fwidth`
now, with a 3×3 cell gather because no bake-time margin contains a resolution-dependent
footprint), the sin-fract hash STRINGS stars on the cell lattice (E10's own numbers, on a
new consumer; shipped PCG under E10's revival clause), and a flux-proportional glare wing
saturates into balls (bounded by the flux tail cap, after a hard cap and a soft knee were
both rejected on look). Six gate arms, each falsified by hand against its named mutation;
the horizon arm's R/B leg is the load-bearing half, and the extinction reading lands
within 10% of the airmass arithmetic.*
Greenfield: `star|night|moon|celestial` has zero first-party hits, and this roadmap had
neither booked nor rejected it. The below-horizon atmosphere is already correct and already
gated — spec 4.7's checklist includes "below-horizon night (no NaN speckle)" — but Hillaire
models scattered SUNLIGHT only, so what it renders at −10° is a black void.

**The insertion point is `sky_radiance.glsl`, beside the sun disc, and it decides most of the
design.** The sun disc is already an analytic point source added to the LUT sample and
attenuated by `transmittanceToSky` — a star field is that pattern a second time, and four
properties fall out of the one line: extinction and reddening at the horizon come from the
existing transmittance LUT; the ground cut is free (`transmittanceToSky` returns zero when
the ray hits ground); clouds occlude for free (the clouds variant computes `sky * cloud.a`,
so a term inside `skyRadiance` sits under the deck); and containment is structural, since the
include has exactly two consumers — the two background programs, at 2 and 3 of 16 sampler
declarations. The env/IBL path must NOT carry stars, and that rule already exists in the
codebase: `sky_env_frag.glsl:6-11` refuses the 0.53° sun disc because a point source aliases
the prefilter into fireflies, and stars are smaller.

**Procedural behind `starRadiance(vec3 dir)`, and a baked catalog texture is rejected on
arithmetic**: a 2048-wide equirect is 0.176°/texel against ~0.03°/px on screen, so it is
always magnified and every star lands as a ~5 px blob; sharpness at screen density needs
~11k wide, ~350 MB. What the procedural field must get right is the magnitude distribution
(counts go as 10^(0.6m), so flux is Pareto −2.5 — a few dominant stars over a faint wash),
restrained Planckian colour, and a Milky Way band, which no point catalog gives anyway (it
is unresolved stars) and which carries more of the look than constellations do. The upgrade
path for real constellations is instanced point sprites (~9k quads), a body swap behind the
same signature — not the texture, which pays the asset cost AND the blur.

**The fade is half emergent, and the half that is not is an engine invariant.** Auto-exposure
is darkening-only by explicit design (`exposure_auto_gain` caps at 1.0 — "brightening is what
it must not do"), so stars vanishing in daylight is free, and stars appearing at night is a
CPU-side elevation ramp uniform beside the hard-coded `sunIntensity` — a visibility ramp
standing in for an adaptation the engine refuses, and the comment should say so. A
`mat3 starFrame` uniform (latitude → pole altitude, hour angle → rotation) is what makes B11
turn the sky instead of freezing it.

**OFF by default in the library, and three gates are protected by nothing else**:
`cornell_rooms` renders at −10° and would take stars across its golden, `dither-band-inverse`
reads flat runs in `aerial_fixture`'s sky gradient that stars would shorten, and
`EMISSIVE_SPEC_SKY` pins −10° assuming a low-variance backdrop. The three things this item
deliberately left out each have their own row now: **B12** the night-sky floor, **B13** the
moon, **B14** the Purkinje shift — a deferred list buried inside a DONE entry is exactly the
"never rows here" failure D3's water follow-ups recorded.
**Depends on:** nothing. **Wall 1:** unaffected — the background programs have 13-14 free units.

### B11. Day/night cycle — Effort M — **DONE (spec 11.81)**

*As built: the time-sliced env bake landed as the row describes, and its acceptance bar —
a sliced re-bake byte-identical to the atomic one — reads 0 px on two fixtures. Three
corrections the row could not have known. The cost is INVERTED from "one face per frame,
one mip per frame": the six env faces are ~10 ms and ~90% lives in the two prefilter
chains, so the schedule is weighted work items and mip 0 splits by FACE. The per-frame
peak is one mip-0 face and is irreducible at face granularity (measured 4.74 ms at
800x500), so the row's implied "a few ms" wants sub-face slicing rather than a smaller
budget. And the tick belongs in the ENGINE loop before the shadow pass, not in an app's
render callback as the row sketched — that placement ships a one-frame shadow/sky
mismatch. Six review agents found eight defects and three false claims in this work after
the author had declared it green; the spec's as-built notes list them, including one gate
arm that failed its own falsification (the sky fixture cannot see the Charlie chain).*
An animated sun, which today would be a slideshow: a sun move re-bakes the sky-view LUT, the
env cube AND the GGX prefilter, priced at **0.11 s** by the config-snapshot restore
(`config_snapshot.c:303-314`) — per frame that is ~9 fps. The GUI slider survives it because
a drag releases; a cycle never releases.

**The shape is a time-sliced env bake, and the constraint that picks it is that STATIC must
not get worse.** One cube face per frame, then one prefilter mip per frame, then swap —
~13 frames of latency, invisible at any sane cycle rate, same texels at full quality. The
rejected alternatives: cheapening the bake (smaller cube, fewer mips) degrades every static
frame the goldens hold; threshold stepping (re-bake per 0.5° moved) pops on reflective
surfaces, and tree's night scene is mostly sea. A stationary sun must quiesce byte-identical
to today's path — the goldens-0-px prediction is the acceptance bar. Half the machinery
already exists: `sky_bake_view_lut` is already factored out (the 192×108 LUT is fine every
frame), CSM refits per frame regardless, the GI volume already updates as a gradual sweep,
and the key light already tracks elevation. The DRIVER lives in the app (tree's frame
callback has `render_delta` in hand); the amortisation lives in `sky.c`.

**Three consequences to book, not fix here.** The cycle forces B12 (the night floor) — it
drives through sunset every run, and past the key-light fade the bottom half of the frame
is black. Probe sets cannot follow the sun (`scene_environment_changed` defers set relight by
design), so a probed interior under a cycle holds capture-time lighting. And shadow motion is
not in the motion vectors, so creeping shadows smear slightly under TAA — negligible at
realistic rates, visible at fast time-lapse.
**Depends on:** B10 (soft — a cycle without stars has a black night; B10 without B11 just
holds `starFrame` constant) and B12 (see above — sequence the floor FIRST, so the cycle
lands on a working night rather than driving through a black one). **Wall 1:** unaffected.

### B12. Night-sky floor — Effort S/M — **DONE (spec 11.80)**

*As built: the row's one-term-in-the-LUT sketch held exactly, and the spec's as-built
notes record the three deviations found on contact — the constants are C-only because a
premultiplied uniform leaves no GLSL copy to sync; the config rows reuse
`_apply_sun_angle` outright (already change-detected, already both flags); and the
transmittance tint rides at the stars' 0.35 saturation, without which no brightness level
looked right (full extinction browns the whole lower sky). Five arms, each falsified; the
fog arm's bar was PLACED by killing the zenith twin (1.043 live / 0.995 dead) before the
arm ever passed, and the ground arm — the env→IBL path, the feature's reason to exist —
was proven against the wrong-layer reimplementation. Level user-calibrated: 0.012
invisible, 0.06 washy and brown, 0.03 with the desaturated tint shipped at brightness 1.0.*
The radiance of the sky between the stars, which Hillaire cannot produce because it models
scattered SUNLIGHT only: airglow (the upper atmosphere's own chemiluminescence), zodiacal
light, and integrated starlight, together ~2e-4 cd/m². Without it the 11.79 night is stars
over a void — the key light fades out over 3°→0° (`sky_apply_sun_to_light`), the env cube
goes black with the sky, and the WORLD below the star field is unlit, which every night
screenshot of `apps/tree` shows (a day-bright sea against a night sky is the same mismatch
from the other side).

**The design constraint that separates it from B10: this one MUST reach `sky_env_frag`.**
The whole point is lighting the GROUND through the env cube and the IBL, and the smooth
term is safe there — the firefly rule that bans the sun disc and the stars from the env
bake bars POINT sources aliasing the prefilter importance sampler, not a near-constant
floor. So it is a lighting change, not a screen-space one: env bake + background + the
`zenith_radiance` / fog-ambient publish, ramped in by the same civil-twilight window the
stars use. **And the exposure honesty applies a second time**: `exposure_auto_gain` only
ever darkens, so no adaptation will lift a dim world — the floor's authored radiance IS the
night brightness, a look-calibrated constant like the star ramp, and should be commented as
such rather than dressed up as physics.
**Depends on:** nothing (B11 depends on IT). **Wall 1:** unaffected — no new sampler
anywhere; it is a term in shaders that already run.

### B13. The moon — Effort L — **DONE (spec 11.82)**

*As built: the row's framing was right — a LIGHT not a backdrop, the sun's split applied
verbatim, and the time-sliced bake did inherit a second slow body for free (zero changes
to the LUT chain, the slicer or the bake cadence). Three things it could not have known.
**Phase is not a parameter**: it is `acos(-dot(moon, sun))`, so the terminator is built
from the real sun direction and a crescent facing the wrong way is unrepresentable rather
than merely discouraged — which deletes a field instead of adding one. **A full moon is
not a Lambertian sphere**: regolith backscatters, so the quarter moon is a ninth of full
and not a half (Krisciunas & Schaefer 1991), and the lit face is FLAT — shading it by
cos(theta) renders a snooker ball. And **the disc size is shared with the sun**, which
subtends 0.53 degrees against the moon's 0.52. Fourteen gate arms, two anti-vacuity pairs;
the falsification round caught one of them claiming something it did not test, which the
spec records rather than quietly fixing.*

*And a fourth, found only after the spec had closed green: **the feature was correct and
did not look like the Moon**, which took seven more commits to fix and is the finding worth
carrying. The arms measure properties, and "looks like the Moon" is not one — so a suite
that passes says nothing about it, and the one stage that asks the question was scoped as a
tuning step with two float constants for output. What was actually wrong: a life-size disc
is twelve pixels and invisible in the frames it was being judged from (hence `moon_size`
plus an aureole whose width divides by its SQUARE ROOT, since a linearly-scaled halo on a
12x disc is 50 degrees and floods the star field); procedural maria are a plausible cratered
world and not the Moon, because where the seas are is a historical accident with no
generating process to model (32 KB of published selenographic centres, generated, not
fetched); painted craters look identical from every sun angle, so they became a HEIGHT field
under Lommel-Seeliger shading; that change then DELETED their albedo and blanked the full
moon, because relief and albedo are not alternatives — relief carries the terminator and
albedo carries full phase; and the crater population was capped at ~27 candidates per octave
by the per-pixel lattice itself, which no tuning reaches a saturated field from. It is baked
now — ~43,000 craters into one 2048x1024 image at 0.09 s, which is FASTER than the lattice
it replaced as well as better. 27/27 goldens stayed 0 px through all of it, which is what
made throwing away the disc implementation twice a safe thing to do.*
The dominant natural night light — ~0.25 lux at full, ~250× the starlit floor — and the
reason most game nights are readable at all: moonlit night is vastly easier to light than
moonless. **A LIGHT, not a backdrop**: a disc with phase and earthshine on the dark limb,
plus a real casting directional whose intensity and colour follow phase and altitude.

The sun's own split applies verbatim and is the reason this is tractable: the disc is
analytic in the background shaders only (the same env-bake firefly rule — a 0.5° disc
aliases the prefilter, so its direct energy ships as the analytic light instead, exactly as
`sky_env_frag`'s header records for the sun), the light rides `sky_apply_sun_to_light`'s
pattern with its own elevation/azimuth pair, and the shadow system already holds up to 3
casting directionals. The disc itself is the sun-disc pattern a third time in
`sky_radiance.glsl`. What is genuinely new: phase (drives both the terminator on the disc
and the light's intensity), a second transmittance-tinted directional, and the authoring
surface (`environment.moon`, flags, config rows — the 11.79 plumbing shape a second time).
**Order note:** arguably ahead of B11 on look value — a moonlit static night beats a cycled
black one — and B11's time-sliced bake would inherit a second slow-moving light for free.
**Depends on:** B12 (the floor is what the dark limb and shadowed ground sit against).
**Wall 1:** unaffected.

### B14. Purkinje / scotopic shift — Effort S/M — **DONE (spec 11.83)**

*As built: the row's framing was right about WHAT and wrong about WHERE and HOW, and both
corrections are recorded here because the row stated them confidently.*

***"Lives in `tonemap_frag`'s finishing stack"* is false**, and this row's own parenthetical
said so three lines later without the headline ever being corrected. The finishing stack is
entirely downstream of `toneSelect`. It splices into `sceneToToned`, on the file's THIRD siting
rule: the gamma line orders stages by whose data space they were authored in, while chromatic
aberration is already sited by the OPTICAL CHAIN as a lens effect "before the sensor" — so the
sensor's spectral response lands after the lens and before the response curve, with grain (already
"sensor noise") as its other half.*

***"driven by the metered luminance the exposure system already reads back to the CPU — no new
measurement machinery"* does not work.** `adapted_luminance` is never written under
`--no-auto-exposure`, which is all 27 goldens and ~60 gate arms, so a term keyed to it would have
been structurally invisible to this repo's own instruments and inert on every pinned scene. Nobody
had checked. The fix was small and better: split `postfx_run_metering` so the three draws run
whenever the shift is on while the blocking readback stays gated on `automatic`, and sample the
1×1 on unit 7 — where its own ancestor lived.*

*And a third thing the row could not have known: **the weight needs TWO gates multiplied**, because
this engine's whole day-to-night range is 4.2 stops where reality is ~17. Measured, the day frame's
darkest half sits BELOW the night frame's brightest third, so no per-pixel threshold separates a
shaded corner at noon from a whole night frame — and without the global veto a noon shadow takes
wLocal 0.997. Every threshold is therefore a LOOK constant, with `purkinjeBiasEV` as the one knob
that migrates them if 10.2 phase 5 lands. Fifteen gate arms; the falsification round found TWO that
were not testing their own claim, including one blind spot that was structural — every colour arm
read chromaticity, which the rod weights divide out of, so "reds go near-black" had no coverage at
all until an arm read luminance instead.*
The perceptual half of night: in dim light the rods take over, colour drains, everything
shifts blue and reds go near-black — it is why moonlight "looks" blue when its spectrum is
sunlight's. Kirk & O'Brien 2011 is the standard model. This is what makes a dark frame read
as NIGHT rather than as an underexposed day photo, and it is the difference no amount of
radiance tuning in B12/B13 can supply.

Lives in `tonemap_frag`'s finishing stack, driven by the metered luminance the exposure
system already reads back to the CPU every frame — no new measurement machinery. Two things
make it a hypothesis row rather than a small task: its POSITION in a stack with two
standing contracts (dither last, LUT after `displayEncode` — a retinal response most
plausibly belongs before the tonemap operator, which is a third positional claim to argue),
and its blast radius — every dim frame in every app moves, so it needs an opt-in, a gate
arm, and defaults chosen against real night frames. **Judge it only after B12 (and ideally
B13) put those frames on screen.**
**Depends on:** B12 (soft — there is nothing to calibrate against until a night frame
exists). **Wall 1:** unaffected — the tonemap program declares 12 of 16.

### B15. Water at night — Effort M — **DONE (spec 11.84)**

**Shipped as two halves, and the diagnosis below was right about both.** The pick became the
brightest directional ranked by `intensity x peak channel`, and the in-scatter became
`scatterAlbedo x incident + scatterGlow`. Seven new arms in a `water-night` group — the
suite's first night-water coverage of any kind, since no arm in `water` or `beach` had ever
rendered a sea below +22 degrees.

**Three things this row did not predict, all worth carrying.**

**The recalibration was free.** The row prices this M because "2 goldens and 35 arms are
calibrated against the current in-scatter", and that turned out to cost nothing: dividing each
fixture's authored value by its own scene's daylight incident reproduced daylight to within
**one 8-bit code** (both goldens PAE exactly 1/255), and the 35 arms read within 1% of their
old numbers. **No bar was re-placed.** What made it free is that the old value was the PRODUCT
of an albedo and a light, so dividing the light out is exact rather than approximate — a
migration by division, not by taste.

**One value could not do both jobs, so there are two fields — and then nothing used the
second.** The row assumes the authored colour simply "becomes the albedo it tints", which is
right for the fixtures. `apps/tree` looked like the counter-example (a 0.8-degree sun delivers
almost nothing, so a pure albedo renders near-black) and `scatterGlow` was added for it. It then
went the other way: raising `moon_brightness` lifts the same sea with REAL LIGHT, which keeps the
in-scatter tracking its illumination, and tree ships with the glow at zero. **So `scatterGlow`
has no in-tree consumer but the arm that tests it.** Kept, because an author with no way to say
"this sea glows" is the missing mechanism this item is about — but it is an escape hatch, not a
worked example, and a later spec citing it should know that. The general lesson survives intact:
the defect was a MISSING MECHANISM rather than a wrong value — nobody chose luminous night water,
and the same absence made a dark realistic sea unreachable too.

**A fixture can be structurally unable to show the thing you are testing.**
`water_fixture`'s geometry is EMISSIVE over a black base, which is what makes it a good
absorption instrument and what makes its water band read 0.2207 at midnight against 0.2478 at
noon no matter what the sea does. Every night arm here is therefore a twin DELTA, never a
brightness read.

---

### B15 (original entry) — Effort M
Render `tree` at a sun elevation of −12 and the frame is a black tree silhouette against a
full star field, over a sea that is **flat daytime turquoise**. Everything else in the shot
went to night correctly. This is B13's best image and the renderer cannot currently produce
it — `tree` and `forest` are mostly sea, so the one shot that would sell a moon hardest is
the one that is broken.

**Two independent defects, and only the first is what makes the frame look wrong.**

**The in-scatter is a constant, not a response to illumination.** `water.c:1851` uploads
`water->scatter` raw and `water_frag.glsl:1056` spends it as
`waterScatter * preExposure * (1.0 - T)` — an authored colour in absolute scene radiance,
multiplied by nothing that knows how much light is falling on the water. So it behaves like
an EMISSIVE: when the sun sets and every other term in the frame collapses, the water body
keeps its daytime colour at full strength. Note the file's own header already says this out
loud ("the authored scatter colour" sits in its list of absolute-radiance inputs) — it is
a stated design that was only ever exercised in daylight, not an oversight in the code.
The fix wants a real answer rather than a fudge factor: in-scattered radiance is
(incident irradiance × single-scatter albedo), so it should be DERIVED from the light
reaching the surface — the same sky/IBL term the reflection already samples — with the
authored colour becoming the albedo it tints rather than the radiance it emits. That
changes every daylight water frame, which is why this is M and not S: two goldens
(`water_fixture`, `water_submerged`) and 35 arms across the `water` and `beach` groups are
calibrated against the current behaviour.

**Water sees exactly ONE directional, and picks the sky's SUN by name.** `water.c:1876`
prefers `scene->sky->sun_light`, deliberately — spec 11.41 put that there because scanning
for the first directional found whatever the scene file listed first, so caustics focused
from one light while the deck occluded another. At night that preference backfires: the
sun light still EXISTS below the horizon, so `sunAvailable` stays 1 and the fallback scan
never runs, while `sky_horizon_fade` (`clamp(elevation/3, 0, 1)`) has zeroed its intensity.
Water ends up holding a direction pointing at a sun underground and `sunRadiance` of exactly
zero, and never looks past it to the moon, which is a second directional. Everything keyed
off that term goes to zero with it: the specular glitter, the Cox-Munk sun lobe (11.42) and
the caustics. **A moonlit sea's defining feature is the glitter path**, so this is the half
that would make the picture good rather than merely correct.

The selection wants to become "the brightest available directional" rather than "the sky's
sun", which keeps 11.41's fix intact (it was about not picking arbitrarily, not about
picking the sun specifically) while letting the moon inherit the term when it is the only
light left. It reaches the caustics, the glitter's cascade lookup, the foam's direct term
and `sunAvailable`'s meaning, so it needs its own arms — a night frame where the glitter
path points at the moon, and a daylight frame that is 0 px against today.

**Order note:** arguably BEFORE B14. That row already says to judge Purkinje against real
night frames; a frame with a lit tropical ocean in it is not one, and tuning a rod-vision
model against it would calibrate the perceptual model to compensate for a radiometric bug.
**Depends on:** B13 (there is no second directional to hand water until the moon exists).
**Wall 1:** unaffected — `water_frag` declares 11 of 16 since 11.45, and neither half of
this needs a new one.

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
cookies, detail/wetness maps, a cloud shadow map, and sampling the froxel volume from
the transparent pass. **"IES textures" was on this list and should never have been** — 11.57 shipped
the full 2D distribution in a UBO block, so the entry was naming an implementation nobody had chosen
and pricing the feature against a wall it does not touch. It does NOT block anything that lives in postfx (which has its own budget and
is nowhere near full), anything that fits in a UBO, or anything small enough to be a `const` table.
Note which Tier 4 items below are postfx-only or UBO-only: those are the cheap ones, and they are
cheap *because* of this wall, not in spite of it.

**That list overstates the wall by one item, and its remaining entries are not interchangeable.**
A unit buys a *declaration*, and a `sampler2DArray` declaration holds many layers — unit 2 already
serves six masks that way, and `material_texture_array.h` says in as many words that the array exists to "scale
to new data by adding a layer". So:

| blocked item | what it actually needs |
|---|---|
| detail / wetness maps | **nothing.** Per-texel material data at canonical size *is* the mask array. Never blocked |
| cloud shadow map (D2's surface half) | **nothing, and this is now SHIPPED rather than argued** (11.41). Read in the opaque pass, where unit 6 is idle — see the ledger's third escape |
| decals + light cookies | **a flat 2D atlas gets them the idle opaque slot too**; only the `sampler2DArray` form needs a freed unit, since every array in the shader (2, 7, 10, 15) is read during opaque shading. Cookies additionally want transparent receivers, where the slot is not free. **And a narrow slice of the ground half needs no texture whatever** (11.68): a mark MADE OF a layer the surface already carries is a splat-weight override driven by segments in a UBO — capped at 4 polylines on one layered world-XZ material per scene |
| froxel volume in the transparent pass | **a real unit, no exceptions.** Read in the LATE pass where refraction is live, so no idle slot exists, and a `sampler3D` cannot share a 2D declaration with anything |

So of the five things this wall was said to block, **two are not blocked at all**, two are blocked
only by a choice of texture layout, and exactly one is blocked outright. **Since 11.68 the decal
row is split further still**: a narrow slice of its ground half is shipped and needed no texture at
all, so what the layout choice governs is the picture-shaped remainder and everything denser than a
handful of marks. The sweep in the ledger
section above has the per-unit occupancy and the type constraint that produces this.

**A sixth item was never on the list and would have been assumed blocked: by-example stochastic
texturing.** It reads a transformed copy of a texture plus an inverse histogram table, which sounds
like two units in the most saturated program in the tree. It cost **zero** — the shader never reads
the untransformed original, so the transform is baked over the source, and the table is 768 bytes of
uniform space. Shipped in 11.46 at 16/16 → 16/16. It is the ledger's fifth escape, and the reason it
belongs in this table's argument rather than beside it: the wall's list was assembled by asking what
data a feature needs, when the question that decides blocking is what data it needs *at the same
time*.

**The wall's own framing is what misled here, and is worth restating.** "Sampler-saturated 16/16" is
about *declarations*, which is a link-time property. Blocking is about *occupancy at the moment of
the read*, which is a per-pass property. Those are different questions, and this section asked only
the first for the length of the roadmap. Anything proposing to free a unit should first say which
pass its read happens in.

**And a second wall existed that this section never named: `water_frag` was also at 16/16.** It is a
separate program with a separate ledger, so nothing here applied to it and nothing it did applied
here — which is exactly why it went unrecorded until it blocked something. Spec 11.45 wanted a baked
foam pattern there, found the program full, and shipped procedural noise instead; the noise has no
mip chain, so the far field lost its filtering, and that cost was paid for a wall that turned out not
to be one.

**Consolidation cleared it: 8 identical cascade declarations became 2 arrays, 16/16 → 10/16.** The
lesson generalises past water and is now the ledger's fourth escape above. The narrower lesson is
about this document: a per-program ledger deserves a per-program entry, and "the sampler wall" as a
singular noun is what let a second one sit unexamined through four specs. `water.h` now carries its
own ledger comment in the same shape as this section's.

**The cost was then refunded in the same spec.** With units free the foam pattern went back to a
baked 256² ridged web with a mip chain, which the ALU version could not have had — distant
whitewater had been aliasing into a speckle band that only foam fading out kept tolerable. Two
things about sampling it were found as artefacts before they were understood, and both are the same
trap in different clothes: **the footprint must come from the UNDISPLACED position** (the shore band
advects the lookup along the bed's downhill, the bed gradient is bilinear off a texture and
therefore C0, so an implicit lookup steps mip level at every bed texel and prints the sea as a grid
of rectangles — advection translates a pattern without resizing it, so the correct footprint is the
pixel's own); and **it is domain-warped**, because one 5 m tile across tens of metres of beach is
wallpaper, and a warp is a reparameterisation that leaves the value distribution — and so the
erosion threshold's calibration — untouched, where a blended second tap would not.

**Wall 2 — geometry submission does not scale — MOSTLY REMOVED (specs 11.28 / 11.29).** Instancing,
LOD chains and per-cascade shadow culling shipped in E5; on `abandoned_window_shadowed` that is shadow
CPU **−83%**, frame **−38%**, 2,148 draws → 272. What remains of the wall is the third limb, draw
**ordering**, which E5 deferred as unfalsifiable against the corpus it had — see E6 below, where it is
no longer unfalsifiable.

*The original wall text read: "One draw per mesh … no LOD, no occlusion culling, and no instancing
anywhere outside `particle_renderer.c`. The renderer is AAA-caliber per pixel and early-2000s per
object." Two of those three are now false. The occlusion clause was always true and is now Wall 4's
business rather than this one's.*

**Wall 4 — the opaque pass is unshielded — MOSTLY REMOVED (specs 11.30 / 11.31).** Front-to-back
opaque ordering ships **on**: `apps/forest` opaque **306 → 169 ms (−45%)**, depth complexity
1.93 → 1.08. A depth prepass was built, extended to masked geometry through a `depthOnly` mode in
`pbr_frag` (11.31), reached a better 1.08 → 0.72 — and **still lost on the clock everywhere in this
corpus**, because a full extra geometry pass costs more than the shading it saves. It ships off. The
two turn out to be substitutes rather than complements; 11.30's "worth more together" was an
artefact of the masked exclusion. What remains of this wall is the third clause below, occlusion
culling, which E7 books and no measurement yet demands.

*Original text and its measurement history follow — worth keeping because three consecutive readings
of the same scene each looked authoritative and each was an artefact of its configuration.*

There is no depth prepass anywhere in the engine, no
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

What the light path still cannot express. **Track C does not touch Wall 1** — no item here has cost
a sampler unit, and that is why it was the track to start with.

The other half of this sentence used to read "every item here is UBO-only or postfx-only", and two of
the five falsified it: C1 shipped as a SHADOW-pass feature (`tsm_resolve_frag.glsl`,
`shadow_absorb_frag.glsl`, a `tsmEnabled` uniform read in the shading path), and C2's own text below
records that "no new shading code" held for five phases and then did not.

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

### C2. Emissive geometry → LTC area lights — Effort S/M — **DONE (11.49)**
An emissive mesh lit nothing. It was bright in the frame, it fed bloom, and its only path into the
lighting solution was a 16²-face DDGI capture that converges over seconds and resolves no detail.
Practicals, screens, strip lights and neon — the entire vocabulary of interior lighting — were
decorative.

`Light` already carried exactly the fit's output: `position`, `direction` (panel normal), `up`,
`size`, and a photometric `intensity` in nits (`light.h:45-73`). So the item was a fit — plane-fit an
emissive mesh's dominant quad, integrate its emissive to a radiance — plus a registration into the
packed light array A1 already uploads.

**It ships OFF by default, and that is the finding that reversed the design.** The sketch assumed
emissive means "this is a lamp". Measured across the corpus, **30 of 32 emissive materials are the
unlit-flat-colour trick** — a glow over a black base, used to make something a flat bright colour,
not to light a room. On by default turns every one of those into a light. `--emissive-lights` opts
in; a material can decline individually with `emissiveLight: "off"`.

**"No new shading code" held for five phases and then did not.** The DDGI double count (below) needed
one uniform and one multiply in `pbr_frag`. It cost no sampler unit, so Wall 1 was never in play —
but the claim is worth correcting rather than leaving to rot.

Of the three v1 limits written down here in advance, one was lifted and one needed a test that did
not exist. **"Static fit at load" was lifted**: the fit is cached in LOCAL space and epoch-gated on
the scene graph, while PLACEMENT runs per frame from the owning node's transform, so a lamp on a
moving node costs three vector transforms rather than a refit and an animated emitter simply works.
**"One rectangle per emissive mesh" held, but the test for it was wrong.** Planarity
(`|Σ nᵢAᵢ| / Σ Aᵢ`) was documented as "one number that rejects everything a rectangle cannot
describe"; it measures normal cancellation, so a closed box reads 0 and *everything flat* reads
exactly 1.0 — an L, a ring, a quad with a hole, two strips five metres apart. Each got one rectangle
spanning the lot, the split one radiating out of the empty gap between its strips. The missing test
is fill, `Σ Aᵢ / (w·h)`, and both terms were already computed: one divide. The single-sided
constraint held unchanged.

**Four defects the arms found, three of them invisible in a frame.** The in-plane fit was planned as
the 2×2 covariance principal axis and that is wrong — a square panel's covariance is isotropic, so
the axis is arbitrary and a square bounded at 45° comes out √2 too wide in both directions.
`cornell_light` is exactly square, so the corpus's only real lamp is the case it fails on; the
minimum-area rectangle over the mesh's own edge directions replaced it. A **DDGI capture runs the
full forward shader**, so a probe saw the emissive surface *and* the surfaces the panel had already
lit — the first bounce landed twice, floor GI lift **1.31× → 0.98×** once the capture learned to
silence a derived emitter. That fix must NOT extend to the reflection probe, whose output is radiance
rather than irradiance, which is why the flag names what a capture is FOR. And a derived panel
inherits `cast_shadows` false, so it lights through walls: `cornell_leak` reads **0.41 of the lit
half behind a solid partition**, taken to an exact 0 by a `light_overrides` entry.

**Cost is not where the sketch implied.** The reconcile — walk, fit, placement — is **0.017–0.021 ms
of CPU and zero GPU** on a 71-mesh scene. What costs is that the scene now has one more LTC area
light to shade, roughly **+2 to +3 ms** for one panel covering most of the frame. The price scales
with how many panels a scene derives, not with graph size.

**The specular double count was measured and declined.** A reflection probe photographs the glowing
quad while the panel integrates the same rectangle analytically. Measured **1.0386** against the
diffuse count's 1.3134 — an order of magnitude smaller, because the split sum gives the panel little
solid angle where DDGI's hemisphere integral lets it dominate. Every available fix costs more than
it buys, so a gate arm bounds it rather than removing it. Note SSR is not a second route and cannot
be: `ssr_frag.glsl:152` traces only surfaces the shadow catcher marked reflective.
**Refs.** Heitz, Dupuy, Hill & Neubelt, *Real-Time Polygonal-Light Shading with Linearly Transformed
Cosines* (SIGGRAPH 2016) — already shipped as A2; this item is a producer for it.
**Depends on:** A1 (shipped), A2 (shipped). **Wall 1:** unaffected.

### C3. IES photometric profiles — Effort S — **DONE (spec 11.57)**
9.9/10.0 made punctual lights genuinely photometric — candela and lux imported as authored, an EV100
camera — and then every one of those lights radiated through a bare analytic cone. That premise held
and, unusually for this table, was checkable before building: `Light.intensity` really is canonical
candela, and `spotConeFactor` really did push it through a smoothstep between two cosines.

**This one fits Wall 1 rather than fighting it**, which was right, and the escape is the uniform-space
one. **But the row deferred asymmetric profiles — "the v2 case … defer them rather than paying a unit
for the symmetric 90%" — and that deferral does not survive its own placement.** It prices the 2D
case in TEXTURE units, which assumes a 2D table means a texture. It does not. Once the table is a
UBO block the two cases differ only in stride, so **11.57 shipped full LM-63 with no v1/v2 split**:
rotational, quadrant, bilateral and fully asymmetric, all four read off the *declared* horizontal
sweep rather than guessed from the values.

**And the row's storage plan is broken as literally written.** "Lives in the existing std140 light
UBO alongside the packed lights" collides with how that block is uploaded: `ubo_upload` orphans the
whole allocation and `light_cluster.c` rewrites only the live light prefix, so a table appended after
`clusterLights` is **undefined every frame**, and one placed before them is re-sent on every
`render_current_scene` invocation — seven times on a probe-capture frame. It went in its own block at
binding 6 (the `ShoreFilmBlock` shape), uploaded once when the profile set changes. Bindings 0–5 were
in use against a GL minimum of 36, so the seventh cost nothing anyone was saving.

Shipped shape: the profile is a NORMALISED [0,1] table that multiplies `intensity`, with `intensity`
seeded from the file's own peak candela when the scene authors none — so it is absolute by default
(`normalised × peak IS absolute`) while `intensity` stays the one brightness control and
`_scale_emitters` stays true. It REPLACES the analytic cone rather than multiplying it, which drags
the spot shadow frustum onto the profile's angular support; the cull radius needed nothing, because
normalising makes `profile ≤ 1` and the existing solve is already an upper bound.
**Two independent limits**, since one sized for the worst case wastes the budget on the common one:
8 descriptors and 3968 shared pool floats, holding seven fully asymmetric profiles or ~125 symmetric
ones. Ceiling 32×16 taps, at or above what real files carry.
New: `cetra/src/ies.c/h`, `tools/gen_ies_table.py`, `assets/gen_ies_fixture.py` (FIVE `.ies` files
whose candela are closed forms -- the four LM-63 symmetry classes plus one whose 90-degree tail is
1.2e-6 rather than 0, which is the only input the tail snap acts on), the `ies` gate group at seven
arms, `--ies-probe` and `--ies-profile`.
**Refs.** Karis, *Real Shading in Unreal Engine 4* (SIGGRAPH 2013 course) — the IES section;
IESNA LM-63 for the file format.
**Depends on:** A1's UBO machinery (shipped), 10.0's photometric units (shipped). **Wall 1:** avoided
by construction — and the deferral above shows how easily "avoided by construction" turns into
"priced against it anyway" one paragraph later.

### C4. Clustered specular probes — Effort L — **DONE (spec 11.70)**
Shipped. Read `specs/11.70-a-mirror-in-every-room.md` rather than the sketch below, which was
written before the code and is wrong in two places kept here for the record: the sampler claim
("reusing A4's") is true of the DECLARATION and not of the texture -- A4's atlas is mip-less with
8x8 tiles where specular needs roughness-varying prefiltered radiance, so the two became tenants of
one physical texture, side by side, with GI's coordinates untouched; and the citation `scene.h:127`
was stale before it was written (the field is `scene->probe_set` now, and was at :201).

`scene->probe` was singular -- **one** parallax-corrected reflection probe for an entire scene, so a
character walking from a lit hall into a side room kept the hall's reflections. It is now a
`ReflectionProbeSet` of up to eight, selected and blended PER FRAGMENT: a per-froxel 8-bit mask on
A1's grid says which probes reach a cell, each probe's proxy box gives a weight, and whatever weight
is left over falls to the global environment through the same expression the no-probe path uses.
Storage is an octahedral atlas of roughness ROWS (mips would filter across the tile gutters) riding
`giAtlasTex` on unit 14, so N probes cost zero new sampler declarations; a new `ProbeBlock` on
binding 9 carries the descriptors AND the masks in 3760 bytes.

**Per-DRAW selection was the alternative and is refused on measurable grounds**, which is why the
fixture is two rooms over ONE floor mesh: that mesh is a single draw, so a per-draw design lights
half of it with the wrong room's reflections while every per-room measurement still passes.
The capture budget resolved the way A4's did -- a load-time sweep, attached only once every probe
succeeds, `captures_total` making converge-then-idle checkable. Relight is deferred behind
`probe_set_mark_dirty`.

**The defect worth carrying out of it:** the box fade must run OUTWARD from the proxy faces. Inward
is the shape that reads as obvious, and it gives every floor in every scene a weight of exactly zero
-- a floor lies ON the bottom face of the box that box-projects it -- so the one surface the feature
exists for falls back to the environment. It shipped that way for a day with a plausible-looking
frame, because a floor reflecting nothing and a floor reflecting a dim room are the same picture.
**Refs.** Lagarde & Zanuttini, *Local Image-Based Lighting with Parallax-Corrected Cubemaps*
(SIGGRAPH 2012 talk) — the parallax correction, now applied per probe; McGuire et al., *Real-Time
Global Illumination using Precomputed Light Field Probes* (I3D 2017) for the atlas structure.
**Depends on:** A1 (shipped), A4 (shipped). **Wall 1:** unaffected, and confirmed so — zero new
sampler declarations in `pbr_frag`.

### C5. Screen-space shadows for local lights — Effort S — **DONE (spec 11.56)**
A3 marched contact shadows along **one** light — the key. The march now walks the cluster list a
fragment sits in and marches every point or spot light with **no punctual shadow map**, folding the
visibilities weighted by each light's own contribution (radiance, `getDistanceAtt` falloff, N.L) —
the fraction of direct light the pixel loses, which is what one R8 channel can honestly carry.

**The justification this row shipped with was false and is left here corrected rather than
deleted.** It said every other light lands on a map "whose texel footprint loses the
millimetre-scale contact". A punctual map is a PERSPECTIVE map fitted to one light, and its edge
comes from the layer count against a 96 MiB budget — 4096² for a lone spot, 2048² at two to six
layers (a point light's six faces), 1024² above that. So a 90° cube face is ~1 mm/texel at 1 m for a
point light and ~2 mm at the floor, every rung finer than the gaps A3 draws, and it runs 3×3 PCF with per-tap
plane bias. The contact hairline that did exist was found in 9.8 and root-caused to far-side depth
STORAGE, not resolution; 10.3 (near-side storage) and 10.4 (receiver-plane bias) fixed it, with
`cornell_box`'s box/floor contact as the visual gate. Where a map exists the contact is already
sharp — so the row's premise had been obsolete for six specs.

The real gap is bigger. `MAX_PUNCTUAL_SHADOW_LAYERS` is **8** and a point light spends **6**, against
`LC_MAX_CLUSTER_LIGHTS` **128** — so ~120 of 128 clusterable lights can never have a map, and the
second point light in a scene silently gets nothing. For that population this is not a sharpening of
an existing shadow; it is the only occlusion there is. The froxel pass already conceded the same
population in a comment.

**C2's derived panels are in that population and are still not served, which this row must not be
read as fixing.** They are ineligible for a map for the same reason — `create_light` defaults
`cast_shadows` false — and 11.49 measured that at 41% of the lit level leaking through a solid wall
(`specs/11.49:787`). 11.56 does not touch it: a panel counts in the fold's denominator, so it no
longer inflates another light's term, but it is never marched. See the area-panel note below. The
remainder is still open.

That flips the cull the row proposed: **skip lights that already have a map** rather than march the N
nearest. One line, and it is both the performance cap and — once the fold's denominator counts the
skipped lights, which the review had to fix — the reason the pass cannot double-shadow anything.
Area panels are skipped from MARCHING for a different reason: a direction does exist
(`dirType.xyz` carries the panel normal), but `pbr_frag` shades a panel through an LTC integral over
its whole area, so one ray at its centre would be a different approximation from the one the shading
used — worse than no ray. A centre is still good enough to weigh how much light arrives.

The run gate widened with it: `cs_active` required a shadow-casting DIRECTIONAL, so a room lit only
by practicals never ran the pass at all.

**Cost.** 2.43 ms per additional fully-covering light at 3200×2000 internal (0.38 ms per megapixel
per light), linear past two. But cost tracks COVERAGE rather than count — sixteen lights spread
across a scene cost +0.77 ms against the +38 ms sixteen coincident ones would, because a pixel
marches only the lights whose cluster entry reaches it. No per-pixel cap, deliberately: a cap is a
silent truncation that goes wrong in exactly the crowded scenes where the term is largest.
New: `assets/contact_local_fixture` (+ its bare twin), `gates.py`'s `contact` group.
**Refs.** the Uncharted 4 contact-shadow technique already cited by A3.
**Depends on:** A1 (shipped), A3 (shipped). **Wall 1:** unaffected (postfx — `create_post_program`
links through `ubo_wire_blocks`, so the cluster list arrives with no C-side binding at all).

## Track D — Surfaces & environment

*Corrected three times, and neither dependent survived — nor did the correction. D2's ground-shadow
half reads in the OPAQUE pass, where unit 6 took a `#define` alias, and shipped in 11.41. D1 was said
to fit the same way "if its atlas is flat rather than a `sampler2DArray`", and that was wrong twice
over: 11.41 had already taken the slot for the whole pass, and 11.73 shipped decals as a TENANT of
unit 2's array, which needs neither an idle unit nor a flat layout. **Both dependents are built and
neither spent D0.** What Wall 1 actually bites is narrower than this preamble ever claimed: a
consumer read in a pass where every unit is already live. See the ledger's occupancy sweep.*

~~Where Wall 1 actually bites. D1 and D2's ground-shadow half both need a texture inside `pbr_frag`,
so **D0 is a hard prerequisite for them**~~ — withdrawn on both counts above. What survives is the
just-in-time rule itself: a foundation lands with its first consumer, and D0's only remaining
candidate consumer is the froxel volume in the transparent pass, which is not scheduled.

### D0. Free two `pbr_frag` sampler units — Effort M — **REFUSED on investigation (11.85)**

***The first consumer to actually want this unit found the fold unsound, and that closes the
item.*** `pbr_frag` *reads* `irradianceMap` *on exactly one line, and that line is the ELSE of*
`giEnabled` *-- the irradiance map is what answers when there is NO GI volume. And*
`gi_volume_bind` *returns before binding the atlas when the volume is inactive. So the texture
this entry proposes folding into is the one not bound in precisely the case the fold has to
serve; making it hold means allocating and binding the GI atlas on every scene that has an
environment, which couples IBL to a subsystem that is off underneath it.*

*11.85 wanted the unit to split the material array by colour, then measured that the array
is the wrong prize anyway -- so the item is refused rather than merely unscheduled. The
entry below stands as the record of what was explored.*

### D0 (original entry) — explored, and the count is ONE
Not a feature; the unblocking item. The concrete candidate: **unit 11 (IBL irradiance) folds into the
GI atlas on unit 14** as a single octahedral tile. `pbr_frag` already samples that atlas, the
octahedral encode/decode include already exists from A4, and a cosine-convolved environment is
precisely what one atlas tile holds — so the fold costs no new sampler and frees a whole unit. Second
candidate, riskier: share unit 6 (`sceneColorTex`) the way 11.17 shared it for moments, valid only
where the two consumers are provably mutually exclusive.
**The 0-diff gate here is unusually strong and should be demanded**: folding irradiance into an atlas
tile is a pure storage change, so the raiden baseline must be 0 px, and if it is not, the
octahedral resampling is lossy in a way that matters and the item should stop.
**Depends on:** A4 (shipped). ~~**Owns foundations:** the freed units D1/D2 spend.~~ — **neither
spends one.** D2's surface half shipped on unit 6's alias (11.41), D1 as a tenant of unit 2's array
(11.73). This item owns a foundation nothing booked has asked for.
**Demanded by measurement since 11.39**, not just by the dependency graph: D2 shipped its froxel
half and measured the ground at RMSE 0.0013 against the air's 0.0353, so the dappled light on
terrain that D2 was written to deliver is entirely on the far side of this item. It is now the only
thing standing between the engine and that look, which makes it the highest-leverage entry in D.
*(Both figures are retro-dated by 11.41, which found the shadow MAP itself wrong — see D2. They are
left here because the framing they support is withdrawn below on separate grounds, but they are not
current readings and no replacement pair has been taken.)*

***And that framing is now withdrawn.*** *D2's surface half is read in the OPAQUE pass, where unit 6
is idle and free for an alias, so the dappled terrain light is NOT on the far side of this item and
never was — it needed a free pass, not a free unit. The "highest-leverage entry in D" line was
written from the dependency graph the entry itself proposed, and the graph was wrong. D0 remains
worth doing, for D1 and for the froxel-in-transparent item, which are the two consumers that really
cannot dodge it.*

***And "two" is now one.*** *D1 dodged it too, by a route this section did not consider at all —
becoming a tenant of an array that was already declared, rather than looking for a unit to alias.
The froxel volume in the transparent pass is the last consumer that genuinely cannot, and it is not
booked. Every prediction this entry made about who would spend it has now been falsified by the
thing shipping without it.*

**Explored against the code after 11.40, nothing built. Five corrections to the entry above.**

**1. It frees ONE unit, not two — 16/16 becomes 15/16.** Only the first mechanism holds. The second
("share unit 6") is not a freed unit at all: it is a *conditional* one, usable only by a feature that
is provably absent whenever refraction or MBOIT runs. It does not raise the count, it lets one
specific consumer squeeze in under a constraint that is invisible at link time and fails as a wrong
image rather than an error. Wall 1 therefore does not fall here; it becomes a one-slot budget, and
per the ledger's occupancy sweep that slot has **one** honest customer left — sampling the froxel
volume from the transparent pass, which is a `sampler3D` read in the one pass where refraction is
live. ~~D1 joins it only if a flat 2D decal atlas is refused in favour of a `sampler2DArray`.~~ —
D1 shipped (11.73) as an array TENANT and joined nothing.
Everything else on the wall's list either was never blocked or can take unit 6's idle opaque slot.

**Which means D0 currently has no SCHEDULED consumer at all.** That is not an argument against
building it — but it does mean the item has to be judged on its own measurement rather than on what
it unblocks, and the just-in-time rule this entry cites is now pointing the other way. *(This
sentence read "headroom before D1 is worth something" until 11.73 built D1 without it. There is no
consumer left to hold headroom for except an unbooked one.)*

**And 11.68 pushed that further, which is worth counting.** Roads are the FIFTH feature this
roadmap booked against a freed unit that needed none — after C2's derived area lights (one float),
C3's IES distribution (a UBO), D9's material layers (a tenant of an array already bound) and D4's
CDLOD heights (two free vertex attributes). Roads needed no unit because a road is not an image at
all: it is a handful of segments the shader evaluates. The pattern behind all five is now explicit
enough to state as a rule — **ask what SHAPE the data is before asking for a unit, because a
sampler is for pictures, and four of the five wanted a table, a scalar, a vertex or an equation.**
D1's poster-and-scorch half is the first booked consumer that genuinely wants a picture.

**And 11.73 made it the SIXTH, which is the more useful lesson.** Wanting a picture turned out not
to be the same as wanting a DECLARATION: the array on unit 2 already holds pictures, so a decal
image joined it as a layer. The rule above should therefore be read one step further — ask what
shape the data is, and if the answer really is "a picture", ask whether something already declared
holds pictures of that shape before asking for a unit.

**2. The fold is about TYPE, not storage, and that changes its cheapest shape.** `pbr_frag.glsl:1766`
already reads `giEnabled > 0 ? giSampleIrradiance(...) : texture(irradianceMap, N).rgb` — the two are
**already mutually exclusive at the call site**. Only their declarations both exist, and the driver
counts declarations. So the fold buys no quality whatever; its entire purpose is to make the two the
same *type* so one declaration can serve both, which is exactly 11.17's escape.
Which means irradiance need not enter the GI atlas at all. A standalone octahedral `sampler2D` owned
by `ibl.c` and bound to unit 14 whenever GI is off is the cheaper shape, and it avoids the trap in
the entry above: `scene->gi_volume` is **NULL by default** (`scene.c:66`; only `apps/render` builds
one) while every scene has IBL irradiance, so folding into the *atlas* would couple an always-present
subsystem to an opt-in one, or force a second atlas producer — buying a sampler at the price of a new
concept.

**3. The 0-diff gate the entry demands cannot pass, so as written this item contains a stop condition
it will always hit.** Cube→octahedral is a resample — different projection, different texel centres,
different filter topology — and cannot be bit-exact. Every IBL scene's ambient term shifts by a few
LSBs. The bar has to be a stated tolerance plus a golden re-bake, not 0 px. This is the entry's own
"measure twice" rule turned on itself: the acceptance criterion was written before anyone checked
whether it was reachable.

**4. Sizing and cost, from the code rather than estimated.** The cube is 32² × 6 = 6,144 texels
(`ibl.h:11`), so an **80×80 octahedral tile matches its angular density** at ~51 KB against the cube's
~49 KB — VRAM is a wash and the resolution question is simply a constant to pick. GI's own 8×8 tile
(`gi_volume.h:35`) would be 96× coarser and is the version to refuse. Runtime: `octEncode`
(`octahedral.glsl:13`) is a reciprocal, three `abs`, two adds and a small branch — 15-20 scalar ops on
the ambient path, comfortably under `apps/forest`'s 0.23% run-to-run floor, i.e. not findable in the
profiler. Per-frame CPU is zero; the bake gains one 80² draw against an env re-bake that already
costs 73.6 ms with clouds. **The failure mode to watch is the seam gutter** — a cubemap gets seamless
cross-face filtering free, an octahedral tile does not — and a wrong gutter reads as a faint cross in
the ambient term on every surface in the scene, which is subtle enough to ship unnoticed.

**5. The second unit exists but should NOT be taken yet: unit 4 (`heightTex`) into the mask array.**
POM reads `.r` and nothing else in all five of its taps (`pbr_frag.glsl:342, 347, 353, 377, 951`), so
height is a scalar per-texel material field — the mask array's literal job description. Precision is
free: every texture in the engine loads `GL_UNSIGNED_BYTE` (`texture.c:470`) and the array is RGBA8.

**VRAM is exactly neutral on the corpus's real POM asset, for a reason worth recording.** `pilot.fbm`
is five materials of 4096² 8-bit **grayscale**, so a height map is `GL_RED` at 4096²×1 B = 16.78 MB
standalone, and an array layer is 2048²×4 B = 16.78 MB — the 4× waste of a scalar in RGBA8 exactly
cancels the 4× area cut from `MASK_ARRAY_CAP 2048`. Whole asset, with mips: **111.9 MB either way**
(five 4096² 8-bit height maps at 16.78 MB each, ×4/3 for mips). The equality is the point and it
holds; the absolute figure read 335.5 MB, which is exactly 3× too high — computed as though the
height were 3-byte RGB, where `texture_gl_formats` maps a 1-channel PNG to `GL_RED`.
The real price is **resolution**: POM would march 2048² instead of 4096², on the one asset that most
wants the detail.

Three preconditions, all inert today and all biting the instant height becomes a layer:
- **The frame loop runs them in the wrong order.** `material_texture_array_ensure_built` (`engine.c:2430`) precedes
  `heights_ensure_resolved` (`:2433`), both defer until the loader is idle and both are run-once — so
  the array would be built with no height layer and never rebuilt. Swap them, and re-arm the rebuild
  on height discovery.
- **Height arrives after the streaming path closes.** `resolve_height_maps` loads synchronously
  (`import.c:1718`) *after* the async loader reports idle, outside the precondition the array build
  is written against.
- **One large layer is charged to all of them.** The canonical size is the largest present mask capped
  at 2048, and every layer shares it (`material_texture_array.c:118-132`). Guard: fold only when the height map is
  not the largest texture in the set — masks at 512² with a 2048² height map would take six existing
  layers from 8 MB to 134 MB for nothing.

**Deliberately not recommended until a second consumer is scheduled.** One unit already covers D2's
surface half, the only Track-D feature with a measurement behind it; ~~D1 is Effort L and
unscheduled~~ **D1 has since shipped (11.73) and took no unit**, and the froxel-in-transparent item
is not scheduled at all. Taking unit 4 now halves POM's resolution
on the tree's only POM asset to buy a slot nothing spends — precisely what the just-in-time rule
exists to prevent. ~~Revisit with D1~~ — D1 came and went without asking. Revisit with whatever
schedules the froxel read, and measure the 4096→2048 question on `pilot` then rather than
assuming it.

*Also learned, since the name misleads: the frame loop's "POM height resolve" is not a GPU pass. It is
a CPU filename-convention resolver (`import.c:1673`) — glTF carries no height texture (the
`KHR_materials_displacement` draft was abandoned), so height maps arrive as `<name>_height` siblings
derived by stripping a known base-map suffix, and finding one **auto-enables POM** by setting
`parallax_scale`.*

**6. A sixth mechanism exists that this entry never listed, and it is the one that actually worked —
on the other program.** Consolidating identical declarations into an array took `water_frag` from
16/16 to 10/16 in a single spec (11.45), which is six units where D0's best mechanism frees one. It
does **not** transfer cheaply: `pbr_frag`'s four `sampler2DArray`s are `materialArray`, `shadowMaps`,
`punctualShadowMaps` and `ltcTex`, and the closest pair — the two shadow arrays, both
`GL_DEPTH_COMPONENT24` through the same `init_depth_array` — are sized independently, so merging them
means one of the two changing resolution. That is a quality trade rather than the free consolidation
water got, and it should be priced against the irradiance fold rather than assumed cheaper than it.
The ledger section above has the full argument as the fourth escape.

**Second candidates examined and rejected.** Unit 13 (`brdfLUT`) to an analytic Lazarov fit: 10.7.1
packed the sheen E-LUT into its `.b` channel for KHR conformance and clearcoat reads `.rg`
(`pbr_frag.glsl:1258, 1280, 1858`), so this trades spec conformance for a sampler. Unit 9 (Charlie
sheen env): only live under sheen, conditional rather than free. Units 10/15 (CSM and punctual shadow
arrays): both `sampler2DArray` with different layouts and resolutions — real work for one unit.

### D1. Clustered decals — Effort L — **SHIPPED (spec 11.73)**

***Shipped, and the texture story is NOT the one this entry planned.*** *The flat-2D-atlas
alias below rests on "unit 6 is idle in the opaque pass", which was already false when it was
written: spec 11.41 binds the cloud-shadow map there for the WHOLE of that pass, unconditionally
at every program switch, and `render.c` explicitly refuses the scene-content predicate that would
have let a second tenant share it. This entry and D2 were written against the same free slot and
only one of them could spend it.*

*What shipped instead is the TENANCY escape: a decal image is one of the scene's unique per-texel
material images, so it rides `materialArray` on unit 2 for a layer index and no declaration. That
is strictly better than the alias it replaces rather than merely equivalent — an opaque-pass alias
would have inherited 11.41's exclusions, so decals would have been absent from the late pass and
from probe captures, which is exactly where a poster ought to appear. **D0 was never a
prerequisite, and neither was a flat atlas.***

*Caps: `DECAL_MAX` 16, sized by the froxel mask rather than by taste — 32 pins the descriptor at
six vec4 rows forever, 64 does not fit GL 4.1's guaranteed 16 KB block. Binding 10 takes `pbr_frag`
to ten uniform blocks of twelve. Cost landed where this entry predicted: the loop is bounded by the
cap and broken on a uniform count, so every golden is 0 px and a decal-free scene compares 0 px
across builds — the data-dependence worth watching is per-pixel fetch count inside overlapping
boxes, which the cap bounds and nothing else does.*

***Spec 11.74 closed what the review round left, no new surface.*** *The multi-decal path had never
been exercised at all — the fixture's two marks are disjoint in two axes, so the over-composite only
ever met an empty accumulator, which is the one input under which paint order, a premultiplied sum
and a plain overwrite are the same picture. `decals-overlap` appends a third mark at gate time and
reads which one wins; reversing the composite fails it and no other arm. Also: the Scene now retains
and releases its decal images rather than holding raw pool pointers, a failed material-array build
puts every decal back to "not built" (measured 36,786 px of opaque black marks otherwise, since GL
defines a sample of a mip-incomplete texture as (0,0,0,1)), and the three froxel z-clamp spellings
became one.*


The largest **environment-art** gap in the engine: there is no way to author localised surface detail
onto geometry — no scorch marks, no leaks, no edge wear, no posters, no puddle edges. Forward
rendering rules out the deferred screen-space decal every AAA engine uses, but the clustered form
fits A1 exactly: box decals culled into the same 16x8x24 grid, sampled in `pbr_frag` before the
lighting loop, modifying albedo / normal / roughness in place.
**Hard-blocked on D0** — a decal atlas is a texture in `pbr_frag`, which is the one thing the ledger
has none of. Second cost worth pricing before committing: decals are the first feature that makes the
forward shader's cost data-dependent per pixel in a way clustering cannot bound tightly.

***And a NARROW slice of the ground half is no longer unbuilt.*** *Spec 11.68 authors localised
surface detail onto a layered world-XZ surface — a road — with **no texture, no atlas and no unit
at all**: it overrides the splat weights toward one of the material's own layers from a handful of
segments in a uniform block. So a mark on the ground that can be MADE OF a layer the ground already
has (a worn path, a track, a dirt patch) has a shipped path that dodges D0 by not being an image in
the first place.*

*Read the caps before planning against it, because they are small and deliberate: **4 polylines of
at most 16 points, on ONE road-bearing material per scene, and that material must be layered with a
world-XZ splat.** Evaluated per fragment on the per-texel path, which is why the spec says a road
NETWORK wants the paged content era rather than a hundred polylines. So what D1 still owns is most
of it: detail on walls and props, which have no world-XZ splat to override; marks whose appearance
is a picture rather than a material the surface already carries — a poster, a scorch, a logo; and
any density past a handful of marks. ~~Price that remainder against the flat-atlas alias below rather
than against the ledger.~~ **That remainder is what 11.73 built, and it was priced against neither:
the atlas question never arose, because the picture went into an array that was already declared.***

*~~Softened by the ledger sweep: this is blocked by a choice of texture LAYOUT, not by the ledger.
Decals are read in the opaque pass, where unit 6 is idle, so a **flat 2D atlas with computed tile
UVs** takes the alias and needs no freed unit at all — `gi_volume.glsl`'s `giTileUV` is the in-tree
pattern, and the GI atlas is itself a flat 2D atlas of tiles for the same reason (hundreds of tiles,
one sampler). Only the `sampler2DArray` form is blocked, because every array declared in the shader
(2, 7, 10, 15) is read during opaque shading. Price the flat atlas before assuming D0 is a
prerequisite; the array's convenience is real but it is convenience, not necessity.~~*

***Struck whole, and worth leaving visible because both of its premises were false.*** *Unit 6 is
not idle in the opaque pass — 11.41 binds the cloud-shadow map there for all of it. And "every array
declared in the shader is read during opaque shading" is true and beside the point: the question was
never whether an array is idle, it was whether one already holds this kind of picture. Unit 2's does.
11.73 added a layer to it, and the `sampler2DArray` form this paragraph called blocked is what
shipped.*
**Refs.** Persson, *Practical Clustered Shading* (SIGGRAPH 2013 course); Wronski,
*Screen-Space Decals* (GDC 2014).
**Depends on:** A1 (shipped). ~~D0 (hard)~~ — withdrawn; see the shipped note above.

### D2. Local fog volumes + cloud shadows — SHIPPED as specs 11.39 + 11.41
Both halves landed. **The surface half too, in 11.41, and it never needed D0** — it reads in the
opaque pass where unit 6 is idle, so it took a `#define` alias and cost the ledger nothing. Ground
RMSE **0.0072 → 0.1241** at coverage 0.10, with the air band unchanged to four decimals as the
control. `pbr_frag`, the shadow catcher and water's caustics all receive; they are three different
mechanisms rather than one lookup in three places, because the catcher is a darkening plane and not
a lit surface, and water has no analytic sun lobe at all.

**And 11.41 found the MAP wrong, which retro-dates every cloud figure above.** The march capped the
SLANT path at 1.2 km while crossing the 2.5 km deck takes `2.5/sin(el)`, so it traversed 48% of the
cloud at zenith, 12% at 15°, 4% at 5° — and below ~10° it never left the cloud base and returned
uniformly 1.0. `apps/tree` runs a 0.8° sun and had never had a cloud shadow from either half. It now
steps equal ALTITUDE increments across the whole deck and clamps only the HORIZONTAL excursion, at
the shape field's own 8 km tile period. **The lesson is the same one 11.40 filed and this item then
repeated twice**: a constant that bounds a length will bind everywhere unless the thing it bounds is
elevation-independent. 11.41's own first attempt at the fix picked 1.2 km for the horizontal reach
and bound below 64° — the identical flaw, caught only by checking that two candidate values agree at
high sun where neither should clamp.

*Original as-shipped notes follow.* Local fog volumes are a Scene-owned world-space AABB with density, an inward
feather and a σ-weighted tint, authored as a top-level `fogVolumes[]` block and arming the froxel
pass by joining the union at the gate rather than latching `fog_enabled`. Cloud shadows are a
256² R16F sun-transmittance map built by the cloud march from the march's own wind offset, read by
shearing each froxel up to the shell — exact for a horizontal layer, and tiled by `GL_REPEAT`
because the density field is periodic over the shape noise's own 8 km.

**The verdict this item was told to produce: the surface half IS still worth a unit, and the
measurement says so more sharply than the entry below expected.**

| band, aerial fixture with `--clouds --fog` | RMSE on/off |
|---|---|
| sky and air | **0.0353** *(retro-dated — see below)* |
| ground | **0.0013** *(retro-dated — see below)* |

The froxel half shadows *in-scattered light*, so it lands where the sight line crosses the most air
and puts essentially nothing underfoot. The entry below called the froxel half "where the visible
payoff is" — that is right about payoff per line and **wrong about which payoff**. What ships is
weather in the haze; the moving dappled light on terrain, which is the thing the entry actually
promised, is the `pbr_frag` half. *That last sentence used to end "and is still entirely unbuilt. So
D0 is not merely unblocked-by here, it is the gating dependency for the feature D2 was written to
deliver." Both clauses were withdrawn by 11.41: the half is built and D0 gated nothing.*

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
**Depends on:** B1 (shipped), B2 (shipped). *The "surface half on D0" this line carried is withdrawn:
that half reads in the opaque pass, where unit 6 is idle, so it takes an alias and no freed unit. The
question the entry set — "measure whether the surface half is still worth a unit" — turns out to have
had a third answer neither branch anticipated: it is worth building and it costs no unit.*

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

**Since 11.42 that handover is measured rather than authored.** The removed slope arrives as an
absolute mean square rather than a fraction, and converts to a lobe width by the Beckmann relation —
so the horizon is as rough as the waves it stopped resolving, and composes with the authored
roughness by adding variances, collapsing to exactly the authored value where nothing is filtered.
It replaced a lerp toward a **0.115** literal inherited from the reference study, which made the
horizon the same roughness for a millpond and a gale and was low by about a factor of three against
this spectrum's own slope variance. `water-farfield` improved on both halves it measures (far-box
spread 0.24x → 0.10x, authored-roughness sensitivity 0.12x → 0.02x) with its near-box invariance
still exactly 0.

**11.42 also gave the surface a sun.** `sky_env_frag` bakes the sky *without* a disc, so under the
procedural sky the environment lobe carried no sun at all and the sea had no specular response to
its own key light — the one thing 11.32 planned (`:205`) and never delivered. It is now an
anisotropic Beckmann/Cox-Munk lobe whose two variances come from the spectrum rather than from
Cox-Munk's wind-speed regression, fed the UNRESOLVED slope so the waves the mesh already carries are
not counted twice, shadowed through `csmOutermostOcclusion` and dimmed by the cloud deck. Whitecaps
persist (one accumulation pass, three bands in one RGB target) instead of vanishing with the crest
that made them, and the spectral sea state is authorable from `.cscn` instead of six `#define`s.

**The entry above stopped at 11.42 and six more specs have landed on this surface since.** Recorded
compactly here, in shipping order, because D3 owns water and a reader wanting the current state
should not have to reconstruct it from six closed specs.

- **11.43 — the fixture told the truth for the first time.** `water_fixture.cscn` authored
  `sun_elevation` / `sun_azimuth` **flat** where `parse_env` reads them one level down under `sun`,
  so its stated 26° sun had never been in effect: every water arm and both water goldens were
  calibrated against the sky's own 35°, and rendering the stated angle moves **85% of the frame**.
  Fixed at the class rather than the instance — `parse_env` now reports a key nothing read, the way
  `parse_water` does — and the fixture was corrected to the angle actually in effect, so it becomes
  truthful at 0 px instead of re-tuning twenty-nine arms as a side effect of a key-name fix. The
  gate boxes stopped being transcriptions in the same spec: `_water_ramp_band` reads the wedge's
  corners out of the glTF and states a box as two HEIGHTS on the ramp, so the ramp could then be
  flipped (waterline row 0.5079 → 0.4530) with every derived box following it untouched.
- **11.44 — the ocean learned how big a world unit is.** Every physical length in the water was a
  bare world-unit literal (the shoal window, the short band's fade, the bend ceiling, the caustic
  depth window), correct only where a unit happened to be a metre. They are metres now, converted at
  use through `waterUnitsPerMetre` off `Sky.world_units_per_km` — the authority the atmosphere
  already uses, so the ocean cannot disagree with its own sky about the size of the world.
  **`apps/tree` set that number for the first time**: `ground.h` had recorded 22 units to the metre
  since 11.35, the GUI had offered a slider, and nothing ever wrote it — so tree shoaled over 12 cm
  of depth and its surf zone was **0.38 m wide**, which is the hard line at the shore the spec was
  called to fix. The atmosphere moved with it and **99.05% of tree's frame** changed, correctly: the
  engine had been putting 0.62 km of air in front of a 28 m island. Provably inert where it had to
  be — `water_fixture` is authored at 1 unit = 1 m, so its factor is exactly 1.0 and all 24 goldens
  read 0 px. **This closes open item 5 below.**
- **11.45 — the shore got a simulation.** A closed form says where the water's edge ought to be; it
  cannot say that THIS wave ran into the last one's backwash and stopped short, because that is a
  collision between two bodies of water and a formula has none. So: a Lagrangian chain per
  alongshore column of the traced shoreline, segments carrying a conserved rest volume, forced by
  the shallow-water pressure form — a linear spring has degenerate equilibria and settles into
  piled-up states, where the pressure form pins the equilibrium to surface-flat uniquely. Both the
  sea's lens and the sand's wetness read the same tips, so they agree by construction. **It costs no
  sampler**, which is what made it possible at all: the tips are a few hundred floats, which is the
  ledger's second escape, 2.3 KB against a cluster block already carrying 12. Its probe caught two
  faults reasoning had not — the dynamics were written in METRES in a world of 22 units to one, so a
  6 m/s CFL cap became 0.27 world-units/s and the chain could not move a centimetre a frame; and the
  first reading was taken 0.2 s after the seed, where a push needs about three seconds to cross the
  beach at `sqrt(gh)`, so the flat answer was the rest state correctly reported.
- **11.46 — by-example texturing.** See **D5** below and the ledger's fifth escape.
- **11.47 — whitecaps that break instead of blobbing.** The reflex, raise the threshold and have
  fewer of them, was measured backwards before any constant moved: at the default 11.5 m/s sea the
  frame carried **1.094% whitecap coverage against Monahan and O'Muircheartaigh's 1.60%**, so
  coverage was already slightly LOW and raising the onset would have made the sea cleaner than a
  real one while leaving every part of the complaint — contrast, edge, motion — untouched. What the
  complaint was actually about is the **short cascade**: a 12 m tile carrying 0.26–5.15 m waves at
  0.64–2.84 m/s, re-selected every frame at the displaced position and voting on where foam is,
  while everything else in the path drifts at 0.12–0.35 m/s. Removing its vote took per-frame
  foam-state churn **0.074% → 0.010% of the sea**, matching the phase-speed ratio its own wavenumber
  range predicts. Three more followed: the accumulator is mipped and read with `textureGrad` (never
  the implicit derivative — `oceanCascadeUv` wraps with `fract`, so an automatic one reads a whole
  period across every tile seam); a fold only births foam near its own band's crest, normalised in
  that band's own RMS so the window scales with sea state rather than being tuned for one
  (2.345% → 1.867%); and the composite opacity splits crest from shore, because a whitecap is a
  millimetre slick with sea showing through and a swash is a decimetre of aerated water, and one
  ceiling could not serve both.
- **11.48 — the swell got its own spectrum.** It had never been authorable at all: seeded from a
  hardcoded 8.4 m/s over 310 km, so a scene lowering its wind kept a gale's swell — in `apps/tree`
  **300% of the wind sea** — and since depth-limited breaking tests `disp.y / (0.39·depth)`, a
  shallow shelf under it broke everywhere at once and painted a white blob over the whole bay.
  `Breaking` was reporting the truth about an absurd sea, which is why 11.47's six commits on the
  foam path never touched it. Now two complete wave trains, `windSea{}` and `swell{}`, eight keys
  each beside a shared `seaDepth`. Its P6 then found the breaking term itself wrong in a way the sea
  state had been masking: **the face term normalised the bed's gradient, and the bed does not know
  which way a wave is going** — exactly zero on a flat shelf, exactly zero where the shore runs
  across the wind (a plane wave's gradient has no component across its own travel), and reversed
  past the beam, which put the foam on the REAR face of every wave on the lee shore. Taken off the
  wave's own direction instead: breaking coverage 1.87% → 1.04% of sea area on tree, the difference
  being the lee face and the crosswind band.

**Build the instrument before the fix — and in this series it contradicted the reflex every time.**
Worth naming because that is what the practice buys: 11.44 found the surf zone was 0.38 m wide
before it touched a constant, 11.47 found coverage already LOW when the whole complaint sounded like
too much foam, and 11.48 found the breaking term wrong when the sea state had been taking the blame.
Two of the instruments are worth carrying past water. `--water-foam-debug` writes a **binary**
selection mask and is read at **nadir**, where a pinhole images a plane affinely — the `1/cos³` of
foreshortening cancels the `cos³` of solid angle, so pixel fraction IS areal fraction, exactly, with
no crop box to place. Both halves are deliberate: coverage measured off the shaded frame is a
measurement of the opacity and colour being calibrated, and an instrument whose sensitivity depends
on the thing under test is not one. And 11.48's containment check is the **per-cascade probe rather
than a golden**, because both water goldens are Gerstner and never reach the seeding at all — cascade
2 carries no swell and read identical to every printed digit across the change, which is what pins
the wind sea as untouched where a green golden would have pinned nothing.

**So the tessellation stage is still entirely unspent**, and it has now been declined twice rather
than merely not reached: D3 shipped a projected grid instead and D4 shipped a CDLOD quadtree whose
morph is ordinary vertex work (11.63). POM silhouettes is the candidate first consumer left, and it
inherits no pipeline from here.

**Open items this entry still owns.** Recorded here rather than only in the closed specs, because a
closed spec is not where anyone looks for work that is still outstanding. Numbers are stable —
item 5 is struck rather than removed so the ones after it do not shift under a citation.

**Six are live and they group into three.** Geometry: 1 and 4's second half, both about the
projector covering a camera the waves reach. The gate corpus: 3, 6 and the red 7. And 2, precision
at distance, which has **no consumer today** and whose correct fix is engine-wide anyway. The corpus
group is the more urgent of the three: 3 and 6 are the same finding in two places, which is that
this subsystem's arms have twice been green over a defect they structurally could not see.

1. **The projector aims at the still plane, not the displaceable slab. ATTEMPTED IN 11.42 AND
   MEASURED HARMFUL — the prescription below is wrong, and this is the correction.** Each lattice
   ray is intersected with the flat plane — which is where the wave field is sampled — so the surface
   then displaces and a wave can lift geometry into view from XZ the lattice never sampled. The
   nearest point a ray can reach is `clearance / tan(top-of-frame angle)`. It bites when the eye is
   within about one wave height of the surface: wading in `apps/tree --player`, a boat deck, a camera
   at sea level. It is **why `water-submerged` sits on a shallow Gerstner framing** rather than the
   FFT one it was written for.

   The blocker this entry named — that nothing publishes a displacement bound — **is gone**: 11.42
   accumulates the seeded spectrum's height and slope variance and `--water-fft-probe` traces the
   normalisation against the transformed field (measured 0.69–0.99 of predicted, the deficit tracking
   how few modes carry the energy). Gerstner's analytic `amplitude × Σ 0.44^i`, i<4, = **1.7188**
   still holds.

   **What did not survive is "costs zero lattice rows".** Offsetting the plane shrinks the lattice's
   world footprint, and `OCEAN_OVERSCAN` compensates horizontal displacement as a *fraction of that
   footprint* — so the same displacement over-runs the overscan and opens a wedge of background at
   the frame edge. Measured on a wading framing (eye 1.5, level 0, amplitude 0.55): a grey hole in
   the bottom-left corner, which is the defect class 11.35 exists to have fixed. Offsetting by the
   full bound is worse still — with the eye inside the slab the plane goes *above* the camera, the
   `OCEAN_MIN_EYE_HEIGHT` clamp pins the clearance at a hair, and the near lattice collapses about
   sixfold. Spanning the lattice across both sides of the horizon row (the geometry half of item 4)
   fails the same way, diluting rows into the `OCEAN_FAR_DIST` cap.

   So the real fix is one of: Johanson's two-plane construction, or an overscan that scales with the
   offset. Both are larger than the one-line change this entry predicted. Reverted cleanly in 11.42;
   `ocean.glsl` is untouched by it.
2. **Water positions are absolute, so precision tracks distance from the world origin, not the
   camera.** Measured: the same sea at 960 vs 307,200 units out differs by **227,458 px of 480,000**
   (RMSE 0.0146) even though the spectral field is exactly periodic across the 960-unit cascade LCM, so
   those two frames should be identical. Attributed with a flat-surface control — no waves, same
   offset, PAE 4/255 against 77/255 with them — so it is the field lookup, not the view transform.
   `p = camPos.xz + rd.xz * t` quantises to ~0.03 units at 3e5, which is 6% of the medium band's
   texel. **No consumer today**: nothing in this tree exceeds ~1,000 units. And the correct fix is
   engine-wide rather than water's — camera-relative rendering for everything, since water alone would
   be precise standing beside jittering terrain. Full recipe in 11.35's phase 3 as-built.
3. **The water gate corpus has a structural blind spot, and it has already cost one shipped defect.
   MEASURED and partly closed in 11.42.** The blind spot is no longer an inference: with the inverse
   twiddle's conjugate deliberately dropped from `water_fft_frag.glsl` — a transform that is simply
   wrong — **twelve arms passed**, `water-fft-det`, `water-fft-live`, `water-fft-motion` and
   `water-caustic` among them, and every variance ratio was unchanged to four decimals. What catches
   it is `water-fft-impulse`, which inverse-transforms two single modes through the same 14 stages
   and the same twiddle table and compares against the closed form: `mode_err` goes from 1.9e-7 to
   exactly 2.0. A centred impulse alone does NOT catch it, because a constant field is invariant
   under that conjugation — which is why there are two modes. 11.42 also added `water-fft-var`,
   `water-waterline`, `water-seastate`, `water-glitter` and `water-foam-persist`. The half that
   remains open is the one below: every arm still reads `water_fixture`, and a scene whose far plane
   is small relative to the horizon is still the instrument this corpus lacks.
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
   **11.44 added a second fixture and it does NOT close this.** `beach_fixture` is a dome rising out
   of the sea — open water at the rim, the whole shoaling ramp, a dry crown, and a shoreline facing
   EVERY direction, which is what lets an arm ask whether the surf is a ring rather than a band that
   happens to cross the frame. Genuinely new coverage, and a different blind spot: it is still a
   small scene, so the horizon-to-far-plane ratio it exercises is no better than the ramp's. It also
   opened a third, which is item 6 below.

4. **A camera AT the waterline is unhandled — SHADING HALF FIXED in 11.42, geometry half still open.**
   The two scopes are now one `seenFromBelow`, computed per PIXEL and read by both the normal flip
   and the optical-path branch, so a crest closing over a camera still above the still level is no
   longer charged its path against air. `water-waterline` asserts it on a framing the fixture cannot
   supply — crests have to close over the eye while the still level is below it, which needs an
   amplitude two orders above the fixture's 0.06 — and reads an in-frame ratio of the overhead-crest
   band against foreground the fix cannot touch: **0.1851 before, 0.2477 after**, with the reference
   box byte-identical across the change. The geometry half is NOT fixed and is now understood to be
   harder than this entry assumed; see item 1's correction. The original diagnosis, kept because it
   is what led to the fix:
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

5. ~~**The water shader does not know how big a world unit is.**~~ **CLOSED by 11.44**, and the
   prescription this item gave — "plumbing one float into the water program scales all five at
   once" — is exactly what shipped. The constants are stated in metres and converted at use through
   `waterUnitsPerMetre` off `Sky.world_units_per_km`, and `apps/tree` writes that number for the
   first time. The item's estimate of the damage was low: it priced the miss as refraction and
   caustics looking wrong, where the shoal window was the load-bearing one and made tree's surf zone
   0.38 m wide. See the 11.44 bullet above.

6. **The gate corpus has no coverage of breaking whitewater at all, and 11.48 measured that
   rather than inferring it.** Two shader edits to the surf path left **all 31 water arms at
   numbers identical to before them** — `water_fixture` is Gerstner at amplitude 0.06, which never
   reaches the depth-limited criterion, so the group was measuring containment and reporting it as
   coverage. `beach_fixture` reaches it only incidentally, through arms written for shoaling and
   for the shore band (`beach-shoal` 0.7962 → 0.7987, `beach-surf-zone` 0.770 → 0.762). A
   `water-breaking` arm — that the selection fires over a bed and not over open water, and that it
   is a band rather than a filled region — is what would have caught both of P6's defects at the
   time they shipped. This is item 3's blind spot in a second location, which is the argument for
   treating it as a corpus property rather than a fixture's.

7. **`beach-shoreline` was red for four specs and the renderer was never at fault.** It read 16
   of 18 azimuths at 11.44, 9 after 11.45, 7 by the time 11.48 measured it on master and 6 after
   that spec's P6 — and the shore ring was intact at every one of them. The arm took the brightest
   sample along a ray outward from the island, and that ray crosses the DRY CROWN: sunlit sand
   measures 580-591 luma against a shore
   band at 570-607, so the statistic was competing whitewater against beach and being decided by
   ties of a few codes — at two azimuths the peaks were EQUAL and the crown won on walk order.
   Clumping the foam (11.45) did not remove the ring, it removed the last few codes of margin the
   arm had been winning by. 11.44 had already made exactly this correction at the OUTER end of the
   ray, stopping it short of the sun's glitter, and recorded the reasoning — but that bound is
   worth **one azimuth** on the current build (6 of 18 bounded against 5 unbounded), because the
   crown sits inside the window's own inner half, which no bound on the ray can reach: the window
   starts a unit BELOW the waterline, deliberately, since the run-up climbs above the still line.
   The correction was made at the end of the ray where it was cheap and never at the end where the
   failure was. The arm now takes the argmax of |with bed − without bed| along the same
   ray, which cannot read dry sand because the bed does not touch it — already asserted by
   `beach-surf-zone`, whose crown samples must read exactly 0 moved. **16 of 18 against the same
   unmoved bar of 13**, and the peak tracks the waterline when the sea level moves (5.70 → 7.75 →
   9.15 as the mesh crossing goes 5.66 → 7.03 → 8.22), so it is testing the registration between
   the analytic bed and the drawn mesh that this fixture exists to test.

   **The transferable lesson is about argmax as a gate statistic.** An argmax needs no threshold,
   which is why 11.44 chose one and why the replacement keeps one — but it is only meaningful when
   nothing else in the search range can win, and that is a property of the SCENE, not of the
   statistic. Here a second bright object sat in range from the day the arm was written, the arm
   passed anyway on a margin of a few codes, and four specs of renderer changes were suspected
   before the statistic was. Item 6's blind spot in a third form: a green arm was reporting
   coverage it did not have.

**Refs.** Tessendorf, *Simulating Ocean Water* (SIGGRAPH 2001 course); Johanson, *Real-time Water
Rendering: Introducing the Projected Grid Concept* (2004) for the mesh that ships; Asirvatham & Hoppe
(GPU Gems 2) for the clipmap it replaced, whose real home is D4 below; Hasselmann et al. (JONSWAP,
1973) and Bouws et al. (TMA, 1985) for the two trains' spectrum; Monahan & O'Muircheartaigh (1980)
for the whitecap-coverage relation 11.47 calibrates against.
**Owns foundations:** none of the ones this entry predicted. What it does own is the water subsystem
itself, its bed-provider seam (`WaterHeightFn`, which `apps/forest`'s terrain satisfies directly),
the CPU wave query buoyancy would consume, and — since 11.45 — the shore chain, a CPU per-column
solver published to the shading stage through a std140 block rather than a texture.

### D4. Terrain — Effort XL — **SHIPPED, both halves: CDLOD (11.63) and STREAMING (11.69)**

**Spec 11.69 closed it.** A stored field's pyramid goes to disk as an fp32 tiled file
(`.cts`, `procedural/terrain_stream.c/h`) and only a rectangle of each level stays in memory,
anchored on the camera and the player. The coarse tail stays whole, which is what makes a miss
ANSWERABLE: a query whose clamped footprint escapes a window retries one level coarser, each
step covering twice the world, and what it returns is exactly what that level would have
returned unstreamed.

**WINDOWS, not a page table, and the reasoning is the opposite of D10's.** The access pattern
here is dense and anchored where virtual texturing's is sparse and feedback-driven: every heavy
consumer walks a contiguous square, and `sample_plane` runs ~30k times per patch build. A window
costs one containment test per sample; a table costs a lookup per corner of a 4x4 footprint that
straddles up to four tiles. The second thing windows buy is that residency becomes a pure
function of the anchor path, so 11.67's eviction ordering, want-guard and hysteresis all
evaporate rather than being re-implemented. **So the two residency systems in this engine are
deliberately different shapes**, and the thing that decides which is the access pattern, not the
scale.

**The rule the consumers are organised by** is worth carrying past terrain: anything that builds
a PERSISTENT artifact -- a Jolt BVH, a scatter placement, a cached patch mesh -- reads exact data
through a synchronous ensure, because a coarse answer there is not a softer picture but a wrong
one that outlives the residency which produced it. Anything transient takes the fall. That closes
the stale-cache problem structurally rather than with an invalidation channel, and it is why fill
is synchronous budgeted I/O with no async completions at all: there is no arrival order for the
content to depend on.

**Fixed the figure this entry has quoted since it was written.** 67 MB at 4096² and 268 MB at
8192² are the HEIGHT PLANE alone; with the three erosion masks a resident field is 291 MB and
1.16 GB, and 4096 builds no coarse levels at all -- the node-centred grid halves only while
res-1 is even, so a streamable field is 4097² or 8193².

What remains terrain's is not D4's: the splat is still one texture over the whole domain
(capped at 2048 under streaming, a bound rather than a fix) and its pyramid is D10's, and
per-tile erosion stays refused for the reason 11.63 gave -- the domain is a closed basin by
construction. Erode offline at top resolution; stream the result.

*The entry as it stood before 11.69 follows.*

**Spec 11.63 answered the question this entry says it had not chosen between**, and chose Strugar
over Hoppe: a CDLOD quadtree (`procedural/terrain_quadtree.c/h`) replaced `apps/forest`'s fixed
`tiles x tiles` grid, with per-vertex morphing to the parent surface. Sixteenfold ground area takes
the quadtree from 364 patches to 706 where the fixed grid goes 64 to 1,024 — the logarithmic-vs-
quadratic claim, measured. `TerrainField` gained an in-memory mip pyramid, and residency moved to
region cells that own their props and their Jolt collider. `apps/forest` is an ISLAND now, and
`--terrain-extent` grows it past a kilometre.

**Three consequences for what is left here.** The T-junction stitch this entry inherits is
SUPERSEDED for the quadtree and still owned for anything else: CDLOD closes a seam by morphing the
fine side onto the parent surface it already shares vertices with, so there is no junction to
stitch. The clipmap at `8d04658` remains the reference for rings, which nothing now plans to build.
And the pyramid is IN MEMORY, so D4's actual scope — rings or quadtree nodes as windows into a
*streamed* pyramid — is untouched: 11.63 made the terrain big, not paged.

**What that leaves unbuilt is exactly what the contradiction below predicted.** Streaming becomes a
problem when terrain becomes DATA, erosion (D7) made it data, and 11.63 made the domain large enough
to care. The figure to schedule against is still 67 MB at 4096² fp32 and 268 MB at 8192².

No terrain STREAMING system exists. `cetra/src/procedural/terrain.c/h` does -- an fbm-over-Perlin
heightfield with `terrain_height_at` and tiled mesh generation, landed for `apps/forest` -- and this
entry cited it by path 25 lines below while opening with "no terrain system exists". What is unbuilt
is D4's actual scope: rings as windows into a streamed mip pyramid. `terrain.h` says so itself. Real gap, but only for outdoor scale, and it depends on Wall 2 far more
than on any rendering technique — a clipmap without instancing, LOD and a streaming story is a
mega-mesh with extra steps, which `apps/tree`'s grass already demonstrates the cost of. **Do not
schedule this before Track E's E5** — now satisfied.

***And this entry contains a contradiction it never resolved.*** *Its scope is "rings as windows into
a streamed mip pyramid of streamed height DATA", and twenty lines below it depends on the surface
being **analytic** — `terrain_height_at` evaluable at arbitrary points — for the T-junction stitch it
inherits. Both cannot be the primary framing. **Today there is nothing to stream**: the height is a
pure function with no stored grid, so the mip pyramid this item is built around has no data to hold,
which is the real reason no measurement has ever demanded it. Streaming becomes a problem only once
terrain becomes DATA, and what forces that is **erosion** (D7), not scale. So this item is now
downstream of D6-D10 rather than their prerequisite, and it acquires a number when they land: a
4096² fp32 field is 67 MB and 8192² is 268 MB, which is the figure to schedule against instead of
the assumption. Note the stitch survives the change — it needs a surface evaluable at arbitrary
points, and a filtered grid sample is one.*

**`apps/forest` (11.29) is not this item and must not be read as it** — though 11.63 moved it a long
way closer. It WAS a consumer of E5: fixed tiles, per-tile LOD chains, everything resident, capped at
1 km² by exactly the streaming story this item owns, and the "mega-mesh with extra steps" the
paragraph above warns about. It is now a quadtree with resident regions and a shoreline, so the
tile-count and the everything-resident halves of that description are both retired; what survives is
the cap, because residency is in memory and nothing pages. What it contributes besides is a fixture —
the first content in the tree where instancing, LOD and culling all matter at once, and the scene
that found Wall 4.

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
**Depends on:** E5 (hard, in practice — satisfied), **D6 (hard: there is no pyramid without a grid)**,
D7/D9/D10 (soft: they are what make a grid big enough to need streaming), D3's tessellation path (soft).
**Inherits:** D3's clipmap implementation at `8d04658`, including the T-junction stitch.

### D5. By-example stochastic texturing — SHIPPED as spec 11.46, unbooked before it
Never a row here, and it would have been assumed blocked if it had been: it reads a
histogram-transformed copy of a texture plus an inverse table, which sounds like two units in the
most saturated program in the tree. It cost **zero** — see the ledger's fifth escape. Heitz & Neyret
2018: skew the UV onto a triangle lattice, sample the texture at a different random offset per
triangle vertex, and blend, so neighbouring points read different parts of the tile and the period
stops existing. Opt-in per material (`stochastic_scale`), and only `apps/tree`'s sand takes it, so
all 24 goldens are 0 px.

**The blend is where it lives or dies, and the reason is why the histogram machinery is not
optional.** A weighted average of three taps divides the variance — contrast collapses and the
ground is flat mush. Dividing by the root of the summed squared weights preserves a single sample's
variance instead, but that is exact only for a Gaussian input, which is what the transform
manufactures. **An earlier claim that the transform could be skipped is kept here because it should
not be made again**: the argument was that a sum of noise octaves is near-Gaussian by CLT, and the
field is a sine train (arcsine, U-shaped) plus fBm plus Worley plus a deliberate skew in the ripple.
Measured contrast held to **0.15%** across the change — 0.02876 plain against 0.02881 stochastic at
the same mean, which is the whole claim: same statistics, different arrangement.

**The normal map rides the same lattice and does NOT get a transform, because it cannot.** Its three
channels are a unit vector, so per-channel transform-and-blend breaks the constraint and produces
non-unit normals with a distribution the map never had. Blended in slope space instead — `-dh/du`,
`-dh/dv` are unconstrained and blend meaningfully, and reconstruction restores the constraint
exactly. Same space D3's far-field handover works in, for the same reason: slope filters linearly
and a normal does not. Relief contrast measured 0.02881 → 0.02893, i.e. preserved rather than
softened, which is the failure it was watched for.

**Roughness is deliberately not done.** It comes off the shared `materialArray` layer rather than a
`sampler2D`, so stochastic sampling means transforming a packed multi-material `sampler2DArray` that
six scalar masks share — much larger, and the least valuable of the three, since the visible tiling
was carried by albedo and relief. **Also unmeasured: the cost.** Six fetches on the ground where
there were two, and `--profiler` has never been pointed at it.

**And two of the three defects it fixed were never rendering bugs**, which is the transferable
lesson. `apps/tree`'s beach was reported repeatedly as "a grid", "columns", "the same tile
boundary", every report read as a water bug and none of them was: `sand_height_field` sampled an
unbounded lattice so all forty tile boundaries were discontinuities in the DATA, which no mip level
or anisotropy setting touches; and the island mesh wrote a **circular** tangent for **planar** UVs,
so the normal map ran through a basis disagreeing with the map it indexes, creasing along every
triangle edge. **A tangent is `d(position)/du` and nothing else** — a frame that looks geometrically
natural for the mesh is wrong unless the UVs agree with it.
**Refs.** Heitz & Neyret, *High-Performance By-Example Noise using a Histogram-Preserving Blending
Operator* (HPG 2018).
**Depends on:** nothing. **Wall 1:** avoided by construction (the fifth escape).

### D6. Heightfield backend for terrain — Effort S/M — **DONE (spec 11.59)**
`terrain_height_at(params, x, z)` is a pure function of position, and five documents assert that
purity as a contract (`terrain.h:11-13, 42-45`, `forest.c:719-721`, D4 below, `specs/11.35:564`,
`specs/11.36:489`). It is what lets **eight** consumers agree with nothing shared between them: the
visual tiles and the physics collider (different lattices — 2.60 vs 3.91 units — that must produce
the same surface), the scatter's slope gate, the water bed's 256² bake, the character spawn, the
`--trace-player` diagnostic, the normal central-difference, and the per-frame camera floor clamp.

This item does not remove that. It makes the SOURCE pluggable — analytic fbm, or a filtered sample of
a stored grid — because a filtered grid sample is still a pure function of `(state, x, z)` and still
evaluable at arbitrary XZ. Every consumer keeps working; `TerrainParams` grows a field pointer, and
NULL is today's path byte-identical.

**It exists because D7 cannot be built without it**, and that is the whole justification. Nothing
about the analytic terrain wants a grid; erosion does, structurally (see below).

**Three decisions are contracts rather than details.** The filter must be **C1** — `terrain_normal_at`
central-differences at `h = extent/(tiles*tile_segments)` = 1.30 units, and bilinear's piecewise-
constant derivative would facet normals on cell boundaries, reaching the scatter's slope gate as well
as the shading. Out of domain the field **clamps to its edge**, because `forest.c:939` queries at the
third-person camera eye which can leave `[-extent, +extent]` and the analytic field answers there —
the same policy `--water-extent`'s bed already documents. And thread-safety is **not** improved: the
analytic path memoizes a permutation table in file statics (`terrain.c:52-63`) and stays unsafe, so
the header must say the field path is incidentally safe rather than imply a new guarantee.

**Depends on:** nothing. **Owns foundations:** the grid D7, D8 and D9 all read.
**Shipped as written**, and the seam held: all eight consumers took a field with no change, and
the analytic path measured **0 px** on forest against a 0 px floor. The one thing the entry did
not anticipate is that a field has to carry its own HEIGHT RANGE — `terrain_tint` normalised
altitude by `TerrainParams.height`, which is the fbm's amplitude and inert once a field exists, so
a 0..1200 import into a params that says 95 put the whole terrain over the snow line.

### D7. Hydraulic + thermal erosion bake — Effort M — **DONE (spec 11.59)**
The item that closes the largest visual gap against a shipped AAA terrain, and the reason D6 exists.

**Erosion is a simulation over a grid, not a function of position.** The height at a point depends on
how much water flowed through it, which depends on the entire upstream watershed. There is no
`f(x, z)` to write. This is why UE, Unity, Frostbite and Decima all consume a baked heightfield and
why the generation happens in Gaea / World Machine / Houdini — not preference, but the shape of the
algorithm.

**And the silhouette is the smaller half of the payoff.** Eroded terrain reads as real because the
materials are placed by the same process that shaped the ground: the sim knows where water flowed, so
it yields flow-accumulation, deposition and wear masks that put gravel in stream beds, silt on valley
floors and bare rock on scoured ridges. `terrain_tint` (`terrain.c:121-155`) approximates this today
with slope + altitude + two decorrelated Perlin fields — the best available without a sim, and exactly
why it reads as procedural: **the dirt is not where water would put dirt.** So D7 and D9 are one
item split across two specs; the sim produces precisely what the layer system consumes.

**CPU, grid-based (Eulerian), double-buffered, threaded like `sky_clouds.c:116-142`.** Not a GPU
fragment ping-pong, for three reasons. The output feeds the physics collider and the scatter, both
CPU — and **JoltC exposes no `HeightFieldShape`** (no such symbol in `ext/JoltC/JoltC/Functions.h`),
so a grid becomes a triangle soup either way and a GPU sim would mandate a readback. The cloud bake
already proves the determinism property this needs: disjoint slabs, zero synchronisation beyond the
joins, output bytes identical at any thread count — which grid erosion inherits **if and only if** it
is double-buffered, since then each output cell reads only the previous buffer. And the tree's own
readback policy already prefers blocking-and-deterministic over async-and-fast (`postfx.c:1762-1766`
rejected a PBO+fence measured 5.253 ms → 0.033 ms because it broke headless run-to-run equality).

**Droplet/Lagrangian erosion must be refused on that same test**: droplets write to shared cells, so
the result depends on scheduling and no thread count agrees with another. If 4096²+ ever forces the
GPU, `_water_fft_transform` (`water.c:1292-1315`) is the tree's worked N-stage ping-pong template.

**Refs.** Mei, Decaudin & Hu, *Fast Hydraulic Erosion Simulation and Visualization on GPU* (2007).
**Depends on:** D6 (hard). **Owns foundations:** the flow/deposit/wear masks D9 blends by, and the
band-parallel bake primitive, now shared with the cloud noise bake it was copied from.

**Two things this entry could not have known, both found by measurement.** Mei's own
**semi-Lagrangian sediment transport leaks** — 3.06% of the budget over 220 iterations, because a
bilinear gather conserves mass only for a divergence-free field. Moving the load by the FLUXES
instead closes it to 5e-09. And **rain over evaporation IS the equilibrium water depth**: the
first defaults put ten units of standing water over two-unit cells, so every cell drained into
every other and the mask painted a kilometre of ground one colour. Cost, release build: **452 ms
at 512² × 220 on eight threads, 1839 ms on one.** The same bake on the default debug preset is
2.3 s / 14.8 s, because `build.sh` passes no `-O` at all — a fact worth knowing before quoting any
CPU figure from this tree.

### D8. Heightmap + mask import / export — Effort S — **DONE (spec 11.59)**
The authoring path in and out, and the item that unifies the two producers: **the bake writes what
the importer reads.** Dev-time bakes and saves; ship-time loads with no sim; a Gaea or World Machine
export drops into the same slot. Without it, D7 is a demo and cetra has no answer to "an artist made
this terrain".

`.r16` — headerless 16-bit unsigned, resolution from file size — is the literal UE / World Machine /
Gaea interchange format and needs no new dependency in either direction. 16-bit PNG on the read side
via **`stbi_load_16`, which is vendored and called nowhere in this repo** (`texture.c` is 8-bit
throughout; `stbi_loadf` appears only at `ibl.c:261`), because Gaea exports PNG16.

**Deliberately not routed through `texture.c`.** `texture_gl_formats` (`texture.c:185-199`) hard-wires
*unsized* internal formats from channel count, so there is no path through it that can request 16-bit
or float at all. This is CPU-side height data, not a texture load, and conflating the two would put a
format the GL never sees behind a function whose whole job is choosing GL formats.
**Depends on:** D6 (hard).

**The masks are the half that is easy to drop, and 11.59 dropped them first.** The save wrote
height only, so a shipping load got the eroded GEOMETRY and then shaded it with the
slope-and-altitude guess this whole track exists to replace — the failure D7 opens by describing,
arrived at through D8's own round trip, with every gate arm green. Measured flow 0.684 out, 0.000
back. **A round-trip arm that compares only the geometry is not a round-trip arm**, which is the
transferable part.

### D9. Terrain material layers — Effort M — **DONE (spec 11.60)**

**This entry called for a second surface program and that was wrong on three counts**, all of them
found by reading the code the entry describes:

1. *"Lights, shadows and IBL arrive with no call-site work"* is true and counts the wrong half. The
   UBO binding is free; the SHADING is ~740 lines written inline in `pbr_frag.glsl` with no include
   behind it — PCSS/CSM, the clustered light loop body, the IBL split-sum block. A second surface
   program duplicates those or extracts them first.
2. The precedent it names is not one. `create_pbr_skinned_program` compiles **`pbr_frag_shader_str`**
   — a different VERTEX shader over the same fragment shader. The tree's only real second surface
   fragment shader is `water_frag`, which does not include `lights_ubo.glsl` at all and pays for it:
   no clustered lights, no punctual shadows, no LTC, no GI. Right for an ocean, ruinous for the
   largest opaque surface in a scene.
3. The declaration it says is missing already exists. `pbr_frag` is at exactly 16/16, so the entry is
   right that there is no room for a NEW one — but `materialArray` is a `sampler2DArray` already declared,
   already built and already bound on every draw, and `material_texture_layer_for` dedups by GL id and knows
   nothing about masks.

**What shipped is a material feature, not a program**: N layers as further tenants of the material
texture array, for **zero new sampler declarations, zero new units and zero new programs**. The test
that separates the two cases is whether the LIGHTING MODEL differs, not whether the texture count
does. Water's does. Terrain's does not — it is the most ordinary PBR surface in the engine, it just
has more than one albedo.

The feature also shipped **inert in `apps/forest`** and was caught in review: the splat was addressed
by UV1, and `build_grid` writes UV1 as a literal zero at every terrain vertex, so a kilometre of
ground sampled one texel. See 11.60's as-built for that and six more defects, and for the three gate
arms that passed their own falsification while measuring something else.

### D9. Terrain material layers — the original entry
No layer-blending system of any kind exists in this engine — a sweep for splat, decal, detail-map,
triplanar and blend-weight returns zero hits outside vendored trees, and `specs/11.29:111` states the
absence directly. Terrain is tinted per VERTEX, at ~2.6 units between vertices, which is why the
ground does not hold up close no matter what the palette is.

**This was going to be booked at L against Wall 1, and that would have been wrong.** `ocean.glsl:63-78`
states the rule 11.45 established: *a cap counted in declarations is a cap on how many DISTINCT
shapes of data a program reads, and six identical fields were only ever one shape.* N material layers
are one shape. `sampler2DArray layerAlbedo` + `sampler2DArray layerNormal` + a weight source is
**three declarations regardless of N** — so multi-layer terrain does not need virtual texturing to
be possible, it needs a texture array and a program with room.

The program is the cheaper half of the two precedents available. Terrain tiles are **ordinary Meshes
with a Material**, so this is `pbr_skinned`'s pattern (a second program pointed at by
`set_material_shader_program`, `apps/render/src/render.c:2435-2440`) and not `water_frag`'s bespoke
VAO and hand-rolled draw. Lights, shadows and IBL arrive with no call-site work: `ubo_wire_blocks`
binds every UBO at link time against buffers bound for the context's lifetime (`ubo.c:23-25`), and
`uniform_set_*` on an undeclared name is a location-guarded no-op, so `_submit_item` can upload the
pbr-shaped uniform set to any material program.

**The gotcha to carry across is `water.c:1975-1984`**: with no IBL you must still POINT each
`samplerCube` at its unit, or it defaults to unit 0 where a 2D texture lives — and two sampler types
against one image unit is undefined for the WHOLE program, not merely for the untaken branch.
**Depends on:** D6, D7 (for the weights).

### D10. Virtual-texture compositing — Effort XL — **stages 1 and 2 DONE (11.66, 11.67); first CONTENT in 11.68**
What UE actually does: composite the layers once into a cached virtual texture, then sample one
albedo and one normal at runtime, with meshes able to blend into the same cache. Page table, physical
cache atlas, and a feedback pass telling the CPU which pages were wanted.

**Shipped as a domain-wide composite pair plus a runtime detail term, and the second half is what
this entry was missing.** Two corrections to what is written below, both measured in 11.66. The
"eight taps" this entry priced D9 at is the flat-ground best case: the real formula is
`1 + A*W + A*B` — 9 flat, **17 on a 45-degree face, 25 worst** — and `triplanar.glsl` names the
45-degree case as most of an eroded terrain. And a composite cache is structurally a MACRO cache at
any affordable density (today's ground resolves 3.9 mm/texel; matching that over 1 km is 524 GiB,
and even a 30 m near field is ~1.4 GB), so "sample one albedo and one normal" is achievable only
with the grain restored at runtime. The bake freezes every layer tap at its top mip so the macro
carries no grain at ANY resolution, and the shading path samples the macro pair plus ONE triplanar
detail tap of the dominant layer's own maps — `3 + 2A` taps, **independent of layer count**, which
is what actually unblocks the three triggers below. Byte-exact on flat content by construction
(ratio of a flat map to its own mean is exactly 1), 0 px against the per-texel path on forest's
interior ground, and the per-texel path stays live for UV1 splats and anything vertical. Zero new
sampler declarations: the pair rides `albedoTex`/`normalTex`, provably unread when `layerCount > 0`
— the strongest of `pbr_frag`'s alias arguments, since it rests on the material rather than a pass.

**Stage 2 SHIPPED too (spec 11.67): pages, residency, and the feedback that fills them.** A
64-slot guttered page atlas (256² tiles at 4x the fallback's density — a RATIO, which is what
bounds the virtual grid at 34² for every domain size), a page table in a **UBO** on binding 7
(the IES lesson again: an indirection is a TABLE, and tables in uniform space cost zero
samplers), the page pair riding units 3/4 freed by an IES-style refusal (a layered material
refuses its clearcoat-normal and height maps — the height half independently a bug fix, POM ran
a dead march that could discard), and a **depth-tested GPU vote pass read back through a PBO
ring at fixed latency** (always the slot from 4 frames ago, never "whichever fence signalled" —
which is what keeps residency a pure function of frame history and every gate arm
deterministic; measured: the whole stage costs 0.23 ms GPU at a half-ground framing and the
fixed-latency map never stalls). Residency = frustum prediction sorted (seen, distance, id):
feedback's occlusion-awareness is one comparator, proven by the arm where a hidden floor casts
no votes while the frustum wants all of it. On today's content pages are a measured **0 px
identity** — the macro's finest content is the 513² splat the fallback already over-resolves —
so what remains for D10 is CONTENT: roads and decals composited into pages at bake time (they
draw into the atlas FBO and never touch `pbr_frag`'s ledger), and the general-mesh era that
turns feedback from a refinement into the only correct source.

**The first of that content shipped as spec 11.68, and it did NOT draw into the atlas FBO** --
which is the correction this entry needs. A road is a procedural override of the SPLAT WEIGHTS
inside `sampleLayeredSurface`, before the height blend, so the bake inherits it by calling that
function unchanged and every consumer follows for free: the fallback, the pages at 4x density,
the per-texel path, the dominant index and the detail term. Drawing into the FBO would have been
a second implementation of the blend, and `--no-layers-vt` would have stopped being lossless.
Segments ride a UBO on binding 8 (the IES lesson a fourth time: geometry small enough for
uniform space costs zero samplers). **Pages are still a 0 px identity at the derived resolution**
-- what pages resolve and the fallback cannot is four times narrower than a fallback texel, which
the fixture's camera does not resolve at all -- but roads are the first content a forced-coarse
fallback can distinguish: a band just inside a road's edge reads the road's own 48 through pages
against 66 through the fallback alone.
**What that leaves for the content era is the part a weight override cannot express**: decals
that are not made of the ground's own layers, and width that varies along a course.
**Depends on:** D9 (hard, shipped). The content era also wants D4-streaming for its source data.

### D11. Large-world origin rebasing — Effort M/L — **DONE (spec 11.62)**
Independent of the terrain chain and needed by anything that wants a world bigger than a few
kilometres. fp32 world coordinates lose the precision to hold still: UE4 capped its world at ~20 km
for exactly this reason and UE5 shipped Large World Coordinates (fp64 positions) to break the cap.

Shipped as origin SHIFTING, not fp64: `scene_set_world_origin` schedules, the engine applies at the
frame top, and `Scene.origin_shift_distance` re-centres automatically once the camera drifts. The
measurement that shaped it is that the dominant cost of moving a world is NOT precision — it is
anything that reads a world position as an IDENTITY. Forest's wind phase hash re-phased 43% of the
frame from twelve units out, flat with distance, where the precision curve underneath is 0.10% at 12
and 45% at 262,140. So the feature is two rules, and the second is the larger: everything linear
takes the delta, and everything that hashes or tiles is handed the authored position back through
`include/world_origin.glsl`. World partition CELLS arrived with 11.63 -- forest's regions own their
props and their collider and load and free against two anchors -- but in memory only, so what remains
D4's is paging them off disk. 11.63 also proved the second rule holds under residency: a region that
is freed and reloaded across an origin shift comes back at the same AUTHORED positions, which
`region-shift` reads through a snapped authored digest because the raw storage bytes are supposed to
differ by the delta.

**It also fixes a defect that is already live and has nothing to do with world size.** The outermost
shadow cascade is fitted around a hardcoded `{0,0,0}` (`shadow.c:1250`) at a fixed ortho size, while
the inner cascades follow the camera — deliberately, since the outermost is the camera-independent
fallback that guarantees no shadow ends at a boundary that moves. The consequence is that terrain
placed away from the origin lost its far shadows with no diagnostic, which `procedural/terrain.h`
used to warn about and work around by centring the terrain. Fixed in 11.62: the hardcoded local is a
`ShadowSystem.scene_center` field, defaulting to the origin so every existing frame is unchanged, and
the terrain's centring workaround is gone.
**Depends on:** nothing.

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

### E2. 3D LUT colour grading — Effort S — **DONE (11.58)**
A `.cube` reader, a `sampler3D` on the tonemap program's unit 11, and the table applied after
`displayEncode`. `--lut` / `--no-lut` / `--lut-strength` / `--lut-interp`, plus a `post.lut` block.
All 24 goldens 0 px: the loaded texture is the enable, so with no table the frame is unchanged.

The text below is the original entry. Three of its five sentences held; the two that did not are
worth keeping visible.

> `tonemap_frag.glsl:52-55` grades with lift/gamma/gain and nothing else. A 32³ `.cube` LUT is the
> format a colourist actually hands back, and post shaders are nowhere near their sampler budget, so
> this is a self-contained pass with a real workflow payoff. Sequence it *after* the tonemap, and pin
> which space the LUT is authored in — the working-space contract from 10.1/10.2 is the thing this
> item is most likely to violate quietly.

**The premise held and the sampler claim held** — `tonemap_frag` declared 10 of 16, and unit 11 was
free. **The warning was the single most useful sentence in the entry**: "after the tonemap" is not
precise enough, because the frame passes through *two* display-referred states in this shader.
`toneSelect` returns LDR-linear and `displayEncode` gamma-encodes it, and a `.cube` is authored
against the second. The lift/gamma/gain grade sits in the first, so the obvious placement — beside
the grade it extends — is the wrong one.

**The size was wrong.** 32³ is not a size `.cube` produces. 33³ is Resolve's export default, 17³ is
common for small looks, 65³ for precision ones; the loader reads `LUT_3D_SIZE` and bounds it 2–64,
and the fixtures deliberately carry three different sizes so a hardcoded one cannot pass.

**And the entry did not anticipate the interpolant question, which is where the measurement went.**
Tetrahedral against trilinear is **0.076 of one 8-bit code** at 33³ — invisible on any real table.
It is the default anyway, for two properties that are checkable rather than aesthetic: greys stay
exactly grey (trilinear tints them 5 codes through a table that is identity on the diagonal), and an
identity table is a bit-exact no-op (0 px, against trilinear's 12,088 px at PAE 1/255 — the texture
unit's fixed-point filter weights, which fp32 storage does not fix and was rejected for). Trilinear
is kept behind a flag because **tetrahedral alone cannot be falsified**: nothing in a frame is a
reference for it, and a path that silently degenerated would pass every arm written against it.

**Log/pre-tonemap was declined, and not for the reason it first looked like.** It is not
structurally hard here — `agxTonemap` already log-encodes with an EV window, transforms, and
linearizes back to `toneSelect`'s contract, so a log LUT is a fourth branch in exactly that shape.
It is declined because no off-the-shelf `.cube` can be authored against a log window we invent, it
would replace the tonemap rather than compose with it, and it would reach the GI debug view, which
calls `toneSelect` where the display path's early return does not.

**Depends on:** 10.1-10.2's working-space contract (shipped). **Wall 1:** unaffected.

### E3. Histogram auto-exposure — Effort M — **DONE (11.52)**
A 128-bin gather histogram over the existing 64² measure target, collapsed by a percentile-clipped
reduce: two raster passes replacing `glGenerateMipmap`, since GL 4.1 has no compute and 4096 source
texels make a gather cheaper than any scatter trick. Plus metering modes, min/max EV bounds, split
adaptation rates, and the controls to drive all of it.

**Both reasons this entry gave were wrong, and the item was right anyway.** The text below is the
original; it is kept rather than rewritten because the correction is the useful part.

> `lum_measure_frag.glsl:44` writes `log2(lum)` and the mip chain averages it: a flat, unweighted,
> whole-frame log-average with no metering mask and no percentile clipping. One bright practical, one
> sun disc, one specular highlight drags the entire frame dark — and because exposure multiplies every
> pixel, that is also the single largest source of cross-build non-determinism this repo has measured
> (99.77% of pixels, per CLAUDE.md).

The bright-pixel failure was **already defended twice**: the average is a geometric mean (the
shader's own comment says it is "robust to a few very bright pixels, unlike an arithmetic mean") and
there was an explicit clamp whose comment reads "stops one sun pixel from crushing everything else"
— a ceiling already raised once from 10000 after exactly this class of bug. And the determinism
claim was **already collected by `EXPOSURE_ADAPT_SNAP`**: once it engages the adaptation holds no
history at all and exposure is a pure function of that frame's measurement. (When it engages is
scene-dependent — frame 91 on the convergence fixture. An earlier draft of this entry said frame 12,
which was measured before the percentiles changed what the meter reads.) Two runs of one build are bit-identical over 200 frames.

So the justification is **capability parity** — every comparable engine ships this and Cetra shipped
the tier UE calls *Basic* — and that was enough on its own.

**What the instrument found instead, which nobody had looked for.** Nothing could observe the metered
value from outside the process: no log line, no readout, and the `exposureEV100` UBO slot reserved
for one was read by nothing. `--exposure-probe` went in first, and the meter turned out **not to be
linear in scene radiance** — scaling every emitter by 1000 moved the metered value 8.36 stops instead
of 9.97. The cause was the absolute floor pinned at the key: a scene metering near 0.18 has most of
its frame clamped *up*, inflating the mean 3.05×, which is `log2 = 1.608` and exactly the deficit.
Percentile rejection is scene-relative and fixes it: **−1.61 stops → +0.021**.

Two traps found on the way, both recorded in the spec because both produced a confident wrong answer
first. Removing the *ceiling* as well produced a **runaway** — a bright scene drives the gain to the
20-stop floor, pre-exposure collapses past fp16 subnormals in the HDR buffer, and the meter, which
divides pre-exposure back out, amplifies the remainder into 1.15e7 nits and pins there. The metered
bound was credited with fixing that and **review showed it had not**: `exposure_auto_gain` floors the
gain at 2^-20, which pins once the metered value passes log2 17.53 — below the bound — so the gain
was already bounded and the bound only made the recorded luminance saner. It changed no pixel.

And the `SCALE_GATES` shape **cannot** be used to test a live meter at all: scaling emitters by K
while dividing the camera by K double-compensates, so `pre_exposure` lands K² off.

**Defaults 0.70/0.95**, aggressive at the bottom for the reason UE's Low Percent defaults to 80 — a
meter that includes the black background is measuring the background, and zero times a thousand is
still zero. It costs 0.61 stops on a sky-lit fixture, accepted rather than hidden. No golden moves:
all 24 pin exposure, which is also why **auto-exposure was the least-tested subsystem shipping on by
default**, and the new `exposure` gate group is six arms over it.
**Refs.** Lagarde & de Rousiers, *Moving Frostbite to Physically Based Rendering* (SIGGRAPH 2014
course) — the exposure section.
**Depends on:** nothing. **Wall 1:** unaffected — it cost no sampler unit.

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
  **Both of the two bullets above are CAMERA-LANE properties since 11.90, not engine ones.** A pass
  free to reorder pays neither, and for a structural reason rather than a measured one: the shadow
  pass compacts and regroups its own casters, so its per-`(mesh, level)` counts — and therefore its
  draw count — are a function of WHICH casters a layer wants, not of the order they arrive in. The
  camera lane keeps both costs because it must keep front-to-back order, a trade it makes
  deliberately at `DRAW_SORT_DEPTH_BUCKETS`. `forest-shadow-lod` holds the level half of that;
  nothing yet holds the order half, which `forest-order` could assert for free since it already
  renders both scatter orders.
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

**On `apps/forest`, the shipping AA path — SUPERSEDED, see 11.31.** This read: opaque
**397 → 234 ms (−41%)** with both, depth complexity **3.87 → 1.94**, ordering alone −36%, prepass
alone −11%, "worth more together". 11.31 re-measured the whole table on a corrected budget and got
**306.0 ms @ 1.93 neither; sort only 168.6 ms (−45%) @ 1.08; prepass only 173.2 ms (−43%) @ 0.72;
both 173.0 ms (−43%)** — so the two are substitutes, not complements, and "worth more together" was
an artefact of the masked exclusion. Tier-4 row 31 quotes the replacement pair; these are kept only
so the older numbers are not mistaken for current ones.

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

### E7. Occlusion culling — Effort L — **DONE (spec 11.98), and both of this row's premises fell**

**Shipped as CPU masked software occlusion culling** (the Frostbite / Intel-MOC family): authored
occluder proxies rasterised into a 256×144 fixed-point CPU depth buffer with per-tile coverage
folded into an EMPTY sentinel, from THIS frame's camera, before the draw list is culled. One test
ahead of the frustum test in `draw_item_visible`, settled once per frame as a byte on `DrawItem`.
Camera-pass-only structurally (one `CullView` constructor initialises the switch false; one site
sets it), skipped under captures, rejections folded into `meshes_culled` so every existing
submission gate held unmodified. Two authoring channels: a top-level `occluders: [{boxMin,boxMax}]`
array and a per-material `occluder` row, both carrying the one contract — the box sits INSIDE
opaque geometry, and the raster's four conservative roundings make that the only path to a wrong
pixel. The probe (`--occlusion-probe`) checks the hierarchy against a per-pixel twin at 4×
resolution over the identical recorded box set, and the interior contract box-against-triangles.

**"The latency story is the whole item" was the first premise to fall.** The delivered shape
rasterises occluders from the CURRENT frame's camera on the CPU — no query, no readback, no
last-frame depth — so the latency, the popping and the accept-one-frame trade this paragraph
described do not arise. The software path was not the compromise; it was the design.

**"Not yet justified" was the second, and it fell to a measurement.** The row reasoned from
`apps/forest` — instanced foliage on open terrain, the least occludable content imaginable. On the
portal fixture (a wall with 300 distinct meshes behind it): **opaque 7.643 ms culled vs 8.708 ms
not, +14% for turning it off, separated over four interleaved pairs**, ~1.38M triangles and 300
draws removed for exactly 0 px. The loss the row predicted is measured too, as integers: on the
scatter twin, culling a third of the triangles splits one instanced run, draws 5 → 7 — and the
unasserted clock still netted ahead (1.878 → 1.800 ms). forest is unchanged byte-for-byte: it
authors no occluders, and the feature's whole off state is two integer compares. The win is
author-opt-in, priced on the fixture built to price it, not implied scene-wide.

**One defect survived the ledger and fell to the review round** (recorded in the spec): the first
raster stood ALL SIX faces of a box, on the argument that min-merge makes a back face harmless —
true along any ray that passes through both faces, false once the near plane clips the front face
away, because the renderer then shows through the opened shell while the raster still stands the
back face across the frame. Measured at **118,794 px of visible scene culled** (24.7% of frame)
with the eye a near-plane's depth from a wall — the one regime where the conservatism contract's
arithmetic was arguing about the wrong surface. Faces are now CLASSIFIED: only a face whose
outward side holds the eye rasterises, which keeps exactly the region the renderer keeps and
subsumes the camera-inside-a-box skip as a theorem instead of a second mechanism. The regime has
its own fixture variant and identity arm (`occl-near`), red at exactly that count under the
reverted fix and green nowhere else — no other camera in the suite can see the difference.
**Depends on:** nothing now. **Wall 1:** unaffected — no sampler, no uniform, no shader edit.

### E8. Fixing the wind cull — Effort S — **DONE (spec 11.53)**
This section said `draw_list.c:94` marks any `wind_response > 0` material `DRAW_UNBOUNDED`. The line
was `:110` and the condition was `mesh->is_skinned || mat->wind_response > 0.0f`, so **every skinned
mesh in every scene was exempt from the camera frustum and every cascade as well** — and the fix this
section prescribed, expanding the AABB by maximum wind displacement, does not address that case at
all. A bind-pose bound is not a loose bound on an animated pose; it is a bound on a different shape.
The flag's own comment knew this. The row did not, for five specs.

**Both bounds were closed-form and nobody had looked.** Every term in `windOffset` is sin/cos-bounded
and nothing scales with the vertex position — it enters only inside sine *arguments*, as phase
decorrelation — so the margin is `strength x response` times a per-mode constant, plus two per-vertex
maxima measured at upload because UV1 is raw unclamped data. And skin weights are convex, so a posed
vertex lies in the convex hull of its bones acting on it alone: bound each bone's own vertices in bind
space, union through the pose, and no blend is ever evaluated.

**`DRAW_UNBOUNDED` is deleted rather than fixed.** It was a build-time flag decided in `classify`,
which sees neither `scene->wind` (GUI-mutable) nor the pose, and which runs at the frame's first pass
— the shadow pass, before the app steps the animation. The decision moved into a `CullView` carried
where the frustum already travelled, built by each pass from what it is about to draw with. Changing
the type is what found every call site; a flag left in place would have gone on being wrong silently.

**The payoff needed a second half this section booked wrong.** It called per-instance wind phase a
natural pairing worth "one instance-block field". It is neither optional nor a field: wind is applied
in object space, so making 2,000 trees cullable does not stop them swaying in lockstep, and the phase
is better *derived* from the object's world origin — an InstanceBlock entry costs a kilobyte of std140
padding to restate what the model matrix says, and a phase keyed to batching would flip as LOD and
culling reshuffle runs.

Measured: `ivy_arcade` aimed away goes 49 of 73 culled to **73 of 73, 0 draws**; raiden **18 of 18**;
`apps/forest` has its wind and it costs **5 meshes of cull out of 28,256**, against a counterfactual
of at most 3,064 opaque culled where it measures 4,085. It also closed a latent defect nothing had
reported: `_count_late_meshes` tested the raw bind AABB while the lane that draws those meshes
exempted them, so a swaying or posed transmissive mesh could be drawn without the refraction resolve
it samples.

**What the review found, and it is the same lesson one level down.** This item was booked from a row
that described half the code; it then shipped a fixture that tested a fraction of the item. The
fixture authored `turbulence: 0` and no `windMode`, which collapses `wind_max_offset` to
`strength × response` — so none of the six shared coefficients, neither per-mesh maximum, nor any of
the vegetation branch affected the value under test, and the vegetation branch is what `apps/forest`
runs on. Zeroing that branch now moves the arm by 4,846 px; before, it moved nothing.

Two more of the same shape. The shared-constants file was written as though the technique were new,
citing the weaker "checked by eye" pattern it improves on — while **`shore_constants.glsl`, four
files away in the same directory, already does exactly this** and its header states that the `f`
suffix is load-bearing on the C side. The suffix was missing, so the bound evaluated in double
against a float shader. And the gate group opted out of the arm-list checker citing two precedents,
**neither of which was one**; fixing it at the root — the file's only helper that printed its own
verdict now returns it — took the checker from 9 groups to 10.

None of the three was found by looking at the picture, and none would have been.

**11.54 closed the gap the review left open.** E8's bound and the shader it bounds share their
coefficients and cannot share their arithmetic, so a term added to `windOffset` made the bound
non-conservative with nothing to say so — and `cull-margin` could not see it, because it pins the
bound at one camera, one frame and one wind field. `--wind-bound-probe` drives `windOffset` itself
through transform feedback and prints the measured travel beside the claimed bound.

**A CPU port was built first and thrown away, and the numbers are why.** It catches a term added to
the model (0.966 → 1.258) and reads *straight through* a term added to the GLSL (0.966, unmoved) —
which is the failure the probe exists for. Capturing the shader catches that same edit at 1.258 and
leaves no third copy. The two independent routes agree to six decimal places, which is its own
evidence. `cull-bound` asserts in both directions: `measured <= bound` for correctness, and a floor
on the tightest sweep so a grid that stopped sweeping reads as a failure rather than as a safer
bound.
**Depends on:** nothing. **Wall 1:** unaffected.

## Track F — The asset pipeline

*Added after B15 closed the table, by the comparison-against-other-engines habit this document
recommends to itself and had run exactly once (11.59, which added D6–D11). Every other track asks
what the renderer DOES; this one asks how content reaches it, and the answer had never been
written down anywhere. Note `docs/game-engine-status.md` — a third document, and the one that owns
everything that is not the graphics pipeline — already books audio, animation blending, IK,
gamepad, save and UI. It does **not** book either row below.*

### F1. Block-compressed textures — Effort M — **DONE (spec 11.85)**

*As built: the platform reading below held exactly — BC7 is unreachable, RGTC and S3TC are
what this driver takes — and the entry's own warning about the array's single-format rule
turned out to settle the item rather than complicate it. **Four things it did not predict.***

***The array was the wrong prize, and the probe is what said so.*** *Forest's 117.3 MB is
eleven layers: four albedo-plus-height maps, four packed normal-roughness-AO maps, a splat
and two roughness masks. The packed maps and the splat are **three independent quantities in
one image**, which BC4 (one channel) and BC5 (two, paired endpoints) cannot compress without
corrupting quantities that have nothing to do with each other. **There are no normal maps in
that array at all** — forest's two are plain material slots. So a split array would have
compressed two layers of eleven.*

***D0 does not hold, and the first consumer to arrive is what found it.*** `pbr_frag` *reads*
`irradianceMap` *on exactly one line and that line is the ELSE of* `giEnabled`*, while*
`gi_volume_bind` *returns before binding the atlas when the volume is inactive. The texture D0
wants to fold into is the one not bound in the only case the fold has to serve. Making it hold
means allocating the GI atlas on every scene with an environment. **This roadmap recorded for
many specs that D0 had no scheduled consumer; it turns out also not to work.***

***A third upload path existed and no document knew.*** `async_loader.c` *had its own*
`glTexImage2D` *and* `glGenerateMipmap`*, and it is the path every EXTERNAL model texture
takes — so the CPU mip chain reached two sites of three for one commit. Compression is what
forced the chain onto the CPU in the first place (a compressed chain cannot be filled by*
`glGenerateMipmap`*), and that is the kind of defect only a second consumer reveals.*

***And the SUITE could not see any of it.*** *All 28 goldens are 0 px with every mip level
painted **solid black**, and so is the raiden recipe: they sit near the camera and never
minify. Most of a compressed texture's bytes are in the chain. The item therefore shipped a
fixture that recedes to a vanishing point, which is the only thing in the corpus that samples
a mip at all.*

*Measured on raiden: **81.354 MB → 74.854 MB** with normals and masks, **→ 37.253 MB** with
colour. Cost RMSE 0.16 of a code (ao_fixture), 0.87 (raiden), 1.5 on the fixture; DXT on the
fixture's hue wheel reads 8.4, which is five times what raiden shows and is why the fixture
exists. Colour is **opt-in and off by default** — the one default here chosen by taste, since
BC5 and BC4 are unobservable and DXT is a judgement.*

***Its leftovers closed in spec 11.86, and the largest one had been mis-sized.*** *The deferred
"seven texture entry points" item was booked as a 27-site sweep. The sweep was the smaller half:
the eight-step body from decoded pixels to a pooled Texture was written out **three times**, and
11.85 had shared only the upload. The three had already drifted — `TextureAlpha` reached one of
them, and the async path recovered `is_srgb` by testing an internal format that has no sRGB
variant below three channels, so a greyscale albedo mipped in the wrong space on the streamed
path alone. Seven entry points became three over a `TextureDesc`; two of the seven had no
callers.* ***And the coverage gap this entry never recorded is the one that mattered***:
`clearcoat_fixture` *was in neither harness, which is why the coat-normal defect above reached a
commit. It has a golden and a five-arm group now, each arm falsified by a named mutation.
`apps/tree` also stopped leaving 13 MB on the table (54.667 → 41.667) for 0.081 of a code.*

### F1 (original entry) — Effort M
Every texture in this engine uploads uncompressed, and **no document has ever said so**.
`texture_gl_formats` (`texture.c:185-197`) hands `glTexImage2D` the UNSIZED internal formats
`GL_RED` / `GL_RG` / `GL_RGB` / `GL_RGBA` / `GL_SRGB` / `GL_SRGB_ALPHA` — so the engine does not
currently state what its textures are stored as at all, and the driver picks. There is not one
`GL_COMPRESSED_*` token in cetra's own source; every hit in the tree is vendored glew or assimp.

Measured on `apps/forest`, both logged at allocation and neither compressed: the material texture
array is **11 layers at 2048x1024, 117.3 MB**, and the composite cache's page pair another
**42.7 MB**.

**What this platform can actually take, measured rather than assumed — and it is NOT what "BC7"
would suggest.** Apple's GL 4.1 Metal driver advertises exactly one compression extension,
`GL_EXT_texture_compression_s3tc`. BPTC is core in GL **4.2**, one version above the ceiling, and
is not exposed as an extension either: `glCompressedTexImage2D` with
`GL_COMPRESSED_RGBA_BPTC_UNORM` returns `GL_INVALID_ENUM`. **So BC7 is unreachable here**, and any
plan written around it dies in its first week. What uploads cleanly is RGTC (core since GL 3.0, so
it carries no extension string to grep for — the reason it reads as absent) and the whole S3TC set
*including* the sRGB variants:

| format | vs today | for |
|---|---|---|
| BC5 `GL_COMPRESSED_RG_RGTC2` | 3-4:1 | normals — two independent 8-bit channels, Z reconstructed |
| BC4 `GL_COMPRESSED_RED_RGTC1` | 6-8:1 | single-channel masks (rough / metal / AO / opacity) |
| DXT5 sRGB `GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT` | 4:1 | albedo with alpha |
| DXT1 sRGB `GL_COMPRESSED_SRGB_S3TC_DXT1_EXT` | 6:1 | albedo without |

**The material texture array is where the win is and also where the problem is.** A
`sampler2DArray` has ONE internal format for every layer, and since 11.60 and 11.73 that array's
tenants are masks, layer albedos, packed layer surface maps, splats and decal images — which want
BC4, DXT5-sRGB, BC5, BC4 and DXT5 respectively. **This is the THIRD time the array's
single-policy rule has bitten**: one canonical size (11.60), one mip policy (`ocean.glsl`'s note),
and now one block format. DXT5 on a normal map is precisely the case BC5 exists to avoid, so the
honest answer is likely to SPLIT the array by format rather than pick a compromise — which costs
declarations against Wall 1, and is the thing to price before anything is built.

**Nothing here is a look change and everything here is a look risk.** Block compression is lossy,
so unlike every other item in this document **the 0 px bar is unavailable by construction**. The
acceptance test has to be a stated per-format error budget measured against the uncompressed
frame, with the goldens re-baked deliberately. DXT1/DXT5 quantise to RGB565 endpoints and are
visibly bad on smooth gradients, so a per-texture format CHOICE is part of the item rather than a
global switch.
**Depends on:** nothing. **Wall 1:** unaffected by compression itself; possibly WORSENED if the
array splits by format, which is the first thing to measure.

### F2. An offline asset cook — Effort L — **DONE (spec 11.99), as a transparent cache rather than a build step**

**Shipped as the UE DDC shape, settled with the user before the plan was written**: a
content-addressed derived-data cache (`cook.c/h`, one `.cca` file per artefact) that the heavy
startup derivations are bracketed through — fetch-or-bake-and-store; the scatter deliberately
stays live — plus a `--cook` pre-warm verb that is nothing but a headless run with a report,
exiting when the async loader drains. The key IS the identity
(input bytes + recipe version + library version where a library owns the byte format: Jolt's
`JPH_VERSION_ID`, meshoptimizer's version), so a stale artefact is never detected, it is
**unfindable**; a miss always falls through to the live bake; a corrupt file is refused by name
against a payload hash and layout cross-check and treated as a miss. Ten sites: forest's
vegetation and terrain-layer textures, the splat, the eroded field (keyed on the SEEDED plane's
bytes, so the fbm folds in transitively), the 20 cluster DAGs, the 16 Jolt region shapes
(serialize/restore through a second C++ TU on `cluster_build.cpp`'s precedent), the moon surface,
the cloud noise, and every texture mip chain through the one publish path — whose
derive/upload split is what makes a hit provably the bytes a miss would have uploaded.

**Measured: forest cold 7.5 s debug → warm 2.8 s on the gate fixture; the shipping config's
on-init went 4.9 s → 0.95 s debug with 59 artefacts hitting.** Phase 0 first attributed the
5.3 s of startup no spec had named (vegetation first at 1.8 s — not the scatter or the erosion
11.65 guessed at). Eight gate arms; the ledger's strongest row was caught live rather than
injected: Jolt's restore checks `IsEOF()` after a stream's LAST field, a positional
implementation refused every restore ever attempted, and the region-collider arm is what noticed.
The scatter is deliberately NOT cooked (2.6% of release startup, the widest key surface, and the
region determinism story is about the live scatter); "two builds are not two runs" is crossed
exactly once and on purpose, with the docs row that sanctions it. **Original row:**

There is no cooked asset format and no build step that produces one. Every app parses source
assets through assimp at run time, decodes PNG/JPEG at run time (async, a worker pool sized
cores-2 clamped to [2,8]), and bakes its procedural content at startup.

**Its beneficiaries are already measured; they have just never been read as one item.** 11.65
measured `apps/forest` starting in **6.58 s debug against 1.36 s release** — procedural texture
bakes, 20 cluster-DAG builds, the scatter and erosion, all redone on every launch. 11.64 then
found the largest single cost inside that is **Jolt's per-region BVH build at 85% of collider
time**, and 24x between build types. None of that work depends on anything known only at run time.

**The precedent already exists and is one subsystem wide.** `.cts` (11.69) is a cooked,
offline-produced, streamed format with a manifest and a mip pyramid **the file stores rather than
re-derives** — written because terrain needed it, and never generalised. F2 is that shape applied
to everything else: block-compressed textures with their mip chains (F1), packed vertex buffers,
prebuilt LOD chains and cluster DAGs, and serialized Jolt shapes.

**Decide first what a cook may NOT do**, because this engine's determinism story rests on
run-time derivation: `terrain_field_seed`, the erosion digest and `cetra_bake_bands` are all
bit-identical at any worker count WITHIN one build, and this document's own rule is that two
builds are not two runs. A cooked artefact crosses that line by construction — produced by one
build, consumed by another — so the format needs a version and a content hash that REFUSES a stale
artefact rather than quietly rendering one.
**Depends on:** F1 (soft — the cook's first payload is compressed textures). **Wall 1:**
unaffected.

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
| 22 | D2 Local fog + cloud shadows | M | **DONE, both halves (11.39 + 11.40 froxel, 11.41 surface).** The surface half was booked as hard-blocked on D0 for a whole spec cycle and never was — it reads in the opaque pass, where unit 6 is idle, so it took a `#define` alias and cost the ledger nothing (Wall 1's third escape). Ground RMSE **0.0072 → 0.1241** with the air band unchanged to four decimals as the control. 11.41's arms then found the shadow MAP itself blank below ~10° of sun and truncated above it, so **every cloud figure from 11.39/11.40 was taken through a map crossing 4-48% of the deck**; fixing it moved both cloud goldens. The review's through-line was **green results that could not have gone red** — four arms passed over a feature dead under `--no-ssao`, and 23 goldens were cited as evidence for a change no golden could see. 11.40 closed three of the four filed items by making those claims falsifiable, and found a shipped map saturated to zero at every texel on the first use of its new debug tile. |
| 23 | C2 Emissive → area lights | S/M | **DONE (11.49).** A fit plus a registration, as sketched — but it ships **off by default**, because measuring the corpus found **30 of 32 emissive materials are the unlit-flat-colour trick** rather than lamps, and on-by-default turns every one into a light. "No new shading code" held five phases and then cost one uniform (no unit — Wall 1 never in play). The planned covariance principal axis is wrong on a SQUARE panel, whose covariance is isotropic, and `cornell_light` is exactly square — the corpus's only real lamp is the case it fails on. The stated "one rectangle per mesh" limit had no working test: planarity measures FLATNESS, so an L, a ring and two strips five metres apart all read exactly 1.0 and got one rectangle spanning the lot. Three arms went red on things no frame shows — a DDGI capture counting the first bounce twice (**1.31× → 0.98×**), a panel lighting through a solid partition (**0.41 → 0.00**), and a placement compounding 29 units of drift over 40 frames. Cost lands on the LIGHT, not the machinery: reconcile 0.017–0.021 ms CPU, but +2–3 ms of shading per panel. |
| 24 | E4 GPU timer queries | S | **DONE (11.27).** Wall 3 removed. It paid for itself immediately and twice: E4's own first draft drew a conclusion the instrument reversed, and Wall 4 exists at all because the instrument said "opaque 312 ms" in a single run. |
| 25 | C3 IES profiles | S | **DONE (11.57).** Collected the payoff for 9.9/10.0's photometric work, and the premise held — `Light.intensity` really is canonical candela and every one of those lights really did radiate through a bare smoothstep. Zero texture units, as booked. **But the row deferred asymmetric profiles by pricing a 2D table in SAMPLER units, and its own placement removes that price**: in a UBO block the symmetric and asymmetric cases differ only in stride, so full LM-63 shipped with no v1/v2 split. **Its storage plan was also broken as written** — "alongside the packed lights" lands after the live prefix `ubo_upload` rewrites, so the table would have been undefined every frame; it took its own block at binding 6, uploaded once. The profile is a normalised shape multiplying `intensity`, with `intensity` seeded from the file's peak when unauthored, so it is absolute by default while `_scale_emitters` stays true. It REPLACES the cone, which moved the spot shadow frustum onto the profile's support — and the cull radius needed nothing, since normalising makes `profile ≤ 1` and the existing solve already bounds it. Applied at all FIVE places the falloff is evaluated, through one shared decision, which also folded in `pbr_frag`'s hand-duplicated debug copy that no golden can see -- though it shipped reaching only FOUR of them and claiming five in a comment, which its review round caught and split into `punctualAngularOf` (values) and `punctualAngular` (the cluster-list adapter) so the fog's standalone spot shaft could stop restating the rule. **Its review round is the entry worth reading**, below. Followed by `--ies-profile`, added from use: a `.cscn` was the only way to attach a profile, which made the feature unreachable without editing a committed asset. |
| 26 | C5 Screen-space shadows for local lights | S | **DONE (11.56).** Postfx-only as booked, and the cluster list arrived with no C-side binding at all — `create_post_program` already links through `ubo_wire_blocks`. But **the row's reason was false and had been for six specs**: it blamed a punctual map's "texel footprint" for losing the contact, where that map is ~1 mm/texel at 1 m for a point light (2048² at 2–6 layers) and ~2 mm at its 1024² floor, and the hairline that did exist was a far-side depth STORAGE defect fixed in 10.3/10.4 with `cornell_box` as its gate. The real gap is that **~120 of 128 clusterable lights can never have a map** (8 punctual layers, 6 per point light), which flips the cull from "the N nearest" to "skip the ones that already have a map" — one line that is the performance cap and, once the fold's denominator counts them, the anti-double-shadow guarantee too. The gate widened as well: a room lit only by practicals never ran the pass. Cost tracks COVERAGE, not count — 2.43 ms per fully-covering light at 3200×2000 internal, but sixteen SPREAD lights cost +0.77 ms against the +38 ms sixteen coincident ones would. No per-pixel cap, deliberately. **Area panels are still not served** — the 41% wall leak below is NOT what this fixed; a panel counts in the fold's denominator but is never marched, because the only direction available is its centre and marching that would be a different approximation from the LTC integral the shading used. **Its own review then found three defects an eight-agent pass caught and the spec had not**: the fold's denominator omitting every skipped light (one blocked practical took 23% off a pixel that should have lost 1%), `shadow_layer` read raw when it goes stale the moment the shadow system is toggled off, and a per-pixel cluster walk nothing gated. Plus a fixture assert that could not fail. |
| 27 | E3 Histogram exposure | M | **DONE (11.52).** A 128-bin gather histogram plus a percentile-clipped reduce, two raster passes replacing the mip chain; metering modes, EV bounds, split adapt rates, and the controls. **Both reasons this row gave were wrong**: the bright-pixel failure was already defended by a geometric mean AND an explicit clamp, and the determinism claim was already collected by `EXPOSURE_ADAPT_SNAP`, after which the adaptation holds no history and two runs are bit-identical over 200 frames. Capability parity was the whole reason and it sufficed. The instrument found what the row had not: the meter was **not linear in radiance**, 8.36 stops against 9.97 at x1000, because the absolute floor at the key inflated a dim scene's mean 3.05x. Percentiles fix it, **-1.61 stops to +0.021**. Removing the ceiling too produced a measured **runaway** to 1.15e7 nits via fp16 underflow -- though review then showed the GAIN was already bounded by the 20-stop floor, so the metered bound recorded a saner number and changed no pixel. And the SCALE_GATES shape cannot test a live meter -- scaling emitters by K while dividing the camera by K double-compensates. **The review then changed the implementation twice more**: the bin pass turned out to be OCCUPANCY-bound rather than fetch-bound (64 fragments is ~0.4% of the GPU; splitting the source across 8 output rows took the scope 0.744 -> 0.392 ms), and the bin ceiling could not be a constant at all, since the measure pass clamps in working space and divides by pre-exposure so its largest emittable value moves with the exposure. No golden moves (all 24 pin exposure), which is why this was the least-tested subsystem shipping on by default; six new arms now cover it. |
| 28 | E2 3D LUT grading | S | **DONE (11.58).** Colourist workflow, as booked, and the working-space warning was the right thing to warn about -- the contract is that a `.cube` is applied AFTER `displayEncode`, because that is the space one is authored in. **But the row's own number was wrong for the format it names**: 32³ is not a size `.cube` produces (33³ is Resolve's default), so the size comes from the file and the fixtures carry three of them. Log/pre-tonemap LUTs were declined -- not as too hard, since `agxTonemap` already log-encodes and linearizes back so one would be a fourth branch of `toneSelect`, but because no off-the-shelf table can be authored against a log window we invent, and it would replace the tonemap rather than compose with it. **Tetrahedral is the default, and the first figure published for it was wrong by 28x**: 0.076 of an 8-bit code was measured on a table whose every output channel is a ridge function of TWO inputs, so trilinear degenerates to bilinear on it and the three-way cross term tetrahedral drops is identically zero. Re-measured on a table with a real three-way term it reads **2.7 codes** — subtle but over this repo's own visibility floor. What buys it is the neutral axis (trilinear tints greys 5 codes through a table that touches them not at all) and an exact identity (0 px, against trilinear's 12,088 at PAE 1/255 -- the texture unit's fixed-point filter weights). **The 32F rejection was also measured on a construction that could not show the effect** — an identity table at 17³, whose every lattice value is k/16 and exactly representable in fp16. Re-measured where fp16 rounding is live it costs 0.004 of a code; 16F stands, on evidence rather than on a coincidence. Trilinear stays reachable because tetrahedral alone is unfalsifiable. **Its gate's first draft claimed coverage it did not have** -- the identity arm said it caught the half-texel inset and was green with the inset deleted, since `lutCoord` is trilinear-only and tetrahedral fetches by integer index. |
| 29 | C4 Clustered specular probes | L | **DONE (11.70)** N probes blended per fragment through A1's grid, stored as octahedral roughness rows tenanting A4's atlas texture. Zero new sampler declarations. |
| 30 | E5 Instancing + LOD + sorting | L | **DONE, two limbs of three (11.28 / 11.29).** Wall 2 mostly removed: `abandoned_window_shadowed` shadow CPU −83%, frame −38%, 2,148 draws → 272. Sorting deferred as unfalsifiable against the corpus, which `apps/forest` has since falsified — moved to E6. Established that scatter *order* decides whether batching happens at all (2,368 → 1,287 draws for identical geometry), that LOD fights instancing on the `(mesh, lod)` key non-monotonically, and that "meshoptimizer locks mesh borders" — in three headers and spec 11.28 — was wrong from the start. |
| 31 | **E6 Depth prepass + opaque ordering** | M | **DONE (11.30 + 11.31).** `apps/forest` opaque **306 → 169 ms (−45%)** from the ORDERING alone, depth complexity 1.93 → 1.08. Masked geometry now prepasses too (11.31, via a `depthOnly` mode in `pbr_frag`) and reaches a better 0.72 — and is still **slower** than the sort, because a full extra geometry pass costs more than the shading it saves. The two are substitutes, not complements: 11.30's "worth more together" was an artefact of the masked exclusion. Ordering ships on, the prepass off, with a gate arm asserting the prepass **costs** on a scene with no overdraw. 11.30's own figures were doubled by a budget that trusted `msaa_samples` over the driver, and its −64% interior does not reproduce. Between them these two specs withdrew seven claims — every one from an instrument that had never been checked against a scene with a known answer. |
| 32 | D0 Free two sampler units | M | **REFUSED on investigation (11.85).** See the D0 entry: the first consumer to actually want the unit found the fold unsound — `pbr_frag` reads `irradianceMap` only in the ELSE of `giEnabled`, and `gi_volume_bind` leaves the atlas unbound exactly when the volume is inactive, so the texture the fold targets is missing in precisely the case the fold has to serve, and making it hold couples IBL to a subsystem that is off underneath it. That falsifies this row's own "the irradiance fold holds" below, which stood here unstamped for fourteen specs after the entry it cites recorded the refusal. What actually returned units is 11.95's declaration gating — a lean variant spends 12 of 16, four back per variant, with no fold at all. **Original row:** Foundation only — ~~schedule it **with** D1 or D2's surface half, never before, per the just-in-time rule~~ **both have shipped and neither wanted it**, so the just-in-time rule now points at the froxel-in-transparent item or at nothing. **Explored after 11.40 and it frees ONE unit, not two** (16/16 → 15/16): the irradiance fold holds, the unit-6 share is a conditional slot rather than a freed one, and the second real candidate (unit 4, POM height into the mask array) costs POM half its resolution on `pilot` to buy a slot nothing currently spends. See D0 for the five corrections. **And the ledger sweep left it with no SCHEDULED consumer at all**: D2's surface half never needed it — **shipped without it in 11.41, so that is now demonstrated rather than predicted** — and ~~D1 can dodge it with a flat 2D decal atlas rather than a `sampler2DArray`~~ **D1 dodged it too (11.73), by neither route this row imagined: a decal image is a TENANT of unit 2's array, so it cost a layer index and no declaration**. Both predicted spenders are now built and neither spent it. The one consumer that genuinely cannot dodge it is sampling the froxel volume from the transparent pass — a `sampler3D` in the one pass where refraction is live — and that is not booked. Judge the item on its own measurement, not on what it unblocks. |
| 33 | D1 Clustered decals | L | **SHIPPED (spec 11.73).** ~~Hard-blocked on D0.~~ ~~Blocked by a texture-layout choice: unit 6 is idle in the opaque pass, so a flat 2D atlas takes the alias.~~ **Both wrong.** Unit 6 is NOT idle there — 11.41's cloud shadow holds it for the whole pass — and no alias was needed: a decal image is a tenant of `materialArray`, which costs a layer index and no declaration, and reaches the late pass and probe captures that an opaque-pass alias would have foreclosed. Cap 16, sized by the froxel mask; binding 10, the tenth uniform block of twelve. The ground slice 11.68 shipped stands: a mark MADE of a layer the surface carries is still a splat-weight override with no image at all. |
| 34 | E8 Fix the wind cull | S | **DONE (11.53), and this row described half of it.** The condition is `is_skinned || wind_response > 0`, so **every skinned mesh** was exempt from the camera frustum and every cascade too — and the prescribed AABB expansion does not address a posed mesh at all, since a bind-pose bound is a bound on a different shape. Both bounds turned out to be closed-form: `windOffset` is sin/cos-bounded with nothing scaling by vertex position, and skin weights are convex so a posed vertex lies in the hull of its bones acting alone. `DRAW_UNBOUNDED` is deleted — it was decided in `classify`, which sees neither the wind field nor the pose — and the decision moved into a `CullView` beside the frustum, built per pass. Aimed away, `ivy_arcade` goes 49 of 73 culled to **73 of 73, 0 draws**, and raiden **18 of 18**. **The payoff needed a second half the row did not book**: wind is object-space, so making trees cullable does not stop 2,000 copies swaying in lockstep — `phaseVariation` fixes that with no InstanceBlock field, hashed from the object's world origin. `apps/forest` now has its wind, and it costs **5 meshes of cull out of 28,256**, against a counterfactual of at most 3,064 opaque culled where it measures 4,085. Also closed a latent defect nothing had reported: `_count_late_meshes` tested the raw bind AABB while the lane that draws those meshes exempted them, so a swaying or posed transmissive mesh could be drawn without the refraction resolve it samples. **The review then found the fixture testing one multiply** — authored at turbulence 0 and mode 0, so `wind_max_offset` collapsed to `strength × response` and none of the six shared coefficients, neither per-mesh maximum, nor any of the vegetation arithmetic reached the value under test, which is the branch `apps/forest` ships on. It also found the shared-constants file written as if the technique were novel: **`shore_constants.glsl` is the precedent**, and the `f` suffix its header calls load-bearing was missing, so the CPU bound evaluated in double against a float shader. |
| 34b | **E9 One sample means one sample** | M | **DONE (11.34).** `apps/forest` opaque **150.9 → 121.6 ms (−19.4%)** against a 0.23% floor, with byte-identical submission integers — the same work, cheaper. One branch in the one allocator plus one at the depth renderbuffer flips the scene, OIT and moment FBOs in lockstep, since they share the depth attachment. The row's original prescription was wrong twice: there is no `sampler2DMS` anywhere in the corpus (11.17 rejected it), and postfx reaches the scene target only through blits, so the GLSL surface was zero files and postfx changed nothing. Priced before built with a new `--msaa <n>` lever, which also decomposed the first confounded A/B: A2C alone costs 202 ms of forest's opaque row (fragment-set explosion, headless-only), a sample ~93 ms on that inflated set. TAA-only edges verified by crops (raiden groom, forest canopy — indistinguishable), all 23 goldens 0 px, and MBOIT's moment-resolve bias (11.17) is now absent on the TAA path for free. |
| 35 | ~~D3 Tessellated water~~ | — | **SHIPPED (11.32, 11.33, 11.35) and it spent no tessellation.** The mesh went through two screen-space schemes instead: clipmap rings (11.33), then a **projected grid** (11.35) after the rings turned out to weld reach to near-field detail — the snap that makes them tile is the same thing that kept the surface 5° short of the horizon while a comment claimed otherwise. The stage this item was scheduled to open is still closed. Reaching the horizon then moved the problem from the MESH to filtering: distant cells cover more than a wave period, so each wave model drops what sits under its footprint and hands the slope energy to roughness — a BRDF answer to a geometry question. See D3. **Six more specs have landed on the surface since and none of them were rows here** (11.43 the fixture's sun, 11.44 world scale, 11.45 the swash film, 11.46 → row 35b, 11.47 whitecaps, 11.48 two wave trains); D3 carries them. |
| 35b | **D5 By-example texturing** | S/M | **DONE (11.46), and it was never booked.** Would have been assumed blocked — a transformed copy of a texture plus an inverse table, in the most saturated program in the tree — and cost **zero** units: the shader never reads the original so the transform is baked over it, and the table is 768 bytes of uniform space. The ledger's fifth escape. Contrast held to 0.15% (0.02876 → 0.02881), which is the measurement that matters, because a broken blend flattens variance rather than shifting colour. **Two of the three defects it fixed were not rendering bugs at all** — a noise field sampled on an unbounded lattice, so forty tile boundaries were discontinuities in the data; and a circular tangent frame under planar UVs, creasing along every triangle edge. Both were reported as water bugs for most of a session. |
| 35c | **Unbooked, shipped anyway** | — | **11.50 and 11.51 appear nowhere else in this document.** 11.50 made `foliage_shadows` a material row, so alpha-masked foliage arriving through a FILE can shadow — it had worked only for meshes built in C. 11.51 is the ivy arcade: the first asset authoring UV1 wind data, and a new `wind-uv` gate group. Both are content-driven work with no roadmap row, which is the same pattern D5 records — the table is a backlog, and the work keeps arriving from outside it. |
| 36 | D4 Terrain **streaming** | XL | **SHIPPED — 11.63 took the LOD half, 11.69 the paging half, and the row is closed.** A CDLOD quadtree replaced forest's fixed grid (Strugar over Hoppe, the choice D4 said it had not made), then an fp32 tiled `.cts` file put the pyramid on disk with a rectangular WINDOW resident per level. Windows rather than 11.67's page table, decided by access pattern and not by scale: terrain reads contiguous squares ~30k times per patch build, so a containment test beats an indirection per footprint corner — and residency becomes a pure function of the anchor path, which deletes the eviction ordering and hysteresis rather than re-implementing them. The organising rule is the transferable part: **anything building a PERSISTENT artifact ensures exact data synchronously, anything transient takes the coarse fall**, which closes cache staleness structurally instead of with an invalidation channel. **Re-scoped once and the re-scoping held**: the entry demanded streamed height DATA while depending on an ANALYTIC surface for its stitch; erosion (39/40) made terrain data and 11.63 made the domain big enough to care, exactly the order predicted. **Its own scheduling figure was wrong the whole time** — 67 MB / 268 MB is the height plane alone, a real field with its three masks is 291 MB / 1.16 GB, and 4096² builds no coarse levels at all (node-centred grids halve only while res−1 is even, so it is 4097² or 8193²). |
| 37 | E7 Occlusion culling | L | **DONE (11.98), and every clause of this row's refusal was answered rather than argued with.** "Not because a measurement demands it" — the measurement now exists: opaque **7.643 → 8.708 ms (+14% for turning it OFF)**, separated over four interleaved pairs, on a portal fixture with ~1.38M triangles behind a wall, for exactly 0 px. The row reasoned from forest, the least occludable content in the tree; the fixture the feature was priced with had to be BUILT, because nothing in the corpus stood behind anything. "A bigger version of the prepass trade" — measured too, as integers on a scatter twin built to lose: culling a third of the triangles splits one instanced run, draws 5 → 7, and the unasserted clock still netted ahead. What shipped is CPU masked software occlusion (current-frame, no query, no readback, no popping — the "latency story" E7's section called the whole item does not arise), author-opt-in through two channels, conservative by four stated roundings, with a brute-force probe twin over the recorded box set and a falsification ledger whose two unfalsifiable arms are structural findings. forest byte-identical; the off state is two integer compares. `overdraw_layers` was the wrong instrument and was not used — it prices fragment overdraw, and this feature's win is submission and vertex cost. |
| 38 | E10 Integer-bit hashes | ~~S~~ M/L | **REJECTED on investigation — the diagnosis is right and the item is still not worth building.** The arithmetic is worse than this row claimed: the hash multiplies `sin` by 43758.5, so it amplifies argument error by the same factor, and pinning the output to ONE 8-bit code needs the argument accurate to 9e-8 — at the real call-site magnitudes (dot products of 912 to 372,401) that is **33 to 42 bits of precision demanded of a 24-bit type**. So it is not "precision-sensitive"; the output is decided by rounding and the function cannot be implemented as written. Cost, in a float32 model: output entropy ~13 bits rather than 24, and on a 64x64 integer lattice **1047 distinct values of 4096 at the origin** (worst duplicate 20), 472 at world offset 1e6 and **53 at 1e7**, against 4096/1 for PCG at every offset. **None of that pays here.** The benefit is cross-driver reproducibility and there is one driver; cross-machine goldens would not follow from it anyway, since `pow`, `exp2` and FMA contraction differ too and 11.28 moved four goldens 26 px on an include reshuffle alone. And **no consumer has a correct pattern to regress against** — grain, wind phase, foam bubbles, curl noise and stochastic tile offsets are all arbitrary by design, which is why the coverage is what it is: **zero of 27 goldens depend on any of the four hashes**. Water is not the exception it looks — both water goldens are Gerstner with no bed, so `foam` is identically 0 and `water_frag.glsl:1120` never opens; `spores` and `stochastic` appear zero times in `gates.py`. **Three things this row said are wrong.** `ign` is NOT one of the sin-fract sites (`noise.glsl:20` is `fract(52.98 * fract(dot(p,k)))`, no transcendental) and must stay out. The **31,800 px** was `ssr_frag.glsl:211` — the *identical* function re-associated from a multiply-add into the include's `dot()`, not a different hash — under a configuration recorded nowhere but as "a no-TAA render", so it prices neither this change nor anything reproducible. And it is not an `S`: the swap is, but four of five consumers are invisible to the whole suite, so the instrumentation is most of the work and all of the value. **What would revive it:** a second GPU actually being rendered against, or a demonstrated artifact — the two candidates are `stochastic.glsl:46`'s "patch of ordinary tiling" far from the origin and a forest where trees share a sway phase, neither of which anyone has reported. A `--hash-probe` driving the real hashes through transform feedback (the `wind_probe_vert.glsl` idiom) is the instrument if either ever shows up. |
| 39 | **D6 Heightfield backend** | S | **DONE (11.59).** The unlock, and it exists only to serve 40. `terrain_height_at` gains a SOURCE — analytic fbm or a filtered sample of a stored grid — while staying a pure function of `(state, x, z)`, so all **eight** consumers keep working unchanged and a NULL field is today's path byte-identical. Three contracts, not details: the filter is **C1** (bilinear's piecewise-constant derivative would facet normals at cell boundaries and reach the scatter's slope gate), out of domain **clamps to edge** (`forest.c:939` queries at a camera eye that can leave the extent), and thread-safety is **not** improved — the analytic path memoizes into file statics and stays unsafe. |
| 40 | **D7 Erosion bake** | M | **DONE (11.59).** The largest visual gap against a shipped AAA terrain, and the reason 39 exists. **Erosion is a simulation over a grid, not a function of position** — the height at a point depends on the whole upstream watershed, so there is no `f(x,z)` to write and terrain must become DATA. That is why UE/Unity/Frostbite/Decima all consume a baked heightfield. **The silhouette is the smaller half**: the sim knows where water flowed, so its flow/deposit/wear masks put gravel in stream beds and bare rock on ridges — where `terrain_tint` today guesses from slope+altitude+noise, which is exactly why it reads as procedural. CPU, Eulerian, double-buffered, threaded like the cloud bake, whose zero-sync disjoint-slab shape gives **bytes identical at any thread count**. Droplet/Lagrangian erosion is refused on that same test. **Two corrections from building it.** Mei's own semi-Lagrangian transport LEAKS -- 3.06% of the sediment budget, since a bilinear gather conserves mass only for a divergence-free field -- so the load rides the fluxes instead and closure is 5e-09. And rain over evaporation IS the equilibrium depth: the first defaults flooded the whole terrain and the mask came out uniform. 452 ms at 512² x 220 on eight threads, release. |
| 41 | **D8 Heightmap import/export** | S | **DONE (11.59).** Unifies the two producers by making them meet at one format: **the bake writes what the importer reads**, so dev-time bakes, ship-time loads, and a Gaea export drops into the same slot. `.r16` (headerless 16-bit, the literal UE/World Machine/Gaea interchange) plus 16-bit PNG on the read side via **`stbi_load_16`, vendored and called nowhere in this repo**. Deliberately not routed through `texture.c`, whose `texture_gl_formats` hard-wires *unsized* internal formats and has no path that can request 16-bit at all. **The masks are the half that is easy to drop and 11.59 dropped them first**: the save wrote height only, so a shipping load got eroded geometry shaded by the guess erosion exists to replace -- 40's opening failure, reached through 41's own round trip, with every arm green. A round-trip arm comparing only the geometry is not one. |
| 42 | **D9 Terrain material layers** | M | **DONE (11.60).** By 11.45's rule (`ocean.glsl:63-78`) N layers are ONE shape — and the row was still one step short: it wanted "a program with room", when the declaration was already there. `materialArray` is bound on every draw and `material_texture_layer_for` knows nothing about masks, so layers became further tenants of it for **zero new declarations, zero new units and zero new programs**. The `pbr_skinned` precedent it cited is a second VERTEX shader over the same fragment shader; the only real second surface program, `water_frag`, has no clustered lights, punctual shadows, LTC or GI, which is right for an ocean and ruinous for terrain. **The test is whether the LIGHTING MODEL differs, not the texture count.** Shipped world-aligned (triplanar) with a height-weighted blend and a splat whose coordinate SPACE the material states — because world XZ cannot address a vertical surface and a mesh-local reading makes the weights swim on a moving prop. It also shipped inert in `apps/forest` and was caught in review: terrain writes UV1 as a literal zero, so the ground sampled one splat texel, through a green suite. |
| 43 | ~~D10 Virtual-texture compositing~~ | — | **Stages 1 AND 2 SHIPPED (11.66, 11.67).** Stage 1: the composite atlas plus a runtime detail term — `3 + 2A` taps against the per-texel 9/17/25, independent of layer count, byte-exact on flat content, zero new sampler declarations. Stage 2: a 64-slot guttered page atlas at 4x the fallback's density (a ratio, bounding the virtual grid at 34² forever), the page table in a UBO on binding 7, the page pair on units 3/4 freed by refusal, frustum-predicted residency sorted (seen, distance, id), and a depth-tested GPU vote pass read back through a fixed-latency PBO ring — deterministic by construction, +0.23 ms GPU, 0 px on today's content by design. What remains is CONTENT, and the general-mesh era where feedback becomes the only correct source. **First content SHIPPED (11.68): roads — and NOT drawn into the atlas FBO**, which is how this row described the plan. A road is a procedural splat-weight override inside `sampleLayeredSurface`, before the height blend, so the bake inherits it by calling the same function unchanged and the fallback, the pages, the per-texel path, the dominant index and the detail term all follow with no bake code. Segments in a UBO on binding 8, zero samplers. **Pages remain a 0 px identity on shipping configuration** — at the derived resolution all three legs agree to five decimal places; roads are the first content a forced-coarse fallback can distinguish, reading 48 through pages against 66 through the fallback just inside a road's edge. Their caps are small on purpose: 4 roads of 16 points on ONE material per scene. |
| 44 | ~~D11 Large-world origin rebasing~~ | M/L | **SHIPPED (spec 11.62)** as origin SHIFTING rather than fp64, and the hardcoded far-cascade centre below is fixed with it. This row read as unbuilt for seven specs while its own entry said DONE -- found by the sweep after 11.69, and the reason to state it here is that the ledger is what gets read when someone asks what is left. Independent of 39-43 and needed by anything wanting a world past a few kilometres: fp32 world coordinates cannot hold still, which is why UE4 capped at ~20 km and UE5 shipped fp64 Large World Coordinates. **It also fixes a defect that is already live and has nothing to do with world size** — the outermost shadow cascade is fitted around a hardcoded `{0,0,0}` (`shadow.c:1250`) while the inner ones follow the camera, so terrain placed away from the origin loses its far shadows with no diagnostic. `terrain.h` already warns about this and works around it by centring the terrain. |
| 45 | **B10 Night sky — stars** | S/M | **DONE (11.79).** See the B10 entry for the as-built record; the sketch below stands as written except the star model itself, rebuilt three times against photographed defects. Original row: The below-horizon sky is already correct (4.7's "no NaN speckle" gate) and already reachable (`--sun-elevation -10`, which `cornell_rooms` ships); what it renders is black, because Hillaire models scattered sunlight only. A procedural star field behind `starRadiance(vec3)` in `sky_radiance.glsl`, beside the sun disc whose pattern it repeats — horizon extinction, the ground cut and cloud occlusion all fall out of the existing terms. Baked catalog texture rejected on arithmetic (always magnified: 0.176°/texel vs 0.03°/px); sprites are the later body swap if real constellations ever matter. OFF by default in the library — three existing gate arms assume a quiet sub-horizon backdrop and are protected by nothing else. The fade in is a CPU elevation ramp, because auto-exposure is darkening-only by design and cannot brighten a night on its own. |
| 46 | **B11 Day/night cycle** | M | **DONE (11.81).** See the B11 entry for the as-built record — the cost is inverted from this row's sketch, the tick lives in the engine loop, and the peak slice is irreducible at face granularity. Original row: Today an animated sun is a slideshow: one sun move re-bakes view LUT + env cube + GGX prefilter at a measured 0.11 s. The item is a time-sliced env bake (face per frame, mip per frame, swap), chosen because the constraint is that STATIC must not get worse — cheapening the bake degrades every golden, threshold stepping pops on the sea. A stationary sun quiesces byte-identical; goldens 0 px is the acceptance bar. Driver in the app, amortisation in `sky.c`. Forces B12 (past the key-light fade the world is black — sequence the floor first), and probe sets hold capture-time lighting by design. |
| 47 | **B12 Night-sky floor** | S/M | **DONE (11.80).** See the B12 entry for the as-built record. Original row: Airglow + zodiacal + integrated starlight, ~2e-4 cd/m² — the radiance Hillaire cannot produce because it models scattered sunlight only, and the reason the 11.79 night is stars over a black world. Unlike the stars it MUST reach `sky_env_frag`: the point is lighting the ground through the env cube and IBL, and a near-constant floor is safe where the prefilter's firefly rule bans point sources. Ramped by the stars' civil-twilight window. The exposure honesty a second time: auto-exposure only darkens, so the authored radiance IS the night brightness. B11 depends on it — sequence the floor before the cycle. |
| 48 | **B13 The moon** | L | **DONE (11.82).** See the B13 entry for the as-built record — phase is derived rather than authored, a full moon is flat rather than Lambertian, the disc size is shared with the sun, and the surface (relief AND albedo, ~43,000 overprinting craters) is baked at startup because a per-pixel lattice caps the population rather than the cost. Carries the spec's largest finding: it passed every arm while not looking like the Moon, and the look-calibration phase that was meant to catch that had been scoped as a two-constant tuning step. Original row: The dominant night light (~0.25 lux full, ~250× the floor) and how game nights become readable. A LIGHT, not a backdrop: disc + phase + earthshine, plus a real casting directional following phase and altitude. The sun's split applies verbatim — analytic disc in the background only (the env-bake firefly rule), energy as the analytic light, `sky_apply_sun_to_light` as the template, 3 casting-directional slots already exist. New: phase, a second transmittance-tinted directional, and the authoring surface (the 11.79 plumbing shape again). Arguably ahead of B11 on look value: a moonlit static night beats a cycled black one. |
| 49 | **B14 Purkinje shift** | S/M | **DONE (11.83).** See the B14 entry for the as-built record — it is NOT in the finishing stack (that is all downstream of the tonemap; it sites by the optical chain, after the lens and before the response curve), and the metered-luminance drive this row proposed does not exist under `--no-auto-exposure`, which is every golden. The weight is two gates MULTIPLIED, because at 4.2 stops of day-to-night range a daylight shadow and a night frame overlap and no per-pixel threshold separates them. Original row: Rod vision: desaturate + blue-shift in dim light (Kirk & O'Brien 2011) — what makes a dark frame read as night rather than as an underexposed day. Lives in `tonemap_frag`, driven by the metered luminance the CPU already reads back. A hypothesis row for two reasons: its position in a finishing stack with two standing order contracts, and a blast radius of every dim frame in every app — needs an opt-in, a gate arm, and defaults chosen against real night frames, which do not exist until B12/B13 land. Judge it last. |
| 50 | **B15 Water at night** — **DONE (11.84).** See the B15 entry for the as-built record. **Original row:** | M | The moon's best image, and the renderer cannot produce it: `tree` at sun −12 is a black tree against a full star field over a **flat daytime turquoise sea**. Two independent defects. The in-scatter is an authored ABSOLUTE constant (`water.c:1851` into `water_frag.glsl:1056`) multiplied by nothing that knows how lit the water is, so it behaves like an emissive and never dims — a stated design that was only ever exercised in daylight. And water sees exactly ONE directional and picks the sky's SUN by name (`water.c:1876`, spec 11.41's fix for a different bug), so at night it holds a below-horizon light of zero radiance and never reaches the moon — killing the glitter, the Cox-Munk lobe and the caustics together. Fix the first and the frame stops being wrong; fix the second and it becomes the shot. M rather than S because 2 goldens and 35 arms across the `water` and `beach` groups are calibrated against the current in-scatter. **Arguably before B14**, whose own row says to judge it against real night frames. |
| 51 | **F1 Block-compressed textures** | M | **DONE (11.85).** See the F1 entry for the as-built record -- the array turned out to be the wrong prize (its packed maps are three independent quantities and no format here compresses one), D0 does not hold (the atlas it folds into is unbound in exactly the no-GI case the fold serves), a THIRD upload path existed in the async loader, and the whole golden corpus is 0 px with every mip painted black. Raiden 81.354 MB -> 74.854 -> 37.253 with colour. **Original row:** Every texture uploads UNCOMPRESSED and no document had ever said so — `texture_gl_formats` passes unsized internal formats, so the engine does not state its own storage, and there is not one `GL_COMPRESSED_*` in cetra's own source. Forest's material array alone is 117.3 MB and its page pair 42.7 MB. **BC7 is unreachable on this platform** — BPTC is core in GL 4.2, is not exposed as an extension, and `GL_COMPRESSED_RGBA_BPTC_UNORM` returns `GL_INVALID_ENUM`; RGTC (BC4/BC5) and the full sRGB S3TC set upload cleanly, verified by probe. The catch is that a `sampler2DArray` has one internal format for every layer, and that array's five tenant kinds want four different block formats — the third time its single-policy rule has bitten. **The only item in this document whose 0 px bar is unavailable by construction**, since compression is lossy: it needs a per-format error budget instead. |
| 52 | **F2 An offline asset cook** | L | **DONE (11.99).** See the F2 entry for the as-built record — shipped as a transparent content-addressed cache plus a `--cook` pre-warm verb rather than a build step, because the dev loop is the only consumer Cetra has and that is the loop UE serves with the DDC, not with the cook. Forest warm 2.8 s against 7.5 cold in debug; the "hash that refuses a stale one" landed stronger than booked: the key is the identity, so staleness is unfindable rather than refused. **Original row:** No cooked format and no build step that makes one: assimp at run time, PNG decode at run time, procedural bakes at startup. Its beneficiaries are already measured and were never read as one item — forest starts in 6.58 s debug / 1.36 s release (11.65), of which Jolt's per-region BVH build is 85% of collider time (11.64). `.cts` (11.69) is the precedent and is one subsystem wide: a cooked, streamed format whose file STORES the pyramid rather than re-deriving it. The hard part is not the format, it is deciding what a cook may not do — this document's determinism rule is that two builds are not two runs, and a cooked artefact crosses that line by construction, so it needs a hash that refuses a stale one. |
| 53 | **G1 Anti-aliased alpha test** | S/M | **DONE (11.87 + 11.88).** Booked nowhere for 53 rows, which is itself the finding — E6 mentions the prepass/A2C interaction in passing and nothing owned the alpha test itself. Two halves. The SHADER half (11.87): the authored `alphaCutoff` was discarded whenever MSAA was on and a fixed 0.02 compared instead, so a masked material had one silhouette at 1 sample, a fatter one at 4, and a third in its own shadow map, which tests the authored value unconditionally. Golus's sharpening — distance-to-threshold divided by `fwidth`, so the transition is one pixel wide whatever the texture's falloff is — in one shared include reached by the shading pass, the prepass and the shadow pass. The MIP half (11.87, fixed in 11.88): coverage preservation, and 11.88 is the more instructive of the two because 11.87 wrote it **from memory of the technique rather than from the sources** and got three things wrong at once. Coverage is measured over the BILINEAR RECONSTRUCTION, not by counting texels — the count is the form every write-up states and neither NVTT nor DirectXTex implements, and it makes coverage a step function the search cannot land on. The applied scale is the BEST-ERROR one seeded at 1, not the bisection's last midpoint, which is never evaluated. And nobody cascades an 8-bit chain: NVTT cascades in float32, DirectXTex derives every level from the pristine source. Cetra did all three wrong and painted distant cutouts SOLID, past 29 goldens and its own gate — which bounded the mid/near ratio only BELOW, so saturation made it pass harder. **The reference implementations now live in `docs/papers/`**, with what cetra takes from each, because the answers were in shipped code and two of those implementations disagree. |
| 54 | **G2 Hashed alpha testing / alpha distribution** | M | **DONE, the distribution half (11.100), and the row needed four corrections on the way in.** Yuksel's A2C extension is **§4, not §3**, and is not render-time-free — §4.1 wants a second nearest-filtered sampler, §4.2 wants `gl_SampleMask` work, and his measured A2C result only MATCHES hashed alpha testing's ("no apparent qualitative improvement in alpha-to-coverage" is a comparison against hashed, not a verdict that §4 buys nothing) — so the refusal rests on the ledger and the shader work, and the win lives on the BINARY paths this row never mentioned: shadows, captures and one-sample renders, which the engine has and got for free. The recipe was `/2`, not `/1`. As built: Floyd-Steinberg error diffusion (serpentine; the pyramid refused for its PRNG against the cook's charter), surgically gated on the rescale's own best-error miss at 0.03 — frozen against a corpus table where the largest miss that must not fire is 0.0021 and the smallest that must is 0.0502, the dots' level 3, STRUCTURED but unreachable, so unreachability and not uniformity is the criterion. The deep arm found the instrument wrong before the feature: under grazing anisotropy the sharpened test DELETES sparse dither (the counting measure read the fix as worse than the bug), so `alphacov-deep` reads a camera-facing quad at mip 4.9 by mean luma — +20.97 distributing, +3.21 vanishing, +180 saturated — and the grazing loss is recorded as the technique's own ceiling. Forest's far canopy moves ~15k px against 0/40-px floors; tree `--player` and raiden move ZERO; all 29 goldens 0 px, `translucent_shadow`'s LOD-0 prediction included. **Hashed alpha testing remains the unbuilt half**, still the right call for TAA-era soft edges and still refused on shader cost and Yuksel's noise verdict. **Original row:** The named successors to 53, and the reason to state a ceiling rather than call alpha testing solved: Yuksel measures the whole scale-the-alpha family as one that "does not always improve the results" (`docs/papers/yuksel-2018-alpha-distribution-for-alpha-testing.md`, §2), and 11.88's own fixture shows where — past the level a chain goes uniform, coverage is 0 or 1 and no scale reproduces a fraction, so the cutout correctly vanishes rather than thinning. Two candidates, both on hand. **Hashed alpha testing** (Wyman & McGuire, `docs/papers/wyman-mcguire-2017-improved-alpha-testing-hashed-sampling.md`) replaces the fixed threshold with a stable per-fragment hash; spec 11.31 deferred to it and 11.87 chose A2C instead. It costs shader complexity in `pbr_frag` — at 16/16 samplers and ten of twelve UBO blocks — and Yuksel measures it as noisy. **Alpha distribution** (error diffusion or the alpha pyramid) is a pure PRE-PROCESS with no render-time change at all, which is the better fit here: it lands entirely in `texture.c` beside the code 11.88 just rewrote, needs no sampler, no uniform and no shader edit, and the paper's own §3 extends it to alpha-to-coverage, which is the path this engine renders. The cook landed (11.99), and the sequencing argument sharpened with it: alpha distribution is a per-texture derivation from the level-0 pixels and the desc — exactly the shape `texture-mips/1` already caches — so it would be a change INSIDE texture_derive_levels plus a recipe-version bump, with the cache absorbing the cost after the first run. |

**Tier 5 — the frame budget (spec 11.89, six specs, all shipped):**

**This table has no denominator, and that is why it could not see the largest thing left.** Every
row above asks "what does this renderer not have". None asks "how long does a frame take". Measured
on `apps/forest` — release, M1 Max, 1600x900 Retina, TAA, 120 headless frames — the answer was
**127.7 ms, 7.8 fps, against a 16.6 ms budget**. GPU-timed 115.2 ms of it, so the frame is
GPU-bound; shadow (33.7) and opaque (61.9) were 83% of that and the whole post chain 17%.

That GPU-bound finding is the one that governed everything after it. Three of the six phases
removed submission work — draws, uploads, batch keys — and the frame did not move once, because
the opaque pass never stopped being ~74% of it. **The tier's lesson is not any of its individual
wins: it is that a frame has one bottleneck at a time, and work removed anywhere else is free
only in the sense that nobody pays for it.**

Nine root causes, the measurements behind them and the sequencing are in
`specs/11.89-frame-budget-and-an-honest-instrument.md`, which every row below cites rather than
re-deriving. Read that before adding a row here.

| # | Item | Effort | Why here |
|---|------|--------|----------|
| 55 | **T0 An honest instrument** | S | **DONE (11.89).** `FRAME (wall)` printed exactly 100.000 ms on every forest frame — not a placeholder but a CLAMP applied to the wrong quantity, so the instrument saturated at 10 fps, which is the regime this tier works in. The row is now unbounded above and excludes the run's first frame, whose `dt` has no predecessor and measured a 2x understatement at `-f 2`. Plus the CPU column's backpressure caveat printed where the column is read, and `draw list build` scoped (0.382 ms CPU, 0.000 GPU — small, and worth knowing it is small). **Its first run corrected the spec that built it**: 11.89's draft called the frame CPU-bound off a 126.6 ms CPU column, 58.7 ms of which is one `taa resolve` row against 0.630 ms of GPU. That row is the queue draining, not work. **Two of its own fixes were defects the review caught**: a stall threshold that let FRAME fall below the TIMED it bounds, and a scope that opened twice a frame and logged 120 errors in a 120-frame run — fixed by `profiler_cpu_scope_begin`, a scope with no query, since the once-per-frame rule was always about `GL_TIME_ELAPSED` and never about scopes. |
| 55b | **T0.1 One denominator** | M | **DONE (11.97), and the row's own prescription was one step short.** `gi capture` 724.689 → 7.949 ms; CPU `TIMED` from 21x its `FRAME` to 99.8% of it. Every row now divides by the window's frame count, so a pass entered once and a pass entered every frame are ADDABLE, which is what lets TIMED be read against FRAME at all. **The GPU column could not simply take that denominator**: its results land up to `PROFILER_RING` frames late and an unready slot was dropped, so its count was a sampling artefact and dividing by frames would understate it. Publishing mean-per-occurrence x occurrence-rate is the obvious repair and is REFUSED — the two come from frame sets four apart, so a one-frame sweep straddling a latch boundary reads zero in BOTH windows, and silently absent is worse than 21x too big. Draining the ring at each latch makes the counts equal by construction, deletes the per-scope sample count instead of adding a factor, and removes an 8-11% warm-up bias that fell hardest on the first window — the one `profiler_report` publishes at the run lengths the gates use. **Self-bracketing cost the headroom reading and bought it back with a second row**: FRAME is now the frame's own work, which is what makes `TIMED <= FRAME` an identity for the CPU column, and PERIOD is begin-to-begin, which is what a budget is read against. The latch moved to the TOP of the frame, the only point where both of a frame's clocks are complete. **What the fix uncovered is the bigger half**: five GPU rows in forest read exactly 0.000 across two runs and read real time now — they ran every frame and their results were never collected, because a dropped result and a free pass print the same 0.000. Two latent defects went with it (a leaked timing scope left its query open, silently misattributing every later GPU row; a CPU scope could open inside a GPU one, double-billing the wall clock the identity depends on). **Original row:** Every published number divides by its own count — GPU results the driver returned, frames the scope was entered, frames in the window — so a scope gated on something occasional prints a per-occurrence cost beside per-frame means and `TIMED` sums it in as though it ran throughout. `dir_shadow_fixture --gi-volume`: a one-frame 699 ms GI sweep in a 38-frame window gives **`TIMED` 17.9x its `FRAME`**, with nothing wrong in the timing code, under a header that asserts FRAME is the ceiling. Does not affect 55's forest numbers (every scope there runs every frame) and does not affect this tier's targets. The fix is one count for all three plus self-bracketing the frame clock, which deletes the `dt` parameter and makes `TIMED <= FRAME` an identity for the CPU column — and changes every number the profiler prints, so it wants its own verification pass. |
| 56 | **T1 The shadow pass** | L | **DONE (11.90).** Shadow CPU **32.9 → 5.5 ms**, GPU **33.1 → 17.1 ms**, draws **7,630 → 730**, triangles **163.6M → 82.9M**, frame **127.7 → 102.0 ms**. Three of five steps shipped and **two closed with measurements**: small-caster rejection culls exactly nothing on this content (the coarsest cascade's texel is 0.488 units against props an order of magnitude larger — the mechanism works, at 60 texels it culls 20,892, but the threshold that bites is one that deletes visible trees), and caching the outermost cascade is refused because every tree in it carries `wind_response` and displaces each frame; the version that works is a static/dynamic map pair min-combined, which is its own design. Also found: a slice fit spends `2·radius + scene_pad` of depth on a box `2·radius` wide with `scene_pad` fixed at `far_plane * 0.5`, so depth-per-unit-of-extent is `1 + scene_pad/(2·radius)` and grows without bound as a slice tightens — 1.96 to 12.7 on `dir_shadow`. That is why `shadow_distance` is a per-app knob and not a derived default, and sizing that pad is the prerequisite for making it one. **The mechanisms**: the pass builds its own caster order — compact, then regroup by level — because a depth map resolves by comparison and cannot see what order it was drawn in, and the level half is 94% of that win because Morton order already makes culled items arrive in long blocks where LOD rings cut across the curve; a shadow distance separate from the camera far clip, which is a VIEW distance and was making cascade 1 fit a 967.8-unit box against the outermost's 1000; and it stopped uploading a `model` an instanced draw provably cannot read. **Unanswered**: `calculateShadow` mins across the fragment's cascade and every wider one, so with forest's boxes now ≈61/167/1000 the near cascade is 16x finer than the map flooring it — whether the refit bought a picture or only a draw count is not established, and forest has no golden. Note for whoever picks up the GPU side: the terrain quadtree was **735 of the 1,293 draws standing after the reorder** and is immune to reordering — `terrain_quadtree.c:152` makes a `Mesh` per patch, so every patch is a run of one under any order. |
| 57 | **T2 The instance arena** | M | **DONE (11.91), and it is one `if`.** Opaque GPU **158.6 → 146.8 ms**, accounted GPU **196.0 → 184.0**, frame **203.0 → 199.8** — orphan AND fill in a single `glBufferData` where the write covers the whole allocation. `glBufferData` with a non-NULL pointer replaces the allocation exactly as the NULL form does, so the property the split form was chosen for survives and a second traversal of 12 KB does not. **The arena itself is REFUSED by measurement**, and so is the row's own premise. Three picture-identical probes (`samples shaded` and `depth complexity` pinned, unlike a first attempt whose 159.9 → 84.5 ms was the instances collapsing onto a stale transform and covering half the screen): dropping the orphan costs **62 ms/frame** — 11.28 reached from the other side, now recorded on `ubo_upload` — and a fence-less bump arena, which is an UPPER BOUND on the real one, moves **nothing** outside a 1.7% floor. The reason: a driver servicing 1,300 identically-sized orphans a frame is popping a free list, not asking the kernel — the recycling an arena proposes to do by hand, already done, by the party that actually knows when the GPU is finished. So 48 MB of VRAM, a fence ring, an overflow fallback and a gate arm to prove the fallback was not swallowing the feature, all to reimplement it worse. **The row's 105 MB/frame was also stale before it was read**: 11.90 took shadow draws 7,630 → 730 first. The depth-only block (64 B/instance against 192, so 192 instances a chunk rather than 64) is untouched by any of this and carried to 59 — it needs a second block declaration and therefore a second program, which is a permutation. |
| 58 | **T3 Degenerate LOD bands** | S | **DONE (11.92), and the row's own prescription was wrong.** Camera draws **1667 → 1028**, shadow **645 → 612**, triangles and instances identical. 22 of 80 bands emit their predecessor's exact cut — six canopies at band 1, eight rocks at bands 1 and 2. **The shrink guard this row asked for is REFUSED three times over.** `break` would discard bands that are not degenerate (a canopy's band 2 is genuinely half of band 0), and taking `lod_levels` to 1 is what `mesh_build_cluster_lod` returns false on — so every canopy would fall back to `mesh_build_lod_chain`, a different surface, which is a content change wearing a performance change's clothes. And a duplicate band is not a defect: band *b* is the coarsest cut whose error fits the limit at distance *b*, so an equal cut means nothing in the DAG simplifies inside that budget and refusing it would draw geometry coarser than the limit allows. What the duplicate actually costs is index memory and a distinct batch key, and both are settled by ALIASING the range and canonicalising the selected level onto it — the picture is unchanged by construction. Compared by CONTENT: two different cuts can share a triangle count, and aliasing those draws the wrong geometry. **The frame does not move** — 208 fewer draws and ~0.9 ms of CPU against a 147 ms shading-bound opaque pass — so this is a submission win banked for a draw-bound scene, which forest is not. Two review findings worth carrying: `mesh_index_total` now takes the MAX over levels rather than the last level's end, because an alias means the last band need not name the buffer's end and the old form would have uploaded a truncated EBO the moment anyone compared against a non-adjacent band; and the batch arm asserts STRICTLY fewer draws where a band aliases, because under `<=` the level-collapse half — which is 100% of the win — could be deleted with the whole suite green. |
| 59 | **T4 Shader permutations** | L | **DONE (11.93), and the only phase of six that moved the frame.** Opaque **149.25 → 123.55 ms (−17.2%)**, frame **203.72 → 177.02 (−13.1%)**, geometry untouched. It beat its own hand-strip ceiling of 125.12, because the probe had left the anisotropy map fetch in. **The mechanism is OCCUPANCY, not skipped work, and that is the row's real content**: every gate in `pbr_frag` is a dynamically uniform branch an unusing scene already skips, so guarding one at RUNTIME is worth 0.05 ms while removing it at COMPILE time is worth 24. Live values and declared samplers are paid by every fragment whether or not the branch is taken — the in-tree calibration (two `vec3`s costing 11% of the pass) was measuring exactly that and nobody had read it that way. What shipped: `shader_source_with_defines` splicing after `#version`; five subtractive gates, so no defines is byte-identical to the uber-shader and a resolver that fails to run yields the SLOW program rather than a fast one missing a feature; and a per-frame resolver, because the mask depends on six material fields a GUI slider moves with nothing marked dirty. **The C-to-GLSL contract is a shared MASK** (`pbr_features.glsl`, the `wind_bounds` mechanism) after the first shape — C emitting a macro NAME the shader tested for — turned out to be untypeable-wrong-and-silent: a rename either side and the guard never fires, the picture stays correct, and the optimisation just stops. **Seven reviewers found one shipped bug**: the resolver read `scene->lights` 48 lines before the frame's only writer of it, so a derived LTC panel got a no-area variant whose diffuse was right and whose specular was identically zero. Still open from this row: **sampler DECLARATIONS**, which the spec put first in evidence order and did not ship — now reachable, since gating the area panel removed the last read of `ltcTex`, and it would be the first mechanism to dissolve Wall 1 rather than dodge it. **`pbr_skinned` gets none of this** (same fragment source, no skinned variants built), and **`PBR_FEAT_PARALLAX` has no coverage of any kind**. The depth-only instance block inherited from 11.91 is also still unbuilt — and 11.91's own arithmetic now argues against it: the shadow pass runs 15.8 instances per draw against a cap of 64, so runs end on a key change, not on the cap. |
| 60 | **T5 The small independent wins** | S | **CLOSED (11.94) having shipped nothing, and the measurements are the deliverable.** Every claim in this row was TRUE — the first time in Tier 5 a booked premise survived checking — but they are four different KINDS of thing and only one could ever have moved a clock. **`glGetError` is not a stall on this driver**: disabling all 23 calls measures 122.6 ms opaque against 122.0 with them on, i.e. slower, i.e. nothing. That was the phase's only candidate and the common wisdom about flushing does not hold here. **The cluster early-out needs no probe** — `cluster build` is 0.036 ms of a 174 ms frame. **`_item_bounds` caching is refused** for the same arithmetic one level up: it sits inside ~11 ms of total CPU submission against ~160 ms of GPU, so eliminating it cannot move the clock and the fix is new cache state. **The shadow VRAM saving is half what this row claims** — `shadow.c`'s MSM comment already warns that the caster count oscillates 1→2→1 daily as sky bodies set, so grow-only settles at 2 casters and saves 50 MB, not 100; and `csm.glsl` computes the transmittance base from `MAX_SHADOW_LIGHTS`, making the ceiling a SHADER constant. Deferred, not refused: it is real memory on an axis nothing here measures. **And the ordering bug is an ARCHITECTURE defect mis-filed as a small win** — the depth pass runs at `engine.c:2625` while all SIX apps apply their scene transform in the render callback at `:2716`, so every shadow is a frame stale and every LOD level is chosen from stale positions. `engine_run`'s update callback at `:2551` is the right hook and already exists, but `apps/render` passes a non-identity matrix, so the engine cannot propagate on everyone's behalf. Carried to its own spec (row 61). |
| 61 | **T6 The frame-stale scene graph** | M | **DONE (11.96), and the row was wrong about five things — every one of which changed the work.** The premise held: the engine now propagates the graph, behind a `pre_render` hook each app fills, and raiden moves **227,349 px (2.7% of frame, PAE 0.027)** concentrated on the rig's body and hair. One golden moved, `froxel_fog` at 51 px and PAE exactly 1/255, rebaked. **(1) There were TWO stale things.** `apps/render` also stepped its skeletal pose in the render callback, and the depth pass draws skinned casters from what `set_render_animation_state` publishes — a rig's shadow lagged its body for the same reason its transform did, and the row named only the transform. **(2) Both apps this row names are wrong.** `apps/shapes` does not animate its matrix (commit `45808f15` removed the last clock read) and `apps/render`'s is a load-time constant; the app that actually passes a non-identity matrix is **`apps/pcb`**, which the row does not mention. Nothing recomputes a root matrix per frame, so the stated reason for needing a per-app matrix hook does not exist — it became `Scene.root_transform`, set once. **(3) The hook 11.94 recommended is the wrong one.** The window is one statement wide: after the origin shift, which rewrites root-child locals and the tree's cached globals; after `sky_cycle_tick`, which rewrites a sun's `original_direction` that the walk rotates; and before the GI capture, whose probes a converged volume never re-bakes. A walk at the update callback is undone by the first and strands a node-parented sun by the second. **(4) The GI half is permanent, not one-frame**, and five of six apps never walked at load at all — so a GI volume in any of them would have baked a scene collapsed at the origin, forever. **(5) `dir_shadow` cannot show it**; the fixture had to be built, and is (see the known-limitations entry). Two design notes worth carrying: folding the root offset into `root_node->original_transform` costs **732,291 px** on `guard_thin_panel`, because a model-loaded scene can already carry a non-identity root local; and the `prev := global` latch had to be **split out of the walk**, which is what makes the walk idempotent, deletes the frame stamp that guarded it, and downgrades "mutate the graph in the wrong hook" from a silent catastrophe to a note. |
| 62 | **T7 Sampler declarations, and the end of Wall 1** | M | **DONE (11.95). A lean variant spends 12 of 16, and Wall 1 now has a sixth escape that RETURNS units rather than routing around them** — the first in the tree that leaves the ledger emptier than it found it. **The row's premise was checked before it was built, and it survived**: 11.93 folds every read of `ltcTex` away on a `pbr-0` variant, so GL might already have dropped the sampler and freed the unit for nothing. It had not — `pbr`, `pbr-4` and `pbr-0` all reported 16 active samplers, with `pbr-4` the control that makes it more than incomplete elimination. A declaration outlives a dead read, which is what the whole item rests on. Three of the row's own claims were wrong. **Three samplers were fully gated, not four** — `heightTex` missed by exactly one read, `layers.glsl:407`'s virtual-texture page tap, whose path is selected by a UNIFORM rather than by `layerCount`. **The one-line `#if` it implies is not the work**: `pbr_frag` contained no `#if` at all, every gate being a runtime bool over a constant macro, so each declaration's readers had to move to the preprocessor with it. And **the `heightTex` trap is loud, not silent** — the alias expands to an undeclared name, which is a compile error on the first forest run and not the plausible frame this table feared. What it cost and bought: the fourth unit needed a sixth feature bit and is worth **0 ms**, while that bit — `PBR_FEAT_LAYERS` — turned out to be **the larger win of the spec at −4.84% opaque**, because `sampleLayeredSurface` is called on every fragment of every material with its `layerCount <= 0` early-out INSIDE the callee. The row had the two backwards: it booked the bit as the price of the unit. Frame **123.41 → 115.35 ms opaque, 177.75 → 169.77 wall (−4.5%)**, seven goldens rebaked at a worst-channel step of exactly 1/255. |
| 63 | **T8 Skinned variants** | S | **DONE (11.95), and it was S as booked.** `pbr_skinned-0` spends 12 units where `pbr_skinned` spends 16. The row was right that the families differ only in the vertex stage, and that is exactly why it stayed small: one mask means the same thing in both, so a second family cost a builder argument rather than a second set of gates. Setting `pbr_features` is the whole of what admits it — membership was already `pbr_features >= 0` and the old constructor simply never set it. Two derived flags carried themselves: `depth_prepass_safe` because both stages take their clip position from `object_position.glsl`, and `instanced` because `ubo_wire_blocks` resolves it from the linked program, so a skinned variant comes out false and 11.28's one-instance rule holds with nothing here asserting it. **What the row understated is the verification, not the work.** `apps/render` being skinned throughout does not mean the goldens are: **none of the 29 is skinned**, the corpus is entirely static, so before `pbr-variant-skinned` this family had no instrument at all and could have returned to the uber-shader with everything green. The one check that can see it is the raiden recipe, which moved **2 px of 8,294,400** at one 8-bit code — real against a 0 px floor, and the instruction-scheduling signature 11.93 already recorded. |

**What this tier will NOT do**, each for a reason already measured: E7 occlusion culling (depth
complexity is 1.81 — the cost is shading and submission, not hidden-object overdraw), chasing the
SSR/atmosphere/`taa resolve` CPU numbers (backpressure, see 55), turning the depth prepass on
(11.30/11.31 measured it as a substitute for the sort and the loser on the clock), or raising
`--lod-bias` (measured: at the default framing the frame gets slower, because it adds draws faster
than it removes triangles -- note `forest-lod` measures 45% of triangles saved from its OWN wide
framing and passes, so the lever is the batching and not the bias).

**If only five ever get built: 20 -> 21 -> 22 -> 23 -> 24.** One afternoon, then three items that each
reuse a shipped subsystem rather than building new machinery, then the instrument the rest needs.
**All five are now done** (20 in 11.24, 21 in 11.26, 22 in 11.39+11.40, 23 in 11.49, 24 in 11.27).
The shortlist is exhausted, and 23 was the only one of the five that needed nothing from Wall 1 —
`Light` already carried every field the fit produces, and it cost no sampler unit.

**31 (E6) was the one to build next, and it is now built.** An earlier draft of this line read
"nothing else on this table is worth 250 ms", which priced E6 at the whole of the opaque pass before
anything had measured how much of that pass is redundant. The real figure is **−45% on forest**
(opaque 306 → 169 ms, from the ORDERING alone) — smaller than the invented number, and arrived at
with the crossover, the worst case and the withdrawn claims all recorded. **This paragraph read
"−41% on forest and −64% on the interior" and both halves were wrong**: the interior's −64% is one
of the seven claims 11.31 withdrew, and that scene is now recorded as one where the prepass *costs*
6.7%. Row 31 has said so since 11.31 while this line went on contradicting it — the same rot the row
above describes, in the paragraph that tells you what to build next. **37 (E7, occlusion culling)
should be re-priced
against them**: it was booked as not-yet-justified on the grounds that E6 would get most of the
benefit for less, and E6 doing so on opaque content is now measured rather than assumed. **Done:
11.98 re-priced it against the post-E6 frame and built it -- see row 37.**

**23 (C2) was what's next for a long time, and with it and 27 (E3) both built this table has no
obvious head.** What remains is either small and self-contained (25 C3 IES, 26 C5 local contact
shadows, 28 E2 grading, 34 E8 the wind cull) or L/XL with no measurement demanding it yet (29 C4,
32 D0, 33 D1, 36 D4, 37 E7). Nothing in the first group is blocked and nothing in the second has a
number behind it, so the next pick is a judgement rather than a consequence — which is a different
situation from the one this table has described since 11.24, and worth saying plainly rather than
nominating a successor by default.

Of the small ones, **34 (E8, the wind cull) was the pick and is now built** (11.53), and **26 (C5,
contact shadows for local lights) followed it** (11.56) — it had gained a claim it did not have when
it was written, since 11.49 shipped a *producer* of local lights that are ineligible for a map by
construction. **25 (C3, IES) followed both** (11.57), and **28 (E2, 3D LUT grading) is now built too**
(11.58). That emptied the small-and-unblocked column: what was left is 29 (C4), 32 (D0),
33 (D1), 36 (D4) and 37 (E7), all L or XL, plus 38 (E10, integer-bit hashes), which is S
and is the only one of those that arrives with a measured price already attached --
11.54 booked it against `ssr_frag`'s 31,800 px, so it can be judged on a number rather
than on tidiness. **36 (D4) has since gone too** (11.69), **29 (C4) with it** (11.70) and **33 (D1) with 11.73**,
leaving 32 and 37.

**And then the column refilled, because the table was missing an entire subject.** Rows 39-44 (D6-D11)
were added by 11.59 after a comparison against what UE, Unity, Frostbite and Decima actually ship:
this engine has no erosion, no material layers, no heightmap authoring path and no large-world
coordinates, and **"splat", "virtual texture" and "erosion" appeared nowhere in this document**. The
one terrain row it did have, 36, booked the STREAMING half and booked it first. That ordering was
backwards in a way worth recording: streaming is a problem you get once terrain is data, and what
turns terrain into data is **erosion** -- a simulation over a grid, with no `f(x,z)` to write -- not
scale. So 36 moves to the end of its own chain and 39 (S) and 41 (S) join 38 in the small column.

**All three of those are now built** (11.59), which left the table with **42 (D9, terrain material
layers, M)** as the next thing to do and the only booked item with a producer already shipped waiting
on it: the flow / deposit / wear masks existed, round-tripped through the heightmap format, and were
consumed by a per-VERTEX tint at 2.6 units. **42 is now built too** (11.60), and the masks have a
consumer that resolves per texel.

What is left is **nothing -- the table is closed**. 54 (G2) went with 11.100, its distribution
half built and its hashed half recorded as refused-for-now inside the row, which closes the last
open item. (This sentence read "54 (G2), and nothing else" until then, and the prediction it
carried held: the change landed inside `texture_derive_levels` plus a recipe bump, with the cook
absorbing the cost -- though the row it pointed at needed four corrections, recorded there.) The
comparison habit's third run is now due -- checking the other documents first, per the Track F
lesson below, since animation blending, IK and audio are already booked in
`docs/game-engine-status.md` and "not in this table" is not "not written down". (This sentence read 32, 37 and 38 until
the sweep after 11.99, and had read 29, 32, 37, 44 and "the re-scoped 36" until the sweep after
11.69: it dropped 33, which was never built, and carried 44 and 36 after 11.62 and 11.69 shipped
them. 29 went with 11.70, **and 33 with 11.73**. It then carried 32 for fourteen specs after 11.85
had refused it -- the rot this tail keeps recording in rows, in the tail itself, and row 32's
summary cell went unstamped for the same stretch.) **The one of those the roadmap argued against
itself was 32 (D0)**, which
had no scheduled consumer after 11.57, and 11.73 is the FIFTH feature in a row to need
no unit at all: N reflection probes arrived as tenants of a declaration that already existed, and
clustered decals -- the one row booked against D0 by name -- arrived the same way. **43 (D10) is the only one 11.60 moved the case for, and it moved it DOWN** -- past ~8 layers or
roads/decals, with four layers at three declarations and 4-25 taps. 11.66 then shipped its stage 1
anyway and the measurement went the other way: the 25-tap end of that range is most of an eroded
terrain, and the cache takes it to 5-9 independent of layer count. Nothing is waiting on stage 2's
paging until the macro itself gains dense content.

**And after B15 closed row 50 the table was, for the first time, empty of anything with a number
behind it** -- 32 and 37 both booked against themselves, 38 rejected. So the habit two paragraphs
below was run rather than merely recommended, and it refilled the column exactly as it did in
11.59: **rows 51 and 52 (F1, F2) are a subject this document never contained.** The finding that
matters most is the one that would have killed a spec in its first week -- **"BC7/BC5" is the
obvious plan and BC7 does not exist on this platform**, since BPTC is core in GL 4.2 and Apple's
4.1 exposes neither it nor an extension for it. RGTC reads as absent for the opposite reason: it
is core in GL 3.0, so it carries no extension string to grep for and is available anyway. Neither
half survives being reasoned about; both took a probe. Same lesson as 11.57's, one layer down --
**re-derive the constraint before accepting the conclusion, and for a platform capability that
means asking the driver, not the version number.**

**The lesson 11.60 adds is about this table rather than about terrain.** 11.52 recorded that a row's
stated reasons can rot; 11.53 that a row can be wrong about what it describes; 11.56 that a row's
premise can name a defect another spec already fixed; 11.59 that the table can be missing an entire
subject. This is the fifth kind: **a row can reason correctly from a real constraint and stop one
step short of the answer.** Row 42 knew the sampler ledger was full, knew N textures of one shape cost
one declaration, and concluded it needed a program with room -- without asking whether the
declaration it wanted was already bound on every draw. It was. The item cost an M as booked and would
have cost an L as described.

**The generalisable part is not about terrain.** 11.52 recorded that a row's stated reasons can rot;
11.53 that a row can be wrong about what it describes; 11.56 that a row's premise can name a defect
another spec already fixed. This is the fourth kind and the largest: **a table can be complete with
respect to itself and still be missing a subject**, and no amount of reading the rows finds it,
because the absence is not written anywhere. What found it was comparing against engines outside this
document. Worth doing again for the tracks that have never had that comparison. **Run a second time
after B15, and it produced Track F** — the asset pipeline, rows 51 and 52. The comparison found
four candidate subjects and only two were genuinely missing: animation blending, IK and audio are
all already booked, with effort estimates, in `docs/game-engine-status.md`, **a third document this
one has never once cited.** So the habit needs a companion: before booking a subject as absent,
check the other documents, because "not in this table" and "not written down" are different claims
and this table is not the whole of what is written down.

**39, 40 and 41 are now built (11.59), and the fifth lesson is about the TEST rather than the row.**
Three of that spec's eight planned arms were green against the exact mutation each existed to catch,
and one arm the plan listed was never written at all. The sharpest was the filter arm: it was
rewritten twice -- as a smoothness ratio, then against an analytic normal -- and passed a bilinear
sampler both times, because `terrain_normal_at` differences over less than a cell and inside a cell a
bilinear surface IS the linearisation that difference estimates. **The instrument could not see the
property by construction, and nothing about it looked wrong.** The FIXTURE had the same defect a
level down: at 85 samples per cycle the two interpolants differ by five 16-bit codes, so it could not
have separated them whatever the arm did.

**And the review found two functional defects that no arm could see, one of which the spec had
specified and not built.** The mask round trip is the one to remember: the save wrote geometry and
dropped the masks, so the shipping path produced eroded terrain shaded by the guess erosion exists to
replace -- the feature's own opening failure, reached through its own save path, with the whole suite
green. The transferable rule is narrow and hard: **a round-trip arm that compares only the payload it
finds easiest to compare is not a round-trip arm**, and the half it skips is the half the feature was
for.

11.53 adds a third entry to the observation below, and the sharpest one: **a row can be wrong about
what it is describing, not merely about why.** E8's row and section both named wind alone where the
code exempted skinned meshes too, and both prescribed a fix that does not address the half they
missed. Five specs read past it. The habit that catches it is the same one 11.52 named — read the code
the row describes — and the cost of not doing it here would have been shipping half an item under the
belief it was whole.

**And its review found the same failure one level down, three times, which is the part worth
generalising.** A row that describes half the code produced a fixture that tested a fraction of the
item (`turbulence: 0` made six shared coefficients and the whole vegetation branch inert), a shared
file written as though its technique were novel when the solved precedent sat four files away in the
same directory, and a gate opting out of a checker on two precedents that were not precedents. So the
lesson is not only "read the code the row describes" — it is that **the same incuriosity produces a
green test, a duplicated mechanism and a false citation**, and only the first of those looks like
success. What caught all three was reading the neighbours: the other 39 generators, the other
`include/` files, the other gate groups.

**11.56 is the third table-driven spec in a row and it completes the pattern.** 11.52 recorded that a
row's stated reasons can rot while the row stays right; 11.53 that a row can be wrong about what it
describes, not merely about why. C5 is both at once, and the sharper form: its reason named a defect
that **another spec in this same document had already fixed** — 10.3 and 10.4, six specs earlier,
with a named visual gate — while the row went on citing it as the thing to build for. Reading the
code the row describes would not have been enough here; what was needed was reading the *specs the
row's premise depends on*. And the correction made the item bigger rather than smaller: the
population C5 actually serves is ~120 of 128 clusterable lights, not a sharpening of the eight that
already have maps. **A row whose justification is stale is not a row to delete — the three so far
have each been worth more than they claimed, and finding out why is what the exploration phase is
for.**

**11.57 is the fourth in a row, and its failure mode is a new one: the row was wrong about its own
constraint.** C3 correctly identified that a table in uniform space costs zero sampler units, said so
in bold, and then in the next sentence deferred the asymmetric case "rather than paying a unit" — an
argument that only makes sense if the table were a texture, which its own preceding sentence had just
established it is not. The premise and the deferral were **two paragraphs apart and contradicted each
other**, and the deferral is what four specs' worth of readers carried forward. So the growing list
is: reasons that rot (11.52), a row wrong about what it describes (11.53), a row citing a defect this
document had already fixed (11.56), and now **a row that argues against itself inside one entry**.

The cheap habit that catches all four is the same and worth stating once: **re-derive the row's
constraint from the code before accepting what the row concludes from it.** In this case that took one
grep — bindings 0–5 used against a GL minimum of 36 — and it turned a v1/v2 split into a single spec.
It also caught the row's storage plan being unimplementable as written, which no amount of agreeing
with its conclusion would have.

**And 11.56's own review is the counterpart observation, about specs rather than rows.** The spec
that corrected C5's stale justification shipped three defects and a dead test of its own, and every
one was a claim running ahead of the code in exactly the way it was written to complain about: a
stated error bound that was false in two directions, a comment asserting three buffers were
single-channel when two are RGBA, a "cannot double-shadow anything" that only became true when the
review fixed the denominator, and an assert whose condition three lines above it made unfailable.
Eight agents found them; five phases of building, a full golden run and 210 green gate arms had not.
**A green suite measures what the tests can see, and the tests were written by the same reading of
the problem that produced the code.** The one guard that worked was independence — the defect that
mattered most was found by the one agent asked to attack the design rather than the implementation.

Worth recording the inverse too, because it is the same discipline: one review finding was itself
wrong (a budget compared in MB against MiB), and taking it would have replaced a correct number with
a wrong one in two permanent documents. **Verify a correction before applying it, exactly as you
would verify a row.**

**11.58 is the fifth in a row, and it moves the pattern down a level: from rows that are wrong to
TESTS that are wrong in the same way.** E2's row was mostly right — its one-line warning about the
working-space contract was the most useful sentence in the entry — and it still carried a wrong
number (32³, a size `.cube` does not produce) in the sentence right beside the warning. That is
familiar. What is new is where the same failure turned up next.

The gate's identity arm was written to catch the half-texel inset, said so in its docstring, **and
was green with the inset deleted.** Not because the arm was weak, but because it ran on a path that
structurally cannot see that mistake: the inset belongs to trilinear, and the default tetrahedral
path addresses texels by integer index. The arm was measuring something real and reporting something
false about what it covered.

Nothing detected it except deleting the mechanism and watching the arm stay green. A passing run,
seven passing arms and a full green suite all said the opposite. So the practice that matters is not
"write an arm per claim" — it is **falsify every arm against the specific mutation its docstring
names**, because an arm that cannot fail is indistinguishable from one that works right up until it
matters. 11.21, 11.22 and 11.40 each recorded a version of this about features; this is the first
time it landed on a claim about *coverage itself*, which is the harder one to notice because the
number the arm prints is correct.

The same spec's fixture failed twice in the same shape and both were silent: the probe table that
separates the two interpolants measured **exactly zero** as a separable construction and **exactly
zero again** as a channel-symmetric one, for two different and individually obvious reasons. Building
it by reasoning produced a green instrument that tested nothing, twice; only measuring it caught
either.

**11.58's review round sharpens the fifth entry into a sixth, and it is the one to carry.** The
spec had already recorded that a probe table built by reasoning can measure exactly zero — twice, on
`lut_neutral`, for two different reasons — and then took **both of its headline measurements on
constructions with the same defect**. The tri-vs-tet number came off a table whose every output
channel is a ridge function of two inputs, so the three-way cross term tetrahedral drops is
identically zero on it; the real figure is 28x larger. The fp16-vs-fp32 storage test came off an
identity table whose every lattice value is exactly representable in fp16, so storage error was
structurally zero. Both conclusions survived re-measurement; neither piece of evidence did.

So the list is now: reasons that rot (11.52), a row wrong about what it describes (11.53), a row
citing a defect this document had already fixed (11.56), a row arguing against itself (11.57), and
**a spec that documented a measurement trap at length and then fell into it twice in the same
file** (11.58). The generator asserted three anti-degeneracy properties on the table it had been
burned by and none on the two either side of it.

The habit that catches this one is narrower than "read the code": **before believing a measurement,
ask what construction it was taken on and whether that construction can exhibit the effect.** A
number is not evidence until its instrument is.

And the sharpest detail is why one of the three survived so long: **the state it goes wrong in was
unreachable from the harness.** `Light.shadow_layer` only goes stale when the shadow system is
switched off *after* the depth pass has run, and `--no-shadows` clears it before frame 0 — so every
headless run exercised the never-enabled path and none exercised the transition. Closing that gap
meant building the instrument first (`--shadows-off-at`, in the `--render-scale-at` idiom). **A
defect that no flag can reach is not covered by any number of gate arms**, and the corpus now has two
of these levers on the same principle.

**11.57's own review is the sharper version of that last point, and it generalises past shadows.**
Eight agents over a finished, fully green spec, and the findings that mattered were not code defects
at all -- they were **claims the spec made about its own coverage**, each true-sounding and each
false:

- "Applied at all five places, through one shared function." Four. The fifth restated the rule, and
  the comment asserting otherwise is worse than no comment, because the next reader trusts it.
- "The fold is held together by `--ies-probe` reading the angles one at a time." It is not, and
  cannot be: the probe samples AT the taps, correctly, and every tap sits inside `[0, span]` where a
  mirror and a modulo agree exactly. Two of the three fold copies were verified by nothing. The
  shader's copy -- the one that decides pixels -- was covered by an arm that rendered only the two
  files where the fold cannot matter.
- "A profile REPLACES the cone." Stated in three places, tested nowhere: the fixture is a POINT
  light and `spotConeFactor` returns 1.0 for every non-spot, so multiplying and replacing were
  byte-identical on every arm in the group.
- "The tail is exactly zero." True, but by the fixture painting literal zeros -- so the snap that
  ENFORCES it was never the thing measured, and deleting it from both readers left everything green.
- "Area panels ignore profiles -- nothing refuses it, the index simply never reaches the LTC
  branch." It reaches the contact-shadow fold's DENOMINATOR, which is the identical error 11.56 had
  just fixed and measured at 23%.

So the pattern to add is not "specs ship defects" -- 11.56 already recorded that. It is that **a
spec's confident sentences about what it verified are the least-verified thing in it.** Each of
those five reads as a result. Each was an intention that the arms had never been asked to confirm,
and four of them were written by the same person, in the same session, as the arms that were
supposed to confirm them. **The test for a coverage claim is not "is it plausible" but "what
one-line mutation would this arm catch, and can I name one it would not"** -- which is the falsify-
by-hand discipline this document already requires for arms, applied one level up, to the sentences
that describe them.

The concrete lever it produced: `ies-mirror` reads the frame's left-right symmetry, because the
fixture is symmetric about x = 0 and a partial sweep therefore MUST render symmetric. Bilateral
0.0055 and quadrant 0.0059, against 0.9861 and 0.8347 with the mirror replaced by a modulo -- with
the symmetric and 360-degree files unmoved either way, which is what makes them controls rather than
more of the same test.

**Two observations about this table rather than items in it**, both from two earlier specs.

11.49: three of its four real defects were invisible in every frame — a first bounce counted twice, a
panel lighting through a solid wall, a placement drifting 29 units over 40 frames — and each was found
by an arm reading an instrument, not by looking. The two that a picture *did* show were both found by
the user looking at the picture.

11.52: **a row's stated reasons can rot while the row stays right.** Both of E3's arguments had been
overtaken by commits that fixed them, and the item was still worth building on parity grounds alone —
but a spec that had inherited those reasons would have claimed benefits its own tests could not show.
The habit that catches it is cheap: read the code the row describes before believing the row. It also
found the defect the row *should* have been arguing from, which no one had looked for because nothing
could see the metered value at all.

**And the honest observation this table should carry: for most of its life it has not driven the
work.** Twenty-three specs have shipped since D3 opened, and **ten of them are the water and shore
series (11.32, 11.33, 11.35, 11.36, 11.42–11.45, 11.47, 11.48), against which this table has exactly
one row — D3 — and it covers three of the ten.** That is not a failure of the roadmap — the surface
turned out to have far more in it than one XL row could hold, and every one of those specs measured
something before it changed anything, which is the standard this document exists to enforce. But it
does mean the table above describes a *backlog*, not a plan, and that all five items in its
own shortlist were finished while the actual work went somewhere else entirely. Anyone reading the
tier order as a schedule should read this paragraph first.

**The last few are the exception, and worth naming as one.** 11.53 came straight off row 34, 11.54
off its review and 11.56 off row 26 — the first table-driven run since the water series began. What that produced is also
the argument for reading a row against the code before believing it: row 34 described half of what
the flag it named actually did, its fixture then tested a fraction of what the spec built, and the
instrument that finally checked the whole thing had to be rebuilt once after the first version was
measured reading straight through the failure it existed for. The table found the work; it did not
describe it correctly, and neither did the first two attempts at testing it.

### Known limitations not booked as items

Recorded because they are real, understood, and currently nobody's row — not because they are
scheduled.

- **Screen-space post does not see transparent surfaces. FOG AND AERIAL ARE FIXED (spec 11.78);
  the rest stands.** The aux attachment holds ONE linear Z per pixel and the late pass never writes
  it, so everything reading it treats a transparent pixel as the wall behind it. **The mechanism is
  the draw-buffer list, not the depth mask** — this entry said `glDepthMask(GL_FALSE)` at a
  `render.c:975` that has since moved to `:1523`, but `render.c:1342` drops `glDrawBuffers` to count 1
  when the opaque scope closes and nothing re-arms it, so the write `pbr_frag.glsl:2418` still
  performs is discarded by the buffer list. A late pass that started writing depth would still write
  no aux. Water escapes it by writing aux like an opaque surface (`water_frag.glsl:1233`); a stack of
  translucent layers cannot, because one slot cannot hold both its depth and the wall's.
  **The consumer list was also a third of the truth.** Aerial is not a separate reader — its depth
  read is the same line as fog's (`froxel_composite_frag.glsl:52`) — and **DoF is a different buffer
  entirely**, reading the resolved hardware depth (`dof_coc_frag.glsl:10`), so fixing aux does
  nothing for it. What actually reads aux, beyond fog and GTAO: **TAA/TAAU reprojection and motion
  blur** (a glass pixel gets the *wall's* velocity), contact shadows, the SSGI/SSR/SSAO edge-stopping
  weights, and the spec-occ composite.
  **11.78 fixed fog and aerial**, and not by the route this entry proposed: sampling the froxel volume
  in `pbr_frag` needs TWO `sampler3D` declarations (fog and aerial are separate volumes with separate
  owners, nears and Z exponents), D0 frees ONE and is itself blocked on ownership, and both volumes
  are built AFTER the scene pass so a fragment read would be a frame stale. It is done in the
  composite instead, off the MBOIT moments — `b0` is the translucent stack's absorbance and `b1/b0`
  its mean warped depth, so the pass folds its two media at that depth as well and mixes by coverage.
  Zero new sampler units, all 27 goldens 0 px.
  **What is left is the harder half and is largely inherent to forward rendering**: AO, TAA, motion
  blur, contact shadows and DoF still read a single-layer depth. Transmissive surfaces are out too —
  they never enter OIT — though that case is mostly right already, since light seen *through* glass
  genuinely did travel from the wall and only the surface term is misfogged. **Particles are out and
  are the weakest of those exclusions**: they draw in their own pass after OIT, generate no moments,
  and a smoke card is exactly the near translucent surface this defect is most visible on.
- ~~**No golden runner.**~~ **BUILT (spec 11.25)** — `python3 scripts/goldens.py` checks the whole
  corpus in one command (nineteen then, **24 now**), and every render command lives in one table
  instead of in whichever spec introduced it. **The corpus was never broken**, which is the part
  worth carrying: 11.17
  recorded three goldens as having no recipe anywhere, 11.21 repeated it and named four, and 11.24
  concluded six of nineteen were unreproducible and wrote that here. All three were wrong. Four of
  the six had their recipe in `assets/area_light_goldens.md`, a per-feature ledger nobody thought
  to open; the two `cloud_fixture` goldens do not name a fixture at all, since **no such file has
  ever existed in this repository** — they are the aerial fixture with `--clouds`. Three specs in
  a row reached a false conclusion from the same cause, which is a better argument for one table
  than any of them made on purpose.
- **Anisotropic filtering is capped at 8x** (`texture.c:89`) and never exposed as a setting.
- ~~**Shadows read last frame's transforms.**~~ **FIXED (spec 11.96)**, and it was TWO lags rather
  than the one this entry described: the node transform, and `apps/render`'s skeletal pose, which the
  depth pass draws skinned casters from. The engine now propagates the graph itself, in a
  one-statement window between the origin shift and the GI capture, behind a `pre_render` hook each
  app fills. **The fixture this entry asked for exists** — `shadow_lag_fixture`, a caster that holds
  still and then starts, out of frame so its shadow is the only thing that can change, reading 551 px
  where the broken build reads 0. Two things it had to be built around are worth carrying: a constant
  velocity shows nothing, because a lagging shadow and a tracking one are displaced identically
  between consecutive frames; and the motion has to be an authored clip, because a schedule flag
  driven from the render app's update hook runs BEFORE the shadow pass and passes against the bug.
- **`glUseProgram` has no choke point, and the uniform value cache assumes one.** The setters write
  through `glUniform*`, which targets whatever is *bound*, not what the `UniformManager` names — so a
  set under the wrong program updates that program and records the value here, after which the next
  legitimate write is skipped as already-held. Verified clean (zero violations across seven scenes
  under `-DCETRA_CHECK_UNIFORM_BINDING=1`) but only *believed* going forward, because the check costs
  a `glGetIntegerv` per set, about 6% of the frame, and is off by default. Neither Godot nor Unreal has
  this problem, and not by asserting harder: neither lets app code call `glUseProgram` at all, so
  "what is bound" is a mirrored variable and the check is a compare rather than a driver round trip.
  Cetra calls it from **123 sites across 13 files** (105 excluding `glUseProgram(0)` resets) -- this
said ~30, which understates the cost by about 3.5x. The fix is still one function.
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
  through the same call. **11.63 closed this twice over and by neither of the answers booked here.**
  Terrain no longer takes a chain at all: a quadtree patch is built at its own level and CDLOD
  morphs the fine side of a seam onto the parent surface, so there is no junction. And props take a
  cluster DAG whose seal is structural — every cluster at every level indexes the ORIGINAL vertex
  buffer, because simplification only ever drops vertices, so two clusters sharing an edge share the
  literal same vertices whatever levels they came from. The stitch at `8d04658` is still the answer
  for a case that has neither, and there is not currently one.
- **The GL 4.1 ceiling itself is the Tier 5 question.** Nothing in Tier 4 needs compute. Lumen-class
  GI, virtual shadow maps, GPU-driven culling and hardware ray tracing all do, and
  **Nanite-style cluster DAGs are the entry on this list that turned out to be TWO things** — 11.63
  shipped the CPU half on GL 4.1 with no compute, no SSBO and no atomic, because the DAG build and a
  cut quantised by DISTANCE BAND are both CPU work and the result lands in the same EBO an LOD chain
  already used. What genuinely needs 4.3+ is GPU-driven SELECTION and the visibility buffer, which
  is a different item and aimed at micropolygons this renderer does not have. Read the rest of this
  list with that in mind: the ceiling blocks less than the label suggests, which is the same lesson
  the sampler ledger's five escapes teach.
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
| Bent normal in the AO chain | A5 — **delivered, and since 11.77 UNREAD**: debug view 9 is its only consumer. The three listed successors want a cheap directional signal, and 11.76 measured the full 32-bit sector mask beating a collapsed mean direction at precisely that. Do not assume this is the foundation you want; see A5 for the open cost question | SSGI directionality, SSR occlusion, DDGI sampling |
| `create_texture_3d_float` + `include/froxel.glsl` (slice count parameterized, so a differently-sized volume reuses it) | B1 | B9 aerial perspective, B2 clouds, future volumetrics |
| CPU 3D noise (`noise_worley3`, Perlin-Worley packing, threaded bake) | B2 | ground fog detail, media |
| Render-res/post-res split — **delivered** as four sizes (`width/height` render, `post_*`, `out_*`, `half_*` = render/2), plus the canvas locals every post-seam pass composites onto | B4 | B5, B7, tonemap |
| Transmittance-vs-depth storage in the shadow path | C1 — **delivered** (11.26), as deep opacity maps rather than the moments the sketch assumed | any translucent caster: hair, glass, foliage tips, smoke |
| Freed `pbr_frag` sampler units | D0 (proposed, and it frees ONE) | **the consumer list this row carried is withdrawn** — D2's surface half shipped without it (11.41), and detail/wetness maps were never blocked (they are mask-array layers). ~~What is left is D1 *if* a flat 2D atlas is refused, and~~ **D1 shipped in 11.73 without one** — a decal image is a tenant of unit 2's array. What is left is sampling the froxel volume from the transparent pass, and that is not booked |
| Tessellation pipeline (program creation, patch draw, distance LOD) | **still unowned** — D3 shipped without it, and D4 shipped without it too (11.63's CDLOD morph is vertex work) | POM silhouettes |
| Bed-height seam (`WaterHeightFn`) + the CPU Gerstner query | D3 — **delivered** (11.32, 11.33) | Jolt buoyancy, gameplay water tests, any surface that shoals |
| Geometry clipmap: coarsest-cell snap + T-junction stitch | D3 — built (11.33), **removed** (11.35), kept at `8d04658` | **nothing now.** D4 chose CDLOD over rings in 11.63, and a morph closes a seam where the stitch would have. Kept as reference for a paged ring structure, which is the only consumer left |
| Screen-space footprint → detail handover (mip level or dropped octave, energy into roughness) | D3 — **delivered** (11.35) | any procedural surface a projected or adaptive mesh under-samples at distance |
| World-scale contract for shader-side physical lengths (`waterUnitsPerMetre` off `Sky.world_units_per_km`, the authority the atmosphere already used) | D3 — **delivered** (11.44) | anything whose constants are metres: terrain, POM depth, contact-shadow reach, decal projection |
| A CPU solver published to the shading stage through a std140 block rather than a texture | D3 — **delivered** (11.45, `ShoreFilmBlock`) | any per-column or per-object field a shader needs that a UBO can hold — the ledger's second escape, generalised |
| Sampler-declaration consolidation: N identical textures → one array, one declaration | D3 — **delivered** (11.45), `water_frag` 16/16 → 10/16 | D0, and any program that hits its own ceiling |
| Histogram-preserving stochastic sampling + periodic noise primitives | D5 — **delivered** (11.46) | terrain, any surface seen over tens of repeats |
| Per-pass GPU timing | E4 — **delivered** (11.27) | E5 LOD thresholds, D4, all budget work |

## Cross-track integration contracts

- **`froxel_inject_frag` is the single agreed integration point** for A1's clustered light data, and
  the option has been **taken**: it walks `clusterLights` per cell and accumulates
  `colorIntensity * atten * phase`, so local lights scatter. This was phrased as an open choice —
  "local-light scattering with it, today's sun+spot coverage without it" — long after it was made.
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
   temporal effects; static-jitter paths keep headless byte-deterministic). TAAU alone was to use a PSNR
   threshold (≥32 dB vs native) since reconstruction differs by design -- **that never reached the
   corpus.** `gates.py` contains no PSNR anywhere and there is no `taau` or `render-scale` group among
   its 40; the threshold exists only in spec 11.7. This bullet describes a check nothing runs.
3. Every feature gets a CLI flag in the render app (`parse_args` pattern) + an ImGui toggle
   (`igCheckbox` bound to Engine/PostFX field pattern).
4. New test content needed along the way: `--area-light` CLI flag (A2), ~~cornell-box GLB~~ **(A4:
   shipped as `assets/cornell_box.gltf` plus a `cornell_leak.gltf` variant, both from one generator
   -- `.gltf` with an embedded base64 buffer, matching every other fixture, not `.glb`)**,
   curvature-sweep GLB (B3), bokeh-chart GLB (B5), low-sun fog/cloud goldens (B1/B9/B2).

## Execution workflow

1. **This master plan lives at `docs/aaa-rendering-roadmap.md` and always has**, committed on a new branch
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
- `apps/render/src/render_args.h` — the struct behind those flags; it moves with every one of them
- `cetra/src/water.c` — D3, which turned out to be eleven specs rather than one row

Both of the last two outrank entries already listed here and were absent; counted by commits since
2025, `apps/render/src/render.c` is 228, `cetra/src/render.c` 184, `pbr_frag.glsl` 160,
`postfx.c` 157, `render_args.h` 75, `water.c` 49 — against `tonemap_frag.glsl` at 38 and
`ocean.glsl` at 35. Note that ranking measures "touched", not "touched by roadmap work".

Tier 4 adds three that Tiers 1-3 barely touched (`cetra/src/render.c` is listed twice — once above
for A1, once below for E5, which is two different rewrites of the same file):

- `cetra/src/shadow.c` — C1 caster filtering + the transmittance resolve (the hair exclusion at
  `shadow.c:485` is C1's starting point)
- `cetra/src/render.c` — E5 submission: instanced draws, LOD selection, draw sorting; D1 decal binding
- `cetra/shaders/tonemap_frag.glsl` — E1 dither, E2 LUT (both at the very end of the chain, where the
  colour is already a display-referred scalar)

And a fourth the list never anticipated, because D3 was priced as one row and became ten specs:

- `cetra/shaders/include/ocean.glsl` + `water_frag.glsl` — the surface evaluation and its shading,
  edited by every water spec from 11.32 on, and the second program in the tree to hit its own
  sampler ceiling
