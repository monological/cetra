#include <math.h>
#include <stdio.h>

#include "cetra/game/audio.h"
#include "cetra/light.h"

#include "cabin.h"
#include "cards.h"
#include "kitchen.h"
#include "layout.h"
#include "mats.h"
#include "sounds.h"

/*
 * The log cabin on the lake's east bank (spec 13.41).
 *
 * Its walls are round logs in courses on a chinked core: the core is an ordinary kit wall, which
 * is what cuts the door and the windows and stops anyone walking through, and the logs are laid
 * along it either side of its middle, cut back at every opening. The side walls' courses sit half
 * a course above the end walls' and every log runs on past the corner, so the four meet
 * interleaved the way saddle-notched logs do. The ridge runs along x, so the gables are the front
 * and the back: boards above the eaves, the door and the porch in the front one, the chimney up
 * the back one.
 *
 * Everything is in the world's coordinates, in a kit of its own; nothing in it moves but the
 * door, and nothing in it is grimed, since a log's grime would be the kit's edge darkening and a
 * log has no edges.
 */

#define CORE      0.16f // the chinked core's thickness
#define LOG_R     0.13f // a wall log's radius
#define COURSE    0.24f // a course's height: the logs overlap, with the chinking between
#define LOG_RUN   0.28f // how far each log runs on past the corner
#define LOG_SIDES 10

#define PITCH    ((CABIN_RIDGE_Y - CABIN_EAVE_Y) / (0.5f * (CABIN_Z1 - CABIN_Z0)))
#define ROOF_T   0.08f // the tin and its battens
#define EAVE_OUT 0.45f // the roof past the side walls
#define MID_Z    (0.5f * (CABIN_Z0 + CABIN_Z1))

#define DECK_X0 (CABIN_X0 - 2.3f) // the porch's outer edge, toward the water
#define DECK_Y  (CABIN_FLOOR_Y - 0.15f)
#define ROOF_X0 (DECK_X0 - 0.35f) // the roof carries on over it
#define ROOF_X1 (CABIN_X1 + 0.3f)

#define DOOR_Z0      (MID_Z - 0.45f)
#define DOOR_Z1      (MID_Z + 0.45f)
#define DOORWAY_HEAD (CABIN_FLOOR_Y + 1.95f)
#define WIN_X0       (-65.3f)
#define WIN_X1       (-64.4f)
#define WIN_SILL     (CABIN_FLOOR_Y + 0.95f)
#define WIN_HEAD     (CABIN_FLOOR_Y + 1.65f)
// The front's window, north of the door, which shows the fire to the dock.
#define FRONT_WIN_Z0 (CABIN_Z0 + 0.8f)
#define FRONT_WIN_Z1 (CABIN_Z0 + 1.6f)

// Against the other loops tools/fetch_sounds.py levels alike: the fire, and under it the room.
#define HEARTH_VOLUME 0.45f
#define ROOM_VOLUME   0.12f

// The hearth's light over what the flipbook's flames cast, as the candles' is (candles.c): the
// night is exposed for a house lit by a few of them, and a fire at its physical brightness left
// the room it was burning in dark.
#define HEARTH_LIGHT_SCALE 8.0f

// The fireplace against the back wall, inside: its stone mass, and the firebox in it.
#define HEARTH_FACE (CABIN_X1 - 0.65f) // the mass's face into the room
#define FIREBOX_Z0  (MID_Z - 0.45f)
#define FIREBOX_Z1  (MID_Z + 0.45f)
#define FIREBOX_Y0  (CABIN_FLOOR_Y + 0.2f)
#define FIREBOX_Y1  (CABIN_FLOOR_Y + 1.0f)
#define FIREBOX_D   0.45f // how deep it goes into the mass

#define TABLE_TOP (CABIN_FLOOR_Y + 0.74f)

// The walls: their core, with the door and the windows cut in it.
static const KitWall WALLS[4] = {
    // The front, onto the porch and the water.
    {.along_x = false,
     .at = CABIN_X0,
     .from = CABIN_Z0,
     .to = CABIN_Z1,
     .y0 = CABIN_FLOOR_Y,
     .y1 = CABIN_EAVE_Y,
     .thick = CORE,
     .inner = 1,
     .mat_inner = MAT_CONCRETE,
     .mat_outer = MAT_CONCRETE,
     .openings = {{DOOR_Z0, DOOR_Z1, CABIN_FLOOR_Y, DOORWAY_HEAD, KIT_ARCH_FLAT, 0.0f, true},
                  {FRONT_WIN_Z0, FRONT_WIN_Z1, WIN_SILL, WIN_HEAD, KIT_ARCH_FLAT, 0.0f, false}},
     .opening_count = 2},
    // The back, the chimney up it.
    {.along_x = false,
     .at = CABIN_X1,
     .from = CABIN_Z0,
     .to = CABIN_Z1,
     .y0 = CABIN_FLOOR_Y,
     .y1 = CABIN_EAVE_Y,
     .thick = CORE,
     .inner = -1,
     .mat_inner = MAT_CONCRETE,
     .mat_outer = MAT_CONCRETE},
    // The two sides, a window in each.
    {.along_x = true,
     .at = CABIN_Z0,
     .from = CABIN_X0,
     .to = CABIN_X1,
     .y0 = CABIN_FLOOR_Y,
     .y1 = CABIN_EAVE_Y,
     .thick = CORE,
     .inner = 1,
     .mat_inner = MAT_CONCRETE,
     .mat_outer = MAT_CONCRETE,
     .openings = {{WIN_X0, WIN_X1, WIN_SILL, WIN_HEAD, KIT_ARCH_FLAT, 0.0f, false}},
     .opening_count = 1},
    {.along_x = true,
     .at = CABIN_Z1,
     .from = CABIN_X0,
     .to = CABIN_X1,
     .y0 = CABIN_FLOOR_Y,
     .y1 = CABIN_EAVE_Y,
     .thick = CORE,
     .inner = -1,
     .mat_inner = MAT_CONCRETE,
     .mat_outer = MAT_CONCRETE,
     .openings = {{WIN_X0, WIN_X1, WIN_SILL, WIN_HEAD, KIT_ARCH_FLAT, 0.0f, false}},
     .opening_count = 1},
};

