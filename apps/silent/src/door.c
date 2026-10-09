#include <math.h>

#include "door.h"
#include "layout.h"
#include "mats.h"

#define DOOR_SECONDS  1.2f  // shut to open, eased at both ends: a heavy door
#define DOOR_SILL_GAP 0.02f // a hung leaf's foot over the floor
#define PLANK         0.17f // the boards' width, seams between them
#define STRAP_H       0.06f
#define STRAP_T       0.006f

// The leaf's middle, in the world, and the yaw it is turned to, at a swing of `travel`.
static void pose(const Door* door, vec3 centre, float* yaw) {
    const float eased = glm_smoothstep(0.0f, 1.0f, door->travel);
    *yaw = door->yaw - door->swing * eased;
    const float half_w = 0.5f * (door->shape.to - door->shape.from);
    const float half_h = 0.5f * (kit_opening_crown(&door->shape) - door->shape.bottom);
    const KitFrame now = {{door->hinge[0], door->hinge[1], door->hinge[2]}, *yaw};
    kit_frame_point(&now, door->shape.from + half_w, 0.0f, 0.0f, centre);
    centre[1] = door->shape.bottom + half_h;
}

// The leaf's head height at a, from its outline: straight up the jambs, then the arch.
static float head_at(const vec2* outline, int count, float a) {
    float top = outline[2][1];
    for (int i = 2; i + 1 < count; i++) {
        const float a0 = outline[i][0], a1 = outline[i + 1][0];
        if ((a - a0) * (a - a1) <= 0.0f && fabsf(a1 - a0) > 1e-5f) {
            const float t = (a - a0) / (a1 - a0);
            top = fmaxf(top, outline[i][1] + t * (outline[i + 1][1] - outline[i][1]));
        }
    }
    return top;
}

// Oak boards with dark seams down both faces, two iron straps across the outside (-d) ending
// in spear points, studded, with their knuckles at the hinge (`from`), and a ring to pull on
// either side by the latch.
void door_leaf(Kit* kit, const KitFrame* f, const KitOpening* o, float t) {
    vec2 outline[KIT_OPENING_POINTS];
    const int n = kit_opening_outline(o, outline);
    kit_frame_extrude(kit, f, MAT_WOOD, outline, n, -0.5f * t, 0.5f * t);
    const float w = o->to - o->from;
    for (float a = o->from + PLANK; a < o->to - 0.05f; a += PLANK) {
        const float top = head_at(outline, n, a) - 0.01f;
        for (int side = -1; side <= 1; side += 2) {
            const float d = (float)side * 0.5f * t;
            kit_frame_box(kit, f, MAT_BLACK, a - 0.003f, a + 0.003f, o->bottom + 0.01f, top, d,
                          d + (float)side * 0.001f, false);
        }
    }
    const float out = -0.5f * t, h = kit_opening_crown(o) - o->bottom;
    const float heights[2] = {o->bottom + 0.28f, o->bottom + 0.62f * h};
    for (int s = 0; s < 2; s++) {
        const float y = heights[s], end = o->from + 0.76f * w;
        const vec2 strap[] = {{o->from + 0.01f, y - 0.5f * STRAP_H},
                              {end, y - 0.5f * STRAP_H},
                              {end + 0.04f, y - STRAP_H},
                              {end + 0.11f, y},
                              {end + 0.04f, y + STRAP_H},
                              {end, y + 0.5f * STRAP_H},
                              {o->from + 0.01f, y + 0.5f * STRAP_H}};
        kit_frame_extrude(kit, f, MAT_IRON, strap, KIT_COUNT(strap), out, out - STRAP_T);
        for (float a = o->from + 0.08f; a < end; a += 0.13f)
            kit_frame_box(kit, f, MAT_IRON, a - 0.01f, a + 0.01f, y - 0.01f, y + 0.01f,
                          out - STRAP_T, out - STRAP_T - 0.008f, false);
        // The knuckle round the pintle at the hinge edge.
        kit_frame_prism(kit, f, MAT_IRON, o->from, 0.0f, y - 0.6f * STRAP_H, y + 0.6f * STRAP_H,
                        0.016f, 8);
    }
    // A boss and a hanging ring on each face, and the keyhole's plate under them.
    const float ra = o->from + 0.86f * w, ry = o->bottom + 1.0f, r = 0.065f;
    const vec2 boss[] = {
        {0.0f, 0.0f}, {0.035f, 0.0f}, {0.03f, 0.012f}, {0.012f, 0.02f}, {0.0f, 0.022f}};
    for (int side = -1; side <= 1; side += 2) {
        const float d = (float)side * 0.5f * t;
        kit_frame_lathe_on(kit, f, MAT_IRON, (vec3){ra, ry, d}, (vec3){0.0f, 0.0f, (float)side},
                           boss, KIT_COUNT(boss), 10);
        enum { LOOP = 13 };
        vec3 ring[LOOP];
        for (int i = 0; i < LOOP; i++) {
            const float th = 2.0f * GLM_PIf * (float)i / (float)(LOOP - 1);
            glm_vec3_copy(
                (vec3){ra + r * sinf(th), ry - r + r * cosf(th), d + (float)side * 0.026f},
                ring[i]);
        }
        kit_frame_pipe(kit, f, MAT_IRON, ring, LOOP, 0.008f, 6);
        kit_frame_box(kit, f, MAT_IRON, ra - 0.03f, ra + 0.03f, ry - 0.3f, ry - 0.16f, d,
                      d + (float)side * 0.004f, false);
    }
}

