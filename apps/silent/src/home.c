#include <math.h>
#include <string.h>

#include "cetra/light.h"
#include "cetra/material.h"
#include "cetra/mesh.h"
#include "cetra/procedural/tree_gen.h"
#include "cetra/procedural/vegetation_tex.h"
#include "cetra/program.h"
#include "cetra/texture.h"
#include "cetra/util.h"

#include "cards.h"
#include "home.h"
#include "house.h"
#include "houses.h"
#include "mats.h"
#include "ornament.h"

/*
 * The player's house (spec 13.25). The plan is layout.h's footprint without the tower: the hall
 * runs the whole depth on HALL_X0..HALL_X1, a cased opening across it on the front band's back
 * wall; the kitchen and the living room either side of its front half; the bathroom, a shut
 * laundry and the back bedroom east of its back half; and west of it the stair behind a shut
 * door, and at the hall's end the cellar stair, whose door opens, down to the basement under the
 * whole house (basement.c, spec 13.31). Every finish is a scan silent already has, tinted.
 *
 *     z            ground floor
 *     ^   +-------------+---+-----------------+
 *     |   |  cellar [c] | h |[b]  bedroom     |   [c] the cellar door
 *     |   +-------------+ a |                 |   STAIRWELL_WALL_Z
 *     |   |  stair  [s] | l |                 |
 *     |   +-------------+ l +------+----------+   HOME_SPLIT_Z
 *     |   |             |   [d] bath | laundry |
 *     |   |   living    +=c=+------+----------+   KITCHEN_BACK_Z, c the cased opening
 *     |   |  room      [o]  =    kitchen      |
 *     |   +-------------+[d]+----[win]--------+
 *     |                  porch
 *     +-------------------------------------------> x
 */

#define CORNER (0.5f * EXT_WALL)

// A plain house's roof, not the Gothic one's: 35 degrees, a short overhang.
#define HOME_PITCH    0.7f
#define EAVE_OVERHANG 0.4f
#define RAKE_OVERHANG 0.3f

#define GROUND_SILL (FLOOR_Y + 0.85f)
#define GROUND_HEAD (FLOOR_Y + 2.15f)
#define UP_SILL     (FLOOR2_Y + 0.85f)
#define UP_HEAD     (FLOOR2_Y + 2.0f)

// The cased opening across the hall, and the window at its far end.
#define CASED_X0    (HALL_X0 + 0.25f)
#define CASED_X1    (HALL_X1 - 0.25f)
#define CASED_HEAD  (FLOOR_Y + 2.15f)
#define HALL_WIN_X0 (-1.1f)
#define HALL_WIN_X1 (-0.4f)
// The basement's door at the hall's end, and the window high in the back wall over its flight.
#define BASEMENT_DOOR_Z0 18.35f
#define BASEMENT_DOOR_Z1 19.15f
#define STAIR_WIN_X0     (-3.7f)
#define STAIR_WIN_X1     (-3.1f)
#define STAIR_WIN_SILL   (FLOOR_Y + 1.3f)
#define STAIR_WIN_HEAD   (FLOOR_Y + 2.0f)

// The skirting and the crown.
#define SKIRT_H     0.14f
#define SKIRT_PROUD 0.016f

#define DOOR_SWING 1.7f

#define WIN(a0, a1, sill, head) {a0, a1, sill, head, KIT_ARCH_FLAT, 0.0f, false}
#define DOORWAY(a0, a1, head)   {a0, a1, FLOOR_Y, head, KIT_ARCH_FLAT, 0.0f, true}
#define OUTSIDE(along, at_, from_, to_, inner_)                                      \
    .along_x = along, .at = at_, .from = from_, .to = to_, .y0 = 0.0f, .y1 = EAVE_Y, \
    .thick = EXT_WALL, .inner = inner_, .mat_inner = MAT_PLASTER, .mat_outer = MAT_SIDING
#define INSIDE(along, at_, from_, to_, inner_, in, out)                                 \
    .along_x = along, .at = at_, .from = from_, .to = to_, .y0 = FLOOR_Y, .y1 = CEIL_Y, \
    .thick = INT_WALL, .inner = inner_, .mat_inner = in, .mat_outer = out

typedef enum {
    HW_FRONT,
    HW_BACK,
    HW_WEST,
    HW_EAST,
    HW_HALL_E_KITCHEN, // the hall's east side, beside the kitchen,
    HW_HALL_E_BATH,    // the bathroom,
    HW_HALL_E_BEDROOM, // and the back bedroom
    HW_HALL_W_LIVING,  // its west side, beside the living room,
    HW_HALL_W_STAIR,   // and the stair and the basement
    HW_CASED,          // across it, with the cased opening
    HW_KITCHEN_BACK,
    HW_BATH_EAST,
    HW_SPLIT_EAST, // behind the bathroom and the laundry
    HW_SPLIT_WEST, // behind the living room
    HW_STAIRWELL,  // closing the stairwell down off the stair up
    HW_COUNT
} HomeWall;

// The openings named outside the table, by their index in their wall's.
enum {
    O_FRONT_DOOR = 0,    // HW_FRONT
    O_KITCHEN_DOOR = 0,  // HW_HALL_E_KITCHEN
    O_BATH_DOOR = 0,     // HW_HALL_E_BATH
    O_BEDROOM_DOOR = 0,  // HW_HALL_E_BEDROOM
    O_LIVING_DOOR = 0,   // HW_HALL_W_LIVING
    O_STAIR_DOOR = 0,    // HW_HALL_W_STAIR
    O_BASEMENT_DOOR = 1, // HW_HALL_W_STAIR
    O_CASED_OPENING = 0, // HW_CASED
};

static const KitWall WALLS[HW_COUNT] = {
    // The front: the door, the kitchen's window, the living room's, and three upstairs.
    [HW_FRONT] = {OUTSIDE(true, HOUSE_FRONT_Z, HOUSE_X0 - CORNER, HOUSE_X1 + CORNER, 1),
                  .openings = {DOORWAY(FRONT_DOOR_X0, FRONT_DOOR_X1, DOOR_HEAD),
                               WIN(KITCHEN_WIN_X0, KITCHEN_WIN_X1, KITCHEN_WIN_SILL,
                                   KITCHEN_WIN_HEAD),
                               WIN(-3.85f, -2.75f, GROUND_SILL, GROUND_HEAD),
                               WIN(-3.8f, -2.9f, UP_SILL, UP_HEAD),
                               WIN(-1.2f, -0.4f, UP_SILL, UP_HEAD),
                               WIN(2.2f, 3.1f, UP_SILL, UP_HEAD)},
                  .opening_count = 6},
    // The back: the window at the hall's end, the stair's and the bedroom's, and two upstairs.
    [HW_BACK] = {OUTSIDE(true, HOUSE_BACK_Z, HOUSE_X0 - CORNER, HOUSE_X1 + CORNER, -1),
                 .openings = {WIN(HALL_WIN_X0, HALL_WIN_X1, FLOOR_Y + 0.9f, FLOOR_Y + 2.1f),
                              WIN(STAIR_WIN_X0, STAIR_WIN_X1, STAIR_WIN_SILL, STAIR_WIN_HEAD),
                              WIN(2.0f, 3.0f, GROUND_SILL, GROUND_HEAD),
                              WIN(-3.8f, -2.9f, UP_SILL, UP_HEAD),
                              WIN(2.0f, 2.9f, UP_SILL, UP_HEAD)},
                 .opening_count = 5},
    // The west side: the living room's two, and two upstairs; the chimney between them.
    [HW_WEST] = {OUTSIDE(false, HOUSE_X0, HOUSE_FRONT_Z, HOUSE_BACK_Z, 1),
                 .openings = {WIN(11.0f, 12.1f, GROUND_SILL, GROUND_HEAD),
                              WIN(14.1f, 15.2f, GROUND_SILL, GROUND_HEAD),
                              WIN(11.2f, 12.1f, UP_SILL, UP_HEAD),
                              WIN(15.2f, 16.1f, UP_SILL, UP_HEAD)},
                 .opening_count = 4},
    // The east side: the laundry's small one, the bedroom's, and two upstairs. The kitchen's
    // shelves are against it.
    [HW_EAST] = {OUTSIDE(false, HOUSE_X1, HOUSE_FRONT_Z, HOUSE_BACK_Z, -1),
                 .openings = {WIN(14.9f, 15.5f, FLOOR_Y + 1.3f, FLOOR_Y + 1.9f),
                              WIN(17.5f, 18.5f, GROUND_SILL, GROUND_HEAD),
                              WIN(11.2f, 12.1f, UP_SILL, UP_HEAD),
                              WIN(15.2f, 16.1f, UP_SILL, UP_HEAD)},
                 .opening_count = 4},
    [HW_HALL_E_KITCHEN] = {INSIDE(false, HALL_X1, HOUSE_FRONT_Z, KITCHEN_BACK_Z, 1, MAT_PLASTER,
                                  MAT_CREAM),
                           .openings = {DOORWAY(KITCHEN_DOOR_Z0, KITCHEN_DOOR_Z1, DOOR_HEAD)},
                           .opening_count = 1},
    [HW_HALL_E_BATH] = {INSIDE(false, HALL_X1, KITCHEN_BACK_Z, HOME_SPLIT_Z, 1, MAT_BACKSPLASH,
                               MAT_CREAM),
                        .openings = {DOORWAY(BATH_DOOR_Z0, BATH_DOOR_Z1, DOOR_HEAD)},
                        .opening_count = 1},
    [HW_HALL_E_BEDROOM] = {INSIDE(false, HALL_X1, HOME_SPLIT_Z, HOUSE_BACK_Z, 1, MAT_PLASTER,
                                  MAT_CREAM),
                           .openings = {DOORWAY(17.6f, 18.4f, DOOR_HEAD)}, .opening_count = 1},
    [HW_HALL_W_LIVING] = {INSIDE(false, HALL_X0, HOUSE_FRONT_Z, HOME_SPLIT_Z, 1, MAT_CREAM,
                                 MAT_WALLPAPER),
                          .openings = {DOORWAY(LIVING_DOOR_Z0, LIVING_DOOR_Z1, DOOR_HEAD)},
                          .opening_count = 1},
    [HW_HALL_W_STAIR] = {INSIDE(false, HALL_X0, HOME_SPLIT_Z, HOUSE_BACK_Z, 1, MAT_CREAM,
                                MAT_PLASTER),
                         .openings = {DOORWAY(16.85f, 17.65f, DOOR_HEAD),
                                      DOORWAY(BASEMENT_DOOR_Z0, BASEMENT_DOOR_Z1, DOOR_HEAD)},
                         .opening_count = 2},
    [HW_CASED] = {INSIDE(true, KITCHEN_BACK_Z, HALL_X0, HALL_X1, -1, MAT_CREAM, MAT_CREAM),
                  .openings = {DOORWAY(CASED_X0, CASED_X1, CASED_HEAD)}, .opening_count = 1},
    [HW_KITCHEN_BACK] = {INSIDE(true, KITCHEN_BACK_Z, HALL_X1, HOUSE_X1, -1, MAT_PLASTER,
                                MAT_BACKSPLASH)},
    [HW_BATH_EAST] = {INSIDE(false, BATH_X1, KITCHEN_BACK_Z, HOME_SPLIT_Z, -1, MAT_BACKSPLASH,
                             MAT_PLASTER)},
    [HW_SPLIT_EAST] = {INSIDE(true, HOME_SPLIT_Z, HALL_X1, HOUSE_X1, -1, MAT_BACKSPLASH,
                              MAT_PLASTER)},
    [HW_SPLIT_WEST] = {INSIDE(true, HOME_SPLIT_Z, HOUSE_X0, HALL_X0, -1, MAT_WALLPAPER,
                              MAT_PLASTER)},
    [HW_STAIRWELL] = {INSIDE(true, STAIRWELL_WALL_Z, HOUSE_X0, HALL_X0, 1, MAT_PLASTER,
                             MAT_PLASTER)},
};

