#include "light_cluster.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "ext/log.h"
#include "intersect.h"
#include "light.h"
#include "scene.h"

// Radiance below this reads as black at the project-standard -E 1.0 (one LDR
// LSB); the derived cull radius is where attenuation crosses it.
#define LC_CULL_EPSILON (1.0f / 256.0f)

LightClusterContext* create_light_cluster_context(void) {
    LightClusterContext* ctx = calloc(1, sizeof(LightClusterContext));
    if (!ctx) {
        log_error("Failed to allocate light cluster context");
        return NULL;
    }
    return ctx;
}

void free_light_cluster_context(LightClusterContext* ctx) {
    free(ctx);
}

float light_cull_radius(const struct Light* light) {
    if (light->range > 0.0f)
        return light->range;

    float peak = fmaxf(light->color[0], fmaxf(light->color[1], light->color[2]));
    float i_eff = light->intensity * peak;
    if (i_eff <= 0.0f)
        return 0.0f;

    // Solve constant + linear*d + quadratic*d^2 = i_eff / epsilon for d
    float target = i_eff / LC_CULL_EPSILON;
    float c = light->constant, l = light->linear, q = light->quadratic;
    float r;
    if (q > 1e-8f) {
        float disc = l * l + 4.0f * q * (target - c);
        if (disc <= 0.0f)
            return 0.0f; // never brighter than epsilon
        r = (-l + sqrtf(disc)) / (2.0f * q);
    } else if (l > 1e-8f) {
        r = (target - c) / l;
    } else {
        return -1.0f; // constant-only attenuation: uncullable
    }
    if (r <= 0.0f)
        return 0.0f;

    if (light->type == LIGHT_AREA)
        r += 0.5f * sqrtf(light->size[0] * light->size[0] + light->size[1] * light->size[1]);
    return r;
}

// slice(z) for view depth z, matching lights_ubo.glsl
static int _slice_for_z(float z, float scale, float bias) {
    int s = (int)floorf(log2f(fmaxf(z, 1e-4f)) * scale + bias);
    return s < 0 ? 0 : (s >= LC_CLUSTER_Z ? LC_CLUSTER_Z - 1 : s);
}

static int _clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Conservative screen-tile range of a view-space sphere: project the four
// extreme points at the sphere's closest depth (max magnification over its
// depth range) with the real projection matrix, clamp NDC, map to tiles.
static void _tile_range_for_sphere(mat4 projection, const vec3 view_center, float radius,
                                   float near_clip, LightClusterRange* range) {
    float zc = -view_center[2]; // view depth (camera looks down -Z)

    if (zc - radius <= near_clip) {
        // Sphere crosses or contains the near plane: every tile
        range->x0 = 0;
        range->x1 = LC_CLUSTER_X - 1;
        range->y0 = 0;
        range->y1 = LC_CLUSTER_Y - 1;
        return;
    }

    float zproj = zc - radius;
    float ndc_min_x = 1.0f, ndc_max_x = -1.0f, ndc_min_y = 1.0f, ndc_max_y = -1.0f;
    for (int corner = 0; corner < 4; corner++) {
        vec4 p = {view_center[0] + ((corner & 1) ? radius : -radius),
                  view_center[1] + ((corner & 2) ? radius : -radius), -zproj, 1.0f};
        vec4 clip;
        glm_mat4_mulv(projection, p, clip);
        if (clip[3] <= 1e-6f)
            continue;
        float nx = clip[0] / clip[3];
        float ny = clip[1] / clip[3];
        ndc_min_x = fminf(ndc_min_x, nx);
        ndc_max_x = fmaxf(ndc_max_x, nx);
        ndc_min_y = fminf(ndc_min_y, ny);
        ndc_max_y = fmaxf(ndc_max_y, ny);
    }

    range->x0 = _clampi((int)floorf((ndc_min_x * 0.5f + 0.5f) * LC_CLUSTER_X), 0,
                        LC_CLUSTER_X - 1);
    range->x1 = _clampi((int)floorf((ndc_max_x * 0.5f + 0.5f) * LC_CLUSTER_X), 0,
                        LC_CLUSTER_X - 1);
    range->y0 = _clampi((int)floorf((ndc_min_y * 0.5f + 0.5f) * LC_CLUSTER_Y), 0,
                        LC_CLUSTER_Y - 1);
    range->y1 = _clampi((int)floorf((ndc_max_y * 0.5f + 0.5f) * LC_CLUSTER_Y), 0,
                        LC_CLUSTER_Y - 1);
}

