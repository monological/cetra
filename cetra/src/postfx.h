#ifndef _POSTFX_H_
#define _POSTFX_H_

#include <GL/glew.h>
#include <stdbool.h>
#include <cglm/cglm.h>

#include "program.h"

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
    POSTFX_DEBUG_AO = 1,       // Blurred SSAO buffer
    POSTFX_DEBUG_NORMALS = 2,  // Resolved normals G-buffer
    POSTFX_DEBUG_SSR = 3,      // Half-res reflection buffer
    POSTFX_DEBUG_ALBEDO = 4,   // Resolved albedo G-buffer (SSGI)
    POSTFX_DEBUG_SSGI = 5,     // Raw gathered GI radiance (half-res, SSGI)
    POSTFX_DEBUG_FOG = 6,      // Half-res fog in-scatter buffer
    POSTFX_DEBUG_SPEC_OCC = 7, // AO visibility after specular occlusion
} PostFXDebugView;

// Mirrors MAX_SHADOW_LIGHTS (shadow.h) without postfx learning about the
// shadow system; the publish step fills at most this many caster slots
#define POSTFX_FOG_MAX_LIGHTS 3
// Mirrors SHADOW_CASCADES the same way; fog_light_space layers stride by
// the RUNTIME published cascade count (layer = slot * count + cascade)
#define POSTFX_FOG_CASCADES 3

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
    GLuint normal_fbo; // Full-res resolved view-space normals + roughness
    GLuint normal_texture;
    GLuint ssao_fbo[2]; // Half-res: [0] raw AO, [1] blurred AO
    GLuint ssao_texture[2];
    PingPong ao_history;    // Half-res temporal-AO accumulation (R16F)
    GLuint ssgi_gi_texture; // Half-res RGBA16F GI radiance, MRT attachment 1 on the GTAO FBO (SSGI)
    PingPong ssgi_history;  // Half-res temporal-GI accumulation (RGBA16F)
    PingPong ssgi_atrous;   // Half-res a-trous denoise ping-pong (RGBA16F)
    bool ssgi_ready;        // Lazy-alloc guard: GI target + the two pairs above
    GLuint noise_texture;   // 4x4 random slice rotations, tiled
    GLuint ssr_fbo;         // Half-res reflection buffer (march target)
    GLuint ssr_texture;
    GLuint hiz_fbo;     // Min-depth pyramid build target (re-attached per mip)
    GLuint hiz_texture; // R32F, half-res base + full mip chain; the SSR
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
    ShaderProgram* temporal_accum_program; // Shared plain-RGBA accumulator (AO .r, fog .rgba)
    ShaderProgram* ssgi_composite_program;
    ShaderProgram* ssgi_accum_program;
    ShaderProgram* ssgi_atrous_program;
    ShaderProgram* lum_measure_program;
    ShaderProgram* lum_adapt_program;
    ShaderProgram* ssr_program;
    ShaderProgram* ssr_hiz_program;
    ShaderProgram* upsample_tent_program; // Shared half-res composite (SSR, fog)
    ShaderProgram* fog_program;
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
    bool spec_occlusion_enabled; // Keep GTAO off specular/reflections (Lagarde spec-occ at tonemap)
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

    // Volumetric fog: a half-res raymarch toward each pixel's depth gathers
    // single-scattered light from the shadow casters plus an ambient term
    // through an exponential height-fog density, composited into the HDR
    // scene before DoF/bloom as scene*transmittance + inscatter. Off by
    // default; fog_enabled false leaves the frame untouched. World-space
    // parameters are scene-scaled by apps.
    bool fog_enabled;
    float fog_density;           // Extinction at floor height (1/world units)
    float fog_height_falloff;    // World units for a 1/e density drop
    float fog_floor_y;           // World height of max density
    float fog_far;               // March length for sky rays / march cap
    float fog_anisotropy;        // Henyey-Greenstein g (forward scattering)
    float fog_sun_boost;         // Artistic multiplier on the shaft in-scatter
    vec3 fog_ambient;            // Isotropic ambient in-scatter radiance
    int fog_steps;               // March steps
    bool fog_ready;              // Lazy-alloc guard for the targets below
    GLuint fog_fbo, fog_texture; // Half-res RGBA16F: inscatter.rgb + transmittance.a
    PingPong fog_history;        // Half-res temporal accumulation

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
} PostFX;

// width/height are the display (downsample-target) size; ss_scale supersamples
// the internal render + post chain by that integer factor (1 = off).
PostFX* create_postfx(int width, int height, int ss_scale);
void free_postfx(PostFX* fx);

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
                bool normals_written, bool aux_written, bool albedo_written, mat4 projection,
                mat4 view);

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

#endif // _POSTFX_H_
