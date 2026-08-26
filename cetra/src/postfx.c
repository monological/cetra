#include <stdio.h>
#include <stdlib.h>

#include <string.h>

#include "postfx.h"
#include "lut.h"
#include "profiler.h"
#include "texture.h"
#include "uniform.h"
#include "util.h"

#include "ext/log.h"

// Auto-exposure measure target: 64x64 log2 luminances, binned by
// lum_histogram_frag and collapsed by lum_reduce_frag. It used to average down
// its own mip chain instead, which gave the geometric mean and nothing else --
// a mean cannot have a tail cut off it after the fact, which is what a
// percentile needs (spec 11.52).
#define LUM_MEASURE_SIZE 64

// One fragment per bin, so the histogram pass is this many fragments and the
// reduce pass loops this many texels. 64 is far more resolution than the
// percentile cut needs; the cost of a bin is one more iteration in a loop that
// runs once per frame.
#define LUM_HISTOGRAM_BINS 128

// Output rows the source is split across, so the bin pass is BINS x ROWS
// fragments instead of BINS. Fragment (bin, r) walks only its slice of the
// source rows and the reduce sums the column.
//
// This is a LATENCY fix, not a work one: the total fetch count is unchanged. 64
// fragments is ~0.4% occupancy, which measured ~0.35 ms standing alone against
// 1.19 us marginal when 10000 draws overlap. At 8 the serial depth is
// 4096/8 + 64*8 = 1024 against 4160, which is near the minimum for this shape.
#define LUM_HISTOGRAM_ROWS 8

// The binned range, as log2 luminance in absolute cd/m^2. Roughly starlight
// (1e-4) to well past a sunlit surface (1e5), which is the photometric span the
// meter reads since it started dividing pre-exposure back out.
//
// Deliberately wider than any scene needs, because the cost of width is
// resolution the percentile does not need, while the cost of being too narrow
// is a scene pinned against an end bin. Samples outside it still contribute
// their true value to the mean -- only their percentile position is clamped.
// The floor is the measure pass's numeric guard (log2(1e-8)), NOT a round
// number, and they must agree. lum_histogram_frag clamps an out-of-range
// sample's BIN index while still summing its true value, so a sample below the
// floor drags bin 0's mean below the bin it represents -- and the reduce pass
// interpolates a split bin using that mean. With the guard 12.5 stops under the
// floor, every black-background texel did exactly that, which is why a frame of
// mostly background metered the same whatever the percentiles were.
#define LUM_HISTOGRAM_MIN_LOG2 (-26.575425f)
// And the ceiling covers the largest value the measure pass can emit, which is
// NOT a constant: it clamps in working space at WS_SCENE_MAX and then divides by
// preExposure, so the largest absolute radiance it can report is
// log2(60000) + 20 ~= 35.9 once the gain sits on its 20-stop floor. A fixed 18
// left bin 127 spanning nine stops while claiming its share, and a percentile
// cut landing there interpolates against a mean that is nowhere near it.
#define LUM_HISTOGRAM_MAX_LOG2 (36.0f)
// Motion-blur tile size (px): tile-max reduces velocity to one dominant vector
// per MOTION_BLUR_TILE^2 tile, which also bounds the max blur radius. Must match
// TILE_SIZE reasoning in motion_blur_frag.glsl (MAX_PIXELS clamp).
#define MOTION_BLUR_TILE 20
// DoF tile size (half-res texels per tile texel): the tile pass reduces the
// signed CoC to per-tile far/near maxima that set the gather's kernel radius.
// Uploaded as the tileSize uniform to dof_tile and dof_gather.
#define DOF_TILE 4
// Aperture kernel taps; mirror of dof_gather_frag's TAPS.
#define DOF_TAPS 64

// Creates a single-sample color-only FBO; returns false on failure
static bool create_color_fbo(int width, int height, GLenum internal_format, GLuint* out_fbo,
                             GLuint* out_texture) {
    glGenTextures(1, out_texture);
    glBindTexture(GL_TEXTURE_2D, *out_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal_format, width, height, 0,
                 gl_transfer_format(internal_format), GL_FLOAT, NULL);
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

// Render every slice of the froxel volume: attach the layer, tell the shader
// which slice it is, draw the fullscreen quad. One draw per layer -- the
// codebase's established idiom (shadow.c cascades, material_texture_array.c layers,
// ibl.c cube faces) -- so no geometry shader and no new shader stage.
// The VAO is bound once around the loop rather than per draw; the shared
// draw_fullscreen_quad rebinds it every call, which is 63 redundant bind pairs
// here. Completeness is checked on the first layer: it cannot be checked at
// creation because the FBO carries no attachment until one is bound, and a
// driver that rejects a layered 3D attachment would otherwise fail silently
// across every slice.
static void draw_volume_slices(PostFX* fx, GLuint volume, UniformManager* um) {
    glBindFramebuffer(GL_FRAMEBUFFER, fx->froxel_fbo);
    glViewport(0, 0, fx->froxel_built_x, fx->froxel_built_y);
    glBindVertexArray(fx->quad_vao);
    for (int slice = 0; slice < fx->froxel_built_z; slice++) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, volume, 0, slice);
        if (slice == 0 && glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            // Sticky, and NOT fog_enabled: that flag is the app's and the GUI's, and two
            // of the three things that arm this pass republish every frame, so clearing
            // it would stop nothing and stomp a user setting on the way past.
            log_error("Froxel volume FBO incomplete; disabling the volume");
            fx->froxel_failed = true;
            break;
        }
        uniform_set_int(um, "sliceIndex", slice);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glBindVertexArray(0);
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
    for (int i = 0; i < 2; i++) {
        gl_delete_fbo(&pp->fbo[i]);
        gl_delete_texture(&pp->tex[i]);
    }
    pp->valid = false;
}

// (Re)create the SSR reflection buffer + the Hi-Z traversal pyramid at the
// current SSR resolution: full-res when fx->ssr_full_res (sharp reflections;
// the half-res march's hard hit/miss coverage edge was the striping source),
// else half-res. Depth/HDR sources the march samples are already full-res.
// Any existing reflection buffer and pyramid must be destroyed first: this
// overwrites the handles rather than reusing them.
static bool create_ssr_buffers(PostFX* fx) {
    int w = fx->ssr_full_res ? fx->width : fx->half_width;
    int h = fx->ssr_full_res ? fx->height : fx->half_height;
    // HDR reflection buffer; carries premultiplied scene color * weight.
    if (!create_color_fbo(w, h, GL_RGBA16F, &fx->ssr_fbo, &fx->ssr_texture))
        return false;
    // Min-depth pyramid for the SSR traversal: base at the SSR resolution with a
    // full mip chain, each level the min (nearest) of the 2x2 below. R32F: fp16
    // depth staircases at scene scale. Set the max level — an incomplete chain
    // samples as black.
    int cw = w, ch = h;
    fx->hiz_mips = 1;
    while ((cw > 1 || ch > 1) && fx->hiz_mips < 16) {
        cw = cw > 1 ? cw / 2 : 1;
        ch = ch > 1 ? ch / 2 : 1;
        fx->hiz_mips++;
    }
    glGenTextures(1, &fx->hiz_texture);
    glBindTexture(GL_TEXTURE_2D, fx->hiz_texture);
    int mw = w, mh = h;
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
    // History pair for temporal accumulation, and the a-trous denoise
    // ping-pong, both at the SSR resolution.
    if (!create_pingpong(w, h, GL_RGBA16F, &fx->ssr_history) ||
        !create_pingpong(w, h, GL_RGBA16F, &fx->ssr_atrous))
        return false;
    return true;
}

// Tear down everything create_ssr_buffers allocated (reflection FBO + Hi-Z
// pyramid + the history/denoise ping-pongs). Paired with it so the runtime
// resolution switch and free_postfx share one teardown list.
static void destroy_ssr_buffers(PostFX* fx) {
    gl_delete_fbo(&fx->ssr_fbo);
    gl_delete_texture(&fx->ssr_texture);
    gl_delete_fbo(&fx->hiz_fbo);
    gl_delete_texture(&fx->hiz_texture);
    free_pingpong(&fx->ssr_history);
    free_pingpong(&fx->ssr_atrous);
}

// Runtime resolution switch: delete the SSR buffers and rebuild them at the new
// resolution. No-op if unchanged.
void postfx_set_ssr_full_res(PostFX* fx, bool full_res) {
    if (!fx || fx->ssr_full_res == full_res)
        return;
    destroy_ssr_buffers(fx);
    fx->ssr_full_res = full_res;
    create_ssr_buffers(fx);
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
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, *out_texture,
                           0);
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

float postfx_clamp_render_scale(float render_scale) {
    if (render_scale < 0.5f)
        return 0.5f;
    if (render_scale > 1.0f)
        return 1.0f;
    return render_scale;
}

int postfx_scaled_dim(int post_dim, float render_scale) {
    if (render_scale >= 1.0f)
        return post_dim;
    int dim = ((int)roundf((float)post_dim * render_scale)) & ~1;
    return dim < 2 ? 2 : dim;
}

// Derive every size the chain runs at from the three inputs. One place, so
// create and resize cannot compute them differently.
static void postfx_derive_sizes(PostFX* fx, int width, int height, int ss_scale,
                                float render_scale) {
    fx->out_width = width;
    fx->out_height = height;
    fx->post_width = width * ss_scale;
    fx->post_height = height * ss_scale;
    fx->render_scale = render_scale;
    fx->width = postfx_scaled_dim(fx->post_width, render_scale);
    fx->height = postfx_scaled_dim(fx->post_height, render_scale);
    fx->bloom_width = fx->post_width / 2 > 0 ? fx->post_width / 2 : 1;
    fx->bloom_height = fx->post_height / 2 > 0 ? fx->post_height / 2 : 1;
    fx->half_width = fx->width / 2 > 0 ? fx->width / 2 : 1;
    fx->half_height = fx->height / 2 > 0 ? fx->height / 2 : 1;
}

