#include <cglm/cglm.h>
#include <math.h>
#include <string.h>

#include "decal.h"
#include "light.h"
#include "light_cluster.h"
#include "scene.h"

bool decal_is_live(const struct Decal* decal) {
    // A decal with no image is not a mark, and one whose image has not reached
    // the array yet has no layer to name -- both would project a fully
    // transparent nothing at the cost of a box test per fragment.
    return decal && decal->enabled && decal->albedo_layer >= 0;
}

void decal_fill_descriptors(const struct Scene* scene, struct GpuDecalBlock* out) {
    if (!scene || !out)
        return;

    int n = 0;
    for (int i = 0; i < scene->decal_count && n < DECAL_MAX; ++i) {
        const Decal* d = &scene->decals[i];
        if (!decal_is_live(d))
            continue;

        vec3 axis = {0.0f, 0.0f, 0.0f};
        vec3 up = {0.0f, 0.0f, 0.0f};
        orientation_frame(d->direction, d->up, axis, up);
        // Right-handed with local +Z along the projection axis, so the image's
        // u runs along `right` and v along `up` -- the frame light_emission_frame
        // hands a panel, used for the same reason.
        vec3 right;
        glm_vec3_cross(up, axis, right);
        glm_vec3_normalize(right);

        // The rows are the frame TRANSPOSED (an orthonormal basis inverts by
        // transpose), each scaled by 1/half_extent so the result is already the
        // [-1,1] box coordinate, with -dot(row, position) as the translation.
        const float* basis[3] = {right, up, axis};
        GpuDecalDesc* desc = &out->descs[n];
        float* rows[3] = {desc->row0, desc->row1, desc->row2};
        for (int r = 0; r < 3; ++r) {
            // A zero half-extent would divide by nothing and put every fragment
            // in the box; clamped rather than refused because the authoring side
            // has already refused a missing size and this is the last defence.
            const float half = fmaxf(fabsf(d->half_extent[r]), 1e-4f);
            const float inv = 1.0f / half;
            rows[r][0] = basis[r][0] * inv;
            rows[r][1] = basis[r][1] * inv;
            rows[r][2] = basis[r][2] * inv;
            rows[r][3] = -glm_vec3_dot((float*)basis[r], (float*)d->position) * inv;
        }

        desc->params0[0] = (float)d->albedo_layer;
        desc->params0[1] = (float)d->surface_layer;
        desc->params0[2] = d->opacity;
        desc->params0[3] = cosf(glm_rad(d->angle_fade));
        desc->params1[0] = d->feather;
        desc->params1[1] = d->normal_strength;
        desc->params1[2] = 0.0f;
        desc->params1[3] = 0.0f;
        n++;
    }

    out->info[0] = n;
}

uint32_t decal_mask_digest(const void* masks, size_t bytes) {
    uint32_t h = 2166136261u;
    const uint8_t* p = (const uint8_t*)masks;
    if (!p)
        return h;
    for (size_t b = 0; b < bytes; ++b) {
        h ^= p[b];
        h *= 16777619u;
    }
    return h;
}
