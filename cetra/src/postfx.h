#ifndef _POSTFX_H_
#define _POSTFX_H_

#include <GL/glew.h>
#include <stdbool.h>
#include <cglm/cglm.h>

#include "exposure.h"
#include "program.h"

// Max distinct per-material SSS scatter profiles per scene. C-side only: the
// gather takes one profile per walk, so no shader mirrors this array.
#define MAX_SSS_PROFILES 8

// The scatter pyramid stops being sampled while its texels are still small
// against the subject, so the widest term it can deliver is set by that cap
// rather than by a constant. postfx_sss_max_sigma_per_depth reports it; there is
// no mirrored #define, because the number now depends on the target.
#define SSS_MAX_LEVEL_TEXEL_FRACTION 18.0f

/*
 * Post-processing stack: the scene renders in linear HDR into the engine's
 * multisampled RGBA16F framebuffer; postfx_run resolves it, computes
 * screen-space ambient occlusion from the resolved depth, extracts and
 * blurs bright pixels at half resolution (bloom), then composites and tone
 * maps (AO + exposure + tonemap + gamma) in a final fullscreen pass to the
 * target framebuffer. Overlays drawn after the run (GUI) are not tone mapped.
 */

typedef enum PostFXTonemapMode {
    POSTFX_TONEMAP_PASSTHROUGH = 0, // Raw copy for display-ready LDR frames
    POSTFX_TONEMAP_ACES = 1,        // Filmic: high contrast, crushed shadows
    POSTFX_TONEMAP_NEUTRAL = 2,     // Khronos PBR Neutral: faithful shadows/colors
    POSTFX_TONEMAP_AGX = 3,         // AgX: desaturates toward white as radiance
                                    // climbs; no hue skew on saturated highlights
} PostFXTonemapMode;

// A two-target render pair for temporal accumulators (indexed by frame
// parity) and iterative ping-pong blurs (indexed by iteration parity).
// `valid` says tex[] holds a previous frame's accumulation: it is cleared
// whenever a frame skips the accumulator, so re-enabling a temporal effect
// mid-run resets cleanly instead of reading stale or never-written history.
typedef struct PingPong {
    GLuint fbo[2];
    GLuint tex[2];
    bool valid;
} PingPong;

// Present an intermediate buffer instead of the composited scene (one view
// at a time by construction; values match the shader's debugView dispatch)
typedef enum PostFXDebugView {
    POSTFX_DEBUG_NONE = 0,
    POSTFX_DEBUG_AO = 1,      // Blurred SSAO buffer
    POSTFX_DEBUG_NORMALS = 2, // Resolved normals G-buffer
    POSTFX_DEBUG_SSR = 3,     // Half-res reflection buffer
    POSTFX_DEBUG_ALBEDO = 4,  // Resolved albedo G-buffer (SSGI)
    POSTFX_DEBUG_SSGI = 5,    // Raw gathered GI radiance (half-res, SSGI)
    // 6 was the half-res fog in-scatter buffer, retired with the screen-space
    // march (spec 9.5). The gap is deliberate -- the values are the shader's
    // debugView dispatch.
    POSTFX_DEBUG_SPEC_OCC = 7, // AO visibility after specular occlusion
    POSTFX_DEBUG_CONTACT = 8,  // Contact-shadow visibility term (before compositing)
    POSTFX_DEBUG_BENT = 9,     // Bent normal from the AO chain, remapped for display
} PostFXDebugView;

// How the AO factor treats specular. Legacy blends AO toward unoccluded by
// smoothness alone; bent intersects the AO chain's visibility cone with the
// reflection lobe, so a reflection aimed into an occluder stays occluded.
// Split routes ambient specular to its own scene attachment and occludes
// exactly that share in post -- no per-pixel guess at the specular fraction.
typedef enum PostFXSpecOccMode {
    POSTFX_SPEC_OCC_OFF = 0,
    POSTFX_SPEC_OCC_LEGACY = 1,
    POSTFX_SPEC_OCC_BENT = 2,
    POSTFX_SPEC_OCC_SPLIT = 3,
} PostFXSpecOccMode;

// Mirrors MAX_SHADOW_LIGHTS (shadow.h) without postfx learning about the
// shadow system; the publish step fills at most this many caster slots
#define POSTFX_FOG_MAX_LIGHTS 3
// Mirrors SHADOW_CASCADES the same way; fog_light_space layers stride by
// the RUNTIME published cascade count (layer = slot * count + cascade)
#define POSTFX_FOG_CASCADES 3

