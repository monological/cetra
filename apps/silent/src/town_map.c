#include "layout.h"
#include "town_map.h"

#include "map_art.h"

const PlaceSpec PLACES[PLACE_COUNT] = {
    // The cross street's north arm, closed: found a few steps short of its barricade, anywhere
    // across the arm, and marked on the barricade's line.
    [PLACE_BARRICADE] = {"barricade",
                         MAP_TOWN,
                         {CROSS_X0 + 1.0f, -STREET_HALF_LEN + 1.0f, CROSS_NORTH_Z - 0.5f,
                          CROSS_NORTH_Z + 5.5f},
                         {CROSS_X, CROSS_NORTH_Z - 1.5f}},
    // The street straight on past the crossroads, where it breaks off at the chasm's lip: found
    // in the last few metres before the guard rail, and marked at the lip.
    [PLACE_ROAD_END] = {"road-end",
                        MAP_TOWN,
                        {CROSS_X0 - 0.5f, CROSS_X0 + 6.0f, -(STREET_HALF_WIDTH + 1.0f),
                         STREET_HALF_WIDTH + 1.0f},
                        {CROSS_X0 - 0.5f, 0.0f}},
    // The cabin on the lake's east shore: found from the track's last bend down onto its pad and
    // the bank, and marked where it stands.
    [PLACE_CABIN] = {"cabin",
                     MAP_TOWN,
                     {CABIN_PAD_X0 - 11.0f, CABIN_PAD_X1 + 2.0f, CABIN_PAD_Z0 - 12.0f,
                      CABIN_PAD_Z1 + 3.0f},
                     {0.5f * (CABIN_X0 + CABIN_X1), 0.5f * (CABIN_Z0 + CABIN_Z1)}},
};
