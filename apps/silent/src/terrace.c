#include <math.h>

#include "land.h"
#include "layout.h"
#include "mats.h"
#include "terrace.h"

/*
 * The far side's terrace (spec 13.35). Each lot is a level box of ground, a step higher than the
 * one east of it; a poured retaining wall holds them up along the back of the far sidewalk, its
 * coping stepping with them; and a straight flight climbs through a notch in the wall to each
 * lot's front door, between concrete cheeks, with a pipe rail up one side and a path on to the
 * door. At the street's ends the wall turns back along the lots' sides, the east one on up into
 * the woods behind until the slope has come up to meet it.
 *
 * Everything collides. The lots, the wall and the stairs are boxes; the stairs are the kit's.
 */

#define FLIGHT_HALF  0.7f  // half a flight's width between its cheeks
#define FLIGHT_RISE  0.18f // the most one riser may be; the flight divides its height evenly
#define FLIGHT_GOING 0.3f
#define CHEEK_THICK  0.2f
#define WALL_FOOT    0.15f // how far the wall runs down under the sidewalk
#define CAP_HEIGHT   0.12f // the coping along the wall's top
#define CAP_OVER     0.05f // how far it overhangs either face
#define RAIL_HEIGHT  0.9f  // the handrail's height above the nosings
#define RAIL_RADIUS  0.022f
#define PATH_HALF    0.55f
#define GROUND_DEEP  0.4f // a lot's ground box, below its top
// The east return follows the ground in pieces this long, up into the woods while the lots stand
// higher than the slope beside them.
#define RETURN_STEP 2.0f

// A length of the wall along the front, x0 to x1, holding up ground at `top`, its coping on it.
static void front_wall(Kit* kit, float x0, float x1, float top) {
    if (x1 - x0 < 0.01f)
        return;
    const float z1 = TERRACE_WALL_Z, z0 = z1 - TERRACE_WALL_THICK;
    kit_frame_box(kit, &KIT_WORLD, MAT_RETAINING, x0, x1, -WALL_FOOT, top, z0, z1, true);
    kit_frame_box(kit, &KIT_WORLD, MAT_RETAINING, x0, x1, top, top + CAP_HEIGHT, z0 - CAP_OVER,
                  z1 + CAP_OVER, true);
}

// A lot's ground: a level box from x0 to x1 and z0 to z1, its top at `top`.
static void lot_ground(Kit* kit, float x0, float x1, float z0, float z1, float top) {
    if (x1 - x0 < 0.01f || z1 - z0 < 0.01f)
        return;
    kit_frame_box(kit, &KIT_WORLD, MAT_DIRT, x0, x1, top - GROUND_DEEP, top, z0, z1, true);
}

/*
 * The flight up to lot height `top` at x = sx: the notch in the wall it climbs through, its cheeks
 * standing the notch's sides, the steps, the rail, and the path on to the door. Returns the z the
 * flight lands at.
 */
static float stair(Kit* kit, float sx, float top) {
    const int risers = (int)ceilf(top / FLIGHT_RISE);
    const float rise = top / (float)risers;
    const float run = (float)(risers - 1) * FLIGHT_GOING;
    // Climbing away from the street, toward -z: a frame turned half round, its d running inward
    // from the wall's face.
    const KitFrame f = {{sx, 0.0f, TERRACE_WALL_Z}, GLM_PIf};
    kit_frame_stair(kit, &f, MAT_CONCRETE, -FLIGHT_HALF, FLIGHT_HALF, 0.0f, 0.0f, rise,
                    FLIGHT_GOING, risers);
    // The cheeks: the wall's own pour carried in along both sides of the flight, its coping too.
    for (int s = -1; s <= 1; s += 2) {
        const float a0 = (float)s * FLIGHT_HALF, a1 = (float)s * (FLIGHT_HALF + CHEEK_THICK);
        kit_frame_box(kit, &f, MAT_RETAINING, a0, a1, -WALL_FOOT, top, 0.0f, run, true);
        kit_frame_box(kit, &f, MAT_RETAINING, a0, a1, top, top + CAP_HEIGHT, 0.0f, run, true);
    }
    // The rail up the west side, a short level stretch at either end, on posts at its ends.
    const float ra = FLIGHT_HALF - 0.12f;
    const vec3 path[4] = {{ra, RAIL_HEIGHT + rise, -0.25f},
                          {ra, RAIL_HEIGHT + rise, 0.1f},
                          {ra, RAIL_HEIGHT + top, run - 0.1f},
                          {ra, RAIL_HEIGHT + top, run + 0.25f}};
    kit_frame_pipe(kit, &f, MAT_LAMP_POST, path, 4, RAIL_RADIUS, 8);
    kit_frame_prism(kit, &f, MAT_LAMP_POST, ra, 0.05f, 0.0f, RAIL_HEIGHT + rise, RAIL_RADIUS, 6);
    kit_frame_prism(kit, &f, MAT_LAMP_POST, ra, run + 0.2f, top, RAIL_HEIGHT + top, RAIL_RADIUS, 6);
    return TERRACE_WALL_Z - run;
}