// Froxel fog volume dimensions (spec 9.5). Fixed, not derived from render
// resolution: the volume covers the camera frustum out to fog_far, so its
// useful density is set by that world-space reach, not by pixel count. Depth
// slices are exponential, matching the cluster grid's Doom-2016 slicing.
// 160*90*64 RGBA16F is ~7.4 MB per volume.
// The AC4 paper's own dimensions. It argues this is sufficient because the
// stored signal is low frequency and the 3D lookup filters across cells, so
// single texels are not visible -- and where that fails, the signal has broken
// the low-frequency assumption and wants band-limiting, not a denser grid.
// Defaults for the froxel_grid_* fields, which is where a scene overrides them.
#define POSTFX_FROXEL_X 160
#define POSTFX_FROXEL_Y 90
#define POSTFX_FROXEL_Z 64

// Side of the fog's ESM cascades. Deliberately far below the scene's shadow
// resolution and independent of it: the point is to throw the high-frequency
// detail away, and a size that tracked the source would keep whatever the
// source has.
#define POSTFX_FOG_ESM_SIZE 512

typedef struct PostFX {
    int width, height;             // Render size: what the scene and the pre-TAA
                                   // chain rasterize at (post size x render_scale)
    int post_width, post_height;   // Post size (display x ss_scale): the TAAU
                                   // output and everything downstream of it
    int out_width, out_height;     // Display size the final pass downsamples to
    int half_width, half_height;   // Half-RENDER working size (AO chain, SSGI,
                                   // half-res SSR, DoF gather)
    int bloom_width, bloom_height; // Half-POST bloom chain size
    float render_scale;            // The scale the current targets were built with (at
                                   // create, or at the last postfx_resize). For display;
                                   // ask postfx_taau_active whether the resolve is live.

    GLuint hdr_fbo; // Single-sample resolve target, no depth
    GLuint hdr_texture;
    GLuint bloom_fbo;     // One FBO, re-attached per pyramid level (hiz idiom)
    GLuint bloom_texture; // Mipped pyramid: level 0 at bloom_width/height,
                          // halving to ~8-16 px; tonemap magnifies level 0
    int bloom_mips;
    GLuint depth_fbo; // Full-res resolved scene depth (blit target)
    GLuint depth_texture;
    GLuint normal_fbo; // Full-res resolved view-space normal .xyz + SSR marker .a
    GLuint normal_texture;
    GLuint ssao_fbo[2]; // Half-res: [0] raw AO + bent normal, [1] blurred
    GLuint ssao_texture[2];
    PingPong ao_history;    // Half-res temporal-AO accumulation (RGBA16F)
    GLuint ssgi_gi_texture; // Half-res RGBA16F GI radiance, MRT attachment 1 on the GTAO FBO (SSGI)
    PingPong ssgi_history;  // Half-res temporal-GI accumulation (RGBA16F)
    PingPong ssgi_atrous;   // Half-res a-trous denoise ping-pong (RGBA16F)
    bool ssgi_ready;        // Lazy-alloc guard: GI target + the two pairs above
    GLuint noise_texture;   // 4x4 random slice rotations, tiled
    GLuint ssr_fbo;         // Reflection buffer (march target); full-res by
    GLuint ssr_texture;     // default, half-res when ssr_full_res is off
    PingPong ssr_history;   // Temporal-SSR accumulation (RGBA16F): averages the jittered march
                            // across frames so the single-frame step banding washes out (TAA only)
    PingPong ssr_atrous;    // SSR a-trous denoise ping-pong (RGBA16F): resolves the stochastic
                            // march's per-pixel noise into a clean reflection in a single frame
    GLuint hiz_fbo;         // Min-depth pyramid build target (re-attached per mip)
    GLuint hiz_texture;     // R32F, SSR-res base + full mip chain; the SSR
                            // traversal walks it so rays cannot step over thin
                            // geometry regardless of march length
    int hiz_mips;
    GLuint aux_fbo; // Full-res resolved aux G-buffer: motion vectors .xy (TAA) + linear view-Z .z
                    // (GTAO)
    GLuint aux_texture;
    GLuint albedo_fbo; // Full-res resolved base color / albedo (attachment 3) for SSGI composite
    GLuint albedo_texture;
    GLuint lum_fbo; // 64x64 log2-luminance measure target, mipmapped each frame (auto-exposure)
    GLuint lum_texture;
    PingPong taa_history; // Post-res history (previous resolved frames)
    // TAAU canvas (render_scale < 1 only, 0 otherwise): the post-res buffer
    // the seam brings the render-res frame up to. At full scale the hdr
    // buffer is post-sized already and serves as the canvas itself.
    GLuint post_fbo, post_texture;
    // This frame's TAA jitter offset in RENDER pixels (Halton - 0.5 per
    // axis), published by the scene pass; the TAAU resolve un-applies it to
    // place the frame's samples on the display grid. Zero when no jitter is
    // live.
    float taau_jitter_px[2];

    ShaderProgram* bloom_bright_program;
    ShaderProgram* bloom_down_program; // Pyramid 13-tap downsample
    ShaderProgram* bloom_up_program;   // Pyramid tent upsample (additive)
    ShaderProgram* tonemap_program;
    ShaderProgram* gtao_program;
    ShaderProgram* ssao_blur_program;
    ShaderProgram* contact_shadow_program; // AO-res depth march toward the key light (spec 9.3)
    ShaderProgram* temporal_accum_program; // Shared plain-RGBA accumulator (AO .r, fog .rgba)
    ShaderProgram* ssgi_composite_program;
    ShaderProgram* ssgi_accum_program;
    ShaderProgram* ssgi_atrous_program;
    ShaderProgram* ssr_atrous_program; // Edge-aware a-trous denoise for the SSR reflection buffer
    ShaderProgram* ssr_accum_program;  // SSR's own accumulator: inverse-luma blend (10.7.2)
    ShaderProgram* lum_measure_program;
    ShaderProgram* ssr_program;
    ShaderProgram* ssr_hiz_program;
    ShaderProgram* upsample_tent_program;    // Shared tent composite (bloom mips, SSR)
    ShaderProgram* froxel_inject_program;    // Per-cell scattering into the volume (spec 9.5)
    ShaderProgram* froxel_integrate_program; // Front-to-back gather along each slice column
    ShaderProgram* froxel_composite_program;
    ShaderProgram* fog_esm_program; // Builds fog_esm_array from the depth cascades // One trilinear
                                    // tap, folded into the HDR scene
    ShaderProgram* taa_resolve_program;
    ShaderProgram* taau_resolve_program; // Compiled the first time render_scale < 1 (at
                                         // create or at a resize), and kept afterward

    GLuint quad_vao;
    GLuint quad_vbo;

    // Borrowed, never owned -- the Engine holds it. The post chain runs the
    // metering passes that FEED exposure but does not decide what it is; see
    // exposure.h for why that split exists. Non-const because the metering pass
    // hands each frame's measured luminance back.
    Exposure* exposure;

    float bloom_threshold;      // Linear luminance where bloom starts
    float bloom_knee;           // Soft-knee width around the threshold
    float bloom_max_brightness; // Firefly clamp on the bloom input
    float bloom_strength;
    bool bloom_enabled;

    // Lens flare (spec 11.21): ghosts of a bright source, mirrored through
    // frame centre, composited additively in the tonemap beside bloom. Reads
    // the bloom pyramid; see lens_flare_frag.glsl for why and how.
    bool flare_enabled;
    float flare_strength;
    int flare_ghosts;
    float flare_ghost_spacing;       // Fraction of the centre vector between ghosts
    float flare_halo_width;          // Halo ring radius in UV units
    float flare_chroma;              // Per-channel radial offset; 0 = achromatic
    float flare_source_lod;          // Pyramid level; a mid mip, clamped to what exists
    bool flare_ready;                // Lazy-alloc guard for the target below
    GLuint flare_fbo, flare_texture; // Quarter post-res
    int flare_width, flare_height;
    ShaderProgram* flare_program;
    bool ssao_enabled;
    float ssao_radius; // Occlusion reach in view-space units
    float ssao_strength;
    PostFXSpecOccMode spec_occlusion_mode; // How the AO factor treats specular (see the enum)
    bool ao_edge_filter_enabled; // Depth-bilateral AO blur (no silhouette bleed onto the floor)

    // Screen-space contact shadows (spec 9.3): an AO-res depth march toward the
    // key light, composited in tonemap as a direct-light occlusion multiplier
    // beside AO. Needs a shadow-casting directional (fog_light_dir[0]) but no
    // shadow map. Off by default; enabled false leaves the frame untouched.
    // Targets are lazily allocated (cs_ready) and freed unconditionally.
    bool contact_shadows_enabled;
    float cs_strength; // Composite darkening weight [0,1]
    float cs_distance; // March reach in view-space units (0 = off, C-gated)
    bool cs_ready;     // Lazy-alloc guard for the targets below
    GLuint cs_fbo[2];  // R8 at AO res: [0] raw march, [1] bilateral-blurred
    GLuint cs_texture[2];
    PingPong cs_history;  // R16F temporal accumulation (0.9 feedback bands in 8 bits)
    bool ssgi_enabled;    // Screen-space GI: one-bounce indirect diffuse (extends the GTAO sweep)
    float ssgi_intensity; // Composite multiplier on the gathered indirect radiance
    bool normals_enabled; // Master switch for the normals G-buffer (MRT)
    bool ssr_enabled;
    float ssr_strength;        // Composite multiplier on the reflections
    float ssr_max_distance;    // March length in view-space units
    float ssr_thickness_min;   // Acceptance-slab floor behind a surface (view units)
    float ssr_max_roughness;   // Reflections fade out toward this roughness
    float ssr_floor_roughness; // Roughness the shadow catcher publishes
    bool ssr_temporal;         // Temporally accumulate the reflection (needs TAA; averages march
                               // step-banding into a smooth reflection)
    bool ssr_denoise;          // SSR denoiser: stochastic march (grid stripes -> noise) + spatial
                      // a-trous, so temporal/a-trous can resolve it into a clean reflection
    float ssr_jitter;  // Base stochastic ray-jitter spread (floor under roughness)
    bool ssr_full_res; // Trace SSR at full res (sharp; kills the half-res march's serrated
                       // reflection edges); off = half-res, byte-identical to the old path

    // Local reflection probe, filled per frame by the engine from the current
    // scene (postfx never learns about Scene): the SSR pass samples it where
    // the march has no answer. probe_enabled false leaves SSR bit-identical.
    bool probe_enabled;
    GLuint probe_cubemap; // prefiltered capture
    vec3 probe_pos;       // world capture origin
    vec3 probe_box_min;   // world parallax proxy AABB
    vec3 probe_box_max;
    float probe_max_lod;
    float probe_intensity;

    // Volumetric fog (spec 9.5): a camera-frustum froxel volume gathers
    // single-scattered light from the shadow casters, the clustered local
    // lights, and an ambient term through an exponential height-fog density,
    // composited into the HDR scene before DoF/bloom as
    // scene*transmittance + inscatter. Off by default; fog_enabled false leaves
    // the frame untouched. World-space parameters are scene-scaled by apps.
    bool fog_enabled;
    float fog_density;        // Extinction at floor height (1/world units)
    float fog_height_falloff; // World units for a 1/e density drop
    float fog_floor_y;        // World height of max density; no medium below it
    float fog_near;           // Near end of the volume's depth range; 0 = derive from fog_far
    float fog_far;            // Far end of the volume's depth range: where its slices are spent
    float fog_depth_dist;     // Slice bias on top of the exponential; 1 = pure exponential,
                              // >1 bunches slices toward fog_far, <1 toward the camera
    float fog_temporal_blend; // Weight the froxel accumulator gives its history
    // Volume dimensions, defaulted from the POSTFX_FROXEL_* constants. Runtime
    // because XY is what resolves a beam's SILHOUETTE: the volume traces it at
    // this density and the composite can only put a one-cell ramp under each
    // step, so anything sharp enough to clip at the tonemap shows the grid.
    // Changing any of them reallocates the volumes at the next fog frame.
    int froxel_grid_x;
    int froxel_grid_y;
    int froxel_grid_z;
    int froxel_built_x; // Dimensions the live volumes were allocated at
    int froxel_built_y;
    int froxel_built_z;
    float fog_anisotropy;     // Henyey-Greenstein g (forward scattering)
    float fog_sun_boost;      // Artistic multiplier on the shaft in-scatter
    vec3 fog_ambient;         // Isotropic ambient in-scatter radiance
    bool froxel_ready;        // Lazy-alloc guard for the volumes below
    GLuint froxel_fbo;        // Attachment-less; a layer is bound per slice draw
    GLuint froxel_scatter[2]; // RGBA16F volumes: in-scatter radiance + extinction, indexed by
                              // frame parity so a frame reprojects against the previous one
                              // without copying a whole volume
    GLuint froxel_integrated; // RGBA16F volume: front-to-back inscatter + transmittance
    // The camera the previous froxel frame was built with. PostFX keeps its own
    // copy because engine->prev_view_proj already holds THIS frame's matrix by
    // the time postfx runs (the scene pass stashes it at its end).
    mat4 froxel_prev_view;
    mat4 froxel_prev_proj;
    // frame_index of the last froxel build, -1 = never. Reprojection is legal
    // only against the IMMEDIATELY preceding frame, so this is compared rather
    // than a validity flag: any frame that skipped the volume -- the legacy fog
    // path, a debug render mode bypassing the chain, fog switched off -- breaks
    // the adjacency without needing to remember to clear anything.
    int froxel_prev_frame;
    // The composited 2D fog layer (inscatter.rgb, transmittance.a) and its
    // temporal accumulation, allocated on the first TAA frame with fog on.
    // Distinct from the volume's own accumulator above and gated the opposite
    // way: that one smooths noise generated INSIDE the volume and runs always,
    // this one cancels the jitter the composite inherits from the aux depth and
    // is therefore pointless without TAA.
    bool fog_layer_ready;
    bool fog_layer_failed; // Allocation is one-shot; see the ensure function
    GLuint fog_layer_fbo;
    GLuint fog_layer_texture;
    PingPong fog_layer_history;
    int fog_layer_frame; // Same adjacency test as froxel_prev_frame, -1 = never

    // Aerial perspective (spec 9.6). Published per frame by
    // sky_publish_to_postfx; postfx never learns about Sky. A zero volume is
    // the SINGLE "no aerial perspective" state -- no sky, toggled off, or its
    // allocation failed -- so there is no separate enable flag here to disagree
    // with it. Independent of fog: this is atmosphere, not a local medium, so
    // it applies whether or not volumetric fog is on.
    GLuint aerial_volume; // (in-scatter.rgb, transmittance.a), sky-owned
    float aerial_far;     // the volume's far depth in WORLD units
    int aerial_slices;    // published with the volume rather than mirrored, so
                          // the sky owns its own dimension (the fog cascade
                          // count crosses the same seam the same way)

    // Published per frame by shadow_publish_to_postfx (mirrors the probe
    // block; postfx never learns about the shadow system): the casters'
    // matrices, radiance, and map array. Count 0 = ambient-only fog.
    int fog_light_count;
    int fog_cascade_count; // Layers per caster in fog_light_space (1 = classic)
    mat4 fog_light_space[POSTFX_FOG_MAX_LIGHTS * POSTFX_FOG_CASCADES];
    vec3 fog_light_color[POSTFX_FOG_MAX_LIGHTS]; // color * intensity
    vec3 fog_light_dir[POSTFX_FOG_MAX_LIGHTS];   // normalized travel direction
    GLuint fog_shadow_map_array;                 // 0 when shadows are off
    float fog_shadow_bias;
    // The fog's own shadow representation, built from the array above: small,
    // exponential, and blurred, so the medium gets a soft low-frequency
    // occlusion instead of the surface path's exact binary compare. 0 until the
    // first shadowed fog frame.
    GLuint fog_esm_array;
    GLuint fog_esm_scratch; // Ping target for the separable blur
    GLuint fog_esm_fbo;     // Re-attached per layer
    int fog_esm_layers;     // Layers the pair was allocated for
    float fog_esm_k;        // Exponent scale; the one tuning constant
    bool fog_esm_enabled;   // false falls the medium back to the exact binary tap

    // One volumetric spot light (the flashlight), published alongside the
    // directional casters. The fog march scatters it per step (cone + distance
    // attenuation + phase) to draw a beam shaft. enabled = false -> no-op.
    bool fog_spot_enabled;
    vec3 fog_spot_pos;        // world position
    vec3 fog_spot_dir;        // normalized cone axis (travel direction)
    vec3 fog_spot_color;      // color * intensity
    vec3 fog_spot_atten;      // x = 1/range^2 (0 = unbounded), yz unused
    float fog_spot_cos_inner; // cutOff (cos inner half-angle)
    float fog_spot_cos_outer; // outerCutOff (cos outer half-angle)
    // Perspective spot shadow (Phase 2): occludes the beam by geometry.
    bool fog_spot_shadowed;    // a spot shadow map was rendered this frame
    mat4 fog_spot_light_space; // perspective proj * lookAt from the spot
    // GL_TEXTURE_2D_ARRAY of perspective depth maps (0 = none)
    GLuint fog_punctual_shadow_maps;
    int fog_spot_shadow_layer; // Which layer of it holds this spot

    PostFXDebugView debug_view;
    PostFXTonemapMode tonemap_mode;

    // Finishing grade: a "look" stack applied in the composite pass after tone
    // mapping. All optional; with every toggle off the frame is unchanged.
    bool sharpen_enabled;
    float sharpen_strength; // Unsharp-mask amount
    bool grade_enabled;
    vec3 grade_lift;  // Raises shadows (0 = none)
    vec3 grade_gamma; // Per-channel midtone curve (1 = none)
    vec3 grade_gain;  // Scales highlights (1 = none)
    bool vignette_enabled;
    float vignette_strength; // Edge darkening, 0..1
    float vignette_radius;   // Fraction of the half-diagonal kept bright
    // Chromatic aberration. A lens effect, so it lands on the scene sample
    // before the tonemap rather than in the finishing block with the others.
    bool ca_enabled;
    float ca_strength; // Channel separation at the corner, in pixels
    bool grain_enabled;
    float grain_strength;
    int frame_index; // Copied from engine->total_frames; seeds deterministic grain

    // Temporal anti-aliasing. Consumes the per-pixel velocity buffer and a
    // reprojected history to resolve sub-pixel-jittered frames. History buffers
    // and the resolve program live below (added with the resolve pass).
    bool taa_enabled;

    // Depth of field. Targets are lazily allocated on first enable (dof_ready)
    // so the feature costs no memory while off. CoC + gather run at half the
    // render resolution; the composite is post-res.
    bool dof_enabled;
    bool dof_autofocus;                        // Recompute focus each frame from camera->subject
    float dof_focus_distance;                  // View-space distance kept sharp
    float dof_focus_range;                     // Ramp width to full blur
    float dof_max_coc;                         // Max blur radius, half-res texels
    int dof_blades;                            // Aperture blade count (< 3 = circular)
    float dof_rotation;                        // Aperture rotation, degrees
    bool dof_ready;                            // Lazy-alloc guard for the targets below
    GLuint dof_coc_fbo, dof_coc_texture;       // Half-res: scene + signed CoC in .a
    GLuint dof_gather_fbo;                     // Half-res MRT: far (att0) + near (att1)
    GLuint dof_far_texture;                    // RGBA16F far field + weight
    GLuint dof_near_texture;                   // RGBA16F premultiplied near + coverage
    GLuint dof_fbo, dof_texture;               // Full-res: composited scene
    int dof_tile_w, dof_tile_h;                // Tile-pass resolution (half-res / DOF_TILE, ceil)
    GLuint dof_tile_fbo, dof_tile_texture;     // RG16F per-tile (maxFarCoC, maxNearCoC)
    GLuint dof_dilate_fbo, dof_dilate_texture; // RG16F neighborhood-dilated tile maxima
    ShaderProgram* dof_coc_program;
    ShaderProgram* dof_tile_program;
    ShaderProgram* dof_dilate_program;
    ShaderProgram* dof_gather_program;
    ShaderProgram* dof_composite_program;

    // Motion blur (McGuire velocity reconstruction, 4.15). Reads the aux
    // velocity buffer (.xy) + resolved HDR, gathers along the velocity, and
    // blits the result back into hdr_fbo. Target lazily allocated
    // (motion_blur_ready) so the feature is free while off; off by default, so
    // the pass is skipped and the frame is byte-identical to master.
    bool motion_blur_enabled;
    float motion_blur_scale;                     // Shutter: velocity multiplier (1 = full frame)
    bool motion_blur_ready;                      // Lazy-alloc guard for the targets below
    int motion_blur_tile_w, motion_blur_tile_h;  // Tile-max resolution (width/TILE, ceil)
    GLuint motion_blur_fbo, motion_blur_texture; // Full-res RGBA16F reconstruction scratch
    GLuint motion_blur_tile_fbo, motion_blur_tile_texture;         // RG16F per-tile max velocity
    GLuint motion_blur_neighbor_fbo, motion_blur_neighbor_texture; // RG16F 3x3-tile max velocity
    ShaderProgram* motion_blur_program;
    ShaderProgram* motion_blur_tilemax_program;
    ShaderProgram* motion_blur_neighbormax_program;

    // Screen-space subsurface scattering. Resolves the scene pass's skin-diffuse
    // attachment (4), gathers it through a scale-space pyramid (depth-aware,
    // per-channel), and additive-blends blur - diffuse into the canvas so the
    // diffuse softens while specular stays sharp. Lazily allocated (sss_ready).
    // Off when engine->sss_enabled is off (attachment 4 unwritten -> pass skipped).
    // Per-material scatter profiles: rgb = per-channel scatter weight (skin
    // ~(1,0.3,0.2), red widest), w = world-space blur radius. pbr_frag tags each
    // skin pixel with its material's profile index (in the diffuse alpha); the
    // pyramid seed keeps only pixels matching the walk's own tag. Slot 0 is the
    // default skin profile (used when a scene configures no profiles).
    vec4 sss_profiles[MAX_SSS_PROFILES];
    int sss_profile_count;
    bool sss_ready; // Lazy-alloc guard for the targets below
    GLuint sss_diffuse_fbo,
        sss_diffuse_texture; // Full-res resolve of attachment 4 (skin diffuse D)
    // The gather writes the composite delta (blur - D) here; under TAA it is
    // temporally accumulated (its own history, like fog/SSR) before the additive
    // fold.
    GLuint sss_delta_fbo, sss_delta_texture;
    PingPong sss_history;
    ShaderProgram* sss_gather_program;

    // Scale-space pyramid for the scatter kernel (spec 11.14).
    //
    // A screen-space blur samples its profile at discrete taps, so a kernel wide
    // enough in pixels shows those taps as rings. Pre-filtering is what removes
    // the tap budget from the picture: each of the profile's three Gaussians is
    // read as ONE trilinear tap at a level where its sigma is a few texels, so
    // reach in world units is bounded only by the level count.
    //
    // Level count halves to <= 4 px rather than stopping at a fixed cap the way
    // the bloom chain does. A cap that does not grow with resolution puts the
    // ceiling back in pixels, which is the defect this exists to remove.
    //
    // colour carries coverage-premultiplied diffuse in rgb and coverage in a;
    // depth carries the coverage-weighted mean view depth, which the DOWNSAMPLE
    // reads for its own per-level bilateral. The gather does not sample it.
    // Walked once per profile, so neither texture scales with MAX_SSS_PROFILES.
    int sss_pyr_mips;
    GLuint sss_pyr_fbo; // one FBO, re-attached per level (the hiz idiom)
    GLuint sss_pyr_color_texture;
    GLuint sss_pyr_depth_texture;
    ShaderProgram* sss_pyr_seed_program;
    ShaderProgram* sss_pyr_down_program;

    // Weighted-blended OIT: single-sample resolves of the engine's OIT MSAA
    // accum/revealage attachments (a separate FBO sharing the scene depth), then a
    // resolve shader folds them over the opaque scene in hdr_fbo before TAA/etc.
    // Lazily allocated on the first OIT frame.
    GLuint oit_accum_fbo, oit_accum_texture;         // RGBA16F: sum(color*a*w) + sum(a*w)
    GLuint oit_revealage_fbo, oit_revealage_texture; // R16F: product(1 - a)
    bool oit_ready;
    ShaderProgram* oit_resolve_program;

    // Split spec-occ: single-sample resolve of the scene pass's ambient-specular
    // attachment (7). Lazily allocated on the first split frame. Same packed
    // float as the MSAA source (multisample blits require identical formats).
    bool spec_ready;
    GLuint spec_fbo, spec_texture;
    ShaderProgram* spec_occ_composite_program;
} PostFX;