#define NO_PANE (-1)

// What glazes each outside wall's openings: the rooms anybody sees into take window glass, the
// shut ones and every room upstairs dark glass, and one upstairs window is lit at night.
static const struct {
    HomeWall wall;
    int glass[KIT_MAX_OPENINGS];
} GLAZING[] = {
    {HW_FRONT,
     {NO_PANE, MAT_WINDOW_GLASS, MAT_WINDOW_GLASS, MAT_DARK_GLASS, MAT_DARK_GLASS, MAT_WINDOW_LIT}},
    {HW_BACK, {MAT_WINDOW_GLASS, MAT_DARK_GLASS, MAT_DARK_GLASS, MAT_DARK_GLASS, MAT_DARK_GLASS}},
    {HW_WEST, {MAT_WINDOW_GLASS, MAT_WINDOW_GLASS, MAT_DARK_GLASS, MAT_DARK_GLASS}},
    {HW_EAST, {MAT_DARK_GLASS, MAT_DARK_GLASS, MAT_DARK_GLASS, MAT_DARK_GLASS}},
};

static float roof_y(float x) {
    return EAVE_Y + (HOUSE_X1 + CORNER - fabsf(x)) * HOME_PITCH;
}

// ---------------------------------------------------------------------------------------------
// The shell

/*
 * A double-hung window in opening `i` of outside wall `w`: its glass at the wall's middle, the
 * two sashes round it with their meeting rail and a bar down each, a white casing and a sill
 * outside, and shutters either side of one wide enough to have had them.
 */
static void window(Kit* kit, const KitWall* w, int i, int glass) {
    const KitWallFrame wf = kit_wall_frame(w);
    const KitOpening* o = &w->openings[i];
    kit_frame_pane(kit, &wf.f, glass, o, wf.at, w->thick);
    const float at = wf.at, t = 0.022f, s = 0.045f;
    const float mid = 0.5f * (o->bottom + o->top), centre = 0.5f * (o->from + o->to);
    const KitFrame* f = &wf.f;
    kit_frame_box(kit, f, MAT_MOULDING, o->from, o->from + s, o->bottom, o->top, at - t, at + t,
                  false);
    kit_frame_box(kit, f, MAT_MOULDING, o->to - s, o->to, o->bottom, o->top, at - t, at + t, false);
    kit_frame_box(kit, f, MAT_MOULDING, o->from, o->to, o->bottom, o->bottom + s + 0.02f, at - t,
                  at + t, false);
    kit_frame_box(kit, f, MAT_MOULDING, o->from, o->to, o->top - s, o->top, at - t, at + t, false);
    kit_frame_box(kit, f, MAT_MOULDING, o->from, o->to, mid - 0.025f, mid + 0.025f, at - t, at + t,
                  false);
    kit_frame_box(kit, f, MAT_MOULDING, centre - 0.012f, centre + 0.012f, o->bottom, o->top,
                  at - 0.01f, at + 0.01f, false);

    const Facade out = facade_of(w);
    ornament_casing(kit, &out, MAT_TRIM, o);
    kit_frame_box(kit, &out.f, MAT_TRIM, o->from - 0.1f, o->to + 0.1f, o->bottom - 0.05f, o->bottom,
                  out.face, out.face + out.out * 0.07f, false);
    const float width = o->to - o->from;
    if (width < 0.8f)
        return;
    const float sw = fminf(0.5f * width, 0.45f), gap = ORNAMENT_CASING_W + 0.02f;
    for (int side = -1; side <= 1; side += 2) {
        const float a0 = side < 0 ? o->from - gap - sw : o->to + gap;
        const float a1 = a0 + sw;
        kit_frame_box(kit, &out.f, MAT_SIDING_B, a0, a1, o->bottom, o->top,
                      out.face + out.out * 0.01f, out.face + out.out * 0.035f, false);
        // Louvres: a row of slats proud of the board across its middle.
        for (float y = o->bottom + 0.12f; y < o->top - 0.1f; y += 0.07f)
            kit_frame_box(kit, &out.f, MAT_SIDING_B, a0 + 0.04f, a1 - 0.04f, y, y + 0.025f,
                          out.face + out.out * 0.035f, out.face + out.out * 0.045f, false);
    }
}

// A shut door filling opening `i` of wall `w`: the panelled leaf, the same either side, in the
// house's kit since it never moves, and a body through the wall.
static void door_shut(Kit* kit, const KitWall* w, int i) {
    const KitWallFrame wf = kit_wall_frame(w);
    const KitOpening* o = &w->openings[i];
    KitFrame leaf = {{0.0f, 0.0f, 0.0f}, wf.f.yaw};
    kit_frame_point(&wf.f, 0.0f, 0.0f, wf.at, leaf.origin);
    KitOpening shape = kit_opening_grow(o, -DOOR_CLEARANCE);
    shape.bottom = FLOOR_Y + 0.01f;
    door_leaf_panelled(kit, &leaf, &shape, DOOR_THICK);
    kit_frame_plug(kit, &wf.f, o, wf.at, w->thick);
}

static void walls(Kit* kit) {
    for (int i = 0; i < HW_COUNT; i++)
        kit_wall(kit, &WALLS[i]);
    for (size_t k = 0; k < sizeof(GLAZING) / sizeof(GLAZING[0]); k++) {
        const KitWall* w = &WALLS[GLAZING[k].wall];
        for (int i = 0; i < w->opening_count; i++)
            if (GLAZING[k].glass[i] != NO_PANE)
                window(kit, w, i, GLAZING[k].glass[i]);
    }
    door_shut(kit, &WALLS[HW_HALL_E_BEDROOM], O_BEDROOM_DOOR);
    door_shut(kit, &WALLS[HW_HALL_W_STAIR], O_STAIR_DOOR);
    // The front door's threshold, under the leaf, and the basement door's (spec 13.31).
    const KitOpening* door = &WALLS[HW_FRONT].openings[O_FRONT_DOOR];
    kit_frame_box(kit, &KIT_WORLD, MAT_MOULDING, door->from, door->to, FLOOR_Y - 0.02f,
                  FLOOR_Y + 0.012f, HOUSE_FRONT_Z - CORNER - 0.04f, HOUSE_FRONT_Z + CORNER, false);
    const KitOpening* cellar = &WALLS[HW_HALL_W_STAIR].openings[O_BASEMENT_DOOR];
    kit_frame_box(kit, &KIT_WORLD, MAT_MOULDING, HALL_OUT_X0 - 0.01f,
                  HALL_X0 + 0.5f * INT_WALL + 0.01f, FLOOR_Y - 0.02f, FLOOR_Y + 0.012f,
                  cellar->from, cellar->to, false);
}

