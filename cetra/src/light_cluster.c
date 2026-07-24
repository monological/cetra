#include "light_cluster.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "ext/log.h"
#include "intersect.h"
#include "light.h"
#include "scene.h"

// Radiance below this reads as black at the project-standard -E 1.0 (one LDR
// LSB); the derived cull radius is where attenuation crosses it.
#define LC_CULL_EPSILON (1.0f / 256.0f)

// View-dependent constants shared by the assignment and fill phases.
typedef struct ClusterFrame {
    float slice_scale, slice_bias;
    float inv_p00, inv_p11;              // reciprocals of the projection's x/y scales
    float slice_depths[LC_CLUSTER_Z + 1]; // view depth at each slice boundary
    float near_clip, far_clip;
} ClusterFrame;

LightClusterContext* create_light_cluster_context(void) {
    LightClusterContext* ctx = calloc(1, sizeof(LightClusterContext));
    if (!ctx) {
        log_error("Failed to allocate light cluster context");
        return NULL;
    }
    ctx->lights_ubo = create_ubo(UBO_LIGHTS_BLOCK_SIZE, UBO_BINDING_LIGHTS);
    ctx->clusters_ubo = create_ubo(UBO_CLUSTERS_BLOCK_SIZE, UBO_BINDING_CLUSTERS);
    ctx->cluster_indices_ubo =
        create_ubo(UBO_CLUSTER_INDICES_BLOCK_SIZE, UBO_BINDING_CLUSTER_INDICES);
    if (!ctx->lights_ubo || !ctx->clusters_ubo || !ctx->cluster_indices_ubo) {
        log_error("Failed to create clustered light UBOs");
        free_light_cluster_context(ctx);
        return NULL;
    }
    return ctx;
}

void free_light_cluster_context(LightClusterContext* ctx) {
    if (!ctx)
        return;
    free_ubo(ctx->lights_ubo);
    free_ubo(ctx->clusters_ubo);
    free_ubo(ctx->cluster_indices_ubo);
    free(ctx);
}

float light_cull_radius(const struct Light* light) {
    if (light->range > 0.0f)
        return light->range;

    float peak = fmaxf(light->color[0], fmaxf(light->color[1], light->color[2]));
    float i_eff = light->intensity * peak;
    if (i_eff <= 0.0f)
        return 0.0f;

    // Area panels ignore the attenuation coefficients entirely -- the LTC
    // form factor carries the falloff, and `intensity` is emitted radiance.
    // Bound the reach by the head-on far-field irradiance I*A/(pi*d^2),
    // solved against the same 1/256 visibility floor the point path uses.
    // Head-on is the directional maximum (real response is that times NdotL),
    // so this is conservative; the half-diagonal covers the panel's own extent.
    if (light->type == LIGHT_AREA) {
        float area = light->size[0] * light->size[1];
        if (area <= 0.0f)
            return 0.0f;
        float half_diagonal =
            0.5f * sqrtf(light->size[0] * light->size[0] + light->size[1] * light->size[1]);
        return sqrtf(i_eff * area / (LC_CULL_EPSILON * (float)M_PI)) + half_diagonal;
    }

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

    return r;
}

// slice(z) for view depth z, matching clusterLightList in lights_ubo.glsl
static int _slice_for_z(float z, const ClusterFrame* cf) {
    int s = (int)floorf(log2f(fmaxf(z, 1e-4f)) * cf->slice_scale + cf->slice_bias);
    return s < 0 ? 0 : (s >= LC_CLUSTER_Z ? LC_CLUSTER_Z - 1 : s);
}