// The roof's top at z: the tin over the ridge, falling to the eaves.
static float roof_y(float z) {
    return CABIN_RIDGE_Y + ROOF_T - PITCH * fabsf(z - MID_Z);
}

/*
 * A log from `from` along `axis`, `len` long: bark round it, the sawn end grain at both ends.
 * One profile in three materials, each piece run the way the whole one would be, up the outside.
 */
static void log_piece(Kit* kit, const vec3 from, const vec3 axis, float len, float r, int sides) {
    const vec2 start[] = {{0.0f, 0.0f}, {r, 0.0f}};
    const vec2 bark[] = {{r, 0.0f}, {r, len}};
    const vec2 end[] = {{r, len}, {0.0f, len}};
    kit_frame_lathe_on(kit, &KIT_WORLD, MAT_PORCH, from, axis, start, 2, sides);
    kit_frame_lathe_on(kit, &KIT_WORLD, MAT_LOG, from, axis, bark, 2, sides);
    kit_frame_lathe_on(kit, &KIT_WORLD, MAT_PORCH, from, axis, end, 2, sides);
}

// The roof's underside at z.
static float roof_under(float z) {
    return CABIN_RIDGE_Y - PITCH * fabsf(z - MID_Z);
}

/*
 * One wall's logs, course by course, each cut back clear of the openings it crosses and kept
 * under the roof: a side wall's stop a course short where the eave comes down over their outside,
 * and an end wall's top courses stop short of running on past the corners, where the roof slopes
 * down over them. The chinking core fills to the eave above the last of them.
 */
static void wall_logs(Kit* kit, const KitWall* w, float lift) {
    const vec3 axis = {w->along_x ? 1.0f : 0.0f, 0.0f, w->along_x ? 0.0f : 1.0f};
    for (float y = CABIN_FLOOR_Y + 0.6f * LOG_R + lift; y < CABIN_EAVE_Y - 0.3f * LOG_R;
         y += COURSE) {
        float lo = w->from - LOG_RUN, hi = w->to + LOG_RUN;
        if (w->along_x) {
            if (y + LOG_R > roof_under(w->at + (float)-w->inner * LOG_R))
                break;
        } else {
            const float reach = (CABIN_RIDGE_Y - (y + LOG_R)) / PITCH;
            lo = fmaxf(lo, MID_Z - reach);
            hi = fminf(hi, MID_Z + reach);
        }
        vec2 blocked[KIT_MAX_OPENINGS] = {{0.0f}}, spans[KIT_MAX_OPENINGS + 1] = {{0.0f}};
        int n = 0;
        for (int i = 0; i < w->opening_count; i++) {
            const KitOpening* o = &w->openings[i];
            if (y > o->bottom - 0.7f * LOG_R && y < o->top + 0.7f * LOG_R)
                glm_vec2_copy((vec2){o->from - 0.02f, o->to + 0.02f}, blocked[n++]);
        }
        const int count = kit_clear_spans(lo, hi, blocked, n, spans);
        for (int s = 0; s < count; s++) {
            const vec3 from = {w->along_x ? spans[s][0] : w->at, y,
                               w->along_x ? w->at : spans[s][0]};
            log_piece(kit, from, axis, spans[s][1] - spans[s][0], LOG_R, LOG_SIDES);
        }
    }
}