// width/height are the display (downsample-target) size; ss_scale supersamples
// the internal render + post chain by that integer factor (1 = off).
// render_scale in [0.5, 1] shrinks only the render-res half of the chain (the
// scene + every pass before the TAA seam); 1 leaves every size as it was.
PostFX* create_postfx(int width, int height, int ss_scale, float render_scale);

// The one render-scale clamp. Three doors reach it -- the engine setter, the
// create, and the resize -- and a scale that differed between them would size
// the scene target and the resolve targets differently.
float postfx_clamp_render_scale(float render_scale);

// The render dimension for a post dimension: verbatim at scale 1 (so odd sizes
// cannot perturb the identity path), else rounded to the nearest even size so
// the half-res chains halve exactly. The one formula both the engine's MSAA
// target and the postfx resolve targets derive from -- they must agree.
int postfx_scaled_dim(int post_dim, float render_scale);

// Rebuild every resolution-dependent target at a new display size / ss_scale /
// render scale. Settings, per-frame published state, the SSS profile table and
// the borrowed exposure pointer all survive. The 2D temporal histories are
// reset; the froxel volumes and their adjacency stamp deliberately do not
// (fixed dimensions, and they reproject through their own stored camera).
// The caller must rebuild the engine's MSAA scene target to the matching
// render size FIRST -- the G-buffer resolves are multisample blits, which
// require identical rects. Returns false on an invalid size, a failed
// allocation, or a failed lazy compile of the TAAU resolve, and in every case
// leaves no targets behind: the chain is unusable until a later size succeeds.
bool postfx_resize(PostFX* fx, int width, int height, int ss_scale, float render_scale);
void free_postfx(PostFX* fx);

