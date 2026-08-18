
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emissive_light.h"
#include "draw_list.h"
#include "light.h"
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
    // One token, no spaces: these are values in a key=value line, and a reader
    // that splits on whitespace -- which is every reader of this format -- turns
    // "not planar" into a truncated reason and a bare word it cannot place.
    case EMISSIVE_FIT_NO_GEOMETRY:
        return "no-indexed-triangles";
    case EMISSIVE_FIT_DEGENERATE:
        return "zero-area";
    case EMISSIVE_FIT_NOT_PLANAR:
        return "not-planar";
    case EMISSIVE_FIT_NOT_FILLED:
        return "not-filled";
    case EMISSIVE_FIT_TOO_DIM:
        return "too-dim";
    case EMISSIVE_FIT_OPTED_OUT:
        return "opted-out";
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

    // FILL: does the mesh occupy the rectangle it was fitted to, or does the
    // rectangle merely span it? Planarity above is a flatness test and answers
    // nothing about this -- two coplanar strips with a gap between them are
    // perfectly flat, and the panel they fit radiates from the empty middle.
    //
    // Computed here rather than in the search because best_area is the winning
    // rectangle and area_total is the mesh's own; both are already in hand.
    out->fill = best_area > 0.0f ? area_total / best_area : 0.0f;
    if (out->fill < EMISSIVE_FIT_MIN_FILL)
        return EMISSIVE_FIT_NOT_FILLED;

    glm_vec3_copy(center, out->center);
    glm_vec3_copy(normal, out->normal);
    // `up` is the v axis, so cross(up, normal) recovers u -- the width axis the
    // extents below were measured along.
    glm_vec3_copy(best_v, out->up);
    out->size[0] = best_umax - best_umin;
    out->size[1] = best_vmax - best_vmin;

    return EMISSIVE_FIT_OK;
}