// A frame round an opening, in the wall's own frame: rough boards nailed round its reveal.
static void casing(Kit* kit, const KitWall* w, const KitOpening* o, int glass) {
    const KitWallFrame wf = kit_wall_frame(w);
    const KitFrame* f = &wf.f;
    const float at = wf.at, t = LOG_R + 0.03f, b = 0.07f;
    kit_frame_box(kit, f, MAT_FENCE_BOARD, o->from - b, o->from, o->bottom, o->top + b, at - t,
                  at + t, false);
    kit_frame_box(kit, f, MAT_FENCE_BOARD, o->to, o->to + b, o->bottom, o->top + b, at - t, at + t,
                  false);
    kit_frame_box(kit, f, MAT_FENCE_BOARD, o->from - b, o->to + b, o->top, o->top + b, at - t,
                  at + t, false);
    if (glass < 0)
        return;
    kit_frame_box(kit, f, MAT_FENCE_BOARD, o->from - b, o->to + b, o->bottom - 0.05f, o->bottom,
                  at - t, at + t + 0.06f * (float)-wf.inner, false);
    kit_frame_pane(kit, f, glass, o, at, w->thick);
    // Four lights: a bar each way across the glass.
    const float mid = 0.5f * (o->bottom + o->top), centre = 0.5f * (o->from + o->to);
    kit_frame_box(kit, f, MAT_FENCE_BOARD, o->from, o->to, mid - 0.02f, mid + 0.02f, at - 0.02f,
                  at + 0.02f, false);
    kit_frame_box(kit, f, MAT_FENCE_BOARD, centre - 0.02f, centre + 0.02f, o->bottom, o->top,
                  at - 0.02f, at + 0.02f, false);
}

static void walls(Kit* kit) {
    for (int i = 0; i < 4; i++) {
        kit_wall(kit, &WALLS[i]);
        // The side walls' courses half a course over the ends', so they interleave at the corners.
        wall_logs(kit, &WALLS[i], WALLS[i].along_x ? 0.5f * COURSE : 0.0f);
    }
    casing(kit, &WALLS[0], &WALLS[0].openings[0], -1);
    casing(kit, &WALLS[0], &WALLS[0].openings[1], MAT_WINDOW_GLASS);
    casing(kit, &WALLS[2], &WALLS[2].openings[0], MAT_WINDOW_GLASS);
    casing(kit, &WALLS[3], &WALLS[3].openings[0], MAT_WINDOW_GLASS);
}

// The footing, the floor, and the tie logs across under the roof.
static void floor_and_ties(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    kit_frame_box(kit, w, MAT_FOUNDATION, CABIN_X0 - 0.15f, CABIN_X1 + 0.15f, CABIN_PAD_Y,
                  CABIN_FLOOR_Y - 0.04f, CABIN_Z0 - 0.15f, CABIN_Z1 + 0.15f, true);
    kit_frame_box(kit, w, MAT_PORCH, CABIN_X0, CABIN_X1, CABIN_FLOOR_Y - 0.04f, CABIN_FLOOR_Y,
                  CABIN_Z0, CABIN_Z1, false);
    kit_frame_box(kit, w, KIT_COLLIDER_ONLY, CABIN_X0, CABIN_X1, CABIN_PAD_Y, CABIN_FLOOR_Y,
                  CABIN_Z0, CABIN_Z1, true);
    for (int i = 0; i < 3; i++) {
        const float x = CABIN_X0 + 1.1f + 1.4f * (float)i;
        log_piece(kit, (vec3){x, CABIN_EAVE_Y - 0.12f, CABIN_Z0 - LOG_RUN},
                  (vec3){0.0f, 0.0f, 1.0f}, CABIN_Z1 - CABIN_Z0 + 2.0f * LOG_RUN, 0.11f, LOG_SIDES);
    }
}

/*
 * The roof: rusted corrugated tin on battens, the ridge along x, carried on west over the porch;
 * boards closing the gables above the eaves; and the eaves dripping.
 */
static void roof(Kit* kit) {
    const float zn = CABIN_Z0 - EAVE_OUT, zs = CABIN_Z1 + EAVE_OUT;
    const vec3 north[4] = {{ROOF_X0, roof_y(zn), zn},
                           {ROOF_X1, roof_y(zn), zn},
                           {ROOF_X1, roof_y(MID_Z), MID_Z},
                           {ROOF_X0, roof_y(MID_Z), MID_Z}};
    const vec3 south[4] = {{ROOF_X0, roof_y(MID_Z), MID_Z},
                           {ROOF_X1, roof_y(MID_Z), MID_Z},
                           {ROOF_X1, roof_y(zs), zs},
                           {ROOF_X0, roof_y(zs), zs}};
    const vec3 down = {0.0f, -ROOF_T, 0.0f};
    kit_extrude(kit, MAT_TIN, MAT_FENCE_BOARD, MAT_FENCE_BOARD, north, 4, down);
    kit_extrude(kit, MAT_TIN, MAT_FENCE_BOARD, MAT_FENCE_BOARD, south, 4, down);
    // The ridge's cap.
    kit_frame_box(kit, &KIT_WORLD, MAT_TIN, ROOF_X0, ROOF_X1, roof_y(MID_Z) - 0.02f,
                  roof_y(MID_Z) + 0.03f, MID_Z - 0.12f, MID_Z + 0.12f, false);

    // Each gable between the side walls' lines, its apex a hair under the roof, so no face of it
    // lies in the roof's plane: in the frame whose a runs along +z and d along -x.
    const float side = 0.5f * (CABIN_Z1 - CABIN_Z0);
    const vec2 gable[3] = {{MID_Z - side, CABIN_EAVE_Y},
                           {MID_Z + side, CABIN_EAVE_Y},
                           {MID_Z, roof_y(MID_Z) - ROOF_T - 0.02f}};
    const float gx[2] = {CABIN_X0, CABIN_X1};
    for (int g = 0; g < 2; g++)
        kit_frame_extrude(kit, &KIT_WORLD_Z, MAT_FENCE_BOARD, gable, 3, -gx[g] - 0.5f * CORE,
                          -gx[g] + 0.5f * CORE);

    const float drip_y = roof_y(zn) - ROOF_T;
    kit_drip_run(kit, &KIT_WORLD, (vec3){ROOF_X0, drip_y, zn}, (vec3){ROOF_X1, drip_y, zn}, 0.6f,
                 0.0f);
    kit_drip_run(kit, &KIT_WORLD, (vec3){ROOF_X0, drip_y, zs}, (vec3){ROOF_X1, drip_y, zs}, 0.6f,
                 0.0f);
}

