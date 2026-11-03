# Cetra AAA Rendering Roadmap — Master Plan

## Context

Cetra is a C11 forward-PBR engine on a strict GL 4.1 core ceiling (macOS: no compute, no SSBO).
This is the plan for taking it to an AAA look: a tiered roadmap of SIGGRAPH-grade features, each
buildable as fullscreen raster passes, LUTs and probe bakes on GL 4.1, and each given its own spec
and branch before implementation. Its premise was that the gap was global illumination, light-count
scaling and volumetric atmosphere, all architectural, rather than more post effects.

**Status.** Every numbered row in the sequencing tables, Tiers 1-5 and the rows after them, is
closed: built, refused or rejected, some with a leftover their entry names. A finished item below is a line or two naming its spec and the
doc that describes it now. What is still open in this part of the plan:
- **B3.2's other half**: the pre-integrated skin wrap in the area-light and IBL paths.
- **B6**: the moment reconstruction loses a near-opaque blend layer, on the default path.
- **The SSS profile tag through the MSAA resolve**, under Verified architecture facts.
- **A5's bent normal**: computed every frame and read only by a debug view.
- **B11's leftovers**: sub-face slicing of the env re-bake, probe sets under a moving sun, and
  shadow motion missing from the motion vectors.

Open work in Tracks C-F is stated in those entries, and real but unbooked defects under "Known
limitations not booked as items".

**The walls.** Tier 4's items could not each be another gated fullscreen pass; four structural
walls, stated in "The four walls" before Track C, decided what was reachable. Wall 3 is gone (E4),
Walls 2 and 4 mostly (E5, E6). Wall 1, the 16-sampler ceiling of `pbr_frag`, blocks far less than
"16/16" suggests: 16/16 counts declarations at link time, while whether a feature is blocked
depends on which pass reads the texture, its sampler type and how many bytes it needs, and since
spec 11.95 a variant declares only the samplers its features use. Ask those questions before booking
anything against it.

**Read an open entry as a hypothesis.** A sketch written before its code has had load-bearing
claims wrong often enough that each open row is a guess with an effort estimate, not a design.

**User decisions locked in:**
- **Environments first**: GI, clustered lighting, clouds, fog lead; skin and hair follow.
- **Strict GL 4.1, single code path**: no dual GL 4.3 compute variants. A shader may go from
  `#version 330 core` to `#version 400 core` only where genuinely needed (e.g. `textureGather`);
  none has needed to.
- **Hair stays in the roadmap as a late-tier item** (B8 closed it).
- **Just-in-time foundations**: shared infrastructure lands with its first consumer, and the spec
  names which feature owns which foundation piece.

## Verified architecture facts (from code exploration)

The engine as it is now is described in `docs/`: `docs/architecture.md` for the G-buffer, the frame
and the post chain, `docs/shaders.md` for programs, variants and samplers, `docs/lighting.md` for
lights, shadows, GI and probes. This section keeps only the facts that still constrain open work.

**GL 4.1 limits that shape every design.** No compute, no SSBO, no image load/store and no texture
barrier, so a pass that reads what it writes needs a ping-pong or a blit. No `glTexStorage*` (GL
4.2), so textures are allocated with raw `glTexImage*`. Every shader is `#version 330 core`.

### Nothing categorical or positional in an MSAA-resolved channel

An MSAA colour resolve is a box filter (`resolve_color_attachment`, a `glBlitFramebuffer`), so it is
valid only for quantities linear in radiance. Averaging a category produces a different category,
and averaging a position produces a point that exists nowhere.

- **att4's alpha is a categorical SSS profile tag, and this is open.** The average of tag 2 and
  tag 0 is tag 1, a different profile, so a half-covered rim pixel is shaded with the wrong one
  (in `apps/tree`, grass with the leaf profile). `sss-tag` measures and guards it. It is reached
  only with several samples: the headless default and an explicit `--taa --msaa N`. The windowed
  path runs one sample with TAA and is immune.
- **att2's `.z` view depth**, the positional case, is fixed (spec 11.37): it is read from the depth
  buffer, which resolves by selecting a sample.
- **Untested suspects under the same rule**: att2's `.w` roughness (averaging roughness is wrong for
  the reason averaging normals is; the right operation averages the NDF, Toksvig or LEAN, as the
  far-field handover of spec 11.35 does), and att1's `.a` SSR marker, a range whose sign and band
  are categorical and whose magnitude is continuous.
- **Stencil for the tag was built and reverted** (spec 11.37). The SSS passes are fullscreen and
  read one stencil value per pixel, so resolving a per-sample tag keeps one sample and discards
  coverage: a rim pixel 90% covered is dropped if that sample missed. That swaps a monotonic error
  for an arbitrary one, and it broke `sss-scale`, which holds scatter width independent of
  resolution.

The same mismatch exists on the input side (spec 11.38): an MSAA fragment shades once per pixel at a
point that may lie outside the primitive, so on a partly covered pixel every varying is
extrapolated out of the range its vertices bound. `RENDER_MODE_EXTRAPOLATION` measures it on any
asset (a normal or tangent longer than one proves a negative barycentric weight); the tag and depth
channels amplified it into the specks once seen on flowers. **`centroid` was measured and
rejected**: it makes derivatives inexact, and every varying except `VertexColor` feeds one
(`dFdx(Normal)` for specular AA, `dFdx(WorldPos)` for curvature, UV gradients for texture LOD), so
it moved 9 to 22 goldens along the tessellation. The answer to both halves is not to multisample:
one sample with TAA is immune by construction, and the full recipe is in
`specs/11.37-sss-tag-and-depth-resolve.md`.

## Global decision: pbr_frag texture-unit ledger

The current unit map, the sampler ceiling, the declaration gating that returns units per variant,
and the ordered ways to fit a new texture are in `docs/shaders.md`, Texture / Sampler Model. This
document refers to the escapes by the order they were found:

