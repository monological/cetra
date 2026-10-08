#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cetra/mesh.h"

#include "kit.h"

void kit_init(Kit* kit, Scene* scene, EntityManager* em, PhysicsWorld* physics) {
    memset(kit, 0, sizeof(*kit));
    kit->scene = scene;
    kit->em = em;
    kit->physics = physics;
    kit->shadow_cell_scale = 1.0f;
}

void kit_init_beside(Kit* kit, Kit* first, const vec3 origin) {
    kit_init(kit, first->scene, first->em, first->physics);
    glm_vec3_copy((float*)origin, kit->origin);
    for (int i = 0; i < first->material_count; i++)
        kit_material(kit, first->materials[i], first->repeat_m[i], first->grime[i]);
    kit->shares_materials = first->shares_materials = true;
}

void kit_free_unused(Kit* const* kits, int count) {
    if (count <= 0)
        return;
    for (int i = 0; i < kits[0]->material_count; i++) {
        bool used = false;
        for (int k = 0; k < count; k++)
            used = used || kits[k]->used[i];
        if (used)
            continue;
        free_material(kits[0]->materials[i]);
        for (int k = 0; k < count; k++)
            kits[k]->materials[i] = NULL;
    }
}

int kit_material(Kit* kit, Material* material, float repeat_m, float grime) {
    if (kit->material_count >= KIT_MAX_MATERIALS) {
        fprintf(stderr, "silent: kit is out of material slots (%d)\n", KIT_MAX_MATERIALS);
        return 0;
    }
    const int slot = kit->material_count++;
    kit->materials[slot] = material;
    kit->repeat_m[slot] = repeat_m > 0.0f ? repeat_m : 1.0f;
    kit->grime[slot] = glm_clamp(grime, 0.0f, 1.0f);
    mb_init(&kit->builders[slot], 256, 384, kit->grime[slot] > 0.0f);
    return slot;
}

/*
 * Grime (spec 13.8): dirt gathers where surfaces meet -- round a door's edge,
 * in the gap between two doors, where a cupboard stands on the floor. A grimed
 * box's face is cut into a grid: a ring GRIME_BAND wide round its edges, a cut
 * inside that ring so the edge can carry a hard dark line with a softer tail,
 * and the interior every GRIME_STEP so the noise has vertices to vary across.
 *
 * Uniform, that reads as a painted frame round every face, so three things
 * break it up. A smooth PATCH field over the room decides how dirty each stretch
 * of edge is at all, so some runs are caked and some nearly clean. The band's
 * reach is jittered per vertex, so the dirt bleeds in further in places. And
 * two edges ADD where they meet, so corners collect the most. Every amount is a
 * pure function of the vertex's world position, so a seed is still one world.
 */
#define GRIME_BAND     0.06f // metres
#define GRIME_STEP     0.12f // metres between interior cuts
#define GRIME_BOTTOM   1.5f  // dirt settles: an upright face's lowest edge takes more
#define GRIME_TOP      0.5f  // and its highest less
#define GRIME_SMUDGE   0.35f // the most a smudge in a face's interior reaches
#define GRIME_PATCH_M  0.45f // the patch field's cell, metres
#define GRIME_MAX_CUTS 64
static const float GRIME_TINT[3] = {0.28f, 0.22f, 0.15f}; // what the dirt is, sRGB

// A value in [0, 1) from a world position, stable to the millimetre, so two
// faces meeting at a corner agree about it.
static float grime_hash(const vec3 p, uint32_t salt) {
    uint32_t h = (uint32_t)(int32_t)lroundf(p[0] * 1000.0f) * 73856093u ^
                 (uint32_t)(int32_t)lroundf(p[1] * 1000.0f) * 19349663u ^
                 (uint32_t)(int32_t)lroundf(p[2] * 1000.0f) * 83492791u ^ salt;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (float)(h & 0xffffffu) * (1.0f / 16777216.0f);
}

// Smooth value noise over GRIME_PATCH_M cells, in [0, 1): the hash at the
// lattice corners, blended trilinearly with a smoothstep, so neighbouring
// vertices agree and the field has stretches rather than speckle.
static float grime_patch(const vec3 p) {
    float cell[3], f[3];
    for (int k = 0; k < 3; k++) {
        const float s = p[k] / GRIME_PATCH_M;
        cell[k] = floorf(s);
        const float t = s - cell[k];
        f[k] = t * t * (3.0f - 2.0f * t);
    }
    float acc = 0.0f;
    for (int c = 0; c < 8; c++) {
        const vec3 corner = {(cell[0] + (float)(c & 1)) * GRIME_PATCH_M,
                             (cell[1] + (float)((c >> 1) & 1)) * GRIME_PATCH_M,
                             (cell[2] + (float)((c >> 2) & 1)) * GRIME_PATCH_M};
        float w = 1.0f;
        for (int k = 0; k < 3; k++)
            w *= ((c >> k) & 1) ? f[k] : 1.0f - f[k];
        acc += w * grime_hash(corner, 0x85ebca6bu);
    }
    return acc;
}

/*
 * The planar frame a face is textured in. Walls get a horizontal U and an
 * upward V, so anything painted downward in a texture (drips, damp) runs down
 * the wall whichever way it faces. Floors and ceilings take world X as U.
 */
static void face_frame(const vec3 n, vec3 t, vec3 b) {
    if (fabsf(n[1]) > 0.7f) {
        vec3 x = {1.0f, 0.0f, 0.0f};
        glm_vec3_scale((float*)n, glm_vec3_dot(x, (float*)n), t);
        glm_vec3_sub(x, t, t);
    } else {
        glm_vec3_cross((vec3){0.0f, 1.0f, 0.0f}, (float*)n, t);
    }
    glm_vec3_normalize(t);
    glm_vec3_cross((float*)n, t, b);
}

// `grime` is 0..1; a builder without colours ignores it. UVs in repeats. A position that is not
// finite is reported, once a kit, naming its material: it reaches the GPU as a band of black
// that the tonemap clamps, nowhere near the solid that made it.
static unsigned int kit_vertex(Kit* kit, int mat, const vec3 p, const vec3 n, const vec3 t, float u,
                               float v, float grime) {
    if (!kit->warned_nonfinite && !(isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]))) {
        const Material* m = kit->materials[mat];
        fprintf(stderr, "silent: a vertex of %s is not finite\n",
                m && m->name ? m->name : "an unnamed material");
        kit->warned_nonfinite = true;
    }
    const float rgba[4] = {1.0f + (GRIME_TINT[0] - 1.0f) * grime,
                           1.0f + (GRIME_TINT[1] - 1.0f) * grime,
                           1.0f + (GRIME_TINT[2] - 1.0f) * grime, 1.0f};
    vec3 at;
    glm_vec3_add((float*)p, kit->origin, at);
    return mb_vertex(&kit->builders[mat], at, n, t, u, v, u, v, rgba);
}

// A vertex of a flat face, UV'd by projecting onto the face's planar frame.
static unsigned int face_vertex(Kit* kit, int mat, const vec3 p, const vec3 n, const vec3 t,
                                const vec3 b, float grime) {
    const float inv = 1.0f / kit->repeat_m[mat];
    return kit_vertex(kit, mat, p, n, t, glm_vec3_dot((float*)p, (float*)t) * inv,
                      glm_vec3_dot((float*)p, (float*)b) * inv, grime);
}

static bool slot_ok(const Kit* kit, int mat) {
    return mat >= 0 && mat < kit->material_count;
}

// The unnormalised normal of the triangle p0 p1 p2, counter-clockwise.
static void corner_cross(const vec3 p0, const vec3 p1, const vec3 p2, vec3 n) {
    vec3 e1, e2;
    glm_vec3_sub((float*)p1, (float*)p0, e1);
    glm_vec3_sub((float*)p2, (float*)p0, e2);
    glm_vec3_cross(e1, e2, n);
}

/*
 * A flat polygon of 3 or 4 corners, fanned from the first, wound to face
 * `outward` whatever order the corners come in -- so the solids below list
 * them in whatever order is easiest to read. The normal is taken from the
 * corners in the order they are emitted.
 */
static void emit_face(Kit* kit, int mat, const vec3* in, int count, const vec3 outward) {
    if (!slot_ok(kit, mat))
        return;
    vec3 p[4], n = {0.0f, 0.0f, 0.0f}, t = {0.0f, 0.0f, 0.0f}, bt = {0.0f, 0.0f, 0.0f};
    corner_cross(in[0], in[1], in[2], n);
    const bool keep = glm_vec3_dot(n, (float*)outward) >= 0.0f;
    for (int i = 0; i < count; i++)
        glm_vec3_copy((float*)in[keep ? i : count - 1 - i], p[i]);
    corner_cross(p[0], p[1], p[2], n);
    if (glm_vec3_norm2(n) < 1e-12f)
        return;
    glm_vec3_normalize(n);
    face_frame(n, t, bt);
    unsigned int idx[4];
    for (int i = 0; i < count; i++)
        idx[i] = face_vertex(kit, mat, p[i], n, t, bt, 0.0f);
    for (int i = 2; i < count; i++)
        mb_tri(&kit->builders[mat], idx[0], idx[i - 1], idx[i]);
}

// Where to cut an edge `len` long: both ends, a cut a third of the way into
// the ring and the ring's inner line at each, and the interior every
// GRIME_STEP. An edge shorter than one step -- a stud, a sill's end, a merlon --
// is cut once across its middle, so its dirt fades to the middle rather than
// drawing a rim, for a ninth of the vertices on a face short both ways.
// Returns the count, at most GRIME_MAX_CUTS.
static int grime_cuts(float len, float band, float* out) {
    int n = 0;
    out[n++] = 0.0f;
    if (len < GRIME_STEP) {
        out[n++] = 0.5f * len;
        out[n++] = len;
        return n;
    }
    out[n++] = 0.35f * band;
    out[n++] = band;
    const float inner = len - 2.0f * band;
    int steps = (int)floorf(inner / GRIME_STEP);
    if (steps > GRIME_MAX_CUTS - 6)
        steps = GRIME_MAX_CUTS - 6;
    for (int i = 1; i <= steps; i++)
        out[n++] = band + inner * (float)i / (float)(steps + 1);
    out[n++] = len - band;
    out[n++] = len - 0.35f * band;
    out[n++] = len;
    return n;
}

// Full on an edge and falling off fast: a hard dark line with a soft tail,
// all but gone `reach` in.
static float grime_falloff(float d, float reach) {
    return expf(-3.0f * d / reach);
}

// How far in the dirt reaches at p, as a multiple of the band: jittered per
// vertex, so the line it leaves wanders.
static float grime_reach(const vec3 p) {
    return 0.4f + 1.6f * grime_hash(p, 0u);
}