// Bloom pyramid: one packed-float texture whose mip chain the pyramid passes
// walk, level 0 at half post res down to a ~8-16 px coarsest level. The
// 8-level cap bounds the pass count and the widest glow radius at large
// internal resolutions (it engages at 4K/SSAA sizes). Hand-built chain, so
// MAX_LEVEL is mandatory (an incomplete chain samples as black); one FBO gets
// re-attached per level like the hi-z build. MIN filter samples bilinearly
// WITHIN the level the passes pin. The FBO is attached and validated here,
// unlike the hi-z pyramid's, because this one is drawn into immediately.
static bool create_bloom_pyramid(PostFX* fx) {
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
        glTexImage2D(GL_TEXTURE_2D, mip, GL_R11F_G11F_B10F, mw, mh, 0, GL_RGB, GL_FLOAT, NULL);
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
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fx->bloom_texture,
                           0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Bloom pyramid framebuffer incomplete");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

// Compile the TAAU resolve and seed its sampler units, once. Absent by
// default: it exists only for a reduced render scale, which create_postfx may
// never see and a later resize may introduce. Kept if the scale returns to 1
// -- the seam dispatches on the canvas, not on the program. Mirrors the
// postfx_ensure_* family; true when the program is present or not wanted.
static bool postfx_ensure_taau_program(PostFX* fx) {
    if (fx->render_scale >= 1.0f || fx->taau_resolve_program)
        return true;
    fx->taau_resolve_program = create_taau_resolve_program();
    if (!fx->taau_resolve_program) {
        log_error("Failed to compile the TAAU resolve");
        return false;
    }
    glUseProgram(fx->taau_resolve_program->id);
    uniform_set_int(fx->taau_resolve_program->uniforms, "currentTex", 0);
    uniform_set_int(fx->taau_resolve_program->uniforms, "velocityTex", 1);
    uniform_set_int(fx->taau_resolve_program->uniforms, "historyTex", 2);
    return true;
}

// The uniforms that depend on a size and are NOT uploaded per draw. Exactly
// one qualifies -- GTAO's noiseScale, which maps the 4x4 rotation tile onto
// the AO buffer -- and it is the single reason a resize needs a fixup step at
// all, so it lives in one place both create and resize call.
static void postfx_seed_size_uniforms(PostFX* fx) {
    if (!fx->gtao_program)
        return;
    glUseProgram(fx->gtao_program->id);
    const float noise_scale[2] = {(float)fx->half_width / 4.0f, (float)fx->half_height / 4.0f};
    uniform_set_vec2(fx->gtao_program->uniforms, "noiseScale", noise_scale);
}

// Every resolution-dependent target, allocated from the sizes already on fx.
// Paired with postfx_free_targets, and called by BOTH create_postfx and the
// runtime resize -- which is why it allocates only, and touches no setting.
// In particular it reads fx->ssr_full_res rather than establishing it: that
// is a user-facing toggle (the GUI checkbox and postfx_set_ssr_full_res own
// it) and a resize must not silently return it to its default.
static bool postfx_alloc_targets(PostFX* fx) {
    // The HDR resolve target must be RGBA16F to match the MSAA source
    // (multisample blits require identical formats); the bloom chain never
    // reads alpha, so the cheaper packed-float format halves its bandwidth
    if (!create_color_fbo(fx->width, fx->height, GL_RGBA16F, &fx->hdr_fbo, &fx->hdr_texture))
        return false;
    if (!create_bloom_pyramid(fx))
        return false;
    if (!create_depth_fbo(fx->width, fx->height, &fx->depth_fbo, &fx->depth_texture))
        return false;
    // Resolve target for the scene pass's second color attachment
    // (view-space normal .xyz + SSR marker .a); RGBA16F to match the MSAA source
    if (!create_color_fbo(fx->width, fx->height, GL_RGBA16F, &fx->normal_fbo, &fx->normal_texture))
        return false;
    // RGBA rather than R: .r is the AO the whole chain has always read, .gba
    // carry the bent normal encoded to [0,1].
    //
    // 16F rather than 8, and the load-bearing reason is the buffer BELOW this
    // one. The temporal accumulation ping-pong is RGBA16F because a 0.97
    // feedback blend needs more than 256 levels to converge without banding --
    // and then the blur wrote its result back through an 8-bit target every
    // frame, which spends that precision on the way out. The two allocations
    // now agree about what the chain is worth keeping.
    //
    // What it does NOT do, measured rather than assumed: remove the contour
    // banding on a shallow AO gradient. That was this change's original
    // headline -- 0.92-1.00 is ~20 codes at 8 bits, so storage looked like the
    // obvious parent -- and widening the format leaves the band structure
    // untouched (identical flat-run lengths along the gradient, identical
    // span). The bands come from the estimator, not from how its answer is
    // stored. Kept anyway for the reason above, which stands on its own.
    //
    // What it costs on a lit frame: ~2% of pixels move, 99.5% of those by one
    // or two codes, which is dequantisation and nothing more.
    for (int i = 0; i < 2; i++) {
        if (!create_color_fbo(fx->half_width, fx->half_height, GL_RGBA16F, &fx->ssao_fbo[i],
                              &fx->ssao_texture[i]))
            return false;
    }
    // Clear the blurred slot: tonemap binds it as aoTex unconditionally, so on
    // any frame the GTAO chain does not run (--no-ssao with spec-occ on) it
    // would otherwise sample whatever the fresh allocation happens to contain.
    // glClearBufferfv rather than glClearColor + glClear: this now runs on
    // every resize, and the global clear colour belongs to the frame loop.
    glBindFramebuffer(GL_FRAMEBUFFER, fx->ssao_fbo[1]);
    const GLfloat unoccluded[4] = {1.0f, 0.5f, 0.5f, 0.5f}; // AO 1, zero bent normal
    glClearBufferfv(GL_COLOR, 0, unoccluded);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // Half-res temporal-AO accumulation ping-pong. Float, not 8-bit: an
    // exponential feedback blend needs more than 256 levels to avoid banding as
    // it converges.
    if (!create_pingpong(fx->half_width, fx->half_height, GL_RGBA16F, &fx->ao_history))
        return false;
    // The SSGI targets (GI radiance MRT + accumulation/a-trous pairs) are
    // lazily allocated on first enable -- see postfx_ensure_ssgi_targets.
    // SSR reflection buffer + Hi-Z traversal pyramid, at the resolution
    // fx->ssr_full_res selects (see create_ssr_buffers).
    if (!create_ssr_buffers(fx))
        return false;
    // Render-res resolve target for the scene pass's aux G-buffer (.xy motion +
    // .z linear view-Z), and the two post-res history buffers the TAA resolve
    // ping-pongs across frames.
    // Full float, matching the scene pass's aux attachment: the MSAA resolve
    // blit requires identical formats, and fp16 view-Z staircases at scene
    // scale (banded GTAO on large grounds).
    if (!create_color_fbo(fx->width, fx->height, GL_RGBA32F, &fx->aux_fbo, &fx->aux_texture))
        return false;
    // Point-sample the aux buffer: view-space Z is NOT screen-linear under
    // perspective, so LINEAR filtering would bend flat surfaces (banding) and
    // mangle GTAO's half-res reconstruction on fine geometry (bright speckle).
    // TAA reads it full-res 1:1, where NEAREST and LINEAR coincide.
    glBindTexture(GL_TEXTURE_2D, fx->aux_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // Full-res resolve target for the scene pass's albedo G-buffer (attachment 3),
    // consumed by the SSGI indirect-diffuse composite. RGBA8 (albedo is LDR).
    if (!create_color_fbo(fx->width, fx->height, GL_RGBA8, &fx->albedo_fbo, &fx->albedo_texture))
        return false;
    if (!create_pingpong(fx->post_width, fx->post_height, GL_RGBA16F, &fx->taa_history))
        return false;
    // TAAU canvas, only when the render scale actually splits the sizes; at
    // full scale the hdr buffer is post-sized and serves as the canvas.
    if (fx->render_scale < 1.0f) {
        if (!create_color_fbo(fx->post_width, fx->post_height, GL_RGBA16F, &fx->post_fbo,
                              &fx->post_texture))
            return false;
    }
    return true;
}

// Delete every resolution-dependent target: what postfx_alloc_targets built,
// plus every lazily-allocated group. Excludes the resolution-INDEPENDENT
// resources -- the luminance measure target, the noise texture, the froxel
// volumes (frustum-sized by design) and the colour-grading LUT (whose path the
// app may no longer hold) -- so a resize keeps them.
//
// For teardown only. Anything that keeps the PostFX alive afterwards wants
// postfx_invalidate_targets, which does this and then clears the flags saying
// the groups exist.
static void postfx_free_targets(PostFX* fx) {
    gl_delete_fbo(&fx->hdr_fbo);
    gl_delete_texture(&fx->hdr_texture);
    gl_delete_fbo(&fx->bloom_fbo);
    gl_delete_texture(&fx->bloom_texture);
    gl_delete_fbo(&fx->depth_fbo);
    gl_delete_texture(&fx->depth_texture);
    gl_delete_fbo(&fx->normal_fbo);
    gl_delete_texture(&fx->normal_texture);
    for (int i = 0; i < 2; i++) {
        gl_delete_fbo(&fx->ssao_fbo[i]);
        gl_delete_texture(&fx->ssao_texture[i]);
    }
    free_pingpong(&fx->ao_history);
    destroy_ssr_buffers(fx);
    gl_delete_fbo(&fx->aux_fbo);
    gl_delete_texture(&fx->aux_texture);
    gl_delete_fbo(&fx->albedo_fbo);
    gl_delete_texture(&fx->albedo_texture);
    free_pingpong(&fx->taa_history);
    gl_delete_fbo(&fx->post_fbo);
    gl_delete_texture(&fx->post_texture);

    // The lazily-allocated groups.
    gl_delete_texture(&fx->ssgi_gi_texture);
    free_pingpong(&fx->ssgi_history);
    free_pingpong(&fx->ssgi_atrous);
    gl_delete_texture(&fx->spec_occ_raw_texture);
    gl_delete_fbo(&fx->spec_occ_fbo);
    gl_delete_texture(&fx->spec_occ_texture);
    free_pingpong(&fx->spec_occ_history);
    gl_delete_fbo(&fx->dof_coc_fbo);
    gl_delete_texture(&fx->dof_coc_texture);
    gl_delete_fbo(&fx->dof_gather_fbo);
    gl_delete_texture(&fx->dof_far_texture);
    gl_delete_texture(&fx->dof_near_texture);
    gl_delete_fbo(&fx->dof_tile_fbo);
    gl_delete_texture(&fx->dof_tile_texture);
    gl_delete_fbo(&fx->dof_dilate_fbo);
    gl_delete_texture(&fx->dof_dilate_texture);
    gl_delete_fbo(&fx->dof_fbo);
    gl_delete_texture(&fx->dof_texture);
    gl_delete_fbo(&fx->fog_layer_fbo);
    gl_delete_texture(&fx->fog_layer_texture);
    free_pingpong(&fx->fog_layer_history);
    for (int i = 0; i < 2; i++) {
        gl_delete_fbo(&fx->cs_fbo[i]);
        gl_delete_texture(&fx->cs_texture[i]);
    }
    free_pingpong(&fx->cs_history);
    gl_delete_fbo(&fx->motion_blur_fbo);
    gl_delete_texture(&fx->motion_blur_texture);
    gl_delete_fbo(&fx->motion_blur_tile_fbo);
    gl_delete_texture(&fx->motion_blur_tile_texture);
    gl_delete_fbo(&fx->motion_blur_neighbor_fbo);
    gl_delete_texture(&fx->motion_blur_neighbor_texture);
    gl_delete_fbo(&fx->sss_diffuse_fbo);
    gl_delete_texture(&fx->sss_diffuse_texture);
    gl_delete_fbo(&fx->sss_delta_fbo);
    gl_delete_texture(&fx->sss_delta_texture);
    free_pingpong(&fx->sss_history);
    gl_delete_fbo(&fx->sss_pyr_fbo);
    gl_delete_texture(&fx->sss_pyr_color_texture);
    gl_delete_texture(&fx->sss_pyr_depth_texture);
    gl_delete_fbo(&fx->oit_accum_fbo);
    gl_delete_texture(&fx->oit_accum_texture);
    gl_delete_fbo(&fx->oit_revealage_fbo);
    gl_delete_texture(&fx->oit_revealage_texture);
    gl_delete_fbo(&fx->spec_fbo);
    gl_delete_texture(&fx->spec_texture);
    gl_delete_fbo(&fx->flare_fbo);
    gl_delete_texture(&fx->flare_texture);
}

// Delete the targets AND forget they existed, so the next frame that needs a
// lazily-allocated group rebuilds it at the current size. One call, not a
// pair: a ready flag left true beside a deleted handle makes the ensure_
// guard return early, and the pass then binds framebuffer 0 and samples
// texture 0 -- it draws into the default framebuffer instead of failing.
// free_shadow_map_array (shadow.c) is the same shape for the same reason.
//
// froxel_ready and froxel_prev_frame are deliberately absent. Those volumes
// are frustum-sized, not framebuffer-sized, so a resolution change does not
// invalidate them -- and their reprojection runs volume-to-volume through a
// stored camera rather than through any render-res buffer. Dropping the stamp
// would cost a frame of un-averaged cascade taps (visibly stair-stepped fog)
// to re-derive what already holds.
static void postfx_invalidate_targets(PostFX* fx) {
    postfx_free_targets(fx);
    fx->ssgi_ready = false;
    fx->spec_occ_ready = false;
    fx->dof_ready = false;
    fx->flare_ready = false;
    fx->fog_layer_ready = false;
    // One-shot latch: without clearing it, a transient failure at the old size
    // would permanently disable temporal fog at every later one.
    fx->fog_layer_failed = false;
    fx->fog_layer_frame = -1;
    fx->cs_ready = false;
    fx->motion_blur_ready = false;
    fx->sss_ready = false;
    fx->oit_ready = false;
    fx->spec_ready = false;
}

PostFX* create_postfx(int width, int height, int ss_scale, float render_scale) {
    if (width <= 0 || height <= 0) {
        log_error("create_postfx: invalid size %dx%d", width, height);
        return NULL;
    }
    if (ss_scale < 1) {
        ss_scale = 1;
    }
    float clamped = postfx_clamp_render_scale(render_scale);
    if (clamped != render_scale) {
        log_warn("create_postfx: render scale %.2f outside [0.5, 1]; using %.2f", render_scale,
                 clamped);
        render_scale = clamped;
    }

    PostFX* fx = calloc(1, sizeof(PostFX));
    if (!fx) {
        log_error("Failed to allocate memory for PostFX");
        return NULL;
    }

    // The scene and the pre-TAA chain render at the render resolution (post
    // size x render_scale); the TAA seam brings the frame to post size, and
    // the final tonemap pass box-downsamples that to the display size.
    postfx_derive_sizes(fx, width, height, ss_scale, render_scale);

    fx->exposure = NULL; // borrowed at postfx_set_exposure; see exposure.h
    // Working space (shaders/include/view.glsl), so these read as stops over
    // diffuse white rather than as absolute radiance: bloom starts exactly at
    // white, fades in over the half stop below it, and ignores anything past
    // +3. That is why they survived the photometric switch unchanged -- the
    // buffer they threshold is pre-exposed, so the numbers still mean what they
    // meant when a "1.0 is white" scene was the only kind the engine had.
    fx->bloom_threshold = 1.0f;
    fx->bloom_knee = 0.5f;
    fx->bloom_max_brightness = 8.0f;
    // The pyramid's level 0 accumulates every coarser level, carrying
    // several times the energy one blurred buffer did -- the composite
    // strength drops to match
    fx->bloom_strength = 0.015f;
    fx->bloom_enabled = true;

    fx->flare_enabled = false;
    // Measured on the flare fixture: 0.02 is present but easy to miss, 0.06
    // is unmistakable, and past ~0.15 the ghosts stop being artifacts and
    // start being the image. Bloom composites the SAME pyramid at 0.015, and
    // this sums roughly five taps of it, so the numbers are not comparable.
    fx->flare_strength = 0.03f;
    fx->flare_ghosts = 4;
    // Fraction of the mirrored-source-to-centre vector between ghosts. Below
    // ~0.2 they stack into one smear; near 1.0 they pile onto frame centre,
    // which is where the falloff weights them highest.
    fx->flare_ghost_spacing = 0.28f;
    fx->flare_halo_width = 0.32f;
    fx->flare_chroma = 2.0f; // pixels at the frame edge
    fx->flare_source_lod = 2;
    fx->ssao_enabled = true;
    fx->ssao_radius = 0.4f;
    fx->ssao_strength = 0.8f;
    fx->spec_occlusion_mode = POSTFX_SPEC_OCC_SPLIT; // Ambient spec on its own target, occluded in post
    fx->ao_edge_filter_enabled = true; // Depth-aware AO blur (no silhouette bleed)
    fx->contact_shadows_enabled = false; // Opt-in (spec 9.3); off leaves the frame untouched
    fx->cs_strength = 0.23f;             // Subtle: it stacks on CSM + AO in the same crevices, so a
                                         // higher weight crushes near-contacts to hard black
    fx->cs_distance = 0.3f;              // View-space reach; apps scene-scale this
    fx->ssgi_enabled = false;          // experimental; off by default
    fx->ssgi_intensity = 1.0f;
    fx->normals_enabled = true;
    fx->ssr_enabled = true;
    fx->ssr_strength = 1.0f;
    fx->ssr_max_distance = 8.0f;
    fx->ssr_thickness_min = 0.05f; // View units; apps scene-scale it (never down)
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
    fx->ca_enabled = false;
    fx->ca_strength = 3.0f; // pixels at the corner
    // Off: a perceptual model is a look, and the criterion three lines down is
    // that a defect fix defaults on and a look defaults off.
    fx->purkinje_enabled = false;
    fx->purkinje_strength = 0.7f;
    fx->purkinje_bias_ev = 0.0f;
    fx->grain_enabled = false;
    fx->grain_strength = 0.015f;
    // On by default: banding is a defect of the 8-bit write, not a look.
    fx->dither_enabled = true;
    fx->dither_strength = 1.0f;
    // No LUT until an app loads one. `lut_texture` 0 is what gates the branch,
    // so there is no separate enable to keep in step with it.
    fx->lut_texture = 0;
    fx->lut_size = 0;
    fx->lut_strength = 1.0f;
    fx->lut_interp = POSTFX_LUT_TETRAHEDRAL;
    fx->lut_name[0] = '\0';
    fx->frame_index = 0;

    fx->taa_enabled = false; // Enabled per-app (the render app turns it on when windowed)

    // Depth of field (off by default; targets allocated lazily on first enable)
    fx->dof_enabled = false;
    fx->dof_autofocus = true; // Track the camera's subject unless a focus is pinned
    fx->dof_focus_distance = 3.0f;
    fx->dof_focus_range = 1.5f;
    fx->dof_max_coc = 6.0f; // ~12px max blur — a natural background falloff,
                            // not the over-creamy look of a larger radius
    fx->dof_blades = 0;     // Circular aperture; >= 3 warps the kernel to an N-gon
    fx->dof_rotation = 0.0f;
    fx->dof_ready = false;
    fx->flare_ready = false;

    // Volumetric fog (off by default; targets allocated lazily on first
    // enable). World-space defaults are meter-scale; apps scene-scale them.
    fx->fog_enabled = false;
    fx->fog_density = 0.02f;
    fx->fog_height_falloff = 4.0f;
    fx->fog_floor_y = 0.0f;
    fx->fog_near = 0.0f; // derive
    fx->fog_far = 60.0f;
    fx->fog_depth_dist = 1.0f;
    fx->fog_temporal_blend = 0.9f;
    fx->fog_esm_array = 0;
    fx->fog_esm_scratch = 0;
    fx->fog_esm_fbo = 0;
    fx->fog_esm_layers = 0;
    // Sharper than this and the exponential stops surviving the blur (and fp32);
    // softer and blockers leak light through their own shadow.
    fx->fog_esm_k = 40.0f;
    fx->fog_esm_enabled = true;
    fx->froxel_grid_x = POSTFX_FROXEL_X;
    fx->froxel_grid_y = POSTFX_FROXEL_Y;
    fx->froxel_grid_z = POSTFX_FROXEL_Z;
    fx->froxel_built_x = 0;
    fx->froxel_built_y = 0;
    fx->froxel_built_z = 0;
    fx->fog_anisotropy = 0.45f;
    fx->fog_sun_boost = 1.0f;
    glm_vec3_copy((vec3){0.05f, 0.05f, 0.05f}, fx->fog_ambient);
    fx->froxel_ready = false;
    fx->froxel_prev_frame = -1;   // no froxel frame yet; 0 would match frame 0
    fx->fog_layer_frame = -1;     // likewise for the composited layer's history
    fx->fog_spot_enabled = false; // published per frame by shadow_publish_to_postfx
    // -1 is "no profile"; a calloc'd 0 would name the first one.
    fx->fog_spot_ies_profile = -1;

    // Motion blur (off by default; target allocated lazily on first enable).
    fx->motion_blur_enabled = false;
    fx->motion_blur_scale = 1.0f; // full-shutter velocity
    fx->motion_blur_ready = false;

    // Separable SSS profile ( targets allocated lazily on first skin frame).
    // The engine toggle gates the effect; these set the blur width/tint.
    // Default skin profile in slot 0 (rgb = per-channel scatter weight with red
    // widest, w = world radius); the app overwrites/extends this per skin material.
    glm_vec4_copy((vec4){1.0f, 0.3f, 0.2f, 0.2f}, fx->sss_profiles[0]);
    fx->sss_profile_count = 1;
    fx->sss_ready = false;
    fx->oit_ready = false;
    fx->spec_ready = false;

    // SSR tracing resolution and denoiser defaults. Set BEFORE the targets are
    // allocated because create_ssr_buffers sizes off ssr_full_res -- and kept
    // here rather than inside postfx_alloc_targets because these are settings:
    // the allocation runs again on every resize, and sweeping them into it
    // would quietly undo the GUI's SSR controls each time.
    fx->ssr_full_res = true; // Full-res tracing keeps reflections sharp
    fx->ssr_temporal = true;
    fx->ssr_denoise = true;
    fx->ssr_jitter = 0.03f;

    if (!postfx_alloc_targets(fx)) {
        free_postfx(fx);
        return NULL;
    }

    // Auto-exposure, three fixed-size targets. R16F for the measure: log2
    // luminance is small-range but needs sub-ulp precision. RG32F for the
    // histogram because its second channel is a SUM of up to 4096 log2 values,
    // where fp16 would start losing the low bits of the mean the reduce derives
    // from it. R32F for the 1x1 the CPU reads back.
    //
    // None of the three is resolution-dependent, so they are allocated once here
    // and freed only in free_postfx -- deliberately outside postfx_free_targets
    // and postfx_invalidate_targets, which exist for the targets a resize
    // rebuilds.
    //
    // The measure target no longer carries a mip chain. It used to be averaged
    // down to 1x1 by glGenerateMipmap, and there is no adapted-luminance
    // ping-pong beside it either, though this comment described one until 11.52:
    // that pass was deleted in f98ab0a.
    //
    // The 1x1 is read by the CPU for auto-exposure AND sampled on unit 7 by the
    // tonemap's Purkinje stage (spec 11.83), so it outlives a pinned exposure.
    if (!create_color_fbo(LUM_MEASURE_SIZE, LUM_MEASURE_SIZE, GL_R16F, &fx->lum_fbo,
                          &fx->lum_texture) ||
        !create_color_fbo(LUM_HISTOGRAM_BINS, LUM_HISTOGRAM_ROWS, GL_RG32F,
                          &fx->lum_hist_fbo, &fx->lum_hist_texture) ||
        !create_color_fbo(1, 1, GL_R32F, &fx->lum_reduce_fbo, &fx->lum_reduce_texture)) {
        free_postfx(fx);
        return NULL;
    }
    // Both consumers texelFetch, which ignores filtering -- but a mipmapped
    // min filter on a texture with no mips is incomplete, and the default IS
    // mipmapped. create_color_fbo already sets LINEAR; NEAREST states that
    // nothing here samples between texels.
    glBindTexture(GL_TEXTURE_2D, fx->lum_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, fx->lum_hist_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    unsigned int rng = 0x9E3779B9u;
    fx->noise_texture = create_ssao_noise_texture(&rng);

    fx->bloom_bright_program = create_bloom_bright_program();
    fx->bloom_down_program = create_bloom_down_program();
    fx->bloom_up_program = create_bloom_up_program();
    fx->flare_program = create_lens_flare_program();
    fx->tonemap_program = create_tonemap_program();
    fx->spec_occ_composite_program = create_spec_occ_composite_program();
    fx->gtao_program = create_gtao_program();
    fx->ssao_blur_program = create_ssao_blur_program();
    fx->temporal_accum_program = create_temporal_accum_program();
    fx->ssgi_composite_program = create_ssgi_composite_program();
    fx->ssgi_accum_program = create_ssgi_accum_program();
    fx->ssgi_atrous_program = create_ssgi_atrous_program();
    fx->ssr_atrous_program = create_ssr_atrous_program();
    fx->ssr_accum_program = create_ssr_accum_program();
    fx->lum_measure_program = create_lum_measure_program();
    fx->lum_histogram_program = create_lum_histogram_program();
    fx->lum_reduce_program = create_lum_reduce_program();
    fx->ssr_program = create_ssr_program();
    fx->ssr_hiz_program = create_ssr_hiz_program();
    fx->upsample_tent_program = create_upsample_tent_program();
    fx->froxel_inject_program = create_froxel_inject_program();
    fx->froxel_integrate_program = create_froxel_integrate_program();
    fx->froxel_composite_program = create_froxel_composite_program();
    fx->fog_esm_program = create_fog_esm_program();
    fx->taa_resolve_program = create_taa_resolve_program();
    // The TAAU resolve is compiled on demand -- here when the launch scale is
    // already reduced, otherwise at the resize that first reduces it.
    if (!postfx_ensure_taau_program(fx)) {
        free_postfx(fx);
        return NULL;
    }
    fx->dof_coc_program = create_dof_coc_program();
    fx->dof_tile_program = create_dof_tile_program();
    fx->dof_dilate_program = create_dof_dilate_program();
    fx->dof_gather_program = create_dof_gather_program();
    fx->dof_composite_program = create_dof_composite_program();
    fx->motion_blur_program = create_motion_blur_program();
    fx->motion_blur_tilemax_program = create_motion_blur_tilemax_program();
    fx->motion_blur_neighbormax_program = create_motion_blur_neighbormax_program();
    fx->sss_gather_program = create_sss_gather_program();
    fx->sss_pyr_seed_program = create_sss_pyr_seed_program();
    fx->sss_pyr_down_program = create_sss_pyr_down_program();
    fx->contact_shadow_program = create_contact_shadow_program();
    fx->oit_resolve_program = create_oit_resolve_program();
    if (!fx->sss_pyr_seed_program || !fx->sss_pyr_down_program ||
        !fx->contact_shadow_program || !fx->oit_resolve_program || !fx->sss_gather_program ||
        !fx->motion_blur_program ||
        !fx->motion_blur_tilemax_program || !fx->motion_blur_neighbormax_program ||
        !fx->bloom_bright_program || !fx->bloom_down_program || !fx->bloom_up_program ||
        !fx->tonemap_program || !fx->gtao_program || !fx->ssao_blur_program ||
        !fx->temporal_accum_program || !fx->ssgi_composite_program || !fx->ssgi_accum_program ||
        !fx->ssgi_atrous_program || !fx->ssr_atrous_program || !fx->ssr_accum_program ||
        !fx->lum_measure_program || !fx->lum_histogram_program || !fx->lum_reduce_program ||
        !fx->ssr_program || !fx->upsample_tent_program ||
        !fx->taa_resolve_program || !fx->dof_coc_program ||
        !fx->froxel_inject_program || !fx->froxel_integrate_program ||
        !fx->froxel_composite_program ||
        !fx->dof_tile_program || !fx->dof_dilate_program ||
        !fx->dof_gather_program || !fx->dof_composite_program) {
        free_postfx(fx);
        return NULL;
    }

    // Sampler bindings never change; set them once on the program objects
    glUseProgram(fx->bloom_bright_program->id);
    uniform_set_int(fx->bloom_bright_program->uniforms, "hdrTex", 0);
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
    // Unit 7 held the adapted-luminance texture the tonemap divided by. Exposure
    // is applied whole at the scene passes now -- but the Purkinje shift wants a
    // frame luminance again, so the metering 1x1 goes back on that same unit
    // (spec 11.83). A sampler2D, which is what the note below was insuring
    // against; the LUT staying on 11 is what makes that harmless.
    uniform_set_int(fx->tonemap_program->uniforms, "purkinjeAdaptTex", 7);
    uniform_set_int(fx->tonemap_program->uniforms, "auxTex", 9); // linZ + roughness for spec-occ
    uniform_set_int(fx->tonemap_program->uniforms, "csTex", 10); // contact-shadow visibility
    // 11, not the free 7, and the reason is NOT that 7 would be an error today:
    // no sampler in this program names it, so a sampler3D there would link and
    // draw perfectly well. Unit 7 carries a BINDING (the retired adapted-
    // luminance slot, bound to 2D 0 each frame) and the INVALID_OPERATION rule
    // is about two sampler DECLARATIONS of different types sharing a unit.
    // Appending is what keeps that unrepresentable if a 2D sampler is ever
    // restored to 7 -- it is insurance, not a fix.
    uniform_set_int(fx->tonemap_program->uniforms, "lutTex", 11);
    // Debug view 7 only: the split term is applied in the composite pass, so
    // this exists so that view can show what was applied rather than recompute
    // a second opinion about it.
    uniform_set_int(fx->tonemap_program->uniforms, "specOccTex", 12);

    glUseProgram(fx->lum_measure_program->id);
    uniform_set_int(fx->lum_measure_program->uniforms, "hdrTex", 0);
    glUseProgram(fx->lum_histogram_program->id);
    uniform_set_int(fx->lum_histogram_program->uniforms, "lumTex", 0);
    glUseProgram(fx->lum_reduce_program->id);
    uniform_set_int(fx->lum_reduce_program->uniforms, "histTex", 0);

    glUseProgram(fx->ssr_program->id);
    uniform_set_int(fx->ssr_program->uniforms, "depthTex", 0);
    uniform_set_int(fx->ssr_program->uniforms, "normalsTex", 1);
    uniform_set_int(fx->ssr_program->uniforms, "hdrTex", 2);
    uniform_set_int(fx->ssr_program->uniforms, "probeTex", 3);
    uniform_set_int(fx->ssr_program->uniforms, "hizTex", 4);
    // The probe atlas (spec 11.70), on its own unit rather than aliasing
    // probeTex: those are two sampler TYPES, and pointing both at one image unit
    // is an INVALID_OPERATION at draw. This pass has the units to spare -- it is
    // pbr_frag that does not.
    uniform_set_int(fx->ssr_program->uniforms, "probeAtlasTex", 5);
    glUseProgram(fx->ssr_hiz_program->id);
    uniform_set_int(fx->ssr_hiz_program->uniforms, "srcTex", 0);
    glUseProgram(fx->froxel_inject_program->id);
    uniform_set_int(fx->froxel_inject_program->uniforms, "shadowMaps", 1);
    uniform_set_int(fx->froxel_inject_program->uniforms, "punctualShadowMaps", 2);
    uniform_set_int(fx->froxel_inject_program->uniforms, "historyVolume", 3);
    uniform_set_int(fx->froxel_inject_program->uniforms, "cloudShadowTex", 4);

    // The stand-in for an absent optional sampler; see white_tex in postfx.h for why an
    // incomplete texture is worse than a real one.
    const float white_texel = 1.0f;
    glGenTextures(1, &fx->white_tex);
    glBindTexture(GL_TEXTURE_2D, fx->white_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, 1, 1, 0, GL_RED, GL_FLOAT, &white_texel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(fx->froxel_integrate_program->id);
    uniform_set_int(fx->froxel_integrate_program->uniforms, "scatterVolume", 0);
    glUseProgram(fx->froxel_composite_program->id);
    uniform_set_int(fx->froxel_composite_program->uniforms, "linDepthTex", 0);
    uniform_set_int(fx->froxel_composite_program->uniforms, "integratedVolume", 1);
    uniform_set_int(fx->froxel_composite_program->uniforms, "layerTex", 2);
    uniform_set_int(fx->froxel_composite_program->uniforms, "aerialVolume", 3);
    uniform_set_int(fx->froxel_composite_program->uniforms, "momentTex", 4);

    glUseProgram(fx->dof_coc_program->id);
    uniform_set_int(fx->dof_coc_program->uniforms, "sceneTex", 0);
    uniform_set_int(fx->dof_coc_program->uniforms, "depthTex", 1);
    glUseProgram(fx->dof_tile_program->id);
    uniform_set_int(fx->dof_tile_program->uniforms, "cocColorTex", 0);
    uniform_set_int(fx->dof_tile_program->uniforms, "tileSize", DOF_TILE);
    glUseProgram(fx->dof_dilate_program->id);
    uniform_set_int(fx->dof_dilate_program->uniforms, "tileTex", 0);
    glUseProgram(fx->dof_gather_program->id);
    uniform_set_int(fx->dof_gather_program->uniforms, "cocColorTex", 0);
    uniform_set_int(fx->dof_gather_program->uniforms, "tileTex", 1);
    uniform_set_int(fx->dof_gather_program->uniforms, "tileSize", DOF_TILE);
    glUseProgram(fx->dof_composite_program->id);
    uniform_set_int(fx->dof_composite_program->uniforms, "sceneTex", 0);
    uniform_set_int(fx->dof_composite_program->uniforms, "nearTex", 1);
    uniform_set_int(fx->dof_composite_program->uniforms, "farTex", 2);
    uniform_set_int(fx->dof_composite_program->uniforms, "depthTex", 3);

    glUseProgram(fx->motion_blur_tilemax_program->id);
    uniform_set_int(fx->motion_blur_tilemax_program->uniforms, "auxTex", 0);
    glUseProgram(fx->motion_blur_neighbormax_program->id);
    uniform_set_int(fx->motion_blur_neighbormax_program->uniforms, "tileTex", 0);
    glUseProgram(fx->motion_blur_program->id);
    uniform_set_int(fx->motion_blur_program->uniforms, "sceneTex", 0);
    uniform_set_int(fx->motion_blur_program->uniforms, "neighborMaxTex", 1);
    uniform_set_int(fx->motion_blur_program->uniforms, "velocityTex", 2);

    glUseProgram(fx->sss_gather_program->id);
    uniform_set_int(fx->sss_gather_program->uniforms, "pyrColor", 0);
    uniform_set_int(fx->sss_gather_program->uniforms, "origTex", 1);
    uniform_set_int(fx->sss_gather_program->uniforms, "auxTex", 2);
    uniform_set_int(fx->sss_gather_program->uniforms, "depthTex", 3);
    glUseProgram(fx->sss_pyr_seed_program->id);
    uniform_set_int(fx->sss_pyr_seed_program->uniforms, "srcTex", 0);
    uniform_set_int(fx->sss_pyr_seed_program->uniforms, "auxTex", 1);
    uniform_set_int(fx->sss_pyr_seed_program->uniforms, "depthTex", 2);
    glUseProgram(fx->sss_pyr_down_program->id);
    uniform_set_int(fx->sss_pyr_down_program->uniforms, "srcColor", 0);
    uniform_set_int(fx->sss_pyr_down_program->uniforms, "srcDepth", 1);
    glUseProgram(fx->oit_resolve_program->id);
    uniform_set_int(fx->oit_resolve_program->uniforms, "accumTex", 0);
    uniform_set_int(fx->oit_resolve_program->uniforms, "revealageTex", 1);

    glUseProgram(fx->spec_occ_composite_program->id);
    uniform_set_int(fx->spec_occ_composite_program->uniforms, "specTex", 0);
    uniform_set_int(fx->spec_occ_composite_program->uniforms, "aoTex", 1);
    uniform_set_int(fx->spec_occ_composite_program->uniforms, "normalsTex", 2);
    uniform_set_int(fx->spec_occ_composite_program->uniforms, "auxTex", 3);
    uniform_set_int(fx->spec_occ_composite_program->uniforms, "specOccTex", 4);

    glUseProgram(fx->gtao_program->id);
    uniform_set_int(fx->gtao_program->uniforms, "linDepthTex", 0);
    uniform_set_int(fx->gtao_program->uniforms, "noiseTex", 1);
    uniform_set_int(fx->gtao_program->uniforms, "normalsTex", 2);
    uniform_set_int(fx->gtao_program->uniforms, "hdrTex", 3);  // SSGI radiance source
    uniform_set_int(fx->gtao_program->uniforms, "specTex", 4); // split spec share of that radiance
    // noiseScale is size-dependent and seeded by postfx_seed_size_uniforms
    // below, which the resize path calls too.

    // No texelSize for either of the next two: every consumer of ssao_blur and
    // of the shared tent uploads it per draw from its own resolution, so a
    // seed here would be dead, and a dead seed reads as a sanctioned fallback
    // that a future third consumer could rely on by forgetting to upload.
    glUseProgram(fx->ssao_blur_program->id);
    uniform_set_int(fx->ssao_blur_program->uniforms, "aoTex", 0);
    uniform_set_int(fx->ssao_blur_program->uniforms, "auxTex", 1); // linZ for the bilateral weight

    glUseProgram(fx->contact_shadow_program->id);
    uniform_set_int(fx->contact_shadow_program->uniforms, "linDepthTex", 0);
    uniform_set_int(fx->contact_shadow_program->uniforms, "normalsTex", 1);

    glUseProgram(fx->upsample_tent_program->id);
    uniform_set_int(fx->upsample_tent_program->uniforms, "srcTex", 0);

    // temporal_accum, ssr_accum, and ssgi_accum are seeded entirely by
    // run_temporal_accum, which owns their unit layout and texelSize together.

    glUseProgram(fx->ssgi_composite_program->id);
    uniform_set_int(fx->ssgi_composite_program->uniforms, "giTex", 0);
    uniform_set_int(fx->ssgi_composite_program->uniforms, "albedoTex", 1);

    // Both a-trous programs get texelSize per call from run_atrous (the SSR one
    // tracks the full-res toggle; the SSGI one always resolves to the AO texel).
    glUseProgram(fx->ssgi_atrous_program->id);
    uniform_set_int(fx->ssgi_atrous_program->uniforms, "giTex", 0);
    uniform_set_int(fx->ssgi_atrous_program->uniforms, "linDepthTex", 1);
    uniform_set_int(fx->ssgi_atrous_program->uniforms, "normalsTex", 2);

    glUseProgram(fx->ssr_atrous_program->id);
    uniform_set_int(fx->ssr_atrous_program->uniforms, "reflTex", 0);
    uniform_set_int(fx->ssr_atrous_program->uniforms, "linDepthTex", 1);
    uniform_set_int(fx->ssr_atrous_program->uniforms, "normalsTex", 2);

    // The TAAU resolve's own units are seeded by postfx_ensure_taau_program,
    // wherever it first compiles; run_taau_resolve uploads the sizes and the
    // jitter, which are the only things that vary.
    postfx_seed_size_uniforms(fx);
    glUseProgram(0);

    create_fullscreen_quad_vao(&fx->quad_vao, &fx->quad_vbo);

    check_gl_error("create_postfx");
    return fx;
}

// Allocate the depth-of-field targets on first use so the feature is free when
// off: the half-render-res CoC+colour buffer, the near/far gather MRT, the
// tile-pass pair (per-tile CoC maxima + their dilation), and a post-res
// composite. Returns false and leaves DoF disabled if allocation fails.
static bool postfx_ensure_dof_targets(PostFX* fx) {
    if (fx->dof_ready)
        return true;
    fx->dof_tile_w = (fx->half_width + DOF_TILE - 1) / DOF_TILE;
    fx->dof_tile_h = (fx->half_height + DOF_TILE - 1) / DOF_TILE;
    if (!create_color_fbo(fx->half_width, fx->half_height, GL_RGBA16F, &fx->dof_coc_fbo,
                          &fx->dof_coc_texture) ||
        !create_color_fbo(fx->half_width, fx->half_height, GL_RGBA16F, &fx->dof_gather_fbo,
                          &fx->dof_far_texture) ||
        !create_color_fbo(fx->dof_tile_w, fx->dof_tile_h, GL_RG16F, &fx->dof_tile_fbo,
                          &fx->dof_tile_texture) ||
        !create_color_fbo(fx->dof_tile_w, fx->dof_tile_h, GL_RG16F, &fx->dof_dilate_fbo,
                          &fx->dof_dilate_texture) ||
        !create_color_fbo(fx->post_width, fx->post_height, GL_RGBA16F, &fx->dof_fbo,
                          &fx->dof_texture)) {
        log_error("Failed to allocate depth-of-field targets");
        fx->dof_enabled = false;
        return false;
    }
    // Near field joins the gather FBO as attachment 1 (the SSGI-on-GTAO
    // idiom); the draw-buffer pair is FBO state, set once here.
    fx->dof_near_texture =
        create_texture_2d_float(fx->half_width, fx->half_height, GL_RGBA16F, GL_RGBA, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, fx->dof_gather_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                           fx->dof_near_texture, 0);
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("DoF gather MRT framebuffer is not complete");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        fx->dof_enabled = false;
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, fx->half_width, fx->half_height, 0, GL_RGBA,
                 GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, fx->ssao_fbo[0]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, fx->ssgi_gi_texture,
                           0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!create_pingpong(fx->half_width, fx->half_height, GL_RGBA16F, &fx->ssgi_history) ||
        !create_pingpong(fx->half_width, fx->half_height, GL_RGBA16F, &fx->ssgi_atrous)) {
        log_error("Failed to allocate SSGI targets");
        return false;
    }
    fx->ssgi_ready = true;
    return true;
}

// Allocate the split spec-occ targets on first enable (the pattern above): the
// half-res reflection-lobe sums as attachment 2 of the GTAO FBO, plus the blur
// target and accumulation pair that denoise them. Two channels, because what
// the sweep writes is an estimator's numerator and denominator rather than a
// colour -- see gtao_frag.glsl's SpecOccOut.
//
// The sums ride the AO chain's own denoise programs unchanged: the blur weights
// every channel it is handed identically, and the accumulator blends a vec4, so
// neither needed to learn what this buffer means. What they cannot share is the
// DRAW, since each writes one target -- hence a second blur pass and a second
// accumulation rather than teaching a program that the contact-shadow denoise
// also runs to carry a passenger.
static bool postfx_ensure_spec_occ_targets(PostFX* fx) {
    if (fx->spec_occ_ready)
        return true;
    glGenTextures(1, &fx->spec_occ_raw_texture);
    glBindTexture(GL_TEXTURE_2D, fx->spec_occ_raw_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, fx->half_width, fx->half_height, 0, GL_RG, GL_FLOAT,
                 NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, fx->ssao_fbo[0]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D,
                           fx->spec_occ_raw_texture, 0);
    // Checked here, unlike the SSGI re-attach above: a re-attach can fail the
    // completeness rules (mismatched size, an unsupported format) and an
    // incomplete FBO discards every draw into it silently, which reads as the
    // feature having no effect rather than as an error.
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        log_error("spec-occ attachment left the GTAO framebuffer incomplete (0x%x)", status);
        return false;
    }
    if (!create_color_fbo(fx->half_width, fx->half_height, GL_RG16F, &fx->spec_occ_fbo,
                          &fx->spec_occ_texture) ||
        !create_pingpong(fx->half_width, fx->half_height, GL_RG16F, &fx->spec_occ_history)) {
        log_error("Failed to allocate spec-occ targets");
        return false;
    }
    fx->spec_occ_ready = true;
    return true;
}

// Allocate the froxel fog volumes on first enable (same lazy pattern): the
// scatter volume, the integrated volume the composite samples, and the history
// the inject pass reprojects against. Fixed dimensions -- the grid covers the
// frustum out to fog_far, so render resolution does not size it.
static bool postfx_ensure_froxel_targets(PostFX* fx) {
    // A dimension change invalidates the volumes AND the history in them, so it
    // drops the adjacency stamp too -- reprojecting a differently-sized history
    // would read neighbours, not this cell.
    if (fx->froxel_ready &&
        (fx->froxel_built_x != fx->froxel_grid_x || fx->froxel_built_y != fx->froxel_grid_y ||
         fx->froxel_built_z != fx->froxel_grid_z)) {
        glDeleteTextures(2, fx->froxel_scatter);
        glDeleteTextures(1, &fx->froxel_integrated);
        glDeleteFramebuffers(1, &fx->froxel_fbo);
        fx->froxel_scatter[0] = fx->froxel_scatter[1] = 0;
        fx->froxel_integrated = 0;
        fx->froxel_fbo = 0;
        fx->froxel_ready = false;
        fx->froxel_prev_frame = -1;
    }
    if (fx->froxel_ready)
        return true;
    fx->froxel_built_x = fx->froxel_grid_x > 0 ? fx->froxel_grid_x : POSTFX_FROXEL_X;
    fx->froxel_built_y = fx->froxel_grid_y > 0 ? fx->froxel_grid_y : POSTFX_FROXEL_Y;
    fx->froxel_built_z = fx->froxel_grid_z > 0 ? fx->froxel_grid_z : POSTFX_FROXEL_Z;
    // GL_TEXTURE_3D rather than the codebase's usual GL_TEXTURE_2D_ARRAY: the
    // composite reads a single trilinear tap that filters ACROSS slices, which
    // an array texture does not do, and CLAMP on R makes a lookup past the last
    // slice hold the fully integrated column instead of wrapping to the near one.
    // Attachment-less FBO: draw_volume_slices binds one layer per draw, so
    // completeness is only meaningful once a layer is attached (checked there).
    glGenFramebuffers(1, &fx->froxel_fbo);
    for (int i = 0; i < 2; i++) {
        fx->froxel_scatter[i] =
            create_texture_3d_float(fx->froxel_built_x, fx->froxel_built_y, fx->froxel_built_z,
                                    GL_RGBA16F, GL_RGBA, NULL);
    }
    fx->froxel_integrated =
        create_texture_3d_float(fx->froxel_built_x, fx->froxel_built_y, fx->froxel_built_z,
                                GL_RGBA16F, GL_RGBA, NULL);
    if (!fx->froxel_scatter[0] || !fx->froxel_scatter[1] || !fx->froxel_integrated) {
        log_error("Failed to allocate froxel fog volumes");
        return false;
    }
    // Zero both scatter volumes: an unwritten parity slot is otherwise
    // undefined (glTexImage3D with NULL), and the temporal blend would weight
    // that at 0.9 and write the result back as the next frame's history.
    glBindFramebuffer(GL_FRAMEBUFFER, fx->froxel_fbo);
    glViewport(0, 0, fx->froxel_built_x, fx->froxel_built_y);
    for (int i = 0; i < 2; i++) {
        for (int slice = 0; slice < fx->froxel_built_z; slice++) {
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, fx->froxel_scatter[i],
                                      0, slice);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    fx->froxel_ready = true;
    return true;
}

// Allocate the composited fog layer plus its temporal ping-pong, on the first
// TAA frame with fog enabled. Deliberately NOT part of postfx_ensure_froxel_
// targets: those are resolution-independent by design (the volume covers the
// frustum, not the framebuffer), while these are full-res, and a headless or
// TAA-off run never touches them -- at ss_scale 2 that is ~200 MB unallocated.
static bool postfx_ensure_fog_layer_targets(PostFX* fx) {
    if (fx->fog_layer_ready)
        return true;
    if (fx->fog_layer_failed)
        return false;
    if (!create_color_fbo(fx->width, fx->height, GL_RGBA16F, &fx->fog_layer_fbo,
                          &fx->fog_layer_texture) ||
        !create_pingpong(fx->width, fx->height, GL_RGBA16F, &fx->fog_layer_history)) {
        // One-shot, unlike the other ensure_ helpers, which are retried every
        // frame. Fog is the only one whose caller has a complete path for "no
        // targets" -- it composites straight into the scene, just without the
        // temporal pass -- so retrying buys nothing and orphans another set of
        // full-res targets every frame (~200 MB at ss_scale 2, plus 60 Hz of
        // log spam).
        log_error("Failed to allocate fog layer targets; fog will run untemporal");
        fx->fog_layer_failed = true;
        return false;
    }
    fx->fog_layer_ready = true;
    return true;
}

// Allocate the contact-shadow targets on first enable (fog pattern): two R8
// buffers (raw march, then bilateral-blurred) plus the R16F temporal ping-pong.
// R16F history because the 0.9-feedback accumulation bands in 8 bits. FULL
// internal res, not half: contact shadows are a sharpness feature, and half-res
// output smears the per-texel silhouette response into a visible stipple.
static bool postfx_ensure_contact_targets(PostFX* fx) {
    if (fx->cs_ready)
        return true;
    if (!create_color_fbo(fx->width, fx->height, GL_R8, &fx->cs_fbo[0], &fx->cs_texture[0]) ||
        !create_color_fbo(fx->width, fx->height, GL_R8, &fx->cs_fbo[1], &fx->cs_texture[1]) ||
        !create_pingpong(fx->width, fx->height, GL_R16F, &fx->cs_history)) {
        log_error("Failed to allocate contact-shadow targets");
        return false;
    }
    fx->cs_ready = true;
    return true;
}

// Allocate the motion-blur targets on first enable so the feature is free while
// off (DoF pattern): a post-res RGBA16F reconstruction scratch plus the two
// RG16F tile buffers (tile-max + neighbor-max velocity) at tile resolution.
static bool postfx_ensure_motion_blur_targets(PostFX* fx) {
    if (fx->motion_blur_ready)
        return true;
    fx->motion_blur_tile_w = (fx->post_width + MOTION_BLUR_TILE - 1) / MOTION_BLUR_TILE;
    fx->motion_blur_tile_h = (fx->post_height + MOTION_BLUR_TILE - 1) / MOTION_BLUR_TILE;
    if (!create_color_fbo(fx->post_width, fx->post_height, GL_RGBA16F, &fx->motion_blur_fbo,
                          &fx->motion_blur_texture) ||
        !create_color_fbo(fx->motion_blur_tile_w, fx->motion_blur_tile_h, GL_RG16F,
                          &fx->motion_blur_tile_fbo, &fx->motion_blur_tile_texture) ||
        !create_color_fbo(fx->motion_blur_tile_w, fx->motion_blur_tile_h, GL_RG16F,
                          &fx->motion_blur_neighbor_fbo, &fx->motion_blur_neighbor_texture)) {
        log_error("Failed to allocate motion blur targets");
        return false;
    }
    fx->motion_blur_ready = true;
    return true;
}

// Motion blur (4.15): reconstruct plausible blur from the aux velocity buffer.
// (1) tile-max reduces the velocity to one dominant vector per tile, (2)
// neighbor-max spreads it across the 3x3 tile neighborhood so a fast object
// blurs past its silhouette, (3) the reconstruction gathers the scene along
// that dominant velocity. Runs post-seam at post res on the canvas; the result
// is blitted back over it so DoF/bloom/tonemap read the blurred image (GL 4.1
// has no texture barrier, so the pass cannot read+write the canvas in place).
static void postfx_run_motion_blur(PostFX* fx, GLuint canvas_fbo, GLuint canvas_tex) {
    // Pass 1: tile-max -- velocity -> per-tile dominant velocity.
    const float aux_texel[2] = {1.0f / (float)fx->post_width, 1.0f / (float)fx->post_height};
    glBindFramebuffer(GL_FRAMEBUFFER, fx->motion_blur_tile_fbo);
    glViewport(0, 0, fx->motion_blur_tile_w, fx->motion_blur_tile_h);
    glUseProgram(fx->motion_blur_tilemax_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx->aux_texture);
    uniform_set_vec2(fx->motion_blur_tilemax_program->uniforms, "auxTexel", aux_texel);
    uniform_set_int(fx->motion_blur_tilemax_program->uniforms, "tileSize", MOTION_BLUR_TILE);
    draw_fullscreen_quad(fx->quad_vao);

    // Pass 2: neighbor-max -- max over each tile's 3x3 neighborhood.
    const float tile_texel[2] = {1.0f / (float)fx->motion_blur_tile_w,
                                 1.0f / (float)fx->motion_blur_tile_h};
    glBindFramebuffer(GL_FRAMEBUFFER, fx->motion_blur_neighbor_fbo);
    glUseProgram(fx->motion_blur_neighbormax_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx->motion_blur_tile_texture);
    uniform_set_vec2(fx->motion_blur_neighbormax_program->uniforms, "tileTexel", tile_texel);
    draw_fullscreen_quad(fx->quad_vao);

    // Pass 3: reconstruction -- gather the scene along the dominant velocity.
    glBindFramebuffer(GL_FRAMEBUFFER, fx->motion_blur_fbo);
    glViewport(0, 0, fx->post_width, fx->post_height);
    glUseProgram(fx->motion_blur_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, canvas_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, fx->motion_blur_neighbor_texture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, fx->aux_texture);
    uniform_set_vec2(fx->motion_blur_program->uniforms, "texelSize", aux_texel);
    uniform_set_float(fx->motion_blur_program->uniforms, "scale", fx->motion_blur_scale);
    uniform_set_float(fx->motion_blur_program->uniforms, "maxBlurPx", (float)MOTION_BLUR_TILE);
    draw_fullscreen_quad(fx->quad_vao);

    // Copy the reconstructed scene back over the canvas (same size/format,
    // NEAREST -> exact copy) so the rest of the chain reads the blurred result.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fx->motion_blur_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, canvas_fbo);
    glBlitFramebuffer(0, 0, fx->post_width, fx->post_height, 0, 0, fx->post_width, fx->post_height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    check_gl_error("postfx motion blur");
}

// Split spec-occ composite: fold the ambient specular the scene pass routed
// to its own buffer back over the scene, in one blended pass -- the shader
// outputs (spec * SO, aoFactor) and the (GL_ONE, GL_SRC_ALPHA) blend forms
// spec * SO + scene * aoFactor in place. This is where AO lands on the whole
// frame in split mode; the tonemap's ambient factor stands down via aoEnabled.
// Runs before TAA so the reunited frame is stabilized as one image, and
// before the SSR march / fog / bloom so every later pass sees the corrected
// color.
static void postfx_run_spec_occ_composite(PostFX* fx, GLuint ao_result_tex, GLuint spec_occ_tex,
                                          bool have_normals, bool aux_written) {
    glBindFramebuffer(GL_FRAMEBUFFER, fx->hdr_fbo);
    glViewport(0, 0, fx->width, fx->height);
    glUseProgram(fx->spec_occ_composite_program->id);
    UniformManager* sc = fx->spec_occ_composite_program->uniforms;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx->spec_texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, fx->ssao_enabled ? ao_result_tex : 0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, have_normals ? fx->normal_texture : 0);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, aux_written ? fx->aux_texture : 0);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, spec_occ_tex);
    // Per draw rather than seeded, the rule the texelSize uniforms follow: this
    // pass and the tonemap read the same buffer at different resolutions, so a
    // seeded value would be right for one of them and a silent fallback for the
    // other.
    const float ao_res[2] = {(float)fx->half_width, (float)fx->half_height};
    uniform_set_vec2(sc, "aoRes", ao_res);
    // The term needs the AO chain to have run, the normals for its guard, and
    // the aux depth for both magnifications. Missing any of them, fold the
    // specular back unoccluded rather than read an unbound unit.
    uniform_set_int(sc, "aoActive",
                    fx->ssao_enabled && have_normals && aux_written ? 1 : 0);
    uniform_set_float(sc, "aoStrength", fx->ssao_strength);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_SRC_ALPHA);
    draw_fullscreen_quad(fx->quad_vao);
    glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    check_gl_error("postfx spec-occ composite");
}

