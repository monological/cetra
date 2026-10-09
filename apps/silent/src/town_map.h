#ifndef _SILENT_TOWN_MAP_H_
#define _SILENT_TOWN_MAP_H_

#include <stdbool.h>
#include <cglm/cglm.h>

/*
 * The maps the player can carry (spec 13.43), and what the player finds that goes on one by hand
 * -- their own house, the roads out of town that cannot be followed, and the cabin down by the
 * lake. Each PLACE is where the player's feet find it, and where its mark goes on its map; marks
 * are written in the table's order.
 *
 * A map is an ID, so a second one is a row here and a run of a tool rather than new code: its
 * pictures, its frame and its marks are one MapArt, which tools/make_map.py generates into
 * map_art.h.
 */

typedef enum { MAP_NONE = -1, MAP_TOWN, MAP_COUNT } MapId;

typedef enum {
    PLACE_NONE = -1,
    PLACE_HOME,
    PLACE_BARRICADE,
    PLACE_ROAD_END,
    PLACE_CABIN,
    PLACE_COUNT
} PlaceId;

typedef struct PlaceSpec {
    const char* id; // as a command line and tools/make_map.py name it
    MapId map;      // the map its mark goes on
    float box[4];   // where the feet find it, in plan: x0, x1, z0, z1
    float mark[2];  // where its mark goes on the map: world x, z
    // What the player thinks on arriving: the first time with the map, which they mark it on; the
    // first time without; and every time after, NULL for nothing.
    const char* found_mapped;
    const char* found;
    const char* again;
    bool known; // found when its map is had, never walked into; its box is not read
} PlaceSpec;

extern const PlaceSpec PLACES[PLACE_COUNT];

// The place a command line names, or PLACE_NONE.
PlaceId place_by_id(const char* id);

// What the player has found, and where their feet are among the places.
typedef struct Finds {
    bool found[PLACE_COUNT];
    bool here[PLACE_COUNT]; // in its box, and not since out of it by FIND_LEAVE
} Finds;

// Per fixed step, with where the player's feet are: the place they come into on this step, or
// PLACE_NONE -- once each time they come back to it from FIND_LEAVE away -- and in `first`
// whether it is the first time. It is found from then on.
PlaceId finds_arrive(Finds* f, const vec3 feet, bool* first);
// The player has `map`: every place on it they know without going there is found.
void finds_have_map(Finds* f, MapId map);

// A find's mark as the tool drew it: where it is in its map's marks picture, the point in it that
// goes on the find's place, and how long it takes to write on. The marks picture holds the ink in
// alpha and, in red, when the pen reached each pixel as a fraction of `seconds`.
typedef struct MapMark {
    PlaceId place;
    float uv[4];     // {u0, v0, u1, v1} in the marks picture, V down
    float size[2];   // print pixels
    float anchor[2]; // print pixels from the mark's top left to the point it marks
    float seconds;
} MapMark;

// One map's art. The print and the marks are UI pictures, top row first; a world point (x, z)
// lands on the print at `at + (p - origin) * px_per_m`, x east to the right and z south down.
typedef struct MapArt {
    const char* print_file;
    const char* marks_file;
    float print_size[2]; // pixels
    float marks_size[2];
    float origin[2]; // world x, z
    float at[2];     // the print pixel the origin lands on
    float px_per_m;
    // The arrow where the player stands: `arrow_frames` turns, clockwise from north, frame k at
    // 2 pi k / arrow_frames, in square cells `arrow_cell` pixels a side, `arrow_cols` to a row from
    // the marks picture's top left; each frame's point is its cell's middle.
    int arrow_frames, arrow_cell, arrow_cols;
    int mark_count;
    MapMark marks[PLACE_COUNT];
    // The folded map's faces in its texture set, V up: its printed cover, and the panel that shows
    // when it hangs half open.
    float folded_cover[4], folded_inside[4];
    unsigned int seed; // the street seed whose houses the print shows
} MapArt;

extern const MapArt MAP_ART[MAP_COUNT];

#endif // _SILENT_TOWN_MAP_H_