// Borrow the Engine's exposure. Must be called before the first postfx_run; the
// chain reads camera settings and the adaptation key from it, and writes each
// frame's metered luminance back through exposure_set_adapted_luminance.
void postfx_set_exposure(PostFX* fx, Exposure* exposure);

// Per-material SSS scatter profiles. The app resets the table when it configures
// a scene's skin materials, then adds one profile per distinct skin material
// (color = per-channel scatter weight, radius = world-space blur width); the
// returned slot index is written to material->subsurface_profile so pbr_frag can
// tag each of that material's pixels. Returns -1 if the table is full.
void postfx_reset_sss_profiles(PostFX* fx);
int postfx_add_sss_profile(PostFX* fx, const float* color, float radius);

// Enable the whole finishing stack at a cinematic "film" look (stronger
// vignette, visible grain, extra sharpen, teal-cool shadows / warm highlights).
void postfx_apply_film_look(PostFX* fx);

// Resolve msaa_fbo, run SSAO and bloom, and tone map (fx->tonemap_mode) into
// target_fbo (0 = default framebuffer). projection is the camera projection
// used to render the frame (needed to reconstruct positions from depth).
// Which scene-MRT attachments the scene pass actually wrote this frame -- the
// engine's frame-start decisions, passed through rather than re-derived from
// fx flags that may have changed mid-frame.
typedef struct PostFXGBufferWrites {
    bool normals;   // attachment 1
    bool aux;       // attachment 2 (motion + linear-Z + roughness)
    bool albedo;    // attachment 3
    bool sss;       // attachment 4 (skin diffuse)
    bool spec;      // attachment 7 (split ambient specular)
    GLuint oit_fbo; // 0 when the OIT accumulate did not run this frame
    // Which form the accumulation is in: moment-weighted layers already carry
    // their own transmittance and composite as a straight sum, while
    // weighted-blended ones are a weighted average and must be divided by their
    // own weight first. Adjacent to oit_fbo because it is meaningless when that
    // is 0.
    bool oit_moment_weighted;
} PostFXGBufferWrites;