/*
 * The fireplace: a mass of fieldstone against the back wall inside, the firebox in it blackened,
 * a stone hearth in front, an iron grate with logs on it -- the fire is the flipbook's, on cards
 * over them -- and outside, the chimney up the back gable past the ridge.
 */
static void fireplace(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    const float top = CABIN_EAVE_Y - 0.25f, back = CABIN_X1 - 0.5f * CORE;
    const float fz0 = FIREBOX_Z0 - 0.5f, fz1 = FIREBOX_Z1 + 0.5f;
    // The cheeks, the lintel over the box, and its base.
    kit_frame_box(kit, w, MAT_FOUNDATION, HEARTH_FACE, back, CABIN_FLOOR_Y, top, fz0, FIREBOX_Z0,
                  true);
    kit_frame_box(kit, w, MAT_FOUNDATION, HEARTH_FACE, back, CABIN_FLOOR_Y, top, FIREBOX_Z1, fz1,
                  true);
    kit_frame_box(kit, w, MAT_FOUNDATION, HEARTH_FACE, back, FIREBOX_Y1, top, FIREBOX_Z0,
                  FIREBOX_Z1, true);
    kit_frame_box(kit, w, MAT_FOUNDATION, HEARTH_FACE, back, CABIN_FLOOR_Y, FIREBOX_Y0, FIREBOX_Z0,
                  FIREBOX_Z1, true);
    // The firebox's soot: its back and sides, and its floor.
    const float fb = HEARTH_FACE + FIREBOX_D;
    kit_frame_box(kit, w, MAT_SOOT, fb, back, FIREBOX_Y0, FIREBOX_Y1, FIREBOX_Z0, FIREBOX_Z1,
                  false);
    kit_frame_box(kit, w, MAT_SOOT, HEARTH_FACE + 0.01f, fb, FIREBOX_Y0, FIREBOX_Y1,
                  FIREBOX_Z0 - 0.01f, FIREBOX_Z0 + 0.02f, false);
    kit_frame_box(kit, w, MAT_SOOT, HEARTH_FACE + 0.01f, fb, FIREBOX_Y0, FIREBOX_Y1,
                  FIREBOX_Z1 - 0.02f, FIREBOX_Z1 + 0.01f, false);
    kit_frame_box(kit, w, MAT_SOOT, HEARTH_FACE + 0.01f, fb, FIREBOX_Y0 - 0.01f, FIREBOX_Y0 + 0.01f,
                  FIREBOX_Z0, FIREBOX_Z1, false);
    // A shelf of a mantel, and the hearthstone out in front.
    kit_frame_box(kit, w, MAT_FENCE_BOARD, HEARTH_FACE - 0.12f, back, FIREBOX_Y1 + 0.35f,
                  FIREBOX_Y1 + 0.41f, fz0 - 0.05f, fz1 + 0.05f, false);
    kit_frame_box(kit, w, MAT_STONE, HEARTH_FACE - 0.5f, HEARTH_FACE, CABIN_FLOOR_Y,
                  CABIN_FLOOR_Y + 0.04f, fz0 + 0.1f, fz1 - 0.1f, false);
    // The grate, and two logs on it with a third across them.
    const float gy = FIREBOX_Y0 + 0.08f, gx = HEARTH_FACE + 0.22f;
    for (int i = 0; i < 4; i++) {
        const float z = FIREBOX_Z0 + 0.15f + 0.2f * (float)i;
        kit_frame_box(kit, w, MAT_IRON, gx - 0.15f, gx + 0.15f, gy - 0.02f, gy, z - 0.01f,
                      z + 0.01f, false);
    }
    kit_frame_box(kit, w, MAT_IRON, gx - 0.17f, gx - 0.13f, FIREBOX_Y0, gy + 0.12f,
                  FIREBOX_Z0 + 0.1f, FIREBOX_Z1 - 0.1f, false);
    log_piece(kit, (vec3){gx - 0.06f, gy + 0.06f, FIREBOX_Z0 + 0.08f}, (vec3){0.0f, 0.0f, 1.0f},
              0.72f, 0.06f, 8);
    log_piece(kit, (vec3){gx + 0.08f, gy + 0.06f, FIREBOX_Z0 + 0.1f}, (vec3){0.0f, 0.0f, 1.0f},
              0.7f, 0.055f, 8);
    log_piece(kit, (vec3){gx - 0.12f, gy + 0.15f, MID_Z - 0.1f}, (vec3){0.88f, 0.3f, 0.36f}, 0.3f,
              0.05f, 8);

    // The chimney up the back, past the ridge.
    const float cx0 = CABIN_X1 + 0.5f * CORE, cx1 = cx0 + 0.85f, cz0 = MID_Z - 0.75f,
                cz1 = MID_Z + 0.75f;
    const float shoulder = CABIN_EAVE_Y - 0.4f, crown = CABIN_RIDGE_Y + 0.9f;
    kit_frame_box(kit, w, MAT_FOUNDATION, cx0, cx1, CABIN_PAD_Y, shoulder, cz0, cz1, true);
    kit_frame_box(kit, w, MAT_FOUNDATION, cx0, cx1 - 0.25f, shoulder, crown, cz0 + 0.2f, cz1 - 0.2f,
                  true);
    kit_frame_box(kit, w, MAT_STONE, cx0 - 0.05f, cx1 - 0.2f, crown, crown + 0.08f, cz0 + 0.15f,
                  cz1 - 0.15f, false);
    kit_frame_box(kit, w, MAT_SOOT, cx0 + 0.15f, cx1 - 0.4f, crown + 0.08f, crown + 0.1f,
                  cz0 + 0.4f, cz1 - 0.4f, false);
}

