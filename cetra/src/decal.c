#include <cglm/cglm.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "decal.h"
#include "light.h"
#include "light_cluster.h"
#include "scene.h"

// A decal with no image is not a mark, and one whose image has not reached the
// array yet has no layer to name -- both would project a fully transparent
// nothing at the cost of a box test per fragment.
//
// Static: the one walk that decides membership is below, and it hands its answer
// out. Nothing else re-derives it.
static bool _decal_is_live(const Decal* decal) {
    return decal && decal->enabled && decal->albedo_layer >= 0;
}

int decal_fill_descriptors(const struct Scene* scene, struct GpuDecalBlock* out,
                           float bounds[][4]) {
    if (!scene || !out || !bounds)
        return 0;

    int n = 0;
    for (int i = 0; i < scene->decal_count && n < DECAL_MAX; ++i) {
        const Decal* d = &scene->decals[i];
        if (!_decal_is_live(d))
            continue;

        vec3 axis = {0.0f, 0.0f, 0.0f};
        vec3 up = {0.0f, 0.0f, 0.0f};
        orientation_frame(d->direction, d->up, axis, up);
        // cross(axis, up), NOT cross(up, axis), and the order is the difference
        // between a poster and its mirror image. A projector faces along `axis`
        // and is looked at from the far side of it, so the viewer's right hand
        // is axis x up; the other order points `right` at the viewer's LEFT and
        // lays every image on backwards. Nothing catches that without an
        // asymmetric mark, which is why the fixture has four quadrants.
        vec3 right;
        glm_vec3_cross(axis, up, right);
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
        // CLAMPED, and it is not defensive tidiness: the shader's coverage ends
        // up as this value, and the consumer is a mix() -- so an authored 2.0
        // does not make a decal twice as opaque, it EXTRAPOLATES past the mark
        // into 2*decal - substrate, which is negative in any channel where the
        // mark is darker than half the surface. Negative albedo then reaches the
        // diffuse term, the SSGI attachment, bloom and the meter. The GUI slider
        // stops at 1; a scene file and a restored snapshot do not.
        desc->params0[2] = glm_clamp(d->opacity, 0.0f, 1.0f);
        // The angle as its COSINE, which is what a dot product is compared
        // against -- the spot-cutoff convention. Floored a hair under 1 because
        // smoothstep is undefined where its edges meet, which is exactly what
        // an authored 0 degrees produces.
        desc->params0[3] = fminf(cosf(glm_rad(d->angle_fade)), 1.0f - 1e-4f);
        desc->params1[0] = d->feather;
        desc->params1[1] = d->normal_strength;
        desc->params1[2] = 0.0f;
        desc->params1[3] = 0.0f;

        // The oriented box's own bounding sphere, for the grid. Its radius is
        // the half-diagonal, which a rotation does not change -- so the frame
        // above never enters it, and the over-cover at the corners is harmless
        // for the probes' reason: the fragment recomputes the exact box.
        glm_vec3_copy((float*)d->position, bounds[n]);
        bounds[n][3] = glm_vec3_norm((float*)d->half_extent);
        n++;
    }

    out->info[0] = n;
    return n;
}

void decal_probe_print(const struct Scene* scene, int frame, bool final, uint32_t mask_digest,
                       int mask_bits) {
    if (!scene)
        return;

    int live = 0;
    for (int i = 0; i < scene->decal_count; ++i)
        if (_decal_is_live(&scene->decals[i]))
            live++;

    printf("decal-probe frame=%d authored=%d live=%d mask_bits=%d digest=%08x\n", frame,
           scene->decal_count, live, mask_bits, mask_digest);

    if (!final)
        return;

    for (int i = 0; i < scene->decal_count; ++i) {
        const Decal* d = &scene->decals[i];
        // Says WHY a decal is not live rather than omitting it: an authored
        // decal missing from this list and an authored decal that never loaded
        // its image look the same from outside, and the second is the failure.
        const char* state = !d->enabled          ? "disabled"
                            : d->albedo_layer < 0 ? "no-layer"
                                                  : "live";
        printf("decal-probe decal idx=%d state=%s pos=%.3f,%.3f,%.3f half=%.3f,%.3f,%.3f "
               "dir=%.3f,%.3f,%.3f albedo_layer=%d surface_layer=%d opacity=%.3f "
               "angle_fade=%.1f feather=%.3f\n",
               i, state, (double)d->position[0], (double)d->position[1], (double)d->position[2],
               (double)d->half_extent[0], (double)d->half_extent[1], (double)d->half_extent[2],
               (double)d->direction[0], (double)d->direction[1], (double)d->direction[2],
               d->albedo_layer, d->surface_layer, (double)d->opacity, (double)d->angle_fade,
               (double)d->feather);
    }
    fflush(stdout);
}
