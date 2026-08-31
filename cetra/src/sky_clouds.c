#include <math.h>
#include <stdlib.h>

#include "sky.h" // pulls GL/glew.h, which must precede GLFW's GL header

#include <GLFW/glfw3.h>

#include "cook.h"
#include "engine.h"
#include "noise.h"
#include "texture.h"
#include "thread.h"
#include "util.h"
#include "ext/log.h"

// CPU bake of the cloud noise fields (spec 11.0). Every voxel is a pure
// function of (coordinate, fixed seed) through the local-table/stateless
// noise entry points, so the bake threads over disjoint Z-slabs and the
// output bytes are identical at any thread count -- which is what lets the
// cloud goldens stay byte-deterministic across machines.
//
// The seed is fixed here, not configurable: cloud noise is an engine field
// like the LTC tables, not authored content. Coverage/type/density shape the
// FIELD at march time; the field itself never changes.
#define CLOUD_NOISE_SEED 0xC10DD5EEu

// Inverted Worley: feature points become puffs. noise_worley3's F1 distance
// runs ~[0, 1.2] cell units; the clamp folds the far tail.
static float worley_puff(float x, float y, float z, int period, unsigned int seed) {
    float d = noise_worley3(x, y, z, period, seed);
    d = d > 1.0f ? 1.0f : d;
    return 1.0f - d;
}

