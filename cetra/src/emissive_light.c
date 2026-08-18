
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "emissive_light.h"
#include "material.h"
#include "mesh.h"
#include "scene.h"

// Candidate orientations tried when fitting the rectangle, one bin per degree
// over the half-circle a rectangle's axis is unique in. The bins DEDUPLICATE
// mesh edge directions rather than quantizing them -- each holds the first
// exact angle that lands in it -- so a quad's four edges are tested at their
// true angles and only a mesh with hundreds of distinct edge directions pays
// the approximation. That is what bounds the cost at O(bins * vertices)
// regardless of triangle count.
#define EMISSIVE_FIT_ANGLE_BINS 180

const char* emissive_fit_reject_name(EmissiveFitReject reject) {
    switch (reject) {
    case EMISSIVE_FIT_OK:
        return "ok";
    case EMISSIVE_FIT_NO_GEOMETRY:
        return "no indexed triangles";
    case EMISSIVE_FIT_DEGENERATE:
        return "zero area";
    case EMISSIVE_FIT_NOT_PLANAR:
        return "not planar";
    case EMISSIVE_FIT_TOO_DIM:
        return "radiance too low to light anything";
    case EMISSIVE_FIT_OPTED_OUT:
        return "material opted out";
    }
    return "unknown";
}

// Callers zero-initialise the vec3 they pass here even though this writes all
// three components unconditionally: cppcheck cannot see through it and reports
// uninitvar at every call site. Same trade light_cluster.c:228 already records
// for its panel-frame helpers.
static void _vertex(const Mesh* mesh, size_t i, vec3 out) {
    out[0] = mesh->vertices[i * 3 + 0];
    out[1] = mesh->vertices[i * 3 + 1];
    out[2] = mesh->vertices[i * 3 + 2];
}

// Extent of every vertex along one candidate in-plane frame, and the area of
// the rectangle that bounds them. `u` and `v` are built so that u x v == normal,
// which is what makes cross(up, normal) recover the width axis in ltc.glsl.
static float _rect_for_axis(const Mesh* mesh, const vec3 center, const vec3 u, const vec3 v,
                            float* out_umin, float* out_umax, float* out_vmin, float* out_vmax) {
    float umin = FLT_MAX, umax = -FLT_MAX, vmin = FLT_MAX, vmax = -FLT_MAX;
    for (size_t i = 0; i < mesh->vertex_count; i++) {
        vec3 p = {0.0f, 0.0f, 0.0f}, d;
        _vertex(mesh, i, p);
        glm_vec3_sub(p, (float*)center, d);
        float du = glm_vec3_dot(d, (float*)u);
        float dv = glm_vec3_dot(d, (float*)v);
        if (du < umin)
            umin = du;
        if (du > umax)
            umax = du;
        if (dv < vmin)
            vmin = dv;
        if (dv > vmax)
            vmax = dv;
    }
    *out_umin = umin;
    *out_umax = umax;
    *out_vmin = vmin;
    *out_vmax = vmax;
    return (umax - umin) * (vmax - vmin);
}