// Allocate the ambient-specular resolve target on the first split frame.
// R11G11B10F to match the MSAA attachment exactly (a multisample blit with
// mismatched formats is an INVALID_OPERATION and resolves nothing).
static bool postfx_ensure_spec_target(PostFX* fx) {
    if (fx->spec_ready)
        return true;
    if (!create_color_fbo(fx->width, fx->height, GL_R11F_G11F_B10F, &fx->spec_fbo,
                          &fx->spec_texture)) {
        log_error("Failed to allocate the ambient-specular resolve target");
        return false;
    }
    fx->spec_ready = true;
    return true;
}

// Allocate the SSS targets on first skin frame (DoF pattern): the full-res
// resolve of the skin-diffuse attachment plus the H/V separable-blur ping-pong.
// Coarsest level the gather may read, from the render height alone.
//
// A level's texels are 2^L render pixels, so the wide terms otherwise land where
// the subject spans a handful of them and reads as facets. Holding texels at
// 1/SSS_MAX_LEVEL_TEXEL_FRACTION of frame height keeps a subject a third of the
// frame across at about a dozen of them.
//
// This is the delivered-scatter CEILING, and it is resolution-independent by
// construction: the cap is a fraction of height and the pixels per world unit
// are proportional to height, so the two cancel -- which is exactly what the
// pixel cap this branch removed failed to do.
static float sss_lod_cap_for_height(int height) {
    return log2f(fmaxf((float)height / SSS_MAX_LEVEL_TEXEL_FRACTION, 1.0f));
}

// Size of pyramid level L. The ONE place the chain's geometry is expressed, so
// the allocator and the walk cannot disagree -- a mismatch there writes a level
// at the wrong viewport and corrupts the chain silently, and the bloom walk's
// `width >> mip` form is only safe because it stops at 15 px, where it still
// agrees with a halving loop. Below that the two diverge.
static void sss_pyr_level_size(const PostFX* fx, int level, int* w, int* h) {
    int mw = fx->width;
    int mh = fx->height;
    for (int i = 0; i < level; i++) {
        mw = mw > 1 ? mw / 2 : 1;
        mh = mh > 1 ? mh / 2 : 1;
    }
    *w = mw;
    *h = mh;
}