/*
 * A cabin's door (spec 13.41): grey boards with dark seams down both faces, two ledges and a brace
 * between them across the inside (+d), a rusted strap hinge outside along each ledge's line with
 * its knuckle at the hinge (`from`), and a thumb latch -- a handle outside, its bar inside.
 */
void door_leaf_battened(Kit* kit, const KitFrame* f, const KitOpening* o, float t) {
    vec2 outline[KIT_OPENING_POINTS];
    const int n = kit_opening_outline(o, outline);
    kit_frame_extrude(kit, f, MAT_FENCE_BOARD, outline, n, -0.5f * t, 0.5f * t);
    for (float a = o->from + PLANK; a < o->to - 0.05f; a += PLANK) {
        const float top = head_at(outline, n, a) - 0.01f;
        for (int side = -1; side <= 1; side += 2) {
            const float d = (float)side * 0.5f * t;
            kit_frame_box(kit, f, MAT_BLACK, a - 0.003f, a + 0.003f, o->bottom + 0.01f, top, d,
                          d + (float)side * 0.001f, false);
        }
    }
    const float in = 0.5f * t, w = o->to - o->from, h = kit_opening_crown(o) - o->bottom;
    const float ledge[2] = {o->bottom + 0.28f, o->bottom + h - 0.3f}, lw = 0.06f;
    for (int s = 0; s < 2; s++)
        kit_frame_box(kit, f, MAT_FENCE_BOARD, o->from + 0.04f, o->to - 0.04f, ledge[s] - lw,
                      ledge[s] + lw, in, in + 0.025f, false);
    const float a0 = o->from + 0.08f, a1 = o->to - 0.08f, bw = 0.1f;
    const vec2 brace[] = {{a0, ledge[0] + lw},
                          {a1, ledge[1] - lw - bw},
                          {a1, ledge[1] - lw},
                          {a0, ledge[0] + lw + bw}};
    kit_frame_extrude(kit, f, MAT_FENCE_BOARD, brace, KIT_COUNT(brace), in, in + 0.022f);
    for (int s = 0; s < 2; s++) {
        const float y = ledge[s];
        kit_frame_box(kit, f, MAT_LAMP_POST, o->from + 0.01f, o->from + 0.45f * w, y - 0.022f,
                      y + 0.022f, -in, -in - 0.005f, false);
        kit_frame_prism(kit, f, MAT_LAMP_POST, o->from, 0.0f, y - 0.05f, y + 0.05f, 0.014f, 8);
    }
    // The latch: a bent handle on the outside, and the bar it lifts across the inside.
    const float la = o->from + 0.88f * w, ly = o->bottom + 1.0f;
    const vec3 handle[] = {{la, ly + 0.09f, -in},
                           {la, ly + 0.07f, -in - 0.035f},
                           {la, ly - 0.07f, -in - 0.035f},
                           {la, ly - 0.09f, -in}};
    kit_frame_pipe(kit, f, MAT_LAMP_POST, handle, KIT_COUNT(handle), 0.007f, 6);
    kit_frame_box(kit, f, MAT_LAMP_POST, la - 0.2f, la + 0.02f, ly + 0.03f, ly + 0.05f, in,
                  in + 0.012f, false);
}

/*
 * A four-panel door, painted, as every house on the street has: a slab with two short panels
 * over two tall ones, each a raised field in a sunk frame on both faces, and a brass knob on a
 * rose either side by the latch. Flat-headed, which is the only head it is hung in.
 */
