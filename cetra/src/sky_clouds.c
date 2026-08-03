#include <math.h>
#include <stdlib.h>

#include "sky.h" // pulls GL/glew.h, which must precede GLFW's GL header

#include <GLFW/glfw3.h>

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

// Bake one field across worker threads. The perm table is read-only by the
// time threads start (initialized on this thread), so no synchronization is
// needed beyond the joins.
static void bake_field(unsigned char* out, const NoisePerm* perm, int size, int detail) {
    int workers = get_cpu_cores();
    if (workers > 8)
        workers = 8;
    if (workers > size)
        workers = size;
    if (workers < 1)
        workers = 1;

    CloudBakeSlab slabs[8];
    cetra_thread_t threads[8];
    bool running[8] = {false};
    int per = (size + workers - 1) / workers;
    for (int i = 0; i < workers; i++) {
        int z0 = i * per;
        int z1 = z0 + per > size ? size : z0 + per;
        if (z0 >= z1)
            break;
        slabs[i] = (CloudBakeSlab){out, perm, size, z0, z1, detail};
        running[i] = cetra_thread_create(&threads[i], cloud_bake_worker, &slabs[i]);
        if (!running[i])
            cloud_bake_worker(&slabs[i]); // could not start: bake the slab inline
    }
    for (int i = 0; i < workers; i++) {
        if (running[i])
            cetra_thread_join(threads[i]);
    }
}

int sky_bake_cloud_noise(SkyAtmosphere* sky) {
    if (!sky || !sky->clouds.enabled)
        return 0;
    if (sky->clouds.noise_baked)
        return 0;

    double t0 = glfwGetTime();

    NoisePerm perm;
    noise_perm_init(&perm, CLOUD_NOISE_SEED);

    const int ss = SKY_CLOUD_SHAPE_SIZE;
    const int ds = SKY_CLOUD_DETAIL_SIZE;
    unsigned char* shape = malloc((size_t)ss * ss * ss * 4);
    unsigned char* detail = malloc((size_t)ds * ds * ds * 4);
    if (!shape || !detail) {
        log_error("Cloud noise bake: allocation failed");
        free(shape);
        free(detail);
        return -1;
    }

    bake_field(shape, &perm, ss, 0);
    bake_field(detail, &perm, ds, 1);

    double t1 = glfwGetTime();

    sky->clouds.shape_tex = create_texture_3d_rgba8_tiling(ss, ss, ss, shape);
    sky->clouds.detail_tex = create_texture_3d_rgba8_tiling(ds, ds, ds, detail);
    free(shape);
    free(detail);

    double t2 = glfwGetTime();
    sky->clouds.noise_baked = true;
    log_info("Cloud noise baked: fields %.1f ms (%d threads), upload+mips %.1f ms",
             (t1 - t0) * 1000.0, get_cpu_cores() > 8 ? 8 : get_cpu_cores(), (t2 - t1) * 1000.0);
    return 0;
}

// Lazy half-internal-res march ping-pong. One-shot like the aerial volume:
// a failed allocation logs and disables rather than retrying every frame.
static bool ensure_march_targets(CloudLayer* c, int w, int h) {
    if (c->march_tex[0] && c->march_w == w && c->march_h == h)
        return true;
    for (int i = 0; i < 2; i++) {
        if (c->march_tex[i]) {
            glDeleteTextures(1, &c->march_tex[i]);
            glDeleteFramebuffers(1, &c->march_fbo[i]);
            c->march_tex[i] = 0;
            c->march_fbo[i] = 0;
        }
    }
    for (int i = 0; i < 2; i++) {
        glGenFramebuffers(1, &c->march_fbo[i]);
        glGenTextures(1, &c->march_tex[i]);
        glBindTexture(GL_TEXTURE_2D, c->march_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, c->march_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               c->march_tex[i], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            log_error("Cloud march FBO incomplete; clouds disabled");
            for (int j = 0; j <= i; j++) {
                glDeleteTextures(1, &c->march_tex[j]);
                glDeleteFramebuffers(1, &c->march_fbo[j]);
                c->march_tex[j] = 0;
                c->march_fbo[j] = 0;
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
    vec3 cam_km = {inv_view[3][0] / units, inv_view[3][1] / units, inv_view[3][2] / units};

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
    uniform_set_vec3(um, "camPosKm", cam_km);
    uniform_set_vec3(um, "sunDir", sky->sun_dir);
    uniform_set_float(um, "coverage", c->coverage);
    uniform_set_float(um, "cloudType", c->cloud_type);
    uniform_set_float(um, "densityScale", c->density);
    uniform_set_int(um, "steps", 64);
    uniform_set_int(um, "lightSteps", 6);
    uniform_set_int(um, "debugShell", c->debug_shell);
    uniform_set_int(um, "temporal", temporal);
    uniform_set_int(um, "frameIndex", frame % 4096);
    uniform_set_mat4(um, "prevView", (float*)c->prev_view);
    uniform_set_vec2(um, "prevFocal", c->prev_focal);

    // Drift: fixed-step headless (deterministic goldens), real delta live
    c->scroll += engine->headless ? (1.0 / 60.0) : engine->render_delta;
    float wind_kms = c->wind_speed_kmh / 3600.0f;
    float wind_rad = c->wind_dir_deg * 0.01745329f;
    vec3 wind_off = {sinf(wind_rad) * wind_kms * (float)c->scroll, 0.0f,
                     cosf(wind_rad) * wind_kms * (float)c->scroll};
    uniform_set_vec3(um, "windOffsetKm", wind_off);

    draw_fullscreen_quad(sky->quad_vao);
    c->march_valid = true;
    c->march_read = write;

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