/*
 * The porch toward the water: boards a step down from the floor, a step down again to the pad
 * where the dock's line comes in, two log posts holding the roof's end, and a rail of poles round
 * it but at the steps. On it a rocking chair turned to the water.
 */
static void porch(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    const float z0 = CABIN_Z0 + 0.3f, z1 = CABIN_Z1 - 0.3f;
    kit_frame_box(kit, w, MAT_PORCH, DECK_X0, CABIN_X0 - 0.5f * CORE, CABIN_PAD_Y, DECK_Y, z0, z1,
                  true);
    kit_frame_box(kit, w, MAT_PORCH, DECK_X0 - 0.32f, DECK_X0, CABIN_PAD_Y,
                  0.5f * (CABIN_PAD_Y + DECK_Y), DOOR_Z0 - 0.15f, DOOR_Z1 + 0.15f, true);
    const float px = DECK_X0 + 0.15f;
    const float pz[2] = {z0 + 0.15f, z1 - 0.15f};
    for (int i = 0; i < 2; i++) {
        const float top = roof_y(pz[i]) - ROOF_T;
        log_piece(kit, (vec3){px, DECK_Y, pz[i]}, (vec3){0.0f, 1.0f, 0.0f}, top - DECK_Y, 0.09f, 8);
    }
    // Rails of poles, two high: along each side, and across the front either side of the steps.
    const float rails[2] = {DECK_Y + 0.45f, DECK_Y + 0.88f};
    for (int r = 0; r < 2; r++) {
        for (int i = 0; i < 2; i++)
            log_piece(kit, (vec3){px, rails[r], pz[i]}, (vec3){1.0f, 0.0f, 0.0f},
                      CABIN_X0 - 0.5f * CORE - px, 0.04f, 6);
        log_piece(kit, (vec3){px, rails[r], pz[0]}, (vec3){0.0f, 0.0f, 1.0f},
                  DOOR_Z0 - 0.3f - pz[0], 0.04f, 6);
        log_piece(kit, (vec3){px, rails[r], DOOR_Z1 + 0.3f}, (vec3){0.0f, 0.0f, 1.0f},
                  pz[1] - DOOR_Z1 - 0.3f, 0.04f, 6);
    }
    for (int i = 0; i < 2; i++) {
        log_piece(kit, (vec3){px, DECK_Y, i ? DOOR_Z1 + 0.3f : DOOR_Z0 - 0.3f},
                  (vec3){0.0f, 1.0f, 0.0f}, 0.95f, 0.05f, 6);
        kit_frame_box(kit, w, KIT_COLLIDER_ONLY, px - 0.05f, CABIN_X0, DECK_Y, DECK_Y + 0.95f,
                      pz[i] - 0.05f, pz[i] + 0.05f, true);
    }
    kit_frame_box(kit, w, KIT_COLLIDER_ONLY, px - 0.05f, px + 0.05f, DECK_Y, DECK_Y + 0.95f, pz[0],
                  DOOR_Z0 - 0.3f, true);
    kit_frame_box(kit, w, KIT_COLLIDER_ONLY, px - 0.05f, px + 0.05f, DECK_Y, DECK_Y + 0.95f,
                  DOOR_Z1 + 0.3f, pz[1], true);

    // The rocking chair: the kitchen's chair on two rockers, its sitter facing the water.
    const KitFrame chair = {{CABIN_X0 - 1.0f, DECK_Y + 0.04f, z0 + 0.65f}, -0.5f * GLM_PIf + 0.2f};
    kitchen_chair(kit, &chair);
    for (int s = -1; s <= 1; s += 2) {
        enum { ARC = 7 };
        vec3 rocker[ARC] = {{0.0f}};
        for (int i = 0; i < ARC; i++) {
            const float d = -0.36f + 0.66f * (float)i / (float)(ARC - 1);
            kit_frame_point(&chair, (float)s * 0.19f, -0.03f + 0.55f * (d + 0.02f) * (d + 0.02f), d,
                            rocker[i]);
        }
        kit_frame_pipe(kit, &KIT_WORLD, MAT_WOOD, rocker, ARC, 0.018f, 6);
    }
}