static bool create_sss_pyramid(PostFX* fx) {
    int mw = fx->width;
    int mh = fx->height;
    // Build exactly as far as the gather can read, and no further. The level
    // count and the LOD cap are the same rule; expressing them separately built
    // two levels per frame per profile that nothing could sample, at ~17 GL
    // calls each. The cap is a fraction of frame height, so this still grows
    // with resolution -- which is what keeps the ceiling resolution-independent.
    fx->sss_pyr_mips = (int)ceilf(sss_lod_cap_for_height(fx->height)) + 1;
    (void)mw;
    (void)mh;

    const GLenum fmts[2] = {GL_RGBA16F, GL_R32F};
    const GLenum chans[2] = {GL_RGBA, GL_RED};
    GLuint* texes[2] = {&fx->sss_pyr_color_texture, &fx->sss_pyr_depth_texture};
    for (int t = 0; t < 2; t++) {
        glGenTextures(1, texes[t]);
        glBindTexture(GL_TEXTURE_2D, *texes[t]);
        for (int mip = 0; mip < fx->sss_pyr_mips; mip++) {
            int lw, lh;
            sss_pyr_level_size(fx, mip, &lw, &lh);
            glTexImage2D(GL_TEXTURE_2D, mip, fmts[t], lw, lh, 0, chans[t], GL_FLOAT, NULL);
        }
        // LINEAR between mips, unlike the bloom pyramid's MIPMAP_NEAREST. Bloom
        // pins a level per draw; the gather's LOD is continuous in the scatter
        // radius, and NEAREST would step at every level boundary -- rings again,
        // in the one place this whole pyramid exists to remove them.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Without this a hand-built chain samples as black.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, fx->sss_pyr_mips - 1);
    }

    glGenFramebuffers(1, &fx->sss_pyr_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fx->sss_pyr_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           fx->sss_pyr_color_texture, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                           fx->sss_pyr_depth_texture, 0);
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!ok)
        log_error("SSS pyramid framebuffer incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok;
}

// Pre-filter sigma of pyramid level L, in render pixels. Mirrors levelSigmaPx
// in sss_gather_frag.glsl; one 13-tap step contributes variance 0.4375 in its
// destination texels, so var_L = 0.5833 * (1 - 4^-L) and level L's texels are
// 2^L render pixels wide.
static float sss_level_sigma_px(float level) {
    return powf(2.0f, level) * sqrtf(0.5833333f * (1.0f - powf(2.0f, -2.0f * level)));
}

static float sss_lod_cap(const PostFX* fx) {
    return fminf((float)(fx->sss_pyr_mips - 1), sss_lod_cap_for_height(fx->height));
}

// Build the scatter pyramid for one profile: seed level 0 from the resolved
// skin diffuse, then halve to the top.
//
// Leaves BOTH textures reopened (BASE 0, MAX top) and the FBO unbound; the
// caller re-binds for the gather.
static void postfx_build_sss_pyramid(PostFX* fx, int profile_tag, float proj_scale,
                                     mat4 projection) {
    glBindFramebuffer(GL_FRAMEBUFFER, fx->sss_pyr_fbo);
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           fx->sss_pyr_color_texture, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                           fx->sss_pyr_depth_texture, 0);
    glDrawBuffers(2, bufs);
    glViewport(0, 0, fx->width, fx->height);
    glUseProgram(fx->sss_pyr_seed_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx->sss_diffuse_texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, fx->aux_texture);
    // The RESOLVED depth buffer, not the aux buffer's linear Z. Both describe the same
    // surface, but a depth resolve selects a sample where a colour resolve averages, and
    // an averaged depth is the difference between a petal's own blur radius and one taken
    // from a surface partway to the sky.
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, fx->depth_texture);
    uniform_set_int(fx->sss_pyr_seed_program->uniforms, "profileTag", profile_tag);
    uniform_set_mat4(fx->sss_pyr_seed_program->uniforms, "projection", (const float*)projection);
    draw_fullscreen_quad(fx->quad_vao);

    glUseProgram(fx->sss_pyr_down_program->id);
    float sigma_z = fx->sss_profiles[profile_tag - 1][3];
    for (int mip = 1; mip < fx->sss_pyr_mips; mip++) {
        int lw, lh, sw, sh;
        sss_pyr_level_size(fx, mip, &lw, &lh);
        sss_pyr_level_size(fx, mip - 1, &sw, &sh);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               fx->sss_pyr_color_texture, mip);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                               fx->sss_pyr_depth_texture, mip);
        glDrawBuffers(2, bufs);
        glViewport(0, 0, lw, lh);
        // Pin the source level on BOTH textures. GL 4.1 has no texture barrier,
        // so without this the level being written is also reachable for reading
        // and the result is undefined -- and undefined here means "works on the
        // GPU you tested". Pinning only the colour texture is the easy half-slip.
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, fx->sss_pyr_color_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, mip - 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mip - 1);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, fx->sss_pyr_depth_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, mip - 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mip - 1);

        uniform_set_vec2(fx->sss_pyr_down_program->uniforms, "texelSize",
                         (const float[]){1.0f / (float)sw, 1.0f / (float)sh});
        // World units one SOURCE texel spans per unit of view depth. A source
        // texel is 2^(mip-1) render texels, and a render texel subtends
        // 1/proj_scale per unit depth.
        uniform_set_float(fx->sss_pyr_down_program->uniforms, "srcFootprint",
                          proj_scale > 0.0f ? (float)(1 << (mip - 1)) / proj_scale : 0.0f);
        uniform_set_float(fx->sss_pyr_down_program->uniforms, "sigmaZFloor", sigma_z);
        draw_fullscreen_quad(fx->quad_vao);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx->sss_pyr_color_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, fx->sss_pyr_mips - 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, fx->sss_pyr_depth_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, fx->sss_pyr_mips - 1);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    check_gl_error("postfx sss pyramid build");
}

static bool postfx_ensure_sss_targets(PostFX* fx) {
    if (fx->sss_ready)
        return true;
    if (!create_color_fbo(fx->width, fx->height, GL_RGBA16F, &fx->sss_diffuse_fbo,
                          &fx->sss_diffuse_texture) ||
        !create_color_fbo(fx->width, fx->height, GL_RGBA16F, &fx->sss_delta_fbo,
                          &fx->sss_delta_texture) ||
        !create_pingpong(fx->width, fx->height, GL_RGBA16F, &fx->sss_history)) {
        log_error("Failed to allocate SSS targets");
        return false;
    }
    if (!create_sss_pyramid(fx))
        return false;
    fx->sss_ready = true;
    return true;
}

// Rebuild every resolution-dependent target at a new size / render scale,
// in place. Only GL handles and derived sizes move: the ~90 setting,
// per-frame-published, and borrowed-pointer fields on PostFX are untouched,
// which is why this is a teardown-and-rebuild rather than a destroy and
// re-create (a re-create would need a restore list of all of them, and would
// silently drop whichever one was added without updating it).
//
// The caller must have rebuilt the engine's MSAA scene target to the same
// render size FIRST: every G-buffer resolve in postfx_run is a multisample
// blit, which requires identical source and destination rects.
bool postfx_resize(PostFX* fx, int width, int height, int ss_scale, float render_scale) {
    if (!fx)
        return false;
    if (width <= 0 || height <= 0) {
        log_error("postfx_resize: invalid size %dx%d", width, height);
        return false;
    }
    if (ss_scale < 1)
        ss_scale = 1;
    render_scale = postfx_clamp_render_scale(render_scale);

    postfx_invalidate_targets(fx);
    postfx_derive_sizes(fx, width, height, ss_scale, render_scale);

    // Compile BEFORE allocating: a failure here must not be able to leave a
    // live canvas (which postfx_taau_active keys on) beside a NULL resolve
    // program, which the seam would then dereference.
    bool ok = postfx_ensure_taau_program(fx);
    if (ok)
        ok = postfx_alloc_targets(fx);
    if (!ok) {
        // Leave nothing half-built. "Unusable until a later size succeeds" is
        // then true of the data, not merely of the one caller that happens to
        // latch a flag on the way out.
        log_error("postfx_resize: rebuild failed at %dx%d", fx->width, fx->height);
        postfx_invalidate_targets(fx);
        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    postfx_seed_size_uniforms(fx);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    check_gl_error("postfx_resize");
    return true;
}

void postfx_reset_sss_profiles(PostFX* fx) {
    if (fx)
        fx->sss_profile_count = 0;
}

int postfx_add_sss_profile(PostFX* fx, const float* color, float radius) {
    if (!fx || fx->sss_profile_count >= MAX_SSS_PROFILES)
        return -1;
    int slot = fx->sss_profile_count++;
    glm_vec4_copy((vec4){color[0], color[1], color[2], radius}, fx->sss_profiles[slot]);
    return slot;
}

void postfx_set_exposure(PostFX* fx, Exposure* exposure) {
    if (!fx)
        return;
    fx->exposure = exposure;
}

void postfx_set_profiler(PostFX* fx, struct Profiler* profiler) {
    if (!fx)
        return;
    fx->profiler = profiler;
}

static GLuint run_temporal_accum(PostFX* fx, ShaderProgram* prog, PingPong* pp, int w, int h,
                                 GLuint current_tex, float feedback);

// Whatever a consumer feeds the accumulator settles only if the input settles.
// Most of them converge on their own -- their per-frame change is the camera
// moving, which stops when the camera does -- so a ~10-frame window is all they
// need and this is the value every one of them has always run at.
#define TEMPORAL_FEEDBACK_DEFAULT 0.9f
// GTAO is the exception: it re-randomises its slice set every frame on purpose,
// so its input never settles and the blend keeps a (1 - feedback) share of each
// new estimate indefinitely. That share IS the flicker, and the only way to
// shrink it is a longer window. ~33 frames, chosen as the point where the
// residual stops being visible without the response becoming sluggish enough to
// smear AO behind a moving camera.
#define TEMPORAL_FEEDBACK_AO 0.97f
// SSR joined the same class in 10.7.2: its ray jitter reseeds every frame
// while TAA resolves, so like GTAO its input never settles and the residual
// scales with (1 - feedback). Measured at a close-up over the catcher floor
// reflecting a bright sheen rim: 0.9 left the reflection band visibly
// boiling; the AO-length window shrinks the per-frame share 3.3x.
#define TEMPORAL_FEEDBACK_SSR 0.97f

// Additive-fold the currently-bound fullscreen setup into the canvas
// (GL_ONE,GL_ONE), restoring blend state.
static void _sss_fold_into_canvas(PostFX* fx, GLuint canvas_fbo) {
    glBindFramebuffer(GL_FRAMEBUFFER, canvas_fbo);
    glViewport(0, 0, fx->post_width, fx->post_height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    draw_fullscreen_quad(fx->quad_vao);
    glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    check_gl_error("postfx sss");
}

// Screen-space SSS through the scatter pyramid: the skin-diffuse buffer (D,
// attachment 4, already resolved) is built into a mip chain per profile, then
// gathered with one trilinear tap per profile Gaussian per channel, forming the
// recomposite delta blur - D that is additive-blended into the canvas. Diffuse
// softens; FragColor's specular is untouched. Under TAA the delta is temporally
// accumulated first (its own history, like fog/SSR). Pyramid, delta and history
// stay at render res; only the fold magnifies.
static void postfx_run_sss(PostFX* fx, GLuint canvas_fbo, mat4 projection, bool taa_resolving) {
    // World radius -> screen pixels: a world unit at view depth d spans
    // 0.5 * proj[1][1] * height / d pixels.
    const float proj_scale = 0.5f * projection[1][1] * (float)fx->height;
    const int count = fx->sss_profile_count > 0 ? fx->sss_profile_count : 1;

    // Each profile gets its own walk of the one pyramid. An array texture would
    // save the repeat but scales memory with MAX_SSS_PROFILES and puts a
    // per-(level, layer) attach on the least-travelled GL 4.1 path in this tree;
    // every shipping scene has one profile and the fixture has two.
    for (int p = 0; p < count; p++) {
        postfx_build_sss_pyramid(fx, p + 1, proj_scale, projection);

        // Accumulate into the delta target: profile p's gather emits exactly zero
        // off its own tag, so the profiles sum without overlapping.
        glBindFramebuffer(GL_FRAMEBUFFER, fx->sss_delta_fbo);
        glViewport(0, 0, fx->width, fx->height);
        if (p == 0) {
            glClearBufferfv(GL_COLOR, 0, (const GLfloat[]){0.0f, 0.0f, 0.0f, 0.0f});
        } else {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
        }
        glUseProgram(fx->sss_gather_program->id);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, fx->sss_pyr_color_texture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, fx->sss_diffuse_texture);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, fx->aux_texture);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, fx->depth_texture);
        uniform_set_vec4(fx->sss_gather_program->uniforms, "sssProfile", fx->sss_profiles[p]);
        uniform_set_int(fx->sss_gather_program->uniforms, "profileTag", p + 1);
        uniform_set_mat4(fx->sss_gather_program->uniforms, "projection", (const float*)projection);
        uniform_set_float(fx->sss_gather_program->uniforms, "projScale", proj_scale);
        uniform_set_float(fx->sss_gather_program->uniforms, "maxLod", sss_lod_cap(fx));
        uniform_set_vec2(fx->sss_gather_program->uniforms, "renderTexel",
                         (const float[]){1.0f / (float)fx->width, 1.0f / (float)fx->height});
        draw_fullscreen_quad(fx->quad_vao);
        if (p > 0) {
            glDisable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
    }

    // The tent with a zero texel size is the documented exact-copy form, so the
    // fold needs no shader of its own.
    GLuint delta = fx->sss_delta_texture;
    if (taa_resolving) {
        delta = run_temporal_accum(fx, fx->temporal_accum_program, &fx->sss_history, fx->width,
                                   fx->height, fx->sss_delta_texture, TEMPORAL_FEEDBACK_DEFAULT);
    } else {
        fx->sss_history.valid = false;
    }

    glUseProgram(fx->upsample_tent_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, delta);
    uniform_set_vec2(fx->upsample_tent_program->uniforms, "texelSize",
                     (const float[]){0.0f, 0.0f});
    _sss_fold_into_canvas(fx, canvas_fbo);
}

// Bloom pyramid (Jimenez dual-filter): bright pass into mip 0, 13-tap
// downsample chain, additive tent upsample back up, all on one packed-float
// mip texture via FBO re-attach + BASE/MAX_LEVEL pinning (the hi-z idiom;
// GL 4.1 has no texture barrier). Leaves the full chain reopened so the
// tonemap's magnified read hits level 0. Level dims come from bit-shifts
// everywhere: the mip-count policy at allocation stops at >= 8 px per
// axis, so the shifts never degenerate and viewport always agrees with
// texelSize.
// Bin the frame's log2 luminances and collapse them to the percentile-clipped
// mean, returned in log2. GPU only: what happens to the number is exposure.c's.
//
// Its own function for the reason every other multi-draw sequence in this file
// has one -- postfx_run_bloom, _dof, _ssr, _sss, _oit, _atmosphere,
// _motion_blur, _flare. This grew from one draw plus a mipmap into three draws
// into three framebuffers while sitting inline, which is how it ended up the
// only multi-FBO chain in postfx_run that had not made that move.
/*
 * The three draws, and nothing else. Split from the readback below because the
 * two have different consumers: auto-exposure needs the VALUE on the CPU, and
 * the Purkinje shift needs only the 1x1 TEXTURE, which it samples on unit 7.
 *
 * That split is what lets a rod model work under a pinned exposure. The
 * readback is gated on `automatic` exactly as it always was; these draws are
 * not, so a scene that pins its exposure -- which is every fixture in the
 * corpus -- still has a metered frame luminance for anything that wants one.
 * Cheap on its own: the 5.253 ms the comment below quotes is the blocking read
 * draining the pipeline, not this.
 */
static void postfx_measure_luminance(PostFX* fx, GLuint scene_tex) {
    glBindFramebuffer(GL_FRAMEBUFFER, fx->lum_fbo);
    glViewport(0, 0, LUM_MEASURE_SIZE, LUM_MEASURE_SIZE);
    glUseProgram(fx->lum_measure_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_tex);
    draw_fullscreen_quad(fx->quad_vao);

    // Bin, then collapse.
    //
    // The bin pass is the expensive half of this block and it is not the
    // fetch count that makes it so. LUM_HISTOGRAM_ROWS exists because 64
    // fragments is around 0.4% occupancy on this GPU: measured standing
    // alone the draw costs ~0.35 ms, while 10000 of them back to back
    // cost 1.19 us each -- a 300x gap, which is what latency rather than
    // work looks like. Splitting the source rows across output rows cuts
    // the serial depth from 4160 to about 1024. Count and sum are
    // associative, so the reduce summing R rows per bin is the same
    // statistic.
    glBindFramebuffer(GL_FRAMEBUFFER, fx->lum_hist_fbo);
    glViewport(0, 0, LUM_HISTOGRAM_BINS, LUM_HISTOGRAM_ROWS);
    glUseProgram(fx->lum_histogram_program->id);
    glBindTexture(GL_TEXTURE_2D, fx->lum_texture);
    uniform_set_int(fx->lum_histogram_program->uniforms, "srcSize", LUM_MEASURE_SIZE);
    uniform_set_int(fx->lum_histogram_program->uniforms, "binCount", LUM_HISTOGRAM_BINS);
    uniform_set_int(fx->lum_histogram_program->uniforms, "rowCount", LUM_HISTOGRAM_ROWS);
    uniform_set_vec2(fx->lum_histogram_program->uniforms, "binRange",
                     (vec2){LUM_HISTOGRAM_MIN_LOG2, LUM_HISTOGRAM_MAX_LOG2});
    uniform_set_int(fx->lum_histogram_program->uniforms, "meterMode",
                    (int)fx->exposure->meter_mode);
    uniform_set_float(fx->lum_histogram_program->uniforms, "meterRadius",
                      fx->exposure->meter_radius);
    draw_fullscreen_quad(fx->quad_vao);

    glBindFramebuffer(GL_FRAMEBUFFER, fx->lum_reduce_fbo);
    glViewport(0, 0, 1, 1);
    glUseProgram(fx->lum_reduce_program->id);
    glBindTexture(GL_TEXTURE_2D, fx->lum_hist_texture);
    uniform_set_int(fx->lum_reduce_program->uniforms, "binCount", LUM_HISTOGRAM_BINS);
    uniform_set_int(fx->lum_reduce_program->uniforms, "rowCount", LUM_HISTOGRAM_ROWS);
    uniform_set_vec2(fx->lum_reduce_program->uniforms, "percentiles",
                     (vec2){fx->exposure->meter_low, fx->exposure->meter_high});
    draw_fullscreen_quad(fx->quad_vao);
    check_gl_error("postfx luminance measure");
}

// The read BLOCKS. A PBO plus a fence would not (measured 0.033 ms against
// 5.253), but it lands each measurement whenever the GPU happens to finish,
// which makes adaptation depend on frame timing -- and equal headless runs then
// stop matching. This engine trades that the other way round everywhere else,
// so it does here too. It is also why this is its own function: the cost lives
// here, and the consumer that does not need the value does not pay it.
static float postfx_read_luminance(PostFX* fx) {
    float measured = 0.0f;
    glBindTexture(GL_TEXTURE_2D, fx->lum_reduce_texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, &measured);
    check_gl_error("postfx auto exposure");
    return measured;
}

