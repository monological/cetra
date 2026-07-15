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

typedef struct PostFX {
    int width, height;             // Full-res HDR size (engine framebuffer)
    int bloom_width, bloom_height; // Half-res bloom chain size
    int ssao_width, ssao_height;   // Half-res SSAO size

    GLuint hdr_fbo; // Single-sample resolve target, no depth
    GLuint hdr_texture;
    GLuint bloom_fbo[2]; // Half-res ping-pong pair
    GLuint bloom_texture[2];
    GLuint depth_fbo; // Full-res resolved scene depth (blit target)
    GLuint depth_texture;
    GLuint ssao_fbo[2]; // Half-res: [0] raw AO, [1] blurred AO
    GLuint ssao_texture[2];
    GLuint noise_texture; // 4x4 random kernel rotations, tiled

    ShaderProgram* bright_program;
    ShaderProgram* blur_program;
    ShaderProgram* tonemap_program;
    ShaderProgram* ssao_program;
    ShaderProgram* ssao_blur_program;

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
    bool ssao_debug;   // Present the raw AO buffer instead of the scene
    float ssao_radius; // Occlusion reach in view-space units
    float ssao_strength;
    float ssao_bias;
    PostFXTonemapMode tonemap_mode;
} PostFX;

PostFX* create_postfx(int width, int height);
void free_postfx(PostFX* fx);

// Resolve msaa_fbo, run SSAO and bloom, and tone map (fx->tonemap_mode) into
// target_fbo (0 = default framebuffer). projection is the camera projection
// used to render the frame (needed to reconstruct positions from depth).
// Pass frame_is_hdr = false for frames whose shaders already emitted
// display-ready colors (debug render modes): they are copied unchanged,
// skipping SSAO, bloom, and tone mapping.
void postfx_run(PostFX* fx, GLuint msaa_fbo, GLuint target_fbo, bool frame_is_hdr, mat4 projection);

#endif // _POSTFX_H_