// Pass frame_is_hdr = false for frames whose shaders already emitted
// display-ready colors (debug render modes): they are copied unchanged,
// skipping SSAO, bloom, and tone mapping.
void postfx_run(PostFX* fx, GLuint msaa_fbo, GLuint target_fbo, bool frame_is_hdr,
                const PostFXGBufferWrites* writes, mat4 projection, mat4 view);

// Producer-side predicate: true when some active effect will consume the
// normals G-buffer, so the scene pass should write color attachment 1. The
// engine samples this at frame start and hands the result to postfx_run.
bool postfx_wants_normals(const PostFX* fx);

// Widest scatter sigma the pyramid can deliver, in WORLD units per unit of view
// depth. Pre-integrated skin (§11.13) supplies whatever the screen-space pass
// cannot, so it has to know where that pass stops.
//
// A function rather than a constant because the ceiling is now a property of
// the chain: it is where the walk stops being sampled, not a number someone
// chose. It is still resolution-independent -- the cap is a fraction of frame
// height and the pixels per world unit are proportional to height, so the two
// cancel and what remains depends only on the vertical FOV.
float postfx_sss_max_sigma_per_depth(const PostFX* fx, mat4 projection);

// Producer-side predicate: true when TAA runs this frame (jitter + velocity
// buffer + resolve all gate on it). Mirrors postfx_wants_normals.
bool postfx_taa_active(const PostFX* fx);

