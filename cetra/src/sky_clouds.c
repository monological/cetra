#include <stdlib.h>

#include "sky.h" // pulls GL/glew.h, which must precede GLFW's GL header

#include <GLFW/glfw3.h>

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