// Schneider's remap, the workhorse of the whole recipe.
static float remap01(float v, float lo, float hi) {
    float t = (v - lo) / (hi - lo);
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

typedef struct CloudBakeSlab {
    unsigned char* out; // the whole field; this worker writes only its slabs
    const NoisePerm* perm;
    int size;
    int z_begin, z_end;
    int detail; // 0 = the 128^3 shape recipe, 1 = the 32^3 detail recipe
} CloudBakeSlab;

// Shape voxel: R = Perlin-Worley (Perlin fbm carved by Worley so coverage
// keeps cauliflower edges instead of fog banks), GBA = inverted-Worley
// octaves the marcher combines into an erosion fbm.
static void bake_shape_voxel(const NoisePerm* perm, float nx, float ny, float nz,
                             unsigned char* px) {
    // Tiled Perlin fbm: base period 8, five octaves, period doubling with
    // frequency so every octave still tiles.
    float p = 0.0f;
    float amp = 0.5f;
    int period = 8;
    for (int o = 0; o < 5; o++) {
        p += amp * noise_perlin3_tiled(perm, nx * (float)period, ny * (float)period,
                                       nz * (float)period, period);
        amp *= 0.5f;
        period *= 2;
    }
    float perlin = p * 0.5f + 0.5f; // ~[-1,1] -> [0,1]

    float w4 = worley_puff(nx * 4.0f, ny * 4.0f, nz * 4.0f, 4, CLOUD_NOISE_SEED);
    float w8 = worley_puff(nx * 8.0f, ny * 8.0f, nz * 8.0f, 8, CLOUD_NOISE_SEED + 1u);
    float w16 = worley_puff(nx * 16.0f, ny * 16.0f, nz * 16.0f, 16, CLOUD_NOISE_SEED + 2u);
    float wfbm = w4 * 0.625f + w8 * 0.25f + w16 * 0.125f;

    float pw = remap01(perlin, wfbm - 1.0f, 1.0f);

    px[0] = (unsigned char)(pw * 255.0f + 0.5f);
    px[1] = (unsigned char)(w4 * 255.0f + 0.5f);
    px[2] = (unsigned char)(w8 * 255.0f + 0.5f);
    px[3] = (unsigned char)(w16 * 255.0f + 0.5f);
}

// Detail voxel: three finer Worley octaves; the marcher erodes low-density
// cloud edges with their fbm. Alpha unused.
static void bake_detail_voxel(float nx, float ny, float nz, unsigned char* px) {
    float w2 = worley_puff(nx * 2.0f, ny * 2.0f, nz * 2.0f, 2, CLOUD_NOISE_SEED + 3u);
    float w4 = worley_puff(nx * 4.0f, ny * 4.0f, nz * 4.0f, 4, CLOUD_NOISE_SEED + 4u);
    float w8 = worley_puff(nx * 8.0f, ny * 8.0f, nz * 8.0f, 8, CLOUD_NOISE_SEED + 5u);
    px[0] = (unsigned char)(w2 * 255.0f + 0.5f);
    px[1] = (unsigned char)(w4 * 255.0f + 0.5f);
    px[2] = (unsigned char)(w8 * 255.0f + 0.5f);
    px[3] = 255;
}

static void* cloud_bake_worker(void* arg) {
    CloudBakeSlab* slab = (CloudBakeSlab*)arg;
    int n = slab->size;
    float inv = 1.0f / (float)n;
    for (int z = slab->z_begin; z < slab->z_end; z++) {
        float nz = ((float)z + 0.5f) * inv;
        for (int y = 0; y < n; y++) {
            float ny = ((float)y + 0.5f) * inv;
            unsigned char* row = slab->out + ((size_t)z * n + y) * n * 4;
            for (int x = 0; x < n; x++) {
                float nx = ((float)x + 0.5f) * inv;
                if (slab->detail)
                    bake_detail_voxel(nx, ny, nz, row + (size_t)x * 4);
                else
                    bake_shape_voxel(slab->perm, nx, ny, nz, row + (size_t)x * 4);
            }
        }
    }
    return NULL;
}

// Bake one field across worker threads; returns the worker count so the log
// reports the policy actually applied. The perm table is read-only by the
// time threads start (initialized on this thread), so no synchronization is
// needed beyond the joins -- which is the property cetra_bake_bands is written
// around, and why the band split now lives there rather than here.
static void cloud_bake_band(void* ctx, int z0, int z1) {
    CloudBakeSlab slab = *(CloudBakeSlab*)ctx;
    slab.z_begin = z0;
    slab.z_end = z1;
    cloud_bake_worker(&slab);
}

static int bake_field(unsigned char* out, const NoisePerm* perm, int size, int detail) {
    int workers = cetra_bake_workers(0, size);
    CloudBakeSlab slab = {out, perm, size, 0, size, detail};
    cetra_bake_bands(size, workers, cloud_bake_band, &slab);
    return workers;
}

int sky_bake_cloud_noise(SkyAtmosphere* sky) {
    if (!sky || !sky->clouds.enabled)
        return 0;
    if (sky->clouds.noise_baked)
        return 0;

    double t0 = glfwGetTime();

    const int ss = SKY_CLOUD_SHAPE_SIZE;
    const int ds = SKY_CLOUD_DETAIL_SIZE;
    const size_t shape_bytes = (size_t)ss * ss * ss * 4;
    const size_t detail_bytes = (size_t)ds * ds * ds * 4;

    // The seed is an engine constant, so the sizes are the whole cook key
    // (spec 11.99). One artefact, two sections: the fields hit or bake as one.
    CookKey ck = cook_key("cloud-noise/1");
    cook_key_i32(&ck, ss);
    cook_key_i32(&ck, ds);
    cook_key_u32(&ck, CLOUD_NOISE_SEED);

    unsigned char* shape = NULL;
    unsigned char* detail = NULL;
    int workers = 0;
    CookBlob fields[2];
    if (cook_fetch(&ck, fields, 2)) {
        shape = fields[0].data;
        detail = fields[1].data;
    } else {
        NoisePerm perm;
        noise_perm_init(&perm, CLOUD_NOISE_SEED);
        shape = malloc(shape_bytes);
        detail = malloc(detail_bytes);
        if (!shape || !detail) {
            log_error("Cloud noise bake: allocation failed");
            free(shape);
            free(detail);
            return -1;
        }
        workers = bake_field(shape, &perm, ss, 0);
        bake_field(detail, &perm, ds, 1);
        CookBlob out[2] = {{shape, shape_bytes}, {detail, detail_bytes}};
        cook_store(&ck, out, 2);
    }

    double t1 = glfwGetTime();

    sky->clouds.shape_tex = create_texture_3d_rgba8_tiling(ss, ss, ss, shape);
    sky->clouds.detail_tex = create_texture_3d_rgba8_tiling(ds, ds, ds, detail);
    free(shape);
    free(detail);

    double t2 = glfwGetTime();
    sky->clouds.noise_baked = true;
    log_info("Cloud noise baked: fields %.1f ms (%d threads), upload+mips %.1f ms",
             (t1 - t0) * 1000.0, workers, (t2 - t1) * 1000.0);
    return 0;
}

// Lazy half-internal-res march ping-pong. One-shot like the aerial volume:
// a failed allocation logs and disables rather than retrying every frame.
static bool ensure_march_targets(CloudLayer* c, int w, int h) {
    if (c->march_tex[0] && c->march_w == w && c->march_h == h)
        return true;
    for (int i = 0; i < 2; i++) {
        gl_delete_texture(&c->march_tex[i]);
        gl_delete_fbo(&c->march_fbo[i]);
    }
    // The FBO wrapping duplicates postfx.c's create_color_fbo, which is
    // static there and postfx-owned; hoisting it to a shared home was
    // deliberately not done here to keep this branch's gates narrow (the
    // bake_aerial_volume precedent, sky.c). The texture half reuses the
    // shared LUT helper so the sampler state cannot drift.
    for (int i = 0; i < 2; i++) {
        glGenFramebuffers(1, &c->march_fbo[i]);
        c->march_tex[i] = create_texture_2d_float(w, h, GL_RGBA16F, GL_RGBA, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, c->march_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               c->march_tex[i], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            log_error("Cloud march FBO incomplete; clouds disabled");
            for (int j = 0; j <= i; j++) {
                gl_delete_texture(&c->march_tex[j]);
                gl_delete_fbo(&c->march_fbo[j]);
            }
            c->enabled = false;
            return false;
        }
    }
    c->march_w = w;
    c->march_h = h;
    c->prev_frame = -1; // fresh targets carry no history
    return true;
}

// Side of the sun-transmittance map. Deliberately coarse: it is read through a trilinear
// volume lookup and then averaged again by the froxel accumulator, so detail finer than a
// cloud's own silhouette is discarded twice over -- the argument POSTFX_FOG_ESM_SIZE makes for
// the fog's own cascades. 256 texels over an 8 km tile is ~31 m, finer than any edge a deck
// 1.5 km up can cast.
#define CLOUD_SHADOW_SIZE 256
// Mirrors include/clouds.glsl, which owns the shell geometry and the noise period. Only the
// published altitude and tile size need them on this side, and a drift between the two is a
// shadow in the wrong place rather than a compile error -- hence named here rather than
// spelled twice.
#define CLOUD_SHADOW_SHELL_KM 1.5f
#define CLOUD_SHADOW_TILE_KM 8.0f

static bool ensure_shadow_target(CloudLayer* c) {
    if (c->shadow_tex)
        return true;
    glGenFramebuffers(1, &c->shadow_fbo);
    c->shadow_tex =
        create_texture_2d_float(CLOUD_SHADOW_SIZE, CLOUD_SHADOW_SIZE, GL_R16F, GL_RED, NULL);
    // REPEAT, which is the whole trick: the map holds one period of a field that is periodic
    // over the same distance, so wrapping it covers the world exactly rather than approximately.
    // The shared helper defaults to CLAMP_TO_EDGE, which would smear the tile's last texel
    // across everything beyond it.
    glBindTexture(GL_TEXTURE_2D, c->shadow_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, c->shadow_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->shadow_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Cloud shadow FBO incomplete; cloud shadows disabled");
        gl_delete_texture(&c->shadow_tex);
        gl_delete_fbo(&c->shadow_fbo);
        c->shadows_enabled = false;
        return false;
    }
    return true;
}

// Sun transmittance through the deck, into one world-anchored tile of it.
//
// Called from the march with the march's OWN wind offset, which is the whole reason it lives
// here: the drift clock advances once per frame, so a pass computing its own offset would sit
// one frame away from the deck it shadows.
static void build_cloud_shadow(SkyAtmosphere* sky, struct Engine* engine, CloudLayer* c,
                               float units, const vec3 wind_off) {
    if (!c->shadows_enabled)
        return;
    // Before the target, so a missing program cannot orphan a texture and an FBO.
    if (!c->shadow_program) {
        c->shadow_program = get_engine_shader_program_by_name(engine, "cloud_shadow");
        if (!c->shadow_program) {
            log_error("No cloud_shadow program; cloud shadows disabled");
            c->shadows_enabled = false;
            return;
        }
    }
    if (!ensure_shadow_target(c))
        return;

    // One noise tile, world-locked at the origin, wrapped by the sampler. No window to follow
    // the camera and no snapping to keep it from crawling: with detail off the field is exactly
    // periodic over this distance, so the tile IS the whole world.
    c->shadow_tile = CLOUD_SHADOW_TILE_KM * units;
    c->shadow_shell_y = CLOUD_SHADOW_SHELL_KM * units;

    // Everything the march reads. Camera-free by construction, so an unchanged set means a
    // bit-identical texture and the whole pass can be skipped -- which is the DEFAULT case,
    // since wind is still unless an app asks for it. 1.57M density taps either way.
    const float inputs[8] = {sky->sun_dir[0], sky->sun_dir[1], sky->sun_dir[2], c->coverage,
                             c->cloud_type,   c->density,      wind_off[0],    wind_off[2]};
    if (c->shadow_built && memcmp(inputs, c->shadow_inputs, sizeof(inputs)) == 0)
        return;
    memcpy(c->shadow_inputs, inputs, sizeof(inputs));
    c->shadow_built = true;

    glBindFramebuffer(GL_FRAMEBUFFER, c->shadow_fbo);
    glViewport(0, 0, CLOUD_SHADOW_SIZE, CLOUD_SHADOW_SIZE);
    glUseProgram(c->shadow_program->id);
    UniformManager* um = c->shadow_program->uniforms;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, c->shape_tex);
    uniform_set_int(um, "shapeTex", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, c->detail_tex);
    uniform_set_int(um, "detailTex", 1);
    glActiveTexture(GL_TEXTURE0);

    uniform_set_vec3(um, "sunDir", sky->sun_dir);
    uniform_set_float(um, "coverage", c->coverage);
    uniform_set_float(um, "cloudType", c->cloud_type);
    uniform_set_float(um, "densityScale", c->density);
    uniform_set_vec3(um, "windOffsetKm", (float*)wind_off);

    draw_fullscreen_quad(sky->quad_vao);
}