// Split wood stacked against the south wall under the eave, end grain out.
static void woodpile(Kit* kit, KitRng* rng) {
    const float x0 = -66.4f, x1 = -63.6f, z = CABIN_Z1 + LOG_R + 0.02f, len = 0.42f;
    for (int layer = 0; layer < 5; layer++) {
        const float r = 0.065f, y = CABIN_PAD_Y + r + 2.0f * r * 0.92f * (float)layer;
        const float shift = (layer & 1) ? r : 0.0f;
        for (float x = x0 + r + shift; x < x1 - r; x += 2.0f * r) {
            if (layer == 4 && kit_rnd(rng) < 0.35f)
                continue;
            const float jut = 0.04f * kit_rnd(rng);
            log_piece(kit, (vec3){x, y, z + jut}, (vec3){0.0f, 0.0f, 1.0f}, len, r * 0.95f, 6);
        }
    }
    kit_frame_box(kit, &KIT_WORLD, KIT_COLLIDER_ONLY, x0, x1, CABIN_PAD_Y, CABIN_PAD_Y + 0.65f, z,
                  z + len + 0.04f, true);
}

// The cook stove in the north-east corner, cast iron, cold: on legs, its oven door on the front,
// two lids on its top, and its pipe up out through the roof.
static void stove(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    const float x0 = CABIN_X1 - 1.5f, x1 = CABIN_X1 - 0.85f, z0 = CABIN_Z0 + LOG_R + 0.12f,
                z1 = z0 + 0.6f;
    const float y0 = CABIN_FLOOR_Y + 0.15f, y1 = CABIN_FLOOR_Y + 0.78f;
    kit_frame_box(kit, w, MAT_IRON, x0, x1, y0, y1, z0, z1, true);
    for (int i = 0; i < 4; i++) {
        const float x = (i & 1) ? x1 - 0.05f : x0 + 0.05f, z = (i & 2) ? z1 - 0.05f : z0 + 0.05f;
        kit_prism(kit, MAT_IRON, x, z, CABIN_FLOOR_Y, y0, 0.025f, 6, false);
    }
    kit_frame_box(kit, w, MAT_BLACK, x0 + 0.08f, x1 - 0.08f, y0 + 0.08f, y1 - 0.2f, z1, z1 + 0.012f,
                  false);
    kit_frame_box(kit, w, MAT_STAINLESS, x0 + 0.25f, x1 - 0.25f, y1 - 0.28f, y1 - 0.26f,
                  z1 + 0.012f, z1 + 0.03f, false);
    for (int i = 0; i < 2; i++)
        kit_prism(kit, MAT_BLACK, x0 + 0.17f + 0.3f * (float)i, z0 + 0.35f, y1, y1 + 0.01f, 0.09f,
                  12, false);
    const float px = x0 + 0.15f, pz = z0 + 0.12f;
    kit_prism(kit, MAT_IRON, px, pz, y1, roof_y(pz) + 0.6f, 0.065f, 10, false);
    kit_prism(kit, MAT_IRON, px, pz, roof_y(pz) + 0.6f, roof_y(pz) + 0.66f, 0.1f, 10, false);
}