// The dirt at p, given how much edge it is near: the patch field decides how
// dirty this stretch is at all, and a sparse smudge can land anywhere.
static float grime_amount(float strength, const vec3 p, float edge) {
    const float patch = 0.1f + 1.8f * glm_smoothstep(0.3f, 0.7f, grime_patch(p));
    const float smudge = GRIME_SMUDGE * fmaxf(0.0f, grime_hash(p, 0x9e3779b9u) - 0.7f) / 0.3f;
    return glm_clamp(strength * (patch * (edge + 0.12f) + smudge), 0.0f, 1.0f);
}

/*
 * One face of a grimed box: the rectangle from c0 along edge_a and edge_b, cut
 * by grime_cuts and wound to face `outward`. On a face that stands upright,
 * the edge lowest in the world takes GRIME_BOTTOM and the highest GRIME_TOP.
 */
static void emit_grimed_face(Kit* kit, int mat, const vec3 c0, const vec3 edge_a, const vec3 edge_b,
                             const vec3 outward) {
    vec3 ea, eb, n;
    glm_vec3_copy((float*)edge_a, ea);
    glm_vec3_copy((float*)edge_b, eb);
    glm_vec3_cross(ea, eb, n);
    if (glm_vec3_norm2(n) < 1e-12f)
        return;
    if (glm_vec3_dot(n, (float*)outward) < 0.0f) {
        glm_vec3_copy((float*)edge_b, ea);
        glm_vec3_copy((float*)edge_a, eb);
        glm_vec3_negate(n);
    }
    glm_vec3_normalize(n);
    vec3 t = {0.0f, 0.0f, 0.0f}, bt = {0.0f, 0.0f, 0.0f};
    face_frame(n, t, bt);

    const float la = glm_vec3_norm(ea), lb = glm_vec3_norm(eb);
    const float band_a = fminf(GRIME_BAND, 0.3f * la), band_b = fminf(GRIME_BAND, 0.3f * lb);
    float cut_a[GRIME_MAX_CUTS], cut_b[GRIME_MAX_CUTS];
    const int na = grime_cuts(la, band_a, cut_a), nb = grime_cuts(lb, band_b, cut_b);

    // The four edges -- a = 0, a = la, b = 0, b = lb -- by the height of their
    // midpoints, when the face stands.
    float weight[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (fabsf(n[1]) < 0.7f) {
        const float mid[4] = {c0[1] + 0.5f * eb[1], c0[1] + ea[1] + 0.5f * eb[1],
                              c0[1] + 0.5f * ea[1], c0[1] + eb[1] + 0.5f * ea[1]};
        int lo = 0, hi = 0;
        for (int e = 1; e < 4; e++) {
            lo = mid[e] < mid[lo] ? e : lo;
            hi = mid[e] > mid[hi] ? e : hi;
        }
        weight[lo] = GRIME_BOTTOM;
        weight[hi] = GRIME_TOP;
    }

    const float strength = kit->grime[mat];
    unsigned int row[2][GRIME_MAX_CUTS];
    for (int j = 0; j < nb; j++) {
        for (int i = 0; i < na; i++) {
            const float a = cut_a[i], b = cut_b[j];
            vec3 p;
            glm_vec3_copy((float*)c0, p);
            glm_vec3_muladds(ea, a / la, p);
            glm_vec3_muladds(eb, b / lb, p);
            const float reach = grime_reach(p);
            const float edge = weight[0] * grime_falloff(a, band_a * reach) +
                               weight[1] * grime_falloff(la - a, band_a * reach) +
                               weight[2] * grime_falloff(b, band_b * reach) +
                               weight[3] * grime_falloff(lb - b, band_b * reach);
            row[j & 1][i] = face_vertex(kit, mat, p, n, t, bt, grime_amount(strength, p, edge));
        }
        if (j == 0)
            continue;
        const unsigned int* lo_row = row[(j - 1) & 1];
        const unsigned int* hi_row = row[j & 1];
        for (int i = 1; i < na; i++) {
            mb_tri(&kit->builders[mat], lo_row[i - 1], lo_row[i], hi_row[i]);
            mb_tri(&kit->builders[mat], lo_row[i - 1], hi_row[i], hi_row[i - 1]);
        }
    }
}

void kit_quad_facing(Kit* kit, int mat, const vec3 a, const vec3 b, const vec3 c, const vec3 d,
                     const vec3 outward) {
    const vec3 p[4] = {
        {a[0], a[1], a[2]}, {b[0], b[1], b[2]}, {c[0], c[1], c[2]}, {d[0], d[1], d[2]}};
    emit_face(kit, mat, p, 4, outward);
}

void kit_tri_facing(Kit* kit, int mat, const vec3 a, const vec3 b, const vec3 c,
                    const vec3 outward) {
    const vec3 p[3] = {{a[0], a[1], a[2]}, {b[0], b[1], b[2]}, {c[0], c[1], c[2]}};
    emit_face(kit, mat, p, 3, outward);
}

// Twice the signed area of the triangle a b c in the plane: positive when counter-clockwise.
static float area2(const vec2 a, const vec2 b, const vec2 c) {
    return (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
}

// Whether p lies inside or on the counter-clockwise triangle a b c.
static bool in_triangle(const vec2 p, const vec2 a, const vec2 b, const vec2 c) {
    return area2(a, b, p) >= 0.0f && area2(b, c, p) >= 0.0f && area2(c, a, p) >= 0.0f;
}

/*
 * Ear clipping: a counter-clockwise polygon's `n` corners into n - 2 triangles, as indices
 * into `pts`. An ear is a convex corner whose triangle holds no other corner; cutting one
 * leaves a smaller polygon that still has one, which is what makes the loop end. Corners in a
 * straight line make ears of no area, which are cut like any other. Should the outline cross
 * itself and leave no ear, the rest is fanned rather than looped on forever.
 */
static int ear_clip(const vec2* pts, int n, unsigned int (*tris)[3]) {
    int left[KIT_MAX_OUTLINE];
    for (int i = 0; i < n; i++)
        left[i] = i;
    int count = 0, m = n;
    while (m > 3) {
        int ear = -1;
        for (int i = 0; i < m && ear < 0; i++) {
            const int a = left[(i + m - 1) % m], b = left[i], c = left[(i + 1) % m];
            if (area2(pts[a], pts[b], pts[c]) < 0.0f)
                continue;
            bool empty = true;
            for (int j = 0; j < m && empty; j++) {
                const int q = left[j];
                if (q != a && q != b && q != c && in_triangle(pts[q], pts[a], pts[b], pts[c]))
                    empty = false;
            }
            if (empty)
                ear = i;
        }
        if (ear < 0)
            break;
        tris[count][0] = (unsigned int)left[(ear + m - 1) % m];
        tris[count][1] = (unsigned int)left[ear];
        tris[count][2] = (unsigned int)left[(ear + 1) % m];
        count++;
        memmove(&left[ear], &left[ear + 1], (size_t)(m - ear - 1) * sizeof(int));
        m--;
    }
    for (int i = 1; i + 1 < m; i++) {
        tris[count][0] = (unsigned int)left[0];
        tris[count][1] = (unsigned int)left[i];
        tris[count][2] = (unsigned int)left[i + 1];
        count++;
    }
    return count;
}

// Newell's normal of a closed outline, unnormalised: the way round it runs, which a concave
// outline's first three corners cannot be trusted for.
static void newell(const vec3* corners, int count, vec3 n) {
    glm_vec3_zero(n);
    for (int i = 0; i < count; i++) {
        const float* p = corners[i];
        const float* q = corners[(i + 1) % count];
        n[0] += (p[1] - q[1]) * (p[2] + q[2]);
        n[1] += (p[2] - q[2]) * (p[0] + q[0]);
        n[2] += (p[0] - q[0]) * (p[1] + q[1]);
    }
}

// Whether an outline has a corner count the kit can take, saying so when it has not.
static bool outline_ok(int count) {
    if (count >= 3 && count <= KIT_MAX_OUTLINE)
        return true;
    fprintf(stderr, "silent: an outline needs 3 to %d corners, not %d\n", KIT_MAX_OUTLINE, count);
    return false;
}

/*
 * A flat polygon, ear-clipped and wound to face `outward`. Its UVs are the face's world
 * projection, or `uv` per corner with `tangent` the direction U runs, for a picture laid on
 * it.
 */
static void polygon(Kit* kit, int mat, const vec3* corners, int count, const vec3 outward,
                    const vec2* uv, const vec3 tangent) {
    if (!slot_ok(kit, mat) || !outline_ok(count))
        return;
    vec3 n = {0.0f, 0.0f, 0.0f};
    newell(corners, count, n);
    if (glm_vec3_norm2(n) < 1e-12f)
        return;
    const bool flip = glm_vec3_dot(n, (float*)outward) < 0.0f;
    if (flip)
        glm_vec3_negate(n);
    glm_vec3_normalize(n);
    vec3 t = {0.0f, 0.0f, 0.0f}, b = {0.0f, 0.0f, 0.0f};
    face_frame(n, t, b);
    // In the face's own (t, b) plane, which turns counter-clockwise about n.
    vec2 flat[KIT_MAX_OUTLINE] = {{0.0f}};
    unsigned int idx[KIT_MAX_OUTLINE];
    for (int i = 0; i < count; i++) {
        const int k = flip ? count - 1 - i : i;
        const float* p = corners[k];
        flat[i][0] = glm_vec3_dot((float*)p, t);
        flat[i][1] = glm_vec3_dot((float*)p, b);
        idx[i] = uv ? kit_vertex(kit, mat, p, n, tangent, uv[k][0], uv[k][1], 0.0f)
                    : face_vertex(kit, mat, p, n, t, b, 0.0f);
    }
    unsigned int tris[KIT_MAX_OUTLINE][3] = {{0}};
    const int made = ear_clip(flat, count, tris);
    for (int i = 0; i < made; i++)
        mb_tri(&kit->builders[mat], idx[tris[i][0]], idx[tris[i][1]], idx[tris[i][2]]);
}

void kit_polygon_facing(Kit* kit, int mat, const vec3* corners, int count, const vec3 outward) {
    polygon(kit, mat, corners, count, outward, NULL, NULL);
}

void kit_extrude(Kit* kit, int mat_base, int mat_cap, int mat_side, const vec3* base, int count,
                 const vec3 offset) {
    if (!outline_ok(count))
        return;
    vec3 top[KIT_MAX_OUTLINE] = {{0.0f}}, n = {0.0f, 0.0f, 0.0f}, back = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < count; i++)
        glm_vec3_add((float*)base[i], (float*)offset, top[i]);
    glm_vec3_negate_to((float*)offset, back);
    kit_polygon_facing(kit, mat_cap, top, count, offset);
    kit_polygon_facing(kit, mat_base, base, count, back);
    // Which way round the outline runs about the offset decides which side of an edge is out.
    newell(base, count, n);
    const float turn = glm_vec3_dot(n, (float*)offset) >= 0.0f ? 1.0f : -1.0f;
    for (int i = 0; i < count; i++) {
        const int j = (i + 1) % count;
        vec3 edge = {0.0f, 0.0f, 0.0f}, out = {0.0f, 0.0f, 0.0f};
        glm_vec3_sub((float*)base[j], (float*)base[i], edge);
        glm_vec3_cross(edge, (float*)offset, out);
        glm_vec3_scale(out, turn, out);
        kit_quad_facing(kit, mat_side, base[i], base[j], top[j], top[i], out);
    }
}

void kit_slab(Kit* kit, int mat, const vec2* xz, int count, float y0, float y1, bool collide) {
    if (!outline_ok(count))
        return;
    vec3 base[KIT_MAX_OUTLINE];
    vec2 centre = {0.0f, 0.0f};
    for (int i = 0; i < count; i++) {
        glm_vec3_copy((vec3){xz[i][0], y0, xz[i][1]}, base[i]);
        centre[0] += xz[i][0] / (float)count;
        centre[1] += xz[i][1] / (float)count;
    }
    if (mat != KIT_COLLIDER_ONLY)
        kit_extrude(kit, mat, mat, mat, base, count, (vec3){0.0f, y1 - y0, 0.0f});
    if (!collide && mat != KIT_COLLIDER_ONLY)
        return;
    // A convex outline is the union of, for each edge, the strip from that edge in to the
    // line through the centroid parallel to it, as wide as the edge.
    for (int i = 0; i < count; i++) {
        const float* p = xz[i];
        const float* q = xz[(i + 1) % count];
        vec2 along = {q[0] - p[0], q[1] - p[1]};
        const float len = glm_vec2_norm(along);
        if (len < 1e-4f)
            continue;
        glm_vec2_scale(along, 1.0f / len, along);
        const vec2 to_centre = {centre[0] - p[0], centre[1] - p[1]};
        const float depth = fabsf(along[0] * to_centre[1] - along[1] * to_centre[0]);
        // Inward is the side of the edge the centroid lies on.
        const float side = along[0] * to_centre[1] - along[1] * to_centre[0] >= 0.0f ? 1.0f : -1.0f;
        const vec2 in = {-along[1] * side, along[0] * side};
        const vec3 mid = {0.5f * (p[0] + q[0]) + 0.5f * depth * in[0], 0.5f * (y0 + y1),
                          0.5f * (p[1] + q[1]) + 0.5f * depth * in[1]};
        // The yaw that turns local +x along the edge, the sense rotate_y turns.
        const float yaw = atan2f(-along[1], along[0]);
        kit_collider(kit, mid, (vec3){0.5f * len, 0.5f * (y1 - y0), 0.5f * depth}, yaw);
    }
}

// Turned about +Y by `yaw`, the same sense entity_set_rotation_euler turns a body.
static void rotate_y(const vec3 in, float yaw, vec3 out) {
    const float c = cosf(yaw), s = sinf(yaw);
    const float x = in[0] * c + in[2] * s;
    const float z = -in[0] * s + in[2] * c;
    out[0] = x;
    out[1] = in[1];
    out[2] = z;
}

void kit_mesh_collider(Kit* kit, const float* vertices, int vertex_count,
                       const unsigned int* indices, int index_count) {
    if (!kit->em || !kit->physics || vertex_count <= 0 || index_count <= 0)
        return;
    char name[48];
    snprintf(name, sizeof(name), "kit_mesh_%d", kit->collider_count++);
    Entity* e = create_entity(kit->em, name);
    if (!e)
        return;
    glm_vec3_copy(kit->origin, e->position);
    PhysicsShapeDesc shape = {.type = SHAPE_MESH, .density = 0.0f};
    shape.mesh.vertices = vertices;
    shape.mesh.vertex_count = (size_t)vertex_count;
    shape.mesh.indices = indices;
    shape.mesh.index_count = (size_t)index_count;
    entity_add_rigid_body(e, kit->physics, &shape, MOTION_STATIC, OBJ_LAYER_STATIC);
}

void kit_collider(Kit* kit, const vec3 centre, const vec3 half, float yaw) {
    if (!kit->em || !kit->physics)
        return;
    char name[48];
    snprintf(name, sizeof(name), "kit_col_%d", kit->collider_count++);
    Entity* e = create_entity(kit->em, name);
    if (!e)
        return;
    glm_vec3_add((float*)centre, kit->origin, e->position);
    // Before the body, which reads the entity's rotation when it is created.
    if (yaw != 0.0f)
        entity_set_rotation_euler(e, (vec3){0.0f, yaw, 0.0f});
    PhysicsShapeDesc shape = {
        .type = SHAPE_BOX, .box.half_extents = {half[0], half[1], half[2]}, .density = 0.0f};
    entity_add_rigid_body(e, kit->physics, &shape, MOTION_STATIC, OBJ_LAYER_STATIC);
}

// A box of the KitFace bits `shown` (its local +x, -x, +y, -y, +z, -z), its material's grime
// baked into it unless `clean`.
static void box(Kit* kit, int mat, const vec3 centre, const vec3 half, float yaw, bool collide,
                bool clean, unsigned shown) {
    if (mat == KIT_COLLIDER_ONLY) {
        kit_collider(kit, centre, half, yaw);
        return;
    }
    vec3 corner[8];
    for (int i = 0; i < 8; i++) {
        vec3 local = {(i & 1) ? half[0] : -half[0], (i & 2) ? half[1] : -half[1],
                      (i & 4) ? half[2] : -half[2]};
        rotate_y(local, yaw, corner[i]);
        glm_vec3_add(corner[i], (float*)centre, corner[i]);
    }
    // Each face as the four corners sharing one bit, walked in a cycle, and
    // wound from the outward direction.
    static const int faces[6][4] = {
        {1, 3, 7, 5}, {0, 4, 6, 2}, // +x, -x
        {2, 6, 7, 3}, {0, 1, 5, 4}, // +y, -y
        {4, 5, 7, 6}, {0, 2, 3, 1}, // +z, -z
    };
    static const float dirs[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                     {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    const bool grimed = !clean && slot_ok(kit, mat) && kit->grime[mat] > 0.0f;
    for (int f = 0; f < 6; f++) {
        if (!(shown & (1u << f)))
            continue;
        vec3 out = {0.0f, 0.0f, 0.0f};
        rotate_y(dirs[f], yaw, out);
        const float* c0 = corner[faces[f][0]];
        if (grimed) {
            vec3 ea, eb;
            glm_vec3_sub(corner[faces[f][1]], (float*)c0, ea);
            glm_vec3_sub(corner[faces[f][3]], (float*)c0, eb);
            emit_grimed_face(kit, mat, c0, ea, eb, out);
        } else {
            kit_quad_facing(kit, mat, c0, corner[faces[f][1]], corner[faces[f][2]],
                            corner[faces[f][3]], out);
        }
    }
    if (collide)
        kit_collider(kit, centre, half, yaw);
}

void kit_box(Kit* kit, int mat, const vec3 centre, const vec3 half, float yaw, bool collide) {
    box(kit, mat, centre, half, yaw, collide, false, KIT_FACES_ALL);
}

void kit_leaning_box(Kit* kit, int mat, const vec3 base, const vec3 half, float yaw, float lean) {
    vec3 corner[8];
    for (int i = 0; i < 8; i++) {
        vec3 p = {(i & 1) ? half[0] : -half[0], (i & 2) ? 2.0f * half[1] : 0.0f,
                  (i & 4) ? half[2] : -half[2]};
        // Lean about X, then turn about Y, then stand on the base.
        const float y = p[1] * cosf(lean) - p[2] * sinf(lean);
        const float z = p[1] * sinf(lean) + p[2] * cosf(lean);
        corner[i][0] = base[0] + p[0] * cosf(yaw) + z * sinf(yaw);
        corner[i][1] = base[1] + y;
        corner[i][2] = base[2] - p[0] * sinf(yaw) + z * cosf(yaw);
    }
    vec3 centre = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 8; i++)
        glm_vec3_add(centre, corner[i], centre);
    glm_vec3_scale(centre, 1.0f / 8.0f, centre);
    static const int FACES[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                    {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    for (int f = 0; f < 6; f++) {
        vec3 mid = {0.0f, 0.0f, 0.0f}, out = {0.0f, 0.0f, 0.0f};
        for (int k = 0; k < 4; k++)
            glm_vec3_add(mid, corner[FACES[f][k]], mid);
        glm_vec3_scale(mid, 0.25f, mid);
        glm_vec3_sub(mid, centre, out);
        kit_quad_facing(kit, mat, corner[FACES[f][0]], corner[FACES[f][1]], corner[FACES[f][2]],
                        corner[FACES[f][3]], out);
    }
}

/*
 * A capped prism from end0 to end1, which differ only along axis `ax`. Its
 * section is round in the other two axes, `u` taking the cosine and `w` the
 * sine, which fixes where the first facet falls.
 */
static void prism_between(Kit* kit, int mat, const vec3 end0, const vec3 end1, int ax, int u, int w,
                          float r, int sides) {
    if (sides < 3)
        sides = 3;
    for (int i = 0; i < sides; i++) {
        const float a0 = (float)i / (float)sides * 2.0f * GLM_PIf;
        const float a1 = (float)(i + 1) / (float)sides * 2.0f * GLM_PIf;
        const float amid = 0.5f * (a0 + a1);
        vec3 p0, p1, p2, p3, out = {0.0f, 0.0f, 0.0f};
        glm_vec3_copy((float*)end0, p0);
        glm_vec3_copy((float*)end0, p1);
        p0[u] += r * cosf(a0);
        p0[w] += r * sinf(a0);
        p1[u] += r * cosf(a1);
        p1[w] += r * sinf(a1);
        glm_vec3_copy(p1, p2);
        glm_vec3_copy(p0, p3);
        p2[ax] = end1[ax];
        p3[ax] = end1[ax];
        out[u] = cosf(amid);
        out[w] = sinf(amid);
        kit_quad_facing(kit, mat, p0, p1, p2, p3, out);
        vec3 cap_out = {0.0f, 0.0f, 0.0f};
        cap_out[ax] = 1.0f;
        kit_tri_facing(kit, mat, end1, p3, p2, cap_out);
        cap_out[ax] = -1.0f;
        kit_tri_facing(kit, mat, end0, p0, p1, cap_out);
    }
}

void kit_prism(Kit* kit, int mat, float x, float z, float y0, float y1, float r, int sides,
               bool collide) {
    prism_between(kit, mat, (vec3){x, y0, z}, (vec3){x, y1, z}, 1, 0, 2, r, sides);
    if (collide)
        kit_collider(kit, (vec3){x, 0.5f * (y0 + y1), z}, (vec3){r, 0.5f * (y1 - y0), r}, 0.0f);
}

void kit_prism_lying(Kit* kit, int mat, const vec3 centre, float half_len, float r, int sides,
                     bool along_x) {
    const int ax = along_x ? 0 : 2;
    vec3 end0, end1;
    glm_vec3_copy((float*)centre, end0);
    glm_vec3_copy((float*)centre, end1);
    end0[ax] -= half_len;
    end1[ax] += half_len;
    prism_between(kit, mat, end0, end1, ax, 1, along_x ? 2 : 0, r, sides);
}

/*
 * Smooth surfaces, for the few things the eye knows are round and polished --
 * a pot, a tap. Everything else in the kit is faceted on purpose; these carry
 * a normal per vertex, so a surface of a few dozen sides shades as a curve.
 * A surface is built as rings of sides + 1 vertices, the last repeating the
 * first so the texture's U can run on past the seam instead of wrapping back.
 */
#define KIT_MAX_SIDES 64

typedef struct Ring {
    unsigned int idx[KIT_MAX_SIDES + 1];
    vec3 p[KIT_MAX_SIDES + 1];
    vec3 n[KIT_MAX_SIDES + 1];
} Ring;

/*
 * Grime on a smooth solid gathers at its JOINTS -- where a lathe stands, both
 * ends of a pipe -- which is where it meets whatever it is fixed to: the tap
 * where it enters the counter, a pot where it sits on the burner, a handle
 * where it is riveted on. The dirt needs rings to fade across, so a grimed
 * solid is cut every GRIME_RING while it is near a joint, and every GRIME_STEP
 * past that, as a box face's interior is; a clean one keeps only the rings its
 * shape asks for.
 */
#define GRIME_RING      0.01f               // metres
#define GRIME_RING_NEAR (2.0f * GRIME_BAND) // past this from a joint the falloff is spent

// The rings along a stretch `len` long whose ends are j0 and j1 from the solid's nearest joint,
// as fractions of it from 0 to 1 into `out`, which holds GRIME_MAX_CUTS + 1. Returns the count.
static int ring_cuts(const Kit* kit, int mat, float len, float j0, float j1, float* out) {
    int n = 0;
    out[n++] = 0.0f;
    if (kit->grime[mat] > 0.0f && len > 0.0f) {
        float s = 0.0f;
        while (n < GRIME_MAX_CUTS) {
            s += j0 + (j1 - j0) * s / len < GRIME_RING_NEAR ? GRIME_RING : GRIME_STEP;
            if (s >= len - 0.25f * GRIME_RING)
                break;
            out[n++] = s / len;
        }
    }
    out[n++] = 1.0f;
    return n;
}

// Where one ring sits and how it shades.
typedef struct RingAt {
    vec3 centre, axis, u, w; // u takes the cosine and w the sine
    float r;
    float nr, na; // the normal: the radial direction weighted nr, plus the axis weighted na
    float v;      // texture V, in repeats
    float joint;  // metres to the solid's nearest joint
    bool planar;  // UVs projected onto the plane of u and w: a face turned along the axis
} RingAt;

/*
 * A ring of sides + 1 vertices. U runs round it `u_scale` per radian, and its
 * grime falls off from the joint over `band`. A planar ring takes its UVs from
 * the plane across its axis instead -- a plate's top, a bob's face -- since
 * running U round a face that closes on its axis squeezes the whole texture
 * into a pinwheel at the middle.
 */
static void make_ring(Kit* kit, int mat, Ring* ring, const RingAt* at, float u_scale, float band,
                      int sides) {
    const float strength = kit->grime[mat], inv = 1.0f / kit->repeat_m[mat];
    for (int j = 0; j <= sides; j++) {
        const float a = (float)j / (float)sides * 2.0f * GLM_PIf;
        vec3 radial = {0.0f, 0.0f, 0.0f}, t = {0.0f, 0.0f, 0.0f};
        glm_vec3_scale((float*)at->u, cosf(a), radial);
        glm_vec3_muladds((float*)at->w, sinf(a), radial);
        glm_vec3_scale((float*)at->u, -sinf(a), t);
        glm_vec3_muladds((float*)at->w, cosf(a), t);
        glm_vec3_copy((float*)at->centre, ring->p[j]);
        glm_vec3_muladds(radial, at->r, ring->p[j]);
        glm_vec3_scale(radial, at->nr, ring->n[j]);
        glm_vec3_muladds((float*)at->axis, at->na, ring->n[j]);
        glm_vec3_normalize(ring->n[j]);
        const float grime =
            strength > 0.0f ? grime_amount(strength, ring->p[j],
                                           grime_falloff(at->joint, band * grime_reach(ring->p[j])))
                            : 0.0f;
        const float u = at->planar ? glm_vec3_dot(ring->p[j], (float*)at->u) * inv : a * u_scale;
        const float v = at->planar ? glm_vec3_dot(ring->p[j], (float*)at->w) * inv : at->v;
        ring->idx[j] = kit_vertex(kit, mat, ring->p[j], ring->n[j], t, u, v, grime);
    }
}

// One triangle across two rings: skipped where it has no area (a ring closed
// to a point on its axis), and wound to agree with its corners' normals, so a
// profile may run in either direction.
static void ring_tri(Kit* kit, int mat, const Ring* a, int ja, const Ring* b, int jb, const Ring* c,
                     int jc) {
    vec3 cross = {0.0f, 0.0f, 0.0f}, n = {0.0f, 0.0f, 0.0f};
    corner_cross(a->p[ja], b->p[jb], c->p[jc], cross);
    if (glm_vec3_norm2(cross) < 1e-20f)
        return;
    glm_vec3_add((float*)a->n[ja], (float*)b->n[jb], n);
    glm_vec3_add(n, (float*)c->n[jc], n);
    if (glm_vec3_dot(cross, n) >= 0.0f)
        mb_tri(&kit->builders[mat], a->idx[ja], b->idx[jb], c->idx[jc]);
    else
        mb_tri(&kit->builders[mat], a->idx[ja], c->idx[jc], b->idx[jb]);
}

static void ring_band(Kit* kit, int mat, const Ring* a, const Ring* b, int sides) {
    for (int j = 0; j < sides; j++) {
        ring_tri(kit, mat, a, j, a, j + 1, b, j + 1);
        ring_tri(kit, mat, a, j, b, j + 1, b, j);
    }
}

// A flat cap over a ring, facing `out`.
static void ring_cap(Kit* kit, int mat, const Ring* ring, const vec3 centre, const vec3 out,
                     int sides) {
    for (int j = 0; j < sides; j++)
        kit_tri_facing(kit, mat, centre, ring->p[j], ring->p[j + 1], out);
}

static bool points_ok(int count) {
    if (count >= 2 && count <= KIT_MAX_POINTS)
        return true;
    fprintf(stderr, "silent: a path or profile needs 2 to %d points, not %d\n", KIT_MAX_POINTS,
            count);
    return false;
}

// A section's first direction off `axis` (unit): up, unless the axis is nearly
// upright itself.
static void section_start(const vec3 axis, vec3 u) {
    const bool upright = fabsf(axis[1]) >= 0.9f;
    const vec3 ref = {upright ? 1.0f : 0.0f, upright ? 0.0f : 1.0f, 0.0f};
    glm_vec3_cross((float*)axis, (float*)ref, u);
    glm_vec3_normalize(u);
}

/*
 * A round pipe along a polyline, capped both ends. Each ring faces along the
 * average of the segments either side of its point, and its orientation is
 * carried from ring to ring by parallel transport, so a bent pipe neither
 * pinches at a joint nor twists along its length.
 */
static void pipe(Kit* kit, int mat, const vec3* path, int count, float r, int sides) {
    const float inv = 1.0f / kit->repeat_m[mat];
    float total = 0.0f;
    for (int i = 1; i < count; i++)
        total += glm_vec3_distance((float*)path[i], (float*)path[i - 1]);
    const float band = fminf(GRIME_BAND, 0.3f * total);
    Ring rings[2];
    RingAt at = {.r = r, .nr = 1.0f};
    vec3 prev = {0.0f, 0.0f, 0.0f};
    float len = 0.0f, before = 0.0f; // along the rings so far, and to the start of the segment
    int made = 0;
    for (int i = 0; i < count; i++) {
        // The segment arriving at point i, cut into pieces; point 0 is one ring.
        float cut[GRIME_MAX_CUTS + 1] = {0.0f};
        int pieces = 1;
        if (i > 0) {
            const float seg = glm_vec3_distance((float*)path[i], (float*)path[i - 1]);
            pieces = ring_cuts(kit, mat, seg, fminf(before, total - before),
                               fminf(before + seg, total - before - seg), cut) -
                     1;
            before += seg;
        }
        for (int s = i == 0 ? pieces : 1; s <= pieces; s++) {
            if (s < pieces) {
                // Inside a segment, a ring faces along it.
                glm_vec3_lerp((float*)path[i - 1], (float*)path[i], cut[s], at.centre);
                glm_vec3_sub((float*)path[i], (float*)path[i - 1], at.axis);
            } else {
                // At a point, along the segments either side of it.
                const int behind = i > 0 ? i - 1 : 0, ahead = i < count - 1 ? i + 1 : count - 1;
                glm_vec3_copy((float*)path[i], at.centre);
                glm_vec3_sub((float*)path[ahead], (float*)path[behind], at.axis);
            }
            if (glm_vec3_norm2(at.axis) < 1e-12f)
                return;
            glm_vec3_normalize(at.axis);
            if (made == 0) {
                section_start(at.axis, at.u);
            } else {
                glm_vec3_muladds(at.axis, -glm_vec3_dot(at.u, at.axis), at.u);
                len += glm_vec3_distance(at.centre, prev);
            }
            glm_vec3_normalize(at.u);
            glm_vec3_cross(at.axis, at.u, at.w);
            glm_vec3_copy(at.centre, prev);
            at.v = len * inv;
            at.joint = fminf(len, total - len);
            Ring* ring = &rings[made & 1];
            make_ring(kit, mat, ring, &at, r * inv, band, sides);
            if (made == 0) {
                vec3 back = {0.0f, 0.0f, 0.0f};
                glm_vec3_negate_to(at.axis, back);
                ring_cap(kit, mat, ring, path[0], back, sides);
            } else {
                ring_band(kit, mat, &rings[(made - 1) & 1], ring, sides);
            }
            made++;
        }
    }
    ring_cap(kit, mat, &rings[(made - 1) & 1], path[count - 1], at.axis, sides);
}

/*
 * The profile's normal where two segments meet is their average when they
 * turn by less than this -- a rolled rim, a domed lid -- and each keeps its
 * own where they turn by more, which is a crease: a pot's base meets its wall.
 */
#define LATHE_SMOOTH_COS 0.5f // 60 degrees

// Segment k's outward normal in (radius, height): the profile runs up the
// outside, so a rising wall faces out and a bottom running outward faces down.
static void profile_normal(const vec2* profile, int k, vec2 out) {
    out[0] = profile[k + 1][1] - profile[k][1];
    out[1] = profile[k][0] - profile[k + 1][0];
    glm_vec2_normalize(out);
}

// The normal the profile carries at point k of segment `seg`, smoothed with the
// neighbouring segment `other` unless the turn between them is a crease.
static void profile_joint(const vec2* profile, int count, int seg, int other, vec2 out) {
    profile_normal(profile, seg, out);
    if (other < 0 || other > count - 2)
        return;
    vec2 n = {0.0f, 0.0f};
    profile_normal(profile, other, n);
    if (glm_vec2_dot(out, n) > LATHE_SMOOTH_COS) {
        glm_vec2_add(out, n, out);
        glm_vec2_normalize(out);
    }
}

/*
 * A surface of revolution about `axis` through `base`, from a profile of
 * {radius, distance along the axis} points, the section starting along u and
 * turning toward w. Every segment is its own run of bands, so a crease costs
 * nothing extra; a radius of 0 closes the surface on its axis. It stands on
 * its lowest point, so that is its joint.
 */
static void lathe(Kit* kit, int mat, const vec3 base, const vec3 axis, const vec3 u, const vec3 w,
                  const vec2* profile, int count, int sides) {
    const float inv = 1.0f / kit->repeat_m[mat];
    float rmax = 0.0f, lo = profile[0][1], hi = profile[0][1], len = 0.0f;
    for (int k = 0; k < count; k++) {
        rmax = glm_max(rmax, profile[k][0]);
        lo = glm_min(lo, profile[k][1]);
        hi = glm_max(hi, profile[k][1]);
    }
    const float band = fminf(GRIME_BAND, 0.3f * (hi - lo));
    RingAt at = {0};
    glm_vec3_copy((float*)axis, at.axis);
    glm_vec3_copy((float*)u, at.u);
    glm_vec3_copy((float*)w, at.w);
    for (int k = 0; k < count - 1; k++) {
        vec2 n0 = {0.0f, 0.0f}, n1 = {0.0f, 0.0f}, ns = {0.0f, 0.0f};
        profile_joint(profile, count, k, k - 1, n0);
        profile_joint(profile, count, k, k + 1, n1);
        profile_normal(profile, k, ns);
        at.planar = fabsf(ns[1]) > 0.7f;
        const float seg = glm_vec2_distance((float*)profile[k + 1], (float*)profile[k]);
        float cut[GRIME_MAX_CUTS + 1] = {0.0f};
        const int pieces =
            ring_cuts(kit, mat, seg, profile[k][1] - lo, profile[k + 1][1] - lo, cut) - 1;
        Ring rings[2];
        for (int s = 0; s <= pieces; s++) {
            const float f = cut[s];
            vec2 p = {0.0f, 0.0f}, n = {0.0f, 0.0f};
            glm_vec2_lerp((float*)profile[k], (float*)profile[k + 1], f, p);
            glm_vec2_lerp(n0, n1, f, n);
            glm_vec3_copy((float*)base, at.centre);
            glm_vec3_muladds((float*)axis, p[1], at.centre);
            at.r = p[0];
            at.nr = n[0];
            at.na = n[1];
            at.v = (len + f * seg) * inv;
            at.joint = p[1] - lo;
            make_ring(kit, mat, &rings[s & 1], &at, rmax * inv, band, sides);
            if (s > 0)
                ring_band(kit, mat, &rings[(s - 1) & 1], &rings[s & 1], sides);
        }
        len += seg;
    }
}

/*
 * The arch's two arcs, sampled KIT_ARCH_SEGMENTS each from a springing point to the apex.
 * POINTED: each half is an arc whose centre sits on the springing line at the far side's
 * distance r, r = (w^2 + rise^2) / 2w for half-span w -- w itself is a round arch. TUDOR: a
 * haunch of radius TUDOR_HAUNCH * w turning TUDOR_TURN, then the tangent arc that reaches
 * the apex, whose radius the apex fixes.
 */
#define TUDOR_HAUNCH 0.35f
#define TUDOR_TURN   (GLM_PIf / 3.0f)
#define TUDOR_ARCS   3 // of a half's segments, on the haunch

/*
 * A four-centred half of half-span w and `rise`: from the haunch's end, (dx, dy) to the apex,
 * and the denominator of the long arc's radius, which is at or below zero for an arch too high
 * to be four-centred.
 */
static float tudor_span(float w, float rise, float* dx, float* dy) {
    const float r1 = TUDOR_HAUNCH * w;
    *dx = w - r1 + r1 * cosf(TUDOR_TURN);
    *dy = rise - r1 * sinf(TUDOR_TURN);
    return *dx * cosf(TUDOR_TURN) - *dy * sinf(TUDOR_TURN);
}

// The head an arch of half-span w is drawn with: a Tudor too high to be four-centred is pointed.
static KitArchShape arch_drawn(KitArchShape shape, float w, float rise) {
    float dx, dy;
    if (shape == KIT_ARCH_TUDOR && tudor_span(w, rise, &dx, &dy) <= 1e-4f)
        return KIT_ARCH_POINTED;
    return shape;
}

// One half, from (a0, spring) up to the apex over the mid-span: `out` gets
// KIT_ARCH_SEGMENTS + 1 points. `dir` is +1 for the left half, -1 for the right, mirrored.
static void arch_half(KitArchShape shape, float a0, float w, float spring, float rise, float dir,
                      vec2* out) {
    const int n = KIT_ARCH_SEGMENTS;
    if (arch_drawn(shape, w, rise) == KIT_ARCH_TUDOR) {
        const float r1 = TUDOR_HAUNCH * w;
        const vec2 c1 = {a0 + dir * r1, spring};
        const vec2 p1 = {c1[0] - dir * r1 * cosf(TUDOR_TURN), spring + r1 * sinf(TUDOR_TURN)};
        float dx, dy;
        const float den = tudor_span(w, rise, &dx, &dy);
        // The long arc's centre lies on the line through the haunch's end and its centre.
        const float r2 = (dx * dx + dy * dy) / (2.0f * den);
        const vec2 c2 = {p1[0] + dir * r2 * cosf(TUDOR_TURN), p1[1] - r2 * sinf(TUDOR_TURN)};
        const float end = atan2f(spring + rise - c2[1], dir * (a0 + dir * w - c2[0]));
        for (int i = 0; i <= n; i++) {
            if (i <= TUDOR_ARCS) {
                const float t = GLM_PIf - TUDOR_TURN * (float)i / (float)TUDOR_ARCS;
                out[i][0] = c1[0] + dir * r1 * cosf(t);
                out[i][1] = c1[1] + r1 * sinf(t);
            } else {
                const float f = (float)(i - TUDOR_ARCS) / (float)(n - TUDOR_ARCS);
                const float t = (GLM_PIf - TUDOR_TURN) + f * (end - (GLM_PIf - TUDOR_TURN));
                out[i][0] = c2[0] + dir * r2 * cosf(t);
                out[i][1] = c2[1] + r2 * sinf(t);
            }
        }
        return;
    }
    const float r = (w * w + rise * rise) / (2.0f * w);
    const vec2 c = {a0 + dir * r, spring};
    const float end = atan2f(rise, dir * (a0 + dir * w - c[0]));
    for (int i = 0; i <= n; i++) {
        const float t = GLM_PIf + (end - GLM_PIf) * (float)i / (float)n;
        out[i][0] = c[0] + dir * r * cosf(t);
        out[i][1] = c[1] + r * sinf(t);
    }
}

// The head over [a0, a1] in (a, y): KIT_ARCH_POINTS points from (a0, spring) over its apex,
// `rise` above the springing line, to (a1, spring). FLAT runs straight across at `spring`.
static void arch_outline(KitArchShape shape, float a0, float a1, float spring, float rise,
                         vec2 out[KIT_ARCH_POINTS]) {
    const int n = KIT_ARCH_SEGMENTS;
    const float w = 0.5f * (a1 - a0);
    if (shape == KIT_ARCH_FLAT || rise <= 0.0f || w <= 0.0f) {
        for (int i = 0; i < KIT_ARCH_POINTS; i++) {
            out[i][0] = a0 + (a1 - a0) * (float)i / (float)(KIT_ARCH_POINTS - 1);
            out[i][1] = spring;
        }
        return;
    }
    vec2 right[KIT_ARCH_SEGMENTS + 1];
    arch_half(shape, a0, w, spring, rise, 1.0f, out);
    arch_half(shape, a1, w, spring, rise, -1.0f, right);
    // The right half runs up from a1; it is wanted coming down, and the apex only once.
    for (int i = 1; i <= n; i++)
        glm_vec2_copy(right[n - i], out[n + i]);
    out[n][0] = a0 + w;
    out[n][1] = spring + rise;
}

int kit_clear_spans(float lo, float hi, const vec2* blocked, int n, vec2* out) {
    // Walked in order of where each starts, so one sort settles overlaps and gaps alike.
    int order[KIT_MAX_OPENINGS + 2];
    if (n > KIT_MAX_OPENINGS + 2)
        n = KIT_MAX_OPENINGS + 2;
    for (int i = 0; i < n; i++) {
        order[i] = i;
        for (int j = i; j > 0 && blocked[order[j]][0] < blocked[order[j - 1]][0]; j--) {
            const int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    }
    int count = 0;
    float cursor = lo;
    for (int i = 0; i < n; i++) {
        const float* b = blocked[order[i]];
        if (fminf(b[0], hi) > cursor)
            glm_vec2_copy((vec2){cursor, fminf(b[0], hi)}, out[count++]);
        cursor = fmaxf(cursor, b[1]);
    }
    if (hi > cursor)
        glm_vec2_copy((vec2){cursor, hi}, out[count++]);
    return count;
}

int kit_arc_band(const vec2 c, float rx_out, float ry_out, float rx_in, float ry_in, float t0,
                 float t1, int seg, vec2* out) {
    int n = 0;
    for (int i = 0; i <= seg; i++) {
        const float t = t0 + (t1 - t0) * (float)i / (float)seg;
        glm_vec2_copy((vec2){c[0] + rx_out * cosf(t), c[1] + ry_out * sinf(t)}, out[n++]);
    }
    for (int i = seg; i >= 0; i--) {
        const float t = t0 + (t1 - t0) * (float)i / (float)seg;
        glm_vec2_copy((vec2){c[0] + rx_in * cosf(t), c[1] + ry_in * sinf(t)}, out[n++]);
    }
    return n;
}

int kit_opening_outline(const KitOpening* o, vec2 out[KIT_OPENING_POINTS]) {
    glm_vec2_copy((vec2){o->from, o->bottom}, out[0]);
    glm_vec2_copy((vec2){o->to, o->bottom}, out[1]);
    if (!kit_opening_arched(o)) {
        glm_vec2_copy((vec2){o->to, o->top}, out[2]);
        glm_vec2_copy((vec2){o->from, o->top}, out[3]);
        return 4;
    }
    vec2 head[KIT_ARCH_POINTS];
    arch_outline(o->arch, o->from, o->to, o->top, o->rise, head);
    for (int i = 0; i < KIT_ARCH_POINTS; i++)
        glm_vec2_copy(head[KIT_ARCH_POINTS - 1 - i], out[2 + i]);
    return KIT_OPENING_POINTS;
}

KitOpening kit_opening_grow(const KitOpening* o, float w) {
    KitOpening g = *o;
    g.from -= w;
    g.to += w;
    const float half = 0.5f * (o->to - o->from);
    if (!kit_opening_arched(o)) {
        g.top += w;
    } else if (arch_drawn(o->arch, half, o->rise) == KIT_ARCH_POINTED) {
        // Each arc keeps its centre and gains w of radius.
        const float r = (half * half + o->rise * o->rise) / (2.0f * half);
        g.rise = sqrtf((r + w) * (r + w) - (r - half) * (r - half));
    } else {
        // A four-centred head's haunch is a fixed share of its span, so it cannot grow
        // concentrically; this keeps the band near enough its width over the crown.
        g.rise += 0.6f * w;
    }
    return g;
}

// One slab of a wall layer, between `a` and `b` along the wall and `y0`..`y1`, centred
// `offset` out of the wall's middle; never grimed.
static void wall_slab(Kit* kit, const KitWallFrame* wf, int mat, float offset, float thick, float a,
                      float b, float y0, float y1) {
    if (b - a < 1e-4f || y1 - y0 < 1e-4f)
        return;
    vec3 centre = {0.0f, 0.0f, 0.0f};
    kit_frame_point(&wf->f, 0.5f * (a + b), 0.5f * (y0 + y1), wf->at + offset, centre);
    const vec3 half = {0.5f * (b - a), 0.5f * (y1 - y0), 0.5f * thick};
    box(kit, mat, centre, half, wf->f.yaw, false, true, KIT_FACES_ALL);
}

/*
 * The wall left over an arched opening's springing line, either side of its head: from the
 * springing point round the arch to its apex, then out along the apex's height, back down to
 * the springing line. Each is a flat polygon through the layer, whose curved edge is the
 * underside of the arch.
 */
static void wall_spandrels(Kit* kit, const KitWallFrame* wf, const KitOpening* o, int mat,
                           float offset, float thick) {
    vec2 head[KIT_ARCH_POINTS], half[KIT_ARCH_SEGMENTS + 2];
    arch_outline(o->arch, o->from, o->to, o->top, o->rise, head);
    const float d = wf->at + offset, crown = kit_opening_crown(o);
    const int n = KIT_ARCH_SEGMENTS;
    for (int i = 0; i <= n; i++)
        glm_vec2_copy(head[i], half[i]);
    glm_vec2_copy((vec2){o->from, crown}, half[n + 1]);
    kit_frame_extrude(kit, &wf->f, mat, half, n + 2, d - 0.5f * thick, d + 0.5f * thick);
    for (int i = 0; i <= n; i++)
        glm_vec2_copy(head[n + i], half[i]);
    glm_vec2_copy((vec2){o->to, crown}, half[n + 1]);
    kit_frame_extrude(kit, &wf->f, mat, half, n + 2, d - 0.5f * thick, d + 0.5f * thick);
}

/*
 * One layer of the wall, round its openings. A storeyed wall stacks openings -- a window over
 * a door -- so the wall is cut into COLUMNS at every opening's edges, and each column is solid
 * from the floor up, between the openings that span it, to the top. Openings side by side make
 * one column each and a solid one between, which is the wall a row of them always made.
 * Openings must not overlap. The body through the whole wall treats an arch's head as solid
 * from its springing line: nothing walks through the top of an arch. An opening off the wall's
 * span -- a piece of a longer wall given the whole wall's openings -- leaves it uncut.
 */
static void wall_layer(Kit* kit, const KitWall* w, const KitWallFrame* wf, int mat, float offset,
                       float thick) {
    const int n = w->opening_count < KIT_MAX_OPENINGS ? w->opening_count : KIT_MAX_OPENINGS;
    float cuts[2 * KIT_MAX_OPENINGS + 2];
    int nc = 0;
    cuts[nc++] = w->from;
    cuts[nc++] = w->to;
    for (int i = 0; i < n; i++) {
        cuts[nc++] = glm_clamp(w->openings[i].from, w->from, w->to);
        cuts[nc++] = glm_clamp(w->openings[i].to, w->from, w->to);
    }
    for (int i = 1; i < nc; i++)
        for (int j = i; j > 0 && cuts[j] < cuts[j - 1]; j--) {
            const float t = cuts[j];
            cuts[j] = cuts[j - 1];
            cuts[j - 1] = t;
        }
    for (int c = 0; c + 1 < nc; c++) {
        const float a0 = cuts[c], a1 = cuts[c + 1];
        if (a1 - a0 < 1e-4f)
            continue;
        // The openings spanning this column, bottom first.
        const KitOpening* stack[KIT_MAX_OPENINGS];
        int ns = 0;
        for (int i = 0; i < n; i++) {
            const KitOpening* o = &w->openings[i];
            if (o->from <= a0 + 1e-4f && o->to >= a1 - 1e-4f)
                stack[ns++] = o;
        }
        for (int i = 1; i < ns; i++)
            for (int j = i; j > 0 && stack[j]->bottom < stack[j - 1]->bottom; j--) {
                const KitOpening* t = stack[j];
                stack[j] = stack[j - 1];
                stack[j - 1] = t;
            }
        float y = w->y0;
        for (int i = 0; i < ns; i++) {
            const KitOpening* o = stack[i];
            // An opening above the layer's top, a window upstairs over a lining downstairs, cuts
            // nothing from it.
            if (o->bottom >= w->y1)
                break;
            wall_slab(kit, wf, mat, offset, thick, a0, a1, y, o->bottom);
            y = mat != KIT_COLLIDER_ONLY ? kit_opening_crown(o) : o->top;
        }
        wall_slab(kit, wf, mat, offset, thick, a0, a1, y, w->y1);
    }
    if (mat == KIT_COLLIDER_ONLY)
        return;
    for (int i = 0; i < n; i++) {
        const KitOpening* o = &w->openings[i];
        if (kit_opening_arched(o) && o->from >= w->from - 1e-4f && o->to <= w->to + 1e-4f)
            wall_spandrels(kit, wf, o, mat, offset, thick);
    }
}

static void wall_in(Kit* kit, const KitWall* w, const KitWallFrame* wf) {
    const float q = 0.25f * w->thick;
    const float s = wf->inner >= 0 ? 1.0f : -1.0f;
    wall_layer(kit, w, wf, w->mat_inner, s * q, 0.5f * w->thick);
    wall_layer(kit, w, wf, w->mat_outer, -s * q, 0.5f * w->thick);
    // One body per span through the whole thickness, rather than one per layer:
    // two coplanar bodies meeting mid-wall buy nothing.
    wall_layer(kit, w, wf, KIT_COLLIDER_ONLY, 0.0f, w->thick);
}

KitWallFrame kit_wall_frame(const KitWall* w) {
    // Along X, the world frame already runs a along x and d along z. Along Z, a quarter turn
    // the other way runs a along +z, and d then points along -x.
    const int inner = w->inner >= 0 ? 1 : -1;
    if (w->along_x)
        return (KitWallFrame){KIT_WORLD, w->at, inner};
    return (KitWallFrame){KIT_WORLD_Z, -w->at, -inner};
}

void kit_wall(Kit* kit, const KitWall* w) {
    const KitWallFrame wf = kit_wall_frame(w);
    wall_in(kit, w, &wf);
}

void kit_frame_wall(Kit* kit, const KitFrame* f, const KitWall* w) {
    const KitWallFrame wf = {*f, w->at, w->inner >= 0 ? 1 : -1};
    wall_in(kit, w, &wf);
}

void kit_frame_panel(Kit* kit, const KitFrame* f, int mat, const KitWall* w) {
    const KitWallFrame wf = {*f, w->at, 1};
    wall_layer(kit, w, &wf, mat, 0.0f, w->thick);
}

void kit_frame_point(const KitFrame* f, float a, float y, float d, vec3 out) {
    kit_frame_dir(f, a, y, d, out);
    glm_vec3_add(out, (float*)f->origin, out);
}

void kit_frame_dir(const KitFrame* f, float a, float y, float d, vec3 out) {
    const vec3 local = {a, y, d};
    rotate_y(local, f->yaw, out);
}

void kit_drip(Kit* kit, const KitFrame* f, const vec3 from, const vec3 to, float rate,
              float ground) {
    if (kit->drip_count >= RAIN_DRIP_MAX) {
        fprintf(stderr, "silent: kit is out of drip lines (%d)\n", RAIN_DRIP_MAX);
        return;
    }
    RainDripLine* l = &kit->drips[kit->drip_count++];
    kit_frame_point(f, from[0], from[1], from[2], l->from);
    kit_frame_point(f, to[0], to[1], to[2], l->to);
    glm_vec3_add(l->from, kit->origin, l->from);
    glm_vec3_add(l->to, kit->origin, l->to);
    l->rate = rate;
    l->ground = ground + kit->origin[1];
}

void kit_drip_run(Kit* kit, const KitFrame* f, const vec3 from, const vec3 to, float per_m,
                  float ground) {
    kit_drip(kit, f, from, to, per_m * glm_vec3_distance((float*)from, (float*)to), ground);
}

void kit_wick(Kit* kit, const KitFrame* f, float a, float y, float d, float size) {
    if (kit->wick_count >= FIRE_MAX) {
        fprintf(stderr, "silent: kit is out of wicks (%d)\n", FIRE_MAX);
        return;
    }
    KitWick* w = &kit->wicks[kit->wick_count++];
    kit_frame_point(f, a, y, d, w->tip);
    glm_vec3_add(w->tip, kit->origin, w->tip);
    w->size = size;
}

void kit_frame_box(Kit* kit, const KitFrame* f, int mat, float a0, float a1, float y0, float y1,
                   float d0, float d1, bool collide) {
    vec3 centre = {0.0f, 0.0f, 0.0f};
    kit_frame_point(f, 0.5f * (a0 + a1), 0.5f * (y0 + y1), 0.5f * (d0 + d1), centre);
    const vec3 half = {0.5f * fabsf(a1 - a0), 0.5f * fabsf(y1 - y0), 0.5f * fabsf(d1 - d0)};
    kit_box(kit, mat, centre, half, f->yaw, collide);
}

void kit_frame_quad(Kit* kit, const KitFrame* f, int mat, const vec3 p[4], const vec3 out) {
    vec3 w[4] = {{0.0f}}, o = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 4; i++)
        kit_frame_point(f, p[i][0], p[i][1], p[i][2], w[i]);
    kit_frame_dir(f, out[0], out[1], out[2], o);
    kit_quad_facing(kit, mat, w[0], w[1], w[2], w[3], o);
}

void kit_frame_box_faces(Kit* kit, const KitFrame* f, int mat, float a0, float a1, float y0,
                         float y1, float d0, float d1, unsigned shown) {
    vec3 centre = {0.0f, 0.0f, 0.0f};
    kit_frame_point(f, 0.5f * (a0 + a1), 0.5f * (y0 + y1), 0.5f * (d0 + d1), centre);
    const vec3 half = {0.5f * fabsf(a1 - a0), 0.5f * fabsf(y1 - y0), 0.5f * fabsf(d1 - d0)};
    box(kit, mat, centre, half, f->yaw, false, false, shown);
}

// An (a, y) outline at distance d, in the world.
static bool outline_at(const KitFrame* f, const vec2* outline, int count, float d, vec3* out) {
    if (!outline_ok(count))
        return false;
    for (int i = 0; i < count; i++)
        kit_frame_point(f, outline[i][0], outline[i][1], d, out[i]);
    return true;
}

void kit_frame_polygon(Kit* kit, const KitFrame* f, int mat, const vec2* outline, int count,
                       float d) {
    vec3 world[KIT_MAX_OUTLINE], out = {0.0f, 0.0f, 0.0f};
    if (!outline_at(f, outline, count, d, world))
        return;
    kit_frame_dir(f, 0.0f, 0.0f, 1.0f, out);
    kit_polygon_facing(kit, mat, world, count, out);
}

void kit_frame_card_polygon(Kit* kit, const KitFrame* f, int mat, const vec2* outline, int count,
                            float d, const float uv[4], bool back) {
    vec3 world[KIT_MAX_OUTLINE], out = {0.0f, 0.0f, 0.0f}, along = {0.0f, 0.0f, 0.0f};
    if (!outline_at(f, outline, count, d, world))
        return;
    float lo[2] = {outline[0][0], outline[0][1]}, hi[2] = {outline[0][0], outline[0][1]};
    for (int i = 1; i < count; i++)
        for (int k = 0; k < 2; k++) {
            lo[k] = fminf(lo[k], outline[i][k]);
            hi[k] = fmaxf(hi[k], outline[i][k]);
        }
    vec2 tex[KIT_MAX_OUTLINE] = {{0.0f, 0.0f}};
    for (int i = 0; i < count; i++) {
        tex[i][0] = uv[0] + (uv[2] - uv[0]) * (outline[i][0] - lo[0]) / fmaxf(hi[0] - lo[0], 1e-6f);
        tex[i][1] = uv[1] + (uv[3] - uv[1]) * (outline[i][1] - lo[1]) / fmaxf(hi[1] - lo[1], 1e-6f);
    }
    kit_frame_dir(f, 0.0f, 0.0f, back ? -1.0f : 1.0f, out);
    kit_frame_dir(f, 1.0f, 0.0f, 0.0f, along);
    polygon(kit, mat, world, count, out, tex, along);
}

void kit_frame_extrude(Kit* kit, const KitFrame* f, int mat, const vec2* outline, int count,
                       float d0, float d1) {
    vec3 base[KIT_MAX_OUTLINE], offset = {0.0f, 0.0f, 0.0f};
    if (!slot_ok(kit, mat) || !outline_at(f, outline, count, d0, base))
        return;
    kit_frame_dir(f, 0.0f, 0.0f, d1 - d0, offset);
    kit_extrude(kit, mat, mat, mat, base, count, offset);
}

void kit_frame_run(Kit* kit, const KitFrame* f, int mat, const vec2* profile, int count, float a0,
                   float a1) {
    if (!outline_ok(count))
        return;
    // A quarter turn: this frame's a runs along the old -d and its d along the old a, so the
    // profile's (d, y) is (-a, y) here and the run is an extrusion along d.
    const KitFrame g = {{f->origin[0], f->origin[1], f->origin[2]}, f->yaw + 0.5f * GLM_PIf};
    vec2 outline[KIT_MAX_OUTLINE] = {{0.0f}};
    for (int i = 0; i < count; i++) {
        outline[i][0] = -profile[i][0];
        outline[i][1] = profile[i][1];
    }
    kit_frame_extrude(kit, &g, mat, outline, count, a0, a1);
}

void kit_frame_surround(Kit* kit, const KitFrame* f, int mat, const KitOpening* o, float w,
                        bool head_only, float d0, float d1) {
    const KitOpening g = kit_opening_grow(o, w);
    if (!kit_opening_arched(o)) {
        kit_frame_box(kit, f, mat, g.from, g.to, o->top, g.top, d0, d1, false);
        if (head_only)
            return;
        kit_frame_box(kit, f, mat, g.from, o->from, o->bottom, o->top, d0, d1, false);
        kit_frame_box(kit, f, mat, o->to, g.to, o->bottom, o->top, d0, d1, false);
        return;
    }
    vec2 in[KIT_ARCH_POINTS], out[KIT_ARCH_POINTS];
    arch_outline(o->arch, o->from, o->to, o->top, o->rise, in);
    arch_outline(g.arch, g.from, g.to, g.top, g.rise, out);
    // Each half as one polygon: up its inner edge to the inner crown, across to the outer
    // crown and back down its outer edge -- with the jamb below, unless only the head.
    const int n = KIT_ARCH_SEGMENTS;
    vec2 half[2 * KIT_ARCH_SEGMENTS + 6];
    for (int side = 0; side < 2; side++) {
        int c = 0;
        if (!head_only)
            glm_vec2_copy((vec2){side ? o->to : o->from, o->bottom}, half[c++]);
        for (int i = 0; i <= n; i++)
            glm_vec2_copy(in[side ? 2 * n - i : i], half[c++]);
        for (int i = n; i >= 0; i--)
            glm_vec2_copy(out[side ? 2 * n - i : i], half[c++]);
        if (!head_only)
            glm_vec2_copy((vec2){side ? g.to : g.from, o->bottom}, half[c++]);
        kit_frame_extrude(kit, f, mat, half, c, d0, d1);
    }
}

void kit_frame_plug(Kit* kit, const KitFrame* f, const KitOpening* o, float d, float thick) {
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, o->from, o->to, o->bottom, kit_opening_crown(o),
                  d - 0.5f * thick, d + 0.5f * thick, true);
}

void kit_frame_pane(Kit* kit, const KitFrame* f, int mat, const KitOpening* o, float d,
                    float thick) {
    vec2 outline[KIT_OPENING_POINTS];
    const int n = kit_opening_outline(o, outline);
    kit_frame_extrude(kit, f, mat, outline, n, d - KIT_PANE_HALF, d + KIT_PANE_HALF);
    kit_frame_plug(kit, f, o, d, thick);
}

void kit_frame_stair(Kit* kit, const KitFrame* f, int mat, float a0, float a1, float y0, float d0,
                     float rise, float going, int risers) {
    for (int i = 1; i < risers; i++)
        kit_frame_box(kit, f, mat, a0, a1, y0, y0 + (float)i * rise, d0 + (float)(i - 1) * going,
                      d0 + (float)i * going, true);
}

void kit_frame_prism(Kit* kit, const KitFrame* f, int mat, float a, float d, float y0, float y1,
                     float r, int sides) {
    vec3 p = {0.0f, 0.0f, 0.0f};
    kit_frame_point(f, a, 0.0f, d, p);
    kit_prism(kit, mat, p[0], p[2], f->origin[1] + y0, f->origin[1] + y1, r, sides, false);
}

void kit_frame_bar(Kit* kit, const KitFrame* f, int mat, float a0, float a1, float y, float d,
                   float r) {
    vec3 p = {0.0f, 0.0f, 0.0f};
    kit_frame_point(f, 0.5f * (a0 + a1), y, d, p);
    // The frames here are quarter turns, so "along the wall" is world X or Z.
    const bool along_x = fabsf(cosf(f->yaw)) > 0.5f;
    kit_prism_lying(kit, mat, p, 0.5f * fabsf(a1 - a0), r, 6, along_x);
}

static int clamp_sides(int sides) {
    return sides < 3 ? 3 : sides > KIT_MAX_SIDES ? KIT_MAX_SIDES : sides;
}

void kit_frame_pipe(Kit* kit, const KitFrame* f, int mat, const vec3* path, int count, float r,
                    int sides) {
    if (!slot_ok(kit, mat) || !points_ok(count))
        return;
    vec3 world[KIT_MAX_POINTS] = {{0.0f}};
    for (int i = 0; i < count; i++)
        kit_frame_point(f, path[i][0], path[i][1], path[i][2], world[i]);
    pipe(kit, mat, world, count, r, clamp_sides(sides));
}

void kit_frame_lathe(Kit* kit, const KitFrame* f, int mat, float a, float d, float y,
                     const vec2* profile, int count, int sides) {
    if (!slot_ok(kit, mat) || !points_ok(count))
        return;
    vec3 base = {0.0f, 0.0f, 0.0f};
    kit_frame_point(f, a, y, d, base);
    const vec3 up = {0.0f, 1.0f, 0.0f}, x = {1.0f, 0.0f, 0.0f}, z = {0.0f, 0.0f, 1.0f};
    lathe(kit, mat, base, up, x, z, profile, count, clamp_sides(sides));
}

void kit_frame_lathe_on(Kit* kit, const KitFrame* f, int mat, const vec3 base, const vec3 axis,
                        const vec2* profile, int count, int sides) {
    if (!slot_ok(kit, mat) || !points_ok(count))
        return;
    vec3 at = {0.0f, 0.0f, 0.0f}, dir = {0.0f, 0.0f, 0.0f}, u = {0.0f, 0.0f, 0.0f};
    vec3 w = {0.0f, 0.0f, 0.0f};
    kit_frame_point(f, base[0], base[1], base[2], at);
    kit_frame_dir(f, axis[0], axis[1], axis[2], dir);
    if (glm_vec3_norm2(dir) < 1e-12f)
        return;
    glm_vec3_normalize(dir);
    section_start(dir, u);
    glm_vec3_cross(dir, u, w);
    lathe(kit, mat, at, dir, u, w, profile, count, clamp_sides(sides));
}

void kit_frame_card(Kit* kit, const KitFrame* f, int mat, const vec3 corner, const vec3 across,
                    const vec3 up, const float uv[4]) {
    vec3 p[4] = {{0.0f}}, along = {0.0f, 0.0f, 0.0f}, rise = {0.0f, 0.0f, 0.0f};
    vec3 n = {0.0f, 0.0f, 0.0f};
    kit_frame_point(f, corner[0], corner[1], corner[2], p[0]);
    kit_frame_dir(f, across[0], across[1], across[2], along);
    kit_frame_dir(f, up[0], up[1], up[2], rise);
    glm_vec3_add(p[0], along, p[1]);
    glm_vec3_add(p[1], rise, p[2]);
    glm_vec3_add(p[0], rise, p[3]);
    glm_vec3_cross(along, rise, n);
    glm_vec3_normalize(along);
    const vec2 tex[4] = {{uv[0], uv[1]}, {uv[2], uv[1]}, {uv[2], uv[3]}, {uv[0], uv[3]}};
    polygon(kit, mat, p, 4, n, tex, along);
}

void kit_frame_card_lying(Kit* kit, const KitFrame* f, int mat, const float uv[4], float w, float h,
                          float a, float y, float d, float turn) {
    const float c = cosf(turn), s = sinf(turn);
    const vec3 across = {w * c, 0.0f, w * s}, up = {h * s, 0.0f, -h * c};
    const vec3 corner = {a - 0.5f * (across[0] + up[0]), y, d - 0.5f * (across[2] + up[2])};
    kit_frame_card(kit, f, mat, corner, across, up, uv);
}

void kit_frame_card_rect(Kit* kit, const KitFrame* f, int mat, const float uv[4], float a0,
                         float a1, float y0, float y1, float d, float toward) {
    // Seen from -d, +a runs leftward, so the card starts at a1 and runs back.
    const bool front = toward > 0.0f;
    kit_frame_card(kit, f, mat, (vec3){front ? a0 : a1, y0, d},
                   (vec3){front ? a1 - a0 : a0 - a1, 0.0f, 0.0f}, (vec3){0.0f, y1 - y0, 0.0f}, uv);
}

void kit_frame_card_row(Kit* kit, const KitFrame* f, int mat, const float uv[4], float a0, float a1,
                        float y0, float y1, float d, float toward, float width) {
    const float len = a1 - a0;
    const int n = (int)fmaxf(1.0f, roundf(len / width));
    for (int i = 0; i < n; i++)
        kit_frame_card_rect(kit, f, mat, uv, a0 + len * (float)i / (float)n,
                            a0 + len * (float)(i + 1) / (float)n, y0, y1, d, toward);
}

/*
 * What shadows draw in place of the kit's meshes (MESH_SHADOW_ONLY): the triangles of every
 * material that casts plainly, merged across materials into one mesh per cell of a grid, by
 * where each triangle's centroid falls. A shadow face that sees a corner of the world -- a
 * candle's, above all -- then draws that corner in a few draws, where the per-material meshes
 * the camera draws each span the street and are in every face. Cutting those meshes instead
 * multiplied the camera's draws by the materials in a cell and slowed the frame (spec 13.16).
 * A triangle longer than KIT_CELL_LARGE on any axis goes to one large cell, where it cannot
 * stretch a cell's bounds across the street. Within a cell, triangles keep the order built. The
 * three sizes are each scaled by the kit's shadow_cell_scale.
 */
#define KIT_CELL_XZ    3.0f
#define KIT_CELL_Y     3.5f
#define KIT_CELL_LARGE 6.0f
#define KIT_CELL_BIAS  (1 << 20) // keeps a cell index positive in its 21 bits of key

typedef struct CellTri {
    int64_t key;
    unsigned int mat;
    unsigned int tri;
} CellTri;

static int cell_tri_order(const void* a, const void* b) {
    const CellTri* x = a;
    const CellTri* y = b;
    if (x->key != y->key)
        return x->key < y->key ? -1 : 1;
    if (x->mat != y->mat)
        return x->mat < y->mat ? -1 : 1;
    return x->tri < y->tri ? -1 : (x->tri > y->tri ? 1 : 0);
}

static int64_t cell_key(const Kit* kit, const MeshBuilder* mb, unsigned int tri) {
    const float s = kit->shadow_cell_scale;
    AABB bounds;
    aabb_empty(&bounds);
    vec3 c = {0.0f, 0.0f, 0.0f};
    for (int k = 0; k < 3; k++) {
        const float* p = &mb->pos[mb->idx[tri * 3 + k] * 3];
        aabb_add_point(&bounds, p);
        glm_vec3_add(c, (float*)p, c);
    }
    vec3 extent = {0.0f, 0.0f, 0.0f};
    glm_vec3_sub(bounds.max, bounds.min, extent);
    if (glm_vec3_max(extent) > KIT_CELL_LARGE * s)
        return INT64_MAX;
    glm_vec3_scale(c, 1.0f / 3.0f, c);
    const int64_t ix = (int64_t)floorf(c[0] / (KIT_CELL_XZ * s)) + KIT_CELL_BIAS;
    const int64_t iy = (int64_t)floorf(c[1] / (KIT_CELL_Y * s)) + KIT_CELL_BIAS;
    const int64_t iz = (int64_t)floorf(c[2] / (KIT_CELL_XZ * s)) + KIT_CELL_BIAS;
    return (ix << 42) | (iy << 21) | iz;
}

// Whether a material's shadow can be the cells': opaque, single-sided, still and drawn as the lit
// surface draws it. Anything else -- glass, a cutout, a swaying leaf, a surface its shader hook
// may move, or one drawn in the late draw, which casts nothing -- keeps casting from its own mesh,
// or not at all, exactly as before.
static bool casts_plainly(const Material* m) {
    return m->pass == MATERIAL_PASS_MAIN && !m->shader_hook && m->alpha_mode == ALPHA_OPAQUE &&
           m->opacity >= 1.0f && !m->opacity_tex && m->transmission <= 0.0f && !m->doubleSided &&
           m->wind_response == 0.0f;
}

// One cell's triangles, `count` of them from any of the builders, as one shape-only mesh on
// `node`. `remap` is each builder's vertex index into the cell, -1 where not yet copied, and is
// left all -1 again. Returns 1, or 0 if it could not be built.
static int shadow_cell(Kit* kit, SceneNode* node, Material* shape, const CellTri* tris,
                       size_t count, int* const* remap) {
    MeshBuilder part;
    if (!mb_init(&part, count * 2, count * 3, false))
        return 0;
    for (size_t i = 0; i < count; i++) {
        const MeshBuilder* mb = &kit->builders[tris[i].mat];
        int* map = remap[tris[i].mat];
        unsigned int v[3];
        for (int k = 0; k < 3; k++) {
            const unsigned int s = mb->idx[tris[i].tri * 3 + k];
            if (map[s] < 0)
                map[s] = (int)mb_vertex(&part, &mb->pos[s * 3], &mb->nrm[s * 3], &mb->tan[s * 4],
                                        mb->uv0[s * 2], mb->uv0[s * 2 + 1], mb->uv1[s * 2],
                                        mb->uv1[s * 2 + 1], NULL);
            v[k] = (unsigned int)map[s];
        }
        mb_tri(&part, v[0], v[1], v[2]);
    }
    // A vertex the next cell shares is copied again for it.
    for (size_t i = 0; i < count; i++)
        for (int k = 0; k < 3; k++)
            remap[tris[i].mat][kit->builders[tris[i].mat].idx[tris[i].tri * 3 + k]] = -1;
    Mesh* mesh = create_mesh();
    if (!mb_transfer(&part, mesh)) {
        free_mesh(mesh);
        return 0;
    }
    mesh->material = shape;
    mesh->shadow_role = MESH_SHADOW_ONLY;
    node_add_mesh(node, mesh);
    return 1;
}

// The shadow cells on `node`. Returns how many: 0 leaves every material casting from its own
// mesh.
static int shadow_cells(Kit* kit, SceneNode* node) {
    size_t tris = 0;
    int first = -1;
    for (int i = 0; i < kit->material_count; i++) {
        const MeshBuilder* mb = &kit->builders[i];
        if (!mb->ok || mb->icount == 0 || !casts_plainly(kit->materials[i]))
            continue;
        tris += mb->icount / 3;
        if (first < 0)
            first = i;
    }
    if (tris == 0)
        return 0;
    CellTri* order = malloc(tris * sizeof(*order));
    int* remap[KIT_MAX_MATERIALS] = {NULL};
    bool ok = order != NULL;
    size_t n = 0;
    for (int i = 0; ok && i < kit->material_count; i++) {
        const MeshBuilder* mb = &kit->builders[i];
        if (!mb->ok || mb->icount == 0 || !casts_plainly(kit->materials[i]))
            continue;
        remap[i] = malloc(mb->vcount * sizeof(int));
        ok = remap[i] != NULL;
        for (size_t v = 0; ok && v < mb->vcount; v++)
            remap[i][v] = -1;
        for (size_t t = 0; ok && t < mb->icount / 3; t++)
            order[n++] =
                (CellTri){cell_key(kit, mb, (unsigned int)t), (unsigned int)i, (unsigned int)t};
    }
    int cells = 0;
    Material* shape = ok ? create_material() : NULL;
    if (shape) {
        shape->name = safe_strdup("shadow_cells");
        material_set_program(shape, kit->materials[first]->shader_program);
        qsort(order, n, sizeof(*order), cell_tri_order);
        for (size_t run = 0; run < n;) {
            size_t end = run;
            while (end < n && order[end].key == order[run].key)
                end++;
            cells += shadow_cell(kit, node, shape, order + run, end - run, remap);
            run = end;
        }
        if (cells == 0)
            free_material(shape);
    }
    free(order);
    for (int i = 0; i < KIT_MAX_MATERIALS; i++)
        free(remap[i]);
    return cells;
}

SceneNode* kit_finish(Kit* kit, const char* name) {
    SceneNode* node = create_node();
    node_set_name(node, name);
    // The cells first: they read the builders, which handing a mesh over empties.
    kit->shadow_cell_count = kit->casts_nothing ? 0 : shadow_cells(kit, node);
    for (int i = 0; i < kit->material_count; i++) {
        MeshBuilder* mb = &kit->builders[i];
        kit->vertex_count += (int)mb->vcount;
        if (mb->vcount == 0) {
            mb_free(mb);
            // A material another kit shares may be used there: kit_free_unused decides.
            if (!kit->shares_materials) {
                free_material(kit->materials[i]);
                kit->materials[i] = NULL;
            }
            continue;
        }
        kit->used[i] = true;
        Mesh* mesh = create_mesh();
        if (!mb_transfer(mb, mesh)) {
            free_mesh(mesh);
            continue;
        }
        mesh->material = kit->materials[i];
        // What the camera draws; where the cells were built, they are its shadow.
        if (kit->casts_nothing || (kit->shadow_cell_count > 0 && casts_plainly(mesh->material)))
            mesh->shadow_role = MESH_SHADOW_NONE;
        node_add_mesh(node, mesh);
        kit->mesh_count++;
    }
    node_add_child(kit->scene->root_node, node);
    return node;
}
