#include <string.h>

#include "layout.h"
#include "town_map.h"

#include "map_art.h"

// How far the feet must go from a place before coming back to it is arriving again.
#define FIND_LEAVE 3.0f

const PlaceSpec PLACES[PLACE_COUNT] = {
    // The cross street's north arm, closed: found a few steps short of its barricade, anywhere
    // across the arm, and marked on the barricade's line.
    [PLACE_BARRICADE] = {"barricade",
                         MAP_TOWN,
                         {CROSS_X0 + 1.0f, -STREET_HALF_LEN + 1.0f, CROSS_NORTH_Z - 0.5f,
                          CROSS_NORTH_Z + 5.5f},
                         {CROSS_X, CROSS_NORTH_Z - 1.5f},
                         "Hmm, can't go that way. Let me mark that on the map.",
                         "Hmm, can't go that way.",
                         "Hmm, can't go that way."},
    // The street straight on past the crossroads, where it breaks off at the chasm's lip: found
    // in the last few metres before the guard rail, and marked at the lip.
    [PLACE_ROAD_END] = {"road-end",
                        MAP_TOWN,
                        {CROSS_X0 - 0.5f, CROSS_X0 + 6.0f, -(STREET_HALF_WIDTH + 1.0f),
                         STREET_HALF_WIDTH + 1.0f},
                        {CROSS_X0 - 0.5f, 0.0f},
                        "The road just ends. Can't go that way. Let me mark that on the map.",
                        "The road just ends. Can't go that way.",
                        "The road just ends. Can't go that way."},
    // The cabin on the lake's east shore: found from the track's last bend down onto its pad and
    // the bank, and marked where it stands. Said once.
    [PLACE_CABIN] = {"cabin",
                     MAP_TOWN,
                     {CABIN_PAD_X0 - 11.0f, CABIN_PAD_X1 + 2.0f, CABIN_PAD_Z0 - 12.0f,
                      CABIN_PAD_Z1 + 3.0f},
                     {0.5f * (CABIN_X0 + CABIN_X1), 0.5f * (CABIN_Z0 + CABIN_Z1)},
                     "There's a cabin here. Let me go check it out.",
                     "There's a cabin here. Let me go check it out.",
                     NULL},
};

PlaceId place_by_id(const char* id) {
    for (int i = 0; i < PLACE_COUNT; i++)
        if (id && !strcmp(PLACES[i].id, id))
            return (PlaceId)i;
    return PLACE_NONE;
}

PlaceId finds_arrive(Finds* f, const vec3 feet, bool* first) {
    PlaceId arrived = PLACE_NONE;
    for (int i = 0; i < PLACE_COUNT; i++) {
        const float* b = PLACES[i].box;
        const float d = plan_box_distance(feet[0], feet[2], b[0], b[1], b[2], b[3]);
        if (d <= 0.0f) {
            if (!f->here[i] && arrived == PLACE_NONE) {
                arrived = (PlaceId)i;
                *first = !f->found[i];
                f->found[i] = true;
            }
            f->here[i] = true;
        } else if (d > FIND_LEAVE) {
            f->here[i] = false;
        }
    }
    return arrived;
}