/*
 * The trim round the outside: a course of brick at the foot, white corner boards, a belt at the
 * upper floor's line, and a frieze board under each side eave.
 */
static void outside_trim(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    const float x0 = HOUSE_X0 - CORNER, x1 = HOUSE_X1 + CORNER;
    const float z0 = HOUSE_FRONT_Z - CORNER, z1 = HOUSE_BACK_Z + CORNER;
    const struct {
        int mat;
        float y0, y1, proud;
    } BANDS[] = {{MAT_BRICK, 0.0f, 0.28f, 0.025f}, {MAT_TRIM, CEIL_Y, CEIL_Y + 0.18f, 0.03f}};
    for (size_t b = 0; b < sizeof(BANDS) / sizeof(BANDS[0]); b++) {
        const float p = BANDS[b].proud, y0 = BANDS[b].y0, y1 = BANDS[b].y1;
        const int m = BANDS[b].mat;
        kit_frame_box(kit, w, m, x0 - p, x1 + p, y0, y1, z0 - p, z0, false);
        kit_frame_box(kit, w, m, x0 - p, x1 + p, y0, y1, z1, z1 + p, false);
        kit_frame_box(kit, w, m, x0 - p, x0, y0, y1, z0, z1, false);
        kit_frame_box(kit, w, m, x1, x1 + p, y0, y1, z0, z1, false);
    }
    const float c = 0.12f, p = 0.02f;
    const float xs[2] = {x0, x1}, zs[2] = {z0, z1};
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 2; k++) {
            const float sx = i ? 1.0f : -1.0f, sz = k ? 1.0f : -1.0f;
            kit_frame_box(kit, w, MAT_TRIM, xs[i] - (sx > 0 ? c : 0.0f) - (sx < 0 ? p : 0.0f),
                          xs[i] + (sx < 0 ? c : 0.0f) + (sx > 0 ? p : 0.0f), 0.28f, EAVE_Y,
                          zs[k] + (sz > 0 ? 0.0f : -p), zs[k] + (sz > 0 ? p : 0.0f), false);
            kit_frame_box(kit, w, MAT_TRIM, xs[i] + (sx > 0 ? 0.0f : -p),
                          xs[i] + (sx > 0 ? p : 0.0f), 0.28f, EAVE_Y, zs[k] - (sz > 0 ? c : 0.0f),
                          zs[k] + (sz < 0 ? c : 0.0f), false);
        }
    for (int s = -1; s <= 1; s += 2) {
        const float x = s < 0 ? x0 : x1;
        kit_frame_box(kit, w, MAT_TRIM, x, x + (float)s * 0.03f, EAVE_Y - 0.25f, EAVE_Y, z0, z1,
                      false);
    }
}

/*
 * Floors and ceilings: each room's boards on the ground, the ceiling over the whole ground floor
 * and the boards over that, and the upper ceiling. Nothing upstairs is furnished; its windows
 * are dark.
 *
 * A room's boards are only the finished layer: the joists and the subfloor under them are the
 * basement's ceiling (basement.c). Each room's body still fills the floor's whole depth, and the
 * stair room's stops at the stairwell, but for the strip of floor inside the basement door
 * before its flight starts.
 */
static void floors(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    const struct {
        int mat;
        float x0, x1, z0, z1;
    } FLOORS[] = {
        {MAT_HARDWOOD, HALL_X0, HALL_X1, HOUSE_FRONT_Z, HOUSE_BACK_Z},
        {MAT_WOOD_FLOOR, HOUSE_X0, HALL_X0, HOUSE_FRONT_Z, STAIRWELL_Z0},
        {MAT_WOOD_FLOOR, CELLAR_HEAD_X, HALL_X0, STAIRWELL_Z0, HOUSE_BACK_Z},
        {MAT_KITCHEN_FLOOR, HALL_X1, HOUSE_X1, HOUSE_FRONT_Z, KITCHEN_BACK_Z},
        {MAT_BACKSPLASH, HALL_X1, BATH_X1, KITCHEN_BACK_Z, HOME_SPLIT_Z},
        {MAT_WOOD_FLOOR, BATH_X1, HOUSE_X1, KITCHEN_BACK_Z, HOME_SPLIT_Z},
        {MAT_WOOD_FLOOR, HALL_X1, HOUSE_X1, HOME_SPLIT_Z, HOUSE_BACK_Z},
    };
    for (size_t i = 0; i < sizeof(FLOORS) / sizeof(FLOORS[0]); i++) {
        kit_frame_box(kit, w, FLOORS[i].mat, FLOORS[i].x0, FLOORS[i].x1, FLOOR_Y - FLOOR_BOARDS,
                      FLOOR_Y, FLOORS[i].z0, FLOORS[i].z1, false);
        kit_frame_box(kit, w, KIT_COLLIDER_ONLY, FLOORS[i].x0, FLOORS[i].x1, 0.0f, FLOOR_Y,
                      FLOORS[i].z0, FLOORS[i].z1, true);
    }
    kit_frame_box(kit, w, MAT_CEILING, HOUSE_X0, HOUSE_X1, CEIL_Y, CEIL_Y + SLAB, HOUSE_FRONT_Z,
                  HOUSE_BACK_Z, false);
    kit_frame_box(kit, w, MAT_WOOD_FLOOR, HOUSE_X0, HOUSE_X1, CEIL_Y + SLAB, FLOOR2_Y,
                  HOUSE_FRONT_Z, HOUSE_BACK_Z, true);
    kit_frame_box(kit, w, MAT_CEILING, HOUSE_X0, HOUSE_X1, CEIL2_Y, CEIL2_Y + SLAB, HOUSE_FRONT_Z,
                  HOUSE_BACK_Z, false);
}

// A roof slab under the plane y = y0 + gx x + gz z over the outline `xz` in plan: shingle on top
// and a roof's thickness under it the soffit, its edges the fascia.
static void roof_slab(Kit* kit, const vec2* xz, int count, float y0, float gx, float gz,
                      float thick) {
    vec3 top[KIT_MAX_OUTLINE] = {{0.0f}};
    for (int i = 0; i < count && i < KIT_MAX_OUTLINE; i++)
        glm_vec3_copy((vec3){xz[i][0], y0 + gx * xz[i][0] + gz * xz[i][1], xz[i][1]}, top[i]);
    kit_extrude(kit, MAT_ROOF, MAT_TRIM, MAT_TRIM, top, count, (vec3){0.0f, -thick, 0.0f});
}

/*
 * The roof: one ridge from front to back, so the street sees a gable as it does on the Gothic
 * house, at a plain house's pitch; the gables closed under it in the boards, the front one with
 * an attic vent; the side eaves dripping.
 */
static void roof(Kit* kit) {
    const float x_eave = HOUSE_X1 + CORNER + EAVE_OVERHANG;
    const float z0 = HOUSE_FRONT_Z - CORNER - RAKE_OVERHANG;
    const float z1 = HOUSE_BACK_Z + CORNER + RAKE_OVERHANG;
    const float ridge = roof_y(0.0f);
    const vec2 west[4] = {{-x_eave, z0}, {0.0f, z0}, {0.0f, z1}, {-x_eave, z1}};
    roof_slab(kit, west, 4, ridge, HOME_PITCH, 0.0f, ROOF_THICK);
    const vec2 east[4] = {{0.0f, z0}, {x_eave, z0}, {x_eave, z1}, {0.0f, z1}};
    roof_slab(kit, east, 4, ridge, -HOME_PITCH, 0.0f, ROOF_THICK);

    // Each gable's apex a roof's thickness down, inside the slab, so no face of it lies in the
    // roof's plane.
    const float side = HOUSE_X1 + CORNER;
    const vec2 gable[3] = {{-side, EAVE_Y}, {side, EAVE_Y}, {0.0f, ridge - ROOF_THICK}};
    const float gz[2] = {HOUSE_FRONT_Z, HOUSE_BACK_Z};
    for (int g = 0; g < 2; g++)
        kit_frame_extrude(kit, &KIT_WORLD, MAT_SIDING, gable, 3, gz[g] - CORNER, gz[g] + CORNER);
    const float vy = ridge - 1.6f, vz = HOUSE_FRONT_Z - CORNER;
    kit_frame_box(kit, &KIT_WORLD, MAT_TRIM, -0.4f, 0.4f, vy - 0.3f, vy + 0.3f, vz - 0.03f, vz,
                  false);
    for (int i = 0; i < 6; i++) {
        const float y = vy - 0.22f + 0.085f * (float)i;
        kit_frame_box(kit, &KIT_WORLD, MAT_BLACK, -0.32f, 0.32f, y, y + 0.03f, vz - 0.045f,
                      vz - 0.03f, false);
    }

    const float drip_y = roof_y(x_eave) - ROOF_THICK;
    kit_drip_run(kit, &KIT_WORLD, (vec3){-x_eave, drip_y, z0}, (vec3){-x_eave, drip_y, z1},
                 EAVE_DRIPS_PER_M, 0.0f);
    kit_drip_run(kit, &KIT_WORLD, (vec3){x_eave, drip_y, z0}, (vec3){x_eave, drip_y, z1},
                 EAVE_DRIPS_PER_M, 0.0f);
}

