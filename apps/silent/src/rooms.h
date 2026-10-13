#ifndef _SILENT_ROOMS_H_
#define _SILENT_ROOMS_H_

#include <cglm/cglm.h>

#include "cetra/aabb.h"

/*
 * The rooms of the town's houses (specs 13.33 and 13.49): each house over its footprint, the
 * home's living room, kitchen and basement, the mansion's upper storey, and the cabin, as boxes in
 * the world. A room later in the list carves itself out of one round it, so a point is in the last
 * room whose boxes hold it, and outdoors, ROOM_WORLD, in none.
 */
typedef enum {
    ROOM_WORLD, // the outdoors, outside every room
    ROOM_HOME,
    ROOM_LIVING,
    ROOM_KITCHEN,
    ROOM_BASEMENT,
    ROOM_MANSION,
    ROOM_UPSTAIRS,
    ROOM_CABIN,
    ROOMS
} RoomId;

#define ROOM_BOXES 8 // the most boxes a room is made of

// `room`'s boxes in the world into `out`: how many. None for ROOM_WORLD.
int room_boxes(RoomId room, AABB out[ROOM_BOXES]);
// Its name in code, "kitchen", and as a player reads it, "Home, kitchen".
const char* room_name(RoomId room);
const char* room_label(RoomId room);
// The room `at` is in.
RoomId room_at(const vec3 at);
// The house `room` is in -- ROOM_HOME, ROOM_MANSION or ROOM_CABIN -- or ROOM_WORLD outdoors.
RoomId room_house(RoomId room);

#endif // _SILENT_ROOMS_H_
