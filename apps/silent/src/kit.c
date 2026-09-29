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

int kit_material(Kit* kit, Material* material, float repeat_m) {
    if (kit->material_count >= KIT_MAX_MATERIALS) {
        fprintf(stderr, "silent: kit is out of material slots (%d)\n", KIT_MAX_MATERIALS);
        return 0;
    }
    const int slot = kit->material_count++;
    kit->materials[slot] = material;
    kit->repeat_m[slot] = repeat_m > 0.0f ? repeat_m : 1.0f;
    mb_init(&kit->builders[slot], 256, 384, false);
    return slot;
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

static unsigned int face_vertex(Kit* kit, int mat, const vec3 p, const vec3 n, const vec3 t,
                                const vec3 b) {
    const float inv = 1.0f / kit->repeat_m[mat];
    const float u = glm_vec3_dot((float*)p, (float*)t) * inv;
    const float v = glm_vec3_dot((float*)p, (float*)b) * inv;
    return mb_vertex(&kit->builders[mat], p, n, t, u, v, u, v, NULL);
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
        idx[i] = face_vertex(kit, mat, p[i], n, t, bt);
    for (int i = 2; i < count; i++)
        mb_tri(&kit->builders[mat], idx[0], idx[i - 1], idx[i]);
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
    for (int f = 0; f < 6; f++) {
        vec3 out = {0.0f, 0.0f, 0.0f};
        rotate_y(dirs[f], yaw, out);
        kit_quad_facing(kit, mat, corner[faces[f][0]], corner[faces[f][1]], corner[faces[f][2]],
                        corner[faces[f][3]], out);
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

SceneNode* kit_finish(Kit* kit, const char* name) {
    SceneNode* node = create_node();
    node_set_name(node, name);
    for (int i = 0; i < kit->material_count; i++) {
        MeshBuilder* mb = &kit->builders[i];
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