// A brick chimney up the west wall between the living room's windows, past the eave.
static void chimney(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    const float x1 = HOUSE_X0 - CORNER, x0 = x1 - 0.62f, z0 = 12.6f, z1 = 13.6f;
    const float top = roof_y(-3.0f) + 0.6f, shoulder = 2.2f;
    kit_frame_box(kit, w, MAT_BRICK, x0 - 0.15f, x1, 0.0f, shoulder, z0 - 0.15f, z1 + 0.15f, true);
    kit_frame_box(kit, w, MAT_BRICK, x0, x1, shoulder, top, z0, z1, true);
    kit_frame_box(kit, w, MAT_CONCRETE, x0 - 0.06f, x1 + 0.06f, top, top + 0.1f, z0 - 0.06f,
                  z1 + 0.06f, false);
    kit_frame_box(kit, w, MAT_BLACK, x0 + 0.12f, x1 - 0.12f, top + 0.1f, top + 0.22f, z0 + 0.25f,
                  z1 - 0.25f, false);
}

// A railing from a0 to a1 along frame `f` at d: a top and a bottom rail, square balusters
// between, and its body.
static void railing(Kit* kit, const KitFrame* f, float a0, float a1, float d) {
    const float top = FLOOR_Y + 0.85f, foot = FLOOR_Y + 0.06f;
    kit_frame_box(kit, f, MAT_TRIM, a0, a1, top - 0.05f, top, d - 0.035f, d + 0.035f, false);
    kit_frame_box(kit, f, MAT_TRIM, a0, a1, foot, foot + 0.04f, d - 0.025f, d + 0.025f, false);
    for (float a = a0 + 0.08f; a < a1 - 0.04f; a += 0.11f)
        kit_frame_box(kit, f, MAT_TRIM, a - 0.016f, a + 0.016f, foot + 0.04f, top - 0.05f,
                      d - 0.016f, d + 0.016f, false);
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, a0, a1, FLOOR_Y, top, d - 0.04f, d + 0.04f, true);
}

/*
 * The porch: boards on the floor's level, two concrete steps up to it, square posts holding a
 * shingled roof from the upper floor's line, and a railing either side of the steps. The roof
 * runs on past the porch as a strip over the kitchen window carrying the gutter, which leaks
 * past it, as the Gothic house's does.
 */
static void porch(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    kit_frame_box(kit, w, MAT_PORCH, PORCH_X0, PORCH_X1, 0.0f, FLOOR_Y, PORCH_Z0, HOUSE_FRONT_Z,
                  true);
    kit_frame_box(kit, w, MAT_CONCRETE, PATH_X0 - 0.15f, PATH_X1 + 0.15f, 0.0f, 0.2f,
                  PORCH_Z0 - 0.3f, PORCH_Z0, true);
    kit_frame_box(kit, w, MAT_CONCRETE, PATH_X0 - 0.15f, PATH_X1 + 0.15f, 0.0f, 0.1f,
                  PORCH_Z0 - 0.6f, PORCH_Z0 - 0.3f, true);

    const float wall = HOUSE_FRONT_Z - CORNER;
    const float y0 = PENT_Y - PENT_PITCH * wall; // the plane's height at z = 0
    const float porch_edge = PORCH_Z0 - 0.1f, strip_edge = wall - PENT_DEPTH;
    const float x_start = PORCH_X0 - 0.15f, x_end = HOUSE_X1 + CORNER + 0.3f;
    const vec2 over[4] = {
        {x_start, porch_edge}, {PORCH_ROOF_X1, porch_edge}, {PORCH_ROOF_X1, wall}, {x_start, wall}};
    roof_slab(kit, over, 4, y0, 0.0f, PENT_PITCH, PENT_THICK);
    const vec2 strip[4] = {
        {PORCH_ROOF_X1, strip_edge}, {x_end, strip_edge}, {x_end, wall}, {PORCH_ROOF_X1, wall}};
    roof_slab(kit, strip, 4, y0, 0.0f, PENT_PITCH, PENT_THICK);

    const float post_z = PORCH_Z0 + 0.1f, under = y0 + PENT_PITCH * post_z - PENT_THICK;
    const float posts[2] = {PORCH_X0 + 0.1f, PORCH_X1 - 0.1f};
    for (int i = 0; i < 2; i++)
        kit_frame_box(kit, w, MAT_TRIM, posts[i] - 0.055f, posts[i] + 0.055f, FLOOR_Y, under,
                      post_z - 0.055f, post_z + 0.055f, true);
    kit_frame_box(kit, w, MAT_TRIM, x_start, PORCH_ROOF_X1, under - 0.16f, under, post_z - 0.05f,
                  post_z + 0.05f, false);
    railing(kit, w, posts[0] + 0.06f, PATH_X0 - 0.15f, post_z);
    railing(kit, w, PATH_X1 + 0.15f, posts[1] - 0.06f, post_z);
    for (int i = 0; i < 2; i++)
        railing(kit, &KIT_WORLD_Z, post_z + 0.06f, wall - 0.02f, -posts[i]);

    kit_drip_run(
        kit, w, (vec3){x_start + 0.1f, y0 + PENT_PITCH * porch_edge - PENT_THICK, porch_edge},
        (vec3){PORCH_ROOF_X1 - 0.05f, y0 + PENT_PITCH * porch_edge - PENT_THICK, porch_edge},
        PORCH_DRIPS_PER_M, 0.0f);
    house_front_gutter(kit, PORCH_ROOF_X1 + 0.05f, x_end - 0.05f,
                       y0 + PENT_PITCH * strip_edge - PENT_THICK, strip_edge);
}

// ---------------------------------------------------------------------------------------------
// The rooms' finishes

// One side of a room: a wall, the stretch of it the room has, and the finish laid over its
// plaster, or -1 where the wall's own face is the finish.
typedef struct RoomSide {
    HomeWall wall;
    float a0, a1;
    int lining;
} RoomSide;

// A finish of `mat` laid over a wall's face toward (x, z), from a0 to a1 along it and y0 to y1,
// cut round the wall's openings and `extra` when there is one. The facade it returns stands at
// the finish's face.
static Facade line_wall(Kit* kit, const KitWall* w, int mat, float a0, float a1, float y0, float y1,
                        float x, float z, const KitOpening* extra) {
    Facade s = facade_toward(w, x, z);
    KitWall l = *w;
    l.at = s.face + s.out * 0.5f * LINING;
    l.from = a0;
    l.to = a1;
    l.y0 = y0;
    l.y1 = y1;
    l.thick = LINING;
    if (extra && l.opening_count < KIT_MAX_OPENINGS)
        l.openings[l.opening_count++] = *extra;
    kit_frame_panel(kit, &s.f, mat, &l);
    s.face += s.out * LINING;
    return s;
}

/*
 * A room's walls dressed, seen from (x, z): each side's lining, a skirting stopping at every
 * doorway, a crown moulding at the ceiling when `crown`, and a white casing round every opening
 * in its stretch, with a stool under each window.
 */
