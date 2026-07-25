#ifndef _POSTFX_H_
#define _POSTFX_H_

#include <GL/glew.h>
#include <stdbool.h>
#include <cglm/cglm.h>

#include "program.h"

// Max distinct per-material SSS scatter profiles per scene. Mirrored as the
// sssProfiles[] array size in sss_blur_frag.glsl -- keep the two in sync.
#define MAX_SSS_PROFILES 8

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
} PostFXDebugView;

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
#define POSTFX_FROXEL_X 160
#define POSTFX_FROXEL_Y 90
#define POSTFX_FROXEL_Z 64

typedef struct PostFX {
    int width, height;             // Internal (supersampled) HDR size
    int out_width, out_height;     // Display size the final pass downsamples to
    int bloom_width, bloom_height; // Half-res bloom chain size
    int ssao_width, ssao_height;   // Half-res SSAO size

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
    GLuint ssao_fbo[2]; // Half-res: [0] raw AO, [1] blurred AO
    GLuint ssao_texture[2];
    PingPong ao_history;    // Half-res temporal-AO accumulation (R16F)
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
    PingPong lum_adapt;   // 1x1 adapted log2-luminance (eye adaptation)
    PingPong taa_history; // Full-res history (previous resolved frames)

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
    ShaderProgram* lum_measure_program;
    ShaderProgram* lum_adapt_program;
    ShaderProgram* ssr_program;
    ShaderProgram* ssr_hiz_program;
    ShaderProgram* upsample_tent_program;    // Shared tent composite (bloom mips, SSR)
    ShaderProgram* froxel_inject_program;    // Per-cell scattering into the volume (spec 9.5)
    ShaderProgram* froxel_integrate_program; // Front-to-back gather along each slice column
    ShaderProgram* froxel_composite_program; // One trilinear tap, folded into the HDR scene
    ShaderProgram* taa_resolve_program;

    GLuint quad_vao;
    GLuint quad_vbo;

    float exposure;             // Manual exposure; an EV bias when auto_exposure is on
    bool auto_exposure;         // Adapt exposure to the scene's mean luminance
    float auto_exposure_key;    // Target middle gray the mean is mapped to (0.18)
    float bloom_threshold;      // Linear luminance where bloom starts
    float bloom_knee;           // Soft-knee width around the threshold
    float bloom_max_brightness; // Firefly clamp on the bloom input
    float bloom_strength;
    bool bloom_enabled;
    bool ssao_enabled;
    float ssao_radius; // Occlusion reach in view-space units
    float ssao_strength;
    bool spec_occlusion_enabled; // Keep GTAO off specular/reflections (spec-occ at tonemap)
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
    float ssr_thickness;       // Accepted depth gap behind a surface
    int ssr_steps;             // Linear march steps
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
    float fog_far;            // Far end of the volume's depth range: where its slices are spent
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

    // One volumetric spot light (the flashlight), published alongside the
    // directional casters. The fog march scatters it per step (cone + distance
    // attenuation + phase) to draw a beam shaft. enabled = false -> no-op.
    bool fog_spot_enabled;
    vec3 fog_spot_pos;        // world position
    vec3 fog_spot_dir;        // normalized cone axis (travel direction)
    vec3 fog_spot_color;      // color * intensity
    vec3 fog_spot_atten;      // constant, linear, quadratic
    float fog_spot_cos_inner; // cutOff (cos inner half-angle)
    float fog_spot_cos_outer; // outerCutOff (cos outer half-angle)
    // Perspective spot shadow (Phase 2): occludes the beam by geometry.
    bool fog_spot_shadowed;     // a spot shadow map was rendered this frame
    mat4 fog_spot_light_space;  // perspective proj * lookAt from the spot
    GLuint fog_spot_shadow_map; // GL_TEXTURE_2D depth (0 = none)

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
    bool grain_enabled;
    float grain_strength;
    int frame_index; // Copied from engine->total_frames; seeds deterministic grain

    // Temporal anti-aliasing. Consumes the per-pixel velocity buffer and a
    // reprojected history to resolve sub-pixel-jittered frames. History buffers
    // and the resolve program live below (added with the resolve pass).
    bool taa_enabled;

    // Depth of field. Targets are lazily allocated on first enable (dof_ready)
    // so the feature costs no memory while off. CoC + gather run at half the
    // internal resolution; the composite is full-res.
    bool dof_enabled;
    bool dof_autofocus;                    // Recompute focus each frame from camera->subject
    float dof_focus_distance;              // View-space distance kept sharp
    float dof_focus_range;                 // Ramp width to full blur
    float dof_max_coc;                     // Max blur radius, half-res texels
    bool dof_ready;                        // Lazy-alloc guard for the targets below
    GLuint dof_coc_fbo, dof_coc_texture;   // Half-res: scene + signed CoC in .a
    GLuint dof_blur_fbo, dof_blur_texture; // Half-res: gathered blur
    GLuint dof_fbo, dof_texture;           // Full-res: composited scene
    ShaderProgram* dof_coc_program;
    ShaderProgram* dof_blur_program;
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

    // Separable screen-space subsurface scattering. Resolves the scene
    // pass's skin-diffuse attachment (4), blurs it separably (depth-aware,
    // per-channel), and additive-blends blur - diffuse into hdr_fbo so the
    // diffuse softens while specular stays sharp. Lazily allocated (sss_ready).
    // Off when engine->sss_enabled is off (attachment 4 unwritten -> pass skipped).
    // Per-material scatter profiles: rgb = per-channel scatter weight (skin
    // ~(1,0.3,0.2), red widest), w = world-space blur radius. pbr_frag tags each
    // skin pixel with its material's profile index (in the diffuse alpha); the
    // blur reads that per pixel and looks the profile up here. Slot 0 is the
    // default skin profile (used when a scene configures no profiles).
    vec4 sss_profiles[MAX_SSS_PROFILES];
    int sss_profile_count;
    bool sss_ready; // Lazy-alloc guard for the targets below
    GLuint sss_diffuse_fbo,
        sss_diffuse_texture;               // Full-res resolve of attachment 4 (skin diffuse D)
    GLuint sss_blur_fbo, sss_blur_texture; // Full-res H-blur scratch (V pass composites to hdr)
    // Under TAA the V pass writes the composite delta (blur - D) here instead of
    // straight into hdr, so it can be temporally accumulated (its own history, like
    // fog/SSR) before the additive fold; without TAA these stay unused.
    GLuint sss_delta_fbo, sss_delta_texture;
    PingPong sss_history;
    ShaderProgram* sss_blur_program;

    // Weighted-blended OIT: single-sample resolves of the engine's OIT MSAA
    // accum/revealage attachments (a separate FBO sharing the scene depth), then a
    // resolve shader folds them over the opaque scene in hdr_fbo before TAA/etc.
    // Lazily allocated on the first OIT frame.
    GLuint oit_accum_fbo, oit_accum_texture;         // RGBA16F: sum(color*a*w) + sum(a*w)
    GLuint oit_revealage_fbo, oit_revealage_texture; // R16F: product(1 - a)
    bool oit_ready;
    ShaderProgram* oit_resolve_program;
} PostFX;