bool emissive_material_radiance(Material* material, vec3 out_nits) {
    glm_vec3_zero(out_nits);
    if (!material)
        return true;

    // Mirrors what render.c uploads as `emissiveFactor`, including the fallback
    // where a black factor beside an emissive texture means "the texture IS the
    // colour". That value is what pbr_frag adds to Lo before the pre-exposure
    // multiply, so it is already scene radiance in nits -- the unit an area
    // panel's intensity is measured in. There is no conversion here and there
    // must not be one.
    material_emissive_factor(material, out_nits);

    // pbr_frag multiplies that factor by the texel, so the surface's MEAN
    // radiance is the factor times the texture's mean -- which is the 1x1 top
    // mip. Without it a mostly-black emissive atlas under a bright factor
    // reports the factor as if the whole panel were lit, and over-states the
    // lamp by whatever fraction of the map is actually emitting: a screen
    // showing one bright window would light a room like a screen showing white.
    //
    // Declining leaves the factor alone rather than zeroing, so a texture whose
    // chain is missing over-states rather than vanishing -- the same direction
    // the code had before this existed.
    if (material->emissive_tex) {
        vec3 mean = {1.0f, 1.0f, 1.0f};
        if (!texture_mean_color(material->emissive_tex, mean))
            return false;
        glm_vec3_mul(out_nits, mean, out_nits);
    }
    return true;
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

// Whether this mesh should carry a derived panel at all, and its radiance if so.
// The material's verdict and the dimness floor together, because a caller that
// asked them separately would have to remember the order they compose in.
static EmissiveFitReject _mesh_candidacy(Mesh* mesh, vec3 out_nits, bool* out_final) {
    glm_vec3_zero(out_nits);
    if (out_final)
        *out_final = true;
    if (!mesh || !mesh->material)
        return EMISSIVE_FIT_TOO_DIM;

    bool final = emissive_material_radiance(mesh->material, out_nits);
    if (out_final)
        *out_final = final;
    float lum = 0.0f;
    emissive_radiance_to_light(out_nits, NULL, &lum);

    // Dimness BEFORE the material's verdict, in that order for two reasons. It
    // fills out_nits either way, so a reject can report the radiance it turned
    // down rather than a zero. And it keeps the opt-out reject meaning what it
    // says: a scene that set the key on a whole material library would otherwise
    // report every wall in it as opted out, and a reject list nobody can read is
    // the same as no reject list.
    if (lum < EMISSIVE_FIT_MIN_NITS)
        return EMISSIVE_FIT_TOO_DIM;
    return mesh->material->emissive_light != 0 ? EMISSIVE_FIT_OPTED_OUT : EMISSIVE_FIT_OK;
}

static void _probe_node(const Scene* scene, const SceneNode* node, int* count) {
    if (!node)
        return;

    for (size_t m = 0; m < node->mesh_count; m++) {
        Mesh* mesh = node->meshes[m];
        if (!mesh || !mesh->material)
            continue;

        const char* node_name = node->name ? node->name : "(unnamed)";
        const char* mat_name = mesh->material->name ? mesh->material->name : "(unnamed)";

        vec3 nits = {0.0f, 0.0f, 0.0f};
        // The same verdict the reconcile reaches, so the probe cannot report a
        // panel the renderer declines to build. Dimness is inside it and is
        // tested before geometry, so a scene full of non-emissive meshes reports
        // nothing rather than calling every wall "not planar" -- a reject list is
        // only useful if it is short.
        bool final = true;
        EmissiveFitReject reject = _mesh_candidacy(mesh, nits, &final);
        if (reject == EMISSIVE_FIT_TOO_DIM)
            continue;

        vec3 color = {0.0f, 0.0f, 0.0f};
        float intensity = 0.0f;
        emissive_radiance_to_light(nits, color, &intensity);

        EmissivePanelFit fit;
        memset(&fit, 0, sizeof(fit));
        if (reject == EMISSIVE_FIT_OK)
            reject = emissive_panel_fit(mesh, &fit);
        if (reject != EMISSIVE_FIT_OK) {
            printf("emissive-light-probe reject node=%s material=%s mesh=%u reason=%s "
                   "planarity=%.6f fill=%.6f area=%.6f nits=%.6f\n",
                   node_name, mat_name, mesh->id, emissive_fit_reject_name(reject), fit.planarity,
                   fit.fill, fit.area, intensity);
            continue;
        }

        printf("emissive-light-probe panel node=%s material=%s mesh=%u "
               "center=%.6f,%.6f,%.6f normal=%.6f,%.6f,%.6f up=%.6f,%.6f,%.6f "
               "space=local size=%.6f,%.6f planarity=%.6f fill=%.6f area=%.6f nits=%.6f "
               "radiance=%s "
               "color=%.6f,%.6f,%.6f\n",
               node_name, mat_name, mesh->id, fit.center[0], fit.center[1], fit.center[2],
               fit.normal[0], fit.normal[1], fit.normal[2], fit.up[0], fit.up[1], fit.up[2],
               fit.size[0], fit.size[1], fit.planarity, fit.fill, fit.area, intensity,
               final ? "final" : "pending", color[0], color[1], color[2]);
        (*count)++;

        // The panel line above is the LOCAL fit. Saying so is not pedantry: the
        // arm that reads it asserts world numbers taken from a generator, and the
        // two agree only because every emissive fixture node is identity-
        // transformed. A second line reports where the panel actually ENDED UP,
        // which is the half a fit report cannot see -- and the half where the
        // field-aliasing defect this module shipped with lived, undetected by a
        // green nine-arm group.
        const Light* placed = NULL;
        for (size_t i = 0; i < scene->light_count && !placed; i++)
            if (scene->lights[i] && scene->lights[i]->emissive_source_id == mesh->id)
                placed = scene->lights[i];
        if (placed)
            printf("emissive-light-probe placed node=%s mesh=%u space=world "
                   "center=%.6f,%.6f,%.6f normal=%.6f,%.6f,%.6f up=%.6f,%.6f,%.6f "
                   "size=%.6f,%.6f nits=%.6f\n",
                   node_name, mesh->id, placed->global_position[0], placed->global_position[1],
                   placed->global_position[2], placed->direction[0], placed->direction[1],
                   placed->direction[2], placed->up[0], placed->up[1], placed->up[2],
                   placed->size[0], placed->size[1], placed->intensity);
    }

    for (size_t c = 0; c < node->children_count; c++)
        _probe_node(scene, node->children[c], count);
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
    _probe_node(scene, scene->root_node, &count);
    printf("emissive-light-probe header count=%d\n", count);
}

/*
 * The panel registry: where a derived panel's LOCAL fit lives.
 *
 * It did not exist, and its absence was the module's one real defect. The fit
 * was stashed in Light.original_position/_direction/_up and Light.size and read
 * back as local on the next frame -- but _place writes WORLD values into those
 * same four fields, so from frame two the "local" fit was a world fit and the
 * node transform was re-applied to it every frame, compounding. A translated node
 * walked its panel away; a scaled one doubled or vanished it; a rotated one spun
 * its normal. Invisible in the whole corpus because every emissive fixture node
 * is identity-transformed, which is the exact fixed point where world == local.
 *
 * light.h defines original_* as the AUTHORED pre-transform value and scene.c
 * treats it as read-only; Light.size has no local/world split at all, so there
 * was nowhere for a local size to survive even in principle. That missing slot is
 * what forced the write-through-and-read-back in the first place.
 *
 * With the fit owned here, _place writes only world outputs and becomes
 * re-runnable -- which the six cube-capture re-entries per face already assumed.
 */
typedef struct EmissivePanel {
    unsigned mesh_id; // stable across frames; 0 is never a real mesh
    SceneNode* node;  // borrowed; re-resolved on every rebuild
    Light* light;     // borrowed; the Scene owns it
    EmissivePanelFit fit; // LOCAL to the mesh, and it stays local
    uint64_t fit_epoch;   // graph epoch `fit` was solved at; 0 = never
    uint64_t seen;        // reconcile pass that last touched this panel
} EmissivePanel;

struct EmissivePanels {
    EmissivePanel* items;
    size_t count, capacity;
    uint64_t pass; // bumped per reconcile; a panel not stamped with it is gone
};

void emissive_panels_free(struct EmissivePanels* panels) {
    if (!panels)
        return;
    free(panels->items);
    free(panels);
}

static EmissivePanel* _panel_for(struct EmissivePanels* panels, unsigned mesh_id) {
    for (size_t i = 0; i < panels->count; i++)
        if (panels->items[i].mesh_id == mesh_id)
            return &panels->items[i];
    return NULL;
}

// Swap-remove: the registry has no ordering contract, unlike scene->lights,
// whose walk order light_cluster.c documents as what makes packing deterministic.
static void _panel_drop(struct EmissivePanels* panels, EmissivePanel* panel) {
    if (panel->light)
        panel->light = NULL; // the caller owns whether the Light itself goes
    *panel = panels->items[panels->count - 1];
    panels->count--;
}

static EmissivePanel* _panel_add(struct EmissivePanels* panels, unsigned mesh_id) {
    if (panels->count == panels->capacity) {
        size_t want = panels->capacity ? panels->capacity * 2 : 8;
        EmissivePanel* grown = realloc(panels->items, want * sizeof(*grown));
        if (!grown)
            return NULL;
        panels->items = grown;
        panels->capacity = want;
    }
    EmissivePanel* p = &panels->items[panels->count++];
    memset(p, 0, sizeof(*p));
    p->mesh_id = mesh_id;
    return p;
}

// World placement, from the panel's own local fit. Writes ONLY world outputs:
// nothing here feeds the next frame's input, which is what makes it re-runnable
// and what the field aliasing above got wrong.
//
// The two axes carry their EXTENT, so one product delivers rotation and scale
// together and a scaled node scales its panel. The normal goes through the node's
// normal_matrix instead, which is what a normal needs under non-uniform scale.
static void _place(Light* light, const SceneNode* node, const EmissivePanelFit* fit) {
    vec3 u, v;
    glm_vec3_cross((float*)fit->up, (float*)fit->normal, u); // the width axis
    glm_vec3_scale(u, fit->size[0], u);
    glm_vec3_scale((float*)fit->up, fit->size[1], v);

    mat4* xf = (mat4*)&node->global_transform; // cglm takes mat4 non-const
    vec3 world_u, world_v, world_c;
    glm_mat4_mulv3(*xf, u, 0.0f, world_u);
    glm_mat4_mulv3(*xf, v, 0.0f, world_v);
    glm_mat4_mulv3(*xf, (float*)fit->center, 1.0f, world_c);

    glm_vec3_copy(world_c, light->global_position);
    set_light_size(light, glm_vec3_norm(world_u), glm_vec3_norm(world_v));

    vec3 world_n;
    glm_mat3_mulv((float(*)[3])node->normal_matrix, (float*)fit->normal, world_n);
    if (glm_vec3_norm(world_n) < 1e-8f)
        glm_vec3_copy((float*)fit->normal, world_n);
    glm_vec3_normalize(world_n);
    glm_vec3_copy(world_n, light->direction);

    if (glm_vec3_norm(world_v) < 1e-8f)
        glm_vec3_copy((float*)fit->up, world_v);
    glm_vec3_normalize(world_v);
    glm_vec3_copy(world_v, light->up);
}

// One walk, and it is the only one. There were three -- clear the marks, collect
// the survivors, reconcile -- two of which evaluated candidacy over every mesh,
// which is also what made a textured emitter pay two synchronous GL readbacks a
// frame. Marking unconditionally rather than only on the candidate path is what
// lets the clear pass go: a mesh that stops being a candidate is un-marked by the
// same statement that would have marked it, so a stale mark cannot exist.
static void _walk(Scene* scene, SceneNode* node, bool enabled, uint64_t epoch, int* live) {
    if (!node)
        return;

    for (size_t m = 0; m < node->mesh_count; m++) {
        Mesh* mesh = node->meshes[m];
        if (!mesh)
            continue;

        vec3 nits = {0.0f, 0.0f, 0.0f};
        bool candidate =
            enabled && _mesh_candidacy(mesh, nits, NULL) == EMISSIVE_FIT_OK;

        EmissivePanel* panel = candidate ? _panel_for(scene->emissive_panels, mesh->id) : NULL;
        if (candidate && !panel) {
            panel = _panel_add(scene->emissive_panels, mesh->id);
            candidate = panel != NULL;
        }

        // Re-fit when the graph moved, and when this panel is new. A REJECT is
        // recorded by dropping the panel, so a shape the fit refuses is retried
        // once per graph change rather than once per frame -- the fit is an
        // O(bins x vertices) search and a glowing box would otherwise pay it
        // forever.
        if (candidate && (panel->fit_epoch != epoch)) {
            if (emissive_panel_fit(mesh, &panel->fit) == EMISSIVE_FIT_OK) {
                panel->fit_epoch = epoch;
            } else {
                // The fit refused the shape. Drop the panel AND its light: a
                // mesh that stopped being a rectangle is no longer a lamp.
                if (panel->light)
                    remove_light_from_scene(scene, panel->light);
                _panel_drop(scene->emissive_panels, panel);
                candidate = false;
            }
        }

        mesh->emissive_derived = candidate;
        if (!candidate)
            continue;

        panel->node = node;
        panel->seen = scene->emissive_panels->pass;

        if (!panel->light) {
            Light* light = create_light();
            if (!light) {
                _panel_drop(scene->emissive_panels, panel);
                mesh->emissive_derived = false;
                continue;
            }
            set_light_type(light, LIGHT_AREA);
            light->emissive_source_id = mesh->id;
            // Named for the NODE, not the material: a name has to identify one
            // lamp for light_overrides to address it, and a material is shared by
            // every mesh that uses it.
            set_light_name(light, node->name ? node->name : "emissive");
            if (add_light_to_scene(scene, light) != 0) {
                free_light(light);
                _panel_drop(scene->emissive_panels, panel);
                mesh->emissive_derived = false;
                continue;
            }
            panel->light = light;
        }

        _place(panel->light, node, &panel->fit);
        emissive_radiance_to_light(nits, panel->light->color, &panel->light->intensity);
        panel->light->units = LIGHT_UNITS_NITS;
        (*live)++;
    }

    for (size_t c = 0; c < node->children_count; c++)
        _walk(scene, node->children[c], enabled, epoch, live);
}

int scene_build_emissive_lights(Scene* scene, bool enabled) {
    if (!scene || !scene->root_node)
        return 0;

    if (!scene->emissive_panels) {
        // Nothing to tear down and nothing to build: the common case is a scene
        // with the feature off and no registry, and it costs one branch.
        if (!enabled)
            return 0;
        scene->emissive_panels = calloc(1, sizeof(*scene->emissive_panels));
        if (!scene->emissive_panels)
            return 0;
    }

    struct EmissivePanels* panels = scene->emissive_panels;
    panels->pass++;

    int live = 0;
    _walk(scene, scene->root_node, enabled, scene_graph_epoch(), &live);

    // Sweep AFTER the walk, on the stamp the walk just wrote. The previous shape
    // swept first against a separately-collected id array sized from
    // scene->light_count + 64 -- a set of MESH ids sized from a count of LIGHTS,
    // which silently truncated, and whose calloc failing removed every derived
    // light (losing exactly the light_overrides state reconcile-not-rebuild
    // exists to keep). Nothing reads scene->lights between the two, so the old
    // ordering bought nothing it claimed.
    for (size_t i = panels->count; i-- > 0;) {
        if (panels->items[i].seen == panels->pass)
            continue;
        if (panels->items[i].light)
            remove_light_from_scene(scene, panels->items[i].light);
        panels->items[i] = panels->items[panels->count - 1];
        panels->count--;
    }

    return live;
}