// The meal nobody came back to: a table by the south window laid for one, its chair pushed back,
// the lamp lit beside the plate.
static void table(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    const float cx = 0.5f * (WIN_X0 + WIN_X1), cz = CABIN_Z1 - LOG_R - 0.45f;
    const float hx = 0.45f, hz = 0.32f;
    kit_frame_box(kit, w, MAT_WOOD, cx - hx, cx + hx, TABLE_TOP - 0.04f, TABLE_TOP, cz - hz,
                  cz + hz, false);
    for (int i = 0; i < 4; i++) {
        const float x = (i & 1) ? cx + hx - 0.05f : cx - hx + 0.05f;
        const float z = (i & 2) ? cz + hz - 0.05f : cz - hz + 0.05f;
        kit_frame_box(kit, w, MAT_WOOD, x - 0.025f, x + 0.025f, CABIN_FLOOR_Y, TABLE_TOP - 0.04f,
                      z - 0.025f, z + 0.025f, false);
    }
    kit_frame_box(kit, w, KIT_COLLIDER_ONLY, cx - hx, cx + hx, CABIN_FLOOR_Y, TABLE_TOP, cz - hz,
                  cz + hz, true);
    // The place, from the side the chair is on: a plate with the meal half eaten on it, a mug, a
    // fork and a knife.
    const float pz = cz - 0.12f;
    kitchen_plates(kit, w, cx, pz, TABLE_TOP, 1);
    kit_frame_box(kit, w, MAT_PRUNE, cx - 0.05f, cx + 0.03f, TABLE_TOP + 0.008f, TABLE_TOP + 0.03f,
                  pz - 0.03f, pz + 0.04f, false);
    kit_frame_box(kit, w, MAT_CONTENTS_PALE, cx + 0.02f, cx + 0.07f, TABLE_TOP + 0.008f,
                  TABLE_TOP + 0.022f, pz - 0.05f, pz - 0.01f, false);
    kitchen_mug(kit, w, cx + 0.22f, pz + 0.08f, TABLE_TOP);
    kit_frame_box(kit, w, MAT_STAINLESS, cx - 0.17f, cx - 0.155f, TABLE_TOP, TABLE_TOP + 0.004f,
                  pz - 0.09f, pz + 0.09f, false);
    kit_frame_box(kit, w, MAT_STAINLESS, cx + 0.15f, cx + 0.162f, TABLE_TOP, TABLE_TOP + 0.003f,
                  pz - 0.1f, pz + 0.1f, false);
    // The chair, pushed back from the place and turned a little, as somebody got up.
    const KitFrame chair = {{cx + 0.08f, CABIN_FLOOR_Y, cz - hz - 0.42f}, 0.35f};
    kitchen_chair(kit, &chair);

    // The lamp: a glass font on a brass foot, the collar and the burner, the chimney over the
    // flame, and the wick lit.
    const float lx = cx - 0.27f, lz = cz + 0.12f;
    const vec2 font[] = {{0.0f, 0.0f},    {0.055f, 0.0f},  {0.05f, 0.012f}, {0.02f, 0.03f},
                         {0.02f, 0.045f}, {0.06f, 0.075f}, {0.062f, 0.11f}, {0.04f, 0.14f},
                         {0.02f, 0.15f},  {0.0f, 0.15f}};
    kit_frame_lathe(kit, w, MAT_GLASS_AMBER, lx, lz, TABLE_TOP, font, KIT_COUNT(font), 16);
    const vec2 collar[] = {
        {0.0f, 0.0f}, {0.03f, 0.0f}, {0.03f, 0.025f}, {0.024f, 0.04f}, {0.0f, 0.04f}};
    kit_frame_lathe(kit, w, MAT_BRASS, lx, lz, TABLE_TOP + 0.15f, collar, KIT_COUNT(collar), 12);
    const vec2 chimney[] = {
        {0.026f, 0.0f}, {0.038f, 0.05f}, {0.036f, 0.1f}, {0.022f, 0.16f}, {0.02f, 0.2f}};
    kit_frame_lathe(kit, w, MAT_GLASS_CLEAR, lx, lz, TABLE_TOP + 0.19f, chimney, KIT_COUNT(chimney),
                    16);
    kit_wick(kit, w, lx, TABLE_TOP + 0.205f, lz, 1.3f);
}

// The cot along the north wall: a plank frame on legs, a thin mattress, a grey blanket thrown
// back, and the pillow.
static void cot(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    const float x0 = CABIN_X0 + LOG_R + 0.05f, x1 = x0 + 1.95f;
    const float z0 = CABIN_Z0 + LOG_R + 0.04f, z1 = z0 + 0.8f, y = CABIN_FLOOR_Y + 0.4f;
    kit_frame_box(kit, w, MAT_WOOD, x0, x1, y - 0.12f, y, z0, z1, false);
    for (int i = 0; i < 4; i++) {
        const float x = (i & 1) ? x1 - 0.04f : x0 + 0.04f, z = (i & 2) ? z1 - 0.04f : z0 + 0.04f;
        kit_frame_box(kit, w, MAT_WOOD, x - 0.03f, x + 0.03f, CABIN_FLOOR_Y, y - 0.12f, z - 0.03f,
                      z + 0.03f, false);
    }
    kit_frame_box(kit, w, MAT_CUSHION, x0 + 0.03f, x1 - 0.03f, y, y + 0.1f, z0 + 0.03f, z1 - 0.03f,
                  false);
    kit_frame_box(kit, w, MAT_RUG, x0 + 0.55f, x1 - 0.02f, y + 0.1f, y + 0.13f, z0 + 0.01f,
                  z1 + 0.02f, false);
    kit_frame_box(kit, w, MAT_RUG, x0 + 0.42f, x0 + 0.6f, y + 0.1f, y + 0.17f, z0 + 0.02f,
                  z1 - 0.02f, false);
    kit_frame_box(kit, w, MAT_TOWEL, x0 + 0.06f, x0 + 0.38f, y + 0.1f, y + 0.19f, z0 + 0.12f,
                  z1 - 0.12f, false);
    kit_frame_box(kit, w, KIT_COLLIDER_ONLY, x0, x1, CABIN_FLOOR_Y, y + 0.1f, z0, z1, true);
}

// Two shelves on the south wall between the window and the fireplace: tins and jars of preserves.
static void shelves(Kit* kit, KitRng* rng) {
    // Facing into the room off the wall's logs: a runs along -x, d out of the wall toward -z.
    const KitFrame f = {{0.0f, 0.0f, CABIN_Z1 - LOG_R - 0.01f}, GLM_PIf};
    const float a0 = -(WIN_X1 + 0.35f), a1 = -(HEARTH_FACE - 0.1f);
    for (int s = 0; s < 2; s++) {
        const float y = CABIN_FLOOR_Y + 1.2f + 0.42f * (float)s;
        kit_frame_box(kit, &f, MAT_FENCE_BOARD, a1, a0, y - 0.03f, y, 0.0f, 0.24f, false);
        for (int b = 0; b < 2; b++) {
            const float a = b ? a1 - 0.08f : a0 + 0.08f;
            kit_frame_box(kit, &f, MAT_LAMP_POST, a - 0.012f, a + 0.012f, y - 0.16f, y - 0.03f,
                          0.0f, 0.18f, false);
        }
        kitchen_preserves(kit, &f, rng, fminf(a0, a1) + 0.05f, fmaxf(a0, a1) - 0.05f, y);
    }
}