// The east return, along x = STREET_HALF_LEN from the front wall back into the woods: its top
// with the higher ground to its west, its foot under the lower ground east of it, while the two
// differ.
static void east_return(Kit* kit) {
    const float x0 = STREET_HALF_LEN, x1 = x0 + TERRACE_WALL_THICK;
    for (float z = TERRACE_WALL_Z; z > WORLD_Z0; z -= RETURN_STEP) {
        const float zn = z - RETURN_STEP, zm = 0.5f * (z + zn);
        const float high = land_height(x0 - 0.05f, zm), low = land_height(x1 + 0.05f, zm);
        if (high - low < 0.15f)
            break;
        kit_frame_box(kit, &KIT_WORLD, MAT_RETAINING, x0, x1, low - WALL_FOOT - 0.3f, high, zn, z,
                      true);
        kit_frame_box(kit, &KIT_WORLD, MAT_RETAINING, x0 - CAP_OVER, x1 + CAP_OVER, high,
                      high + CAP_HEIGHT, zn, z, true);
    }
}

void terrace_build(Kit* kit, const float door_x[TERRACE_LOTS]) {
    const float wall_back = TERRACE_WALL_Z - TERRACE_WALL_THICK;
    const float notch = FLIGHT_HALF + CHEEK_THICK;
    for (int lot = 0; lot < TERRACE_LOTS; lot++) {
        float x0, x1;
        land_terrace_lot_span(lot, &x0, &x1);
        const float top = land_terrace_height(0.5f * (x0 + x1));
        // The flight stands square to its door, inside the lot.
        const float sx = fminf(fmaxf(door_x[lot], x0 + notch + 0.3f), x1 - notch - 0.3f);
        const float landing = stair(kit, sx, top);

        // The lot's ground round the notch: either side of it, and behind the flight.
        lot_ground(kit, x0, sx - notch, TERRACE_BACK_Z, wall_back, top);
        lot_ground(kit, sx + notch, x1, TERRACE_BACK_Z, wall_back, top);
        lot_ground(kit, sx - notch, sx + notch, TERRACE_BACK_Z, landing, top);
        // The wall either side of the notch.
        front_wall(kit, x0, sx - notch, top);
        front_wall(kit, sx + notch, x1, top);
        // The path from the flight's head to the door.
        kit_frame_box(kit, &KIT_WORLD, MAT_CONCRETE, sx - PATH_HALF, sx + PATH_HALF, top,
                      top + 0.02f, FAR_HOUSE_FRONT_Z, landing, false);
    }

    // The west return, facing the cross street the street's west end will meet, the lot's height
    // all the way back.
    const float west = land_terrace_height(-STREET_HALF_LEN);
    const float wx1 = -STREET_HALF_LEN, wx0 = wx1 - TERRACE_WALL_THICK;
    kit_frame_box(kit, &KIT_WORLD, MAT_RETAINING, wx0, wx1, -WALL_FOOT, west, TERRACE_BACK_Z,
                  TERRACE_WALL_Z, true);
    kit_frame_box(kit, &KIT_WORLD, MAT_RETAINING, wx0 - CAP_OVER, wx1 + CAP_OVER, west,
                  west + CAP_HEIGHT, TERRACE_BACK_Z, TERRACE_WALL_Z + CAP_OVER, true);
    east_return(kit);
}
