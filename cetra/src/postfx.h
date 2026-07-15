#ifndef _POSTFX_H_
#define _POSTFX_H_

#include <GL/glew.h>
#include <stdbool.h>

#include "program.h"

/*
 * Post-processing stack: the scene renders in linear HDR into the engine's
 * multisampled RGBA16F framebuffer; postfx_run resolves it, extracts and
 * blurs bright pixels at half resolution (bloom), then composites and tone
 * maps (exposure + ACES + gamma) in a final fullscreen pass to the target
 * framebuffer. Overlays drawn after the run (GUI) are not tone mapped.
 */

typedef enum PostFXTonemapMode {
    POSTFX_TONEMAP_PASSTHROUGH = 0, // Raw copy for display-ready LDR frames
    POSTFX_TONEMAP_ACES = 1,        // Exposure + ACES fit + gamma 2.2
} PostFXTonemapMode;

typedef struct PostFX {
    int width, height;             // Full-res HDR size (engine framebuffer)
    int bloom_width, bloom_height; // Half-res bloom chain size

    GLuint hdr_fbo; // Single-sample resolve target, no depth
    GLuint hdr_texture;
    GLuint bloom_fbo[2]; // Half-res ping-pong pair
    GLuint bloom_texture[2];

    ShaderProgram* bright_program;
    ShaderProgram* blur_program;
    ShaderProgram* tonemap_program;

    GLuint quad_vao;
    GLuint quad_vbo;

    float exposure;
    float bloom_threshold; // Linear luminance where bloom starts
    float bloom_knee;      // Soft-knee width around the threshold
    float bloom_strength;
    bool bloom_enabled;
    int blur_iterations; // Each iteration is one horizontal + one vertical pass
    PostFXTonemapMode tonemap_mode;
} PostFX;

PostFX* create_postfx(int width, int height);
void free_postfx(PostFX* fx);

// Resolve msaa_fbo, run bloom, and tone map into target_fbo (0 = default
// framebuffer). mode overrides fx->tonemap_mode for this frame (pass
// POSTFX_TONEMAP_PASSTHROUGH for debug render modes).
void postfx_run(PostFX* fx, GLuint msaa_fbo, GLuint target_fbo, PostFXTonemapMode mode);

#endif // _POSTFX_H_