void light_cluster_build_and_upload(LightClusterContext* ctx, struct Engine* engine,
                                    struct Scene* scene, mat4 view, mat4 projection, int fb_width,
                                    int fb_height, float near_clip, float far_clip) {
    if (!ctx || !engine || !scene)
        return;

    memset(&ctx->lights, 0, sizeof(ctx->lights));
    memset(ctx->counts, 0, sizeof(ctx->counts));

    float log_ratio = log2f(far_clip / near_clip);
    float slice_scale = (float)LC_CLUSTER_Z / log_ratio;
    float slice_bias = -(float)LC_CLUSTER_Z * log2f(near_clip) / log_ratio;
    ctx->lights.cluster_params[0] = slice_scale;
    ctx->lights.cluster_params[1] = slice_bias;
    ctx->lights.cluster_params[2] = (float)LC_CLUSTER_X / (float)fb_width;
    ctx->lights.cluster_params[3] = (float)LC_CLUSTER_Y / (float)fb_height;

    // World-space frustum for the sphere pre-cull (same planes the node
    // culling uses)
    mat4 view_proj;
    glm_mat4_mul(projection, view, view_proj);
    Frustum frustum;
    frustum_extract_from_vp(view_proj, &frustum);

    // Gather directional lights (shaded unclustered: they hit every fragment)
    // and clusterable punctual lights, both in scene->lights order so the
    // packing -- and therefore the shading loop order -- is deterministic.
    int num_dir = 0;
    int num_packed = 0;
    for (size_t i = 0; i < scene->light_count; i++) {
        struct Light* light = scene->lights[i];
        if (!light || light->type == LIGHT_UNKNOWN)
            continue;

        if (light->type == LIGHT_DIRECTIONAL) {
            if (num_dir >= LC_MAX_DIR_LIGHTS) {
                if (!ctx->warned_dir_overflow) {
                    log_warn("More than %d directional lights; extras ignored",
                             LC_MAX_DIR_LIGHTS);
                    ctx->warned_dir_overflow = true;
                }
                continue;
            }
            GpuDirLight* dst = &ctx->lights.dir_lights[num_dir];
            dst->dir_shadow[0] = light->direction[0];
            dst->dir_shadow[1] = light->direction[1];
            dst->dir_shadow[2] = light->direction[2];
            dst->dir_shadow[3] = (float)light->shadow_map_index;
            dst->color_intensity[0] = light->color[0] * light->intensity;
            dst->color_intensity[1] = light->color[1] * light->intensity;
            dst->color_intensity[2] = light->color[2] * light->intensity;
            dst->size_misc[0] = light->size[0];
            dst->size_misc[1] = light->size[1];
            num_dir++;
            continue;
        }

        float radius = light_cull_radius(light);
        if (radius == 0.0f)
            continue; // never reaches epsilon
        bool uncullable = radius < 0.0f;
        if (!uncullable && !frustum_test_sphere(&frustum, light->global_position, radius))
            continue;

        if (num_packed >= LC_MAX_CLUSTER_LIGHTS) {
            if (!ctx->warned_packed_overflow) {
                log_warn("More than %d clusterable lights in view; extras ignored",
                         LC_MAX_CLUSTER_LIGHTS);
                ctx->warned_packed_overflow = true;
            }
            break;
        }

        GpuPackedLight* dst = &ctx->lights.cluster_lights[num_packed];
        dst->pos_range[0] = light->global_position[0];
        dst->pos_range[1] = light->global_position[1];
        dst->pos_range[2] = light->global_position[2];
        dst->pos_range[3] = uncullable ? 0.0f : radius;
        dst->dir_type[0] = light->direction[0];
        dst->dir_type[1] = light->direction[1];
        dst->dir_type[2] = light->direction[2];
        dst->dir_type[3] = (float)light->type; // 1 point / 2 spot / 3 area
        dst->color_intensity[0] = light->color[0] * light->intensity;
        dst->color_intensity[1] = light->color[1] * light->intensity;
        dst->color_intensity[2] = light->color[2] * light->intensity;
        dst->atten_cutoff[0] = light->constant;
        dst->atten_cutoff[1] = light->linear;
        dst->atten_cutoff[2] = light->quadratic;
        dst->atten_cutoff[3] = light->cutOff;
        dst->spot_shadow_size[0] = light->outerCutOff;
        dst->spot_shadow_size[1] = (float)light->shadow_map_index;
        dst->spot_shadow_size[2] = light->size[0];
        dst->spot_shadow_size[3] = light->size[1];

        // Cluster coverage: exponential Z slices + conservative screen tiles
        LightClusterRange* range = &ctx->ranges[num_packed];
        if (uncullable) {
            range->x0 = 0;
            range->x1 = LC_CLUSTER_X - 1;
            range->y0 = 0;
            range->y1 = LC_CLUSTER_Y - 1;
            range->z0 = 0;
            range->z1 = LC_CLUSTER_Z - 1;
        } else {
            vec3 view_center;
            glm_mat4_mulv3(view, light->global_position, 1.0f, view_center);
            float zc = -view_center[2];
            if (zc + radius < near_clip || zc - radius > far_clip) {
                continue; // entirely outside the depth range (packed slot reused)
            }
            range->z0 = _slice_for_z(fmaxf(zc - radius, near_clip), slice_scale, slice_bias);
            range->z1 = _slice_for_z(fminf(zc + radius, far_clip), slice_scale, slice_bias);
            _tile_range_for_sphere(projection, view_center, radius, near_clip, range);
        }
        num_packed++;
    }

    ctx->lights.light_counts[0] = num_dir;
    ctx->lights.light_counts[1] = num_packed;

    // Fill pass 1: count lights per covered cluster
    for (int li = 0; li < num_packed; li++) {
        const LightClusterRange* r = &ctx->ranges[li];
        for (int z = r->z0; z <= r->z1; z++)
            for (int y = r->y0; y <= r->y1; y++)
                for (int x = r->x0; x <= r->x1; x++)
                    ctx->counts[x + LC_CLUSTER_X * (y + LC_CLUSTER_Y * z)]++;
    }

    // Prefix sum into index-pool offsets, truncating at the pool cap; a
    // truncated cluster keeps count 0 rather than aliasing another's list
    uint32_t total = 0;
    bool truncated = false;
    for (int ci = 0; ci < LC_CLUSTER_COUNT; ci++) {
        uint32_t count = ctx->counts[ci];
        if (total + count > LC_MAX_CLUSTER_INDICES) {
            ctx->offsets[ci] = 0;
            ctx->counts[ci] = 0;
            truncated = true;
            continue;
        }
        ctx->offsets[ci] = total;
        total += count;
    }
    if (truncated && !ctx->warned_index_overflow) {
        log_warn("Cluster index pool (%d) overflowed; some clusters dropped their lights",
                 LC_MAX_CLUSTER_INDICES);
        ctx->warned_index_overflow = true;
    }

    // Fill pass 2: write indices in packed-light order (deterministic).
    // counts[] doubles as the per-cluster write cursor and ends back at the
    // per-cluster count, which is what the packed grid word wants.
    memset(ctx->counts, 0, sizeof(ctx->counts));
    for (int li = 0; li < num_packed; li++) {
        const LightClusterRange* r = &ctx->ranges[li];
        for (int z = r->z0; z <= r->z1; z++)
            for (int y = r->y0; y <= r->y1; y++)
                for (int x = r->x0; x <= r->x1; x++) {
                    int ci = x + LC_CLUSTER_X * (y + LC_CLUSTER_Y * z);
                    uint32_t slot = ctx->offsets[ci] + ctx->counts[ci];
                    if (slot >= LC_MAX_CLUSTER_INDICES)
                        continue; // truncated cluster
                    ctx->index_pool.indices[slot] = (uint16_t)li;
                    ctx->counts[ci]++;
                }
    }

    // Pack the grid words: (offset << 12) | count
    for (int ci = 0; ci < LC_CLUSTER_COUNT; ci++)
        ctx->grid.clusters[ci] = (ctx->offsets[ci] << 12) | (uint32_t)ctx->counts[ci];

    ubo_upload(engine->lights_ubo, &ctx->lights, sizeof(ctx->lights));
    ubo_upload(engine->clusters_ubo, &ctx->grid, sizeof(ctx->grid));
    ubo_upload(engine->cluster_indices_ubo, &ctx->index_pool, sizeof(ctx->index_pool));

    if (!ctx->logged_first_build) {
        log_info("clustered: %d directional + %d clusterable lights, %u cluster indices", num_dir,
                 num_packed, total);
        ctx->logged_first_build = true;
    }
}