static void postfx_run_bloom(PostFX* fx, GLuint scene_tex) {
    // Bright pass into pyramid level 0 (linear sampling downsamples)
    glBindFramebuffer(GL_FRAMEBUFFER, fx->bloom_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fx->bloom_texture,
                           0);
    glViewport(0, 0, fx->bloom_width, fx->bloom_height);
    glUseProgram(fx->bloom_bright_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_tex);
    uniform_set_float(fx->bloom_bright_program->uniforms, "threshold", fx->bloom_threshold);
    uniform_set_float(fx->bloom_bright_program->uniforms, "knee", fx->bloom_knee);
    uniform_set_float(fx->bloom_bright_program->uniforms, "maxBrightness",
                      fx->bloom_max_brightness);
    draw_fullscreen_quad(fx->quad_vao);

    // Downsample the chain: each level reads a 13-tap filter of the level
    // above it, with BASE/MAX_LEVEL pinning the sampled set to the source
    glUseProgram(fx->bloom_down_program->id);
    glBindTexture(GL_TEXTURE_2D, fx->bloom_texture);
    for (int mip = 1; mip < fx->bloom_mips; mip++) {
        int sw = fx->bloom_width >> (mip - 1);
        int sh = fx->bloom_height >> (mip - 1);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               fx->bloom_texture, mip);
        glViewport(0, 0, fx->bloom_width >> mip, fx->bloom_height >> mip);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, mip - 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mip - 1);
        uniform_set_vec2(fx->bloom_down_program->uniforms, "texelSize",
                         (vec2){1.0f / (float)sw, 1.0f / (float)sh});
        draw_fullscreen_quad(fx->quad_vao);
    }

    // Upsample back: tent-filter each coarser level additively onto the
    // finer one, so level 0 ends as the sum of progressively wider blurs
    // -- one smooth wide kernel instead of ringed bands.
    glUseProgram(fx->bloom_up_program->id);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    for (int mip = fx->bloom_mips - 2; mip >= 0; mip--) {
        int sw = fx->bloom_width >> (mip + 1);
        int sh = fx->bloom_height >> (mip + 1);
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
// Lens flare target, allocated on first use like the DoF and fog chains: a
// scene that never enables it never pays the memory.
//
// Quarter of the OUTPUT resolution -- the size the tonemap composites at.
// Ghosts are defocused, so resolution buys nothing. Dropped by
// postfx_invalidate_targets on any rebuild; note render_scale does not move
// out_*, so that drop is broader than this target strictly needs.
static bool postfx_ensure_flare(PostFX* fx) {
    if (fx->flare_ready)
        return true;
    fx->flare_width = fx->out_width > 4 ? fx->out_width / 4 : 1;
    fx->flare_height = fx->out_height > 4 ? fx->out_height / 4 : 1;
    if (!create_color_fbo(fx->flare_width, fx->flare_height, GL_R11F_G11F_B10F, &fx->flare_fbo,
                          &fx->flare_texture)) {
        log_error("PostFX: lens flare target incomplete");
        // Latch off rather than retry: this runs every frame, and a driver
        // refusing the target would otherwise leak an FBO and a texture per
        // frame. Same shape as the froxel volume and the DoF chain.
        fx->flare_enabled = false;
        return false;
    }
    fx->flare_ready = true;
    return true;
}

// Ghosts from the finished bloom pyramid; see lens_flare_frag.glsl for why the
// source is the pyramid rather than a private bright pass. Returns whether it
// actually wrote the target, which is what the composite needs to know --
// flare_ready only says a target exists.
static bool postfx_run_flare(PostFX* fx) {
    if (!postfx_ensure_flare(fx))
        return false;
    // Below the allocation gate: the call site cannot see it, and it captures
    // this function's own return value only after a call-site scope would
    // already have filed the row.
    profiler_scope_begin(fx->profiler, "lens flare");
    glBindFramebuffer(GL_FRAMEBUFFER, fx->flare_fbo);
    glViewport(0, 0, fx->flare_width, fx->flare_height);
    glUseProgram(fx->flare_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx->bloom_texture);
    UniformManager* u = fx->flare_program->uniforms;
    uniform_set_int(u, "bloomTex", 0);
    uniform_set_int(u, "ghostCount", fx->flare_ghosts);
    uniform_set_float(u, "ghostSpacing", fx->flare_ghost_spacing);
    uniform_set_float(u, "haloWidth", fx->flare_halo_width);
    uniform_set_float(u, "chroma", fx->flare_chroma);
    uniform_set_vec2(u, "texelSize",
                     (vec2){1.0f / (float)fx->out_width, 1.0f / (float)fx->out_height});
    // Clamped to what the pyramid actually has: the mip count follows the
    // resolution, so an authored level valid at 1080p silently pins to the top
    // level in a small window and the flare softens without saying why.
    const int top_mip = fx->bloom_mips > 0 ? fx->bloom_mips - 1 : 0;
    uniform_set_float(u, "sourceLod",
                      (float)(fx->flare_source_lod < top_mip ? fx->flare_source_lod : top_mip));
    draw_fullscreen_quad(fx->quad_vao);
    check_gl_error("postfx lens flare");
    profiler_scope_end(fx->profiler);
    return true;
}


// Unit-radius aperture points: a Vogel spiral (uniform disk coverage),
// optionally warped so each angular wedge maps onto a regular N-gon's wedge
// (radial scale cos(seg/2)/cos(a): radially linear, so each direction keeps
// its area-uniform radial distribution; the residual ANGULAR density
// variation is bounded by 1/cos^2(pi/N) and invisible at 64 taps). Pure
// libm on fixed inputs: no RNG, identical across runs and builds, which is
// what keeps DoF goldens deterministic. The rotation is applied here, once,
// CPU-side -- never per pixel, which would grind the polygon shape into
// grainy noise.
static void dof_build_kernel(int blades, float rotation_deg, float k[DOF_TAPS][2]) {
    const float ga = 2.399963229728653f; // golden angle
    float rot = rotation_deg * GLM_PIf / 180.0f;
    for (int i = 0; i < DOF_TAPS; i++) {
        float r = sqrtf(((float)i + 0.5f) / (float)DOF_TAPS);
        float th = (float)i * ga;
        if (blades >= 3) {
            float seg = 2.0f * GLM_PIf / (float)blades;
            float a = fmodf(th, seg) - 0.5f * seg;
            r *= cosf(0.5f * seg) / cosf(a);
        }
        th += rot;
        k[i][0] = r * cosf(th);
        k[i][1] = r * sinf(th);
    }
}

// Depth of field: signed CoC + gather at half render res, composite at post
// res into fx->dof_texture. Callers must have ensured the targets exist and
// read fx->dof_texture as the scene afterward. canvas_tex is the post-seam
// scene color the CoC pass and the composite read.
static void postfx_run_dof(PostFX* fx, GLuint canvas_tex, mat4 projection) {
    const float dof_texel[2] = {1.0f / (float)fx->half_width, 1.0f / (float)fx->half_height};

    // Pass 1: signed CoC + half-res scene colour
    glBindFramebuffer(GL_FRAMEBUFFER, fx->dof_coc_fbo);
    glViewport(0, 0, fx->half_width, fx->half_height);
    glUseProgram(fx->dof_coc_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, canvas_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, fx->depth_texture);
    uniform_set_mat4(fx->dof_coc_program->uniforms, "projection", (float*)projection);
    uniform_set_float(fx->dof_coc_program->uniforms, "focusDistance", fx->dof_focus_distance);
    uniform_set_float(fx->dof_coc_program->uniforms, "focusRange", fx->dof_focus_range);
    uniform_set_float(fx->dof_coc_program->uniforms, "maxCoC", fx->dof_max_coc);
    draw_fullscreen_quad(fx->quad_vao);

    // Pass 2: per-tile CoC maxima (far in .r, near in .g).
    glBindFramebuffer(GL_FRAMEBUFFER, fx->dof_tile_fbo);
    glViewport(0, 0, fx->dof_tile_w, fx->dof_tile_h);
    glUseProgram(fx->dof_tile_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx->dof_coc_texture);
    uniform_set_vec2(fx->dof_tile_program->uniforms, "texelSize", dof_texel);
    draw_fullscreen_quad(fx->quad_vao);

    // Pass 3: dilate the maxima so a defocused tile's reach covers every tile
    // its blur can spill into. K from maxCoC: a blur of C half-res texels
    // spills at most ceil(C / DOF_TILE) tiles.
    int dilate = (int)ceilf(fx->dof_max_coc / (float)DOF_TILE);
    dilate = dilate < 1 ? 1 : (dilate > 8 ? 8 : dilate);
    glBindFramebuffer(GL_FRAMEBUFFER, fx->dof_dilate_fbo);
    glUseProgram(fx->dof_dilate_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx->dof_tile_texture);
    const float tile_texel[2] = {1.0f / (float)fx->dof_tile_w, 1.0f / (float)fx->dof_tile_h};
    uniform_set_vec2(fx->dof_dilate_program->uniforms, "tileTexel", tile_texel);
    uniform_set_int(fx->dof_dilate_program->uniforms, "dilateRadius", dilate);
    draw_fullscreen_quad(fx->quad_vao);

    // Pass 4: aperture gather (half res), MRT to the near+far fields. The
    // kernel is rebuilt per frame -- 64 sincos is nothing, and it keeps the
    // GUI blade/rotation sliders live with no dirty tracking.
    glBindFramebuffer(GL_FRAMEBUFFER, fx->dof_gather_fbo);
    glViewport(0, 0, fx->half_width, fx->half_height);
    glUseProgram(fx->dof_gather_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx->dof_coc_texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, fx->dof_dilate_texture);
    uniform_set_vec2(fx->dof_gather_program->uniforms, "texelSize", dof_texel);
    float kernel[DOF_TAPS][2];
    dof_build_kernel(fx->dof_blades, fx->dof_rotation, kernel);
    // Ranged upload of the contiguous array (the SSS-profile idiom).
    GLint kloc = uniform_location(fx->dof_gather_program->uniforms, "kernel[0]");
    if (kloc >= 0)
        glUniform2fv(kloc, DOF_TAPS, (const GLfloat*)kernel);
    draw_fullscreen_quad(fx->quad_vao);

    // Pass 5: composite (post res) -- far under sharp, near alpha-over both
    glBindFramebuffer(GL_FRAMEBUFFER, fx->dof_fbo);
    glViewport(0, 0, fx->post_width, fx->post_height);
    glUseProgram(fx->dof_composite_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, canvas_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, fx->dof_near_texture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, fx->dof_far_texture);
    glActiveTexture(GL_TEXTURE3);
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

    // Every resolution-dependent target, including the lazily-allocated groups
    // (all 0 and no-op if never allocated -- this doubles as create_postfx's
    // error-unwind over a half-built PostFX).
    postfx_free_targets(fx);
    // The ones the resize deliberately keeps, and so are only freed here.
    gl_delete_texture(&fx->noise_texture);
    gl_delete_texture(&fx->white_tex);
    gl_delete_texture(&fx->lut_texture);
    gl_delete_fbo(&fx->lum_fbo);
    gl_delete_texture(&fx->lum_texture);
    gl_delete_fbo(&fx->lum_hist_fbo);
    gl_delete_texture(&fx->lum_hist_texture);
    gl_delete_fbo(&fx->lum_reduce_fbo);
    gl_delete_texture(&fx->lum_reduce_texture);
    gl_delete_fbo(&fx->froxel_fbo);
    glDeleteTextures(2, fx->froxel_scatter);
    fx->froxel_scatter[0] = 0;
    fx->froxel_scatter[1] = 0;
    gl_delete_texture(&fx->froxel_integrated);

    free_program(fx->bloom_bright_program);
    free_program(fx->bloom_down_program);
    free_program(fx->bloom_up_program);
    free_program(fx->flare_program);
    free_program(fx->tonemap_program);
    free_program(fx->spec_occ_composite_program);
    free_program(fx->gtao_program);
    free_program(fx->ssao_blur_program);
    free_program(fx->temporal_accum_program);
    free_program(fx->ssgi_composite_program);
    free_program(fx->ssgi_accum_program);
    free_program(fx->ssr_accum_program);
    free_program(fx->ssgi_atrous_program);
    free_program(fx->ssr_atrous_program);
    free_program(fx->lum_measure_program);
    free_program(fx->lum_histogram_program);
    free_program(fx->lum_reduce_program);
    free_program(fx->ssr_program);
    free_program(fx->ssr_hiz_program);
    free_program(fx->upsample_tent_program);
    free_program(fx->froxel_inject_program);
    free_program(fx->froxel_integrate_program);
    free_program(fx->froxel_composite_program);
    free_program(fx->taa_resolve_program);
    free_program(fx->taau_resolve_program);
    free_program(fx->dof_coc_program);
    free_program(fx->dof_tile_program);
    free_program(fx->dof_dilate_program);
    free_program(fx->dof_gather_program);
    free_program(fx->dof_composite_program);
    free_program(fx->motion_blur_program);
    free_program(fx->motion_blur_tilemax_program);
    free_program(fx->motion_blur_neighbormax_program);
    free_program(fx->sss_gather_program);
    free_program(fx->sss_pyr_seed_program);
    free_program(fx->sss_pyr_down_program);
    free_program(fx->contact_shadow_program);
    free_program(fx->oit_resolve_program);

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

bool postfx_load_lut(PostFX* fx, const char* path) {
    if (!fx || !path || !path[0])
        return false;

    ColorLut lut;
    memset(&lut, 0, sizeof(lut));
    if (!lut_load_cube(path, &lut))
        return false; // lut_load_cube named the reason; keep whatever was loaded

    int size = lut.size; // lut_free zeroes the struct, so read it first
    // 16F, and the evidence for it had to be taken twice. The first experiment
    // compared 16F against 32F on an IDENTITY table -- 17^3, so every lattice
    // value is k/16 and exactly representable in fp16. Storage error there is
    // structurally zero, so the test could not have detected storage error, and
    // it was then generalised into a decision for all tables.
    //
    // Re-measured on a table with a real three-way term, where fp16 rounding is
    // live: it costs **0.004 of an 8-bit code** against fp64. The decision
    // stands; the reasoning is now the measurement rather than a coincidence.
    // What DOES decide an exact identity is the interpolant's weights, not the
    // bytes -- tetrahedral computes them in the shader, trilinear takes them
    // from the texture unit's fixed-point subtexel fraction.
    GLuint tex = create_texture_3d_float(size, size, size, GL_RGB16F, GL_RGB, lut.data);
    // The CPU copy has done its job the moment the driver has the bytes -- the
    // same shape sky_clouds.c uses for its baked noise volumes.
    lut_free(&lut);
    if (!tex) {
        log_warn("lut: '%s' parsed but could not be uploaded", path);
        return false;
    }

    gl_delete_texture(&fx->lut_texture);
    fx->lut_texture = tex;
    fx->lut_size = size;

    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(fx->lut_name, sizeof(fx->lut_name), "%s", base);
    return true;
}

void postfx_clear_lut(PostFX* fx) {
    if (!fx)
        return;
    gl_delete_texture(&fx->lut_texture);
    fx->lut_size = 0;
    fx->lut_name[0] = '\0';
}

float postfx_sss_max_sigma_per_depth(const PostFX* fx, const mat4 projection) {
    if (!fx)
        return 0.0f;
    float proj_scale = 0.5f * projection[1][1] * (float)fx->height;
    if (proj_scale <= 0.0f)
        return 0.0f;
    // Deliberately NOT gated on the pyramid being allocated. The targets are
    // allocated lazily inside postfx_run, which is AFTER render.c has uploaded
    // this frame's value -- so gating here reported a zero ceiling on the first
    // skin frame and on the frame after every resize, which reads as
    // pre-integration snapping to full strength for one frame. The cap depends
    // only on the render height and the projection, both known before any
    // allocation, so it does not need the guard.
    return sss_level_sigma_px(sss_lod_cap(fx)) / proj_scale;
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

bool postfx_taau_active(const PostFX* fx) {
    // Keyed on the canvas rather than on render_scale: the canvas is what the
    // seam actually dispatches on, and it exists only when create_postfx both
    // wanted AND managed to build the reduced-scale chain.
    return fx && fx->post_fbo != 0;
}

bool postfx_has_medium(const PostFX* fx) {
    // froxel_failed is sticky and part of the answer rather than a separate test at the
    // gate: clearing fog_enabled used to stop the pass after an incomplete FBO, and it
    // cannot any more, because two of the three arming sources are republished every
    // frame. Without this the pass re-arms, re-fails and re-logs twice a frame forever.
    return fx && !fx->froxel_failed &&
           (fx->fog_enabled || fx->water_medium != 0 || fx->local_fog_count > 0);
}

bool postfx_wants_aux_gbuffer(const PostFX* fx) {
    // Attachment 2 carries motion vectors (.xy, for TAA reprojection) and linear
    // view-Z (.z, for GTAO position reconstruction). Produce it whenever either
    // consumer is active -- decoupled from TAA so GTAO gets linear depth even
    // with TAA off (e.g. headless). SSGI rides the GTAO sweep, so it needs the
    // linear depth too even if AO display is off (--ssgi --no-ssao). The fog
    // march reconstructs its ray endpoints from the linear Z as well.
    // Motion blur consumes the .xy velocity, so it too forces the aux buffer
    // (otherwise it would silently no-op under, e.g., --no-ssao). Contact
    // shadows reconstruct positions from the linear Z, same as GTAO.
    // Aerial perspective indexes its volume by the same linear Z, so it forces
    // the buffer for the same reason fog does. Keyed on the published volume
    // rather than a toggle: with no sky there is nothing to composite.
    return fx && (fx->taa_enabled || fx->ssao_enabled || fx->ssgi_enabled ||
                  postfx_has_medium(fx) || fx->motion_blur_enabled ||
                  fx->contact_shadows_enabled || fx->aerial_volume != 0);
}

bool postfx_wants_albedo(const PostFX* fx) {
    // Attachment 3 (base color) is needed by SSGI's indirect-diffuse composite
    // and by the albedo debug view -- without the latter, --albedo-debug alone
    // was silently suppressed and showed the normal render instead.
    return fx && (fx->ssgi_enabled || fx->debug_view == POSTFX_DEBUG_ALBEDO);
}

bool postfx_wants_spec_split(const PostFX* fx) {
    // Not gated on ssao_enabled: with AO off the composite still owes the
    // scene its ambient specular (it folds back with occlusion 1). Gated on
    // the tonemap mode: a passthrough frame blits hdr and never runs the
    // composite, so splitting would silently discard the specular -- keep it
    // inline there instead.
    return fx && fx->spec_occlusion_mode == POSTFX_SPEC_OCC_SPLIT &&
           fx->tonemap_mode != POSTFX_TONEMAP_PASSTHROUGH;
}

bool postfx_ssr_active(const PostFX* fx, bool normals_written) {
    // The single "SSR runs this frame" predicate: the effect is enabled and
    // the normals buffer it marches against was actually produced. Both the
    // postfx pass and the shadow catcher's floor marker derive from this so
    // they cannot disagree about whether the floor is being reflected.
    return fx && fx->ssr_enabled && normals_written;
}

bool postfx_contact_shadows_have_light(const PostFX* fx) {
    return fx && (fx->fog_light_count > 0 || fx->cs_mapless_lights > 0);
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

// Reproject-and-blend temporal accumulation, shared by seven consumers across
// three programs: AO, SSGI, SSR, SSS, TAA, contact shadows, and the composited
// fog layer. Indexes the pair by frame parity, resets when the history is not
// valid (first use, or the accumulator was skipped last frame), and returns the
// freshly written texture.
//
// Owns everything that depends on the resolution or the unit layout, so a new
// consumer cannot get it wrong: the sampler units, and texelSize. Both were
// previously split across create_postfx and the call sites, and the split is
// what let a half-res texelSize reach the full-res consumers -- their
// neighborhood clamp ran at +/-2 texels instead of +/-1 until this owned it.
// A complete set of call-site uploads would be equally correct; the difference
// is that this is a one-site invariant the next consumer cannot break, rather
// than an N-site one they can.
//
// RESTORES NOTHING. On return the ping-pong FBO is still bound, the viewport is
// at (w,h), prog is current, and texture unit 2 is active. Every caller that
// draws afterwards has to re-bind its own target, viewport, program and unit.
static GLuint run_temporal_accum(PostFX* fx, ShaderProgram* prog, PingPong* pp, int w, int h,
                                 GLuint current_tex, float feedback) {
    int write = fx->frame_index & 1;
    int read = write ^ 1;
    glBindFramebuffer(GL_FRAMEBUFFER, pp->fbo[write]);
    glViewport(0, 0, w, h);
    glUseProgram(prog->id);
    const float texel[2] = {1.0f / (float)w, 1.0f / (float)h};
    uniform_set_vec2(prog->uniforms, "texelSize", texel);
    // Consumed by temporal_accum_frag and ssr_accum_frag (its at-rest
    // weight); a no-op against the programs that hard-code their feedback
    // (taa_resolve, ssgi_accum).
    uniform_set_float(prog->uniforms, "feedback", feedback);
    uniform_set_int(prog->uniforms, "currentTex", 0);
    uniform_set_int(prog->uniforms, "velocityTex", 1);
    uniform_set_int(prog->uniforms, "historyTex", 2);
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

// TAAU resolve: reconstruct the jittered render-res frame at post res and
// blend it with the post-res history (taau_resolve_frag has the math). A
// dedicated driver rather than a consumer of run_temporal_accum: that
// helper's one-site invariant is same-res in and out -- it seeds texelSize
// from a single resolution -- while this pass straddles two and adds the
// jitter uniform. Unit layout follows the shared convention (0 current,
// 1 velocity, 2 history), seeded once at create like every other
// single-consumer program. Same restore contract as run_temporal_accum:
// RESTORES NOTHING. The caller reads the written side out of taa_history.
//
// The two resolutions stay per-draw rather than joining the create-time
// seed, which is what makes them free under postfx_resize: a stale size here
// would mis-register every sample rather than fail loudly.
static void run_taau_resolve(PostFX* fx) {
    PingPong* pp = &fx->taa_history;
    int write = fx->frame_index & 1;
    int read = write ^ 1;
    glBindFramebuffer(GL_FRAMEBUFFER, pp->fbo[write]);
    glViewport(0, 0, fx->post_width, fx->post_height);
    glUseProgram(fx->taau_resolve_program->id);
    UniformManager* u = fx->taau_resolve_program->uniforms;
    uniform_set_vec2(u, "renderSize", (vec2){(float)fx->width, (float)fx->height});
    uniform_set_vec2(u, "postSize", (vec2){(float)fx->post_width, (float)fx->post_height});
    uniform_set_vec2(u, "jitterPx", fx->taau_jitter_px);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx->hdr_texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, fx->aux_texture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, pp->tex[read]);
    uniform_set_int(u, "reset", pp->valid ? 0 : 1);
    draw_fullscreen_quad(fx->quad_vao);
    pp->valid = true;
}

// Edge-aware a-trous denoise, shared by the SSGI and SSR denoisers (their
// a-trous shaders share the reflTex/giTex + linDepthTex + normalsTex unit layout
// and the stepSize/texelSize/useNormalsTex uniforms). Three B3-spline passes at
// doubling tap spacing (1, 2, 4) smooth input_tex across the ping-pong pp,
// weighted by aux depth (unit 1) and, when present, view normals (unit 2).
// texelSize is set per call (the SSR resolution changes with the full-res
// toggle; for SSGI it reproduces the fixed ao_texel value bit-for-bit). Returns
// the final smoothed texture.
static GLuint run_atrous(PostFX* fx, ShaderProgram* prog, PingPong* pp, int w, int h,
                         GLuint input_tex, bool have_normals) {
    glUseProgram(prog->id);
    uniform_set_int(prog->uniforms, "useNormalsTex", have_normals ? 1 : 0);
    const float texel[2] = {1.0f / (float)w, 1.0f / (float)h};
    uniform_set_vec2(prog->uniforms, "texelSize", texel);
    glViewport(0, 0, w, h);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, fx->aux_texture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, have_normals ? fx->normal_texture : 0);
    for (int i = 0; i < 3; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, pp->fbo[i & 1]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, input_tex);
        uniform_set_int(prog->uniforms, "stepSize", 1 << i);
        draw_fullscreen_quad(fx->quad_vao);
        input_tex = pp->tex[i & 1];
    }
    return input_tex;
}

// The fog medium plus the caster/spot block shadow_publish_to_postfx flattened,
// under uniform names both fog shaders declare identically. One edit site when a
// fog parameter is added -- uniform_set_* no-ops on an absent name, so a rename
// that reaches only one shader would otherwise render silently wrong.
// The caller has the program current and has bound the CSM array (unit 1) and
// the spot depth map (unit 2).
// The volume's front face. Defaults to the camera's near plane, which puts the
// most slices closest to the viewer -- the distribution the AC4 paper settled on
// deliberately, because that is where precision is needed and where aliasing
// shows first for a camera sitting inside the medium.
//
// The cost is that an exponential mapping gives equal depth RATIOS equal slices,
// so a small near spends much of the volume on air near the lens. A scene whose
// medium is all far away sets fog_near itself to buy those slices back;
// fog_depth_dist biases the curve without moving either end.
float postfx_fog_near(const PostFX* fx, mat4 projection) {
    if (fx->fog_near > 0.0f)
        return fx->fog_near;
    float cam_near = projection[3][2] / (projection[2][2] - 1.0f);
    return cam_near > 0.0f ? cam_near : 0.1f;
}

static void upload_fog_uniforms(PostFX* fx, UniformManager* u, mat4 projection, mat4 inv_view) {
    uniform_set_mat4(u, "projection", (float*)projection);
    uniform_set_mat4(u, "invView", (float*)inv_view);
    uniform_set_float(u, "fogNear", postfx_fog_near(fx, projection));
    uniform_set_float(u, "fogFar", fx->fog_far);
    uniform_set_float(u, "fogDepthDist", fx->fog_depth_dist);
    uniform_set_vec3(u, "ambientColor", fx->fog_ambient);
    // Zero when the app has not asked for global fog, because the pass now runs for media
    // the app did NOT ask for -- a submerged camera, an authored volume. fog_enabled means
    // "the global height medium exists"; the gate above means "some medium does". Before
    // there was only one medium the two were the same statement, and uploading the density
    // unconditionally would now put the app's height fog in every frame that has a box in
    // it.
    uniform_set_float(u, "density", fx->fog_enabled ? fx->fog_density : 0.0f);
    uniform_set_float(u, "heightFalloff", fx->fog_height_falloff);
    uniform_set_float(u, "floorY", fx->fog_floor_y);
    // Water's medium, published by water_publish_to_postfx. 0 when there is no water,
    // which is the one "air only" state -- no separate enable to disagree with it.
    uniform_set_int(u, "waterMedium", fx->water_medium);
    uniform_set_float(u, "waterLevelY", fx->water_level_y);
    uniform_set_vec3(u, "waterExtinction", fx->water_extinction);
    uniform_set_vec3(u, "waterInscatter", fx->water_inscatter);
    // The cloud deck, published by sky_publish_to_postfx. Tile 0 is the single off state --
    // no deck, no march yet, or shadows switched off all reach the shader as that one value,
    // because the publisher clears it rather than leaving it stale. The sampler unit is
    // seeded once at program creation, like every other sampler on this program.
    uniform_set_float(u, "cloudShadowTile", fx->cloud_shadow_tile);
    uniform_set_float(u, "cloudShadowShellY", fx->cloud_shadow_shell_y);
    uniform_set_vec2(u, "cloudShadowShear", fx->cloud_shadow_shear);
    uniform_set_int(u, "cloudShadowLight", fx->cloud_shadow_light);
    // Local volumes, ranged like the light arrays below rather than one name per element:
    // the destinations are contiguous, and a name built per element is the one shape
    // warn_if_array_shorter cannot check. Only the live entries go up; the shader loops to
    // the count, so the tail is never read.
    uniform_set_int(u, "localFogCount", fx->local_fog_count);
    if (fx->local_fog_count > 0) {
        GLint loc = uniform_location(u, "localFogCenterDensity[0]");
        if (loc >= 0)
            glUniform4fv(loc, fx->local_fog_count, (const GLfloat*)fx->local_fog_center_density);
        loc = uniform_location(u, "localFogExtentFeather[0]");
        if (loc >= 0)
            glUniform4fv(loc, fx->local_fog_count, (const GLfloat*)fx->local_fog_extent_feather);
        loc = uniform_location(u, "localFogTint[0]");
        if (loc >= 0)
            glUniform3fv(loc, fx->local_fog_count, (const GLfloat*)fx->local_fog_tint);
    }
    uniform_set_float(u, "anisotropy", fx->fog_anisotropy);
    uniform_set_float(u, "sunBoost", fx->fog_sun_boost);
    uniform_set_float(u, "shadowBias", fx->fog_shadow_bias);
    // Count 0 (shadows off or absent) degrades fog to ambient haze; the publish
    // guarantees the map array is valid whenever the count is nonzero.
    uniform_set_int(u, "numLights", fx->fog_light_count);
    uniform_set_int(u, "cascadeCount", fx->fog_cascade_count);
    if (fx->fog_light_count > 0) {
        GLint lloc = uniform_location(u, "lightSpaceMatrix[0]");
        if (lloc >= 0)
            glUniformMatrix4fv(lloc, fx->fog_light_count * fx->fog_cascade_count, GL_FALSE,
                               (const GLfloat*)fx->fog_light_space);
        lloc = uniform_location(u, "lightColor[0]");
        if (lloc >= 0)
            glUniform3fv(lloc, fx->fog_light_count, (const GLfloat*)fx->fog_light_color);
        lloc = uniform_location(u, "lightDir[0]");
        if (lloc >= 0)
            glUniform3fv(lloc, fx->fog_light_count, (const GLfloat*)fx->fog_light_dir);
    }
    uniform_set_int(u, "spotEnabled", fx->fog_spot_enabled ? 1 : 0);
    int spot_shadowed = (fx->fog_spot_enabled && fx->fog_spot_shadowed) ? 1 : 0;
    uniform_set_int(u, "spotShadowed", spot_shadowed);
    if (fx->fog_spot_enabled) {
        uniform_set_vec3(u, "spotPos", fx->fog_spot_pos);
        uniform_set_vec3(u, "spotDir", fx->fog_spot_dir);
        uniform_set_vec3(u, "spotColor", fx->fog_spot_color);
        uniform_set_vec3(u, "spotAtten", fx->fog_spot_atten);
        uniform_set_float(u, "spotCosInner", fx->fog_spot_cos_inner);
        uniform_set_float(u, "spotCosOuter", fx->fog_spot_cos_outer);
        uniform_set_int(u, "spotIesProfile", fx->fog_spot_ies_profile);
        uniform_set_vec3(u, "spotUp", fx->fog_spot_up);
        if (spot_shadowed) {
            uniform_set_mat4(u, "spotLightSpaceMatrix", (float*)fx->fog_spot_light_space);
            uniform_set_int(u, "spotShadowLayer", fx->fog_spot_shadow_layer);
        }
    }
}

// Downsample the scene's depth cascades into the fog's exponential shadow
// representation, then blur it separably. Runs before the volume is injected,
// once per frame, and only while a caster is published.
//
// Layer count follows the publish, so a cascade-count change reallocates rather
// than reading layers that no longer exist.
// One layer, three passes: depth -> exp into dst, then blur dst -> scratch and
// scratch -> dst, one axis each. Ending in dst is why there is no handle swap
// and no fourth copy. `layer` addresses the SOURCE, which is the depth array on
// the first pass and the ESM array on the other two.
static void esm_build_layer(PostFX* fx, UniformManager* eu, GLuint src_depth, int src_layer,
                            int dst_layer) {
    const float texel = 1.0f / (float)POSTFX_FOG_ESM_SIZE;
    const struct {
        GLuint dst, src;
        int layer, mode;
        float dx, dy;
    } steps[3] = {
        {fx->fog_esm_array, src_depth, src_layer, 0, 0.0f, 0.0f},
        {fx->fog_esm_scratch, fx->fog_esm_array, dst_layer, 1, texel, 0.0f},
        {fx->fog_esm_array, fx->fog_esm_scratch, dst_layer, 1, 0.0f, texel},
    };
    for (int s = 0; s < 3; s++) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, steps[s].dst, 0, dst_layer);
        uniform_set_int(eu, "layer", steps[s].layer);
        uniform_set_int(eu, "mode", steps[s].mode);
        uniform_set_vec2(eu, "blurStep", (vec2){steps[s].dx, steps[s].dy});
        glActiveTexture(GL_TEXTURE0 + (steps[s].mode == 0 ? 0 : 1));
        glBindTexture(GL_TEXTURE_2D_ARRAY, steps[s].src);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
}

static bool postfx_build_fog_esm(PostFX* fx) {
    int cascade_layers = fx->fog_shadow_map_array ? fx->fog_light_count * fx->fog_cascade_count : 0;
    bool spot = fx->fog_spot_enabled && fx->fog_spot_shadowed && fx->fog_punctual_shadow_maps;
    int layers = cascade_layers + (spot ? 1 : 0);
    if (layers <= 0 || !fx->fog_esm_program || !fx->fog_esm_enabled)
        return false;

    if (fx->fog_esm_layers != layers) {
        if (fx->fog_esm_array)
            glDeleteTextures(1, &fx->fog_esm_array);
        if (fx->fog_esm_scratch)
            glDeleteTextures(1, &fx->fog_esm_scratch);
        fx->fog_esm_array = create_texture_2d_array_float(POSTFX_FOG_ESM_SIZE, POSTFX_FOG_ESM_SIZE,
                                                          layers, GL_R32F, GL_RED);
        fx->fog_esm_scratch = create_texture_2d_array_float(
            POSTFX_FOG_ESM_SIZE, POSTFX_FOG_ESM_SIZE, layers, GL_R32F, GL_RED);
        if (!fx->fog_esm_array || !fx->fog_esm_scratch) {
            log_error("Failed to allocate fog ESM cascades");
            fx->fog_esm_layers = 0;
            return false;
        }
        fx->fog_esm_layers = layers;
    }
    if (!fx->fog_esm_fbo)
        glGenFramebuffers(1, &fx->fog_esm_fbo);

    glBindFramebuffer(GL_FRAMEBUFFER, fx->fog_esm_fbo);
    glViewport(0, 0, POSTFX_FOG_ESM_SIZE, POSTFX_FOG_ESM_SIZE);
    glUseProgram(fx->fog_esm_program->id);
    UniformManager* eu = fx->fog_esm_program->uniforms;
    uniform_set_int(eu, "srcDepth", 0);
    uniform_set_int(eu, "srcEsm", 1);
    uniform_set_float(eu, "esmK", fx->fog_esm_k);
    glBindVertexArray(fx->quad_vao);
    // Completeness is only meaningful once a layer is attached, and a driver
    // that rejects a layered array attachment would otherwise fail silently.
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, fx->fog_esm_array, 0, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Fog ESM FBO incomplete; falling back to the binary tap");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }

    for (int layer = 0; layer < cascade_layers; layer++)
        esm_build_layer(fx, eu, fx->fog_shadow_map_array, layer, layer);
    // The spot rides the same array in the layer after the cascades, so the
    // medium reads one texture whatever is casting into it.
    if (spot)
        esm_build_layer(fx, eu, fx->fog_punctual_shadow_maps, fx->fog_spot_shadow_layer,
                        cascade_layers);

    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

// Build the fog scattering volume: light the medium once per cell, then
// integrate front-to-back along each froxel column. Everything about froxel
// parity, reprojection and the adjacency stamp lives here, so the composite
// stage above it does not have to carry any of it.
static void postfx_build_fog_volume(PostFX* fx, mat4 projection, mat4 view, bool esm_on) {
    // Frame parity picks this frame's write target; the other volume still
    // holds the previous frame's scattering for reprojection.
    const int write = fx->frame_index & 1;
    const int prev = write ^ 1;
    // Unlike the other accumulators here this one is NOT gated on TAA: it owns
    // a history volume and reprojects through its own stored camera, so it
    // needs nothing from the TAA resolve. It is also not optional. The cascade
    // tap is binary per cell, and one tap per 160x90 cell makes a shadow
    // boundary in the fog visibly blocky -- the screen-space march hid the same
    // hard taps by averaging 24 of them along every ray. Jittering the sample
    // and averaging across frames is what replaces that averaging, so gating it
    // on TAA left the default configuration showing raw stair-stepped shadows.
    // Determinism survives because the jitter is a function of the frame index:
    // frame N is the same on every run.
    // The >= 0 is what makes the sentinel a sentinel. froxel_prev_frame starts
    // at -1 to mean "no previous volume", and its own comment says 0 was avoided
    // because it would match frame 0 -- but the test is against frame_index - 1,
    // so at frame 0 the sentinel matched exactly the frame it was chosen to
    // exclude. The volume then blended against its zero-cleared history and the
    // first frame's fog came out at a tenth of its converged value, ramping up
    // over the next few dozen. sky_clouds.c has carried this guard all along.
    const int temporal =
        (fx->froxel_prev_frame >= 0 && fx->froxel_prev_frame == fx->frame_index - 1) ? 1 : 0;

    mat4 inv_view;
    glm_mat4_inv(view, inv_view);

    // 1. Inject: scattering + extinction per cell, one draw per slice.
    glUseProgram(fx->froxel_inject_program->id);
    UniformManager* iu = fx->froxel_inject_program->uniforms;
    // The medium reads the ESM cascades when they built this frame, the exact
    // depth array otherwise -- the shader branches on esmEnabled.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, esm_on ? fx->fog_esm_array : fx->fog_shadow_map_array);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D_ARRAY, fx->fog_punctual_shadow_maps);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_3D, fx->froxel_scatter[prev]);
    // The cloud deck's sun transmittance, or a 1x1 white stand-in when there is no deck.
    //
    // The stand-in rather than texture 0, which was the obvious way to write this and is
    // MEASURABLY worse: an incomplete texture bound to a live sampler makes this driver
    // substitute per draw, and the substitution costs more than sampling a real texture.
    // White is the transmittance identity, so the value is right even for a reader that
    // ignores cloudShadowTile -- the unit is never stale and never incomplete.
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, fx->cloud_shadow_tex ? fx->cloud_shadow_tex : fx->white_tex);
    glActiveTexture(GL_TEXTURE0);
    upload_fog_uniforms(fx, iu, projection, inv_view);
    uniform_set_int(iu, "froxelDepth", fx->froxel_built_z);
    uniform_set_int(iu, "temporal", temporal);
    uniform_set_float(iu, "temporalBlend", fx->fog_temporal_blend);
    uniform_set_int(iu, "esmEnabled", esm_on ? 1 : 0);
    uniform_set_float(iu, "esmK", fx->fog_esm_k);
    // Where the spot's ESM landed: the layer after the cascades.
    uniform_set_int(iu, "spotEsmLayer", fx->fog_light_count * fx->fog_cascade_count);
    uniform_set_int(iu, "frameIndex", fx->frame_index);
    uniform_set_mat4(iu, "prevView", (float*)fx->froxel_prev_view);
    uniform_set_mat4(iu, "prevProjection", (float*)fx->froxel_prev_proj);
    draw_volume_slices(fx, fx->froxel_scatter[write], iu);

    // 2. Integrate: slice k gathers 0..k. Reads the scatter volume, writes the
    // integrated one -- different textures, so no read-write hazard.
    glUseProgram(fx->froxel_integrate_program->id);
    UniformManager* gu = fx->froxel_integrate_program->uniforms;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, fx->froxel_scatter[write]);
    uniform_set_mat4(gu, "projection", (float*)projection);
    uniform_set_float(gu, "fogNear", postfx_fog_near(fx, projection));
    uniform_set_float(gu, "fogFar", fx->fog_far);
    uniform_set_float(gu, "fogDepthDist", fx->fog_depth_dist);
    uniform_set_int(gu, "froxelDepth", fx->froxel_built_z);
    draw_volume_slices(fx, fx->froxel_integrated, gu);

    // Remember the camera and the frame this volume was built with; next frame
    // reprojects its cells through them to find where they were. Inside this
    // function precisely because it is only true when a volume was built.
    glm_mat4_copy(view, fx->froxel_prev_view);
    glm_mat4_copy(projection, fx->froxel_prev_proj);
    fx->froxel_prev_frame = fx->frame_index;
}