// width/height are the display (downsample-target) size; ss_scale supersamples
// the internal render + post chain by that integer factor (1 = off).
PostFX* create_postfx(int width, int height, int ss_scale);
void free_postfx(PostFX* fx);

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
// Pass frame_is_hdr = false for frames whose shaders already emitted
// display-ready colors (debug render modes): they are copied unchanged,
// skipping SSAO, bloom, and tone mapping. normals_written reports whether
// the scene pass produced color attachment 1 this frame — the engine's
// frame-start decision, passed through rather than re-derived from fx flags
// that may have changed mid-frame.
void postfx_run(PostFX* fx, GLuint msaa_fbo, GLuint target_fbo, bool frame_is_hdr,
                bool normals_written, bool aux_written, bool albedo_written, bool sss_written,
                GLuint oit_fbo, mat4 projection, mat4 view);

// Producer-side predicate: true when some active effect will consume the
// normals G-buffer, so the scene pass should write color attachment 1. The
// engine samples this at frame start and hands the result to postfx_run.
bool postfx_wants_normals(const PostFX* fx);

// Producer-side predicate: true when TAA runs this frame (jitter + velocity
// buffer + resolve all gate on it). Mirrors postfx_wants_normals.
bool postfx_taa_active(const PostFX* fx);

// Producer-side predicate: true when the scene pass should write the aux
// G-buffer (attachment 2: motion .xy for TAA + linear view-Z .z for GTAO).
// Produced whenever TAA needs motion or GTAO needs linear depth.
bool postfx_wants_aux_gbuffer(const PostFX* fx);

// Producer-side predicate: true when the scene pass should write the albedo
// G-buffer (attachment 3), i.e. when SSGI is active and needs it for the
// indirect-diffuse composite.
bool postfx_wants_albedo(const PostFX* fx);

// The single "SSR runs this frame" predicate (enabled + normals produced).
// The postfx pass and the shadow catcher's floor marker both derive from
// it so they cannot disagree about whether the floor is reflected.
bool postfx_ssr_active(const PostFX* fx, bool normals_written);

// Switch SSR tracing between full-res (sharp) and half-res, reallocating the
// reflection buffer + Hi-Z pyramid at the new resolution. Safe to call at runtime.
void postfx_set_ssr_full_res(PostFX* fx, bool full_res);

#endif // _POSTFX_H_