// Producer-side predicate: true when the TAAU upscaling resolve owns the seam
// (the chain was built at a reduced render scale). The jitter site widens its
// Halton sequence on this and the seam dispatches on it, so the sequence and
// the resolve consuming it cannot disagree.
bool postfx_taau_active(const PostFX* fx);

// Producer-side predicate: true when the scene pass should write the aux
// G-buffer (attachment 2: motion .xy for TAA + linear view-Z .z for GTAO).
// Produced whenever TAA needs motion or GTAO needs linear depth.
bool postfx_wants_aux_gbuffer(const PostFX* fx);

// Producer-side predicate: true when the scene pass should write the albedo
// G-buffer (attachment 3), i.e. when SSGI is active and needs it for the
// indirect-diffuse composite.
bool postfx_wants_albedo(const PostFX* fx);

// Producer-side predicate: true when the scene pass should split ambient
// specular out to attachment 7 (spec-occ mode SPLIT).
bool postfx_wants_spec_split(const PostFX* fx);

// The single "SSR runs this frame" predicate (enabled + normals produced).
// The postfx pass and the shadow catcher's floor marker both derive from
// it so they cannot disagree about whether the floor is reflected.
bool postfx_ssr_active(const PostFX* fx, bool normals_written);

// Switch SSR tracing between full-res (sharp) and half-res, reallocating the
// reflection buffer + Hi-Z pyramid at the new resolution. Safe to call at runtime.
void postfx_set_ssr_full_res(PostFX* fx, bool full_res);

#endif // _POSTFX_H_