// The survey map of the lake, tacked up on the front wall beside the door.
static void lake_map(Kit* kit) {
    // Facing into the room off the front wall's logs: a runs along -z, d along +x.
    const KitFrame f = {{0.0f, 0.0f, 0.0f}, 0.5f * GLM_PIf};
    const float w = CARDS[CARD_LAKE_MAP].size[0], h = CARDS[CARD_LAKE_MAP].size[1];
    const float a = -(DOOR_Z1 + 0.75f), y = CABIN_FLOOR_Y + 1.5f, d = CABIN_X0 + LOG_R + 0.012f;
    const float tilt = 0.03f, c = cosf(tilt), s = sinf(tilt);
    const vec3 across = {w * c, w * s, 0.0f}, up = {-h * s, h * c, 0.0f};
    const vec3 corner = {a - 0.5f * (across[0] + up[0]), y - 0.5f * (across[1] + up[1]), d};
    kit_frame_card(kit, &f, MAT_CARDS, corner, across, up, CARDS[CARD_LAKE_MAP].uv);
}

void cabin_build(Kit* kit, unsigned int seed) {
    KitRng rng = {seed * 2654435761u + 0x13410u};
    walls(kit);
    floor_and_ties(kit);
    roof(kit);
    fireplace(kit);
    porch(kit);
    woodpile(kit, &rng);
    stove(kit);
    table(kit);
    cot(kit);
    shelves(kit, &rng);
    lake_map(kit);
}

void cabin_light(FireSystem* fs, Scene* scene, bool shadows) {
    if (!fs)
        return;
    Fire* fire = fire_system_add(fs, FIRE_FLIPBOOK, "cabin_hearth");
    if (!fire)
        return;
    if (!fire_set_flipbook(fire, scene->tex_pool, "assets/textures/fire_hearth.json"))
        return;
    // Two cards on the grate out of step, the second a little smaller and further back.
    const float gx = HEARTH_FACE + 0.22f, gy = FIREBOX_Y0 + 0.05f;
    fire->cards.list[0] = (FireCard){{gx, gy, MID_Z}, {0.78f, 0.61f}, 0.0f};
    fire->cards.list[1] = (FireCard){{gx + 0.06f, gy, MID_Z - 0.12f}, {0.6f, 0.47f}, 0.5f};
    fire->cards.count = 2;
    fire->light_scale = HEARTH_LIGHT_SCALE;
    // Across the firebox's mouth, facing the room: kept, since the fire never moves.
    const LightDesc desc = {
        .name = "cabin_hearth",
        .type = LIGHT_AREA,
        .position = {HEARTH_FACE - 0.01f, 0.5f * (FIREBOX_Y0 + FIREBOX_Y1), MID_Z},
        .direction = {-1.0f, 0.0f, 0.0f},
        .up = {0.0f, 1.0f, 0.0f},
        .color = {1.0f, 0.6f, 0.3f},
        .size = {FIREBOX_Z1 - FIREBOX_Z0, FIREBOX_Y1 - FIREBOX_Y0},
        .range = 9.0f,
        .cast_shadows = shadows,
        .shadow_cache = shadows};
    Light* light = create_light(&desc);
    if (!light)
        return;
    scene_add_light(scene, light);
    fire->light = light;
}

void cabin_start_audio(AudioSystem* audio) {
    Sound* fire = sounds_loop(audio, "assets/audio/silent/hearth_fire.flac");
    if (fire) {
        audio_sound_set_position(fire, (vec3){HEARTH_FACE + 0.25f, FIREBOX_Y0 + 0.3f, MID_Z});
        audio_sound_set_volume(fire, HEARTH_VOLUME);
    }
    Sound* room = sounds_loop(audio, "assets/audio/silent/cabin_room.flac");
    if (room) {
        audio_sound_set_position(room,
                                 (vec3){0.5f * (CABIN_X0 + CABIN_X1), CABIN_FLOOR_Y + 1.5f, MID_Z});
        audio_sound_set_volume(room, ROOM_VOLUME);
    }
}

bool cabin_door(Door* door, Engine* engine, Scene* scene, EntityManager* em,
                PhysicsWorld* physics) {
    const KitOpening* opening = &WALLS[0].openings[0];
    // Turned a quarter, so a runs toward -z from the hinge at the south jamb and d into the room.
    const float x = CABIN_X0 + 0.5f * CORE + 0.5f * DOOR_THICK + 0.005f;
    const KitFrame hinge = {{x, 0.0f, opening->to}, 0.5f * GLM_PIf};
    return door_hang(door, engine, scene, em, physics, "cabin_door", door_leaf_battened, &hinge,
                     *opening, 1.55f);
}