static int _clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void _cluster_frame_init(ClusterFrame* cf, mat4 projection, float near_clip,
                                float far_clip) {
    float log_ratio = log2f(far_clip / near_clip);
    cf->slice_scale = (float)LC_CLUSTER_Z / log_ratio;
    cf->slice_bias = -(float)LC_CLUSTER_Z * log2f(near_clip) / log_ratio;
    cf->inv_p00 = 1.0f / projection[0][0];
    cf->inv_p11 = 1.0f / projection[1][1];
    cf->near_clip = near_clip;
    cf->far_clip = far_clip;
    // Inverse of the slice mapping: slice s spans [slice_depths[s], [s+1]]
    for (int s = 0; s <= LC_CLUSTER_Z; s++)
        cf->slice_depths[s] = near_clip * powf(far_clip / near_clip, (float)s / (float)LC_CLUSTER_Z);
}

// Does the light's view-space bounding sphere touch this cluster's view-space
// AABB? The tile/slice range box is only a loop bound -- it over-covers badly
// for near-camera lights (a sphere whose screen rect spans the frame still
// only intersects a curved shell of that box's clusters), so this is the test
// that actually decides membership. The cluster wedge's AABB comes from its 8
// corners: x = ndc_x * depth / P00 (symmetric projection), z = -depth.
static bool _sphere_touches_cluster(const float* sphere, float radius_sq, int x, int y, int z,
                                    const ClusterFrame* cf) {
    float d0 = cf->slice_depths[z], d1 = cf->slice_depths[z + 1];
    float nx0 = (float)x * (2.0f / (float)LC_CLUSTER_X) - 1.0f;
    float nx1 = nx0 + 2.0f / (float)LC_CLUSTER_X;
    float ny0 = (float)y * (2.0f / (float)LC_CLUSTER_Y) - 1.0f;
    float ny1 = ny0 + 2.0f / (float)LC_CLUSTER_Y;
    float xa = nx0 * d0 * cf->inv_p00, xb = nx0 * d1 * cf->inv_p00;
    float xc = nx1 * d0 * cf->inv_p00, xd = nx1 * d1 * cf->inv_p00;
    float ya = ny0 * d0 * cf->inv_p11, yb = ny0 * d1 * cf->inv_p11;
    float yc = ny1 * d0 * cf->inv_p11, yd = ny1 * d1 * cf->inv_p11;
    float min_x = fminf(fminf(xa, xb), fminf(xc, xd));
    float max_x = fmaxf(fmaxf(xa, xb), fmaxf(xc, xd));
    float min_y = fminf(fminf(ya, yb), fminf(yc, yd));
    float max_y = fmaxf(fmaxf(ya, yb), fmaxf(yc, yd));
    // Closest point on the AABB to the sphere center (view space, z = -depth)
    float cx = glm_clamp(sphere[0], min_x, max_x) - sphere[0];
    float cy = glm_clamp(sphere[1], min_y, max_y) - sphere[1];
    float cz = glm_clamp(sphere[2], -d1, -d0) - sphere[2];
    return cx * cx + cy * cy + cz * cz <= radius_sq;
}