static void dress(Kit* kit, const RoomSide* sides, int count, float x, float z, bool crown) {
    for (int k = 0; k < count; k++) {
        const RoomSide* side = &sides[k];
        const KitWall* w = &WALLS[side->wall];
        Facade s = side->lining >= 0 ? line_wall(kit, w, side->lining, side->a0, side->a1, FLOOR_Y,
                                                 CEIL_Y, x, z, NULL)
                                     : facade_toward(w, x, z);
        vec2 blocked[KIT_MAX_OPENINGS] = {{0.0f}}, spans[KIT_MAX_OPENINGS + 1];
        int nb = 0;
        for (int i = 0; i < w->opening_count; i++) {
            const KitOpening* o = &w->openings[i];
            if (o->to < side->a0 || o->from > side->a1 || o->bottom > CEIL_Y)
                continue;
            ornament_casing(kit, &s, MAT_MOULDING, o);
            if (o->door)
                glm_vec2_copy((vec2){o->from - ORNAMENT_CASING_W, o->to + ORNAMENT_CASING_W},
                              blocked[nb++]);
            else
                kit_frame_box(kit, &s.f, MAT_MOULDING, o->from - ORNAMENT_CASING_W - 0.03f,
                              o->to + ORNAMENT_CASING_W + 0.03f, o->bottom - 0.03f, o->bottom,
                              s.face, s.face + s.out * 0.07f, false);
        }
        const int n = kit_clear_spans(side->a0, side->a1, blocked, nb, spans);
        for (int i = 0; i < n; i++)
            facade_box(kit, &s, MAT_MOULDING, spans[i][0], spans[i][1], FLOOR_Y, FLOOR_Y + SKIRT_H,
                       SKIRT_PROUD);
        if (crown) {
            const float f = s.face, o = s.out;
            const vec2 cove[] = {{f, CEIL_Y - 0.12f},
                                 {f + o * 0.012f, CEIL_Y - 0.115f},
                                 {f + o * 0.02f, CEIL_Y - 0.09f},
                                 {f + o * 0.045f, CEIL_Y - 0.05f},
                                 {f + o * 0.08f, CEIL_Y - 0.025f},
                                 {f + o * 0.095f, CEIL_Y - 0.012f},
                                 {f + o * 0.1f, CEIL_Y},
                                 {f, CEIL_Y}};
            kit_frame_run(kit, &s.f, MAT_MOULDING, cove, KIT_COUNT(cove), side->a0, side->a1);
        }
    }
}

// Points in each room, which sides of its walls are its own.
#define HALL_MID_X  (0.5f * (HALL_X0 + HALL_X1))
#define HALL_FRONT  12.0f
#define HALL_BACK   16.0f
#define LIVING_AT_X (0.5f * (LIVING_IN_X0 + LIVING_IN_X1))
#define LIVING_AT_Z (0.5f * (LIVING_IN_Z0 + LIVING_IN_Z1))
#define BATH_AT_X   (0.5f * (BATH_IN_X0 + BATH_IN_X1))
#define BATH_AT_Z   (0.5f * (BATH_IN_Z0 + BATH_IN_Z1))

static void finishes(Kit* kit) {
    const float split = KITCHEN_BACK_Z - 0.5f * INT_WALL, past = KITCHEN_BACK_Z + 0.5f * INT_WALL;
    const RoomSide front_half[] = {
        {HW_FRONT, HALL_IN_X0, HALL_IN_X1, MAT_CREAM},
        {HW_HALL_E_KITCHEN, HALL_IN_Z0, split, -1},
        {HW_HALL_W_LIVING, HALL_IN_Z0, split, -1},
        {HW_CASED, HALL_IN_X0, HALL_IN_X1, -1},
    };
    dress(kit, front_half, KIT_COUNT(front_half), HALL_MID_X, HALL_FRONT, true);
    const RoomSide back_half[] = {
        {HW_CASED, HALL_IN_X0, HALL_IN_X1, -1},
        {HW_HALL_E_BATH, past, HOME_SPLIT_Z, -1},
        {HW_HALL_E_BEDROOM, HOME_SPLIT_Z, HALL_IN_Z1, -1},
        {HW_HALL_W_LIVING, past, HOME_SPLIT_Z, -1},
        {HW_HALL_W_STAIR, HOME_SPLIT_Z, HALL_IN_Z1, -1},
        {HW_BACK, HALL_IN_X0, HALL_IN_X1, MAT_CREAM},
    };
    dress(kit, back_half, KIT_COUNT(back_half), HALL_MID_X, HALL_BACK, true);
    const RoomSide living[] = {
        {HW_FRONT, LIVING_IN_X0, LIVING_IN_X1, MAT_WALLPAPER},
        {HW_WEST, LIVING_IN_Z0, LIVING_IN_Z1, MAT_WALLPAPER},
        {HW_HALL_W_LIVING, LIVING_IN_Z0, LIVING_IN_Z1, -1},
        {HW_SPLIT_WEST, LIVING_IN_X0, LIVING_IN_X1, -1},
    };
    dress(kit, living, KIT_COUNT(living), LIVING_AT_X, LIVING_AT_Z, true);
    // The bathroom's door is cased on its side too; its tiles want no skirting.
    const Facade bath = facade_toward(&WALLS[HW_HALL_E_BATH], BATH_AT_X, BATH_AT_Z);
    ornament_casing(kit, &bath, MAT_MOULDING, &WALLS[HW_HALL_E_BATH].openings[O_BATH_DOOR]);
    // And the kitchen's doorway on the kitchen's side, as the Gothic house cased it.
    const Facade kitchen = facade_toward(&WALLS[HW_HALL_E_KITCHEN], 2.0f, 12.0f);
    ornament_casing(kit, &kitchen, MAT_MOULDING,
                    &WALLS[HW_HALL_E_KITCHEN].openings[O_KITCHEN_DOOR]);
}

void home_line_stairwell(Kit* kit, int mat, const KitOpening* foot) {
    const float x = 0.5f * (CELLAR_X0 + CELLAR_HEAD_X), z = 0.5f * (STAIRWELL_Z0 + CELLAR_Z1);
    const float hall = HALL_OUT_X0;
    line_wall(kit, &WALLS[HW_BACK], mat, CELLAR_X0, hall, BASEMENT_Y, CEIL_Y, x, z, NULL);
    line_wall(kit, &WALLS[HW_STAIRWELL], mat, CELLAR_X0, hall, BASEMENT_Y, CEIL_Y, x, z, foot);
    line_wall(kit, &WALLS[HW_WEST], mat, STAIRWELL_Z0, CELLAR_Z1, BASEMENT_Y, CEIL_Y, x, z, NULL);
    // The hall's wall only from the floor up: under it the stairwell's end is the basement's.
    line_wall(kit, &WALLS[HW_HALL_W_STAIR], mat, STAIRWELL_Z0, CELLAR_Z1, FLOOR_Y, CEIL_Y, x, z,
              NULL);
}

// ---------------------------------------------------------------------------------------------
// The hall

// A picture from cards.h hung on a wall's face at a along it, its middle at height y: a frame's
// depth of backing, and the picture, frame and all, on it.
static void hang(Kit* kit, const Facade* s, CardId id, float a, float y) {
    const float w = CARDS[id].size[0], h = CARDS[id].size[1];
    const float a0 = a - 0.5f * w, a1 = a + 0.5f * w, y0 = y - 0.5f * h, y1 = y + 0.5f * h;
    facade_box(kit, s, MAT_BLACK, a0, a1, y0, y1, 0.022f);
    kit_frame_card_rect(kit, &s->f, MAT_CARDS, CARDS[id].uv, a0, a1, y0, y1,
                        s->face + s->out * 0.023f, s->out);
}

/*
 * A lantern on a wall: a backplate and an arm out to a little iron cage of four frosted panes
 * under a pyramid cap, a bulb's light inside it. Its shadow is cached, softened by the frosted
 * glass, which is what glows.
 */
#define LANTERN_CANDELA 22.0f
static void lantern(Kit* kit, Scene* scene, const Facade* s, float a, float y) {
    const KitFrame* f = &s->f;
    const float face = s->face, o = s->out, out = face + o * 0.2f;
    kit_frame_box(kit, f, MAT_IRON, a - 0.05f, a + 0.05f, y - 0.1f, y + 0.12f, face,
                  face + o * 0.012f, false);
    const vec3 arm[] = {
        {a, y, face + o * 0.012f}, {a, y + 0.02f, face + o * 0.14f}, {a, y + 0.1f, out}};
    kit_frame_pipe(kit, f, MAT_IRON, arm, KIT_COUNT(arm), 0.008f, 6);
    const float h = 0.2f, r = 0.065f, y0 = y - 0.06f;
    for (int i = 0; i < 4; i++) {
        const float da = (i & 1) ? r : -r, dd = (i & 2) ? r : -r;
        kit_frame_box(kit, f, MAT_IRON, a + da - 0.006f, a + da + 0.006f, y0, y0 + h,
                      out + o * dd - 0.006f, out + o * dd + 0.006f, false);
    }
    kit_frame_box(kit, f, MAT_FROSTED, a - r, a + r, y0 + 0.01f, y0 + h - 0.01f, out - r * o,
                  out + r * o, false);
    kit_frame_box(kit, f, MAT_IRON, a - r - 0.01f, a + r + 0.01f, y0 - 0.02f, y0,
                  out - (r + 0.01f) * o, out + (r + 0.01f) * o, false);
    const vec2 cap[] = {{0.0f, 0.0f}, {0.1f, 0.0f}, {0.0f, 0.08f}};
    kit_frame_lathe(kit, f, MAT_IRON, a, out, y0 + h, cap, KIT_COUNT(cap), 4);
    vec3 at = {0.0f, 0.0f, 0.0f};
    kit_frame_point(f, a, y0 + 0.5f * h, out, at);
    glm_vec3_add(at, kit->origin, at);
    LightDesc desc = {.name = "hall_lantern",
                      .type = LIGHT_POINT,
                      .position = {at[0], at[1], at[2]},
                      .color = {1.0f, 0.74f, 0.46f},
                      .intensity = LANTERN_CANDELA,
                      .range = 7.0f,
                      .cast_shadows = true,
                      .shadow_cache = true,
                      .source_radius = r,
                      .shadow_near = r + 0.02f};
    scene_add_light(scene, create_light(&desc));
}

