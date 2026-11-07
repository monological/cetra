# Shaders

The shader sources and how they reach the driver, the lit surface's variants, the headers C and
GLSL share, the vertex attributes, the texture and sampler model, and the debug render modes.
How each subsystem's shaders work is in [shader-subsystems.md](shader-subsystems.md); an app's own
GLSL is its [Shader hooks](shader-subsystems.md#shader-hooks) section.

## Sources and the generated header

GLSL sources live in `cetra/shaders/`, shared chunks in `cetra/shaders/include/`. Edit the `.glsl`
files, never a generated header.

- **`shader_strings.h`**: at build time `gen_shader_header.py` turns every `.glsl` into a C string
  literal in `out/generated/shader_strings.h`, with its includes expanded. It lives in the build
  tree so a read-only checkout builds and two build directories never share one file. The build
  regenerates it on any `.glsl` edit (`GLOB_RECURSE ... CONFIGURE_DEPENDS`, so a chunk under
  `include/` counts); `./build.sh` is the whole answer.
- **Only `program.c` includes it.** Included from a header that `engine.h` pulls in, it put 2 MB of
  string literals into every file that used the engine, and a `.glsl` edit rebuilt dozens of
  objects.
- **Never generate it into `cetra/src/`.** That path is gitignored, and a quoted `#include` finds
  the including file's own directory first, so an in-tree copy silently shadows the build-tree one
  and the engine renders whatever the stale file says. `cetra/CMakeLists.txt` refuses to configure
  while one exists; delete `cetra/src/shader_strings.h` if you meet that error.
- **`shader_includes.h`** is the `include/` chunks unexpanded, and only `shader.c` includes it: the
  table the runtime `#include` resolver expands an app's shader against (spec 13.29). `shader.c`
  also holds the splice that puts an app's GLSL at a marker line. Both are described under
  [Shader hooks](shader-subsystems.md#shader-hooks).
- **`post_vert.glsl`** is the fullscreen-triangle vertex shader every post pass shares.

`program.c/h`, `shader.c/h` and `uniform.c/h` compile and link programs and set their uniforms.
**Never ask the driver about uniforms one index at a time** (spec 13.39). An active uniform is
every member of every element of every uniform block too, and Apple's driver finds a uniform by
index by walking all of them, so counting a program's samplers that way cost more than compiling
and linking it (1.5 s of silent's load). `_count_program_samplers` asks the blocks for their
members and only the remaining uniforms for their types.

## The lit surface's variants

The lit surface is not one program but a family of variants (spec 11.93); know this before editing
`pbr_frag.glsl`.

- **The mask.** `program.c` splices `#define CETRA_PBR_FEATURES <mask>` in after the `#version`
  line (`shader_source_with_defines`, followed by `#line 2` so every body line number survives).
  The bits are in `include/pbr_features.glsl`: decals 1, area lights 2, sheen 4, anisotropy 8,
  parallax 16, layers 32, rain 64, puddle relief 128, cached shadow tiles 256, fur 512. Fur is the
  one bit the vertex stage reads, so the mask is spliced into both stages.
- **The resolver.** `render.c` recomputes each material's mask every frame. The mask is a pure
  function of the material's fields and the scene's facts (decals, area panels, rain, cached
  lights), any of which a GUI slider can change with nothing marked dirty.
  `engine_pbr_variant` compiles and caches one program per distinct mask; `apps/forest`, for
  example, resolves to `pbr-32` for its layered terrain and `pbr-0` for everything else.
- **Two families share the fragment source**, `pbr` and `pbr_skinned` (spec 11.95), differing only
  in the vertex stage. A mask means the same in both. A skinned program has no `InstanceBlock`, so
  it can never carry more than one instance, with nothing needing to assert it. `puppet_golden` is
  the golden that goes through `pbr_skinned_vert` and `skin.glsl`; `pbr-variant-skinned` is the
  gate arm.

Three rules:

- **The polarity is subtractive, and that is structural.** No define at all means every feature,
  because `pbr_features.glsl` defaults the mask to `PBR_FEAT_ALL`. A resolver that fails to run,
  or a mask never assembled, gives the slow uber-shader, never a fast program missing a feature its
  material needed. Wrong and slow is recoverable; wrong and pretty is what ships.
- **C and GLSL share a number, not a name.** Had C emitted macro names (`#define CETRA_NO_SHEEN 1`)
  for the shader to `#ifdef`, renaming either side would silently stop the guard firing: the
  picture stays right and the optimisation just stops happening.
- **What a gate is worth depends on where it is.** A feature gate is a dynamically uniform branch
  that an unusing scene already skips, so guarding it at runtime is worth about 0.05 ms; removing
  the same code at compile time was worth 24 ms. The cost is occupancy (live values and declared
  samplers), which every fragment pays whether or not the branch is taken. A runtime `if` around a
  gated feature is close to free and close to pointless.

## Headers C and GLSL share

Fifteen headers under `include/` are compiled by both languages, because a number the GPU and the
CPU must agree on is the half that drifts:

- `shore_constants.glsl` (from `shore_runup.h`: the swash solver runs on the host) and
  `wind_bounds.glsl` (from `wind.c`: the bound that makes a swaying mesh cullable is a CPU bound
  on GPU arithmetic). Read `shore_constants.glsl`'s header before inventing this technique again.
- `sky_constants.glsl`; the five `water_*_constants.glsl` (caustic, dry, probe, ripple, touch);
  `rain_constants.glsl` (the drop physics, the occlusion map's depth range and polygon offset);
  `shadow_tile_constants.glsl` (the cached tiles' size and guard band, the view count, the R3
  constants that place a light's views); `gi_constants.glsl` (the resident GI cap both sides size
  their arrays by); `fire_constants.glsl`; `loading_constants.glsl`; `canvas_constants.glsl` (the
  canvas's record kinds and end flags, `u`-suffixed because a record's fields are unsigned, and the
  glyph table's shape).
- `pbr_features.glsl` (from `program.h`): the variant bits, which C emits as a mask and the shader
  reads back through the same names, plus `CETRA_HAS` and the `#ifndef` default that makes an
  unspliced source the uber-shader. It lives apart from `pbr_frag` so a chunk can gate its own
  sampler declaration (`ltc.glsl` decides when `ltcTex` exists). C sees both macros and ignores
  them.

The rules:

- **`#define`s only.** No type, function or qualifier: `const`, `static` and `float[3](...)` each
  belong to one language only.
- **Every float literal takes `f`.** An unsuffixed literal is a `double` in C and promotes every
  expression it touches to a precision the shader does not have. `pbr_features.glsl` is the
  exception, being integer mask bits, which mean the same to both preprocessors.

## Hashes

`include/noise.glsl` owns the shared hashes.

- **`ign`** is interleaved-gradient noise for screen-space dither.
- **`hash21(vec2 p, vec2 k)` and `hash13(vec3 p, vec3 k)`** are the sin-fract value hash. `k` is a
  parameter because water keys off `(127.1, 311.7)` and the rain's ripples off their own, and
  both are right: sharing the form changes no value, sharing a constant would change one.
- **`frameNoise(cell, frame)`** is noise new every frame that goes nowhere, over
  `include/pcg4d.glsl` (Jarzynski & Olano's integer hash, the rain's and the fire's, and the one
  integer hash to reach for). The film grain and the Purkinje rod noise use it; see
  [Film grain](shader-subsystems.md#film-grain) for why an offset-seeded hash crawled.

Exemptions and refusals:

- **`ssr_frag` and `contact_shadow_frag` inline IGN** in the explicit multiply-add form. The
  include spells it as a `dot()`, which rounds differently, and the hash's low bits steer a traced
  ray, so migrating them moves pixels (tens of thousands on a no-TAA render).
- **`particle_sim_vert`'s `hash33` and `stochastic.glsl`'s `_stochasticHash` stay where they are**:
  their arity differs, and the latter would drag `stochasticLut` and `dFdx` into any vertex program
  that included it.
- **Replacing the sin-fract form with an integer hash is rejected** (roadmap E10). Its precision is
  genuinely poor: the `* 43758.5` amplifies argument error by the same factor, and a 64x64 lattice
  hashes to 1047 distinct values of 4096 at the origin and 53 at a world offset of 1e7. But the
  gain is cross-driver reproducibility on a one-driver machine, and no consumer (wind phase, foam
  bubbles, curl noise, tile offsets) has a correct pattern to regress against. Only noise that is
  seen changing frame to frame has a right answer, and that takes `frameNoise`.

## Inventory

Programs and chunks by subsystem. A dagger marks a subsystem with an entry in
[shader-subsystems.md](shader-subsystems.md); read it before changing that subsystem. The post
chain's defaults (what is on, what is lazily allocated) are in
[architecture.md, Render Pipeline](architecture.md#render-pipeline).

- **PBR:** `pbr_vert`, `pbr_frag`, `pbr_skinned_vert` (GPU skinning)
- **Shadows:** `shadow_depth_vert/frag`, `catcher_vert/frag` (the shadow-catcher ground),
  `include/punctual_shadow.glsl` (the per-frame punctual lookup), `include/cube_face.glsl` (a
  point light's face, and in the panel's own frame an area light's, `panelCubeFace`)
- **Cached point- and area-light shadows †:** `include/punctual_tiles.glsl`,
  `include/tile_lookup.glsl` (shared with the fog's tap), `include/cube_face.glsl`,
  `include/shadow_tile_constants.glsl`. Gated by `PBR_FEAT_SHADOW_TILES`, which declares no sampler
- **IBL:** `ibl_cubemap_vert`, `ibl_equirect_frag`, `ibl_irradiance_frag`, `ibl_prefilter_frag`,
  `ibl_charlie_prefilter_frag` (the sheen environment), `ibl_brdf_frag`
- **Atmospheric sky:** `sky_transmittance/multiscatter/view/env/background/debug_frag`
- **Overcast and the sky's scale †:** `include/sky_emission.glsl`, uploaded only by
  `sky_bind_emission`, over `include/sky_constants.glsl`, which `sky.h` includes too
- **Day/night cycle †:** `sky_cycle_tick` (`sky_cycle_advance` + `sky_slicer_pump`), called from
  `engine.c` before the GI sweep and the shadow pass
- **The moon †:** `include/moon.glsl` and one guarded block in `include/sky_radiance.glsl`, over
  the surface `moon_surface.c` bakes
- **Night floor †:** one term at the end of `sky_view_frag`'s march
- **Stars †:** `include/stars.glsl` and one term in `include/sky_radiance.glsl`
- **Skybox:** `skybox_vert/frag`
- **Bloom:** `bloom_bright_frag`, `bloom_downsample_frag`, `upsample_tent_frag`
- **Tonemap / exposure †:** `tonemap_frag`, `lum_measure_frag`, `lum_histogram_frag`,
  `lum_reduce_frag`
- **Purkinje / scotopic shift †:** `include/purkinje.glsl`, `tools/gen_scotopic_weights.py`
- **DoF:** `dof_coc/tile/dilate/gather/composite_frag`
- **Motion blur:** `motion_blur_tilemax/neighbormax/_frag`
- **TAA / temporal:** `taa_resolve_frag`, `taau_resolve_frag` (the render-to-post upscale under
  `--render-scale`), `temporal_accum_frag`
- **AO / GI:** `gtao_frag`, `ssao_blur_frag`, `ssgi_accum/atrous/composite_frag`
- **Specular occlusion †:** `spec_occ_composite_frag`, `include/spec_occ.glsl`
- **SSR:** `ssr_frag`, `ssr_hiz_frag`, `ssr_accum_frag` (temporal), `ssr_atrous_frag`
- **SSS:** `sss_gather_frag`, `sss_pyr_seed_frag`, `sss_pyr_down_frag`: a pyramid gather. A
  comment in `froxel_composite_frag` still names the separable `sss_blur_frag` it replaced
- **IES profiles †:** `include/lights_ubo.glsl`'s `IesBlock` and `iesProfile`
- **Contact shadows †:** `contact_shadow_frag`
- **Water †:** `water_vert/frag`, `water_spectrum_frag`, `water_fft_frag`, `water_foam_frag`,
  `include/ocean.glsl`, `include/water_light.glsl` (read by `pbr_frag` too), `water_probe_frag`,
  and the caustics' `water_caustic_land_frag` and `water_caustic_vert/geo/frag`. Three parts the
  Water entry does not cover:
  - **The surface query** (`water_probe_set` / `water_probe_result` in `water.h`, spec 13.1):
    `water_probe_frag` evaluates one texel per point through the same `oceanEvaluateAt`, read
    back through a pack-buffer ring at fixed latency, never a fence, which keeps a headless run
    equal to itself. One path serves either wave model; the sample says which instant it
    describes, and a slot registered or moved since its answer was rendered is refused as `stale`
    rather than answered for the point it used to name. `water_surface_at` stays the Gerstner
    closed form and the query's reference.
  - **The refracted-grid caustics** (spec 13.2), on either wave model: every lattice corner is
    traced once through the shading normal (`oceanShadingNormal`, the one the surface is lit by)
    onto the floor, then the lattice is drawn additively where it landed, each cell carrying beam
    area over landed area, so flat water is exactly 1 and any sea averages 1. The geometry stage
    rescales each triangle's corners to its exact ratio, so per-corner smoothing adds no light at
    folds. `water_frag` applies the result to the key light's share only, at the point where the
    refracted view ray meets the bed; sampled at the refracted screen position, whose rows squeeze
    together low in the frame, it smeared into streaks. Sizes, depth and the ceiling are
    `water_caustic_constants.glsl`.
  - **Where the water is** (`WaterDomain`: `Water.domain.bounds`, an xz rectangle, all zero
    meaning everywhere, spec 13.41; and `Water.domain.dry`, up to `WATER_DRY_MAX` superellipses an
    app moves each frame, the inside of a floating hull, spec 13.56). Past the bounds or inside a
    dry shape there is no surface, no light weakened under the level, no shore wet band, no medium
    round a submerged eye and no rain landing on the plane; an unbounded water at a lake's level
    would darken every surface below that level anywhere in the world. The rule is stated once per
    language, `water_bounds.h` (`water_reaches`) and `include/water_bounds.glsl`
    (`waterReaches`), and every reader asks it. PostFX and the rain carry the domain whole, and
    `water_domain_upload` gives it to any program that includes the GLSL rule, so a new property
    of where the water is is one field. `--water-bounds` and `--water-dry` are the A/B,
    `water-bounds-*` and `water-dry-*` the arms; the bounds arms use +/-1e20, since the projected
    grid reaches past a kilometre.
- **Diffraction glare †:** `glare_source/fft/multiply/output_frag`, `glare.c`
- **Local exposure †:** `le_half/le_bins/le_grid/le_grid_blur/le_block_sum/le_block/le_blur_frag`,
  `local_exposure.c`, sliced in the tonemap by `include/local_exposure.glsl` over
  `include/le_grid.glsl` (the atlas layout both sides read)
- **CRT †:** `crt_resample_frag`, `crt_frag`, and `include/display.glsl` (the display encode and
  its inverse) and `include/dither.glsl` (the output dither), which it shares with the tonemap so
  it encodes and dithers exactly as the tonemap does
- **Loading screen:** `loading_logo_frag` (CETRA's letters as striped tubes round skeletons of
  strokes and arcs, each turned in perspective), `loading_blur_frag` (the tube's bloom, a Gaussian
  across then down at a quarter size), `loading_tape_frag` (colour split, tracking band, glitches,
  static, the switch on and off), shown through a CRT of their own (spec 13.34). The ident's
  timeline is `include/loading_constants.glsl`, since the C side will not lift the screen before
  the ident has played
- **OIT:** `oit_resolve_frag`, `include/mboit.glsl` (the absorbance moments and their
  reconstruction, shared by the generation and accumulate sub-passes)
- **Atmosphere †:** `froxel_inject/integrate/composite_frag`, `include/froxel.glsl`,
  `aerial_lut_frag`
- **Volumetric clouds:** `cloud_march_frag`, `sky_background_clouds_frag`, `sky_env_clouds_frag`,
  `cloud_noise_debug_frag`
- **Cloud shadow †:** `cloud_shadow_frag`
- **Clustered specular probes †:** `probe_project_frag`, `include/probe_specular.glsl`
- **Clustered decals †:** `include/decals_ubo.glsl`
- **Layered surfaces †:** `include/layers.glsl`, `include/triplanar.glsl`
- **Roads †:** `roadReshapedWeights` in `layers.glsl`, `include/roads_ubo.glsl`
- **Composite cache †:** `layers_vt_bake_frag`, `sampleCachedSurface` in `layers.glsl`,
  `layers_vt_feedback_vert/frag` (the vote pass)
- **Rain †:** `rain_vert/frag` (drops, splash droplets and drips, one program, from the late draw),
  `include/rain_constants.glsl`, `rain_occlusion.glsl`, `rain_surface.glsl` (over the shore's
  `wet_surface.glsl`), `rain_ripples.glsl`, `rain_phase.glsl`, `rain_glass.glsl`; for wet ground's
  reflections `spec_occ_composite_frag`'s `wetReflection`, `ssr_fold_wet_frag` and
  `include/env_medium.glsl`
- **Particles:** `particle_vert/frag`
- **Text:** `text_vert/frag`
- **Game UI:** `ui_vert/frag`, one program for every UI primitive, chosen per vertex by a `mode`
  attribute: a flat fill, a textured quad or an SDF glyph.
  - Rounded corners and borders are an exact signed-distance evaluation, so a radius costs no
    vertices; their edge is a one-pixel ramp (`docs/ui.md`, The draw list).
  - The glyph branch takes `fwidth` with no constant smoothing term, which keeps text the same
    weight at any size.
  - **A border is laid over its fill by Porter-Duff over** in straight colour, the box's outer edge
    counted once (spec 14.12). Mixed into the fill's colour as though the fill were opaque, a
    border over a clear fill is pulled toward black and applied at its alpha twice, drawing at
    about a third of its strength. A fading screen fades a border with its fill.
    `canvas-ui-borders` holds it.
- **2D canvas:** `canvas_vert/frag` (one instance of a unit quad per record: segments, arcs,
  circles, boxes, a fill's outer edge ramp, glyphs, outline rings; a dashed record is 64 bytes, its
  pattern in attribute 3 on the dashed store's own vertex array, and the vertex stage resolves each
  end of a segment or arc to a cap, a cut, an open corner or a join, `CANVAS_END_*`, which one
  fragment function draws), `canvas_fill_vert/frag` (a
  polygon's triangles and their inner ramp), `canvas_composite_frag` (a view's rectangle into the
  frame, opaque), over `include/canvas_constants.glsl`, `include/canvas_paint.glsl` (the view, the
  tables, a shape's colour through its tags) and `include/canvas_pass.glsl` (the layer resolve's
  passes, and a fading view's fade of each shape). See [canvas.md](canvas.md)
- **App shaders †:** `late_surface_vert` with `include/late_surface.glsl`,
  `include/surface_hook.glsl`, `present_frag`. The splice markers are `// CETRA_SURFACE_HOOK_CHUNK`
  (`pbr_frag`, `shadow_depth_frag`) and `// CETRA_OFFSET_HOOK_CHUNK`
  (`include/object_position.glsl`). See [Shader hooks](shader-subsystems.md#shader-hooks)
- **Debug / util:** `shape_vert/geo/frag`, `bone_vert/frag`, `xyz_vert/frag`, `mask_copy_frag`

## Rules that span files

Each of these is broken from a file other than the one that states it.

**`tonemap_frag`'s finishing stack has an order**, stated at the top of the file:
`sharpen -> grade -> vignette -> gamma -> LUT -> grain -> dither`.
- **Dither stays last** (spec 11.24): it is the quantization stage, and anything after it reaches
  the 8-bit target undithered. With the CRT or an after-tonemap pass on, the tonemap writes an
  undithered RGBA16F picture and the pass that writes the window dithers instead
  (`crt-dither-last`).
- **The LUT stays after `displayEncode`** (spec 11.58), because a `.cube` is authored against
  display-encoded values; grain follows it as sensor noise over a finished look.
- **The LUT's `sampler3D` is on unit 11.** This program's units are its own, and a `sampler3D`
  sharing a unit with a `sampler2D` is `INVALID_OPERATION` at draw, so it does not take the free
  unit 7.
- **`lutCoord`'s half-texel inset applies to the trilinear path only.** `lutTetrahedral` addresses
  texels by integer index, so the default path is an exact identity whether that remap is right or
  wrong; the identity gate arm needs its trilinear leg to see it.

**`punctualAngular` is the one place a punctual light's angular falloff is decided** (spec 11.57):
the IES profile where there is one, `spotConeFactor` otherwise. It is evaluated in five places:
`pbr_frag`'s light loop, `pbr_frag`'s `renderMode 7` debug copy (which no golden sees), the froxel
fog's clustered point walk, the fog's spot shaft, and `contact_shadow_frag`'s fold weight. A
profile applied in some but not all makes the beam disagree with the pool it casts.
- **The decision is split from its data source.** The spot shaft is published standalone, so it
  can carry its shadow map, and does not come from the cluster list. `punctualAngularOf` takes the
  values; `punctualAngular` is the cluster-list adapter over it; the shaft calls the former. A
  site restating the rule as its own ternary is the failure to look for.
- **A profile is refused on an area panel and a directional**, at authoring and again at pack.
  `contact_shadow_frag` folds the angular term into its denominator before it classifies the
  light, so a profiled panel would be weighted by a distribution its LTC shading never applies.
  `--ies-profile` skips the same two types, so it can be pointed at a whole scene: the practicals
  take the profile and the sun and softboxes do not.
- **`iesProfile` answers 1.0 out of range**, the neutral for a multiplier and wrong for a
  replacement, so the "is this a profile" bounds test lives in `punctualAngularOf`. Done the other
  way, a stale index makes a light an unbounded omnidirectional emitter that never returns exact
  zero, and loses `pbr_frag`'s early-out.

**`shadowMisc.y` is the live punctual layer, not `Light.shadow_layer`** (spec 11.56).
`shadow_layer` is maintained only while the depth pass runs, so a shadow system switched off at
runtime leaves every mapped light claiming a layer nothing draws. `light_cluster.c` packs
`shadow_live_punctual_layer`, which applies `enabled` and the `punctual_layer_count` bound, the same
bound `punctual_shadow.glsl` puts on its lookup. Anything asking whether a light has a map goes
through that helper, never the raw field.

## Vertex Attributes

From `common.h`:

```c
#define GL_ATTR_POSITION      0    // vec3
#define GL_ATTR_NORMAL        1    // vec3
#define GL_ATTR_TEXCOORD      2    // vec2  (UV0)
#define GL_ATTR_TANGENT       3    // vec4  (xyz tangent, w = bitangent handedness)
//                            4       free for mesh VAOs, see below
#define GL_ATTR_COLOR         5    // vec4  (vertex colour)
#define GL_ATTR_BONE_IDS      6    // ivec4 (skinning)
#define GL_ATTR_BONE_WEIGHTS  7    // vec4  (skinning)
#define GL_ATTR_TEXCOORD2     8    // vec2  (UV1: lightmap/AO or a UV1 splat, OR wind data
                                   //        under wind_mode >= 1 -- mutually exclusive uses)
#define GL_ATTR_MORPH        12    // vec3  (CDLOD: parent Y, window start, 1/span)
#define GL_ATTR_MORPH_NORMAL 13    // vec3  (CDLOD: the parent surface's normal)
```

- **There is no bitangent attribute or stream.** The fragment shader rebuilds `B = cross(N, T)`
  and needs only the sign, which rides in `tangent.w`, so a mesh uploads four floats of tangent.
- **A tangent is `d(position)/du` and nothing else.** A frame that looks natural for the mesh (a
  circular tangent on an island) is wrong when the UVs are planar: the normal map runs through a
  basis that disagrees with the map it indexes, and creases along every triangle edge.
- **Slot 4 is free for mesh VAOs, but not unclaimed.** `particle_sim_vert.glsl` binds location 4 as
  `iLife` on the GPU-sim VAO, and the billboard renderer uses 9-11 for per-instance centre, params
  and colour as bare literals. Those are separate VAOs, so nothing conflicts at runtime, but mesh
  geometry on 4 or 9-11 would collide with the convention. 14 and 15 are unclaimed; GL 4.1
  guarantees at least 16.
- **The morph attributes are optional, and absent is off.** An unbound attribute reads (0,0,0), so
  the span reciprocal and the morph factor are 0 and every mesh outside a terrain quadtree is an
  exact identity, with no flag to set. The morph window rides the attribute rather than a
  per-level uniform array indexed by a baked level; that costs eight bytes a vertex of per-patch
  constant.

**Anything that moves vertices in `cetra_local_position`** (`include/object_position.glsl`) is a
displacer, which commits it to two things:
- **A CPU bound** in `draw_item_bounds` (`draw_list.c`), or a moving mesh becomes uncullable or,
  worse, is culled while on screen.
- **All five programs that include the chunk** (pbr, pbr_skinned, the depth prepass, shadow depth,
  shadow absorb) must get its uniforms, through `engine_upload_displacement_uniforms`. A program
  that misses one displaces its vertices elsewhere, and under the prepass's one-sided `LEQUAL` that
  deletes the surface rather than shading it wrong, with the gate suite green.

## Texture / Sampler Model

### Material textures

`Material` carries 13 surface texture pointers (albedo, normal, roughness, metalness, AO, emissive,
height, opacity, microsurface, anisotropy, sheen, reflectance, clearcoat normal) and a layered
material's `splat_tex`. `reflectance_tex` is loaded and owned but has no unit and is never bound
(KHR specular colour is not implemented).

**A loader is told three facts as one `TextureDesc`**: `is_srgb`, `TextureAlpha` and `TextureUse`,
through `texture_load_file`, `texture_load_memory` and `texture_load_memory_owned`. They are three
because they disagree in practice: reflectance is colour-like and linear; a decal image is colour
with a real opacity alpha, loaded linear because it lives in the material array; `apps/tree`'s sand
albedo is baked non-sRGB on purpose so its stochastic transform works on stored codes.
`texture_desc(is_srgb)` gives the usual inference and is not a zero initialiser:
`TEXTURE_ALPHA_OPACITY` is the zero of its enum, so a zeroed desc claims a linear image has an
opacity in alpha and dilates a height map.

**`texture_pool_publish` is the one path from decoded pixels to a pooled `Texture`** (spec 11.86):
create, bind, sampler state, formats, upload, insert. The transparent-texel dilate happens in three
places, the async one on a worker thread on purpose, but the decision is `texture_wants_dilate`,
from the caller's `TextureAlpha` (a layer's alpha is a height, a decal's an opacity).
`moon_surface.c` is outside the pool: pure CPU pixels that `sky.c` uploads itself.

**By-example stochastic texturing** (`include/stochastic.glsl`, spec 11.46, per material through
`stochastic_scale`) blends three lattice taps under Heitz and Neyret's histogram transform. **The
normal map rides the same lattice and is blended in slope space, with no transform**: its channels
are a unit vector, so a per-channel transform and blend makes non-unit normals with a distribution
the map never had, while the slopes `-dh/du`, `-dh/dv` are unconstrained and filter linearly, and
the reconstruction restores the unit length.

### Block compression

Storage is block-compressed where the caller says what a texture is (`TextureUse`, spec 11.85).
`is_srgb` alone is false for both a tangent normal and a roughness mask, which want opposite
formats; `import.c`'s `texture_mappings` table is the one place that knows which is which.

- **Normals take BC5, single-channel masks BC4.** Colour takes DXT only under
  `--texture-compress-colour`: BC5 and BC4 cost a fraction of a code, while DXT quantises endpoints
  to RGB565 and is a judgement.
- **It is a VRAM win, not a speed win** (raiden: 81.4 MB to 37.3 with colour on; frame time and
  load time unchanged). Nothing in this engine is texture-fetch bound. The GPU profiler cannot
  price it; only wall clock answers.
- **BC7 does not exist here.** BPTC is core in GL 4.2, Apple stops at 4.1 with no extension, and
  the upload returns `GL_INVALID_ENUM`. RGTC (BC4/BC5) is core since GL 3.0, so it works with no
  extension string and exists on any context the engine can create. S3TC is an extension that may
  be absent: the colour path asks once, warns by name and falls back to uncompressed.
- **`pbr_frag` rebuilds a normal's Z from XY unconditionally**, since no block format carries three
  channels: a no-op on a well-formed map, a repair on a damaged one.
- **Mips are built on the CPU**, because `glGenerateMipmap` cannot fill a compressed chain; sRGB is
  averaged in linear space, which `texture_mean_rgb` depends on.
- **The material texture array is not compressed**: its packed maps hold three independent
  quantities in one image, which BC4 and BC5 cannot, and it holds no normal maps.

### Material units and the material texture array

Material textures bind to units 0-8 (`common.h`, `TEXUNIT_*`, `TEXUNIT_MATERIAL_MAX = 8`):

```
0: albedo   1: normal   2: material array (sampler2DArray)   3: clearcoat normal   4: height (POM)
5: emissive 6: scene colour (refraction resolve, engine-bound)
7: LTC tables, 2-layer sampler2DArray (engine-bound)   8: sheen colour
```

The engine binds 9-15: the Charlie sheen environment cube (9), the CSM depth array or its MSM
replacement (10), the IBL irradiance, prefilter and BRDF LUT (11-13), the GI and probe atlas (14)
and the punctual shadow array (15).

**`MaterialTextureArray`** (`material_texture_array.c/h`, unit 2) holds a scene's unique per-texel
material images:
- the six per-texel masks: roughness, metallic, AO, opacity, microsurface, anisotropy. There is no
  subsurface layer, and the hair strand map rides the anisotropy slot (spec 11.20);
- a layered material's layer maps (albedo + height; packed normal, roughness and AO) and its splat,
  up to `MATERIAL_MAX_LAYERS` of each (spec 11.60);
- decal images, an albedo and an optional packed surface map per mark, up to `DECAL_MAX` (spec
  11.73).

`material_texture_layer_for` dedups by GL id and knows nothing about what a layer means. Rules
that follow:
- **Every layer shares one canonical width and height.**
- **The array is linear RGBA8**, so a layer albedo and a decal albedo are stored undecoded and
  `pbr_frag` decodes after the blend.
- **Anything stored here must survive being averaged**, since the array resamples and mips every
  layer. Anisotropy is a vector field: `.rg` is a grain direction stored as a coherence-weighted
  doubled angle, `.b` a per-strand identity. A direction equals its reverse, so a raw vector would
  average to nothing exactly where the surface is minified.

### The sampler ceiling

**A fragment program may declare 16 samplers, and the driver counts declarations, not uses.** A
declaration whose every read a constant branch folds away still counts; one the preprocessor
removes does not. Each program has its own sixteen, and units may alias freely across programs.

**Within one program, two sampler types on one unit is `INVALID_OPERATION` at draw.** A
`sampler2D` and a `sampler2DArray` (or `sampler3D`, or cube) bound to the same unit number in one
program fail. Aliasing across programs is fine, and so is a unit whose other binding point the
program leaves alone.

**The uber-shader is full at 16**: `sceneColorTex` (6), `albedoTex` (0), `normalTex` (1),
`emissiveTex` (5), `sheenTex` (8), `clearcoatNormalTex` (3), `heightTex` (4), `materialArray` (2),
`ltcTex` (7), `charliePrefilteredMap` (9), `shadowMaps` (10), `irradianceMap`, `prefilteredMap`,
`brdfLUT` (11-13), `giAtlasTex` (14), `punctualShadowMaps` (15).

**A variant is not full** (spec 11.95). Four declarations are gated on the mask with
`#if CETRA_HAS(...)`: `sheenTex` and `charliePrefilteredMap` on sheen, `ltcTex` on area lights (in
`ltc.glsl`), and `heightTex` on parallax, layers or relief. `pbr-0` declares 12. Ask which units a
variant needs before assuming the ledger; the common case is four short of the ceiling.

Gating a declaration is how a feature stops costing variants that cannot use it. It works when:
- **the feature has a bit** in `pbr_features.glsl`, a scene or material fact the CPU can answer
  (clearcoat is decided by a uniform, so `clearcoatNormalTex` stays ungated);
- **every read moves under the preprocessor with the declaration**; a surviving read names
  something that no longer exists, a compile error, which is the good failure;
- **the chunk that declares gates itself** (`ltc.glsl` decides when `ltcTex` exists; a guard
  round the `#include` would take the functions `pbr_frag` calls with it).

The units come back per variant, not globally: a scene using every feature still declares 16. The
win is the unit, not the clock (freeing `heightTex` measured 0 ms; the code bodies gated with it
were where the milliseconds were).

`pbr_frag` also uses ten uniform blocks of GL 4.1's guaranteed twelve, so two remain.

### Fitting a new texture in

Try these in order. Each says when it applies and gives one example.

1. **Does it need a texture at all?** Ask how many bytes it needs and whether it belongs on the
   vertex.
   - *Per-vertex data goes in an attribute* when it is constant per vertex. The CDLOD terrain's
     heights ride `GL_ATTR_MORPH` and `GL_ATTR_MORPH_NORMAL` instead of a height texture, which
     also keeps the drawn surface the one the collider agrees with.
   - *A table goes in a uniform block or uniforms* when it fits in bytes. The whole 2D IES
     distribution is a block of its own; the virtual-texture page table, road segments (geometry
     the shader evaluates), probe descriptors and decal transforms with their froxel masks are
     blocks too, and the emissive-lights feature (spec 11.49) is one `float`. Mind the two
     remaining blocks.
2. **Become a tenant of a texture already declared**, when the image fits the incumbent's shape:
   its format, the material array's canonical size, one mip policy for every layer, and
   coordinates the shader takes from a uniform. Material layers and decal images are layers of
   `materialArray`; the reflection probes' columns are a region of the GI atlas on unit 14, beside
   the GI slots, which works because `giAtlasSize` is a uniform. It is refused when the image would
   promote every other layer to its size: the composite cache owns plain 2D textures for that
   reason.
3. **Fold textures of one shape into one array**, when they share format, size and filtering. A
   cap counted in declarations is a cap on distinct shapes of data: `water_frag`'s six identical
   RGBA16F cascades became one `sampler2DArray`, same memory, filtering and reads, five units back.
   An array has one mip policy, so folding textures with different mip needs costs memory.
4. **Share a declaration with a consumer that provably never runs at the same time.** The
   strongest proof is a material fact: the composite cache's two textures ride
   `albedoTex`/`normalTex`, because the same `layerCount > 0` that arms the cache skips both reads.
   Next is authoring policy: the virtual-texture page pair takes units 3/4
   (`clearcoatNormalTex`/`heightTex`) because a layered material is refused a clearcoat normal, a
   height map and the rain's relief. Next is a pass: the OIT moments ride `sceneColorTex` during
   the accumulate sub-pass.
5. **Alias a unit idle in the pass that needs it.** The cloud shadow map is
   `#define cloudShadowTex sceneColorTex` in the opaque pass, where the refraction resolve is not
   bound; `cloudShadowLight = -1` turns it off wherever the unit has another tenant.
6. **Bake the transform over the original** so the second texture is never read (spec 11.46).
7. **Give it a program of its own**, which has its own sixteen, as water does. Check first that the
   data it wants is not already declared: the layered material system needed no program and no
   unit, because the array it wanted is bound on every draw.

What genuinely cannot be fitted today: light cookies, and sampling the froxel volume from the
transparent pass, where a `sampler3D` has nothing to alias because the refraction resolve is live.

### Other programs' ledgers

- **`water_frag`** declares 15, one unit left. Its unit numbers reuse `pbr_frag`'s freely, each
  chosen so that water declares only one sampler type on it: the foam target is a `sampler2D` on
  15, the punctual array's unit in `pbr_frag`, and water binds that array (for the rain's cover)
  on 9 instead. `water.h` states each unit and why.
- **`tonemap_frag`** has the LUT's `sampler3D` on 11 (see Rules that span files).

## Render Modes

From `common.h`:

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

**Probe coverage is the view for a shiny indoor surface that looks washed out.** Indoors a
reflection should come from a reflection probe; a surface outside every probe box falls back to the
global environment, which in the shaded frame is only a faint grey sheen. Here it is magenta,
scaled by the share that fell through, so indoors any magenta is a miss (a door hung mid-wall,
outside room boxes that stop at the wall's inner face, is the typical case). Outdoors everything is
magenta, correctly. It is in the debug GUI's Render Mode list.