EmissiveFitReject emissive_panel_fit(const Mesh* mesh, EmissivePanelFit* out) {
    if (!mesh || !out || !mesh->vertices || !mesh->indices || mesh->vertex_count == 0 ||
        mesh->index_count < 3 || mesh->draw_mode != MESH_TRIANGLES)
        return EMISSIVE_FIT_NO_GEOMETRY;

    memset(out, 0, sizeof(*out));

    // One pass over the triangles for all three area-weighted quantities. The
    // cross product IS twice the area times the normal, so the same vector
    // serves the normal sum and the area without a normalize per triangle.
    vec3 normal_sum = {0.0f, 0.0f, 0.0f};
    vec3 centroid_sum = {0.0f, 0.0f, 0.0f};
    float area_total = 0.0f;
    for (size_t i = 0; i + 2 < mesh->index_count; i += 3) {
        unsigned i0 = mesh->indices[i], i1 = mesh->indices[i + 1], i2 = mesh->indices[i + 2];
        if (i0 >= mesh->vertex_count || i1 >= mesh->vertex_count || i2 >= mesh->vertex_count)
            continue;

        vec3 p0 = {0.0f, 0.0f, 0.0f}, p1 = {0.0f, 0.0f, 0.0f}, p2 = {0.0f, 0.0f, 0.0f};
        vec3 e1, e2, cross;
        _vertex(mesh, i0, p0);
        _vertex(mesh, i1, p1);
        _vertex(mesh, i2, p2);
        glm_vec3_sub(p1, p0, e1);
        glm_vec3_sub(p2, p0, e2);
        glm_vec3_cross(e1, e2, cross);

        float area = 0.5f * glm_vec3_norm(cross);
        if (area <= 0.0f)
            continue;
        area_total += area;

        // cross has magnitude 2*area, so half of it is already n_i * A_i.
        vec3 weighted;
        glm_vec3_scale(cross, 0.5f, weighted);
        glm_vec3_add(normal_sum, weighted, normal_sum);

        vec3 tri_centroid;
        glm_vec3_add(p0, p1, tri_centroid);
        glm_vec3_add(tri_centroid, p2, tri_centroid);
        glm_vec3_scale(tri_centroid, area / 3.0f, tri_centroid);
        glm_vec3_add(centroid_sum, tri_centroid, centroid_sum);
    }

    if (area_total <= 0.0f)
        return EMISSIVE_FIT_DEGENERATE;

    out->area = area_total;
    out->planarity = glm_vec3_norm(normal_sum) / area_total;
    if (out->planarity < EMISSIVE_FIT_MIN_PLANARITY)
        return EMISSIVE_FIT_NOT_PLANAR;

    vec3 normal;
    glm_vec3_copy(normal_sum, normal);
    glm_vec3_normalize(normal);

    vec3 centroid;
    glm_vec3_scale(centroid_sum, 1.0f / area_total, centroid);

    // Reference in-plane frame. Any pair will do -- the search below rotates
    // within the plane -- so this only has to be orthonormal, not meaningful.
    vec3 e0, ep;
    glm_vec3_ortho(normal, e0);
    glm_vec3_normalize(e0);
    glm_vec3_cross(normal, e0, ep);

    // MINIMUM-AREA rectangle, not the principal axis of the vertex covariance.
    // Covariance is the obvious choice and it fails on exactly the shape this
    // feature meets most: a SQUARE panel has an isotropic covariance, so the
    // axis it reports is arbitrary, and a square bounded at 45 degrees is
    // sqrt(2) too wide in both directions. The minimum-area rectangle has a
    // side flush with a hull edge, so testing the mesh's own edge directions
    // finds it without building a hull.
    float best_angle[EMISSIVE_FIT_ANGLE_BINS];
    bool bin_used[EMISSIVE_FIT_ANGLE_BINS];
    memset(bin_used, 0, sizeof(bin_used));

    // The reference frame itself is always a candidate, so a mesh whose edges
    // all project to nothing (every edge parallel to the normal, which cannot
    // happen on a planar mesh but costs one line to exclude) still gets a fit.
    best_angle[0] = 0.0f;
    bin_used[0] = true;

    for (size_t i = 0; i + 2 < mesh->index_count; i += 3) {
        for (int e = 0; e < 3; e++) {
            unsigned ia = mesh->indices[i + e];
            unsigned ib = mesh->indices[i + (e + 1) % 3];
            if (ia >= mesh->vertex_count || ib >= mesh->vertex_count)
                continue;
            vec3 pa = {0.0f, 0.0f, 0.0f}, pb = {0.0f, 0.0f, 0.0f}, d;
            _vertex(mesh, ia, pa);
            _vertex(mesh, ib, pb);
            glm_vec3_sub(pb, pa, d);
            float du = glm_vec3_dot(d, e0);
            float dv = glm_vec3_dot(d, ep);
            if (fabsf(du) < 1e-9f && fabsf(dv) < 1e-9f)
                continue;
            // Modulo pi: a rectangle's orientation repeats every half turn, so
            // an edge and its reverse are the same candidate.
            float angle = atan2f(dv, du);
            if (angle < 0.0f)
                angle += (float)M_PI;
            if (angle >= (float)M_PI)
                angle -= (float)M_PI;
            int bin = (int)(angle / (float)M_PI * EMISSIVE_FIT_ANGLE_BINS);
            if (bin < 0)
                bin = 0;
            if (bin >= EMISSIVE_FIT_ANGLE_BINS)
                bin = EMISSIVE_FIT_ANGLE_BINS - 1;
            if (!bin_used[bin]) {
                bin_used[bin] = true;
                best_angle[bin] = angle;
            }
        }
    }

    float best_area = FLT_MAX;
    vec3 best_u = {0.0f, 0.0f, 0.0f}, best_v = {0.0f, 0.0f, 0.0f};
    float best_umin = 0.0f, best_umax = 0.0f, best_vmin = 0.0f, best_vmax = 0.0f;
    for (int b = 0; b < EMISSIVE_FIT_ANGLE_BINS; b++) {
        if (!bin_used[b])
            continue;
        float c = cosf(best_angle[b]), s = sinf(best_angle[b]);
        vec3 u, v, t;
        glm_vec3_scale(e0, c, u);
        glm_vec3_scale(ep, s, t);
        glm_vec3_add(u, t, u);
        glm_vec3_scale(e0, -s, v);
        glm_vec3_scale(ep, c, t);
        glm_vec3_add(v, t, v);

        float umin, umax, vmin, vmax;
        float rect = _rect_for_axis(mesh, centroid, u, v, &umin, &umax, &vmin, &vmax);
        if (rect < best_area) {
            best_area = rect;
            glm_vec3_copy(u, best_u);
            glm_vec3_copy(v, best_v);
            best_umin = umin;
            best_umax = umax;
            best_vmin = vmin;
            best_vmax = vmax;
        }
    }

    // The panel's centre is the midpoint of the EXTENTS, not the area-weighted
    // centroid: an emissive mesh whose triangles are denser at one end pulls
    // the centroid there, and a rectangle centred on it would not bound the
    // geometry it was fitted to.
    vec3 center;
    glm_vec3_copy(centroid, center);
    vec3 offset;
    glm_vec3_scale(best_u, 0.5f * (best_umin + best_umax), offset);
    glm_vec3_add(center, offset, center);
    glm_vec3_scale(best_v, 0.5f * (best_vmin + best_vmax), offset);
    glm_vec3_add(center, offset, center);

    glm_vec3_copy(center, out->center);
    glm_vec3_copy(normal, out->normal);
    // `up` is the v axis, so cross(up, normal) recovers u -- the width axis the
    // extents below were measured along.
    glm_vec3_copy(best_v, out->up);
    out->size[0] = best_umax - best_umin;
    out->size[1] = best_vmax - best_vmin;

    return EMISSIVE_FIT_OK;
}

