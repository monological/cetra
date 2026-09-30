#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cetra/mesh.h"

#include "kit.h"

void kit_init(Kit* kit, Scene* scene, EntityManager* em, PhysicsWorld* physics) {
    memset(kit, 0, sizeof(*kit));
    kit->scene = scene;
    kit->em = em;
    kit->physics = physics;
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

// `grime` is 0..1; a builder without colours ignores it. UVs in repeats.
static unsigned int kit_vertex(Kit* kit, int mat, const vec3 p, const vec3 n, const vec3 t, float u,
                               float v, float grime) {
    const float rgba[4] = {1.0f + (GRIME_TINT[0] - 1.0f) * grime,
                           1.0f + (GRIME_TINT[1] - 1.0f) * grime,
                           1.0f + (GRIME_TINT[2] - 1.0f) * grime, 1.0f};
    return mb_vertex(&kit->builders[mat], p, n, t, u, v, u, v, rgba);
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
// GRIME_STEP. Returns the count, at most GRIME_MAX_CUTS.
static int grime_cuts(float len, float band, float* out) {
    int n = 0;
    out[n++] = 0.0f;
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

// Turned about +Y by `yaw`, the same sense entity_set_rotation_euler turns a body.
static void rotate_y(const vec3 in, float yaw, vec3 out) {
    const float c = cosf(yaw), s = sinf(yaw);
    const float x = in[0] * c + in[2] * s;
    const float z = -in[0] * s + in[2] * c;
    out[0] = x;
    out[1] = in[1];
    out[2] = z;
}

void kit_collider(Kit* kit, const vec3 centre, const vec3 half, float yaw) {
    if (!kit->em || !kit->physics)
        return;
    char name[48];
    snprintf(name, sizeof(name), "kit_col_%d", kit->collider_count++);
    Entity* e = create_entity(kit->em, name);
    if (!e)
        return;
    glm_vec3_copy((float*)centre, e->position);
    // Before the body, which reads the entity's rotation when it is created.
    if (yaw != 0.0f)
        entity_set_rotation_euler(e, (vec3){0.0f, yaw, 0.0f});
    PhysicsShapeDesc shape = {
        .type = SHAPE_BOX, .box.half_extents = {half[0], half[1], half[2]}, .density = 0.0f};
    entity_add_rigid_body(e, kit->physics, &shape, MOTION_STATIC, OBJ_LAYER_STATIC);
}

void kit_box(Kit* kit, int mat, const vec3 centre, const vec3 half, float yaw, bool collide) {
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
    const bool grimed = slot_ok(kit, mat) && kit->grime[mat] > 0.0f;
    for (int f = 0; f < 6; f++) {
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
 * solid is cut every GRIME_RING along its length; a clean one keeps only the
 * rings its shape asks for.
 */
#define GRIME_RING 0.01f // metres

// How many pieces a stretch `len` long is cut into.
static int ring_pieces(const Kit* kit, int mat, float len) {
    if (kit->grime[mat] <= 0.0f)
        return 1;
    const int n = (int)ceilf(len / GRIME_RING);
    return n < 1 ? 1 : n > GRIME_MAX_CUTS ? GRIME_MAX_CUTS : n;
}

// Where one ring sits and how it shades.
typedef struct RingAt {
    vec3 centre, axis, u, w; // u takes the cosine and w the sine
    float r;
    float nr, na; // the normal: the radial direction weighted nr, plus the axis weighted na
    float v;      // texture V, in repeats
    float joint;  // metres to the solid's nearest joint
} RingAt;

// A ring of sides + 1 vertices. U runs round it `u_scale` per radian, and its
// grime falls off from the joint over `band`.
static void make_ring(Kit* kit, int mat, Ring* ring, const RingAt* at, float u_scale, float band,
                      int sides) {
    const float strength = kit->grime[mat];
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
        ring->idx[j] = kit_vertex(kit, mat, ring->p[j], ring->n[j], t, a * u_scale, at->v, grime);
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
    float len = 0.0f;
    int made = 0;
    for (int i = 0; i < count; i++) {
        // The segment arriving at point i, cut into pieces; point 0 is one ring.
        const int pieces =
            i == 0 ? 1
                   : ring_pieces(kit, mat, glm_vec3_distance((float*)path[i], (float*)path[i - 1]));
        for (int s = i == 0 ? pieces : 1; s <= pieces; s++) {
            if (s < pieces) {
                // Inside a segment, a ring faces along it.
                glm_vec3_lerp((float*)path[i - 1], (float*)path[i], (float)s / (float)pieces,
                              at.centre);
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
                // Any direction off the axis starts the section; up unless the
                // pipe sets off nearly upright.
                const bool upright = fabsf(at.axis[1]) >= 0.9f;
                const vec3 ref = {upright ? 1.0f : 0.0f, upright ? 0.0f : 1.0f, 0.0f};
                glm_vec3_cross(at.axis, (float*)ref, at.u);
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
 * A surface of revolution about the upright through `base`, from a profile of
 * {radius, height above base} points. Every segment is its own run of bands,
 * so a crease costs nothing extra; a radius of 0 closes the surface on its
 * axis. It stands on its lowest point, so that is its joint.
 */
static void lathe(Kit* kit, int mat, const vec3 base, const vec2* profile, int count, int sides) {
    const float inv = 1.0f / kit->repeat_m[mat];
    float rmax = 0.0f, lo = profile[0][1], hi = profile[0][1], len = 0.0f;
    for (int k = 0; k < count; k++) {
        rmax = glm_max(rmax, profile[k][0]);
        lo = glm_min(lo, profile[k][1]);
        hi = glm_max(hi, profile[k][1]);
    }
    const float band = fminf(GRIME_BAND, 0.3f * (hi - lo));
    RingAt at = {.axis = {0.0f, 1.0f, 0.0f}, .u = {1.0f, 0.0f, 0.0f}, .w = {0.0f, 0.0f, 1.0f}};
    for (int k = 0; k < count - 1; k++) {
        vec2 n0 = {0.0f, 0.0f}, n1 = {0.0f, 0.0f};
        profile_joint(profile, count, k, k - 1, n0);
        profile_joint(profile, count, k, k + 1, n1);
        const float seg = glm_vec2_distance((float*)profile[k + 1], (float*)profile[k]);
        const int pieces = ring_pieces(kit, mat, seg);
        Ring rings[2];
        for (int s = 0; s <= pieces; s++) {
            const float f = (float)s / (float)pieces;
            vec2 p = {0.0f, 0.0f}, n = {0.0f, 0.0f};
            glm_vec2_lerp((float*)profile[k], (float*)profile[k + 1], f, p);
            glm_vec2_lerp(n0, n1, f, n);
            glm_vec3_copy((float*)base, at.centre);
            at.centre[1] += p[1];
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

// One slab of a wall layer, between `a` and `b` along the wall and `y0`..`y1`.
static void wall_slab(Kit* kit, const KitWall* w, int mat, float offset, float thick, float a,
                      float b, float y0, float y1) {
    if (b - a < 1e-4f || y1 - y0 < 1e-4f)
        return;
    const float mid = 0.5f * (a + b), half_len = 0.5f * (b - a);
    vec3 centre, half;
    if (w->along_x) {
        glm_vec3_copy((vec3){mid, 0.5f * (y0 + y1), w->at + offset}, centre);
        glm_vec3_copy((vec3){half_len, 0.5f * (y1 - y0), 0.5f * thick}, half);
    } else {
        glm_vec3_copy((vec3){w->at + offset, 0.5f * (y0 + y1), mid}, centre);
        glm_vec3_copy((vec3){0.5f * thick, 0.5f * (y1 - y0), half_len}, half);
    }
    kit_box(kit, mat, centre, half, 0.0f, false);
}

// The wall between openings, and under and over each one, for one layer.
// `sorted` holds the wall's `n` openings in order along it.
static void wall_layer(Kit* kit, const KitWall* w, const KitOpening* sorted, int n, int mat,
                       float offset, float thick) {
    float cursor = w->from;
    for (int i = 0; i < n; i++) {
        const KitOpening* o = &sorted[i];
        wall_slab(kit, w, mat, offset, thick, cursor, o->from, w->y0, w->y1);
        wall_slab(kit, w, mat, offset, thick, o->from, o->to, w->y0, o->bottom);
        wall_slab(kit, w, mat, offset, thick, o->from, o->to, o->top, w->y1);
        cursor = o->to;
    }
    wall_slab(kit, w, mat, offset, thick, cursor, w->to, w->y0, w->y1);
}

void kit_wall(Kit* kit, const KitWall* w) {
    // Openings in order along the wall, so the solid spans are the gaps.
    KitOpening sorted[KIT_MAX_OPENINGS];
    const int n = w->opening_count < KIT_MAX_OPENINGS ? w->opening_count : KIT_MAX_OPENINGS;
    memcpy(sorted, w->openings, (size_t)n * sizeof(KitOpening));
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && sorted[j].from < sorted[j - 1].from; j--) {
            KitOpening t = sorted[j];
            sorted[j] = sorted[j - 1];
            sorted[j - 1] = t;
        }
    const float q = 0.25f * w->thick;
    const float s = w->inner >= 0 ? 1.0f : -1.0f;
    wall_layer(kit, w, sorted, n, w->mat_inner, s * q, 0.5f * w->thick);
    wall_layer(kit, w, sorted, n, w->mat_outer, -s * q, 0.5f * w->thick);
    // One body per span through the whole thickness, rather than one per layer:
    // two coplanar bodies meeting mid-wall buy nothing.
    wall_layer(kit, w, sorted, n, KIT_COLLIDER_ONLY, 0.0f, w->thick);
}

void kit_frame_point(const KitFrame* f, float a, float y, float d, vec3 out) {
    kit_frame_dir(f, a, y, d, out);
    glm_vec3_add(out, (float*)f->origin, out);
}

void kit_frame_dir(const KitFrame* f, float a, float y, float d, vec3 out) {
    const vec3 local = {a, y, d};
    rotate_y(local, f->yaw, out);
}

void kit_frame_box(Kit* kit, const KitFrame* f, int mat, float a0, float a1, float y0, float y1,
                   float d0, float d1, bool collide) {
    vec3 centre = {0.0f, 0.0f, 0.0f};
    kit_frame_point(f, 0.5f * (a0 + a1), 0.5f * (y0 + y1), 0.5f * (d0 + d1), centre);
    const vec3 half = {0.5f * fabsf(a1 - a0), 0.5f * fabsf(y1 - y0), 0.5f * fabsf(d1 - d0)};
    kit_box(kit, mat, centre, half, f->yaw, collide);
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
    lathe(kit, mat, base, profile, count, clamp_sides(sides));
}

SceneNode* kit_finish(Kit* kit, const char* name) {
    SceneNode* node = create_node();
    node_set_name(node, name);
    for (int i = 0; i < kit->material_count; i++) {
        MeshBuilder* mb = &kit->builders[i];
        kit->vertex_count += (int)mb->vcount;
        if (mb->vcount == 0) {
            mb_free(mb);
            free_material(kit->materials[i]);
            kit->materials[i] = NULL;
            continue;
        }
        Mesh* mesh = create_mesh();
        if (!mb_transfer(mb, mesh)) {
            free_mesh(mesh);
            continue;
        }
        mesh->material = kit->materials[i];
        node_add_mesh(node, mesh);
    }
    node_add_child(kit->scene->root_node, node);
    return node;
}
