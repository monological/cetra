# Shaders

The shader sources, the lit surface's variants, the vertex attributes, the texture and sampler model, and the debug render modes. `shader-subsystems.md` has each subsystem's detail.

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
shader gates ten optional features on it, rain (bit 64, spec 13.9), its puddle relief
(bit 128, spec 13.12), cached shadows (bit 256, spec 13.16) and a coat of fur shells
(bit 512, spec 13.17 -- the one bit the vertex stage reads too, so the mask is spliced into both)
the latest. `render.c`'s
resolver recomputes each material's mask EVERY FRAME — the mask is a pure function of the
material's fields and the scene's facts (decals, area panels, rain, cached lights), all of which a GUI slider
moves with nothing marked dirty — and `engine_pbr_variant` compiles and caches one program per
distinct mask. `apps/forest` resolves to two: `pbr-32` for its layered
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
`(127.1, 311.7)` and the rain's ripples off their own and both are right — sharing the form
costs neither a value, sharing a constant would have changed one. And `frameNoise(cell, frame)`,
noise new every frame that goes nowhere, for the grain and the Purkinje rod noise (spec 13.28),
over `include/pcg4d.glsl` -- Jarzynski & Olano's integer hash, which the rain and the fire already
used and which is the one integer hash to reach for. The exemptions are
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
machine, **no consumer has a correct pattern to regress against** (wind phase, foam bubbles, curl
noise and tile offsets are all arbitrary by design), and **zero of the 27 goldens it was measured
against depend on any of the four hashes** — both water goldens are Gerstner with no bed, so `foam`
is identically 0 and the bubble hash is never evaluated. Note `ign` is not one of the four and never
was. **The grain and the rod noise are the exception, since spec 13.28, and they met the bar this
rejection set**: each seeded the sin-fract hash with the frame added to BOTH screen coordinates, so
every frame was the last one moved a pixel along the diagonal -- 0.994 correlated at that shift --
and the eye read it as lines crawling over the picture. That is a demonstrated artifact with a
correct answer, noise new every frame, so those two take `frameNoise` and every other consumer
keeps the sin-fract form. `grain-still` is what holds it.

**FIFTEEN include files are compiled by BOTH languages, and there are rules.** `shore_constants.glsl`
(from `shore_runup.h`) and `wind_bounds.glsl` (from `wind.c`) were the first, and exist because a
number the GPU and the CPU must agree on is the half that drifts -- the swash solver runs host-side,
and the wind bound that makes a swaying mesh cullable is a CPU bound on GPU arithmetic. The rest
followed for the same reason: `sky_constants.glsl`, the five `water_*_constants.glsl`,
`rain_constants.glsl` (spec 13.9), which holds the drop physics, the occlusion map's depth range and
the polygon offset it is drawn with, and `shadow_tile_constants.glsl` (spec 13.16), the cached
tiles' size and guard band, the view count and the R3 constants that place a light's views, which
C draws from and the lookup finds, `gi_constants.glsl` (spec 13.24), the resident GI cap the
shader's slot arrays and the C residency are both sized by, `fire_constants.glsl`,
`loading_constants.glsl` (spec 13.34), and `canvas_constants.glsl` (spec 14.0), the 2D canvas's
record kinds and end flags, `u`-suffixed because a record's fields are unsigned, and the glyph
table's shape. All are `#define`s only: **the `f`
suffix is load-bearing**, since an unsuffixed literal is a `double` in C and promotes every
expression it touches to a precision the shader does not have (11.53 shipped without it and the bound
evaluated in double for a spec cycle), and nothing in them may use a type, a function or a qualifier
-- `const`, `static` and `float[3](...)` each belong to one side only. Numbers only. If you are about
to invent this technique, you are not: read `shore_constants.glsl`'s header first.