// Fold the atmospheric media -- volumetric fog (spec 9.5) and aerial
// perspective (spec 9.6) -- into the canvas as one layer, before
// DoF/bloom/tonemap so shafts defocus, bloom, and meter like direct light.
// Either medium runs without the other. The fog layer and its history stay at
// RENDER res so the jitter-cancelling accumulator reads the aux depth 1:1
// (spec 9.5.1); only the stabilized layer magnifies at the fold.
static void postfx_run_atmosphere(PostFX* fx, GLuint canvas_fbo, bool aux_written,
                                  bool taa_resolving, mat4 projection, mat4 view,
                                  const PostFXGBufferWrites* writes) {
    // Both need the aux buffer: it is the only source of the linear depth the
    // composite indexes by.
    // Water and local volumes arm the pass through postfx_has_medium rather than by
    // setting fog_enabled, which belongs to the app and the GUI and is never cleared per
    // frame -- writing it here would leave fog on for good after one frame that had a
    // volume in it.
    const bool fog_on = postfx_has_medium(fx) && postfx_ensure_froxel_targets(fx);
    // Suppressed while submerged: the volume holds the sky-view integral, and that is
    // air the sight line never crosses down there.
    const bool aerial_on = fx->aerial_volume != 0 && !fx->water_suppress_aerial;
    if (!aux_written || (!fog_on && !aerial_on))
        return;
    // Inside the callee, below its early return: this function is a no-op on
    // most frames, and a scope at the call site would file a 0.000 ms row for
    // every one of them. Absent has to mean off.
    profiler_scope_begin(fx->profiler, "atmosphere");

    // Before the volume: the inject pass reads what this writes.
    bool esm_on = fog_on && postfx_build_fog_esm(fx);
    if (fog_on)
        postfx_build_fog_volume(fx, projection, view, esm_on);

    // Composite: out = inscatter + scene * transmittance, the same
    // enable/draw/restore idiom the screen-space fog and SSR composites use.
    //
    // Under TAA the layer is routed through a temporal accumulator first. The
    // composite picks its slice from the aux depth, which comes from the
    // JITTERED raster, while the volume is jitter-blind (inject reads only
    // projection[0][0]/[1][1]/[2][2]/[3][2], and the jitter lives in
    // [2][0]/[2][1]). This pass runs after the TAA resolve, so that per-frame
    // slice wobble lands on already-stabilized color with nothing downstream to
    // average it -- measured as ~3x the frame-to-frame flicker of the
    // screen-space march this replaced, concentrated entirely at silhouettes
    // and sky edges. Resampling the depth to undo the jitter does not work: the
    // buffer holds one sample per pixel taken at the jittered position, so
    // across a discontinuity the unjittered depth was never sampled at all.
    //
    // Gated on TAA, unlike the volume's accumulator above: with no jitter there
    // is no wobble to cancel and this would only add lag.
    glUseProgram(fx->froxel_composite_program->id);
    UniformManager* cu = fx->froxel_composite_program->uniforms;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx->aux_texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, fx->froxel_integrated);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_3D, fx->aerial_volume);
    // The translucent stack's own depth and coverage (spec 11.78). Armed only with
    // a moment atlas: the weighted-blended accumulation carries no depth
    // statistic, so --no-oit-moments keeps the single-depth composite exactly.
    const bool moments_armed = writes->oit_moment_atlas != 0;
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, writes->oit_moment_atlas);
    glActiveTexture(GL_TEXTURE0);
    uniform_set_int(cu, "momentArmed", moments_armed ? 1 : 0);
    uniform_set_vec2(cu, "oitNearFar", writes->oit_near_far);
    uniform_set_mat4(cu, "projection", (float*)projection);
    uniform_set_float(cu, "fogNear", postfx_fog_near(fx, projection));
    uniform_set_float(cu, "fogFar", fx->fog_far);
    uniform_set_float(cu, "fogDepthDist", fx->fog_depth_dist);
    uniform_set_float(cu, "aerialFar", fx->aerial_far);
    // A slice count of zero IS the "medium absent" signal -- the shader reads a
    // neutral (0,0,0,1) for it. One state per medium rather than a count plus a
    // separate on/off flag that could disagree with it.
    uniform_set_int(cu, "froxelDepth", fog_on ? fx->froxel_built_z : 0);
    uniform_set_int(cu, "aerialDepth", aerial_on ? fx->aerial_slices : 0);
    uniform_set_int(cu, "mode", 0);

    if (taa_resolving && postfx_ensure_fog_layer_targets(fx)) {
        // Draw the layer to scratch instead of the scene, stabilize it, then
        // fold that in place of the raw layer.
        glBindFramebuffer(GL_FRAMEBUFFER, fx->fog_layer_fbo);
        glViewport(0, 0, fx->width, fx->height);
        draw_fullscreen_quad(fx->quad_vao);

        // Same adjacency test the fog volume uses, and for the same reason: a
        // history is only reprojectable against the IMMEDIATELY preceding frame.
        // A flag cleared on an else branch could not express that -- this pass
        // is skipped entirely whenever both media are off, or TAA is, so the
        // history would survive an arbitrary gap and then be reprojected by an
        // unrelated frame's velocity.
        fx->fog_layer_history.valid = (fx->fog_layer_frame == fx->frame_index - 1);
        GLuint stable = run_temporal_accum(fx, fx->temporal_accum_program, &fx->fog_layer_history,
                                           fx->width, fx->height, fx->fog_layer_texture,
                                           TEMPORAL_FEEDBACK_DEFAULT);
        fx->fog_layer_frame = fx->frame_index;

        glUseProgram(fx->froxel_composite_program->id);
        uniform_set_int(cu, "mode", 1);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, stable);
        glActiveTexture(GL_TEXTURE0);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, canvas_fbo);
    glViewport(0, 0, fx->post_width, fx->post_height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_SRC_ALPHA);
    draw_fullscreen_quad(fx->quad_vao);
    glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    check_gl_error("postfx atmosphere");
    profiler_scope_end(fx->profiler);
}