// The plant's leaves and stem, grown once with the engine's tree generator at a houseplant's
// size, in a pot.
static void plant(Engine* engine, Scene* scene, const vec3 at) {
    ShaderProgram* pbr = engine_get_program(engine, CETRA_PROGRAM_PBR);
    enum { CELL = 128 };
    BakedMaps maps = {.width = CELL * TG_LEAF_VARIANTS, .height = CELL, .albedo_channels = 4};
    veg_leaf_cluster_maps(maps.width, maps.height, &maps.albedo, &maps.normal, &maps.rough);
    Material* leaf = create_material();
    leaf->name = safe_strdup("plant_leaf");
    glm_vec3_copy((vec3){0.42f, 0.55f, 0.3f}, leaf->albedo);
    // The atlas is rough for a sprig of leaves turned every way, seen from across a field; a
    // houseplant's are waxy and near, and this takes them back down to about 0.4.
    leaf->roughness = 0.45f;
    material_set_program(leaf, pbr);
    TextureDesc albedo = texture_desc(true);
    mats_cutout(leaf, 0.4f, &albedo);
    mats_set_baked(leaf, scene, "plant_leaf", &maps, albedo);
    scene_add_material(scene, leaf);
    Material* stem = create_material();
    stem->name = safe_strdup("plant_stem");
    glm_vec3_copy((vec3){0.22f, 0.26f, 0.12f}, stem->albedo);
    stem->roughness = 0.8f;
    material_set_program(stem, pbr);
    scene_add_material(scene, stem);

    TreeParams tp;
    memset(&tp, 0, sizeof(tp));
    tp.seed = 311;
    tp.recursive.max_depth = 3;
    tp.trunk_length = 40.0f;
    tp.trunk_radius = 2.2f;
    tp.recursive.branches_per_node = 3;
    tp.recursive.length_decay = 0.75f;
    tp.recursive.taper = 0.6f;
    tp.recursive.branch_angle = 48.0f;
    tp.angle_variance = 18.0f;
    tp.twist = 137.5f;
    tp.droop = 0.35f;
    tp.curve_noise = 0.3f;
    tp.phototropism = 0.4f;
    tp.recursive.lateral_density = 0.6f;
    tp.recursive.twig_scale = 1.0f;
    tp.show_leaves = 1;
    tp.leaf_size = 14.0f;
    tp.leaf_density = 2.5f;
    TreeSkeleton skel;
    memset(&skel, 0, sizeof(skel));
    tree_skeleton_build(&skel, &tp);
    SceneNode* node = create_node();
    node_set_name(node, "hall_plant");
    mat4 m;
    glm_translate_make(m, (float*)at);
    glm_scale_uni(m, 0.008f);
    glm_mat4_copy(m, node->original_transform);
    Mesh* bark = create_mesh();
    if (tree_mesh_bark(&skel, &tp, bark)) {
        bark->material = stem;
        node_add_mesh(node, bark);
    } else {
        free_mesh(bark);
    }
    Mesh* leaves = create_mesh();
    if (tree_mesh_leaves(&skel, &tp, leaves)) {
        leaves->material = leaf;
        node_add_mesh(node, leaves);
    } else {
        free_mesh(leaves);
    }
    tree_skeleton_free(&skel);
    node_add_child(scene->root_node, node);
}

/*
 * The console in the hall's back half, against the bathroom's wall past its door: a narrow
 * table with a drawer, and on it a wooden radio, the plant in its pot, an ashtray with a stub
 * in it, keys, and a photograph in a frame.
 */
static void console(Kit* kit, Engine* engine, Scene* scene) {
    const KitFrame f = {{HALL_X1 - 0.5f * INT_WALL, FLOOR_Y, 16.0f}, -0.5f * GLM_PIf};
    const float top = 0.8f, ha = 0.45f, depth = 0.3f;
    kit_frame_box(kit, &f, MAT_WOOD, -ha, ha, top - 0.035f, top, 0.0f, depth, false);
    kit_frame_box(kit, &f, MAT_WOOD, -ha + 0.03f, ha - 0.03f, top - 0.16f, top - 0.035f, 0.02f,
                  depth - 0.02f, false);
    kit_frame_box(kit, &f, MAT_WOOD, -0.2f, 0.2f, top - 0.14f, top - 0.05f, depth - 0.02f,
                  depth - 0.005f, false);
    kit_frame_prism(kit, &f, MAT_BRASS, 0.0f, depth + 0.005f, top - 0.1f, top - 0.09f, 0.012f, 8);
    for (int i = 0; i < 4; i++) {
        const float a = (i & 1) ? ha - 0.05f : -ha + 0.05f, d = (i & 2) ? depth - 0.05f : 0.05f;
        kit_frame_box(kit, &f, MAT_WOOD, a - 0.018f, a + 0.018f, 0.0f, top - 0.035f, d - 0.018f,
                      d + 0.018f, false);
    }
    kit_frame_box(kit, &f, KIT_COLLIDER_ONLY, -ha, ha, 0.0f, top, 0.0f, depth, true);

    // The radio: a wooden case, a cloth grille, a dial and two knobs.
    kit_frame_box(kit, &f, MAT_WOOD, -0.38f, -0.08f, top, top + 0.19f, 0.04f, 0.2f, false);
    kit_frame_box(kit, &f, MAT_RUG, -0.34f, -0.2f, top + 0.03f, top + 0.16f, 0.2f, 0.203f, false);
    kit_frame_box(kit, &f, MAT_PAPER, -0.17f, -0.1f, top + 0.1f, top + 0.15f, 0.2f, 0.203f, false);
    const vec2 knob[] = {{0.0f, 0.0f}, {0.012f, 0.0f}, {0.012f, 0.012f}, {0.0f, 0.014f}};
    for (int k = 0; k < 2; k++)
        kit_frame_lathe_on(kit, &f, MAT_BLACK,
                           (vec3){-0.165f + 0.06f * (float)k, top + 0.05f, 0.2f},
                           (vec3){0.0f, 0.0f, 1.0f}, knob, KIT_COUNT(knob), 8);

    // The ashtray and its stub, the keys, and the photograph on a little easel back.
    const vec2 tray[] = {{0.0f, 0.0f},    {0.05f, 0.0f},    {0.055f, 0.02f},
                         {0.05f, 0.022f}, {0.045f, 0.008f}, {0.0f, 0.008f}};
    kit_frame_lathe(kit, &f, MAT_CERAMIC, 0.02f, 0.2f, top, tray, KIT_COUNT(tray), 12);
    kit_frame_lathe_on(
        kit, &f, MAT_PAPER, (vec3){0.0f, top + 0.014f, 0.2f}, (vec3){1.0f, 0.1f, 0.3f},
        (vec2[]){{0.0f, 0.0f}, {0.004f, 0.0f}, {0.004f, 0.035f}, {0.0f, 0.035f}}, 4, 6);
    enum { RING = 11 };
    vec3 ring[RING] = {{0.0f}};
    for (int i = 0; i < RING; i++) {
        const float t = 6.2831853f * (float)i / (float)(RING - 1);
        glm_vec3_copy((vec3){0.12f + 0.016f * cosf(t), top + 0.003f, 0.09f + 0.016f * sinf(t)},
                      ring[i]);
    }
    kit_frame_pipe(kit, &f, MAT_BRASS, ring, RING, 0.0018f, 5);
    kit_frame_box(kit, &f, MAT_BRASS, 0.136f, 0.19f, top, top + 0.003f, 0.085f, 0.097f, false);
    kit_frame_box(kit, &f, MAT_STEEL, 0.13f, 0.18f, top, top + 0.003f, 0.104f, 0.114f, false);
    const KitFrame stand = {{0.0f, 0.0f, 0.0f}, f.yaw - 0.25f};
    KitFrame photo = stand;
    kit_frame_point(&f, 0.36f, top, 0.1f, photo.origin);
    const CardSpec* pic = &CARDS[CARD_PHOTO_PARK];
    const float pw = pic->size[0] + 0.03f, ph = pic->size[1] + 0.03f;
    const vec3 corner = {-0.5f * pw, 0.0f, 0.0f}, across = {pw, 0.0f, 0.0f};
    const vec3 up = {0.0f, ph * 0.96f, -ph * 0.28f};
    kit_frame_card(kit, &photo, MAT_MAHOGANY, corner, across, up, pic->uv);
    const vec3 inner = {-0.5f * pic->size[0], 0.015f * 0.96f, -0.015f * 0.28f + 0.002f};
    kit_frame_card(kit, &photo, MAT_CARDS, inner, (vec3){pic->size[0], 0.0f, 0.0f},
                   (vec3){0.0f, pic->size[1] * 0.96f, -pic->size[1] * 0.28f}, pic->uv);

    // The pot, and the plant standing in it.
    const vec2 pot[] = {{0.0f, 0.0f},   {0.05f, 0.0f},   {0.065f, 0.12f}, {0.07f, 0.13f},
                        {0.06f, 0.13f}, {0.058f, 0.11f}, {0.0f, 0.11f}};
    kit_frame_lathe(kit, &f, MAT_CERAMIC, 0.2f, 0.17f, top, pot, KIT_COUNT(pot), 14);
    vec3 at = {0.0f, 0.0f, 0.0f};
    kit_frame_point(&f, 0.2f, top + 0.11f, 0.17f, at);
    glm_vec3_add(at, kit->origin, at);
    plant(engine, scene, at);
}