**`pbr_features.glsl` (from `program.h`) is the odd one out**, and it generalises the technique past a
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
`.glsl` edit rebuilt 66 objects. Now it rebuilds one. **Its sibling, `shader_includes.h`, is the
`include/` chunks UNEXPANDED, and only `shader.c` includes that** (spec 13.29): the table the
runtime resolver expands an app's `#include "x.glsl"` against. See App Shaders below.

Inventory by subsystem. **`docs/shader-subsystems.md` carries the per-subsystem detail** — how
each one works, what was rejected, and the failure modes that render a plausible frame. Read the
entry there before changing anything marked with a dagger.

- **PBR:** `pbr_vert`, `pbr_frag`, `pbr_skinned_vert` (GPU skinning)
- **Shadows:** `shadow_depth_vert/frag`, `catcher_vert/frag` (shadow-catcher ground), and
  `include/punctual_shadow.glsl`, the per-frame punctual lookup; `include/cube_face.glsl` picks a
  point light's face and, in the panel's own frame, an area light's (`panelCubeFace`, spec 13.27)
- **Cached point- and area-light shadows:** `include/punctual_tiles.glsl` (the surface's lookup: a 3x3 PCF
  for a light with no body, the averaged views for one with a body), `include/tile_lookup.glsl`
  (a face projected analytically and its tile found from the array's size, shared with the
  fog's tap), `include/cube_face.glsl`, and `include/shadow_tile_constants.glsl` (both languages).
  Gated by `PBR_FEAT_SHADOW_TILES` (256), which declares no sampler
- **IBL:** `ibl_cubemap_vert`, `ibl_equirect_frag`, `ibl_irradiance_frag`, `ibl_prefilter_frag`, `ibl_charlie_prefilter_frag` (sheen env), `ibl_brdf_frag`
- **Atmospheric sky:** `sky_transmittance/multiscatter/view/env/background/debug_frag`
- **Overcast and the sky's scale †:** `include/sky_emission.glsl`, uploaded only by
  `sky_bind_emission` (spec 13.7), over `include/sky_constants.glsl` -- the sun's illuminance and
  the fp16 store ceiling, included by `sky.h` as well, since apps compute with the first. A CIE
  overcast dome, and nits per unit of the sky's relative scale. Both are baked, and both reach the
  shaders as numbers already weighted, so a clear relative sky is the old arithmetic exactly
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
  `include/ocean.glsl`, and `water_probe_frag` -- the surface query (spec 13.1), one texel per
  point through the same `oceanEvaluateAt`, read back through a pack-buffer ring at FIXED
  latency and never a fence, which is what keeps a headless run equal to itself. It is
  `water_probe_set` / `water_probe_result` in `water.h`, one path for either wave model -- both
  answer from the ring, the sample says which instant it describes, and a slot registered or
  moved since its answer was rendered is refused as `stale` rather than answered for the point
  it used to name. `water_surface_at` stays the Gerstner closed form and the query's reference.
  And `water_caustic_land_frag` / `_vert` / `_geo` / `_frag` -- the refracted-grid caustics (spec
  13.2), on either wave model: every lattice corner traced once through the SHADING normal
  (`oceanShadingNormal`, the same one the surface is lit by) onto the floor, then the lattice
  drawn where it landed, additively, each cell carrying beam area over landed area so flat water
  is exactly 1 and any sea averages 1. The geometry stage rescales each triangle's corners to its
  exact ratio, which is what keeps per-corner smoothing from adding light at folds. `water_frag`
  applies it to the KEY light's share of what lights the refracted point only, at the point where
  the refracted view ray meets the bed -- not at the refracted screen sample, whose rows squeeze
  together low in the frame and smeared the pattern into streaks. Sizes, depth and the ceiling are
  `water_caustic_constants.glsl`, read by both languages. **Two LEVELS since 13.3**, one array
  layer each: the 40 m window and a 10 m one of 2.34 cm cells over it, for shallow water only
  (full to 4 m of traced depth, gone by 8, not traced at all deeper), and the key's shadow on the
  bed softened by how far the waves spread the refracted light. **The `glFlush` between each
  level's trace and its lattice draw is load-bearing**: the lattice reads the trace by vertex
  `texelFetch`, and on this driver that saw the texture from before the pass that rendered it.
  **The ripple band (13.3)** is the fourth spectral band, 512² over 6 m past the short band's
  24 rad/m (`water_ripple_constants.glsl`), shading the surface and the caustic trace through
  `oceanShadingNormal` at the mip its footprint asks for, with what that mip drops handed to
  roughness from a per-level variance predicted at seeding. **Spec 13.4 brought Clearwater's
  better halves across**, each behind its own switch and arm:
  - **Light reaching a submerged surface** (`include/water_light.glsl`, read by `pbr_frag` too):
    every directional scaled by `1 - F` and Beer-Lambert along its REFRACTED path, the ambient
    by the sky's diffuse weakening, published through `_water_publish_sea`. `water_fixture`'s
    bed is EMISSIVE and cannot see this; `water_downwell_fixture.cscn` is the lit one.
  - **In-scatter** as the column integral of a sun weakened with depth, Henyey-Greenstein at
    `Water.scatter_g`, normalised against the NADIR -- an eye above water only sees the back
    of a forward lobe, and a sphere normalisation dimmed every sea to a third.
  - **Caustics split into the spectrum from ONE trace**, 8 taps along the first-order shift
    weighted by CIE 1931 and blurred by the key's disc. It gets the AMOUNT of colour right and
    not its placement; `--water-caustic-bands N` traces N real bands and is the reference.
  - **Cubic B-spline reads** of the shading bands where magnified, the bed's **relief** read off
    its own brightness, sunlit **specks** in the column, and **touch ripples** -- a 7 m wave
    simulation following the view, fed by `water_ripple_drop`, on unit 5, which takes `water_frag`
    to 14 samplers

  **A water may be held to an xz rectangle since spec 13.41** (`Water.domain.bounds`, all zero
  meaning everywhere, so every water before it is unchanged). Past it there is no surface, no light
  weakened under the level, no medium round a submerged eye and no rain landing on the plane:
  a lake is not a sea, and an unbounded one at a lake's level darkened every surface below it
  anywhere in the world. The rule is stated once per language, `water_bounds.h` and
  `include/water_bounds.glsl`, and every reader asks it. The render app's `--water-bounds` is the
  A/B, and `water-bounds-*` holds it; those arms bound by +/-1e20, since the projected grid
  reaches past a kilometre and a bound of 1000 cut the horizon.
  **Inside the bounds, DRY SHAPES since spec 13.56** (`Water.domain.dry`, up to `WATER_DRY_MAX`
  superellipses, an app moving them each frame): the inside of a hull floating on it, whose floor
  sits under the level. They are the bounds' rule turned inside out, so `water_reaches` and
  `waterReaches` ask both, and every reader that asked the bounds asks them. That covers the
  surface, the light under the level, the shore's wet band, the medium, the rain and the eye. The
  bounds and the shapes travel together as one `WaterDomain`, which PostFX and the rain carry
  whole and `water_domain_upload` gives any program that includes the GLSL rule, so a property of
  where the water is added later is one field and not another round of every signature. No
  shapes is the old water to the bit. `--water-dry` is the A/B and `water-dry-*` holds it
- **Diffraction glare:** `glare_source/fft/multiply/output_frag` + `glare.c` (spec 13.4) -- see
  the PostFX defaults above
- **Local exposure:** `le_half/le_bins/le_grid/le_grid_blur/le_block_sum/le_block/le_blur_frag` +
  `local_exposure.c` (spec 13.19), sliced in the tonemap by `include/local_exposure.glsl` over
  `include/le_grid.glsl`, the atlas's layout both sides read -- see the PostFX defaults above
- **CRT:** `crt_resample_frag` (the picture to the signal, in linear light) + `crt_frag` (the
  beams, the slot mask, the tube and CRTS's tone curve) (spec 13.28) -- see the PostFX defaults
  above. Two chunks came out of `tonemap_frag` for it: `include/display.glsl`, the display encode
  and its inverse, and `include/dither.glsl`, the output dither, so the CRT encodes and dithers
  exactly as the tonemap does
- **Loading screen:** `loading_logo_frag` (the mark: CETRA's letters as striped tubes round
  skeletons of strokes and arcs, each turned in perspective), `loading_blur_frag` (the tube's
  bloom, a Gaussian blur of the whole picture, across then down at a quarter its size) and
  `loading_tape_frag` (the worn tape: colour split, tracking band and glitches, static, the
  switch on and off) (spec 13.34), shown through a CRT of their own. The ident's timeline is
  `include/loading_constants.glsl`, read by both languages, since the C side will not lift the
  screen before the ident has played
- **OIT:** `oit_resolve_frag` + `include/mboit.glsl` (the absorbance moments and their
  reconstruction, shared by the generation and accumulate sub-passes)
- **Atmosphere †:** `froxel_inject/integrate/composite_frag` + `include/froxel.glsl`, and
  `aerial_lut_frag`. `froxel_inject_frag` is where every medium meets, and is compiled a second
  time under `FROXEL_MISS_COUNT` as `--fog-miss-probe`'s count (spec 13.45)
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
- **Rain †:** `rain_vert/frag` (the falling drops and their splash droplets, one program, from the
  post chain's late draw) + `include/rain_constants.glsl` (both languages) + `rain_occlusion.glsl`
  (the cover) + `rain_surface.glsl` (wet ground and puddles, over the shore's `wet_surface.glsl`) +
  `rain_ripples.glsl` + `rain_phase.glsl` (the drops' phase, shared with the fog inject), and
  for wet ground's reflections `spec_occ_composite_frag`'s `wetReflection` (the replacement,
  before TAA), `ssr_fold_wet_frag` (the catcher's fold on a wet frame) and
  `include/env_medium.glsl`. Since 13.12
  `rain_glass.glsl` (beads and running drops on glass, pure arithmetic), and `rain_vert` draws
  DRIPS too, from authored edge lines, after the splash droplets. Declares no sampler in
  `pbr_frag`: the cover is a layer of the punctual array. The one exception is the RELIEF bit,
  which reads a material's own `heightTex` for where its puddles stand, and is its own bit so a
  plain rain variant does not pay that unit
- **Particles:** `particle_vert/frag`
- **Text:** `text_vert/frag`
- **Game UI:** `ui_vert/frag` — one program for every menu primitive, selected per vertex by a
  `mode` attribute: a flat fill, a textured quad, or an SDF glyph. Rounded corners and borders are
  an exact signed-distance evaluation rather than geometry, so a radius costs no vertices; the
  glyph branch takes `fwidth` with no constant smoothing term, which is what keeps text the same
  weight at any size. **A border is laid over its fill by Porter-Duff over** (spec 14.12), in
  straight colour, the box's outer edge counted once: mixed into the fill's colour as though the
  fill were opaque, a border over a clear fill was pulled toward black and laid on at its alpha
  twice, and drew at about a third of its strength, which no bordered element in the tree showed
  until pcb's schematic, a self-drawn panel whose focus border has nothing under it. A fading
  screen fades a border with its fill. `canvas-ui-borders` holds it
- **2D canvas (spec 14.0):** `canvas_vert/frag` (an instance of a unit quad a record: segments,
  arcs, circles, boxes, a fill's outer edge ramp, glyphs, and outline rings), `canvas_fill_vert/frag`
  (a polygon's triangles and their inner ramp), `canvas_composite_frag` (a view's rectangle into the
  frame, opaque), over `include/canvas_constants.glsl` (both languages), `include/canvas_paint.glsl`
  (the view, the tables, a shape's colour through its tags) and `include/canvas_pass.glsl` (the
  layer resolve's passes)
- **Debug / util:** `shape_vert/geo/frag`, `bone_vert/frag`, `xyz_vert/frag`, `mask_copy_frag`
- **App shaders (spec 13.29):** `late_surface_vert` under an app's late-draw fragment, with
  `include/late_surface.glsl` (its depth test, fog and pre-exposure); `include/surface_hook.glsl`,
  the struct a surface hook decides; `present_frag`, the picture into the window when something
  drew after the tone map and no CRT is on. The splice markers are
  `// CETRA_SURFACE_HOOK_CHUNK` (`pbr_frag`, `shadow_depth_frag`) and
  `// CETRA_OFFSET_HOOK_CHUNK` (`include/object_position.glsl`)

Three rules from that inventory stay HERE, because each one is violated from a different file:

- **`tonemap_frag`'s finishing stack has an ORDER and it is stated at the top of the file**:
  `sharpen -> grade -> vignette -> gamma -> LUT -> grain -> dither`. Two of those positions are
  contracts rather than taste. **Dither must stay last** (spec 11.24) because it is the quantization
  stage and anything appended after it reaches the 8-bit target undithered -- which is why, with the
  CRT on, the tonemap writes the picture undithered at RGBA16F and the CRT's pass dithers the
  window's write instead (spec 13.28; `crt-dither-last` holds it). **The LUT must stay
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
reported 16 active samplers. A declaration outlives a dead read -- but not the absence of
one: spec 13.12 declared `heightTex` in every rain variant with no read anywhere, and
`pbr-64` stayed at 12. **A VARIANT is not full,
and that is 11.95's whole result** -- `pbr-0` spends **12**, because gating the reads with
the PREPROCESSOR rather than a branch lets the declaration go too (`sheenTex`,
`charliePrefilteredMap`, `ltcTex`, and `heightTex` where neither parallax, layers nor the
rain's relief are carried). Ask which units a variant needs before assuming the ledger; the answer for the
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
structure (16 bits and 7344 bytes since 13.24's sixteen resident), and 11.73's DECAL transforms plus a 16-bit froxel mask on binding
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
**15** -- 13.2's caustics targets on unit 3, 13.3's ripple band on 4, 13.4's touch ripples on 5,
and 13.9's rain cover (the punctual array, for the one layer the occlusion map is) on 9 -- not
16, so ONE unit is left, and this paragraph claimed the ceiling for three specs after it was
freed. It is a
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
RENDER_MODE_CLEARCOAT_NORMAL // The clearcoat layer's own normal (13; --clearcoat-debug)
RENDER_MODE_PROBE_COVERAGE   // Albedo, magenta where the reflection falls to the sky (14)
```

**Probe coverage is the view to reach for when a shiny surface indoors looks washed out.** Indoors
a reflection should come from a reflection probe; a surface outside every probe box falls back to
the global environment, the sky, and in the shaded frame that is only a faint grey sheen. Here it is
magenta, scaled by the share that fell through. Outdoors everything is magenta, and right. It found
silent's parlour and upstairs doors: hung in the middle of their walls, outside the room boxes that
stopped at the walls' inner faces. It is in the GUI's Render Mode list, so any app with the debug
panel has it.

The clearcoat-normal view was reached by the literal 13 for a while, with nothing in this enum to
name it; it has a member now, and the enum covers every value the shader handles.

## Modules

### `program.c/h`, `shader.c/h`, `uniform.c/h`

Shader program compile, uniform setup. **Never ask the driver about uniforms one index at a time** (spec 13.39): an active uniform is every member of every element of every uniform block too, and Apple's driver finds a uniform by index by walking all of them. So `_count_program_samplers` cost more than compiling and linking -- 1.5 s of silent's load, a batched index query included -- until it asked the BLOCKS for their members and only the rest for their types. `shader.c` also holds the runtime `#include` resolver every source passes through, and the splice that puts an app's GLSL at a marker line (spec 13.29)
