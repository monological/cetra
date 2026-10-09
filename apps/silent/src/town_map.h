#ifndef _SILENT_TOWN_MAP_H_
#define _SILENT_TOWN_MAP_H_

/*
 * The maps the player can carry (spec 13.43), and what the player finds that goes on one by hand
 * -- the roads out of town that cannot be followed, and the cabin down by the lake. Each PLACE is
 * where the player's feet find it, and where its mark goes on its map.
 *
 * A map is an ID, so a second one is a row here and a run of a tool rather than new code: its
 * pictures, its frame and its marks are one MapArt, which tools/make_map.py generates into
 * map_art.h.
 */

typedef enum { MAP_TOWN, MAP_COUNT } MapId;

typedef enum { PLACE_BARRICADE, PLACE_ROAD_END, PLACE_CABIN, PLACE_COUNT } PlaceId;

typedef struct PlaceSpec {
    const char* id; // as a command line and tools/make_map.py name it
    MapId map;      // the map its mark goes on
    float box[4];   // where the feet find it, in plan: x0, x1, z0, z1
    float mark[2];  // where its mark goes on the map: world x, z
} PlaceSpec;

extern const PlaceSpec PLACES[PLACE_COUNT];

// A find's mark as the tool drew it: where it is in its map's marks picture, and the point in it
// that goes on the find's place.
typedef struct MapMark {
    PlaceId place;
    float uv[4];     // {u0, v0, u1, v1} in the marks picture, V down
    float size[2];   // print pixels
    float anchor[2]; // print pixels from the mark's top left to the point it marks
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
    float folded_front[4], folded_back[4]; // the folded map's faces in its texture set, V up
    unsigned int seed;                     // the street seed whose houses the print shows
} MapArt;

extern const MapArt MAP_ART[MAP_COUNT];

#endif // _SILENT_TOWN_MAP_H_
