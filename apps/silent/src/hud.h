#ifndef _SILENT_HUD_H_
#define _SILENT_HUD_H_

#include <stdbool.h>

#include "cetra/engine.h"
#include "cetra/ui.h"

/*
 * What the game says on screen (specs 13.13 and 13.40), through the engine's UI layer after the
 * tonemap, so the grade and the fog never touch it. Two lines low in the middle: the PROMPT, what
 * pressing the action key would do here -- "E   Open door" -- while there is something to do,
 * and over it a THOUGHT, something the player thinks, which fades in, stays a few seconds and
 * fades out. Both are on one non-modal screen that never leaves the stack. The engine has one
 * overlay, so every screen the game shows -- the backpack's (backpack.h) -- is pushed over it in
 * this one UI system.
 */
typedef struct HudLine {
    UIElement* plate;
    UIElement* label;
    const float* fg; // its words' colour
    float alpha;     // what the plate and its words are drawn at
} HudLine;

typedef struct Hud {
    UISystem* ui;
    Font* font; // the face every screen in it is set in, the font pool's
    UIScreen* screen;
    HudLine prompt, thought;
    float thought_age;  // seconds since the thought was had
    float thought_stay; // and how long it stays before it fades
    const char* hint;   // the prompt while no other is given, borrowed
    float hint_left;    // seconds it has left
    UIElement* save_mark;
    struct ShaderProgram*
        candles;     // the save mark's candlestick, the engine's; NULL for words alone
    float saved_age; // seconds since the game was saved
} Hud;

// False, with the reason printed, if the font or the UI cannot be made; the game runs on without
// words.
bool hud_start(Hud* hud, Engine* engine);
// Show `text` as the prompt, or the hint, or nothing, for NULL. Cheap to call every frame with
// the same text.
void hud_prompt(Hud* hud, const char* text);
// `text` as the prompt for `seconds` while no other is given, from the next hud_prompt.
void hud_hint(Hud* hud, const char* text, float seconds);
// Think `text`, in place of any thought on screen; the same thought again stays up.
void hud_think(Hud* hud, const char* text);
// The same, staying `seconds` before it fades rather than a thought's usual few.
void hud_think_for(Hud* hud, const char* text, float seconds);
// The game was just saved (spec 13.49): in the window's bottom right corner a three-branched
// candlestick lights its candles one after another before "Saving...", then "Saved", and goes.
// Saved again while it is up, it says so again without going out first.
void hud_saved(Hud* hud);
// Nothing thought, hinted or saved on screen, as at the start (spec 13.50).
void hud_clear(Hud* hud);
// Per frame: the thought's fade, the hint's time and the save mark's, and the lines placed in a
// window `height` points tall.
void hud_update(Hud* hud, float dt, float height);
void hud_free(Hud* hud);

// A padding of nothing: a zero is the style's.
#define HUD_BARE 0.01f
// A container in `parent` that draws nothing and has no padding of its own, laying its children
// out along `dir` `spacing` apart.
UIElement* hud_bare(UIElement* parent, UIDir dir, float spacing);
// A modal screen `name` in `ui` over the world, which fades in and centres what is put in it with
// the world dimmed to `dim` behind it; its root in `root`.
UIScreen* hud_modal_screen(UISystem* ui, const char* name, float dim, UIElement** root);

// The menus' look: items as words on nothing, the one the keys are on brightened onto a dim plate.
extern const vec4 HUD_ITEM_FG;
extern const vec4 HUD_FOCUS_FG;
extern const vec4 HUD_HEADING_FG;
#define HUD_HEADING_SIZE 34.0f // points
#define HUD_ITEM_SIZE    26.0f
#define HUD_ROW_SIZE     22.0f
#define HUD_MENU_WIDTH   520.0f
#define HUD_LIST_DIM     0.55f // the world behind a screen with more to read
// A menu item in `parent`, its words `size` points, calling `action` with `user` when chosen.
UIElement* hud_item(UIElement* parent, const char* text, float size, UIActionFn action, void* user);
// An item that takes the screen on top of `ui` down, as back does: Resume, Back, No.
UIElement* hud_back_item(UIElement* parent, const char* text, float size, UISystem* ui);
// Exit to Desktop: the game ends, and what ending it saves is saved.
struct Game;
UIElement* hud_quit_item(UIElement* parent, float size, struct Game* game);
// A heading over a menu's items.
UIElement* hud_heading(UIElement* parent, const char* text);
// For an element drawn in the items' look by a draw of its own: the plate behind it as the keys
// come to it, `style` its words' look, and the rect inside its padding its words go in.
UIRect hud_item_look(const UIElement* el, UIDrawList* dl, UIStyle* style);
// `el` as a menu row of `size` points across its column, drawn by `draw` with `user`.
UIElement* hud_dress(UIElement* el, float size, UIDrawFn draw, void* user);
// Whether `screen` is on top of `ui`.
bool hud_on_top(const UISystem* ui, const UIScreen* screen);
// `screen` pushed over whatever is on top of `ui`, the keys on `focus`; nothing when it is on top.
void hud_show(UISystem* ui, UIScreen* screen, UIElement* focus);
// An item shown or not without leaving the tree: a hidden item takes no height and no key lands
// on it.
void hud_item_shown(UIElement* el, bool shown);
// A column `width` points wide in `root`, centring what is put in it.
UIElement* hud_column(UIElement* root, float width);
// Words in `parent` that are not an item: a heading, a note.
UIElement* hud_words(UIElement* parent, const char* text, float size, const float* fg,
                     float tracking);

#endif // _SILENT_HUD_H_