// Screen-space reflections: rebuild the Hi-Z min-depth pyramid, march the
// reflection buffer (stochastic when denoising), temporally accumulate + a-trous
// denoise, then premultiplied-composite onto the canvas before bloom so
// reflected highlights bloom like direct ones. Owns the ssr_history/ssr_atrous
// lifecycle. Traces at full or half RENDER res per fx->ssr_full_res, marching
// the render-res depth/normals and reading the canvas as its radiance source
// (post-TAA consumers read stabilized color); only the fold magnifies. Same
// extracted-stage shape as postfx_run_atmosphere; inv_projection is passed in
// (shared with DoF).
static void postfx_run_ssr(PostFX* fx, GLuint canvas_fbo, GLuint canvas_tex, bool have_normals,
                           bool taa_resolving, mat4 projection, mat4 inv_projection, mat4 view) {
    // SSR traces at full res (sharp) or half res, per ssr_full_res; the
    // buffer + Hi-Z pyramid were sized to match in create_ssr_buffers.
    int ssr_w = fx->ssr_full_res ? fx->width : fx->half_width;
    int ssr_h = fx->ssr_full_res ? fx->height : fx->half_height;
    // Rebuild the min-depth pyramid the traversal walks: level 0
    // takes the conservative min over the full-res depth, every
    // further level the min of the 2x2 below. The source level is
    // pinned via BASE/MAX_LEVEL so a level never reads itself.
    glBindFramebuffer(GL_FRAMEBUFFER, fx->hiz_fbo);
    glUseProgram(fx->ssr_hiz_program->id);
    glActiveTexture(GL_TEXTURE0);
    {
        int mip_w = ssr_w;
        int mip_h = ssr_h;
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
            // Full-res level 0 is the same size as the depth buffer, so it
            // is a 1:1 copy, not a 2x2 reduction (which would sample depth
            // at twice the coordinate and corrupt the whole pyramid).
            uniform_set_int(fx->ssr_hiz_program->uniforms, "copySrc",
                            (mip == 0 && fx->ssr_full_res) ? 1 : 0);
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

    // March reflections into the reflection buffer (reads the scene,
    // writes elsewhere: GL 4.1 has no texture barrier)
    glBindFramebuffer(GL_FRAMEBUFFER, fx->ssr_fbo);
    glViewport(0, 0, ssr_w, ssr_h);
    glUseProgram(fx->ssr_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx->depth_texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, fx->normal_texture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, canvas_tex);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, fx->hiz_texture);
    glActiveTexture(GL_TEXTURE0);
    uniform_set_int(fx->ssr_program->uniforms, "hizWidth", ssr_w);
    uniform_set_int(fx->ssr_program->uniforms, "hizHeight", ssr_h);
    uniform_set_int(fx->ssr_program->uniforms, "hizMips", fx->hiz_mips);
    uniform_set_mat4(fx->ssr_program->uniforms, "projection", (float*)projection);
    uniform_set_mat4(fx->ssr_program->uniforms, "invProjection", (float*)inv_projection);
    uniform_set_float(fx->ssr_program->uniforms, "maxDistance", fx->ssr_max_distance);
    uniform_set_float(fx->ssr_program->uniforms, "thicknessMin", fx->ssr_thickness_min);
    // The march budget is spent in screen columns, so it scales with the
    // trace width or the reach halves every time the resolution doubles
    // (a floor-hugging grazing ray crawls ~one column per iteration). The
    // 256 floor keeps every trace at or below 2048 wide bit-identical to
    // the fixed-budget era; the 1024 cap bounds the worst-case cost.
    int march_budget = ssr_w / 8;
    if (march_budget < 256)
        march_budget = 256;
    if (march_budget > 1024)
        march_budget = 1024;
    uniform_set_int(fx->ssr_program->uniforms, "marchBudget", march_budget);
    uniform_set_float(fx->ssr_program->uniforms, "floorRoughness", fx->ssr_floor_roughness);
    uniform_set_float(fx->ssr_program->uniforms, "maxRoughness", fx->ssr_max_roughness);
    // Strength folds into the march's premultiplied weight (clamped
    // there) so the composite stays a straight premultiplied lerp
    uniform_set_float(fx->ssr_program->uniforms, "strength", fx->ssr_strength);
    // Stochastic march: jitter the ray per pixel so the deterministic
    // Hi-Z grid stripes scatter into noise a resolver can clean. The
    // a-trous denoise (below) resolves it spatially every frame, so this
    // runs whenever the denoiser is on -- no TAA required. Off ->
    // deterministic march, bit-identical.
    int ssr_stochastic = fx->ssr_denoise ? 1 : 0;
    uniform_set_int(fx->ssr_program->uniforms, "ssrStochastic", ssr_stochastic);
    // Temporal accumulation runs when enabled AND TAA is resolving (it
    // needs per-frame jitter + motion vectors). The seed and the
    // accumulator pass below share this one predicate so the seed only
    // advances when there's an accumulator to average it.
    bool ssr_temporal_on = fx->ssr_temporal && taa_resolving;
    // Advance the per-pixel random each frame ONLY when temporal is on
    // (independent of the projection jitter, so it converges even in
    // headless with TAA on). Without temporal, freeze the seed: the
    // a-trous denoises a stable per-frame pattern, not a boiling one.
    // Modulo keeps the shader's float hash well-conditioned over long
    // sessions (the PCSS seed's bound, render.c).
    int ssr_seed = ssr_temporal_on ? fx->frame_index % 4096 : 0;
    uniform_set_int(fx->ssr_program->uniforms, "ssrFrameIndex", ssr_seed);
    uniform_set_float(fx->ssr_program->uniforms, "ssrJitter", fx->ssr_jitter);
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
        uniform_set_float(fx->ssr_program->uniforms, "probeIntensity", fx->probe_intensity);
    }
    // A SET of probes (spec 11.70): the descriptors already reach this program
    // through ProbeBlock, which is bound for the context's lifetime, so all the
    // pass needs is the atlas and the flag that arms the branch.
    uniform_set_int(fx->ssr_program->uniforms, "probeMulti", fx->probe_multi ? 1 : 0);
    if (fx->probe_multi) {
        mat4 inv_view;
        glm_mat4_inv(view, inv_view);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, fx->probe_atlas);
        glActiveTexture(GL_TEXTURE0);
        uniform_set_int(fx->ssr_program->uniforms, "probeAtlasTex", 5);
        uniform_set_mat4(fx->ssr_program->uniforms, "invView", (float*)inv_view);
    }
    draw_fullscreen_quad(fx->quad_vao);

    // Temporal accumulation: reproject the previous reflection by the
    // motion vectors and blend with SSR's OWN accumulator program --
    // ssr_accum_frag carries the signal policies (inverse-luma blend,
    // motion-adaptive slack and window) and the why. Needs TAA (per-frame
    // jitter + motion); off/no-TAA leaves the raw march.
    GLuint ssr_result = fx->ssr_texture;
    if (ssr_temporal_on) {
        ssr_result = run_temporal_accum(fx, fx->ssr_accum_program, &fx->ssr_history, ssr_w,
                                        ssr_h, fx->ssr_texture, TEMPORAL_FEEDBACK_SSR);
    } else {
        fx->ssr_history.valid = false;
    }

    // Spatial a-trous denoise: three edge-aware B3-spline passes at
    // doubling tap spacing (1, 2, 4) resolve the stochastic march's
    // per-pixel noise into a clean reflection every frame (no temporal
    // convergence wait, and it holds up under motion). It smooths the
    // premultiplied (color*weight, weight) buffer; the depth/normal
    // weights keep the blur on the floor and stop it at the silhouette.
    // SVGF order: denoise the accumulation. Skipped when the denoiser is
    // off -> bit-identical to the pre-denoise reflection.
    if (fx->ssr_denoise) {
        ssr_result = run_atrous(fx, fx->ssr_atrous_program, &fx->ssr_atrous, ssr_w, ssr_h,
                                ssr_result, have_normals);
        check_gl_error("postfx ssr denoise");
    }

    // Lerp the reflections onto the canvas before bloom so
    // reflected highlights bloom like direct ones. The buffer is
    // premultiplied, hence (ONE, ONE_MINUS_SRC_ALPHA). Restore the
    // engine's blend function afterward: it is set once at init
    // and everything else assumes it.
    glBindFramebuffer(GL_FRAMEBUFFER, canvas_fbo);
    glViewport(0, 0, fx->post_width, fx->post_height);
    glUseProgram(fx->upsample_tent_program->id);
    // Sample the tent at the reflection buffer's own texel: full-res is
    // a 1px tent (light AA on the sharp reflection); half-res is the
    // upsample.
    const float ssr_texel[2] = {1.0f / (float)ssr_w, 1.0f / (float)ssr_h};
    uniform_set_vec2(fx->upsample_tent_program->uniforms, "texelSize", ssr_texel);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ssr_result);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    draw_fullscreen_quad(fx->quad_vao);
    glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    check_gl_error("postfx ssr");
}

// Lazy single-sample resolve targets for the OIT accumulate/revealage attachments.
static bool postfx_ensure_oit_targets(PostFX* fx) {
    if (fx->oit_ready)
        return true;
    if (!create_color_fbo(fx->width, fx->height, GL_RGBA16F, &fx->oit_accum_fbo,
                          &fx->oit_accum_texture) ||
        !create_color_fbo(fx->width, fx->height, GL_R16F, &fx->oit_revealage_fbo,
                          &fx->oit_revealage_texture)) {
        log_error("Failed to allocate OIT resolve targets");
        return false;
    }
    fx->oit_ready = true;
    return true;
}

// OIT resolve: resolve the engine's MSAA accum (attachment 5) + revealage
// (attachment 6) to single-sample, then fold the accumulated transparency over
// the opaque scene in hdr_fbo. Runs before every downstream HDR pass so
// TAA/SSR/bloom/tonemap see the transparency. How the two forms of accumulation
// fold differs -- see the blend below.
static void postfx_run_oit(PostFX* fx, GLuint oit_fbo, bool moments) {
    if (!postfx_ensure_oit_targets(fx))
        return;
    // Below the allocation gate, which only this function can see.
    profiler_scope_begin(fx->profiler, "oit composite");
    resolve_color_attachment(oit_fbo, GL_COLOR_ATTACHMENT5, fx->oit_accum_fbo, fx->width,
                             fx->height);
    resolve_color_attachment(oit_fbo, GL_COLOR_ATTACHMENT6, fx->oit_revealage_fbo, fx->width,
                             fx->height);
    glBindFramebuffer(GL_FRAMEBUFFER, fx->hdr_fbo);
    glViewport(0, 0, fx->width, fx->height);
    glUseProgram(fx->oit_resolve_program->id);
    uniform_set_int(fx->oit_resolve_program->uniforms, "momentWeighted", moments ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fx->oit_accum_texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, fx->oit_revealage_texture);
    glEnable(GL_BLEND);
    // Moment-weighted layers already carry their own transmittance, so the sum
    // IS the foreground and only the background needs attenuating; the
    // weighted-blended average has to displace the background in proportion.
    if (moments)
        glBlendFunc(GL_ONE, GL_SRC_ALPHA); // sum + scene*reveal
    else
        glBlendFunc(GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA); // avgColor*(1-reveal) + scene*reveal
    draw_fullscreen_quad(fx->quad_vao);
    glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    check_gl_error("postfx oit");
    profiler_scope_end(fx->profiler);
}