| escape | what it is | `docs/shaders.md`, Fitting a new texture in |
|---|---|---|
| first | ride a declaration whose other consumer provably never runs at the same time (the OIT moments over `sceneColorTex`) | 4 |
| second | put the table in uniform or `const` space; a block holds kilobytes | 1 |
| third | alias a unit idle in the pass that reads it (the cloud shadow map on unit 6) | 5 |
| fourth | fold identical declarations into one array (`water_frag`'s cascades) | 3 |
| fifth | bake the transform over the source so the original is never read (by-example texturing) | 6 |
| sixth | gate the declaration on the variant's feature mask (spec 11.95) | The sampler ceiling |

Becoming a tenant of a texture already declared (material layers and decals in `materialArray`, the
probe columns in the GI atlas) is item 2 there.

**Occupancy, which is what decides whether a unit can be aliased:**
- **Unit 6 is the only unit idle for a whole pass**, and its three tenants are disjoint by pass:
  the cloud shadow map in the opaque pass (spec 11.41), the refraction resolve in the late pass
  (read only where transmission > 0), and the moment atlas in the OIT accumulate. A fourth routing
  on one unit is where it stops being auditable.
- **Units 3, 4 and 8 look free and are not**: they hold per-material maps (clearcoat normal, POM
  height, sheen), so a global texture aliased there would vanish on any draw carrying one.
- **An alias must match the declared sampler type**, since `#define x sceneColorTex` makes `x` that
  `sampler2D`.
- **Unit 14 is shared by the skybox cube and the lighting atlas**, legal because units are per
  program and `pbr_frag` never samples the skybox. The `_Static_assert` chain in `render.c` pins the
  sharing as an equality, so moving either one comes back to this decision.

**Merging the two shadow arrays is not free.** `shadowMaps` and `punctualShadowMaps` are both
`GL_DEPTH_COMPONENT24` arrays but sized independently, so one array would change one of them's
resolution: a quality trade, not the free consolidation water got. Declaration gating is the
cheaper way to win a unit.

What still needs a real unit is in `docs/shaders.md`: light cookies, and sampling the froxel volume
from the transparent pass. Freeing unit 11 by folding irradiance into the GI atlas is D0, refused
in spec 11.85.

## Track A — Direct Lighting & Global Illumination

### A1. Clustered forward light culling (Olsson et al. 2012) — DONE (spec 9.1)
Every local light reaches a fragment through a 16x8x24 froxel grid in std140 uniform blocks, which
replaced the per-node light uniforms; directionals bypass the grid. See `docs/lighting.md`,
Clustered light culling.

### A2. LTC area lights (Heitz/Dupuy/Hill/Neubelt 2016) — DONE (spec 9.2)
Rectangular panels shaded by LTC (tables on unit 7), single-sided. Their shadows are a five-face
cube in the panel's frame (spec 13.27). See `docs/lighting.md`, Lights, and
`docs/shader-subsystems.md`, Area-light shadows.

### A3. Screen-space contact shadows (Uncharted 4-style) — DONE (specs 9.3, 9.4)
A postfx march along the key light, composited in the tonemap beside AO; C5 marches the map-less
local lights through it too. See `docs/shader-subsystems.md`, Contact shadows.

### A4. DDGI-style irradiance probe volume, rasterized (Majercik 2019 adapted) — DONE (spec 9.7)
Rasterized probe captures into an octahedral atlas, which converge and then idle; a world list of
volumes since spec 13.24. See `docs/lighting.md`, GI volumes, and `specs/9.7-ddgi-probe-volume.md`.

### A5. Bent-normal specular occlusion from GTAO (Jimenez 2016) — DONE (11.3, 11.4), then replaced (11.76) and deleted (11.77)
Ambient specular has its own G-buffer attachment and is occluded in a composite before TAA (11.4).
The bent-normal cone was replaced by testing the reflection lobe against the GTAO sector mask inside
the sweep (11.76), and deleted (11.77): collapsing the 32-bit mask to a direction and an aperture
manufactured bands. See `docs/shader-subsystems.md`, Specular occlusion. Spec 11.76 records a failed
attempt to fix the cone's aperture from the bent normal's length; read it before trying again.

**Open: the bent normal is still computed and only debug view 9 reads it.** The consumers once
booked for it (the GI/IBL diffuse sampling direction, SSGI directionality, SSR occlusion) should use
the sector mask instead, which 11.76 measured as the better directional signal. Deleting it was
measured at about 0.13 ms (3.5% of the AO chain, 0.3% of the frame, on `cornell_rooms --no-bloom
--no-ssr` at 1600x1000, which keeps the frame light enough for the timer queries to retire), not
worth the diff for now. The cost scales with AO resolution: measure again before targeting 4K or
when optimising the AO chain.

### A7. Punctual shadow maps — shadows for all four light types — DONE (spec 9.8)
One `sampler2DArray` on unit 15 carries every per-frame perspective map: a spot one layer, a point
light six, an area light five (spec 13.27), from a pool of sixteen. Storage is near-side for every
type, with receiver-plane depth bias (10.3, 10.4). See `docs/lighting.md`, The per-frame punctual
pool.

### A6. Moment shadow maps (Peters & Klein 2015) — DONE (spec 11.22)
Four-moment cascades resolved from the finished depth array, opt-in with `--msm`; PCF and PCSS stay
the default. Their one advantage is no penumbra cap. The blur ships at 0 because four moments leak
through a thin caster when blurred.

## Track B — Atmosphere, Volumetrics, Character & Post

### B1. Froxel volumetric fog (Wronski 2014 / Hillaire 2016) — DONE (specs 9.5, 9.5.1, 11.11, 11.12, 11.39, 11.40)
A camera-frustum froxel volume (inject, integrate, composite), opt-in with `--fog`, carrying local
fog volumes and further media since 11.39. See `docs/shader-subsystems.md`, Atmosphere.

### B2. Volumetric clouds (Schneider/Vos HZD 2015, Nubis) — DONE (spec 11.0)
A ray-marched deck folded into the sky, seen by the env bake so IBL and probes see it, with cloud
shadows on the fog (11.39) and the ground (11.41). See `docs/shaders.md`, Inventory, and
`docs/shader-subsystems.md`, Cloud shadow.

### B3. Pre-integrated skin shading (Penner 2011) — DONE (spec 11.13)
Opt-in per material through `curvature_scale`, a 16-row `const` table in GLSL. **It is inert on
realistic content**: once spec 11.14 made the screen-space scatter resolution-independent, the
angular term fires only where the authored radius exceeds the scatter ceiling, or under `--no-sss`.
Three fixtures carry it (`skin_curvature_fixture`, `skin_area_fixture`, `skin_shadow_fixture`).

### B3.1. Shadow-penumbra scattering (Penner's second LUT) — REJECTED (11.19)
The screen-space blur already scatters light across a cast-shadow edge on skin, because `shadow`
rides inside `sssDiffuse`; on `skin_shadow_fixture` the angular term adds nothing (`--no-skin-preint`
moves 0 px). Reopen only if the scatter ceiling becomes a real constraint again or a scene needs
this under `--no-sss`, and re-run that measurement first.

### B3.2. Skin under an area light — DONE (spec 11.19), one half open
Done: an area panel's diffuse is tapped into the skin buffer as the punctual Lambert term is, so a
softbox now scatters.

**Open: the pre-integrated wrap stays out of the area path**, because it substitutes a widened
falloff for `dot(N, L)` and a rectangle has no single `L`. The hole is smaller than it sounds:
`ff.x` already integrates the clamped cosine over the rectangle, so a wide panel wraps
geometrically, and what is missing is the widening from transport. The IBL path has the same gap.
Both want a representative direction and a solid-angle-aware width. Given B3 is inert on realistic
content, the payoff is small. **Depends on:** A2, B3.

### B4. TAAU temporal upscaling (Karis-style) — DONE (specs 11.7, 11.8)
`--render-scale` 0.5-1, a separate upscaling resolve, render-res and post-res buffers, changeable at
runtime. No per-frame dynamic resolution. See `docs/architecture.md`, TAA and the sample count.

### B5. Bokeh depth of field (Guerrilla-style gather) — DONE (spec 11.6)
Near and far gather fields with an N-gon kernel (`--dof --dof-blades N`). See `docs/shaders.md`,
Inventory.

### B6. Moment-based OIT (Münstermann 2018) — DONE (spec 11.17), one defect open
Four power moments plus optical depth, on by default with OIT; `--no-oit-moments` keeps the WBOIT
weights. The moments ride the refraction sampler through a `#define`, which caps them at four
floats: six or eight moments are a re-plan, not a tweak. See `docs/architecture.md`, Render Pipeline.

**Open: a near-opaque blend layer renders far too dark.** An ALPHA_BLEND pane at alpha 1.0 renders
at about a fifth of its value (38.9 against 180.2, exactly 180.20 under `--no-oit-moments`); at
alpha 0.5 the loss is 4.4%. `MBOIT_OVERESTIMATION` charges a lone layer a quarter of its own
absorbance while `mboitAbsorbance` grows without bound as alpha approaches 1, so the weight
collapses. It is on the default path and hits any near-opaque blend surface: a window pane, a
painted panel, a decal card authored BLEND rather than MASK. No arm sees it: the OIT arms read a
card stack at alpha 0.15, and `oit_sphere_moments` is the only golden on this path.

### B7. Lens flare / cinematic finishing — DONE (spec 11.21)
A ghost chain read off the bloom pyramid, plus lateral chromatic aberration in pixels at the
corner; both off by default. Starburst and a dirt mask are not built: the mask needs an authored
texture and a sampler binding.

### B8. Physically based hair (Karis/Marschner) — CLOSED, split (spec 11.20)
Shipped: `tools/gen_hair_flow.py` derives a strand map from a hair atlas, and it rides the existing
anisotropy slot as a coherence-weighted doubled angle, so the GGX highlight stretches along the
grain. Rejected: the Marschner/Kajiya-Kay fibre lobes. They replaced the microfacet term while still
flowing through `NdotL` on the card normal, and cards carry no per-texel facing, so no setting
looked right; do not reopen them on raiden.

### B9. Aerial perspective (Hillaire 2016) — DONE (spec 9.6)
A 32³ volume rebuilt per frame, combined with the fog in one composite, with the fog's sun and
ambient published by the sky. See `docs/shader-subsystems.md`, Atmosphere.

### B10. Night sky — stars — DONE (spec 11.79)
A procedural star field and Milky Way in `sky_radiance.glsl`, off by default, kept out of the env
and IBL path. See `docs/shader-subsystems.md`, Stars.

### B11. Day/night cycle — DONE (spec 11.81)
A time-sliced env re-bake driven from the engine loop, so a moving sun no longer re-bakes everything
each frame. See `docs/shader-subsystems.md`, Day/night cycle.

**Left open:**
- **The per-frame peak is one mip-0 face** (4.74 ms at 800x500) and needs sub-face slicing to go
  lower.
- **Probe sets do not follow the sun**: a probed interior under a cycle holds its capture-time
  lighting.
- **Shadow motion is not in the motion vectors**, so creeping shadows smear slightly under TAA;
  negligible at realistic rates, visible in a fast time-lapse.

### B12. Night-sky floor — DONE (spec 11.80)
Airglow, zodiacal light and integrated starlight as one term in the sky LUT, reaching the env cube
so the ground is lit at night; its level is a look-calibrated constant, since auto-exposure never
brightens. See `docs/shader-subsystems.md`, Night floor.

### B13. The moon — DONE (spec 11.82)
A disc with phase and earthshine plus a casting directional light that follows phase and altitude.
See `docs/shader-subsystems.md`, The moon.

### B14. Purkinje / scotopic shift — Effort S/M — **DONE (spec 11.83)**

Rod vision in dim light after Kirk & O'Brien 2011: colour drains, the frame shifts blue and reds
go near-black. Spliced into `sceneToToned` before the tone curve, and driven by the metered
luminance, which is measured whenever the shift is on, manual exposure included. See
`docs/shader-subsystems.md`, Purkinje / scotopic shift.

### B15. Water at night — Effort M — **DONE (spec 11.84)**

Water's in-scatter is `scatterAlbedo` times the light reaching it plus an absolute `scatterGlow`,
and its key directional is the brightest available one rather than the sky's sun by name, so a
moonlit sea has its glitter path and caustics. See `docs/shader-subsystems.md`, Water, and the
`water{}` keys in `docs/cli-reference.md`.

## The four walls (Tier 4 preamble)

Tiers 1-3 each added another gated, lazily allocated fullscreen pass. Four walls stood between that
renderer and the next tier; three are down and the first is narrowed.

**Wall 1 — `pbr_frag` is sampler-saturated (16/16) — narrowed (spec 11.95).** The uber-shader
declares all sixteen; a variant declares only what its features read (`pbr-0` declares 12). What
blocks a feature is not the declaration count but whether a unit is free in the pass where its read
happens, and whether the data needs a texture at all: a table goes in a uniform block, per-vertex
data in an attribute, and a picture can often be a layer of an array already declared. Still
blocked: light cookies, and sampling the froxel volume from the transparent pass, a `sampler3D` in
the one pass where the refraction resolve is live. See `docs/shaders.md`, The sampler ceiling and
Fitting a new texture in. `water_frag` has its own ledger, 15 of 16 (`docs/shaders.md`, Other
programs' ledgers).

**Wall 2 — geometry submission does not scale — removed (specs 11.28 / 11.29, ordering 11.30).**
Instancing, LOD chains and per-cascade shadow culling (E5), and draw ordering (E6).

**Wall 3 — the GPU is unmeasured — removed (spec 11.27).** Per-pass `GL_TIME_ELAPSED` scopes and a
HUD table (E4).

**Wall 4 — the opaque pass is unshielded — removed (specs 11.30, 11.31, 11.98).** Front-to-back
opaque ordering ships on; a depth prepass was built and ships off, because a full extra geometry
pass cost more than the shading it saved everywhere in the corpus (E6). Occlusion culling is E7.
The lesson that carries: the cost was fragments, not triangles or draws, and it went unseen while
only submission was measured.

## Track C — Lighting completeness

Every item is built, and none cost a `pbr_frag` sampler unit.

### C1. Translucent shadow maps — Effort M → **L, DONE (spec 11.26)**
Deep opacity maps behind `--translucent-shadows`, on the cascades only: hair casts a partial
shadow, glass no black one. See `docs/lighting.md`, Cascades.

**Deferred, with reasons:**
- the four-moment reconstruction: it pairs MSM's linear warp with MBOIT's, a configuration in
  neither paper, and it is ill-conditioned on thin casters, which is where hair lives;
- hair receiving its own shadow: it draws card-shaped streaks;
- the punctual path: unit 15 has no transmittance home, and the extra layers would push every
  punctual map to its smallest size;
- coloured transmittance: it breaks the scalar contract at three sites.

### C2. Emissive geometry → LTC area lights — Effort S/M — **DONE (11.49)**
`--emissive-lights` fits a rectangle to each emissive mesh and registers it as a `LIGHT_AREA`. Off
by default, because most emissive materials in practice are a flat bright colour rather than a
lamp; a material declines with `emissiveLight: "off"`. See `docs/lighting.md`, Derived area panels
and Captures, which also hold the fit's rules.

**Open:** a derived panel inherits `cast_shadows` false, so it lights through walls unless a
`light_overrides` entry turns its shadow on, and the contact march (C5) skips area panels.

**Declined:** the specular double count, where a reflection probe photographs the glowing quad the
panel also integrates analytically. It is small (1.04) and every fix costs more than it buys; a
gate arm bounds it.

### C3. IES photometric profiles — Effort S — **DONE (spec 11.57)**
Full LM-63 profiles, all four symmetry classes, as a normalised table in its own uniform block on
binding 6, replacing a spot's analytic cone. See `docs/shader-subsystems.md`, IES profiles.

### C4. Clustered specular probes — Effort L — **DONE (spec 11.70)**
A set of reflection probes blended per fragment through a froxel mask, stored as columns of the
lighting atlas on unit 14. See `docs/lighting.md`, Reflection probes.

### C5. Screen-space shadows for local lights — Effort S — **DONE (spec 11.56)**
The contact march covers every clustered point and spot light with no shadow map, folded by each
light's contribution. See `docs/shader-subsystems.md`, Contact shadows. Derived area panels are
still not marched (C2).

## Track D — Surfaces & environment

A foundation lands with its first consumer, never ahead of it.

### D0. Free two `pbr_frag` sampler units — Effort M — **REFUSED on investigation (11.85)**

Refused: no booked feature needs a freed unit, and the obvious fold is unsound. `pbr_frag` reads
`irradianceMap` only when there is no GI volume, and `gi_volume_bind` binds no atlas in that case,
so folding irradiance into the GI atlas fails exactly where it must serve, unless IBL is coupled to
a subsystem that is off. 11.85 wanted the unit to split the material array by colour and measured
that the array was the wrong prize. What returned units instead is 11.95's declaration gating (Wall
1).

If the froxel read in the transparent pass is ever scheduled, these are the candidates examined,
with their costs:
- **A standalone octahedral irradiance texture on unit 14 whenever GI is off**, the cheaper shape
  of the fold. An 80x80 tile matches the 32² cube's angular density at the same memory. It cannot
  be 0 px against the cube (a resample), so it needs a stated tolerance and a golden re-bake, and
  a wrong seam gutter shows as a faint cross in the ambient term on every surface.
- **`heightTex` (unit 4) as a layer of the material array.** Memory is neutral on `pilot`, but POM
  would march at the array's 2048 cap instead of 4096. Three preconditions: the frame loop builds
  the array (`material_texture_array_ensure_built`) before it resolves heights
  (`heights_ensure_resolved`) and both run once, so the order must swap and the rebuild re-arm;
  `resolve_height_maps` loads synchronously after the streaming path has gone idle; and the array's
  one canonical size would charge a large height map to every layer, so fold only when the height
  map is not the largest texture. Unit 4 also carries the rain's relief and the virtual-texture
  page pair.
- **Unit 6 shared with a consumer that is provably absent whenever refraction or MBOIT runs**: a
  conditional slot, not a freed one, and a broken proof shows as a wrong image, not an error.
- **Merging the two shadow arrays (10, 15)**: they are sized independently, so one would change
  resolution.

Refused outright: replacing `brdfLUT` (13) by an analytic fit, since its `.b` carries the sheen
E-LUT for KHR conformance; and unit 9 (Charlie), which is conditional on sheen, not free.

### D1. Clustered decals — Effort L — **SHIPPED (spec 11.73)**
Box decals culled into the light grid, their images layers of the material texture array on unit
2, so they reach the late pass and probe captures. See `docs/shader-subsystems.md`, Clustered
decals. Ground marks made of a layer the surface already carries are roads (spec 11.68,
`docs/shader-subsystems.md`, Roads).

### D2. Local fog volumes + cloud shadows — SHIPPED as specs 11.39 + 11.41
Authored `fogVolumes[]` boxes injected into the froxel volume, and a cloud shadow map read by the
froxels, `pbr_frag` (an alias of unit 6 in the opaque pass), the shadow catcher and water's
caustics. See `docs/shader-subsystems.md`, Atmosphere and Cloud shadow.

**Open:** the froxel accumulator's first active frame is not stable run-to-run when the pass is
armed by a fog volume rather than by `fog_enabled`. Unexplained.

### D3. Tessellated water — SHIPPED as specs 11.32 + 11.33 + 11.35, and NOT tessellated
Gerstner and Tessendorf FFT waves, Beer-Lambert absorption, caustics and foam from the surface
Jacobian, shoaling against a bed, an underwater medium and a `.cscn` block, on a projected grid
(Johanson 2004): a fixed NDC lattice, one draw, each vertex a ray onto the still plane. See
`docs/shader-subsystems.md`, Water.

**Refused: tessellation, and a geometry clipmap.** Patch density is a screen-space sampling
problem with screen-space answers, so tessellation was never needed; the clipmap (11.33) could not
reach the horizon.

Later work on the same surface, by spec: 11.43 (`parse_env` reports a key nothing read; gate boxes
derived from the glTF), 11.44 (lengths in metres, `beach_fixture`), 11.45 (the shore chain and the
baked foam pattern), 11.46 (by-example texturing, D5 below), 11.47 (whitecaps that break, coverage
calibrated against Monahan and O'Muircheartaigh) and 11.48 (two wave trains). Their rules are in
`docs/shader-subsystems.md`, Water.

Two instruments from this series are worth reusing. `--water-foam-debug` writes a binary selection
mask read at nadir, where a pinhole images a plane affinely, so pixel fraction is areal fraction
exactly with no crop box. And the per-cascade probe (`--water-fft-probe`) reaches the spectral
seeding, which both water goldens, being Gerstner, never do.

**The tessellation stage is still unspent.** D3 shipped a projected grid and D4 a CDLOD quadtree
whose morph is ordinary vertex work. POM silhouettes is the candidate first consumer, and inherits
no pipeline from here.

**Open items this entry still owns.** Numbers are stable: a closed item keeps a one-line stub so
the ones after it do not shift under a citation.

Five are open, in three groups. Geometry: 4's second half and 1, both about the projector covering
a camera the waves reach; 4 ranks first. The gate corpus: 3 and 6, the same finding in two places,
arms that structurally could not see a defect; this group is the more urgent. And 2, precision at
distance, which has no consumer and whose fix is engine-wide.

1. **The projector aims at the still plane, not the displaceable slab.** Each lattice ray meets the
   flat plane, where the wave field is sampled, and the surface then displaces, so a wave can lift
   geometry into view from XZ the lattice never sampled. It bites when the eye is within about one
   wave height of the surface: wading in `apps/tree --player`, a boat deck, a camera at sea level.
   It is why `water-submerged` sits on a shallow Gerstner framing rather than the FFT one it was
   written for.

   A displacement bound now exists: the seeded spectrum's height and slope variance
   (`--water-fft-probe`), and Gerstner's analytic `amplitude * sum(0.44^i)`, i<4, = 1.7188.

   **Offsetting the plane by that bound does not work** (tried and reverted in 11.42).
   `OCEAN_OVERSCAN` compensates horizontal displacement as a fraction of the lattice's world
   footprint, and the offset shrinks the footprint, so a wading framing opens a wedge of background
   at the frame edge. Offsetting by the full bound puts the plane above a camera inside the slab,
   the `OCEAN_MIN_EYE_HEIGHT` clamp pins the clearance at a hair, and the near lattice collapses.
   The fix is Johanson's two-plane construction or an overscan that scales with the offset, both
   larger than a one-line change.
2. **Water positions are absolute, so precision follows distance from the world origin, not from
   the camera.** The same sea at 960 and at 307,200 units out differs over nearly half the frame, though
   the spectral field is periodic across the 960-unit cascade LCM; a flat-surface control puts it
   in the field lookup, not the view transform. `p = camPos.xz + rd.xz * t` quantises to about 0.03
   units at 3e5, 6% of the medium band's texel. No consumer today: nothing in this tree exceeds
   about 1,000 units. The correct fix is camera-relative rendering for everything, since precise
   water beside jittering terrain is no better; the recipe is in 11.35's phase 3 as-built.
3. **The water gate corpus has a structural blind spot.** Every water arm reads `water_fixture` or
   `beach_fixture`, both small scenes, and a defect whose size scales with horizon distance over
   the far plane is sub-degree on them: 11.35's shoreline test read a cleared depth buffer as bed
   and discarded 40.6% of an `apps/tree` frame with every water arm and golden green.
   `apps/forest --water` and `apps/tree` inherit the whole surface stack with no pixel coverage.
   What is missing is a scene whose far plane is small relative to the horizon. The transform half
   is closed: `water-fft-impulse` inverse-transforms two single modes through the real stages and
   compares against the closed form (two modes, because a constant field is invariant under the
   conjugation a broken twiddle drops).
4. **A camera at the waterline: the geometry half is open.** The shading half is one per-pixel
   `seenFromBelow` in `water_frag`, held by `water-waterline`. The projector still picks its side
   per frame (`above = waterCamPos.y >= waterLevel` in `ocean.glsl`), so the lattice covers one
   side of the eye plane when a straddling camera needs both, and wading shows a straight boundary
   where the surface crossing the eye should be a wavy line. Item 1's plane offset does not fix
   this, and spanning the lattice across both sides of the horizon row dilutes rows into the
   `OCEAN_FAR_DIST` cap. Ranked above item 1: it fires whenever a walker enters the water.
5. ~~The water shader does not know how big a world unit is.~~ Closed by 11.44.
6. **No arm covers breaking whitewater.** `water_fixture` is Gerstner at amplitude 0.06, which never
   reaches the depth-limited criterion, and `beach_fixture` reaches it only through arms written for
   shoaling and the shore band, so shader edits to the surf path leave every water arm unmoved. A
   `water-breaking` arm would assert that the selection fires over a bed and not over open water,
   and that it is a band rather than a filled region. This is item 3's blind spot in a second place,
   which argues for treating it as a property of the corpus rather than of a fixture.
7. Closed: `beach-shoreline` took a brightness argmax that the dry crown competed in; it now takes
   the argmax of the bed's difference (`docs/verification.md`). An argmax gate is meaningful only
   when nothing else in its search range can win, which is a property of the scene.

**Refs.** Tessendorf, *Simulating Ocean Water* (SIGGRAPH 2001 course); Johanson, *Real-time Water
Rendering: Introducing the Projected Grid Concept* (2004) for the mesh that ships; Hasselmann et al.
(JONSWAP, 1973) and Bouws et al. (TMA, 1985) for the two trains' spectrum; Monahan &
O'Muircheartaigh (1980) for whitecap coverage.
**Owns:** the water subsystem, its bed-provider seam (`WaterHeightFn`, which `apps/forest`'s terrain
satisfies directly), the CPU wave query (`water_probe_set` / `water_probe_result`), and the shore
chain.

### D4. Terrain — Effort XL — **SHIPPED, both halves: CDLOD (11.63) and STREAMING (11.69)**

A CDLOD quadtree with per-vertex morphing (11.63) and a field streamed off disk in windows (11.69):
`docs/terrain.md`, The quadtree and Streaming. Strugar's CDLOD was chosen over Hoppe's clipmap
rings; the water clipmap D3 built and removed, with its T-junction stitch, is at commit `8d04658`
if rings are ever wanted.

What is left is not D4's. The splat is one texture over the whole domain, capped at 2048 under
streaming, and its pyramid is D10's. **Per-tile erosion is refused**: the domain is a closed basin
by construction, so erosion runs offline at top resolution and the result is streamed.

### D5. By-example stochastic texturing — SHIPPED as spec 11.46, unbooked before it
Heitz & Neyret's histogram-preserving blend, opt-in per material (`stochastic_scale`); only
`apps/tree`'s sand takes it. It costs no sampler unit, by baking the transform over the original
(`docs/shaders.md`, Texture / Sampler Model).

- **The histogram transform is not optional.** The variance-preserving blend is exact only for a
  Gaussian input, and a texture built from a sine train, fBm and Worley is not near-Gaussian.
- **Roughness is not done**, deliberately: it comes from the shared material array, so it would
  mean transforming a packed multi-material `sampler2DArray`, the largest and least valuable of the
  three.
- **Its cost is unmeasured**: six fetches on the ground where there were two.

**Refs.** Heitz & Neyret, *High-Performance By-Example Noise using a Histogram-Preserving Blending
Operator* (HPG 2018).

### D6. Heightfield backend for terrain — Effort S/M — **DONE (spec 11.59)**
`terrain_height_at` takes its height from the fbm or a stored `TerrainField` behind one pure
function, so every consumer kept working (`docs/terrain.md`, The height is one pure function). A
field carries its own height range, since `TerrainParams.height` is the fbm's amplitude and inert
once a field exists.

### D7. Hydraulic + thermal erosion bake — Effort M — **DONE (spec 11.59)**
Mei's virtual-pipes erosion on the CPU, Eulerian, double-buffered and bit-identical at any thread
count, producing the flow, deposit and wear masks (`docs/terrain.md`, Erosion). **Droplet erosion
is refused**: droplets write to shared cells, so the result depends on scheduling. If 4096² and up
ever forces the GPU, `_water_fft_transform` is the tree's N-stage ping-pong template.

**Refs.** Mei, Decaudin & Hu, *Fast Hydraulic Erosion Simulation and Visualization on GPU* (2007).

### D8. Heightmap + mask import / export — Effort S — **DONE (spec 11.59)**
`.r16` read and write and 16-bit PNG read, outside `texture.c` (`docs/terrain.md`, Heightmap
files). The bake writes what the importer reads; a round trip carries the masks as well as the
height.

### D9. Terrain material layers — Effort M — **DONE (spec 11.60)**
N layers as further tenants of the material texture array: no new sampler, unit or program
(`docs/shader-subsystems.md`, Layered surfaces; `docs/terrain.md`, What the ground is made of). A
second surface program was rejected: a terrain's lighting model is the ordinary PBR one, and a
second program would duplicate or first extract the shading inlined in `pbr_frag`.

### D10. Virtual-texture compositing — Effort XL — **stages 1 and 2 DONE (11.66, 11.67); first CONTENT in 11.68**
What UE does: composite the layers once into a cached virtual texture, sample one albedo and one
normal at runtime, and let meshes blend into the same cache.

Built (`docs/shader-subsystems.md`, Composite cache and Roads):
- **Stage 1 (11.66)**: a domain-wide macro composite pair plus one triplanar detail tap of the
  dominant layer at runtime, `3 + 2A` taps whatever the layer count. A composite is a macro cache
  at any affordable density, so the grain has to be restored at runtime.
- **Stage 2 (11.67)**: a guttered page atlas, a page table in a uniform block, and a depth-tested
  vote pass read back through a PBO ring at fixed latency.
- **Roads (11.68)**: a procedural override of the splat weights inside `sampleLayeredSurface`, so
  the bake and every consumer inherit them.

On today's content the pages are an identity against the fallback; what they resolve that the
fallback cannot is four times narrower than a fallback texel.

**Open: content.** What a weight override cannot express: decals that are not made of the ground's
own layers, and width that varies along a course. Then the general-mesh era, where feedback stops
being a refinement and becomes the only correct source. The terrain splat's pyramid is D10's too
(see D4).
**Depends on:** D9 (shipped); D4's streaming (shipped) for its source data.

### D11. Large-world origin rebasing — Effort M/L — **DONE (spec 11.62)**
Origin shifting rather than fp64: `scene_set_world_origin` schedules a shift, the engine applies it
at the frame top, and `Scene.origin_shift_distance` re-centres once the camera drifts
(`docs/architecture.md`, The per-frame loop). Everything linear takes the delta; everything that
hashes or tiles is handed the authored position back through `include/world_origin.glsl`. The
outermost cascade is fitted round `ShadowSystem.scene_center`.

`apps/forest`'s regions (11.63) own their props and collider and load against two anchors, but in
memory; nothing pages them off disk.

## Track E — Image finishing & the perf floor

### E1. Output dither / debanding — Effort S — **DONE (spec 11.24)**
A static TPDF dither, last in the tonemap's finishing stack (`include/dither.glsl`; `docs/shaders.md`,
Rules that span files). Debug views return before it and are undithered, which is correct for data.

**Refs.** Mikkelsen, *Banding in Games: A Noisy Rant* (2010); Jimenez, interleaved gradient noise
(2014).

### E2. 3D LUT colour grading — Effort S — **DONE (11.58)**
A `.cube` reader and a `sampler3D` on the tonemap's unit 11, applied after `displayEncode`,
tetrahedral by default (`docs/shaders.md`, Rules that span files). **A log, pre-tonemap LUT was
declined**: no off-the-shelf `.cube` is authored against a log window we invent, it would replace
the tonemap rather than compose with it, and it would reach the GI debug view.

### E3. Histogram auto-exposure — Effort M — **DONE (11.52)**
A 128-bin gather histogram over the 64² measure target and a percentile-clipped reduce, with
metering modes, EV bounds and split adaptation rates (`docs/shader-subsystems.md`, Tonemap /
exposure). The percentiles default to 0.70/0.95: a meter that includes the black background is
measuring the background.

**Refs.** Lagarde & de Rousiers, *Moving Frostbite to Physically Based Rendering* (SIGGRAPH 2014
course), the exposure section.

### E4. GPU timer queries + per-pass HUD — Effort S — **DONE (spec 11.27)**
Per-pass GPU timing behind `--profiler`. `GL_TIME_ELAPSED` works on this driver and
`GL_TIMESTAMP` returns 0, so scopes are flat, not nested.

### E5. Instancing + LOD + sorted submission — Effort L — **DONE, two limbs of three (specs 11.28 / 11.29)**
Geometry dedup by `aiMesh` index, one flattened draw list, per-cascade shadow culling, instanced
draws through a std140 `InstanceBlock`, and LOD chains built at import as index ranges in one EBO
(`docs/architecture.md`, The draw list). The third limb, material-sorted submission, moved to E6.

In the camera lane, scatter order decides batching (the batcher joins only consecutive items, so
props are Morton-ordered within a prototype) and LOD splits runs, since the batch key is
`(mesh, lod)`. The shadow pass regroups its own casters and pays neither. `forest-shadow-lod` holds
the level half; nothing holds the order half, which `forest-order` could assert, since it already
renders both scatter orders.

### E6. Depth prepass + opaque draw ordering — Effort M — **DONE (spec 11.30)**
Front-to-back opaque ordering on by default (`--no-sort-opaque`), a depth prepass off by default
(`--depth-prepass`) that takes masked geometry through its own program's `depthOnly` mode (11.31),
and one shared object-position chunk with `invariant gl_Position`. On `apps/forest` the sort and the
prepass are substitutes, the sort the cheaper (opaque 306 ms with neither, 169 with the sort, 173
with the prepass), and no scene in the corpus has a configuration where the prepass pays.

**Open: E5's third limb, material-sorted submission.** The sort key puts the depth bucket above
material (`docs/architecture.md`, The draw list), so material is ordered only within a bucket; grouping by material needs
material above depth, a different sort to measure against overdraw. With the prepass off,
front-to-back is the only lever on overdraw, so the two compete on one key.

| objective | wants | costs |
|---|---|---|
| batch contiguity (E5, shipped) | identical `(mesh, lod)` adjacent | Morton order, camera-independent |
| material switches (E5's open limb) | same program/material adjacent | fights both others |
| front-to-back (shipped) | near before far | camera-dependent, rebuilt per frame |

**Depends on:** E4 (hard: the claim is a measurement).

### E7. Occlusion culling — Effort L — **DONE (spec 11.98), and both of this row's premises fell**
CPU masked software occlusion culling of authored occluders, from this frame's camera, so there is
no query, readback or frame of latency (`docs/architecture.md`, Scene passes). It pays where an
author places occluders, not scene-wide: on the portal fixture, opaque 7.6 ms culled against 8.7
not.

### E8. Fixing the wind cull — Effort S — **DONE (spec 11.53)**
Swaying and skinned meshes are cullable: the wind's offset is bounded in closed form, a skinned
mesh by per-bone bind-space boxes unioned through the pose, and each pass builds its `CullView`
from what it draws with, in place of the deleted `DRAW_UNBOUNDED` flag. `--wind-bound-probe`
(11.54) drives `windOffset` itself through transform feedback, and `cull-bound` holds the measured
travel under the bound. **A CPU port of `windOffset` was rejected**: it reads straight through a
term added to the GLSL, which is the failure the probe exists for.

## Track F — The asset pipeline

*Every other track asks what the renderer does; this one asks how content reaches it.
`docs/game-engine-roadmap.md` owns what is not the graphics pipeline (audio, animation, IK,
gamepad, save, UI) and books neither row below.*

### F1. Block-compressed textures — Effort M — **DONE (spec 11.85)**

BC5 for normals and BC4 for single-channel masks by default, DXT for colour under
`--texture-compress-colour`, a CPU-built mip chain (`docs/shaders.md`, Texture / Sampler Model,
Block compression). BC7 is unreachable on this driver. The material texture array stays
uncompressed: its packed maps and splat hold independent quantities per channel, which BC4 and BC5
cannot, and it holds no normal maps.

### F2. An offline asset cook — Effort L — **DONE (spec 11.99)**

Built as a transparent content-addressed cache over the deterministic startup bakes (`cook.c/h`,
after Unreal's DDC) plus a `--cook` pre-warm verb, rather than as a build step. The key is the
artefact's identity, so a stale artefact is unfindable rather than refused. `docs/architecture.md`,
"The derived-data cook", describes it.

## Sequencing — tiers & rationale

Every row below is built, refused or rejected; its track entry above says which, and where the
engine's docs describe it now. The row numbers stay because other documents and
`scripts/gates.py` cite them.

**Tier 1 — environments (rows 1-8, all done):** 1 A1 clustered forward (9.1); 2 A2 LTC area
lights (9.2); 3 A3 contact shadows (9.3); 4 B1 froxel fog (9.5); 5 B9 aerial perspective (9.6);
6 A4 DDGI volume (9.7), whose light-leak gate was left unbuilt; 7 A7 punctual shadow maps (9.8);
8 B2 volumetric clouds (11.0).

### Interlude — correctness & conformance series (9.9 → 10.x), shipped between Tier-1 items 7 and 8

Specs 9.9 to 11.2, each driven by a defect found in use rather than by a row here: photometric
lights and the PBR Neutral fix (9.9, 10.0), exposure as a working-space contract (10.1, 10.2),
shadow quality (10.3-10.6), KHR sheen and specular conformance (10.7, 10.7.1, 10.8), temporal
stability (10.7.2, 10.8.1), the SSR trace rewrite (10.9), FBX import unit scale (11.1) and the
fixed-step headless clock (11.2). One rejected alternative from 10.9: an SSR ray that runs out of
reach fades by budget consumed rather than falling back to the environment, since a catcher
mirroring the sky prints as a glowing pool.

**Tier 2 — image quality & performance (rows 9-12.1, all done):** 9 A5 split ambient specular
(11.3, 11.4; the bent-normal cone was replaced in 11.76 and deleted in 11.77); 10 B5 bokeh DoF
(11.6); 11 B4 TAAU (11.7); 12 B3 pre-integrated skin (11.13, inert on realistic content since
11.14 fixed the blur it compensated for); 12.1 the SSS blur as a world width through a scale-space
pyramid (11.14).

**Tier 3 — polish (rows 13-19):**
- 13: SSS ran in no scene that built its materials in code; fixed at the material registry (11.14).
- 14 B3.1 shadow-penumbra scattering: **rejected (11.19).** A cast shadow across skin already
  scatters, because the shadow rides inside the SSS diffuse the blur carries, and Penner's angular
  term moves 0 px. Reopen only if the pyramid's ceiling binds again or a scene needs it under
  `--no-sss`.
- 15 B3.2 skin under an area light: done (11.19), the area diffuse now feeds the skin buffer. The
  pre-integrated wrap under an area light, which has no single `L`, is still open, as is the same
  gap for IBL.
- 16 B6 moment-based OIT: done (11.17), on by default; 11.18 drew the shadow catcher before the
  transparent pass. Six or eight moments would need a third fragment output location, and GL 4.1's
  guaranteed eight are spoken for.
- 17 B7 lens flare: done (11.21), a ghost chain off the bloom pyramid and lateral chromatic
  aberration, both off by default. A lens dirt mask is unbuilt; the starburst came later as
  diffraction glare (13.4).
- 18 A6 moment shadow maps: done (11.22), opt-in `--msm`, resolved from the finished depth array
  and replacing unit 10. The three unifications reviews raise against it (a shared prefilter
  helper, a shared moment kernel, folding the PCSS/MSM switch) are declined in spec 11.22.
- 19 B8 hair: closed, split (11.20). The strand map rides the anisotropy slot. The R/TT/TRT fibre
  lobes were built and deleted: the lobe replaced the microfacet term without its normalisation and
  energy compensation while still passing through a surface integrand, and hair cards carry no
  normal map to face it by.

**Tier 4 — completeness, authoring & the perf floor (rows 20-54):**
| # | Item | Effort | Status |
|---|------|--------|--------|
| 20 | E1 Output dither | S | **DONE (11.24)**, on by default. |
| 21 | C1 Translucent shadows | M→**L** | **DONE (11.26)**, as deep opacity maps. |
| 22 | D2 Local fog + cloud shadows | M | **DONE (11.39 + 11.40 froxel, 11.41 surface).** The surface half aliases unit 6, idle in the opaque pass. |
| 23 | C2 Emissive → area lights | S/M | **DONE (11.49)**, off by default (`--emissive-lights`): 30 of the corpus's 32 emissive materials are flat unlit colour, not lamps. |
| 24 | E4 GPU timer queries | S | **DONE (11.27).** |
| 25 | C3 IES profiles | S | **DONE (11.57)**, full LM-63 in a uniform block, replacing a spot's cone; `--ies-profile`. |
| 26 | C5 Screen-space shadows for local lights | S | **DONE (11.56)**, marching every clustered punctual with no shadow map. Area panels are not marched: the only direction to one is its centre, a different approximation from the LTC integral. |
| 27 | E3 Histogram exposure | M | **DONE (11.52)**, a 128-bin histogram with a percentile-clipped reduce. |
| 28 | E2 3D LUT grading | S | **DONE (11.58)**, applied after `displayEncode`, tetrahedral by default. Log or pre-tonemap LUTs are declined: no off-the-shelf table is authored against a log window of ours, and one would replace the tone map rather than compose with it. |
| 29 | C4 Clustered specular probes | L | **DONE (11.70).** |
| 30 | E5 Instancing + LOD + sorting | L | **DONE (11.28 instancing, 11.29 LOD)**; sorting moved to E6. |
| 31 | E6 Depth prepass + opaque ordering | M | **DONE (11.30 + 11.31).** Opaque ordering is on; the prepass ships off, since a full extra geometry pass costs more than the shading it saves on this content. |
| 32 | D0 Free two sampler units | M | **REFUSED (11.85).** The irradiance fold is unsound: `pbr_frag` reads `irradianceMap` only when GI is off, and `gi_volume_bind` leaves the atlas it would fold into unbound exactly then. Declaration gating (11.95) returns units per variant instead. The one consumer that cannot dodge the ceiling, the froxel volume sampled from the transparent pass, is unbooked (`docs/shaders.md`, "Fitting a new texture in"). |
| 33 | D1 Clustered decals | L | **SHIPPED (11.73)**, decal images as tenants of `materialArray`. |
| 34 | E8 Fix the wind cull | S | **DONE (11.53)**: skinned and swaying meshes cull against closed-form bounds in a per-pass `CullView`. |
| 34b | E9 One sample means one sample | M | **DONE (11.34).** |
| 35 | ~~D3 Tessellated water~~ | — | **SHIPPED (11.32, 11.33, 11.35)** as a projected grid, with no tessellation stage. Later water work is under D3 and in `docs/shader-subsystems.md`, "Water". |
| 35b | D5 By-example texturing | S/M | **DONE (11.46)**, unbooked: the transform is baked over the original texture, so it costs no unit. |
| 35c | Unbooked, shipped anyway | — | 11.50 (`foliage_shadows` as a material row) and 11.51 (the ivy arcade's UV1 wind, the `wind-uv` group). |
| 36 | D4 Terrain streaming | XL | **SHIPPED**, CDLOD (11.63) and streamed windows (11.69); `docs/terrain.md`. |
| 37 | E7 Occlusion culling | L | **DONE (11.98)**, CPU masked occlusion of authored occluders, current-frame and conservative. |
| 38 | E10 Integer-bit hashes | ~~S~~ M/L | **REJECTED.** The diagnosis is right (the sin-fract hash asks 33 to 42 bits of a 24-bit float), but the benefit is cross-driver reproducibility on a one-driver machine, and no consumer has a correct pattern to regress against. Revive it if a second GPU is rendered against or an artefact is shown (stochastic tiling far from the origin, trees sharing a sway phase); a `--hash-probe` through transform feedback would be the instrument. See `docs/shaders.md`, "Hashes". |
| 39 | D6 Heightfield backend | S | **DONE (11.59).** |
| 40 | D7 Erosion bake | M | **DONE (11.59)**, bit-identical at any thread count; droplet erosion is refused on that test. |
| 41 | D8 Heightmap import/export | S | **DONE (11.59)**, `.r16` and 16-bit PNG, masks included. |
| 42 | D9 Terrain material layers | M | **DONE (11.60)**, as tenants of `materialArray`. A second surface program is warranted only when the lighting model differs. |
| 43 | D10 Virtual-texture compositing | — | **Stages 1 and 2 SHIPPED (11.66, 11.67)**, first content roads (11.68). Open: more content, and general meshes, where the feedback vote becomes the only correct source of residency. |
| 44 | ~~D11 Large-world origin rebasing~~ | M/L | **SHIPPED (11.62)** as origin shifting rather than fp64. |
| 45 | B10 Night sky — stars | S/M | **DONE (11.79).** |
| 46 | B11 Day/night cycle | M | **DONE (11.81).** |
| 47 | B12 Night-sky floor | S/M | **DONE (11.80).** |
| 48 | B13 The moon | L | **DONE (11.82).** |
| 49 | B14 Purkinje shift | S/M | **DONE (11.83)**, spliced before the tone curve rather than into the finishing stack. |
| 50 | B15 Water at night | M | **DONE (11.84).** |
| 51 | F1 Block-compressed textures | M | **DONE (11.85)**; `docs/shaders.md`, "Block compression". |
| 52 | F2 An offline asset cook | L | **DONE (11.99).** |
| 53 | G1 Anti-aliased alpha test | S/M | **DONE (11.87 + 11.88)**: the authored cutoff on every path, sharpened alpha, coverage-preserving mips. `docs/papers/README.md`, "Alpha testing / coverage preservation", records the reference implementations and what cetra takes from each. |
| 54 | G2 Hashed alpha testing / alpha distribution | M | **Alpha distribution DONE (11.100)**, and texcoord jitter on the TAA path (11.101), off under alpha-to-coverage. Hashed alpha testing is refused: it costs shader work in `pbr_frag`, at 16/16 samplers and ten of twelve uniform blocks, and Yuksel measures it as noisy. |

**Tier 5 — the frame budget (spec 11.89, six specs, all shipped):** the one tier that asks how
long a frame takes rather than what the renderer lacks. `apps/forest` measured 127.7 ms against a
16.6 ms budget, GPU-bound in the shadow and opaque passes. A frame has one bottleneck at a time,
and removing work anywhere else does not move it. The root causes and the sequencing the rows cite
are in `specs/11.89-frame-budget-and-an-honest-instrument.md`.

| # | Item | Effort | Why here |
|---|------|--------|----------|
| 55 | **T0 An honest instrument** | S | **DONE (11.89).** The profiler's FRAME row is unclamped and skips the run's first frame, whose `dt` has no predecessor; `profiler_cpu_scope_begin` is a CPU scope with no query. Reading the table: `docs/verification.md`, Reading a `--profiler` table. |
| 55b | **T0.1 One denominator** | M | **DONE (11.97).** Every profiler row divides by the window's frame count, the GPU ring is drained at each latch so its counts match, FRAME is the frame's own work and PERIOD is begin-to-begin. Rejected: publishing mean-per-occurrence times occurrence rate, since the two come from frame sets four apart and a one-frame sweep can read zero in both windows. |
| 56 | **T1 The shadow pass** | L | **DONE (11.90).** The shadow pass orders its own casters (compact, then regroup by level), takes a shadow distance separate from the camera's far clip, and skips a `model` upload an instanced draw cannot read; `docs/lighting.md`. Refused: small-caster rejection (culls nothing on this content at any threshold that keeps visible trees) and caching the outermost cascade (every tree in it sways; that wants a static/dynamic map pair). **Left open:** the cascade fit spends `2·radius + scene_pad` of depth with `scene_pad = far_plane * 0.5` fixed, so depth per unit of extent grows without bound as a slice tightens, and sizing that pad is the prerequisite for a derived `shadow_distance` default. Whether the tighter near cascade improves the picture is not established: `calculateShadow` mins across every wider cascade, and forest has no golden. A terrain patch is a `Mesh` of its own, so the quadtree's draws cannot batch. |
| 57 | **T2 The instance arena** | M | **DONE (11.91)** as one change: the instance upload orphans and fills in a single `glBufferData` when the write covers the allocation. **The arena is refused:** a driver handed same-sized orphans every frame recycles them from a free list, and a fence-less arena, an upper bound on the real one, moved nothing outside the floor, while dropping the orphan costs 62 ms a frame. Not built: a depth-only instance block (64 B an instance against 192) needs a second block declaration and so a second program, and the shadow pass's runs end on a key change at about 16 instances, not on the cap of 64. |
| 58 | **T3 Degenerate LOD bands** | S | **DONE (11.92).** A LOD band whose cut equals its predecessor's (compared by content, not triangle count) aliases that index range, and the selected level is canonicalised onto it, so the picture cannot change; `mesh_index_total` takes the maximum over levels. Refused: a guard that stops the chain at a duplicate. A duplicate band is correct (nothing in the DAG simplifies inside that error budget), stopping would drop real bands, and a one-level chain makes `mesh_build_cluster_lod` fail over to `mesh_build_lod_chain`, a different surface. |
| 59 | **T4 Shader permutations** | L | **DONE (11.93).** Lit-surface variants: a subtractive feature mask spliced after `#version`, resolved per material every frame; `docs/shaders.md`, The lit surface's variants. The win is occupancy: removing a gated feature at compile time is worth ~24 ms of forest's opaque pass where guarding it at run time is worth 0.05. **Left open:** `PBR_FEAT_PARALLAX` has no gate arm or golden. |
| 60 | **T5 The small independent wins** | S | **CLOSED (11.94), nothing shipped.** `glGetError` is not a stall on this driver; the cluster early-out (0.036 ms) and caching `_item_bounds` cannot move a GPU-bound frame. **Deferred:** allocating the shadow array grow-only. It saves about 50 MB, not 100, because the caster count goes 1 → 2 → 1 daily as sky bodies set, and `csm.glsl` computes the transmittance base from `MAX_SHADOW_LIGHTS`, so the ceiling is a shader constant. The frame-stale ordering it found became row 61. |
| 61 | **T6 The frame-stale scene graph** | M | **DONE (11.96).** The engine latches and propagates the scene graph after the app's pre-render hook, so shadows, skinned casters and LOD read this frame's transforms; `docs/architecture.md`, Render Pipeline. An app's root offset is `Scene.root_transform`, set once. Rejected: folding that offset into `root_node->original_transform`, since a loaded model's root can carry a local of its own. |
| 62 | **T7 Sampler declarations** | M | **DONE (11.95).** Each sampler declaration is gated on the variant mask, so a lean variant spends 12 of 16 units; `docs/shaders.md`, Texture / Sampler Model. `PBR_FEAT_LAYERS` came with it and is worth more than the unit, because `sampleLayeredSurface` ran on every fragment of every material. |
| 63 | **T8 Skinned variants** | S | **DONE (11.95).** `pbr_skinned` builds the same variants from the same mask; `pbr-variant-skinned` is the arm that sees the family. |
| 64 | **G3 The interactive AA policy for masked content** | S | **REFUSED (11.102).** Promoting interactive sessions to MSAA 4 with alpha-to-coverage holds a cutout's coverage (0.89 of truth against 0.22 at one sample), but costs +108% of forest's opaque row and +296% of `ivy_arcade`'s frame; two samples cost what four do, and churn is worse at two than at one. Real content does not show the gain: raiden recovers no strands, and irregular textures never dither before their 1x1 tail. One-sample TAA keeps 11.101's texcoord jitter. Note: under alpha-to-coverage `--depth-prepass` wins (`ivy_arcade` opaque 105.6 → 47.9 ms), where at one sample it loses. |
| 65 | **G4 The five apps that never chose an AA mode** | S | **DONE (11.103).** Each app's sample count is a stated choice; `docs/architecture.md`, TAA and the sample count. gametest took one sample with TAA. **Left open:** particles write no motion vector, so TAA turns spores' motes into dashes, which is a question for the particle pass. Rejected: deciding the default inside engine creation from the headless flag, which would switch TAA on for apps that cannot use it. |
| 66 | **G5 Orthographic past the jitter** | S | **DONE (11.104).** Every perspective-only formula branches on `projection[2][3]` through `include/depth.glsl`; the `ortho` gate group holds it. The deferred sites (skybox cube, sky background, ocean grid, cloud march, sort key, gizmo) are listed under `--ortho` in `docs/cli-reference.md`. |
| 67 | **G6 The engine as a submodule** | S | **DONE (11.105).** Cetra builds under `add_subdirectory` or FetchContent; `README.md`, Using cetra in your project, and `docs/building.md`. **Left open:** there is no install rule (a working install, then a Homebrew formula, is separate work); cetra cannot be added twice in one build; `CETRA_BUILD_JOLTC=OFF` does not build, since `game/physics.c` includes JoltC unconditionally. |
| 68 | **G7 The public surface** | M | **DONE (11.106).** The public header allowlist the compiler enforces, subject-first naming, return conventions, and creation from description structs; `AGENTS.md`, Code, and `README.md`, Common Operations. |
| 69 | **G8 Nothing fails silently** | M | **DONE (11.107).** The engine derives the camera's matrices, uploads a mesh at attach, says once why the draw list refuses a mesh (`Mesh.draw_refusal`), registers materials itself, clears to `Engine.clear_color`, warns once about a scene with no light, draws the scene when handed no render hook, and `create_scene` makes the root; `README.md`, Common Operations. |
| 70 | **G9 Settings and setters** | S | **DONE (11.108).** Every public struct's fields sit under three banners (engine-owned, by function, settings), and no plain-store setter remains; `docs/architecture.md`, Key Data Structures. |

**What the frame-budget work will not do**, each refused on a measurement: chase the SSR,
atmosphere and `taa resolve` CPU rows (they are the queue draining, see 55); turn the depth prepass
on (11.30 and 11.31: it substitutes for the opaque sort and loses on the clock at one sample, see
row 64 for alpha-to-coverage); raise `--lod-bias` (at the default framing it adds draws faster than
it removes triangles; `forest-lod`'s 45% saving is taken from its own wide framing).

**Every booked row is closed.** The shortlist (20-24) is built, as are E6 (31) and E7 (37); D0
(32) is refused and E10 (38) rejected. Only D10's paging (43) waits, for a macro with dense content.

### What is next

The next step is a comparison sweep against engines that ship (UE, Unity, Frostbite, Decima), the
third. The first found that terrain was missing as a subject (rows 39-44, spec 11.59) and the
second found the asset pipeline (rows 51 and 52). Rows cannot reveal a missing subject, since the
absence is written nowhere. Before booking a subject as absent, check the other plans:
`docs/game-engine-roadmap.md` already books animation blending, IK and audio, and
`docs/rendering-roadmap.md` holds the earlier tiers.

Read this table as a backlog, not a schedule: much of the work arrives from content rather than
from rows (the water and shore series, 11.32 to 11.48, against the one row D3; 11.50 and 11.51 with
none).

### Known limitations not booked as items

Real and understood, and nobody's row.

- **Screen-space post sees one depth per pixel.** The aux attachment holds one linear Z, and the
  late pass writes none: the draw-buffer list drops to one target when the opaque scope closes, so
  a late pass that wrote depth would still write no aux. Water escapes by writing aux like an
  opaque surface; a stack of translucent layers cannot, since one slot cannot hold both its depth
  and the wall's.
  - Fog and aerial perspective handle translucency in their composite from the MBOIT moments
    (spec 11.78): `b0` is the stack's absorbance and `b1/b0` its mean warped depth. Sampling the
    froxel volumes in `pbr_frag` instead was rejected: it needs two `sampler3D` declarations, and
    both volumes are built after the scene pass, so a read there is a frame stale.
  - Still single-layer: GTAO, TAA and TAAU reprojection and motion blur (a glass pixel gets the
    wall's velocity), contact shadows, the SSGI, SSR and SSAO edge weights, and the spec-occ
    composite. DoF reads the resolved hardware depth, so fixing aux does nothing for it.
  - Transmissive surfaces never enter OIT; that is mostly right, since light seen through glass did
    come from the wall. Particles draw after OIT and generate no moments, which is the weakest
    exclusion: a smoke card is where the defect shows most.
- **Anisotropic filtering is capped at 8x** in `texture.c` and is not a setting.
- **`glUseProgram` has no choke point, and the uniform value cache assumes one.** The setters write
  through `glUniform*`, which targets whatever is bound, so a set under the wrong program updates
  that program and records the value as held, and the next legitimate write is skipped.
  `-DCETRA_CHECK_UNIFORM_BINDING=1` checks it, at a `glGetIntegerv` per set (about 6% of the frame),
  so it is off by default. Godot and Unreal avoid it by never letting app code bind a program, so
  "what is bound" is a mirrored variable. Cetra binds from well over a hundred sites; the fix is
  one function.
- **Skinned meshes cannot instance.** The skinned family has no `InstanceBlock` and takes its pose
  per draw; `skinned-nobatch` and `skinned-identity` hold it to one instance a draw.
- **No scene-owned mesh pool.** `mesh_ref` refcounting is the interim; a pool would compose with it.
- **No screen-size culling in the camera pass.** Captures have `GIWorld.cull_pixels` (spec 13.42)
  and nodes a `draw_distance` (spec 13.38). No content asks for more: forest's cost is fragments,
  not object count.
- **Transparent geometry is never depth-sorted.** OIT is on by default, so this matters only under
  `--no-oit`, where sorting would move pixels for no measured win.
- **The GL 4.1 ceiling.** Lumen-class GI, virtual shadow maps, GPU-driven culling and hardware ray
  tracing need compute. The ceiling blocks less than the label suggests: Nanite's CPU half, the
  cluster DAG with a cut quantised by distance band, shipped on 4.1 (spec 11.63), and what needs
  4.3 is GPU-driven selection and a visibility buffer. `docs/rendering-roadmap.md` §5 prices the
  three ways past it (offline CPU path tracer, hybrid Metal RT through IOSurface, a Metal or Vulkan
  backend) and owns the question.

## Foundations ownership (just-in-time)

Shared infrastructure lands with its first consumer, which owns it.

| Foundation | Owner | Later consumers |
|---|---|---|
| UBO machinery (`ubo.c/h`, std140, binding registry) | A1, delivered | froxel light data, bulk constants |
| CPU light culling (`light_cluster.c/h`) and the exponential Z slicing in `include/lights_ubo.glsl` | A1, delivered | froxel injection |
| Float textures from a C array (`create_texture_2d_float`) | A2, delivered | future LUTs |
| `Light.up` orientation frame | A2, delivered | PackedLight layout |
| Key light's view-space direction published to PostFX | A3, delivered | light-aware post passes |
| Octahedral encode/decode (`include/octahedral.glsl`) | A4, delivered | bent-normal storage, octahedral G-buffer normals |
| Capture at a position (`scene_capture_faces`), optionally into a depth cubemap at native size | A4, delivered | probe features |
| Bent normal in the AO chain | A5, delivered; read only by debug view 9 since 11.77, at ~0.13 ms | SSGI directionality, SSR occlusion, GI sampling. Each wants a cheap directional signal, and the full 32-bit sector mask beats the bent normal at that (see A5) |
| `create_texture_3d_float` and `include/froxel.glsl`, slice count a parameter | B1, delivered | aerial perspective, clouds, volumetrics |
| CPU 3D noise (`noise_worley3`, Perlin-Worley packing, threaded bake) | B2, delivered | ground fog detail, media |
| Render-res / post-res split: render, `post_*`, `out_*`, `half_*` sizes and the canvas each post-seam pass composites onto | B4, delivered | every pass after the seam |
| Transmittance against depth in the shadow path (deep opacity maps) | C1, delivered (11.26) | any translucent caster: hair, glass, foliage tips, smoke |
| Sampler units returned per variant by declaration gating | T7, delivered (11.95); D0's fold was refused (11.85) | the one consumer that cannot dodge the ceiling: sampling the froxel volume from the transparent pass, not booked |
| Tessellation pipeline | unowned: D3 and D4 shipped without it | POM silhouettes |
| Bed height seam (`WaterHeightFn`) and the CPU Gerstner query | D3, delivered | buoyancy, gameplay water tests, shoaling surfaces |
| Geometry clipmap: coarsest-cell snap and T-junction stitch | D3, built and removed; kept at `8d04658` | a paged ring structure, if one is ever wanted; D4 chose CDLOD |
| Screen-space footprint to detail handover (mip or dropped octave, energy into roughness) | D3, delivered (11.35) | any procedural surface a projected or adaptive mesh under-samples |
| World scale for shader-side physical lengths (`waterUnitsPerMetre` from `Sky.world_units_per_km`) | D3, delivered (11.44) | anything whose constants are metres |
| A CPU solver published through a std140 block (`ShoreFilmBlock`) | D3, delivered (11.45) | any per-column or per-object field a UBO can hold |
| N identical textures as one array and one declaration | D3, delivered (11.45) | any program at its sampler ceiling |
| Histogram-preserving stochastic sampling and periodic noise | D5, delivered (11.46) | surfaces seen over tens of repeats |
| Per-pass GPU timing | E4, delivered (11.27) | LOD thresholds, budget work |

## Cross-track integration contracts

- **`froxel_inject_frag` is where clustered lights meet the fog**: it walks `clusterLights` per
  cell, so local lights scatter.
- **Fog and cloud ambient come from the sky.** Swapping the fog's ambient for a GI tap
  (`giIrradiance`) is available and deliberately not taken, since it changes fog output.
- **The cluster Z slicing is stated once**, in `include/lights_ubo.glsl`, and the froxels march
  the same slices.
- **Screen-space lighting passes size off render res** (`engine_render_size`), never post res, so
  they take the TAAU speedup.
- **Sky changes reach the GI volumes by re-capture**: `scene_environment_changed` marks them dirty
  (`gi_world_mark_dirty`), and the sweep spreads over the following frames.

## Verification policy (every feature, no exceptions)

How to run the suites, the noise floor and the cross-build recipe are `docs/verification.md`'s;
the rules are `AGENTS.md`, Verifying. What this plan adds for every item:

1. **Off is off:** with the feature disabled, the cross-build recipe renders 0 differing pixels
   against its baseline (`docs/verification.md`, The recipe).
2. **On has a reference:** a deterministic headless golden or analytic gate arms. TAAU has
   neither: spec 11.7's PSNR threshold against native was never added to `gates.py`.
3. **Every feature has a CLI flag** in the render app and an ImGui toggle bound to its field.
4. **Every arm is written to `docs/verification.md`, Writing a gate arm**: falsified against the
   mutation it names, on a construction that can show the effect, a round trip comparing every
   payload, a lever for any state no flag reaches, and a look beside the numbers.
5. **Check a review's correction before applying it**, as you would a row.

## Execution workflow

Each item gets its own spec and branch, per `AGENTS.md`, Workflow; a foundation lands inside its
owner's branch. Tier order is as above.

A row is a hypothesis with an effort guess. The spec's exploration checks it against the code
before any of it is believed: re-derive the row's constraint from the code before accepting what it
concludes. Rows here have been wrong every way a row can be: reasons overtaken by later fixes, a
description of only half the code (E8 named wind and missed skinned meshes), a premise naming a
defect another spec had fixed (C5), a row arguing against itself (C3 priced a UBO table in sampler
units), and a row stopping one step short (D9 asked for a new program when the declaration it
needed was already bound). For a platform capability, ask the driver, not the GL version: BC7 is
absent on this platform, and RGTC is present with no extension string.
