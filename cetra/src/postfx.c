#include <stdlib.h>

#include "postfx.h"
#include "uniform.h"
#include "util.h"

#include "ext/log.h"

// Creates a single-sample color-only FBO; returns false on failure
static bool create_color_fbo(int width, int height, GLenum internal_format, GLuint* out_fbo,
                             GLuint* out_texture) {
    GLenum format = GL_RGB;
    if (internal_format == GL_RGBA16F)
        format = GL_RGBA;
    else if (internal_format == GL_R8)
        format = GL_RED;

    glGenTextures(1, out_texture);
    glBindTexture(GL_TEXTURE_2D, *out_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal_format, width, height, 0, format, GL_FLOAT,
                 NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Clamp so bloom sampling doesn't wrap around screen edges
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, out_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *out_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *out_texture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("PostFX framebuffer is not complete (%dx%d)", width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

// Depth-only FBO used as the blit target when resolving the MSAA depth
// buffer. The format must match the engine's GL_DEPTH24_STENCIL8 exactly
// (multisample blits require identical formats), and a color-less FBO is
// only complete with its draw/read buffers set to GL_NONE (see shadow.c).
static bool create_depth_fbo(int width, int height, GLuint* out_fbo, GLuint* out_texture) {
    glGenTextures(1, out_texture);
    glBindTexture(GL_TEXTURE_2D, *out_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0, GL_DEPTH_STENCIL,
                 GL_UNSIGNED_INT_24_8, NULL);
    // NEAREST: interpolated depth across silhouettes is meaningless
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, out_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *out_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                           *out_texture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("PostFX depth framebuffer is not complete (%dx%d)", width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

// Deterministic PRNG (xorshift32): the SSAO kernel and noise must be
// bit-identical across runs so headless screenshots stay comparable
static float prng_float(unsigned int* state) {
    unsigned int x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return (float)(x & 0xFFFFFF) / (float)0x1000000; // [0, 1)
}

// Hemisphere kernel (+Z) with samples packed toward the center so nearby
// geometry contributes more occlusion than the radius fringe
static void generate_ssao_kernel(float* kernel, int count, unsigned int* rng) {
    for (int i = 0; i < count; i++) {
        vec3 sample = {prng_float(rng) * 2.0f - 1.0f, prng_float(rng) * 2.0f - 1.0f,
                       prng_float(rng)};
        glm_vec3_normalize(sample);
        glm_vec3_scale(sample, prng_float(rng), sample);

        float t = (float)i / (float)count;
        float scale = 0.1f + 0.9f * t * t;
        glm_vec3_scale(sample, scale, sample);

        kernel[i * 3] = sample[0];
        kernel[i * 3 + 1] = sample[1];
        kernel[i * 3 + 2] = sample[2];
    }
}

static GLuint create_ssao_noise_texture(unsigned int* rng) {
    // 4x4 random rotation vectors in the XY plane, tiled across the screen
    float noise[16 * 2];
    for (int i = 0; i < 16; i++) {
        noise[i * 2] = prng_float(rng) * 2.0f - 1.0f;
        noise[i * 2 + 1] = prng_float(rng) * 2.0f - 1.0f;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, 4, 4, 0, GL_RG, GL_FLOAT, noise);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return texture;
}

#define SSAO_KERNEL_SIZE 24

PostFX* create_postfx(int width, int height) {
    if (width <= 0 || height <= 0) {
        log_error("create_postfx: invalid size %dx%d", width, height);
        return NULL;
    }

    PostFX* fx = calloc(1, sizeof(PostFX));
    if (!fx) {
        log_error("Failed to allocate memory for PostFX");
        return NULL;
    }

    fx->width = width;
    fx->height = height;
    fx->bloom_width = width / 2 > 0 ? width / 2 : 1;
    fx->bloom_height = height / 2 > 0 ? height / 2 : 1;
    fx->ssao_width = fx->bloom_width;
    fx->ssao_height = fx->bloom_height;

    fx->exposure = 1.0f;
    fx->bloom_threshold = 1.0f;
    fx->bloom_knee = 0.5f;
    fx->bloom_max_brightness = 8.0f;
    fx->bloom_strength = 0.08f;
    fx->bloom_enabled = true;
    fx->blur_iterations = 2;
    fx->ssao_enabled = true;
    fx->ssao_radius = 0.4f;
    fx->ssao_strength = 0.8f;
    // Small: the model's crevices are centimeter-scale, and a large bias
    // (the textbook 0.025) erases exactly the shallow occlusion we want
    fx->ssao_bias = 0.008f;
    fx->normals_enabled = true;
    fx->debug_view = POSTFX_DEBUG_NONE;
    fx->tonemap_mode = POSTFX_TONEMAP_NEUTRAL;

    // The HDR resolve target must be RGBA16F to match the MSAA source
    // (multisample blits require identical formats); the bloom chain never
    // reads alpha, so the cheaper packed-float format halves its bandwidth
    if (!create_color_fbo(fx->width, fx->height, GL_RGBA16F, &fx->hdr_fbo, &fx->hdr_texture)) {
        free_postfx(fx);
        return NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (!create_color_fbo(fx->bloom_width, fx->bloom_height, GL_R11F_G11F_B10F,
                              &fx->bloom_fbo[i], &fx->bloom_texture[i])) {
            free_postfx(fx);
            return NULL;
        }
    }

    if (!create_depth_fbo(fx->width, fx->height, &fx->depth_fbo, &fx->depth_texture)) {
        free_postfx(fx);
        return NULL;
    }
    // Resolve target for the scene pass's second color attachment
    // (view-space normals + roughness); RGBA16F to match the MSAA source
    if (!create_color_fbo(fx->width, fx->height, GL_RGBA16F, &fx->normal_fbo,
                          &fx->normal_texture)) {
        free_postfx(fx);
        return NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (!create_color_fbo(fx->ssao_width, fx->ssao_height, GL_R8, &fx->ssao_fbo[i],
                              &fx->ssao_texture[i])) {
            free_postfx(fx);
            return NULL;
        }
    }

    unsigned int rng = 0x9E3779B9u;
    fx->noise_texture = create_ssao_noise_texture(&rng);

    fx->bright_program = create_bloom_bright_program();
    fx->blur_program = create_bloom_blur_program();
    fx->tonemap_program = create_tonemap_program();
    fx->ssao_program = create_ssao_program();
    fx->ssao_blur_program = create_ssao_blur_program();
    if (!fx->bright_program || !fx->blur_program || !fx->tonemap_program || !fx->ssao_program ||
        !fx->ssao_blur_program) {
        free_postfx(fx);
        return NULL;
    }

    // Sampler bindings never change; set them once on the program objects
    glUseProgram(fx->bright_program->id);
    uniform_set_int(fx->bright_program->uniforms, "hdrTex", 0);
    glUseProgram(fx->blur_program->id);
    uniform_set_int(fx->blur_program->uniforms, "image", 0);
    glUseProgram(fx->tonemap_program->id);
    uniform_set_int(fx->tonemap_program->uniforms, "hdrTex", 0);
    uniform_set_int(fx->tonemap_program->uniforms, "bloomTex", 1);
    uniform_set_int(fx->tonemap_program->uniforms, "aoTex", 2);
    uniform_set_int(fx->tonemap_program->uniforms, "normalsTex", 3);

    glUseProgram(fx->ssao_program->id);
    uniform_set_int(fx->ssao_program->uniforms, "depthTex", 0);
    uniform_set_int(fx->ssao_program->uniforms, "noiseTex", 1);
    uniform_set_int(fx->ssao_program->uniforms, "normalsTex", 2);
    const float noise_scale[2] = {(float)fx->ssao_width / 4.0f, (float)fx->ssao_height / 4.0f};
    uniform_set_vec2(fx->ssao_program->uniforms, "noiseScale", noise_scale);
    float kernel[SSAO_KERNEL_SIZE * 3];
    generate_ssao_kernel(kernel, SSAO_KERNEL_SIZE, &rng);
    GLint samples_loc = uniform_location(fx->ssao_program->uniforms, "samples[0]");
    if (samples_loc >= 0) {
        glUniform3fv(samples_loc, SSAO_KERNEL_SIZE, kernel);
    }

    glUseProgram(fx->ssao_blur_program->id);
    uniform_set_int(fx->ssao_blur_program->uniforms, "aoTex", 0);
    const float ao_texel[2] = {1.0f / (float)fx->ssao_width, 1.0f / (float)fx->ssao_height};
    uniform_set_vec2(fx->ssao_blur_program->uniforms, "texelSize", ao_texel);
    glUseProgram(0);

    create_fullscreen_quad_vao(&fx->quad_vao, &fx->quad_vbo);

    check_gl_error("create_postfx");
    return fx;
}

void free_postfx(PostFX* fx) {
    if (!fx)
        return;

    glDeleteFramebuffers(1, &fx->hdr_fbo);
    glDeleteTextures(1, &fx->hdr_texture);
    glDeleteFramebuffers(2, fx->bloom_fbo);
    glDeleteTextures(2, fx->bloom_texture);
    glDeleteFramebuffers(1, &fx->depth_fbo);
    glDeleteTextures(1, &fx->depth_texture);
    glDeleteFramebuffers(1, &fx->normal_fbo);
    glDeleteTextures(1, &fx->normal_texture);
    glDeleteFramebuffers(2, fx->ssao_fbo);
    glDeleteTextures(2, fx->ssao_texture);
    glDeleteTextures(1, &fx->noise_texture);

    free_program(fx->bright_program);
    free_program(fx->blur_program);
    free_program(fx->tonemap_program);
    free_program(fx->ssao_program);
    free_program(fx->ssao_blur_program);

    glDeleteVertexArrays(1, &fx->quad_vao);
    glDeleteBuffers(1, &fx->quad_vbo);

    free(fx);
}

bool postfx_wants_normals(const PostFX* fx) {
    if (!fx || !fx->normals_enabled)
        return false;
    // SSAO orients its sample hemisphere with these normals (SSR joins the
    // list when it lands); the debug view forces the buffer on so it can
    // be inspected with every consumer disabled
    return fx->ssao_enabled || fx->debug_view == POSTFX_DEBUG_NORMALS;
}

void postfx_run(PostFX* fx, GLuint msaa_fbo, GLuint target_fbo, bool frame_is_hdr,
                bool normals_written, mat4 projection) {
    if (!fx)
        return;

    PostFXTonemapMode mode = frame_is_hdr ? fx->tonemap_mode : POSTFX_TONEMAP_PASSTHROUGH;

    // Fullscreen composite passes need blending and depth testing off
    GLboolean depth_was_on = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend_was_on = glIsEnabled(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    // Resolve MSAA HDR into the single-sample HDR texture (formats must
    // match exactly for a multisample blit, both are RGBA16F)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msaa_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fx->hdr_fbo);
    glBlitFramebuffer(0, 0, fx->width, fx->height, 0, 0, fx->width, fx->height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    if (mode == POSTFX_TONEMAP_PASSTHROUGH) {
        // Display-ready frame: plain copy to the target (single-sample
        // blits may convert formats), skipping bloom and tone mapping
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fx->hdr_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fbo);
        glBlitFramebuffer(0, 0, fx->width, fx->height, 0, 0, fx->width, fx->height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    } else {
        // Resolve the scene pass's second attachment (normals + roughness)
        // ahead of its consumers (SSAO now, SSR later). The caller reports
        // whether the attachment was written this frame; re-deriving it from
        // fx flags here could disagree with what the scene pass produced.
        bool have_normals = normals_written;
        if (have_normals) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, msaa_fbo);
            glReadBuffer(GL_COLOR_ATTACHMENT1);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fx->normal_fbo);
            glBlitFramebuffer(0, 0, fx->width, fx->height, 0, 0, fx->width, fx->height,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            // Read-buffer selection sticks to the FBO; put attachment 0 back
            // so the next frame's color resolve reads the right buffer
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            check_gl_error("postfx normals resolve");
        }

        if (fx->ssao_enabled) {
            // Resolve depth alongside color so SSAO can reconstruct
            // view-space positions (formats match: both DEPTH24_STENCIL8)
            glBindFramebuffer(GL_READ_FRAMEBUFFER, msaa_fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fx->depth_fbo);
            glBlitFramebuffer(0, 0, fx->width, fx->height, 0, 0, fx->width, fx->height,
                              GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            check_gl_error("postfx depth resolve");

            mat4 inv_projection;
            glm_mat4_inv(projection, inv_projection);

            // Raw occlusion at half res
            glBindFramebuffer(GL_FRAMEBUFFER, fx->ssao_fbo[0]);
            glViewport(0, 0, fx->ssao_width, fx->ssao_height);
            glUseProgram(fx->ssao_program->id);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, fx->depth_texture);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, fx->noise_texture);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, have_normals ? fx->normal_texture : 0);
            uniform_set_int(fx->ssao_program->uniforms, "useNormalsTex", have_normals ? 1 : 0);
            uniform_set_mat4(fx->ssao_program->uniforms, "projection", (float*)projection);
            uniform_set_mat4(fx->ssao_program->uniforms, "invProjection", (float*)inv_projection);
            uniform_set_float(fx->ssao_program->uniforms, "radius", fx->ssao_radius);
            uniform_set_float(fx->ssao_program->uniforms, "bias", fx->ssao_bias);
            draw_fullscreen_quad(fx->quad_vao);

            // 4x4 box blur cancels the rotation-noise tile
            glBindFramebuffer(GL_FRAMEBUFFER, fx->ssao_fbo[1]);
            glUseProgram(fx->ssao_blur_program->id);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, fx->ssao_texture[0]);
            draw_fullscreen_quad(fx->quad_vao);
        }

        if (fx->bloom_enabled) {
            // Bright pass into half-res buffer 0 (linear sampling downsamples)
            glBindFramebuffer(GL_FRAMEBUFFER, fx->bloom_fbo[0]);
            glViewport(0, 0, fx->bloom_width, fx->bloom_height);
            glUseProgram(fx->bright_program->id);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, fx->hdr_texture);
            uniform_set_float(fx->bright_program->uniforms, "threshold", fx->bloom_threshold);
            uniform_set_float(fx->bright_program->uniforms, "knee", fx->bloom_knee);
            uniform_set_float(fx->bright_program->uniforms, "maxBrightness",
                              fx->bloom_max_brightness);
            draw_fullscreen_quad(fx->quad_vao);

            // Separable Gaussian ping-pong; result ends in bloom_texture[0]
            glUseProgram(fx->blur_program->id);
            const float horizontal[2] = {1.0f / (float)fx->bloom_width, 0.0f};
            const float vertical[2] = {0.0f, 1.0f / (float)fx->bloom_height};
            for (int i = 0; i < fx->blur_iterations; i++) {
                glBindFramebuffer(GL_FRAMEBUFFER, fx->bloom_fbo[1]);
                glBindTexture(GL_TEXTURE_2D, fx->bloom_texture[0]);
                uniform_set_vec2(fx->blur_program->uniforms, "direction", horizontal);
                draw_fullscreen_quad(fx->quad_vao);

                glBindFramebuffer(GL_FRAMEBUFFER, fx->bloom_fbo[0]);
                glBindTexture(GL_TEXTURE_2D, fx->bloom_texture[1]);
                uniform_set_vec2(fx->blur_program->uniforms, "direction", vertical);
                draw_fullscreen_quad(fx->quad_vao);
            }
        }

        // Composite + tone map into the target framebuffer
        glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
        glViewport(0, 0, fx->width, fx->height);
        glUseProgram(fx->tonemap_program->id);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, fx->hdr_texture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, fx->bloom_texture[0]);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, fx->ssao_texture[1]);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, have_normals ? fx->normal_texture : 0);
        uniform_set_float(fx->tonemap_program->uniforms, "exposure", fx->exposure);
        uniform_set_float(fx->tonemap_program->uniforms, "bloomStrength", fx->bloom_strength);
        uniform_set_int(fx->tonemap_program->uniforms, "bloomEnabled", fx->bloom_enabled ? 1 : 0);
        uniform_set_int(fx->tonemap_program->uniforms, "aoEnabled", fx->ssao_enabled ? 1 : 0);
        uniform_set_float(fx->tonemap_program->uniforms, "aoStrength", fx->ssao_strength);
        // Suppress debug views whose source buffer was not produced, and
        // say so once per requested view rather than silently every frame
        PostFXDebugView debug_view = fx->debug_view;
        if ((debug_view == POSTFX_DEBUG_AO && !fx->ssao_enabled) ||
            (debug_view == POSTFX_DEBUG_NORMALS && !have_normals)) {
            static PostFXDebugView warned_view = POSTFX_DEBUG_NONE;
            if (warned_view != debug_view) {
                log_warn("debug view %d suppressed: its source buffer is disabled",
                         (int)debug_view);
                warned_view = debug_view;
            }
            debug_view = POSTFX_DEBUG_NONE;
        }
        uniform_set_int(fx->tonemap_program->uniforms, "debugView", (int)debug_view);
        uniform_set_int(fx->tonemap_program->uniforms, "tonemapMode", (int)mode);
        draw_fullscreen_quad(fx->quad_vao);

        glUseProgram(0);
        glActiveTexture(GL_TEXTURE0);
    }

    if (depth_was_on)
        glEnable(GL_DEPTH_TEST);
    if (blend_was_on)
        glEnable(GL_BLEND);
}
