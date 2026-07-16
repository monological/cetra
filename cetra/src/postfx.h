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
} PostFXTonemapMode;

// Present an intermediate buffer instead of the composited scene (one view
// at a time by construction; values match the shader's debugView dispatch)
typedef enum PostFXDebugView {
    POSTFX_DEBUG_NONE = 0,
    POSTFX_DEBUG_AO = 1,      // Blurred SSAO buffer
    POSTFX_DEBUG_NORMALS = 2, // Resolved normals G-buffer
    POSTFX_DEBUG_SSR = 3,     // Half-res reflection buffer
} PostFXDebugView;

typedef struct PostFX {
    int width, height;             // Internal (supersampled) HDR size
    int out_width, out_height;     // Display size the final pass downsamples to
    int bloom_width, bloom_height; // Half-res bloom chain size
    int ssao_width, ssao_height;   // Half-res SSAO size

    GLuint hdr_fbo; // Single-sample resolve target, no depth
    GLuint hdr_texture;
    GLuint bloom_fbo[2]; // Half-res ping-pong pair
    GLuint bloom_texture[2];
    GLuint depth_fbo; // Full-res resolved scene depth (blit target)
    GLuint depth_texture;
    GLuint normal_fbo; // Full-res resolved view-space normals + roughness
    GLuint normal_texture;
    GLuint ssao_fbo[2]; // Half-res: [0] raw AO, [1] blurred AO
    GLuint ssao_texture[2];
    GLuint noise_texture; // 4x4 random kernel rotations, tiled
    GLuint ssr_fbo;       // Half-res reflection buffer (march target)
    GLuint ssr_texture;
    GLuint velocity_fbo; // Full-res resolved screen-space motion vectors (.xy)
    GLuint velocity_texture;
    GLuint taa_history_fbo[2]; // Full-res history ping-pong (previous resolved frames)
    GLuint taa_history_texture[2];

    ShaderProgram* bright_program;
    ShaderProgram* blur_program;
    ShaderProgram* tonemap_program;
    ShaderProgram* ssao_program;
    ShaderProgram* ssao_blur_program;
    ShaderProgram* ssr_program;
    ShaderProgram* ssr_composite_program;
    ShaderProgram* taa_resolve_program;

    GLuint quad_vao;
    GLuint quad_vbo;

    float exposure;
    float bloom_threshold;      // Linear luminance where bloom starts
    float bloom_knee;           // Soft-knee width around the threshold
    float bloom_max_brightness; // Firefly clamp on the bloom input
    float bloom_strength;
    bool bloom_enabled;
    int blur_iterations; // Each iteration is one horizontal + one vertical pass
    bool ssao_enabled;
    float ssao_radius; // Occlusion reach in view-space units
    float ssao_strength;
    float ssao_bias;
    bool normals_enabled; // Master switch for the normals G-buffer (MRT)
    bool ssr_enabled;
    float ssr_strength;        // Composite multiplier on the reflections
    float ssr_max_distance;    // March length in view-space units
    float ssr_thickness;       // Accepted depth gap behind a surface
    int ssr_steps;             // Linear march steps
    float ssr_max_roughness;   // Reflections fade out toward this roughness
    float ssr_floor_roughness; // Roughness the shadow catcher publishes
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
                bool normals_written, bool velocity_written, mat4 projection);

// Producer-side predicate: true when some active effect will consume the
// normals G-buffer, so the scene pass should write color attachment 1. The
// engine samples this at frame start and hands the result to postfx_run.
bool postfx_wants_normals(const PostFX* fx);

// The single "SSR runs this frame" predicate (enabled + normals produced).
// The postfx pass and the shadow catcher's floor marker both derive from
// it so they cannot disagree about whether the floor is reflected.
bool postfx_ssr_active(const PostFX* fx, bool normals_written);

#endif // _POSTFX_H_
