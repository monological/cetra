#include "light_cluster.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "ext/log.h"
#include "intersect.h"
#include "probe_atlas.h"
#include "light.h"
#include "scene.h"
#include "shadow.h"
#include "util.h"

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
    ctx->area_lights_enabled = true;
    ctx->lights_ubo = create_ubo(UBO_LIGHTS_BLOCK_SIZE, UBO_BINDING_LIGHTS);
    ctx->clusters_ubo = create_ubo(UBO_CLUSTERS_BLOCK_SIZE, UBO_BINDING_CLUSTERS);
    ctx->cluster_indices_ubo =
        create_ubo(UBO_CLUSTER_INDICES_BLOCK_SIZE, UBO_BINDING_CLUSTER_INDICES);
    // Created here beside the other light blocks, but NOT uploaded here: it is
    // static per scene, so the per-frame path never touches it (ubo.h).
    ctx->ies_ubo = create_ubo(UBO_IES_BLOCK_SIZE, UBO_BINDING_IES);
    ctx->probes_ubo = create_ubo(UBO_PROBES_BLOCK_SIZE, UBO_BINDING_PROBES);
    ctx->decals_ubo = create_ubo(UBO_DECALS_BLOCK_SIZE, UBO_BINDING_DECALS);
    if (!ctx->lights_ubo || !ctx->clusters_ubo || !ctx->cluster_indices_ubo || !ctx->ies_ubo ||
        !ctx->probes_ubo || !ctx->decals_ubo) {
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
    free_ubo(ctx->ies_ubo);
    free_ubo(ctx->probes_ubo);
    free_ubo(ctx->decals_ubo);
    free(ctx);
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

/*
 * The depth half of the same bound, filling z0/z1 as the above fills x/y -- one
 * expression for what lights, probes and decals each spelled differently.
 *
 * The three spellings were EQUIVALENT and only readably so: _slice_for_z clamps
 * its input to 1e-4 and saturates its output to the grid, so clamping the sphere
 * to the frustum here, clamping it to 1e-4, or not clamping it at all all land on
 * the same slice. That equivalence is worth writing down because it rests
 * entirely on those two internal clamps -- remove either and one of the three
 * former spellings becomes the only correct one, at an end of the range where
 * nothing renders differently until something does.
 *
 * Clamped to the frustum rather than to 1e-4 because that is what the range MEANS:
 * the slices this sphere can reach, which cannot be outside the ones that exist.
 */
static void _slice_range_for_sphere(float zc, float radius, const ClusterFrame* cf,
                                    LightClusterRange* range) {
    range->z0 = _slice_for_z(fmaxf(zc - radius, cf->near_clip), cf);
    range->z1 = _slice_for_z(fminf(zc + radius, cf->far_clip), cf);
}

static void _pack_dir_light(GpuDirLight* dst, const struct Light* light) {
    glm_vec3_copy((float*)light->direction, dst->dir_shadow);
    dst->dir_shadow[3] = (float)light->shadow_map_index;
    glm_vec3_scale((float*)light->color, light->intensity, dst->color_intensity);
    glm_vec2_copy((float*)light->size, dst->size_misc);
}

static void _pack_cluster_light(GpuPackedLight* dst, const struct Light* light, float radius,
                                int live_layer) {
    glm_vec3_copy((float*)light->global_position, dst->pos_range);
    dst->pos_range[3] = radius > 0.0f ? radius : 0.0f; // 0 = unbounded
    dst->dir_type[3] = (float)light->type; // 1 point / 2 spot / 3 area
    glm_vec3_scale((float*)light->color, light->intensity, dst->color_intensity);
    dst->atten_cutoff[0] = light->range > 0.0f ? 1.0f / (light->range * light->range) : 0.0f;
    // The IES profile index, -1 for none -- the same "a float carrying an index
    // or a sentinel" convention shadow_misc[1] uses for the punctual layer, in a
    // slot that was already reserved and read by nothing.
    //
    // Never for a panel. An IES file describes a point-like emitter's angular
    // distribution, and a panel is shaded by an LTC integral over its rectangle
    // instead -- so the index has no meaning there, and shipping it anyway lets
    // a consumer that WEIGHTS lights before it classifies them scale a panel by
    // a distribution its shading never applies. The contact-shadow fold is
    // exactly that consumer, and a wrong denominator is invisible in the frame.
    dst->atten_cutoff[1] = light->type == LIGHT_AREA ? -1.0f : (float)light->ies_profile;
    dst->atten_cutoff[2] = 0.0f;
    dst->atten_cutoff[3] = light->cutOff;
    dst->shadow_misc[0] = light->outerCutOff;
    // The punctual base layer, not the CSM slot: only directionals reach the
    // cascade array and they never arrive through this list.
    //
    // The LIVE layer, resolved by the shadow system rather than read off the
    // light: shadow_layer keeps its last value when the depth pass stops running,
    // so packing it raw hands every consumer a map that is no longer drawn. One
    // site, so the shading lookup and anything using this field as an eligibility
    // test cannot disagree about which lights have a map.
    dst->shadow_misc[1] = (float)live_layer;
    glm_vec2_copy((float*)light->size, &dst->shadow_misc[2]);

    // EVERY type ships the full frame. Both halves used to be panels-only -- the
    // LTC plane test and corner frame assume a unit normal and a height axis --
    // while spot and point shipped their direction as authored and nothing at
    // all for roll.
    //
    // The axis has to be unit because an IES lookup is acos(dot(L, axis)), and a
    // non-unit axis skews that angle non-linearly rather than merely scaling it;
    // spotConeFactor got away with it only by normalizing at use. The roll has
    // to be there because an ASYMMETRIC profile is not rotationally invariant --
    // a wall-washer aimed down still has to say which wall. Three floats that
    // were being zeroed anyway, and a symmetric profile never reads them.
    // Zero-init is dead -- light_emission_frame writes both unconditionally --
    // but cppcheck cannot see through cglm's inlines to know that
    vec3 dir = {0.0f, 0.0f, 0.0f}, up = {0.0f, 0.0f, 0.0f};
    light_emission_frame(light, dir, up);
    glm_vec3_copy(dir, dst->dir_type);
    glm_vec3_copy(up, dst->up_area);
    dst->up_area[3] = 0.0f;
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
        if (light->type == LIGHT_AREA && !ctx->area_lights_enabled)
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
            _slice_range_for_sphere(zc, radius, cf, range);
            _tile_range_for_sphere(projection, view_center, radius, cf->near_clip, range);
            glm_vec3_copy(view_center, sphere);
            sphere[3] = radius;
        }

        _pack_cluster_light(&ctx->lights.cluster_lights[num_packed], light, radius,
                            shadow_live_punctual_layer(scene->shadow_system, light));
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

/*
 * Which probes reach which froxel (spec 11.70).
 *
 * A MASK rather than the lights' offset|count word plus an index pool: at a cap
 * of eight, one byte per froxel holds the whole answer, so there is no prefix
 * sum, no shared pool and no truncation state to represent. It also needs no
 * touched bitset -- that exists because the light path walks its cells twice,
 * and this one writes its answer on the first pass.
 *
 * Each probe's parallax box is bounded by its own sphere and handed to the
 * light path's own tests unchanged. The bound over-covers a box badly at the
 * corners, and that is harmless here in a way it would not be for a light: the
 * fragment recomputes the exact box weight anyway, and outside the box that
 * weight is zero. What the mask decides is only which probes a fragment
 * BOTHERS to weigh.
 */
static void _mark_probe_clusters(LightClusterContext* ctx, const struct Scene* scene, mat4 view,
                                 mat4 projection, float near_clip, const ClusterFrame* cf) {
    const ReflectionProbeSet* set = scene->probe_set;
    memset(&ctx->probes, 0, sizeof(ctx->probes));

    if (!probe_set_multi(set)) {
        // A zero block is the disarmed state and the shader reads count 0 from
        // it. Written once on the way down rather than every frame: the upload
        // is what costs, not the memset.
        if (ctx->probes_armed) {
            ubo_upload(ctx->probes_ubo, &ctx->probes, sizeof(ctx->probes));
            ctx->probes_armed = false;
        }
        return;
    }

    // What a probe IS -- its position, its box, where its column sits -- is the
    // probe set's to state, not this module's. This function owns the froxel
    // masks below and nothing else about them.
    probe_set_fill_descriptors(set, &ctx->probes);

    int bits = 0;
    for (int i = 0; i < set->count; ++i) {
        const ReflectionProbe* probe = set->probes[i];

        // World box -> view-space bounding sphere, the shape the grid tests.
        vec3 center, half;
        glm_vec3_add((float*)probe->box_min, (float*)probe->box_max, center);
        glm_vec3_scale(center, 0.5f, center);
        glm_vec3_sub((float*)probe->box_max, center, half);
        const float radius = glm_vec3_norm(half);

        vec3 view_center;
        glm_mat4_mulv3(view, center, 1.0f, view_center);

        LightClusterRange range;
        _tile_range_for_sphere(projection, view_center, radius, near_clip, &range);
        _slice_range_for_sphere(-view_center[2], radius, cf, &range);

        const float sphere[4] = {view_center[0], view_center[1], view_center[2], radius};
        const float radius_sq = radius * radius;
        for (int z = range.z0; z <= range.z1; ++z) {
            for (int y = range.y0; y <= range.y1; ++y) {
                for (int x = range.x0; x <= range.x1; ++x) {
                    if (!_sphere_touches_cluster(sphere, radius_sq, x, y, z, cf))
                        continue;
                    const int ci = x + LC_CLUSTER_X * (y + LC_CLUSTER_Y * z);
                    ctx->probes.cluster_masks[ci >> 2] |= 1u << (((ci & 3) * 8) + i);
                    bits++;
                }
            }
        }
    }

    ubo_upload(ctx->probes_ubo, &ctx->probes, sizeof(ctx->probes));
    ctx->probes_armed = true;

    // Handed back rather than written into the set from here: this module reads
    // the probes and owns the grid, and the one place it reached sideways to
    // mutate another subsystem's fields was also the one place a `const Scene*`
    // had to be laundered to do it. The digest is over the MASKS alone -- the
    // descriptors are a function of the authored scene, where the masks are a
    // function of the camera path, which is what determinism is in question
    // about.
    probe_set_report_masks(scene->probe_set, ctx->probes.cluster_masks,
                           sizeof(ctx->probes.cluster_masks), bits);
}

/*
 * Which decals reach which froxel (spec 11.73).
 *
 * The probe pass's shape, one bit per (decal, froxel) written on a single walk,
 * with the same argument for a MASK over the lights' offset|count word: at a cap
 * of sixteen the answer fits two bytes a froxel, so there is no prefix sum, no
 * shared pool and no truncation state -- and no touched bitset, since nothing
 * walks the cells twice.
 *
 * What it takes from the LIGHT path and the probes do not is the two rejects: a
 * world-frustum test and a view-depth range test. A probe set is small and
 * mostly on screen, so over-covering costs a handful of froxels; decals are
 * scenery, and a room's worth of them sit behind the camera in any given frame.
 * Marking those would spend mask bits -- and with them per-fragment box tests --
 * on marks that cannot be seen.
 *
 * This walks the SLOTS the descriptor pack produced, not the scene's array, so
 * bit i and descriptor i name the same decal by construction. It used to re-walk
 * the scene under a shared predicate to rediscover that numbering -- see
 * decal.h for what had to agree across the two files for that to hold, and for
 * the one-line edit that would have broken it silently.
 */
static void _mark_decal_clusters(LightClusterContext* ctx, const struct Scene* scene,
                                 const Frustum* frustum, mat4 view, mat4 projection,
                                 float near_clip, const ClusterFrame* cf) {
    memset(&ctx->decals, 0, sizeof(ctx->decals));
    float bounds[DECAL_MAX][4];
    const int live = decal_fill_descriptors(scene, &ctx->decals, bounds);

    if (live <= 0) {
        // The disarmed state, written once on the way down: a zero block reads
        // count 0 and the shader's loop is never entered. The upload is what
        // costs, not the memset. The digest and bit count go with it, or a probe
        // taken after the last decal went dark reports the previous build's.
        if (ctx->decals_armed) {
            ubo_upload(ctx->decals_ubo, &ctx->decals, sizeof(ctx->decals));
            ctx->decals_armed = false;
        }
        ctx->decal_mask_bits = 0;
        return;
    }

    int bits = 0;
    for (int index = 0; index < live; ++index) {
        const float* centre = bounds[index];
        const float radius = bounds[index][3];
        if (radius <= 0.0f)
            continue;

        if (frustum && !frustum_test_sphere(frustum, (float*)centre, radius))
            continue;

        vec3 view_center;
        glm_mat4_mulv3(view, (float*)centre, 1.0f, view_center);
        const float zc = -view_center[2];
        if (zc + radius < cf->near_clip || zc - radius > cf->far_clip)
            continue;

        LightClusterRange range;
        _tile_range_for_sphere(projection, view_center, radius, near_clip, &range);
        _slice_range_for_sphere(zc, radius, cf, &range);

        const float sphere[4] = {view_center[0], view_center[1], view_center[2], radius};
        const float radius_sq = radius * radius;
        for (int z = range.z0; z <= range.z1; ++z) {
            for (int y = range.y0; y <= range.y1; ++y) {
                for (int x = range.x0; x <= range.x1; ++x) {
                    if (!_sphere_touches_cluster(sphere, radius_sq, x, y, z, cf))
                        continue;
                    const int ci = x + LC_CLUSTER_X * (y + LC_CLUSTER_Y * z);
                    ctx->decals.cluster_masks[ci >> 1] |= 1u << (((ci & 1) * 16) + index);
                    bits++;
                }
            }
        }
    }

    ubo_upload(ctx->decals_ubo, &ctx->decals, sizeof(ctx->decals));
    ctx->decals_armed = true;
    ctx->decal_mask_bits = bits;
}

void light_cluster_build_and_upload(LightClusterContext* ctx, struct Scene* scene, mat4 view,
                                    mat4 projection, int fb_width, int fb_height, float near_clip,
                                    float far_clip) {
    if (!ctx || !scene)
        return;
    // The viewport is read live, so a minimized window hands us 0 here and the
    // cluster params below would upload +inf. Keep last frame's grid; nothing
    // is drawn at zero area anyway.
    if (fb_width <= 0 || fb_height <= 0)
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

    // Uploaded when the SET changes, not per frame -- profiles are static once
    // loaded, and this path re-enters seven times on a probe-capture frame.
    //
    // Keyed on the library's revision rather than its profile COUNT. This
    // context belongs to the engine and the library to the scene, so a count
    // compares two scenes' libraries as if they were one: switch between scenes
    // holding one profile each and every light samples the other's table, which
    // renders as a plausible luminaire with the wrong skirt.
    uint64_t ies_rev = ies_library_revision(scene->ies_library);
    if (ies_rev != ctx->ies_uploaded_revision) {
        GpuIesBlock block;
        // Only on success: a failed pack leaves iesCounts.x holding the previous
        // library's count, and committing the revision would keep it.
        if (ies_library_pack(scene->ies_library, &block)) {
            ubo_upload(ctx->ies_ubo, &block, sizeof(block));
            ctx->ies_uploaded_revision = ies_rev;
        }
    }

    _gather_lights(ctx, scene, &frustum, view, projection, &cf);
    _mark_touched_clusters(ctx, &cf);
    uint32_t total_indices = _assign_index_offsets(ctx);
    _fill_index_pool(ctx);

    // Strictly after the light pass and touching none of its three arrays: the
    // light grid has no gate of its own anywhere in the suite, so the only
    // safe place for a second consumer of this frame's ClusterFrame is past
    // the point where the first one is finished with it.
    _mark_probe_clusters(ctx, scene, view, projection, near_clip, &cf);
    _mark_decal_clusters(ctx, scene, &frustum, view, projection, near_clip, &cf);

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

uint32_t light_cluster_decal_mask_digest(const LightClusterContext* ctx) {
    // Hashed here rather than at build time: the masks are still sitting in the
    // context, and both callers run at most once a frame behind a flag.
    return ctx ? fnv1a_bytes(ctx->decals.cluster_masks, sizeof(ctx->decals.cluster_masks)) : 0u;
}

int light_cluster_decal_mask_bits(const LightClusterContext* ctx) {
    return ctx ? ctx->decal_mask_bits : 0;
}
