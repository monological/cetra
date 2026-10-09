#ifndef _SILENT_MAP_SCREEN_H_
#define _SILENT_MAP_SCREEN_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/texture.h"
#include "cetra/ui.h"

#include "town_map.h"

/*
 * The map screen (spec 13.43): a map the player carries, opened by M over the world dimmed behind
 * it, or over the backpack's screen when opened from there. The sheet is a VIEW with a pan and a
 * zoom: at zoom 1 all of it fits, and zoomed in it moves by a drag, WASD or the stick, so a larger
 * map is a larger print and nothing here changes. Each found place's mark is written on stroke by
 * stroke the first time the map shows it, one after another, and a red arrow stands where the
 * player is, pointing the way they face. A modal screen in the HUD's UI system.
 */

typedef struct MapControls {
    float pan[2]; // -1..1 along the sheet's right and down, a full second's worth at 1
    float zoom;   // -1..1, out and in, a full second's worth at 1
    float wheel;  // the wheel's turn this frame, toward the reader positive
} MapControls;

typedef struct MapScreen {
    UISystem* ui; // the HUD's, borrowed
    UIScreen* screen;
    UIElement* paper;
    ShaderProgram* ink;
    Texture* print[MAP_COUNT];
    Texture* marks[MAP_COUNT];
    MapId map;     // the one open, or MAP_NONE
    bool over_bag; // opened from the backpack's screen, which closing it goes back to
    float zoom;    // 1: the whole sheet
    vec2 centre;   // the print pixel the view is centred on, before it is held to the sheet
    bool dragging;
    float drag_from[2];          // the pointer, where the drag last moved the view
    double clock;                // seconds the map has been open
    bool shown[PLACE_COUNT];     // its mark has been written on
    double writing[PLACE_COUNT]; // when on the clock its mark begins to be written; < 0, not now
    float feet[2];               // where the player stands: world x, z
    float heading;               // the way they face: radians clockwise from north
} MapScreen;

// False if there is no UI to show it in or its program will not build, which says why; M then
// shows nothing.
bool map_screen_start(MapScreen* ms, UISystem* ui, Engine* engine, TexturePool* pool);
// Whether it is the screen on top.
bool map_screen_open(const MapScreen* ms);
// Up on `map`, whole and centred on the player, with every mark `found` and not yet written set to
// be written one after another. It goes by the UI's own back.
void map_screen_show(MapScreen* ms, MapId map, bool over_bag, const bool found[PLACE_COUNT]);
// The paper sized to a window `width` x `height` points: each frame before the UI's pass.
void map_screen_layout(MapScreen* ms, float width, float height);
// The frame's pan and zoom while it is open, a drag by the pointer among them.
void map_screen_input(MapScreen* ms, const UIInput* in, const MapControls* controls, float dt);
// Per frame: where the player stands and faces, for the arrow, and the ink's clock.
void map_screen_update(MapScreen* ms, float dt, const vec3 feet, const vec3 forward);
void map_screen_free(MapScreen* ms);

#endif // _SILENT_MAP_SCREEN_H_
