#ifndef _SILENT_TOWN_MAP_H_
#define _SILENT_TOWN_MAP_H_

/*
 * The town's map (spec 13.43): what the player finds that goes on it by hand -- the roads out of
 * town that cannot be followed, and the cabin down by the lake. Each PLACE is where the player's
 * feet find it, and where its mark goes on the map.
 */

typedef enum { PLACE_BARRICADE, PLACE_ROAD_END, PLACE_CABIN, PLACE_COUNT } PlaceId;

typedef struct PlaceSpec {
    const char* id; // as a command line and tools/make_map.py name it
    float box[4];   // where the feet find it, in plan: x0, x1, z0, z1
    float mark[2];  // where its mark goes on the map: world x, z
} PlaceSpec;

extern const PlaceSpec PLACES[PLACE_COUNT];

#endif // _SILENT_TOWN_MAP_H_