void emissive_material_radiance(const Material* material, vec3 out_nits) {
    glm_vec3_zero(out_nits);
    if (!material)
        return;

    // Mirrors what render.c uploads as `emissiveFactor`, including the fallback
    // where a black factor beside an emissive texture means "the texture IS the
    // colour". That value is what pbr_frag adds to Lo before the pre-exposure
    // multiply, so it is already scene radiance in nits -- the unit an area
    // panel's intensity is measured in. There is no conversion here and there
    // must not be one.
    glm_vec3_scale((float*)material->emissive, material->emissive_strength, out_nits);
    if (material->emissive_tex && glm_vec3_norm2((float*)material->emissive) < 1e-8f)
        glm_vec3_fill(out_nits, material->emissive_strength);
}

void emissive_radiance_to_light(const vec3 nits, vec3 out_color, float* out_intensity) {
    // Rec. 709 luminance, which is what makes the number read as nits rather
    // than as "the biggest channel".
    float lum = 0.2126f * nits[0] + 0.7152f * nits[1] + 0.0722f * nits[2];
    if (out_intensity)
        *out_intensity = lum;
    if (!out_color)
        return;
    if (lum <= 0.0f) {
        glm_vec3_one(out_color);
        return;
    }
    glm_vec3_scale((float*)nits, 1.0f / lum, out_color);
}

static void _probe_node(const SceneNode* node, int* count) {
    if (!node)
        return;

    for (size_t m = 0; m < node->mesh_count; m++) {
        const Mesh* mesh = node->meshes[m];
        if (!mesh || !mesh->material)
            continue;

        const char* node_name = node->name ? node->name : "(unnamed)";
        const char* mat_name = mesh->material->name ? mesh->material->name : "(unnamed)";

        vec3 nits = {0.0f, 0.0f, 0.0f};
        emissive_material_radiance(mesh->material, nits);
        vec3 color = {0.0f, 0.0f, 0.0f};
        float intensity = 0.0f;
        emissive_radiance_to_light(nits, color, &intensity);

        // Dimness is tested before geometry so a scene full of non-emissive
        // meshes reports nothing rather than reporting every wall as "not
        // planar" -- the reject list is only useful if it is short.
        if (intensity < EMISSIVE_FIT_MIN_NITS)
            continue;

        EmissivePanelFit fit;
        EmissiveFitReject reject = emissive_panel_fit(mesh, &fit);
        if (reject != EMISSIVE_FIT_OK) {
            printf("emissive-light-probe reject node=%s material=%s mesh=%u reason=\"%s\" "
                   "planarity=%.6f area=%.6f nits=%.6f\n",
                   node_name, mat_name, mesh->id, emissive_fit_reject_name(reject), fit.planarity,
                   fit.area, intensity);
            continue;
        }

        printf("emissive-light-probe panel node=%s material=%s mesh=%u "
               "center=%.6f,%.6f,%.6f normal=%.6f,%.6f,%.6f up=%.6f,%.6f,%.6f "
               "size=%.6f,%.6f planarity=%.6f area=%.6f nits=%.6f color=%.6f,%.6f,%.6f\n",
               node_name, mat_name, mesh->id, fit.center[0], fit.center[1], fit.center[2],
               fit.normal[0], fit.normal[1], fit.normal[2], fit.up[0], fit.up[1], fit.up[2],
               fit.size[0], fit.size[1], fit.planarity, fit.area, intensity, color[0], color[1],
               color[2]);
        (*count)++;
    }

    for (size_t c = 0; c < node->children_count; c++)
        _probe_node(node->children[c], count);
}

void emissive_lights_probe(const Scene* scene) {
    if (!scene || !scene->root_node) {
        printf("emissive-light-probe header count=0 reason=noscene\n");
        return;
    }

    // The count is printed AFTER the rows it counts, because the walk is what
    // produces it and a header promising a number the rows then contradict is
    // the failure mode a gate cannot see.
    int count = 0;
    _probe_node(scene->root_node, &count);
    printf("emissive-light-probe header count=%d\n", count);
}