static void hall(Kit* kit, Engine* engine, Scene* scene) {
    const Facade east_front = facade_toward(&WALLS[HW_HALL_E_KITCHEN], HALL_MID_X, HALL_FRONT);
    const Facade east_back = facade_toward(&WALLS[HW_HALL_E_BATH], HALL_MID_X, HALL_BACK);
    const Facade west_back = facade_toward(&WALLS[HW_HALL_W_LIVING], HALL_MID_X, HALL_BACK);
    hang(kit, &east_front, CARD_PAINTING_ROAD, 11.4f, FLOOR_Y + 1.6f);
    hang(kit, &west_back, CARD_PAINTING_HOUSE, 14.75f, FLOOR_Y + 1.6f);
    hang(kit, &east_back, CARD_PAINTING_LAKE, 16.0f, FLOOR_Y + 1.58f);
    lantern(kit, scene, &west_back, 15.9f, FLOOR_Y + 1.75f);
    console(kit, engine, scene);
}

// ---------------------------------------------------------------------------------------------
// The living room

/*
 * The living room: a sofa with its back to the front window facing a television on a stand
 * against the back wall (tv.c), an armchair turned toward it, a coffee table, a rug, and a floor
 * lamp at the sofa's end, the room's one lamp.
 */
static void sofa(Kit* kit, const KitFrame* f, float ha, bool arms) {
    const float d0 = -0.42f, d1 = 0.42f;
    kit_frame_box(kit, f, MAT_UPHOLSTERY, -ha, ha, 0.08f, 0.42f, d0, d1, false);
    kit_frame_box(kit, f, MAT_UPHOLSTERY, -ha, ha, 0.42f, 0.88f, d0, d0 + 0.2f, false);
    const int seats = arms && ha > 0.6f ? 3 : 1;
    const float inner = arms ? ha - 0.17f : ha, w = 2.0f * inner / (float)seats;
    for (int i = 0; i < seats; i++) {
        const float a0 = -inner + (float)i * w;
        kit_frame_box(kit, f, MAT_UPHOLSTERY, a0 + 0.01f, a0 + w - 0.01f, 0.42f, 0.5f, d0 + 0.2f,
                      d1 - 0.02f, false);
    }
    if (arms)
        for (int s = -1; s <= 1; s += 2)
            kit_frame_box(kit, f, MAT_UPHOLSTERY, s < 0 ? -ha : ha - 0.17f,
                          s < 0 ? -ha + 0.17f : ha, 0.42f, 0.64f, d0, d1, false);
    for (int i = 0; i < 4; i++) {
        const float a = (i & 1) ? ha - 0.06f : -ha + 0.06f, d = (i & 2) ? d1 - 0.06f : d0 + 0.06f;
        kit_frame_box(kit, f, MAT_WOOD, a - 0.02f, a + 0.02f, 0.0f, 0.08f, d - 0.02f, d + 0.02f,
                      false);
    }
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, -ha, ha, 0.0f, 0.88f, d0, d1, true);
}

static void living_room(Kit* kit, Scene* scene) {
    // Facing +z, toward the television.
    const KitFrame couch = {{-TV_X, FLOOR_Y, 12.75f}, 0.0f};
    sofa(kit, &couch, 1.0f, true);
    const KitFrame chair = {{-2.15f, FLOOR_Y, 14.6f}, -0.69f};
    sofa(kit, &chair, 0.42f, true);

    const KitFrame* w = &KIT_WORLD;
    kit_frame_box(kit, w, MAT_RUG, -4.3f, -2.3f, FLOOR_Y, FLOOR_Y + 0.008f, 13.35f, 15.45f, false);
    // The coffee table, and on it a folded newspaper and a mug gone cold.
    const KitFrame table = {{-TV_X, FLOOR_Y, 14.2f}, 0.0f};
    kit_frame_box(kit, &table, MAT_WOOD, -0.5f, 0.5f, 0.4f, 0.44f, -0.26f, 0.26f, false);
    for (int i = 0; i < 4; i++) {
        const float a = (i & 1) ? 0.44f : -0.44f, d = (i & 2) ? 0.2f : -0.2f;
        kit_frame_box(kit, &table, MAT_WOOD, a - 0.02f, a + 0.02f, 0.0f, 0.4f, d - 0.02f, d + 0.02f,
                      false);
    }
    kit_frame_box(kit, &table, KIT_COLLIDER_ONLY, -0.5f, 0.5f, 0.0f, 0.44f, -0.26f, 0.26f, true);
    kit_frame_box(kit, &table, MAT_PAPER, -0.3f, 0.02f, 0.44f, 0.452f, -0.15f, 0.08f, false);
    const vec2 mug[] = {{0.0f, 0.0f},    {0.038f, 0.0f}, {0.042f, 0.004f}, {0.042f, 0.096f},
                        {0.0405f, 0.1f}, {0.037f, 0.1f}, {0.037f, 0.012f}, {0.0f, 0.012f}};
    kit_frame_lathe(kit, &table, MAT_CERAMIC, 0.25f, 0.05f, 0.44f, mug, KIT_COUNT(mug), 18);

    // The floor lamp: a weighted foot, a brass pole, a drum shade lit from inside.
    const float lx = -4.5f, lz = 12.45f, shade_y = FLOOR_Y + 1.32f;
    const vec2 foot[] = {
        {0.0f, 0.0f}, {0.15f, 0.0f}, {0.14f, 0.03f}, {0.03f, 0.05f}, {0.0f, 0.05f}};
    kit_frame_lathe(kit, w, MAT_BRASS, lx, lz, FLOOR_Y, foot, KIT_COUNT(foot), 16);
    kit_frame_prism(kit, w, MAT_BRASS, lx, lz, FLOOR_Y + 0.05f, shade_y + 0.12f, 0.011f, 8);
    const vec2 shade[] = {
        {0.21f, 0.0f}, {0.205f, 0.004f}, {0.15f, 0.3f}, {0.145f, 0.3f}, {0.2f, 0.004f}};
    kit_frame_lathe(kit, w, MAT_LAMPSHADE, lx, lz, shade_y, shade, KIT_COUNT(shade), 20);
    kit_frame_box(kit, w, KIT_COLLIDER_ONLY, lx - 0.15f, lx + 0.15f, FLOOR_Y, shade_y + 0.3f,
                  lz - 0.15f, lz + 0.15f, true);
    LightDesc lamp = {
        .name = "floor_lamp",
        .type = LIGHT_POINT,
        .position = {lx + kit->origin[0], shade_y + 0.14f + kit->origin[1], lz + kit->origin[2]},
        .color = {1.0f, 0.72f, 0.42f},
        .intensity = 40.0f,
        .range = 6.0f,
        .cast_shadows = true,
        .shadow_cache = true,
        .source_radius = 0.03f,
        .shadow_near = 0.05f};
    scene_add_light(scene, create_light(&lamp));
}

// ---------------------------------------------------------------------------------------------
// The bathroom

/*
 * The bathroom: a tub along its back wall, a toilet against its east wall, a pedestal sink with
 * a mirror over it beside the door, a towel on a rail, and one bulb in the ceiling. The door
 * swings in over the stretch of floor kept clear for it.
 */