// Conservative screen-tile bound for the fill loops: project the sphere's four
// extreme points at its closest depth (max magnification over its depth range),
// clamp to NDC, map to tiles. Purely an optimization -- _sphere_touches_cluster
// rejects whatever this over-covers.
static void _tile_range_for_sphere(mat4 projection, const vec3 view_center, float radius,
                                   float near_clip, LightClusterRange* range) {
    float zc = -view_center[2]; // view depth (camera looks down -Z)

    if (zc - radius <= near_clip) {
        // Sphere crosses or contains the near plane: it cannot be safely
        // NDC-projected, so bound it at the whole grid and let the exact test
        // do the work.
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

    range->x0 = _clampi((int)floorf((ndc_min_x * 0.5f + 0.5f) * LC_CLUSTER_X), 0, LC_CLUSTER_X - 1);
    range->x1 = _clampi((int)floorf((ndc_max_x * 0.5f + 0.5f) * LC_CLUSTER_X), 0, LC_CLUSTER_X - 1);
    range->y0 = _clampi((int)floorf((ndc_min_y * 0.5f + 0.5f) * LC_CLUSTER_Y), 0, LC_CLUSTER_Y - 1);
    range->y1 = _clampi((int)floorf((ndc_max_y * 0.5f + 0.5f) * LC_CLUSTER_Y), 0, LC_CLUSTER_Y - 1);
}

static void _pack_dir_light(GpuDirLight* dst, const struct Light* light) {
    glm_vec3_copy((float*)light->direction, dst->dir_shadow);
    dst->dir_shadow[3] = (float)light->shadow_map_index;
    glm_vec3_scale((float*)light->color, light->intensity, dst->color_intensity);
    glm_vec2_copy((float*)light->size, dst->size_misc);
}

static void _pack_cluster_light(GpuPackedLight* dst, const struct Light* light, float radius) {
    glm_vec3_copy((float*)light->global_position, dst->pos_range);
    dst->pos_range[3] = radius > 0.0f ? radius : 0.0f; // 0 = unbounded
    glm_vec3_copy((float*)light->direction, dst->dir_type);
    dst->dir_type[3] = (float)light->type; // 1 point / 2 spot / 3 area
    glm_vec3_scale((float*)light->color, light->intensity, dst->color_intensity);
    dst->atten_cutoff[0] = light->constant;
    dst->atten_cutoff[1] = light->linear;
    dst->atten_cutoff[2] = light->quadratic;
    dst->atten_cutoff[3] = light->cutOff;
    dst->spot_shadow_size[0] = light->outerCutOff;
    dst->spot_shadow_size[1] = (float)light->shadow_map_index;
    glm_vec2_copy((float*)light->size, &dst->spot_shadow_size[2]);

    // Area panels: ship an orthonormal height axis so the shader can build
    // corners without trusting the authored `up`. Gram-Schmidt against the
    // normal; if the two are parallel (or up is degenerate) fall back to
    // crossing the direction with the world axis it leans on least, which is
    // always well-conditioned. Other light types leave the row zeroed.
    if (light->type == LIGHT_AREA) {
        vec3 dir, up;
        glm_vec3_copy((float*)light->direction, dir);
        if (glm_vec3_norm(dir) < 1e-6f)
            glm_vec3_copy((vec3){0.0f, -1.0f, 0.0f}, dir);
        glm_vec3_normalize(dir);

        vec3 proj;
        glm_vec3_scale(dir, glm_vec3_dot((float*)light->up, dir), proj);
        glm_vec3_sub((float*)light->up, proj, up);

        if (glm_vec3_norm(up) < 1e-4f) {
            float ax = fabsf(dir[0]), ay = fabsf(dir[1]), az = fabsf(dir[2]);
            vec3 axis = {0.0f, 0.0f, 1.0f};
            if (ax <= ay && ax <= az)
                glm_vec3_copy((vec3){1.0f, 0.0f, 0.0f}, axis);
            else if (ay <= az)
                glm_vec3_copy((vec3){0.0f, 1.0f, 0.0f}, axis);
            glm_vec3_cross(dir, axis, up);
        }
        glm_vec3_normalize(up);
        glm_vec3_copy(up, dst->up_reserved);
        dst->up_reserved[3] = 0.0f;
    }
}

// Classify, cull and pack scene->lights. Directionals shade unclustered (they
// reach every fragment); everything else gets a cull radius, a frustum test
// and a cluster range. Walked in scene->lights order so the packing -- and so
// the shading loop order -- is deterministic.
static void _gather_lights(LightClusterContext* ctx, struct Scene* scene, const Frustum* frustum,
                           mat4 view, mat4 projection, const ClusterFrame* cf) {
    int num_dir = 0, num_packed = 0, num_area = 0;

    for (size_t i = 0; i < scene->light_count; i++) {
        struct Light* light = scene->lights[i];
        if (!light || light->type == LIGHT_UNKNOWN)
            continue;

        if (light->type == LIGHT_DIRECTIONAL) {
            if (num_dir >= LC_MAX_DIR_LIGHTS) {
                if (!ctx->warned_dir_overflow) {
                    log_warn("More than %d directional lights; extras ignored", LC_MAX_DIR_LIGHTS);
                    ctx->warned_dir_overflow = true;
                }
                continue;
            }
            _pack_dir_light(&ctx->lights.dir_lights[num_dir++], light);
            continue;
        }

        float radius = light_cull_radius(light);
        if (radius == 0.0f)
            continue; // never reaches epsilon
        bool uncullable = radius < 0.0f;
        if (!uncullable && !frustum_test_sphere(frustum, light->global_position, radius))
            continue;

        if (num_packed >= LC_MAX_CLUSTER_LIGHTS) {
            if (!ctx->warned_packed_overflow) {
                log_warn("More than %d clusterable lights in view; extras ignored",
                         LC_MAX_CLUSTER_LIGHTS);
                ctx->warned_packed_overflow = true;
            }
            break;
        }

        LightClusterRange* range = &ctx->ranges[num_packed];
        float* sphere = ctx->view_spheres[num_packed];
        if (uncullable) {
            range->x0 = range->y0 = range->z0 = 0;
            range->x1 = LC_CLUSTER_X - 1;
            range->y1 = LC_CLUSTER_Y - 1;
            range->z1 = LC_CLUSTER_Z - 1;
            sphere[3] = -1.0f; // marker: touches every cluster, skip the exact test
        } else {
            vec3 view_center;
            glm_mat4_mulv3(view, light->global_position, 1.0f, view_center);
            float zc = -view_center[2];
            if (zc + radius < cf->near_clip || zc - radius > cf->far_clip)
                continue; // outside the depth range (this packed slot is reused)
            range->z0 = _slice_for_z(fmaxf(zc - radius, cf->near_clip), cf);
            range->z1 = _slice_for_z(fminf(zc + radius, cf->far_clip), cf);
            _tile_range_for_sphere(projection, view_center, radius, cf->near_clip, range);
            glm_vec3_copy(view_center, sphere);
            sphere[3] = radius;
        }

        _pack_cluster_light(&ctx->lights.cluster_lights[num_packed], light, radius);
        num_packed++;
        if (light->type == LIGHT_AREA)
            num_area++;
    }

    ctx->lights.light_counts[0] = num_dir;
    ctx->lights.light_counts[1] = num_packed;
    // Lets the shader skip the LTC lookups entirely on scenes with no panels
    // -- a dynamically uniform branch, so area-free frames pay nothing
    ctx->lights.light_counts[2] = num_area;
}

// Pass 1: record which clusters each light touches (one bit per pair) and
// count per cluster. The bitset is what pass 2 replays, so the expensive
// sphere-vs-AABB test runs exactly once per candidate cell.
static void _mark_touched_clusters(LightClusterContext* ctx, const ClusterFrame* cf) {
    int num_packed = ctx->lights.light_counts[1];
    memset(ctx->counts, 0, sizeof(ctx->counts));
    memset(ctx->touched, 0, (size_t)num_packed * LC_TOUCH_STRIDE);

    for (int li = 0; li < num_packed; li++) {
        const LightClusterRange* r = &ctx->ranges[li];
        const float* sphere = ctx->view_spheres[li];
        bool uncullable = sphere[3] < 0.0f;
        float radius_sq = sphere[3] * sphere[3];
        uint8_t* touched = ctx->touched[li];

        for (int z = r->z0; z <= r->z1; z++)
            for (int y = r->y0; y <= r->y1; y++)
                for (int x = r->x0; x <= r->x1; x++) {
                    if (!uncullable && !_sphere_touches_cluster(sphere, radius_sq, x, y, z, cf))
                        continue;
                    int ci = x + LC_CLUSTER_X * (y + LC_CLUSTER_Y * z);
                    touched[ci >> 3] |= (uint8_t)(1u << (ci & 7));
                    ctx->counts[ci]++;
                }
    }
}

// Prefix-sum the counts into index-pool offsets, truncating at the pool cap.
// A truncated cluster keeps count 0 rather than aliasing another's list.
// Returns the live index count.
static uint32_t _assign_index_offsets(LightClusterContext* ctx) {
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
    return total;
}

// Pass 2: replay the bitset into the index pool in packed-light order
// (deterministic), then pack each cluster's (offset << 12 | count) word.
// counts[] doubles as the per-cluster write cursor and ends back at the
// per-cluster count, which is what the grid word wants.
static void _fill_index_pool(LightClusterContext* ctx) {
    int num_packed = ctx->lights.light_counts[1];
    memset(ctx->counts, 0, sizeof(ctx->counts));

    for (int li = 0; li < num_packed; li++) {
        const LightClusterRange* r = &ctx->ranges[li];
        const uint8_t* touched = ctx->touched[li];

        for (int z = r->z0; z <= r->z1; z++)
            for (int y = r->y0; y <= r->y1; y++)
                for (int x = r->x0; x <= r->x1; x++) {
                    int ci = x + LC_CLUSTER_X * (y + LC_CLUSTER_Y * z);
                    if (!(touched[ci >> 3] & (1u << (ci & 7))))
                        continue;
                    uint32_t slot = ctx->offsets[ci] + ctx->counts[ci];
                    if (slot >= LC_MAX_CLUSTER_INDICES)
                        continue; // truncated cluster
                    ctx->index_pool.indices[slot] = (uint16_t)li;
                    ctx->counts[ci]++;
                }
    }

    for (int ci = 0; ci < LC_CLUSTER_COUNT; ci++)
        ctx->grid.clusters[ci] = (ctx->offsets[ci] << 12) | (uint32_t)ctx->counts[ci];
}

void light_cluster_build_and_upload(LightClusterContext* ctx, struct Scene* scene, mat4 view,
                                    mat4 projection, int fb_width, int fb_height, float near_clip,
                                    float far_clip) {
    if (!ctx || !scene)
        return;

    ClusterFrame cf;
    _cluster_frame_init(&cf, projection, near_clip, far_clip);

    memset(&ctx->lights, 0, sizeof(ctx->lights));
    ctx->lights.cluster_params[0] = cf.slice_scale;
    ctx->lights.cluster_params[1] = cf.slice_bias;
    ctx->lights.cluster_params[2] = (float)LC_CLUSTER_X / (float)fb_width;
    ctx->lights.cluster_params[3] = (float)LC_CLUSTER_Y / (float)fb_height;

    // World-space frustum for the sphere pre-cull (same planes node culling uses)
    mat4 view_proj;
    glm_mat4_mul(projection, view, view_proj);
    Frustum frustum;
    frustum_extract_from_vp(view_proj, &frustum);

    _gather_lights(ctx, scene, &frustum, view, projection, &cf);
    _mark_touched_clusters(ctx, &cf);
    uint32_t total_indices = _assign_index_offsets(ctx);
    _fill_index_pool(ctx);

    // Upload only the live prefix of the variable-length blocks (the light
    // array and index pool are both tail fields, and the shader never reads
    // past the counts). The grid is always full: the shader indexes it
    // directly by cluster id, empty clusters included.
    int num_packed = ctx->lights.light_counts[1];
    ubo_upload(ctx->lights_ubo, &ctx->lights,
               (GLsizeiptr)(offsetof(GpuLightsBlock, cluster_lights) +
                            (size_t)num_packed * sizeof(GpuPackedLight)));
    ubo_upload(ctx->clusters_ubo, &ctx->grid, sizeof(ctx->grid));
    ubo_upload(ctx->cluster_indices_ubo, &ctx->index_pool,
               (GLsizeiptr)((size_t)total_indices * sizeof(uint16_t)));

    if (!ctx->logged_first_build) {
        log_info("clustered: %d directional + %d clusterable lights, %u cluster indices",
                 ctx->lights.light_counts[0], num_packed, total_indices);
        ctx->logged_first_build = true;
    }
}
