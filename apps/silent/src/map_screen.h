#ifndef _SILENT_MAP_SCREEN_H_
#define _SILENT_MAP_SCREEN_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/texture.h"
#include "cetra/ui.h"

#include "town_map.h"

/*
 * The map screen (spec 13.43): a map the player carries, over whatever screen is under it. The
 * sheet is a VIEW with a pan and a zoom: at zoom 1 all of it fits, and zoomed in it moves, so a
 * larger map is a larger print and nothing here changes. Each mark due is written on stroke by
 * stroke the first time the map shows it, one after another, and a red arrow stands where the
 * player is, pointing the way they face. A modal screen in the HUD's UI system.
 */

typedef struct MapControls {
    float pan[2]; // -1..1 along the sheet's right and down, a full second's worth at 1
    float zoom;   // -1..1, out and in, a full second's worth at 1
    float wheel;  // the wheel's turn this frame, toward the reader positive
} MapControls;

typedef struct MapScreen {
    Engine* engine;
    UISystem* ui; // the HUD's, borrowed
    UIScreen* screen;
    UIElement* paper;
    ShaderProgram* ink;
    Texture* print[MAP_COUNT]; // NULL until the loader has it
    Texture* marks[MAP_COUNT];
    MapId map;   // the one last opened, MAP_NONE before any
    float zoom;  // 1: the whole sheet
    vec2 centre; // the print pixel the view is centred on, before it is held to the sheet
    bool dragging;
    float drag_from[2]; // the pointer, where the drag last moved the view
    double clock;       // seconds the map has been open this time
    // When on the clock each place's mark began to be written: -inf once written whole, +inf when
    // it is not written this time.
    double began[PLACE_COUNT];
    float feet[2]; // where the player stands: world x, z
    float heading; // the way they face: radians clockwise from north
} MapScreen;

// Its screen in `ui`, and its pictures asked of the engine's loader. A program that will not build
// is said once, and the map then shows no marks.
void map_screen_start(MapScreen* ms, UISystem* ui, Engine* engine, TexturePool* pool);
// Whether it is the screen on top.
bool map_screen_open(const MapScreen* ms);
// Up on `map`, whole and centred on the player, with every mark due and not yet written -- one
// `found`, or a place known from the first opening -- set to be written one after another. It
// goes by the UI's own back.
void map_screen_show(MapScreen* ms, MapId map, const bool found[PLACE_COUNT]);
// The paper sized to a window `width` x `height` points: each frame before the UI's pass.
void map_screen_layout(MapScreen* ms, float width, float height);
// The frame's pan and zoom while it is open, a drag by the pointer among them.
void map_screen_input(MapScreen* ms, const UIInput* in, const MapControls* controls, float dt);
// Per frame: where the player stands and faces, for the arrow, and the ink's clock.
void map_screen_update(MapScreen* ms, float dt, const vec3 feet, const vec3 forward);
void map_screen_free(MapScreen* ms);

#endif // _SILENT_MAP_SCREEN_H_