static void bathroom(Kit* kit, Scene* scene) {
    const KitFrame* w = &KIT_WORLD;
    const float x0 = 0.95f, x1 = BATH_IN_X1, z0 = BATH_IN_Z1 - 0.72f, z1 = BATH_IN_Z1;
    const float rim = FLOOR_Y + 0.56f, t = 0.06f;
    kit_frame_box(kit, w, MAT_CERAMIC, x0, x1, FLOOR_Y, rim, z0, z0 + t, false);
    kit_frame_box(kit, w, MAT_CERAMIC, x0, x1, FLOOR_Y, rim, z1 - t, z1, false);
    kit_frame_box(kit, w, MAT_CERAMIC, x0, x0 + t, FLOOR_Y, rim, z0, z1, false);
    kit_frame_box(kit, w, MAT_CERAMIC, x1 - t, x1, FLOOR_Y, rim, z0, z1, false);
    kit_frame_box(kit, w, MAT_CERAMIC, x0 + t, x1 - t, FLOOR_Y, FLOOR_Y + 0.12f, z0 + t, z1 - t,
                  false);
    kit_frame_box(kit, w, KIT_COLLIDER_ONLY, x0, x1, FLOOR_Y, rim, z0, z1, true);
    const vec3 spout[] = {{x1 - 0.02f, rim + 0.18f, 0.5f * (z0 + z1)},
                          {x1 - 0.12f, rim + 0.2f, 0.5f * (z0 + z1)},
                          {x1 - 0.16f, rim + 0.14f, 0.5f * (z0 + z1)}};
    kit_frame_pipe(kit, w, MAT_STAINLESS, spout, 3, 0.012f, 8);

    // The toilet: a tank on the wall, a bowl in front of it, a seat and a lid.
    const KitFrame loo = {{BATH_IN_X1, FLOOR_Y, 14.35f}, -0.5f * GLM_PIf};
    kit_frame_box(kit, &loo, MAT_CERAMIC, -0.2f, 0.2f, 0.42f, 0.8f, 0.0f, 0.18f, false);
    const vec2 bowl[] = {{0.0f, 0.0f},  {0.1f, 0.0f},   {0.09f, 0.18f}, {0.16f, 0.34f},
                         {0.18f, 0.4f}, {0.16f, 0.41f}, {0.0f, 0.38f}};
    kit_frame_lathe(kit, &loo, MAT_CERAMIC, 0.0f, 0.38f, 0.0f, bowl, KIT_COUNT(bowl), 16);
    kit_frame_box(kit, &loo, MAT_CERAMIC, -0.17f, 0.17f, 0.41f, 0.43f, 0.2f, 0.56f, false);
    kit_frame_box(kit, &loo, KIT_COLLIDER_ONLY, -0.2f, 0.2f, 0.0f, 0.8f, 0.0f, 0.56f, true);

    // The sink on the front wall, the mirror over it, a glass on the shelf under the mirror.
    const KitFrame front = {{1.55f, FLOOR_Y, BATH_IN_Z0}, 0.0f};
    const vec2 column[] = {
        {0.0f, 0.0f}, {0.11f, 0.0f}, {0.07f, 0.1f}, {0.06f, 0.6f}, {0.0f, 0.62f}};
    kit_frame_lathe(kit, &front, MAT_CERAMIC, 0.0f, 0.22f, 0.0f, column, KIT_COUNT(column), 14);
    const vec2 basin[] = {{0.0f, 0.0f},   {0.08f, 0.0f},  {0.24f, 0.12f}, {0.26f, 0.2f},
                          {0.24f, 0.21f}, {0.22f, 0.14f}, {0.06f, 0.07f}, {0.0f, 0.07f}};
    kit_frame_lathe(kit, &front, MAT_CERAMIC, 0.0f, 0.24f, 0.6f, basin, KIT_COUNT(basin), 20);
    const vec3 tap[] = {{0.0f, 0.81f, 0.03f}, {0.0f, 0.9f, 0.06f}, {0.0f, 0.87f, 0.14f}};
    kit_frame_pipe(kit, &front, MAT_STAINLESS, tap, 3, 0.01f, 8);
    kit_frame_box(kit, &front, KIT_COLLIDER_ONLY, -0.26f, 0.26f, 0.0f, 0.82f, 0.0f, 0.48f, true);
    kit_frame_box(kit, &front, MAT_MIRROR, -0.25f, 0.25f, 1.12f, 1.72f, 0.0f, 0.008f, false);
    kit_frame_box(kit, &front, MAT_STAINLESS, -0.28f, 0.28f, 1.06f, 1.08f, 0.0f, 0.1f, false);
    const vec2 cup[] = {{0.0f, 0.0f},   {0.028f, 0.0f},   {0.032f, 0.1f},
                        {0.029f, 0.1f}, {0.026f, 0.008f}, {0.0f, 0.008f}};
    kit_frame_lathe(kit, &front, MAT_GLASS_CLEAR, 0.15f, 0.05f, 1.08f, cup, KIT_COUNT(cup), 12);

    // The towel on its rail on the east wall, over the tub's end.
    const KitFrame east = {{BATH_IN_X1, FLOOR_Y, 15.2f}, -0.5f * GLM_PIf};
    kit_frame_bar(kit, &east, MAT_STAINLESS, -0.3f, 0.3f, 1.25f, 0.06f, 0.01f);
    kit_frame_box(kit, &east, MAT_TOWEL, -0.22f, 0.22f, 0.8f, 1.27f, 0.05f, 0.075f, false);
    kit_frame_box(kit, w, MAT_RUG, 1.2f, 2.2f, FLOOR_Y, FLOOR_Y + 0.008f, z0 - 0.55f, z0 - 0.05f,
                  false);

    // The bulb, bare on a ceiling rose.
    const float bx = 1.35f, bz = 15.0f, by = CEIL_Y - 0.12f;
    const vec2 rose[] = {{0.0f, 0.0f},     {0.05f, 0.0f},    {0.045f, -0.02f},
                         {0.012f, -0.03f}, {0.012f, -0.07f}, {0.0f, -0.075f}};
    kit_frame_lathe(kit, w, MAT_CERAMIC, bx, bz, CEIL_Y, rose, KIT_COUNT(rose), 12);
    const vec2 glass[] = {{0.0f, 0.0f},    {0.012f, 0.004f}, {0.025f, 0.018f}, {0.03f, 0.035f},
                          {0.026f, 0.05f}, {0.014f, 0.062f}, {0.0f, 0.065f}};
    kit_frame_lathe(kit, w, MAT_BULB, bx, bz, by - 0.03f, glass, KIT_COUNT(glass), 14);
    LightDesc bulb = {.name = "bath_bulb",
                      .type = LIGHT_POINT,
                      .position = {bx + kit->origin[0], by + kit->origin[1], bz + kit->origin[2]},
                      .color = {1.0f, 0.82f, 0.6f},
                      .intensity = 24.0f,
                      .range = 5.0f,
                      .cast_shadows = true,
                      .shadow_cache = true,
                      .source_radius = 0.03f,
                      .shadow_near = 0.045f};
    scene_add_light(scene, create_light(&bulb));
}

void home_build(Kit* kit, Engine* engine, Scene* scene) {
    walls(kit);
    outside_trim(kit);
    floors(kit);
    roof(kit);
    chimney(kit);
    porch(kit);
    finishes(kit);
    hall(kit, engine, scene);
    living_room(kit, scene);
    bathroom(kit, scene);
}

// The front door, hung on its west jamb against the front wall's inner face so it swings into
// the hall, as the Gothic house's does.
bool home_front_door(Door* door, Engine* engine, Scene* scene, EntityManager* em,
                     PhysicsWorld* physics) {
    const KitOpening* opening = &WALLS[HW_FRONT].openings[O_FRONT_DOOR];
    const KitFrame hinge = {{opening->from, 0.0f, FRONT_DOOR_Z}, 0.0f};
    return door_hang(door, engine, scene, em, physics, "front_door", door_leaf_panelled, &hinge,
                     *opening, DOOR_SWING);
}

// The bathroom's, hung on its back jamb against the bathroom's face of the wall, so it swings
// into the bathroom and stands open along the floor kept clear for it.
bool home_bath_door(Door* door, Engine* engine, Scene* scene, EntityManager* em,
                    PhysicsWorld* physics) {
    const KitOpening* opening = &WALLS[HW_HALL_E_BATH].openings[O_BATH_DOOR];
    // Turned a quarter, so a runs toward -z from the hinge and d into the bathroom.
    const float x = HALL_X1 + 0.5f * INT_WALL - 0.5f * DOOR_THICK - 0.005f;
    const KitFrame hinge = {{x, 0.0f, opening->to}, 0.5f * GLM_PIf};
    return door_hang(door, engine, scene, em, physics, "bath_door", door_leaf_panelled, &hinge,
                     *opening, 1.6f);
}

// The basement's (spec 13.31), hung on its front jamb against the stairwell's face of the wall,
// so it swings in over the head of the flight and stands open along the partition, just over the
// stringer. Out into the hall it would sweep where whoever opens it is standing.
bool home_basement_door(Door* door, Engine* engine, Scene* scene, EntityManager* em,
                        PhysicsWorld* physics) {
    const KitOpening* opening = &WALLS[HW_HALL_W_STAIR].openings[O_BASEMENT_DOOR];
    // Turned a quarter the other way, so a runs toward +z from the hinge and d into the stairwell.
    const float x = HALL_OUT_X0 + 0.5f * DOOR_THICK + 0.005f;
    const KitFrame hinge = {{x, 0.0f, opening->from}, -0.5f * GLM_PIf};
    return door_hang(door, engine, scene, em, physics, "basement_door", door_leaf_panelled, &hinge,
                     *opening, 1.55f);
}
