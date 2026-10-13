#include "rooms.h"

#include "home.h"
#include "layout.h"
#include "tower.h"

// Over every roof and the tower's spire: where a house's rooms stop going up.
#define ROOFS_Y 20.0f

typedef struct Room {
    const char* name;
    const char* label;
    bool mansion; // its boxes are in the mansion's plan, moved to where it stands
    bool tower;   // and the tower's octagon with them, as high as the first box
    int box_count;
    AABB boxes[2];
} Room;

_Static_assert(2 + TOWER_BOXES <= ROOM_BOXES, "a room's boxes and the tower's fit ROOM_BOXES");

/*
 * The home over its footprint, which is its hall and everything shut; the living room and the
 * kitchen, each off the hall through an open doorway; and its basement -- the cellar under it all
 * and the flight down behind its door, so the door is the basement's boundary. The mansion over
 * its footprint and its tower, and its upper storey -- the rooms over the front and the study's
 * bay in the tower -- carved out of it; the gallery stays the great hall's, open to it.
 */
static const Room ROOM_TABLE[ROOMS] = {
    [ROOM_WORLD] = {"world", "Outside"},
    [ROOM_HOME] = {"home",
                   "Home",
                   false,
                   false,
                   1,
                   {{{HOUSE_OUT_X0, BASEMENT_Y, HOUSE_OUT_Z0},
                     {HOUSE_OUT_X1, ROOFS_Y, HOUSE_OUT_Z1}}}},
    [ROOM_LIVING] = {"living_room",
                     "Home, living room",
                     false,
                     false,
                     1,
                     {{{LIVING_IN_X0, SUBFLOOR_Y0, LIVING_IN_Z0},
                       {LIVING_IN_X1, HOME_CEIL_Y, LIVING_IN_Z1}}}},
    [ROOM_KITCHEN] = {"kitchen",
                      "Home, kitchen",
                      false,
                      false,
                      1,
                      {{{KITCHEN_X0, SUBFLOOR_Y0, KITCHEN_Z0},
                        {KITCHEN_X1, HOME_CEIL_Y, KITCHEN_Z1}}}},
    [ROOM_BASEMENT] = {"basement",
                       "Home, basement",
                       false,
                       false,
                       2,
                       {{{CELLAR_X0, BASEMENT_Y, CELLAR_Z0}, {CELLAR_X1, SUBFLOOR_Y0, CELLAR_Z1}},
                        {{CELLAR_X0, SUBFLOOR_Y0, STAIRWELL_Z0},
                         {HALL_OUT_X0, HOME_CEIL_Y, CELLAR_Z1}}}},
    [ROOM_MANSION] = {"mansion",
                      "Mansion",
                      true,
                      true,
                      1,
                      {{{HOUSE_OUT_X0, 0.0f, HOUSE_OUT_Z0},
                        {HOUSE_OUT_X1, ROOFS_Y, HOUSE_OUT_Z1}}}},
    [ROOM_UPSTAIRS] = {"mansion_up",
                       "Mansion, upstairs",
                       true,
                       true,
                       1,
                       {{{HOUSE_OUT_X0, CEIL_Y, HOUSE_OUT_Z0},
                         {HOUSE_OUT_X1, ROOFS_Y, KITCHEN_BACK_Z}}}},
    // The cabin by the lake (spec 13.41), its one room to the roof.
    [ROOM_CABIN] = {"cabin",
                    "Cabin",
                    false,
                    false,
                    1,
                    {{{CABIN_X0, CABIN_FLOOR_Y - 0.5f, CABIN_Z0},
                      {CABIN_X1, CABIN_RIDGE_Y + 0.5f, CABIN_Z1}}}},
};

int room_boxes(RoomId room, AABB out[ROOM_BOXES]) {
    if (room <= ROOM_WORLD || room >= ROOMS)
        return 0;
    const Room* r = &ROOM_TABLE[room];
    int n = 0;
    for (int i = 0; i < r->box_count; i++)
        out[n++] = r->boxes[i];
    if (r->tower)
        n += tower_boxes(r->boxes[0].min[1], r->boxes[0].max[1], &out[n]);
    for (int i = 0; r->mansion && i < n; i++) {
        mansion_at(out[i].min, out[i].min);
        mansion_at(out[i].max, out[i].max);
    }
    return n;
}

const char* room_name(RoomId room) {
    return ROOM_TABLE[room >= 0 && room < ROOMS ? room : ROOM_WORLD].name;
}

const char* room_label(RoomId room) {
    return ROOM_TABLE[room >= 0 && room < ROOMS ? room : ROOM_WORLD].label;
}

RoomId room_at(const vec3 at) {
    for (int r = ROOMS - 1; r > ROOM_WORLD; r--) {
        AABB boxes[ROOM_BOXES];
        const int n = room_boxes((RoomId)r, boxes);
        for (int i = 0; i < n; i++)
            if (aabb_dist_sq(&boxes[i], at) == 0.0f)
                return (RoomId)r;
    }
    return ROOM_WORLD;
}