void door_leaf_panelled(Kit* kit, const KitFrame* f, const KitOpening* o, float t) {
    const float w = o->to - o->from, h = o->top - o->bottom, half = 0.5f * t, sink = 0.006f;
    const float stile = 0.12f * w + 0.03f, rail = 0.11f, mid = 0.42f * h;
    const float cols[3] = {o->from + stile, o->from + 0.5f * w, o->to - stile};
    const float rows[2][2] = {{o->bottom + rail + 0.08f, o->bottom + mid - 0.5f * rail},
                              {o->bottom + mid + 0.5f * rail, o->top - rail}};
    // The stiles, rails and muntins at the leaf's full thickness, tiling all of it but the four
    // panels with no two overlapping: a face lying in another's plane fights it for depth, and
    // each box's grime is its own, so the fight shows as patches.
    kit_frame_box(kit, f, MAT_MOULDING, o->from, cols[0], o->bottom, o->top, -half, half, false);
    kit_frame_box(kit, f, MAT_MOULDING, cols[2], o->to, o->bottom, o->top, -half, half, false);
    const float rail_y[3][2] = {
        {o->bottom, rows[0][0]}, {rows[0][1], rows[1][0]}, {rows[1][1], o->top}};
    for (int r = 0; r < 3; r++)
        kit_frame_box(kit, f, MAT_MOULDING, cols[0], cols[2], rail_y[r][0], rail_y[r][1], -half,
                      half, false);
    for (int r = 0; r < 2; r++)
        kit_frame_box(kit, f, MAT_MOULDING, cols[1] - 0.5f * rail, cols[1] + 0.5f * rail,
                      rows[r][0], rows[r][1], -half, half, false);
    for (int c = 0; c < 2; c++) {
        const float a0 = c ? cols[1] + 0.5f * rail : cols[0];
        const float a1 = c ? cols[2] : cols[1] - 0.5f * rail;
        for (int r = 0; r < 2; r++) {
            const float y0 = rows[r][0], y1 = rows[r][1];
            // The panel sunk into the frame on both faces, and its raised field standing back
            // out of it on each.
            kit_frame_box(kit, f, MAT_MOULDING, a0, a1, y0, y1, -half + sink, half - sink, false);
            for (int side = -1; side <= 1; side += 2) {
                const float d = (float)side * half;
                kit_frame_box(kit, f, MAT_MOULDING, a0 + 0.035f, a1 - 0.035f, y0 + 0.035f,
                              y1 - 0.035f, d - (float)side * sink, d + (float)side * 0.004f, false);
            }
        }
    }
    const vec2 knob[] = {{0.0f, 0.0f},     {0.026f, 0.0f},   {0.024f, 0.008f},
                         {0.009f, 0.012f}, {0.008f, 0.045f}, {0.024f, 0.05f},
                         {0.029f, 0.065f}, {0.022f, 0.078f}, {0.0f, 0.08f}};
    const float ka = o->to - 0.07f, ky = o->bottom + 0.95f;
    for (int side = -1; side <= 1; side += 2)
        kit_frame_lathe_on(kit, f, MAT_BRASS, (vec3){ka, ky, (float)side * 0.5f * t},
                           (vec3){0.0f, 0.0f, (float)side}, knob, KIT_COUNT(knob), 12);
}

// The body and the node where the swing puts them.
static void place(Door* door) {
    float yaw = 0.0f;
    pose(door, door->entity->position, &yaw);
    entity_set_rotation_euler(door->entity, (vec3){0.0f, yaw, 0.0f});
}

bool door_build(Door* door, Engine* engine, Scene* scene, EntityManager* em, PhysicsWorld* physics,
                const char* name, DoorLeafFn leaf, const KitFrame* hinge, const KitOpening* shape,
                float thick, float swing) {
    *door = (Door){.yaw = hinge->yaw, .shape = *shape, .swing = swing};
    glm_vec3_copy((float*)hinge->origin, door->hinge);

    // Built round the leaf's middle, so the node's transform is the leaf's place and turn.
    const float half_w = 0.5f * (shape->to - shape->from);
    const float half_h = 0.5f * (kit_opening_crown(shape) - shape->bottom);
    const KitFrame local = {{-(shape->from + half_w), -(shape->bottom + half_h), 0.0f}, 0.0f};
    Kit kit;
    mats_kit(&kit, engine, scene);
    leaf(&kit, &local, shape, thick);
    SceneNode* node = kit_finish(&kit, name);

    door->entity = create_entity(em, name);
    if (!door->entity)
        return false;
    door->entity->node = node;
    place(door);
    PhysicsShapeDesc box = {
        .type = SHAPE_BOX, .box.half_extents = {half_w, half_h, 0.5f * thick}, .density = 0.0f};
    entity_add_rigid_body(door->entity, physics, &box, MOTION_KINEMATIC, OBJ_LAYER_KINEMATIC);
    return true;
}

bool door_hang(Door* door, Engine* engine, Scene* scene, EntityManager* em, PhysicsWorld* physics,
               const char* name, DoorLeafFn leaf, const KitFrame* hinge, KitOpening opening,
               float swing) {
    // Along the frame from the hinge, whichever jamb it is on: the frame's yaw says which way.
    opening.to -= opening.from;
    opening.from = 0.0f;
    KitOpening shape = kit_opening_grow(&opening, -DOOR_CLEARANCE);
    // Into the world: the opening's heights are its building's, whose ground the hinge's
    // height is, and the leaf stands a gap off the opening's sill.
    shape.top += hinge->origin[1];
    shape.bottom = hinge->origin[1] + opening.bottom + DOOR_SILL_GAP;
    return door_build(door, engine, scene, em, physics, name, leaf, hinge, &shape, DOOR_THICK,
                      swing);
}

bool door_will_open(const Door* door) {
    return door->want < 0.5f;
}

void door_toggle(Door* door) {
    door->want = door_will_open(door) ? 1.0f : 0.0f;
}

void door_update(Door* door, float dt) {
    if (!door->entity || door->travel == door->want)
        return;
    const float step = dt / DOOR_SECONDS;
    door->travel = door->want > door->travel ? fminf(door->want, door->travel + step)
                                             : fmaxf(door->want, door->travel - step);
    place(door);
}