void sky_clouds_march(SkyAtmosphere* sky, struct Engine* engine, mat4 view, mat4 projection) {
    if (!sky || !engine || !sky->enabled || !sky->clouds.enabled || !sky->clouds.noise_baked ||
        !sky->luts_baked || !sky->sky_view_lut)
        return;
    CloudLayer* c = &sky->clouds;

    if (!c->march_program) {
        c->march_program = get_engine_shader_program_by_name(engine, "cloud_march");
        if (!c->march_program) {
            log_error("No cloud_march program; clouds disabled");
            c->enabled = false;
            return;
        }
    }

    int rw = 0, rh = 0;
    engine_render_size(engine, &rw, &rh);
    if (!ensure_march_targets(c, rw > 1 ? rw / 2 : 1, rh > 1 ? rh / 2 : 1))
        return;

    // Parity ping-pong + adjacency stamp: history is only trusted when the
    // previous march was exactly last frame (a skipped frame resets rather
    // than blending stale sky).
    int frame = (int)(engine->total_frames);
    int write = frame & 1;
    int temporal = (c->prev_frame >= 0 && c->prev_frame == frame - 1) ? 1 : 0;

    GLint prev_viewport[4];
    GLint prev_framebuffer;
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_framebuffer);
    GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend_was = glIsEnabled(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    mat4 inv_view;
    glm_mat4_inv(view, inv_view);
    float units = sky->world_units_per_km > 0.0f ? sky->world_units_per_km : 1000.0f;
    float cam_alt_km = inv_view[3][1] / units;

    glBindFramebuffer(GL_FRAMEBUFFER, c->march_fbo[write]);
    glViewport(0, 0, c->march_w, c->march_h);
    glUseProgram(c->march_program->id);
    UniformManager* um = c->march_program->uniforms;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, c->shape_tex);
    uniform_set_int(um, "shapeTex", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, c->detail_tex);
    uniform_set_int(um, "detailTex", 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, sky->transmittance_lut);
    uniform_set_int(um, "transmittanceLut", 2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, sky->sky_view_lut);
    uniform_set_int(um, "skyViewLut", 3);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, c->march_tex[write ^ 1]);
    uniform_set_int(um, "historyTex", 4);
    glActiveTexture(GL_TEXTURE0);

    uniform_set_mat4(um, "invView", (float*)inv_view);
    const float inv_focal[2] = {1.0f / projection[0][0], 1.0f / projection[1][1]};
    uniform_set_vec2(um, "invFocal", (float*)inv_focal);
    uniform_set_float(um, "camAltKm", cam_alt_km);
    uniform_set_vec3(um, "sunDir", sky->sun_dir);
    uniform_set_float(um, "coverage", c->coverage);
    uniform_set_float(um, "cloudType", c->cloud_type);
    uniform_set_float(um, "densityScale", c->density);
    uniform_set_int(um, "temporal", temporal);
    uniform_set_int(um, "frameIndex", frame % 4096);
    uniform_set_mat4(um, "prevView", (float*)c->prev_view);
    uniform_set_vec2(um, "prevFocal", c->prev_focal);

    // Drift rides the frame clock: fixed-step headless via the producer, real
    // delta live, and honestly frozen when an embedder's clock is paused
    c->scroll += engine->render_delta;
    float wind_kms = c->wind_speed_kmh / 3600.0f;
    float wind_rad = c->wind_dir_deg * 0.01745329f;
    vec3 wind_off = {sinf(wind_rad) * wind_kms * (float)c->scroll, 0.0f,
                     cosf(wind_rad) * wind_kms * (float)c->scroll};
    uniform_set_vec3(um, "windOffsetKm", wind_off);

    draw_fullscreen_quad(sky->quad_vao);

    // The sun-transmittance map, from the SAME wind offset this march just used. Sharing the
    // offset is the point: the drift clock advanced once, above, so anything recomputing it
    // would shadow a deck one frame from the one on screen.
    build_cloud_shadow(sky, engine, c, units, wind_off);

    // Store this frame's camera for the next march's reprojection:
    // rotation-only view (ray-direction reprojection) + the projection
    // focals, and the adjacency stamp.
    glm_mat4_copy(view, c->prev_view);
    c->prev_view[3][0] = 0.0f;
    c->prev_view[3][1] = 0.0f;
    c->prev_view[3][2] = 0.0f;
    c->prev_focal[0] = projection[0][0];
    c->prev_focal[1] = projection[1][1];
    c->prev_frame = frame;

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_framebuffer);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (depth_was)
        glEnable(GL_DEPTH_TEST);
    if (blend_was)
        glEnable(GL_BLEND);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_3D, 0);
}