void postfx_run(PostFX* fx, GLuint msaa_fbo, GLuint target_fbo, bool frame_is_hdr,
                const PostFXGBufferWrites* writes, mat4 projection, mat4 view) {
    const bool normals_written = writes->normals;
    const bool aux_written = writes->aux;
    const bool albedo_written = writes->albedo;
    const bool sss_written = writes->sss;
    const bool spec_written = writes->spec;
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
    profiler_scope_begin(fx->profiler, "msaa resolve");
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msaa_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fx->hdr_fbo);
    glBlitFramebuffer(0, 0, fx->width, fx->height, 0, 0, fx->width, fx->height, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    profiler_scope_end(fx->profiler);

    // OIT: composite the accumulated transparent layer over the resolved opaque
    // scene before any downstream HDR pass (TAA/SSR/bloom/tonemap) reads it.
    if (writes->oit_fbo != 0)
        postfx_run_oit(fx, writes->oit_fbo, writes->oit_moment_atlas != 0);

    if (mode == POSTFX_TONEMAP_PASSTHROUGH) {
        // Display-ready frame: copy to the target, skipping bloom and tone
        // mapping -- and with it the seam, so at a reduced render scale this
        // magnifies rather than reconstructs. Linear filtering box-downsamples
        // the supersampled buffer to the display size (a 1:1 identity blit at
        // supersampling 1 and render scale 1).
        //
        // Also skips the output dither, which lives in the tonemap shader: a
        // blit cannot dither without becoming a shader pass. Deliberate -- this
        // branch carries debug and LDR-authored frames, which are data rather
        // than graded images, and they skip exposure and tone mapping too.
        profiler_scope_begin(fx->profiler, "passthrough blit");
        if (fx->width == fx->out_width && fx->height == fx->out_height) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fx->hdr_fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fbo);
            glBlitFramebuffer(0, 0, fx->width, fx->height, 0, 0, fx->out_width, fx->out_height,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        } else {
            /*
             * A SCALED blit is not available here, so the copy goes through a shader.
             *
             * glBlitFramebuffer refuses a stretch whenever either side is multisample, and the
             * DEFAULT framebuffer is exactly that on a windowed session that asked for
             * samples. It raises GL_INVALID_OPERATION and draws nothing -- and because GL
             * errors are sticky it is then reported by whichever pass checks next, which is
             * the sky's aerial bake on the following frame. That is a debug view failing to
             * present and blaming the atmosphere.
             *
             * It only bites when the two sizes disagree, which is why it hid: at a window the
             * compositor grants in full they are equal and the blit is an identity. Ask for
             * one larger than the display, or supersample, and the sizes part.
             *
             * The tent with a zero texel size is the documented exact-copy form, so at 1:1
             * this is the same image the blit produced, and above 1:1 it magnifies where a
             * scaled blit would have box-filtered. Debug frames are data rather than graded
             * images; a nearest magnification of data is the honest one.
             */
            glUseProgram(fx->upsample_tent_program->id);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, fx->hdr_texture);
            uniform_set_vec2(fx->upsample_tent_program->uniforms, "texelSize",
                             (const float[]){0.0f, 0.0f});
            glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
            glViewport(0, 0, fx->out_width, fx->out_height);
            draw_fullscreen_quad(fx->quad_vao);
        }
        profiler_scope_end(fx->profiler);
    } else {
        // The color TAA resolves iff it is enabled and its velocity buffer was
        // produced. The single invariant behind both the TAA pass and the AO
        // temporal accumulation: AO accumulates exactly when color TAA does.
        bool taa_resolving = postfx_taa_active(fx) && aux_written;

        // Resolve the aux G-buffer (attachment 2: motion .xy + linear view-Z .z)
        // once, ahead of both consumers -- TAA reprojection reads the motion,
        // GTAO reconstructs positions from the linear Z. Hoisted out of the TAA
        // block so GTAO gets linear depth even when TAA is off.
        // One scope for all five G-buffer resolves below rather than five rows
        // of one blit each: they are the same operation on adjacent
        // attachments, and which of them ran is already decided by the
        // engine's per-frame write flags.
        profiler_scope_begin(fx->profiler, "gbuffer resolves");
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
        // Resolve the skin-diffuse G-buffer (attachment 4) for separable SSS, iff
        // the scene pass wrote it (engine's frame-start decision, like albedo).
        if (sss_written && postfx_ensure_sss_targets(fx)) {
            resolve_color_attachment(msaa_fbo, GL_COLOR_ATTACHMENT4, fx->sss_diffuse_fbo, fx->width,
                                     fx->height);
        }
        // Resolve the ambient-specular attachment (7) for the split spec-occ
        // composite, iff the scene pass wrote it (same threading as the
        // others). split_live is the ONE owner of "the composite runs this
        // frame": the resolve, the composite draw, and the tonemap's
        // aoEnabled stand-down all derive from it, so they cannot disagree.
        // If the target or program is unavailable the mode latches back to
        // legacy -- a half-live split loses the specular the scene already
        // routed away, and would lose AO too if the tonemap stood down.
        bool split_live = false;
        if (spec_written) {
            split_live = postfx_ensure_spec_target(fx) && fx->spec_occ_composite_program != NULL;
            if (split_live) {
                resolve_color_attachment(msaa_fbo, GL_COLOR_ATTACHMENT7, fx->spec_fbo, fx->width,
                                         fx->height);
            } else {
                log_error("split spec-occ unavailable; falling back to legacy");
                fx->spec_occlusion_mode = POSTFX_SPEC_OCC_LEGACY;
            }
        }

        // Resolve the scene pass's second attachment (normal .xyz + SSR marker .a)
        // ahead of its consumers (SSAO now, SSR later). The caller reports
        // whether the attachment was written this frame; re-deriving it from
        // fx flags here could disagree with what the scene pass produced.
        bool have_normals = normals_written;
        if (have_normals) {
            resolve_color_attachment(msaa_fbo, GL_COLOR_ATTACHMENT1, fx->normal_fbo, fx->width,
                                     fx->height);
            check_gl_error("postfx normals resolve");
        }
        profiler_scope_end(fx->profiler);

        // Contact shadows (spec 9.3): a full-res depth march toward the lights,
        // blurred and temporally accumulated like GTAO, consumed by tonemap.
        // Gated on SOMETHING TO MARCH -- a shadow-casting directional being
        // published (fog_light_dir[0]), or a local light no shadow map can serve
        // (spec 11.56); a room lit only by practicals used to skip the pass
        // entirely, which is where the feature was needed most. Also gated on
        // cs_distance > 0, which makes --cs-distance 0 take the exact off path (a
        // zero-length march would still evaluate a 1.0 multiply whose
        // bit-exactness is not guaranteed). Runs before GTAO so both occlusion
        // terms are ready when tonemap composites.
        GLuint cs_result_tex = 0;
        bool cs_accum_ran = false;
        const bool cs_key_light = fx->fog_light_count > 0;
        const bool cs_active = fx->contact_shadows_enabled && aux_written &&
                               postfx_contact_shadows_have_light(fx) && fx->cs_distance > 0.0f &&
                               postfx_ensure_contact_targets(fx);
        if (cs_active) {
            profiler_scope_begin(fx->profiler, "contact shadows");
            // World-space travel direction -> view-space TOWARD-light unit vector
            // (pbr uses L = -light->direction; fog_light_dir[0] is that direction).
            // Zeroed rather than left indeterminate, since the uniform is uploaded
            // either way and hasKeyLight is what tells the shader to ignore it.
            // Same for keyRadiance below, which reads a slot the publish step
            // leaves untouched when it reports no caster.
            vec3 toward, cs_dir_vs = {0.0f, 0.0f, 0.0f};
            if (cs_key_light) {
                glm_vec3_negate_to(fx->fog_light_dir[0], toward);
                glm_mat4_mulv3(view, toward, 0.0f, cs_dir_vs);
                glm_vec3_normalize(cs_dir_vs);
            }

            glBindFramebuffer(GL_FRAMEBUFFER, fx->cs_fbo[0]);
            glViewport(0, 0, fx->width, fx->height);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glUseProgram(fx->contact_shadow_program->id);
            UniformManager* cu = fx->contact_shadow_program->uniforms;
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, fx->aux_texture);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, have_normals ? fx->normal_texture : 0);
            uniform_set_int(cu, "useNormalsTex", have_normals ? 1 : 0);
            uniform_set_mat4(cu, "projection", (float*)projection);
            // The cluster list stores WORLD positions, so the march needs the
            // view matrix to reach them; the key light arrives pre-transformed
            // above because it has no position to transform.
            uniform_set_mat4(cu, "view", (float*)view);
            uniform_set_vec3(cu, "lightDirVS", cs_dir_vs);
            // Same slot as the direction, so the fold weight and the direction
            // cannot describe two different lights.
            uniform_set_vec3(cu, "keyRadiance", fx->fog_light_color[0]);
            uniform_set_int(cu, "hasKeyLight", cs_key_light ? 1 : 0);
            uniform_set_float(cu, "csDistance", fx->cs_distance);
            uniform_set_int(cu, "temporal", taa_resolving ? 1 : 0);
            // % 4096 keeps the shader's float hash well-conditioned over
            // long sessions (the PCSS/SSR seed bound)
            uniform_set_int(cu, "frameIndex", fx->frame_index % 4096);
            draw_fullscreen_quad(fx->quad_vao);

            // Reuse the AO bilateral blur. Its texelSize is per-draw now (the
            // GTAO consumer runs at AO res, this one at full res), so set the
            // full-res texel here. edgeAware is forced on: silhouette bleed drips
            // a directional shadow worse than it does ambient AO.
            glBindFramebuffer(GL_FRAMEBUFFER, fx->cs_fbo[1]);
            glUseProgram(fx->ssao_blur_program->id);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, fx->cs_texture[0]);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, fx->aux_texture);
            const float cs_texel[2] = {1.0f / (float)fx->width, 1.0f / (float)fx->height};
            uniform_set_vec2(fx->ssao_blur_program->uniforms, "texelSize", cs_texel);
            uniform_set_int(fx->ssao_blur_program->uniforms, "edgeAware", 1);
            draw_fullscreen_quad(fx->quad_vao);
            cs_result_tex = fx->cs_texture[1];

            if (taa_resolving) {
                cs_result_tex = run_temporal_accum(fx, fx->temporal_accum_program, &fx->cs_history,
                                                   fx->width, fx->height, fx->cs_texture[1],
                                                   TEMPORAL_FEEDBACK_DEFAULT);
                cs_accum_ran = true;
            }
            check_gl_error("postfx contact shadows");
            profiler_scope_end(fx->profiler);
        }
        if (!cs_accum_ran)
            fx->cs_history.valid = false;

        // Depth (and its inverse) serve SSR and DoF's circle-of-confusion. GTAO
        // no longer needs it -- it reconstructs from the aux buffer's linear Z.
        bool ssr_active = postfx_ssr_active(fx, have_normals);
        bool dof_active = fx->dof_enabled;
        // SSS needs it for a reason the other two do not: it wants a depth that is a
        // SAMPLE rather than a mean. A depth blit resolves by selecting; a colour blit
        // averages, and a depth averaged with the cleared far value describes a surface
        // that is nowhere -- which the gather then divides its blur radius by.
        bool sss_active = sss_written && fx->sss_ready;
        mat4 inv_projection;
        if (ssr_active || dof_active || sss_active) {
            // Resolve depth alongside color so screen-space passes can
            // reconstruct view-space positions (formats match: both are
            // DEPTH24_STENCIL8)
            profiler_scope_begin(fx->profiler, "depth resolve");
            glBindFramebuffer(GL_READ_FRAMEBUFFER, msaa_fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fx->depth_fbo);
            glBlitFramebuffer(0, 0, fx->width, fx->height, 0, 0, fx->width, fx->height,
                              GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            check_gl_error("postfx depth resolve");
            profiler_scope_end(fx->profiler);

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
        // The reflection lobe is only worth measuring when the split composite
        // is there to apply it AND the masks are actually being swept for AO.
        // With AO off the composite already folds the specular back untouched
        // through its own aoActive == 0 arm, so there is nothing for this to say.
        const bool spec_occ_swept =
            split_live && fx->ssao_enabled && postfx_ensure_spec_occ_targets(fx);
        bool ao_accum_ran = false;
        bool gi_accum_ran = false;
        bool spec_occ_accum_ran = false;
        if (gtao_active) {
            profiler_scope_begin(fx->profiler, "gtao sweep");
            // Raw occlusion at half res. GTAO reads linear view-Z from the aux
            // buffer's .z (unit 0) and reconstructs positions from it -- the
            // non-linear depth buffer staircased flat surfaces into AO banding.
            glBindFramebuffer(GL_FRAMEBUFFER, fx->ssao_fbo[0]);
            glViewport(0, 0, fx->half_width, fx->half_height);
            // SSGI and split spec-occ both ride this sweep, and both are
            // optional, so the draw-buffer list is built rather than chosen:
            // GL_NONE holds an absent slot open so attachment 2 keeps its
            // location whether or not SSGI is on. With neither on, only AO is
            // written and the path is byte-identical to plain GTAO.
            if (ssgi_active || spec_occ_swept) {
                const GLenum bufs[3] = {GL_COLOR_ATTACHMENT0,
                                        ssgi_active ? GL_COLOR_ATTACHMENT1 : GL_NONE,
                                        spec_occ_swept ? GL_COLOR_ATTACHMENT2 : GL_NONE};
                glDrawBuffers(3, bufs);
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
            // The composite has not rejoined ambient spec yet; the gather
            // sums it itself (rationale at the shader's specTex declaration).
            const bool gather_spec = ssgi_active && split_live;
            glActiveTexture(GL_TEXTURE4);
            glBindTexture(GL_TEXTURE_2D, gather_spec ? fx->spec_texture : 0);
            uniform_set_int(fx->gtao_program->uniforms, "useNormalsTex", have_normals ? 1 : 0);
            uniform_set_mat4(fx->gtao_program->uniforms, "projection", (float*)projection);
            uniform_set_float(fx->gtao_program->uniforms, "radius", fx->ssao_radius);
            uniform_set_int(fx->gtao_program->uniforms, "temporal", taa_resolving ? 1 : 0);
            // % 4096: same float-hash conditioning bound as PCSS/SSR
            uniform_set_int(fx->gtao_program->uniforms, "frameIndex", fx->frame_index % 4096);
            uniform_set_int(fx->gtao_program->uniforms, "gatherGI", ssgi_active ? 1 : 0);
            uniform_set_int(fx->gtao_program->uniforms, "gatherSpec", gather_spec ? 1 : 0);
            uniform_set_int(fx->gtao_program->uniforms, "specOcclusion",
                            spec_occ_swept ? 1 : 0);
            draw_fullscreen_quad(fx->quad_vao);
            profiler_scope_end(fx->profiler);

            if (fx->ssao_enabled) {
                // Accumulate the RAW sweep, then blur the accumulation -- the
                // SVGF order the SSGI chain below already uses, and not merely a
                // stylistic match to it.
                //
                // The accumulator bounds history to the current frame's 3x3
                // neighbourhood, which is a spatial stand-in for "has this pixel
                // disoccluded". That reads correctly only while the neighbourhood
                // still carries the estimator's spread. Blurring first is exactly
                // what removes that spread: the 4x4 box exists to cancel the
                // rotation tile, so by the time the clamp looked at it, the
                // window had collapsed to narrower than the frame-to-frame
                // variation it was supposed to admit, and every frame it snapped
                // history back onto the current noisy estimate. Ordered this way
                // the clamp sees the unfiltered spread and passes the history it
                // was meant to keep, while still rejecting a genuine disocclusion.
                profiler_scope_begin(fx->profiler, "ao denoise");
                GLuint ao_denoise_src = fx->ssao_texture[0];
                if (taa_resolving) {
                    ao_denoise_src =
                        run_temporal_accum(fx, fx->temporal_accum_program, &fx->ao_history,
                                           fx->half_width, fx->half_height, fx->ssao_texture[0],
                                           TEMPORAL_FEEDBACK_AO);
                    ao_accum_ran = true;
                }

                // 4x4 box blur cancels the rotation-noise tile; depth-bilateral
                // when the edge filter is on so it does not bleed across silhouettes.
                glBindFramebuffer(GL_FRAMEBUFFER, fx->ssao_fbo[1]);
                glViewport(0, 0, fx->half_width, fx->half_height);
                glUseProgram(fx->ssao_blur_program->id);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, ao_denoise_src);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, fx->aux_texture);
                // texelSize is per-draw now (the contact-shadow blur runs the
                // same program at full res); set the AO-res texel for this one.
                const float ao_texel[2] = {1.0f / (float)fx->half_width,
                                           1.0f / (float)fx->half_height};
                uniform_set_vec2(fx->ssao_blur_program->uniforms, "texelSize", ao_texel);
                uniform_set_int(fx->ssao_blur_program->uniforms, "edgeAware",
                                fx->ao_edge_filter_enabled ? 1 : 0);
                draw_fullscreen_quad(fx->quad_vao);
                ao_result_tex = fx->ssao_texture[1];
                profiler_scope_end(fx->profiler);

                // The reflection-lobe sums through the same two stages, in the
                // same order and with the same weights, because they carry the
                // same estimator's noise: one mask per slice, popcounted. The
                // blur program is reused verbatim -- it weights all four
                // channels alike and neither of these is a colour.
                if (spec_occ_swept) {
                    profiler_scope_begin(fx->profiler, "spec occ denoise");
                    GLuint spec_src = fx->spec_occ_raw_texture;
                    if (taa_resolving) {
                        spec_src = run_temporal_accum(
                            fx, fx->temporal_accum_program, &fx->spec_occ_history, fx->half_width,
                            fx->half_height, fx->spec_occ_raw_texture, TEMPORAL_FEEDBACK_AO);
                        spec_occ_accum_ran = true;
                    }
                    glBindFramebuffer(GL_FRAMEBUFFER, fx->spec_occ_fbo);
                    glViewport(0, 0, fx->half_width, fx->half_height);
                    glUseProgram(fx->ssao_blur_program->id);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, spec_src);
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D, fx->aux_texture);
                    uniform_set_vec2(fx->ssao_blur_program->uniforms, "texelSize", ao_texel);
                    uniform_set_int(fx->ssao_blur_program->uniforms, "edgeAware",
                                    fx->ao_edge_filter_enabled ? 1 : 0);
                    draw_fullscreen_quad(fx->quad_vao);
                    profiler_scope_end(fx->profiler);
                }
            }
        }
        if (!ao_accum_ran)
            fx->ao_history.valid = false;
        if (!spec_occ_accum_ran)
            fx->spec_occ_history.valid = false;

        if (split_live) {
            profiler_scope_begin(fx->profiler, "spec occ composite");
            postfx_run_spec_occ_composite(fx, ao_result_tex,
                                          spec_occ_swept ? fx->spec_occ_texture : 0, have_normals,
                                          aux_written);
            profiler_scope_end(fx->profiler);
        }

        // The seam. Everything above ran at render res into hdr_fbo; everything
        // below composites onto the canvas at post res. At render scale 1 the
        // canvas IS the hdr buffer, so the values below equal their old ones
        // and the seam is a no-op; below 1 the post buffer takes over and the
        // render-res frame is brought up to it here.
        const bool post_canvas = postfx_taau_active(fx);
        GLuint canvas_fbo = post_canvas ? fx->post_fbo : fx->hdr_fbo;
        GLuint canvas_tex = post_canvas ? fx->post_texture : fx->hdr_texture;

        // Temporal AA resolve, after the AO chain and before every HDR
        // consumer (SSR/DoF/bloom/tonemap read anti-aliased color). The AO
        // chain runs first so the split spec-occ composite can rejoin ambient
        // specular to the scene BEFORE this resolve -- the reunited frame is
        // then stabilized as one image, which is what keeps smooth metal from
        // shimmering against its own occlusion. The one input this moves:
        // SSGI's gather now samples pre-TAA color (it rides the GTAO sweep).
        if (taa_resolving) {
            profiler_scope_begin(fx->profiler, "taa resolve");
            // The history is post-res either way; only how this frame reaches
            // it differs -- TAAU reconstructs the smaller raster onto it, plain
            // TAA accumulates a same-size frame.
            if (post_canvas)
                run_taau_resolve(fx);
            else
                run_temporal_accum(fx, fx->taa_resolve_program, &fx->taa_history, fx->width,
                                   fx->height, fx->hdr_texture, TEMPORAL_FEEDBACK_DEFAULT);

            // Push the resolved frame onto the canvas (the history side is
            // kept as next frame's accumulation buffer). The copy is what
            // keeps the history clean: post-seam passes composite onto the
            // canvas, and folding fog/SSR into the history would feed them
            // back through every later frame's blend.
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fx->taa_history.fbo[fx->frame_index & 1]);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, canvas_fbo);
            glBlitFramebuffer(0, 0, fx->post_width, fx->post_height, 0, 0, fx->post_width,
                              fx->post_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            check_gl_error("postfx taa");
            profiler_scope_end(fx->profiler);
        } else {
            // Bilinear magnify onto the post canvas: the fallback when TAA is
            // toggled off at a reduced scale -- nothing else would bring the
            // frame to display size. At full scale the canvas IS hdr_fbo and
            // there is nothing to bring anywhere.
            if (post_canvas) {
                profiler_scope_begin(fx->profiler, "seam magnify");
                glBindFramebuffer(GL_READ_FRAMEBUFFER, fx->hdr_fbo);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fx->post_fbo);
                glBlitFramebuffer(0, 0, fx->width, fx->height, 0, 0, fx->post_width,
                                  fx->post_height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                check_gl_error("postfx taau seam");
                profiler_scope_end(fx->profiler);
            }
            fx->taa_history.valid = false;
        }

        // SSGI denoise chain: temporal accumulation of the raw gather (TAA
        // frames only -- it needs velocity and the per-frame slice jitter),
        // then an edge-aware a-trous blur. SVGF-style order: accumulate the
        // raw signal, smooth the accumulation.
        GLuint gi_result_tex = fx->ssgi_gi_texture;
        if (ssgi_active) {
            profiler_scope_begin(fx->profiler, "ssgi denoise");
            if (taa_resolving) {
                gi_result_tex =
                    run_temporal_accum(fx, fx->ssgi_accum_program, &fx->ssgi_history,
                                       fx->half_width, fx->half_height, fx->ssgi_gi_texture,
                                       TEMPORAL_FEEDBACK_DEFAULT);
                gi_accum_ran = true;
            }

            // Three a-trous iterations with doubling tap spacing (1, 2, 4).
            gi_result_tex =
                run_atrous(fx, fx->ssgi_atrous_program, &fx->ssgi_atrous, fx->half_width,
                           fx->half_height, gi_result_tex, have_normals);
            check_gl_error("postfx ssgi denoise");
            profiler_scope_end(fx->profiler);
        }
        if (!gi_accum_ran)
            fx->ssgi_history.valid = false;

        if (ssr_active) {
            profiler_scope_begin(fx->profiler, "ssr");
            postfx_run_ssr(fx, canvas_fbo, canvas_tex, have_normals, taa_resolving, projection,
                           inv_projection, view);
            profiler_scope_end(fx->profiler);
        }

        if (ssgi_active && albedo_written) {
            profiler_scope_begin(fx->profiler, "ssgi composite");
            // Add one bounce of indirect diffuse (albedo x gathered GI x intensity)
            // into the canvas before bloom, so bounce light blooms like direct
            // light. Half-res GI is bilinear-upsampled; albedo is render-res.
            glBindFramebuffer(GL_FRAMEBUFFER, canvas_fbo);
            glViewport(0, 0, fx->post_width, fx->post_height);
            glUseProgram(fx->ssgi_composite_program->id);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, gi_result_tex); // accumulated + denoised
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, fx->albedo_texture);
            uniform_set_float(fx->ssgi_composite_program->uniforms, "intensity",
                              fx->ssgi_intensity);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            draw_fullscreen_quad(fx->quad_vao);
            glDisable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            check_gl_error("postfx ssgi composite");
            profiler_scope_end(fx->profiler);
        }

        postfx_run_atmosphere(fx, canvas_fbo, aux_written, taa_resolving, projection, view,
                              writes);

        // Separable SSS: blur the resolved skin-diffuse buffer and fold
        // blur - diffuse into the canvas, softening diffuse while specular
        // stays sharp. Runs on the composited scene, before motion blur / DoF.
        // Skipped when SSS is off (attachment 4 unwritten); a no-op (adds 0) on
        // scenes with no skin material.
        if (sss_written && fx->sss_ready) {
            profiler_scope_begin(fx->profiler, "sss");
            postfx_run_sss(fx, canvas_fbo, projection, taa_resolving);
            profiler_scope_end(fx->profiler);
        }

        // Motion blur (4.15): velocity-driven blur on the linear HDR scene,
        // blitted back into the canvas so DoF/bloom/tonemap see it. Needs the
        // aux velocity buffer; skipped (frame untouched) when off or unavailable.
        if (fx->motion_blur_enabled && aux_written && postfx_ensure_motion_blur_targets(fx)) {
            profiler_scope_begin(fx->profiler, "motion blur");
            postfx_run_motion_blur(fx, canvas_fbo, canvas_tex);
            profiler_scope_end(fx->profiler);
        }

        // Depth of field replaces the scene that bloom and tone mapping read.
        // scene_tex is the sharp canvas unless DoF ran into fx->dof_texture.
        GLuint scene_tex = canvas_tex;
        if (dof_active && postfx_ensure_dof_targets(fx)) {
            profiler_scope_begin(fx->profiler, "dof");
            postfx_run_dof(fx, canvas_tex, projection);
            profiler_scope_end(fx->profiler);
            scene_tex = fx->dof_texture;
        }

        /*
         * The frame's luminance, and it now has TWO consumers with different
         * needs -- which is why the draws and the readback are separate calls.
         *
         * Auto-exposure wants the VALUE: the adaptation half (the blend toward
         * it, and the deadband) is exposure.c's, because the number has to reach
         * the CPU to be blended at all. That split replaced a second fullscreen
         * pass blending into a 1x1 ping-pong pair, purely so the tonemap could
         * sample the result; reading the value directly deleted that pass, its
         * shader, the ping-pong and its validity flag.
         *
         * The Purkinje shift wants the TEXTURE, which it samples on unit 7 --
         * so the 1x1 has a GPU reader again, and the ping-pong's descendant is
         * back in a smaller form. It needs the draws under a PINNED exposure
         * too, where auto-exposure runs nothing at all, so the two gates below
         * are deliberately different.
         */
        const bool metering = fx->exposure && fx->exposure->automatic;
        if (fx->exposure && (fx->exposure->automatic || fx->purkinje_enabled)) {
            profiler_scope_begin(fx->profiler, "luminance measure");
            postfx_measure_luminance(fx, scene_tex);
            profiler_scope_end(fx->profiler);
        }
        if (metering) {
            // Its own scope, and the number it reports is not comparable with
            // the others: the blocking read inside drains the pipeline, so this
            // row carries the whole frame's outstanding GPU work rather than the
            // cost of the three draws above.
            profiler_scope_begin(fx->profiler, "exposure read (drains)");
            float measured = postfx_read_luminance(fx);
            exposure_submit_measurement(fx->exposure, measured);
            // After the submit, so the report carries this frame's adapted value
            // rather than last frame's. `measured` is passed in because the
            // blend consumes it and nothing keeps the raw reading.
            if (fx->exposure->probe)
                exposure_probe_report(fx->exposure, measured, fx->frame_index);
            profiler_scope_end(fx->profiler);
        } else {
            // Drop the history, or re-enabling auto-exposure would pre-expose by
            // a value metered under whatever was on screen before.
            exposure_reset_adaptation(fx->exposure);
        }

        // The pyramid is the frame's thresholded bright image, not bloom's
        // private output, so it is built whenever a consumer wants one. Running
        // it with bloomEnabled 0 at the composite costs a pass and changes
        // nothing visible -- which is what lets the flare stand on its own
        // instead of carrying "needs bloom" through the CLI, the GUI, two
        // headers and a shader.
        const bool flare_wanted = fx->flare_enabled && fx->flare_strength > 0.0f;
        bool flare_active = false;
        if (fx->bloom_enabled || flare_wanted) {
            profiler_scope_begin(fx->profiler, "bloom pyramid");
            postfx_run_bloom(fx, scene_tex);
            profiler_scope_end(fx->profiler);
            if (flare_wanted)
                flare_active = postfx_run_flare(fx);
        }

        // Composite + tone map into the target framebuffer. The quad runs at
        // the display size while sampling the supersampled HDR texture, so each
        // output pixel linearly averages its 2x2 source block (the SSAA
        // resolve). Tone mapping the averaged linear radiance is correct; a 1:1
        // pass-through when supersampling is off.
        profiler_scope_begin(fx->profiler, "tonemap + finishing");
        glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
        glViewport(0, 0, fx->out_width, fx->out_height);
        glUseProgram(fx->tonemap_program->id);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, scene_tex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, fx->bloom_texture); // magnified -> level 0
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, ao_result_tex); // always the BLURRED buffer, at half res
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
        // The metered 1x1, back on the unit its own ancestor held. That slot
        // carried an adapted-luminance texture until the tonemap stopped
        // applying exposure; the Purkinje shift wants a frame luminance again,
        // so it reads the meter's own output rather than reviving a second
        // reduction beside it. Bound whenever a meter exists -- the shader is
        // told separately whether it may be trusted, since an unwritten R32F
        // target has undefined content.
        glBindTexture(GL_TEXTURE_2D, fx->exposure ? fx->lum_reduce_texture : 0);
        glActiveTexture(GL_TEXTURE8); // lens flare (unit 8 was the retired fog debug buffer)
        glBindTexture(GL_TEXTURE_2D, flare_active ? fx->flare_texture : 0);
        glActiveTexture(GL_TEXTURE9);
        glBindTexture(GL_TEXTURE_2D,
                      aux_written ? fx->aux_texture : 0); // linZ + roughness (spec-occ)
        glActiveTexture(GL_TEXTURE10);
        glBindTexture(GL_TEXTURE_2D, cs_active ? cs_result_tex : 0); // contact-shadow visibility
        glActiveTexture(GL_TEXTURE11);
        glBindTexture(GL_TEXTURE_3D, fx->lut_texture); // 0 when no table is loaded
        glActiveTexture(GL_TEXTURE12);
        glBindTexture(GL_TEXTURE_2D, fx->spec_occ_ready ? fx->spec_occ_texture : 0);
        UniformManager* tm = fx->tonemap_program->uniforms;
        uniform_set_float(tm, "bloomStrength", fx->bloom_strength);
        uniform_set_int(tm, "bloomEnabled", fx->bloom_enabled ? 1 : 0);
        uniform_set_int(tm, "flareTex", 8);
        uniform_set_float(tm, "flareStrength", fx->flare_strength);
        uniform_set_int(tm, "flareEnabled", flare_active ? 1 : 0);
        // False when the split composite already applied AO this frame --
        // split_live owns the handoff, the shader never re-derives it.
        uniform_set_int(tm, "aoEnabled", fx->ssao_enabled && !split_live ? 1 : 0);
        uniform_set_float(tm, "aoStrength", fx->ssao_strength);
        // Specular occlusion keeps GTAO off reflections. Needs the aux buffer
        // (linZ + roughness) and the normals; both ride the same AO-on gating,
        // so require them here too. Metallic is opportunistic (SSGI's albedo).
        const bool spec_occ_active = fx->spec_occlusion_mode != POSTFX_SPEC_OCC_OFF &&
                                     aux_written && have_normals;
        uniform_set_int(tm, "specOccMode", spec_occ_active ? fx->spec_occlusion_mode : 0);
        uniform_set_int(tm, "specOccHasMetallic", albedo_written ? 1 : 0);
        const float inv_focal[2] = {1.0f / projection[0][0], 1.0f / projection[1][1]};
        uniform_set_vec2(tm, "invFocal", inv_focal);
        // See the composite's copy: uploaded per draw because the two consumers
        // magnify this buffer by different factors.
        const float ao_res[2] = {(float)fx->half_width, (float)fx->half_height};
        uniform_set_vec2(tm, "aoRes", ao_res);
        // Contact shadows multiply the direct-light term beside AO. The exact
        // form 1 - strength*(1 - cs) is identity at cs == 1, so a lit frame is
        // byte-identical to the feature off.
        uniform_set_int(tm, "csEnabled", cs_active ? 1 : 0);
        uniform_set_float(tm, "csStrength", fx->cs_strength);
        // Suppress debug views whose source buffer was not produced, and
        // say so once per requested view rather than silently every frame
        PostFXDebugView debug_view = fx->debug_view;
        if ((debug_view == POSTFX_DEBUG_AO && !fx->ssao_enabled) ||
            (debug_view == POSTFX_DEBUG_NORMALS && !have_normals) ||
            (debug_view == POSTFX_DEBUG_SSR && !ssr_active) ||
            (debug_view == POSTFX_DEBUG_ALBEDO && !albedo_written) ||
            (debug_view == POSTFX_DEBUG_SSGI && !ssgi_active) ||
            (debug_view == POSTFX_DEBUG_SPEC_OCC && !spec_occ_active) ||
            (debug_view == POSTFX_DEBUG_BENT && !fx->ssao_enabled) ||
            (debug_view == POSTFX_DEBUG_CONTACT && !cs_active)) {
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
        uniform_set_int(tm, "caEnabled", fx->ca_enabled ? 1 : 0);
        uniform_set_float(tm, "caStrength", fx->ca_strength);
        uniform_set_int(tm, "purkinjeEnabled", fx->purkinje_enabled ? 1 : 0);
        uniform_set_float(tm, "purkinjeStrength", fx->purkinje_strength);
        uniform_set_float(tm, "purkinjeBiasEV", fx->purkinje_bias_ev);
        // Whether unit 7 holds a value at all. The bind above puts the metering
        // 1x1 there whenever an Exposure exists, but its CONTENT is only defined
        // once the measure draws have run -- which they do under the same
        // condition. Told rather than inferred: the shader cannot see either.
        uniform_set_int(tm, "purkinjeHasMeter",
                        (fx->exposure && (fx->exposure->automatic || fx->purkinje_enabled)) ? 1
                                                                                            : 0);
        uniform_set_int(tm, "grainEnabled", fx->grain_enabled ? 1 : 0);
        uniform_set_float(tm, "grainStrength", fx->grain_strength);
        // % 4096: same float-hash conditioning bound as PCSS/SSR
        uniform_set_float(tm, "grainSeed", (float)(fx->frame_index % 4096));
        // The loaded texture IS the enable: there is no second flag that could
        // disagree with it, so a failed load cannot leave the branch sampling
        // unit 11 with nothing bound to it.
        uniform_set_int(tm, "lutEnabled", fx->lut_texture ? 1 : 0);
        uniform_set_float(tm, "lutSize", (float)fx->lut_size);
        uniform_set_float(tm, "lutStrength", fx->lut_strength);
        uniform_set_int(tm, "lutInterp", (int)fx->lut_interp);
        // No frame term here, deliberately -- see the shader's dither block.
        uniform_set_int(tm, "ditherEnabled", fx->dither_enabled ? 1 : 0);
        uniform_set_float(tm, "ditherStrength", fx->dither_strength);
        draw_fullscreen_quad(fx->quad_vao);
        profiler_scope_end(fx->profiler);

        glUseProgram(0);
        glActiveTexture(GL_TEXTURE0);
    }

    if (depth_was_on)
        glEnable(GL_DEPTH_TEST);
    if (blend_was_on)
        glEnable(GL_BLEND);
}
