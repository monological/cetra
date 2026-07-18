#include <stdio.h>
#include <stdlib.h>

#include "postfx.h"
#include "uniform.h"
#include "util.h"

#include "ext/log.h"

// Auto-exposure measure-target side; its full mip chain averages down to 1x1
// at level log2(size). Must match MEASURE_TOP_MIP in lum_adapt_frag.glsl.
#define LUM_MEASURE_SIZE 64

// Creates a single-sample color-only FBO; returns false on failure
static bool create_color_fbo(int width, int height, GLenum internal_format, GLuint* out_fbo,
                             GLuint* out_texture) {
    // RGBA is the default; only the single-channel and packed-RGB formats
    // need the narrower pixel-transfer format (data is always NULL anyway).
    GLenum format = GL_RGBA;
    if (internal_format == GL_R8 || internal_format == GL_R16F)
        format = GL_RED;
    else if (internal_format == GL_R11F_G11F_B10F)
        format = GL_RGB;

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

// A ping-pong pair: two color FBOs of the same size/format (see PingPong)
static bool create_pingpong(int width, int height, GLenum internal_format, PingPong* pp) {
    pp->valid = false;
    for (int i = 0; i < 2; i++) {
        if (!create_color_fbo(width, height, internal_format, &pp->fbo[i], &pp->tex[i]))
            return false;
    }
    return true;
}

static void free_pingpong(PingPong* pp) {
    glDeleteFramebuffers(2, pp->fbo);
    glDeleteTextures(2, pp->tex);
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

// Deterministic PRNG (xorshift32): the AO rotation noise must be
// bit-identical across runs so headless screenshots stay comparable
static float prng_float(unsigned int* state) {
    unsigned int x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return (float)(x & 0xFFFFFF) / (float)0x1000000; // [0, 1)
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

PostFX* create_postfx(int width, int height, int ss_scale) {
    if (width <= 0 || height <= 0) {
        log_error("create_postfx: invalid size %dx%d", width, height);
        return NULL;
    }
    if (ss_scale < 1) {
        ss_scale = 1;
    }

    PostFX* fx = calloc(1, sizeof(PostFX));
    if (!fx) {
        log_error("Failed to allocate memory for PostFX");
        return NULL;
    }

    // The scene and the entire post chain render at the supersampled
    // resolution; the final tonemap pass box-downsamples to the display size.
    fx->out_width = width;
    fx->out_height = height;
    fx->width = width * ss_scale;
    fx->height = height * ss_scale;
    fx->bloom_width = fx->width / 2 > 0 ? fx->width / 2 : 1;
    fx->bloom_height = fx->height / 2 > 0 ? fx->height / 2 : 1;
    fx->ssao_width = fx->bloom_width;
    fx->ssao_height = fx->bloom_height;

    fx->exposure = 1.0f;
    fx->auto_exposure = true; // Adapt to scene luminance; apps setting a manual exposure clear this
    fx->auto_exposure_key = 0.18f;
    fx->bloom_threshold = 1.0f;
    fx->bloom_knee = 0.5f;
    fx->bloom_max_brightness = 8.0f;
    // The pyramid's level 0 accumulates every coarser level, carrying
    // several times the energy one blurred buffer did -- the composite
    // strength drops to match
    fx->bloom_strength = 0.015f;
    fx->bloom_enabled = true;
    fx->ssao_enabled = true;
    fx->ssao_radius = 0.4f;
    fx->ssao_strength = 0.8f;
    fx->ssgi_enabled = false; // experimental; off by default
    fx->ssgi_intensity = 1.0f;
    fx->normals_enabled = true;
    fx->ssr_enabled = true;
    fx->ssr_strength = 1.0f;
    fx->ssr_max_distance = 8.0f;
    fx->ssr_thickness = 0.3f;
    fx->ssr_steps = 64;
    // Glossy surfaces only (the floor and polished trim): rougher curved
    // surfaces self-graze their own silhouette in screen space and dash
    fx->ssr_max_roughness = 0.25f;
    fx->ssr_floor_roughness = 0.1f;
    fx->debug_view = POSTFX_DEBUG_NONE;
    fx->tonemap_mode = POSTFX_TONEMAP_NEUTRAL;

    // Finishing grade: a subtle vignette is a safe, pleasing default for a
    // viewer; sharpen/grade/grain are opt-in (grain in particular reads like
    // specular speckle on metal, and a grade is a per-shot artistic choice)
    fx->sharpen_enabled = false;
    fx->sharpen_strength = 0.5f;
    fx->grade_enabled = false;
    glm_vec3_zero(fx->grade_lift);
    glm_vec3_one(fx->grade_gamma);
    glm_vec3_one(fx->grade_gain);
    fx->vignette_enabled = true;
    fx->vignette_strength = 0.25f;
    fx->vignette_radius = 0.6f;
    fx->grain_enabled = false;
    fx->grain_strength = 0.015f;
    fx->frame_index = 0;

    fx->taa_enabled = false; // Enabled per-app (the render app turns it on when windowed)

    // Depth of field (off by default; targets allocated lazily on first enable)
    fx->dof_enabled = false;
    fx->dof_autofocus = true; // Track the camera's subject unless a focus is pinned
    fx->dof_focus_distance = 3.0f;
    fx->dof_focus_range = 1.5f;
    fx->dof_max_coc = 6.0f; // ~12px max blur — a natural background falloff,
                            // not the over-creamy look of a larger radius
    fx->dof_ready = false;

    // Volumetric fog (off by default; targets allocated lazily on first
    // enable). World-space defaults are meter-scale; apps scene-scale them.
    fx->fog_enabled = false;
    fx->fog_density = 0.02f;
    fx->fog_height_falloff = 4.0f;
    fx->fog_floor_y = 0.0f;
    fx->fog_far = 60.0f;
    fx->fog_anisotropy = 0.45f;
    fx->fog_sun_boost = 1.0f;
    glm_vec3_copy((vec3){0.05f, 0.05f, 0.05f}, fx->fog_ambient);
    fx->fog_steps = 24;
    fx->fog_ready = false;

    // The HDR resolve target must be RGBA16F to match the MSAA source
    // (multisample blits require identical formats); the bloom chain never
    // reads alpha, so the cheaper packed-float format halves its bandwidth
    if (!create_color_fbo(fx->width, fx->height, GL_RGBA16F, &fx->hdr_fbo, &fx->hdr_texture)) {
        free_postfx(fx);
        return NULL;
    }
    // Bloom pyramid: one packed-float texture whose mip chain the pyramid
    // passes walk, level 0 at half res down to a ~8-16 px coarsest level.
    // Hand-built chain, so MAX_LEVEL is mandatory (an incomplete chain
    // samples as black); one FBO gets re-attached per level like the hi-z
    // build. MIN filter samples bilinearly WITHIN the level the passes pin.
    {
        int bw = fx->bloom_width;
        int bh = fx->bloom_height;
        fx->bloom_mips = 1;
        while (bw > 15 && bh > 15 && fx->bloom_mips < 8) {
            bw /= 2;
            bh /= 2;
            fx->bloom_mips++;
        }
        glGenTextures(1, &fx->bloom_texture);
        glBindTexture(GL_TEXTURE_2D, fx->bloom_texture);
        int mw = fx->bloom_width;
        int mh = fx->bloom_height;
        for (int mip = 0; mip < fx->bloom_mips; mip++) {
            glTexImage2D(GL_TEXTURE_2D, mip, GL_R11F_G11F_B10F, mw, mh, 0, GL_RGB, GL_FLOAT,
                         NULL);
            mw = mw > 1 ? mw / 2 : 1;
            mh = mh > 1 ? mh / 2 : 1;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, fx->bloom_mips - 1);
        glGenFramebuffers(1, &fx->bloom_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fx->bloom_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               fx->bloom_texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            log_error("Bloom pyramid framebuffer incomplete");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            free_postfx(fx);
            return NULL;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
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
    // Half-res temporal-AO accumulation ping-pong. R16F, not R8: an exponential
    // feedback blend needs more than 256 levels to avoid banding as it converges.
    if (!create_pingpong(fx->ssao_width, fx->ssao_height, GL_R16F, &fx->ao_history)) {
        free_postfx(fx);
        return NULL;
    }
    // The SSGI targets (GI radiance MRT + accumulation/a-trous pairs) are
    // lazily allocated on first enable -- see postfx_ensure_ssgi_targets.
    // Half-res reflection buffer; HDR since it carries scene color
    if (!create_color_fbo(fx->ssao_width, fx->ssao_height, GL_RGBA16F, &fx->ssr_fbo,
                          &fx->ssr_texture)) {
        free_postfx(fx);
        return NULL;
    }
    // Min-depth pyramid for the SSR traversal: half-res base with a full mip
    // chain, each level the min (nearest) of the 2x2 below. R32F: fp16 depth
    // staircases at scene scale. The chain stops nowhere special, but set
    // the max level anyway — an incomplete chain samples as black.
    {
        int hw = fx->ssao_width;
        int hh = fx->ssao_height;
        fx->hiz_mips = 1;
        while ((hw > 1 || hh > 1) && fx->hiz_mips < 16) {
            hw = hw > 1 ? hw / 2 : 1;
            hh = hh > 1 ? hh / 2 : 1;
            fx->hiz_mips++;
        }
        glGenTextures(1, &fx->hiz_texture);
        glBindTexture(GL_TEXTURE_2D, fx->hiz_texture);
        int mw = fx->ssao_width;
        int mh = fx->ssao_height;
        for (int mip = 0; mip < fx->hiz_mips; mip++) {
            glTexImage2D(GL_TEXTURE_2D, mip, GL_R32F, mw, mh, 0, GL_RED, GL_FLOAT, NULL);
            mw = mw > 1 ? mw / 2 : 1;
            mh = mh > 1 ? mh / 2 : 1;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, fx->hiz_mips - 1);
        glGenFramebuffers(1, &fx->hiz_fbo);
    }
    // Full-res resolve target for the scene pass's aux G-buffer (.xy motion +
    // .z linear view-Z), and the two full-res history buffers the TAA resolve
    // ping-pongs across frames.
    // Full float, matching the scene pass's aux attachment: the MSAA resolve
    // blit requires identical formats, and fp16 view-Z staircases at scene
    // scale (banded GTAO on large grounds).
    if (!create_color_fbo(fx->width, fx->height, GL_RGBA32F, &fx->aux_fbo,
                          &fx->aux_texture)) {
        free_postfx(fx);
        return NULL;
    }
    // Point-sample the aux buffer: view-space Z is NOT screen-linear under
    // perspective, so LINEAR filtering would bend flat surfaces (banding) and
    // mangle GTAO's half-res reconstruction on fine geometry (bright speckle).
    // TAA reads it full-res 1:1, where NEAREST and LINEAR coincide.
    glBindTexture(GL_TEXTURE_2D, fx->aux_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // Full-res resolve target for the scene pass's albedo G-buffer (attachment 3),
    // consumed by the SSGI indirect-diffuse composite. RGBA8 (albedo is LDR).
    if (!create_color_fbo(fx->width, fx->height, GL_RGBA8, &fx->albedo_fbo, &fx->albedo_texture)) {
        free_postfx(fx);
        return NULL;
    }
    if (!create_pingpong(fx->width, fx->height, GL_RGBA16F, &fx->taa_history)) {
        free_postfx(fx);
        return NULL;
    }

    // Auto-exposure: a 64x64 log2-luminance measure target whose mip chain is
    // regenerated each frame (top mip = geometric-mean scene luminance), and a
    // 1x1 adapted-luminance ping-pong the eye-adaptation pass blends across
    // frames. R16F: log2 luminance is small-range but needs sub-ulp precision.
    if (!create_color_fbo(LUM_MEASURE_SIZE, LUM_MEASURE_SIZE, GL_R16F, &fx->lum_fbo,
                          &fx->lum_texture)) {
        free_postfx(fx);
        return NULL;
    }
    glBindTexture(GL_TEXTURE_2D, fx->lum_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D); // Allocate the chain up front
    if (!create_pingpong(1, 1, GL_R16F, &fx->lum_adapt)) {
        free_postfx(fx);
        return NULL;
    }

    unsigned int rng = 0x9E3779B9u;
    fx->noise_texture = create_ssao_noise_texture(&rng);

    fx->bright_program = create_bloom_bright_program();
    fx->bloom_down_program = create_bloom_down_program();
    fx->bloom_up_program = create_bloom_up_program();
    fx->tonemap_program = create_tonemap_program();
    fx->gtao_program = create_gtao_program();
    fx->ssao_blur_program = create_ssao_blur_program();
    fx->temporal_accum_program = create_temporal_accum_program();
    fx->ssgi_composite_program = create_ssgi_composite_program();
    fx->ssgi_accum_program = create_ssgi_accum_program();
    fx->ssgi_atrous_program = create_ssgi_atrous_program();
    fx->lum_measure_program = create_lum_measure_program();
    fx->lum_adapt_program = create_lum_adapt_program();
    fx->ssr_program = create_ssr_program();
    fx->ssr_hiz_program = create_ssr_hiz_program();
    fx->upsample_tent_program = create_upsample_tent_program();
    fx->fog_program = create_fog_program();
    fx->taa_resolve_program = create_taa_resolve_program();
    fx->dof_coc_program = create_dof_coc_program();
    fx->dof_blur_program = create_dof_blur_program();
    fx->dof_composite_program = create_dof_composite_program();
    if (!fx->bright_program || !fx->bloom_down_program || !fx->bloom_up_program ||
        !fx->tonemap_program || !fx->gtao_program ||
        !fx->ssao_blur_program || !fx->temporal_accum_program || !fx->ssgi_composite_program ||
        !fx->ssgi_accum_program || !fx->ssgi_atrous_program ||
        !fx->lum_measure_program || !fx->lum_adapt_program || !fx->ssr_program ||
        !fx->upsample_tent_program || !fx->fog_program ||
        !fx->taa_resolve_program || !fx->dof_coc_program ||
        !fx->dof_blur_program || !fx->dof_composite_program) {
        free_postfx(fx);
        return NULL;
    }

    // Sampler bindings never change; set them once on the program objects
    glUseProgram(fx->bright_program->id);
    uniform_set_int(fx->bright_program->uniforms, "hdrTex", 0);
    glUseProgram(fx->bloom_down_program->id);
    uniform_set_int(fx->bloom_down_program->uniforms, "srcTex", 0);
    glUseProgram(fx->bloom_up_program->id);
    uniform_set_int(fx->bloom_up_program->uniforms, "srcTex", 0);
    glUseProgram(fx->tonemap_program->id);
    uniform_set_int(fx->tonemap_program->uniforms, "hdrTex", 0);
    uniform_set_int(fx->tonemap_program->uniforms, "bloomTex", 1);
    uniform_set_int(fx->tonemap_program->uniforms, "aoTex", 2);
    uniform_set_int(fx->tonemap_program->uniforms, "normalsTex", 3);
    uniform_set_int(fx->tonemap_program->uniforms, "ssrTex", 4);
    uniform_set_int(fx->tonemap_program->uniforms, "albedoTex", 5);
    uniform_set_int(fx->tonemap_program->uniforms, "giTex", 6);
    uniform_set_int(fx->tonemap_program->uniforms, "lumTex", 7);
    uniform_set_int(fx->tonemap_program->uniforms, "fogTex", 8);

    glUseProgram(fx->lum_measure_program->id);
    uniform_set_int(fx->lum_measure_program->uniforms, "hdrTex", 0);
    glUseProgram(fx->lum_adapt_program->id);
    uniform_set_int(fx->lum_adapt_program->uniforms, "measureTex", 0);
    uniform_set_int(fx->lum_adapt_program->uniforms, "historyTex", 1);

    glUseProgram(fx->ssr_program->id);
    uniform_set_int(fx->ssr_program->uniforms, "depthTex", 0);
    uniform_set_int(fx->ssr_program->uniforms, "normalsTex", 1);
    uniform_set_int(fx->ssr_program->uniforms, "hdrTex", 2);
    uniform_set_int(fx->ssr_program->uniforms, "probeTex", 3);
    uniform_set_int(fx->ssr_program->uniforms, "hizTex", 4);
    glUseProgram(fx->ssr_hiz_program->id);
    uniform_set_int(fx->ssr_hiz_program->uniforms, "srcTex", 0);
    glUseProgram(fx->fog_program->id);
    uniform_set_int(fx->fog_program->uniforms, "linDepthTex", 0);
    uniform_set_int(fx->fog_program->uniforms, "shadowMaps", 1);

    glUseProgram(fx->dof_coc_program->id);
    uniform_set_int(fx->dof_coc_program->uniforms, "sceneTex", 0);
    uniform_set_int(fx->dof_coc_program->uniforms, "depthTex", 1);
    glUseProgram(fx->dof_blur_program->id);
    uniform_set_int(fx->dof_blur_program->uniforms, "cocColorTex", 0);
    glUseProgram(fx->dof_composite_program->id);
    uniform_set_int(fx->dof_composite_program->uniforms, "sceneTex", 0);
    uniform_set_int(fx->dof_composite_program->uniforms, "blurTex", 1);
    uniform_set_int(fx->dof_composite_program->uniforms, "depthTex", 2);

    glUseProgram(fx->gtao_program->id);
    uniform_set_int(fx->gtao_program->uniforms, "linDepthTex", 0);
    uniform_set_int(fx->gtao_program->uniforms, "noiseTex", 1);
    uniform_set_int(fx->gtao_program->uniforms, "normalsTex", 2);
    uniform_set_int(fx->gtao_program->uniforms, "hdrTex", 3); // SSGI radiance source
    const float noise_scale[2] = {(float)fx->ssao_width / 4.0f, (float)fx->ssao_height / 4.0f};
    uniform_set_vec2(fx->gtao_program->uniforms, "noiseScale", noise_scale);

    glUseProgram(fx->ssao_blur_program->id);
    uniform_set_int(fx->ssao_blur_program->uniforms, "aoTex", 0);
    const float ao_texel[2] = {1.0f / (float)fx->ssao_width, 1.0f / (float)fx->ssao_height};
    uniform_set_vec2(fx->ssao_blur_program->uniforms, "texelSize", ao_texel);

    // Half-res effect resolution never changes after create, so the shared
    // tent composite and accumulator take their texel size once here
    glUseProgram(fx->upsample_tent_program->id);
    uniform_set_int(fx->upsample_tent_program->uniforms, "srcTex", 0);
    uniform_set_vec2(fx->upsample_tent_program->uniforms, "texelSize", ao_texel);

    glUseProgram(fx->temporal_accum_program->id);
    uniform_set_int(fx->temporal_accum_program->uniforms, "currentTex", 0);
    uniform_set_int(fx->temporal_accum_program->uniforms, "velocityTex", 1);
    uniform_set_int(fx->temporal_accum_program->uniforms, "historyTex", 2);
    uniform_set_vec2(fx->temporal_accum_program->uniforms, "texelSize", ao_texel);

    glUseProgram(fx->ssgi_composite_program->id);
    uniform_set_int(fx->ssgi_composite_program->uniforms, "giTex", 0);
    uniform_set_int(fx->ssgi_composite_program->uniforms, "albedoTex", 1);

    glUseProgram(fx->ssgi_accum_program->id);
    uniform_set_int(fx->ssgi_accum_program->uniforms, "currentTex", 0);
    uniform_set_int(fx->ssgi_accum_program->uniforms, "velocityTex", 1);
    uniform_set_int(fx->ssgi_accum_program->uniforms, "historyTex", 2);
    uniform_set_vec2(fx->ssgi_accum_program->uniforms, "texelSize", ao_texel);

    glUseProgram(fx->ssgi_atrous_program->id);
    uniform_set_int(fx->ssgi_atrous_program->uniforms, "giTex", 0);
    uniform_set_int(fx->ssgi_atrous_program->uniforms, "linDepthTex", 1);
    uniform_set_int(fx->ssgi_atrous_program->uniforms, "normalsTex", 2);
    uniform_set_vec2(fx->ssgi_atrous_program->uniforms, "texelSize", ao_texel);
    glUseProgram(0);

    create_fullscreen_quad_vao(&fx->quad_vao, &fx->quad_vbo);

    check_gl_error("create_postfx");
    return fx;
}

// Allocate the depth-of-field targets on first use so the feature is free when
// off: two half-res buffers (CoC+colour, gathered blur) and a full-res
// composite. Returns false and leaves DoF disabled if allocation fails.
static bool postfx_ensure_dof_targets(PostFX* fx) {
    if (fx->dof_ready)
        return true;
    if (!create_color_fbo(fx->bloom_width, fx->bloom_height, GL_RGBA16F, &fx->dof_coc_fbo,
                          &fx->dof_coc_texture) ||
        !create_color_fbo(fx->bloom_width, fx->bloom_height, GL_RGBA16F, &fx->dof_blur_fbo,
                          &fx->dof_blur_texture) ||
        !create_color_fbo(fx->width, fx->height, GL_RGBA16F, &fx->dof_fbo, &fx->dof_texture)) {
        log_error("Failed to allocate depth-of-field targets");
        return false;
    }
    fx->dof_ready = true;
    return true;
}

// Allocate the SSGI targets on first enable so the feature is free while off
// (mirrors the DoF pattern): the half-res GI radiance MRT plus the temporal
// accumulation and a-trous ping-pongs, ~5 half-res RGBA16F surfaces. The GI
// texture attaches to the GTAO FBO as attachment 1; until then the pass draws
// only attachment 0, which keeps the FBO complete.
static bool postfx_ensure_ssgi_targets(PostFX* fx) {
    if (fx->ssgi_ready)
        return true;
    glGenTextures(1, &fx->ssgi_gi_texture);
    glBindTexture(GL_TEXTURE_2D, fx->ssgi_gi_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, fx->ssao_width, fx->ssao_height, 0, GL_RGBA,
                 GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, fx->ssao_fbo[0]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                           fx->ssgi_gi_texture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!create_pingpong(fx->ssao_width, fx->ssao_height, GL_RGBA16F, &fx->ssgi_history) ||
        !create_pingpong(fx->ssao_width, fx->ssao_height, GL_RGBA16F, &fx->ssgi_atrous)) {
        log_error("Failed to allocate SSGI targets");
        return false;
    }
    fx->ssgi_ready = true;
    return true;
}

// Allocate the volumetric fog targets on first enable so the feature is free
// while off (DoF pattern): the half-res march buffer plus the temporal
// accumulation ping-pong.
static bool postfx_ensure_fog_targets(PostFX* fx) {
    if (fx->fog_ready)
        return true;
    if (!create_color_fbo(fx->ssao_width, fx->ssao_height, GL_RGBA16F, &fx->fog_fbo,
                          &fx->fog_texture) ||
        !create_pingpong(fx->ssao_width, fx->ssao_height, GL_RGBA16F, &fx->fog_history)) {
        log_error("Failed to allocate volumetric fog targets");
        return false;
    }
    fx->fog_ready = true;
    return true;
}

// Depth of field: signed CoC + gather at half res, composite at full res into
// fx->dof_texture. Callers must have ensured the targets exist and read
// fx->dof_texture as the scene afterward.
static void postfx_run_dof(PostFX* fx, mat4 projection) {
    const float dof_texel[2] = {1.0f / (float)fx->bloom_width, 1.0f / (float)fx->bloom_height};

    // Pass 1: signed CoC + half-res scene colour
    glBindFramebuffer(GL_FRAMEBUFFER, fx->dof_coc_fbo);
    glViewport(0, 0, fx->bloom_width, fx->bloom_height);
    glUseProgram(fx->dof_coc_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx->hdr_texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, fx->depth_texture);
    uniform_set_mat4(fx->dof_coc_program->uniforms, "projection", (float*)projection);
    uniform_set_float(fx->dof_coc_program->uniforms, "focusDistance", fx->dof_focus_distance);
    uniform_set_float(fx->dof_coc_program->uniforms, "focusRange", fx->dof_focus_range);
    uniform_set_float(fx->dof_coc_program->uniforms, "maxCoC", fx->dof_max_coc);
    draw_fullscreen_quad(fx->quad_vao);

    // Pass 2: gather blur (half res)
    glBindFramebuffer(GL_FRAMEBUFFER, fx->dof_blur_fbo);
    glUseProgram(fx->dof_blur_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx->dof_coc_texture);
    uniform_set_vec2(fx->dof_blur_program->uniforms, "texelSize", dof_texel);
    draw_fullscreen_quad(fx->quad_vao);

    // Pass 3: composite sharp + blur (full res)
    glBindFramebuffer(GL_FRAMEBUFFER, fx->dof_fbo);
    glViewport(0, 0, fx->width, fx->height);
    glUseProgram(fx->dof_composite_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx->hdr_texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, fx->dof_blur_texture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, fx->depth_texture);
    uniform_set_mat4(fx->dof_composite_program->uniforms, "projection", (float*)projection);
    uniform_set_float(fx->dof_composite_program->uniforms, "focusDistance", fx->dof_focus_distance);
    uniform_set_float(fx->dof_composite_program->uniforms, "focusRange", fx->dof_focus_range);
    uniform_set_float(fx->dof_composite_program->uniforms, "maxCoC", fx->dof_max_coc);
    draw_fullscreen_quad(fx->quad_vao);

    check_gl_error("postfx dof");
}

void free_postfx(PostFX* fx) {
    if (!fx)
        return;

    glDeleteFramebuffers(1, &fx->hdr_fbo);
    glDeleteTextures(1, &fx->hdr_texture);
    glDeleteFramebuffers(1, &fx->bloom_fbo);
    glDeleteTextures(1, &fx->bloom_texture);
    glDeleteFramebuffers(1, &fx->depth_fbo);
    glDeleteTextures(1, &fx->depth_texture);
    glDeleteFramebuffers(1, &fx->normal_fbo);
    glDeleteTextures(1, &fx->normal_texture);
    glDeleteFramebuffers(2, fx->ssao_fbo);
    glDeleteTextures(2, fx->ssao_texture);
    glDeleteTextures(1, &fx->ssgi_gi_texture);
    free_pingpong(&fx->ssgi_history);
    free_pingpong(&fx->ssgi_atrous);
    free_pingpong(&fx->ao_history);
    glDeleteTextures(1, &fx->noise_texture);
    glDeleteFramebuffers(1, &fx->ssr_fbo);
    glDeleteTextures(1, &fx->ssr_texture);
    glDeleteFramebuffers(1, &fx->hiz_fbo);
    glDeleteTextures(1, &fx->hiz_texture);
    glDeleteFramebuffers(1, &fx->aux_fbo);
    glDeleteTextures(1, &fx->aux_texture);
    glDeleteFramebuffers(1, &fx->albedo_fbo);
    glDeleteTextures(1, &fx->albedo_texture);
    free_pingpong(&fx->taa_history);
    glDeleteFramebuffers(1, &fx->lum_fbo);
    glDeleteTextures(1, &fx->lum_texture);
    free_pingpong(&fx->lum_adapt);
    // DoF/fog targets are 0 (no-op delete) if never lazily allocated
    glDeleteFramebuffers(1, &fx->dof_coc_fbo);
    glDeleteTextures(1, &fx->dof_coc_texture);
    glDeleteFramebuffers(1, &fx->dof_blur_fbo);
    glDeleteTextures(1, &fx->dof_blur_texture);
    glDeleteFramebuffers(1, &fx->dof_fbo);
    glDeleteTextures(1, &fx->dof_texture);
    glDeleteFramebuffers(1, &fx->fog_fbo);
    glDeleteTextures(1, &fx->fog_texture);
    free_pingpong(&fx->fog_history);

    free_program(fx->bright_program);
    free_program(fx->bloom_down_program);
    free_program(fx->bloom_up_program);
    free_program(fx->tonemap_program);
    free_program(fx->gtao_program);
    free_program(fx->ssao_blur_program);
    free_program(fx->temporal_accum_program);
    free_program(fx->ssgi_composite_program);
    free_program(fx->ssgi_accum_program);
    free_program(fx->ssgi_atrous_program);
    free_program(fx->lum_measure_program);
    free_program(fx->lum_adapt_program);
    free_program(fx->ssr_program);
    free_program(fx->ssr_hiz_program);
    free_program(fx->upsample_tent_program);
    free_program(fx->fog_program);
    free_program(fx->taa_resolve_program);
    free_program(fx->dof_coc_program);
    free_program(fx->dof_blur_program);
    free_program(fx->dof_composite_program);

    glDeleteVertexArrays(1, &fx->quad_vao);
    glDeleteBuffers(1, &fx->quad_vbo);

    free(fx);
}

void postfx_apply_film_look(PostFX* fx) {
    if (!fx)
        return;
    fx->vignette_enabled = true;
    fx->vignette_strength = 0.5f;
    fx->vignette_radius = 0.55f;
    fx->grain_enabled = true;
    fx->grain_strength = 0.09f;
    fx->sharpen_enabled = true;
    // Kept gentle on purpose: the unsharp mask amplifies this model's fine
    // scratched-metal specular into bright edge halos, so pushing it harder
    // reads as speckle. Punch comes from the vignette + grain instead.
    fx->sharpen_strength = 0.25f;
    fx->grade_enabled = true;
    glm_vec3_copy((vec3){0.0f, 0.0f, 0.01f}, fx->grade_lift); // whisper-cool shadows
    glm_vec3_one(fx->grade_gamma);
    glm_vec3_copy((vec3){1.05f, 1.0f, 0.95f}, fx->grade_gain); // warm highlights
}

bool postfx_wants_normals(const PostFX* fx) {
    if (!fx || !fx->normals_enabled)
        return false;
    // SSAO orients its sample hemisphere with these normals, SSR reflects off
    // them, and SSGI's sweep + edge-aware denoise consume them (without the
    // buffer both degrade to depth-derived normals); the debug view forces the
    // buffer on so it can be inspected with every consumer disabled
    return fx->ssao_enabled || fx->ssr_enabled || fx->ssgi_enabled ||
           fx->debug_view == POSTFX_DEBUG_NORMALS;
}

bool postfx_taa_active(const PostFX* fx) {
    // The single "TAA runs this frame" producer predicate: the engine gates the
    // jitter and the velocity buffer on it, and the resolve pass runs on it, so
    // they cannot disagree. Mirrors postfx_wants_normals.
    return fx && fx->taa_enabled;
}

bool postfx_wants_aux_gbuffer(const PostFX* fx) {
    // Attachment 2 carries motion vectors (.xy, for TAA reprojection) and linear
    // view-Z (.z, for GTAO position reconstruction). Produce it whenever either
    // consumer is active -- decoupled from TAA so GTAO gets linear depth even
    // with TAA off (e.g. headless). SSGI rides the GTAO sweep, so it needs the
    // linear depth too even if AO display is off (--ssgi --no-ssao). The fog
    // march reconstructs its ray endpoints from the linear Z as well.
    return fx && (fx->taa_enabled || fx->ssao_enabled || fx->ssgi_enabled || fx->fog_enabled);
}

bool postfx_wants_albedo(const PostFX* fx) {
    // Attachment 3 (base color) is only needed by SSGI's indirect-diffuse composite.
    return fx && fx->ssgi_enabled;
}

bool postfx_ssr_active(const PostFX* fx, bool normals_written) {
    // The single "SSR runs this frame" predicate: the effect is enabled and
    // the normals buffer it marches against was actually produced. Both the
    // postfx pass and the shadow catcher's floor marker derive from this so
    // they cannot disagree about whether the floor is being reflected.
    return fx && fx->ssr_enabled && normals_written;
}

// Resolve one color attachment of the MSAA framebuffer into a single-sample FBO,
// then restore the read buffer to attachment 0 (the sticky selection would
// otherwise break the next frame's color resolve).
static void resolve_color_attachment(GLuint msaa_fbo, GLenum attachment, GLuint dst_fbo, int w,
                                     int h) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msaa_fbo);
    glReadBuffer(attachment);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
}

// Reproject-and-blend temporal accumulation, shared by the AO and GI
// accumulators (their shaders share the currentTex/velocityTex/historyTex
// unit layout). Indexes the pair by frame parity, resets when the history is
// not valid (first use, or the accumulator was skipped last frame), and
// returns the freshly written texture.
static GLuint run_temporal_accum(PostFX* fx, ShaderProgram* prog, PingPong* pp, int w, int h,
                                 GLuint current_tex) {
    int write = fx->frame_index & 1;
    int read = write ^ 1;
    glBindFramebuffer(GL_FRAMEBUFFER, pp->fbo[write]);
    glViewport(0, 0, w, h);
    glUseProgram(prog->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, current_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, fx->aux_texture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, pp->tex[read]);
    uniform_set_int(prog->uniforms, "reset", pp->valid ? 0 : 1);
    draw_fullscreen_quad(fx->quad_vao);
    pp->valid = true;
    return pp->tex[write];
}

// Volumetric fog: march the half-res buffer, then fold it into the HDR
// scene before DoF/bloom/tonemap so shafts defocus, bloom, and meter like
// direct light. Owns the fog history lifecycle -- the history is only
// fresh when the accumulator ran, so every other path invalidates it.
// Returns the half-res fog texture for the debug view, 0 when fog is off.
static GLuint postfx_run_fog(PostFX* fx, bool aux_written, bool taa_resolving, mat4 projection,
                             mat4 view) {
    if (!fx->fog_enabled || !aux_written || !postfx_ensure_fog_targets(fx)) {
        fx->fog_history.valid = false;
        return 0;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fx->fog_fbo);
    glViewport(0, 0, fx->ssao_width, fx->ssao_height);
    glUseProgram(fx->fog_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx->aux_texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, fx->fog_shadow_map_array);
    glActiveTexture(GL_TEXTURE0);
    mat4 inv_view;
    glm_mat4_inv(view, inv_view);
    UniformManager* fu = fx->fog_program->uniforms;
    uniform_set_mat4(fu, "invView", (float*)inv_view);
    uniform_set_mat4(fu, "projection", (float*)projection);
    uniform_set_vec3(fu, "ambientColor", fx->fog_ambient);
    uniform_set_float(fu, "density", fx->fog_density);
    uniform_set_float(fu, "heightFalloff", fx->fog_height_falloff);
    uniform_set_float(fu, "floorY", fx->fog_floor_y);
    uniform_set_float(fu, "fogFar", fx->fog_far);
    uniform_set_float(fu, "anisotropy", fx->fog_anisotropy);
    uniform_set_float(fu, "sunBoost", fx->fog_sun_boost);
    uniform_set_float(fu, "shadowBias", fx->fog_shadow_bias);
    uniform_set_int(fu, "steps", fx->fog_steps);
    // Count 0 (shadows off or absent) degrades the march to plain ambient
    // haze; the publish guarantees the map array is valid whenever the
    // count is nonzero.
    uniform_set_int(fu, "numLights", fx->fog_light_count);
    for (int i = 0; i < fx->fog_light_count; i++) {
        char name[48];
        snprintf(name, sizeof(name), "lightSpaceMatrix[%d]", i);
        uniform_set_mat4(fu, name, (const float*)fx->fog_light_space[i]);
        snprintf(name, sizeof(name), "lightColor[%d]", i);
        uniform_set_vec3(fu, name, fx->fog_light_color[i]);
        snprintf(name, sizeof(name), "lightDir[%d]", i);
        uniform_set_vec3(fu, name, fx->fog_light_dir[i]);
    }
    // Under TAA the march's dither rotates per frame and the accumulator
    // integrates it; headless/no-TAA keeps the static dither so equal runs
    // stay byte-identical.
    uniform_set_int(fu, "temporal", taa_resolving ? 1 : 0);
    uniform_set_int(fu, "frameIndex", fx->frame_index);
    draw_fullscreen_quad(fx->quad_vao);

    GLuint result = fx->fog_texture;
    if (taa_resolving) {
        result = run_temporal_accum(fx, fx->temporal_accum_program, &fx->fog_history,
                                    fx->ssao_width, fx->ssao_height, fx->fog_texture);
    } else {
        fx->fog_history.valid = false;
    }

    // Fold into the scene: out = inscatter + scene * transmittance.
    // Same enable/draw/restore idiom as the SSR composite.
    glBindFramebuffer(GL_FRAMEBUFFER, fx->hdr_fbo);
    glViewport(0, 0, fx->width, fx->height);
    glUseProgram(fx->upsample_tent_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, result);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_SRC_ALPHA);
    draw_fullscreen_quad(fx->quad_vao);
    glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    check_gl_error("postfx fog");
    return result;
}

void postfx_run(PostFX* fx, GLuint msaa_fbo, GLuint target_fbo, bool frame_is_hdr,
                bool normals_written, bool aux_written, bool albedo_written, mat4 projection,
                mat4 view) {
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
        // Display-ready frame: copy to the target, skipping bloom and tone
        // mapping. Linear filtering box-downsamples the supersampled buffer to
        // the display size (a 1:1 identity blit when supersampling is off).
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fx->hdr_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fbo);
        glBlitFramebuffer(0, 0, fx->width, fx->height, 0, 0, fx->out_width, fx->out_height,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    } else {
        // The color TAA resolves iff it is enabled and its velocity buffer was
        // produced. The single invariant behind both the TAA pass and the AO
        // temporal accumulation: AO accumulates exactly when color TAA does.
        bool taa_resolving = postfx_taa_active(fx) && aux_written;

        // Resolve the aux G-buffer (attachment 2: motion .xy + linear view-Z .z)
        // once, ahead of both consumers -- TAA reprojection reads the motion,
        // GTAO reconstructs positions from the linear Z. Hoisted out of the TAA
        // block so GTAO gets linear depth even when TAA is off.
        if (aux_written) {
            resolve_color_attachment(msaa_fbo, GL_COLOR_ATTACHMENT2, fx->aux_fbo, fx->width,
                                     fx->height);
        }
        // Resolve the albedo G-buffer (attachment 3) for SSGI's indirect-diffuse
        // composite, iff the scene pass actually wrote it (threaded from the
        // engine's frame-start decision, like normals/aux -- not re-derived here).
        if (albedo_written) {
            resolve_color_attachment(msaa_fbo, GL_COLOR_ATTACHMENT3, fx->albedo_fbo, fx->width,
                                     fx->height);
        }

        // Temporal AA resolve, before every other HDR pass: reproject the
        // accumulated history by the velocity buffer, neighborhood-clamp it
        // against the current frame, blend, and write the result back into
        // hdr_fbo so SSR/DoF/bloom/tonemap consume the anti-aliased color.
        if (taa_resolving) {
            // Bind the program before setting its uniforms (glUniform* acts on
            // the active program); run_temporal_accum re-binds it harmlessly.
            glUseProgram(fx->taa_resolve_program->id);
            UniformManager* tu = fx->taa_resolve_program->uniforms;
            uniform_set_int(tu, "currentTex", 0);
            uniform_set_int(tu, "velocityTex", 1);
            uniform_set_int(tu, "historyTex", 2);
            const float taa_texel[2] = {1.0f / (float)fx->width, 1.0f / (float)fx->height};
            uniform_set_vec2(tu, "texelSize", taa_texel);
            run_temporal_accum(fx, fx->taa_resolve_program, &fx->taa_history, fx->width,
                               fx->height, fx->hdr_texture);

            // Push the resolved frame back into hdr_fbo (the history side is
            // kept as next frame's accumulation buffer).
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fx->taa_history.fbo[fx->frame_index & 1]);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fx->hdr_fbo);
            glBlitFramebuffer(0, 0, fx->width, fx->height, 0, 0, fx->width, fx->height,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            check_gl_error("postfx taa");
        } else {
            fx->taa_history.valid = false;
        }

        // Resolve the scene pass's second attachment (normals + roughness)
        // ahead of its consumers (SSAO now, SSR later). The caller reports
        // whether the attachment was written this frame; re-deriving it from
        // fx flags here could disagree with what the scene pass produced.
        bool have_normals = normals_written;
        if (have_normals) {
            resolve_color_attachment(msaa_fbo, GL_COLOR_ATTACHMENT1, fx->normal_fbo, fx->width,
                                     fx->height);
            check_gl_error("postfx normals resolve");
        }

        // Depth (and its inverse) serve SSR and DoF's circle-of-confusion. GTAO
        // no longer needs it -- it reconstructs from the aux buffer's linear Z.
        bool ssr_active = postfx_ssr_active(fx, have_normals);
        bool dof_active = fx->dof_enabled;
        mat4 inv_projection;
        if (ssr_active || dof_active) {
            // Resolve depth alongside color so screen-space passes can
            // reconstruct view-space positions (formats match: both are
            // DEPTH24_STENCIL8)
            glBindFramebuffer(GL_READ_FRAMEBUFFER, msaa_fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fx->depth_fbo);
            glBlitFramebuffer(0, 0, fx->width, fx->height, 0, 0, fx->width, fx->height,
                              GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            check_gl_error("postfx depth resolve");

            glm_mat4_inv(projection, inv_projection);
        }

        // Tonemap reads AO from here; the accumulate pass repoints it below.
        // When temporal accumulation runs (taa_resolving) GTAO jitters its slices
        // per frame and the accum pass integrates them; otherwise it stays frame-static.
        GLuint ao_result_tex = fx->ssao_texture[1];
        // The GTAO sweep runs when AO is shown OR SSGI needs it (they share the
        // pass). AO-only denoise (blur + temporal accum) stays gated on ssao_enabled.
        // SSGI implies the GTAO sweep (they share it); its targets are lazily
        // allocated on first enable.
        const bool ssgi_active = fx->ssgi_enabled && postfx_ensure_ssgi_targets(fx);
        const bool gtao_active = fx->ssao_enabled || ssgi_active;
        bool ao_accum_ran = false;
        bool gi_accum_ran = false;
        if (gtao_active) {
            // Raw occlusion at half res. GTAO reads linear view-Z from the aux
            // buffer's .z (unit 0) and reconstructs positions from it -- the
            // non-linear depth buffer staircased flat surfaces into AO banding.
            glBindFramebuffer(GL_FRAMEBUFFER, fx->ssao_fbo[0]);
            glViewport(0, 0, fx->ssao_width, fx->ssao_height);
            // SSGI rides the same sweep: when on, also draw the GI radiance into
            // attachment 1 (ssgi_gi_texture); when off, only AO is written so the
            // path is byte-identical to plain GTAO.
            if (ssgi_active) {
                const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
                glDrawBuffers(2, bufs);
            } else {
                glDrawBuffer(GL_COLOR_ATTACHMENT0);
            }
            glUseProgram(fx->gtao_program->id);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, fx->aux_texture);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, fx->noise_texture);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, have_normals ? fx->normal_texture : 0);
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, ssgi_active ? fx->hdr_texture : 0);
            uniform_set_int(fx->gtao_program->uniforms, "useNormalsTex", have_normals ? 1 : 0);
            uniform_set_mat4(fx->gtao_program->uniforms, "projection", (float*)projection);
            uniform_set_float(fx->gtao_program->uniforms, "radius", fx->ssao_radius);
            uniform_set_int(fx->gtao_program->uniforms, "temporal", taa_resolving ? 1 : 0);
            uniform_set_int(fx->gtao_program->uniforms, "frameIndex", fx->frame_index);
            uniform_set_int(fx->gtao_program->uniforms, "gatherGI", ssgi_active ? 1 : 0);
            draw_fullscreen_quad(fx->quad_vao);

            if (fx->ssao_enabled) {
                // 4x4 box blur cancels the rotation-noise tile
                glBindFramebuffer(GL_FRAMEBUFFER, fx->ssao_fbo[1]);
                glUseProgram(fx->ssao_blur_program->id);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, fx->ssao_texture[0]);
                draw_fullscreen_quad(fx->quad_vao);

                // Temporal accumulation: reproject the history by velocity and
                // blend so the per-frame jittered occlusion integrates into a
                // stable result.
                if (taa_resolving) {
                    ao_result_tex =
                        run_temporal_accum(fx, fx->temporal_accum_program, &fx->ao_history,
                                           fx->ssao_width, fx->ssao_height, fx->ssao_texture[1]);
                    ao_accum_ran = true;
                }
            }
        }
        if (!ao_accum_ran)
            fx->ao_history.valid = false;

        // SSGI denoise chain: temporal accumulation of the raw gather (TAA
        // frames only -- it needs velocity and the per-frame slice jitter),
        // then an edge-aware a-trous blur. SVGF-style order: accumulate the
        // raw signal, smooth the accumulation.
        GLuint gi_result_tex = fx->ssgi_gi_texture;
        if (ssgi_active) {
            if (taa_resolving) {
                gi_result_tex =
                    run_temporal_accum(fx, fx->ssgi_accum_program, &fx->ssgi_history,
                                       fx->ssao_width, fx->ssao_height, fx->ssgi_gi_texture);
                gi_accum_ran = true;
            }

            // Three a-trous iterations with doubling tap spacing (1, 2, 4).
            // Units 1/2 (depth, normals) are invariant across iterations;
            // only the GI ping-pong on unit 0 changes.
            glUseProgram(fx->ssgi_atrous_program->id);
            uniform_set_int(fx->ssgi_atrous_program->uniforms, "useNormalsTex",
                            have_normals ? 1 : 0);
            glViewport(0, 0, fx->ssao_width, fx->ssao_height);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, fx->aux_texture);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, have_normals ? fx->normal_texture : 0);
            for (int i = 0; i < 3; i++) {
                glBindFramebuffer(GL_FRAMEBUFFER, fx->ssgi_atrous.fbo[i & 1]);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, gi_result_tex);
                uniform_set_int(fx->ssgi_atrous_program->uniforms, "stepSize", 1 << i);
                draw_fullscreen_quad(fx->quad_vao);
                gi_result_tex = fx->ssgi_atrous.tex[i & 1];
            }
            check_gl_error("postfx ssgi denoise");
        }
        if (!gi_accum_ran)
            fx->ssgi_history.valid = false;

        if (ssr_active) {
            // Rebuild the min-depth pyramid the traversal walks: level 0
            // takes the conservative min over the full-res depth, every
            // further level the min of the 2x2 below. The source level is
            // pinned via BASE/MAX_LEVEL so a level never reads itself.
            glBindFramebuffer(GL_FRAMEBUFFER, fx->hiz_fbo);
            glUseProgram(fx->ssr_hiz_program->id);
            glActiveTexture(GL_TEXTURE0);
            {
                int mip_w = fx->ssao_width;
                int mip_h = fx->ssao_height;
                int src_w = fx->width;
                int src_h = fx->height;
                for (int mip = 0; mip < fx->hiz_mips; mip++) {
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                           fx->hiz_texture, mip);
                    glViewport(0, 0, mip_w, mip_h);
                    if (mip == 0) {
                        glBindTexture(GL_TEXTURE_2D, fx->depth_texture);
                    } else {
                        glBindTexture(GL_TEXTURE_2D, fx->hiz_texture);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, mip - 1);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mip - 1);
                    }
                    uniform_set_int(fx->ssr_hiz_program->uniforms, "srcWidth", src_w);
                    uniform_set_int(fx->ssr_hiz_program->uniforms, "srcHeight", src_h);
                    draw_fullscreen_quad(fx->quad_vao);
                    src_w = mip_w;
                    src_h = mip_h;
                    mip_w = mip_w > 1 ? mip_w / 2 : 1;
                    mip_h = mip_h > 1 ? mip_h / 2 : 1;
                }
                // Reopen the whole chain for the traversal's mip fetches
                glBindTexture(GL_TEXTURE_2D, fx->hiz_texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, fx->hiz_mips - 1);
            }
            check_gl_error("postfx hiz build");

            // March reflections into the half-res buffer (reads the scene,
            // writes elsewhere: GL 4.1 has no texture barrier)
            glBindFramebuffer(GL_FRAMEBUFFER, fx->ssr_fbo);
            glViewport(0, 0, fx->ssao_width, fx->ssao_height);
            glUseProgram(fx->ssr_program->id);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, fx->depth_texture);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, fx->normal_texture);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, fx->hdr_texture);
            glActiveTexture(GL_TEXTURE4);
            glBindTexture(GL_TEXTURE_2D, fx->hiz_texture);
            glActiveTexture(GL_TEXTURE0);
            uniform_set_int(fx->ssr_program->uniforms, "hizWidth", fx->ssao_width);
            uniform_set_int(fx->ssr_program->uniforms, "hizHeight", fx->ssao_height);
            uniform_set_int(fx->ssr_program->uniforms, "hizMips", fx->hiz_mips);
            uniform_set_mat4(fx->ssr_program->uniforms, "projection", (float*)projection);
            uniform_set_mat4(fx->ssr_program->uniforms, "invProjection", (float*)inv_projection);
            uniform_set_float(fx->ssr_program->uniforms, "maxDistance", fx->ssr_max_distance);
            uniform_set_float(fx->ssr_program->uniforms, "thickness", fx->ssr_thickness);
            uniform_set_int(fx->ssr_program->uniforms, "steps", fx->ssr_steps);
            uniform_set_float(fx->ssr_program->uniforms, "floorRoughness", fx->ssr_floor_roughness);
            uniform_set_float(fx->ssr_program->uniforms, "maxRoughness", fx->ssr_max_roughness);
            // Strength folds into the march's premultiplied weight (clamped
            // there) so the composite stays a straight premultiplied lerp
            uniform_set_float(fx->ssr_program->uniforms, "strength", fx->ssr_strength);
            // Local-probe fallback for rays the march cannot answer. The
            // probe lives in world space while SSR is view-space, so this is
            // the only postfx consumer of the view matrix.
            uniform_set_int(fx->ssr_program->uniforms, "probeEnabled", fx->probe_enabled ? 1 : 0);
            if (fx->probe_enabled) {
                mat4 inv_view;
                glm_mat4_inv(view, inv_view);
                glActiveTexture(GL_TEXTURE3);
                glBindTexture(GL_TEXTURE_CUBE_MAP, fx->probe_cubemap);
                glActiveTexture(GL_TEXTURE0);
                uniform_set_mat4(fx->ssr_program->uniforms, "invView", (float*)inv_view);
                uniform_set_vec3(fx->ssr_program->uniforms, "probePos", fx->probe_pos);
                uniform_set_vec3(fx->ssr_program->uniforms, "probeBoxMin", fx->probe_box_min);
                uniform_set_vec3(fx->ssr_program->uniforms, "probeBoxMax", fx->probe_box_max);
                uniform_set_float(fx->ssr_program->uniforms, "probeMaxLOD", fx->probe_max_lod);
                uniform_set_float(fx->ssr_program->uniforms, "probeIntensity",
                                  fx->probe_intensity);
            }
            draw_fullscreen_quad(fx->quad_vao);

            // Lerp the reflections onto the HDR scene before bloom so
            // reflected highlights bloom like direct ones. The buffer is
            // premultiplied, hence (ONE, ONE_MINUS_SRC_ALPHA). Restore the
            // engine's blend function afterward: it is set once at init
            // and everything else assumes it.
            glBindFramebuffer(GL_FRAMEBUFFER, fx->hdr_fbo);
            glViewport(0, 0, fx->width, fx->height);
            glUseProgram(fx->upsample_tent_program->id);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, fx->ssr_texture);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            draw_fullscreen_quad(fx->quad_vao);
            glDisable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            check_gl_error("postfx ssr");
        }

        if (ssgi_active && albedo_written) {
            // Add one bounce of indirect diffuse (albedo x gathered GI x intensity)
            // into the HDR scene before bloom, so bounce light blooms like direct
            // light. Half-res GI is bilinear-upsampled; albedo is full-res.
            glBindFramebuffer(GL_FRAMEBUFFER, fx->hdr_fbo);
            glViewport(0, 0, fx->width, fx->height);
            glUseProgram(fx->ssgi_composite_program->id);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, gi_result_tex); // accumulated + denoised
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, fx->albedo_texture);
            uniform_set_float(fx->ssgi_composite_program->uniforms, "intensity", fx->ssgi_intensity);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            draw_fullscreen_quad(fx->quad_vao);
            glDisable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            check_gl_error("postfx ssgi composite");
        }

        GLuint fog_result_tex = postfx_run_fog(fx, aux_written, taa_resolving, projection, view);

        // Depth of field replaces the scene that bloom and tone mapping read.
        // scene_tex is the sharp HDR unless DoF ran into fx->dof_texture.
        GLuint scene_tex = fx->hdr_texture;
        if (dof_active && postfx_ensure_dof_targets(fx)) {
            postfx_run_dof(fx, projection);
            scene_tex = fx->dof_texture;
        }

        // Auto-exposure: measure the scene's log2 luminance at 64x64, mip down
        // to its geometric mean, and blend the 1x1 adapted value toward it
        // (frame-count-based eye adaptation; the tonemap divides the key by it).
        int lum_write = fx->frame_index & 1;
        if (fx->auto_exposure) {
            glBindFramebuffer(GL_FRAMEBUFFER, fx->lum_fbo);
            glViewport(0, 0, LUM_MEASURE_SIZE, LUM_MEASURE_SIZE);
            glUseProgram(fx->lum_measure_program->id);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, scene_tex);
            // Metering floor == key (the "auto only darkens" invariant); the
            // tonemap divides by the mean with the same uniform.
            uniform_set_float(fx->lum_measure_program->uniforms, "autoKey",
                              fx->auto_exposure_key);
            draw_fullscreen_quad(fx->quad_vao);
            glBindTexture(GL_TEXTURE_2D, fx->lum_texture);
            glGenerateMipmap(GL_TEXTURE_2D);

            glBindFramebuffer(GL_FRAMEBUFFER, fx->lum_adapt.fbo[lum_write]);
            glViewport(0, 0, 1, 1);
            glUseProgram(fx->lum_adapt_program->id);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, fx->lum_texture);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, fx->lum_adapt.tex[lum_write ^ 1]);
            // Snap (reset) when the history is invalid: this accumulator has no
            // neighborhood clamp, so unlike AO/GI/TAA it cannot self-heal from
            // stale or never-written history (a mid-run enable would otherwise
            // blend from undefined texels).
            uniform_set_int(fx->lum_adapt_program->uniforms, "reset",
                            fx->lum_adapt.valid ? 0 : 1);
            draw_fullscreen_quad(fx->quad_vao);
            fx->lum_adapt.valid = true;
            check_gl_error("postfx auto exposure");
        } else {
            fx->lum_adapt.valid = false;
        }

        if (fx->bloom_enabled) {
            // Bright pass into pyramid level 0 (linear sampling downsamples)
            glBindFramebuffer(GL_FRAMEBUFFER, fx->bloom_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   fx->bloom_texture, 0);
            glViewport(0, 0, fx->bloom_width, fx->bloom_height);
            glUseProgram(fx->bright_program->id);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, scene_tex);
            uniform_set_float(fx->bright_program->uniforms, "threshold", fx->bloom_threshold);
            uniform_set_float(fx->bright_program->uniforms, "knee", fx->bloom_knee);
            uniform_set_float(fx->bright_program->uniforms, "maxBrightness",
                              fx->bloom_max_brightness);
            draw_fullscreen_quad(fx->quad_vao);

            // Downsample the chain: each level reads a 13-tap filter of the
            // level above it. Sampling and writing the same texture is safe
            // because BASE/MAX_LEVEL pin the sampled set to the source level
            // (the hi-z build idiom).
            glUseProgram(fx->bloom_down_program->id);
            glBindTexture(GL_TEXTURE_2D, fx->bloom_texture);
            int lw = fx->bloom_width;
            int lh = fx->bloom_height;
            for (int mip = 1; mip < fx->bloom_mips; mip++) {
                int dw = lw > 1 ? lw / 2 : 1;
                int dh = lh > 1 ? lh / 2 : 1;
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                       fx->bloom_texture, mip);
                glViewport(0, 0, dw, dh);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, mip - 1);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mip - 1);
                uniform_set_vec2(fx->bloom_down_program->uniforms, "texelSize",
                                 (vec2){1.0f / (float)lw, 1.0f / (float)lh});
                draw_fullscreen_quad(fx->quad_vao);
                lw = dw;
                lh = dh;
            }

            // Upsample back: tent-filter each coarser level additively onto
            // the finer one, so level 0 ends as the sum of progressively
            // wider blurs -- one smooth wide kernel instead of ringed bands.
            glUseProgram(fx->bloom_up_program->id);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            for (int mip = fx->bloom_mips - 2; mip >= 0; mip--) {
                int sw = fx->bloom_width >> (mip + 1);
                int sh = fx->bloom_height >> (mip + 1);
                sw = sw > 1 ? sw : 1;
                sh = sh > 1 ? sh : 1;
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                       fx->bloom_texture, mip);
                glViewport(0, 0, fx->bloom_width >> mip, fx->bloom_height >> mip);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, mip + 1);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mip + 1);
                uniform_set_vec2(fx->bloom_up_program->uniforms, "texelSize",
                                 (vec2){1.0f / (float)sw, 1.0f / (float)sh});
                draw_fullscreen_quad(fx->quad_vao);
            }
            glDisable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            // Reopen the full chain; the tonemap magnifies level 0
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, fx->bloom_mips - 1);
            check_gl_error("postfx bloom pyramid");
        }

        // Composite + tone map into the target framebuffer. The quad runs at
        // the display size while sampling the supersampled HDR texture, so each
        // output pixel linearly averages its 2x2 source block (the SSAA
        // resolve). Tone mapping the averaged linear radiance is correct; a 1:1
        // pass-through when supersampling is off.
        glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
        glViewport(0, 0, fx->out_width, fx->out_height);
        glUseProgram(fx->tonemap_program->id);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, scene_tex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, fx->bloom_texture); // magnified -> level 0
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, ao_result_tex); // accumulated AO when TAA on, else blurred
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, have_normals ? fx->normal_texture : 0);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, ssr_active ? fx->ssr_texture : 0);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, albedo_written ? fx->albedo_texture : 0);
        glActiveTexture(GL_TEXTURE6);
        // Debug view shows the GI as composited (accumulated + denoised)
        glBindTexture(GL_TEXTURE_2D, ssgi_active ? gi_result_tex : 0);
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, fx->auto_exposure ? fx->lum_adapt.tex[lum_write] : 0);
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D, fog_result_tex); // 0 when fog did not run
        UniformManager* tm = fx->tonemap_program->uniforms;
        uniform_set_float(tm, "exposure", fx->exposure);
        uniform_set_int(tm, "autoExposure", fx->auto_exposure ? 1 : 0);
        uniform_set_float(tm, "autoKey", fx->auto_exposure_key);
        uniform_set_float(tm, "bloomStrength", fx->bloom_strength);
        uniform_set_int(tm, "bloomEnabled", fx->bloom_enabled ? 1 : 0);
        uniform_set_int(tm, "aoEnabled", fx->ssao_enabled ? 1 : 0);
        uniform_set_float(tm, "aoStrength", fx->ssao_strength);
        // Suppress debug views whose source buffer was not produced, and
        // say so once per requested view rather than silently every frame
        PostFXDebugView debug_view = fx->debug_view;
        if ((debug_view == POSTFX_DEBUG_AO && !fx->ssao_enabled) ||
            (debug_view == POSTFX_DEBUG_NORMALS && !have_normals) ||
            (debug_view == POSTFX_DEBUG_SSR && !ssr_active) ||
            (debug_view == POSTFX_DEBUG_ALBEDO && !albedo_written) ||
            (debug_view == POSTFX_DEBUG_SSGI && !ssgi_active) ||
            (debug_view == POSTFX_DEBUG_FOG && fog_result_tex == 0)) {
            static PostFXDebugView warned_view = POSTFX_DEBUG_NONE;
            if (warned_view != debug_view) {
                log_warn("debug view %d suppressed: its source buffer is disabled",
                         (int)debug_view);
                warned_view = debug_view;
            }
            debug_view = POSTFX_DEBUG_NONE;
        }
        uniform_set_int(tm, "debugView", (int)debug_view);
        uniform_set_int(tm, "tonemapMode", (int)mode);

        // Finishing grade (sharpen -> grade -> vignette -> gamma -> grain)
        const float texel[2] = {1.0f / (float)fx->out_width, 1.0f / (float)fx->out_height};
        uniform_set_vec2(tm, "texelSize", texel);
        uniform_set_int(tm, "sharpenEnabled", fx->sharpen_enabled ? 1 : 0);
        uniform_set_float(tm, "sharpenStrength", fx->sharpen_strength);
        uniform_set_int(tm, "gradeEnabled", fx->grade_enabled ? 1 : 0);
        uniform_set_vec3(tm, "gradeLift", fx->grade_lift);
        uniform_set_vec3(tm, "gradeGamma", fx->grade_gamma);
        uniform_set_vec3(tm, "gradeGain", fx->grade_gain);
        uniform_set_int(tm, "vignetteEnabled", fx->vignette_enabled ? 1 : 0);
        uniform_set_float(tm, "vignetteStrength", fx->vignette_strength);
        uniform_set_float(tm, "vignetteRadius", fx->vignette_radius);
        uniform_set_int(tm, "grainEnabled", fx->grain_enabled ? 1 : 0);
        uniform_set_float(tm, "grainStrength", fx->grain_strength);
        uniform_set_float(tm, "grainSeed", (float)fx->frame_index);
        draw_fullscreen_quad(fx->quad_vao);

        glUseProgram(0);
        glActiveTexture(GL_TEXTURE0);
    }

    if (depth_was_on)
        glEnable(GL_DEPTH_TEST);
    if (blend_was_on)
        glEnable(GL_BLEND);
}
