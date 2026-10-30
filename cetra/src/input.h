#ifndef _INPUT_H_
#define _INPUT_H_

#include <stdbool.h>
#include <stdint.h>
#include <cglm/types.h>

struct SceneNode;

// GLFW's key codes run up to GLFW_KEY_LAST, 348; engine.c holds the two together.
#define INPUT_KEYS 349
// Typing events held between two polls; more in one batch are dropped, and said once.
#define INPUT_KEY_EVENTS 64

// The platform's modifiers, as bits of InputKeyEvent.mods, for a shortcut (select all, copy, a
// line's end) and for a word at a time: Cmd and Option on macOS, Ctrl for both elsewhere. The text
// field, the input script's `mod` and an app's own shortcuts read these, so the three agree.
// GLFW's values, which engine.c holds them to.
#ifdef __APPLE__
#define INPUT_MOD_SHORTCUT 0x0008 // GLFW_MOD_SUPER
#define INPUT_MOD_WORD     0x0004 // GLFW_MOD_ALT
#else
#define INPUT_MOD_SHORTCUT 0x0002 // GLFW_MOD_CONTROL
#define INPUT_MOD_WORD     0x0002 // GLFW_MOD_CONTROL
#endif

// One thing typed: a key pressed or repeated (`key` a GLFW key code, `mods` its GLFW modifier
// bits, `codepoint` 0), or a character the keyboard produced (`codepoint`, `key` 0). A press of
// a letter brings both, the key first, as GLFW sends them.
typedef struct InputKeyEvent {
    int key;
    int mods;
    uint32_t codepoint;
} InputKeyEvent;

// The engine's own reading of the pointer, written by its GLFW callbacks
// before the app's are called. Positions are FRAMEBUFFER pixels with +Y up,
// which is the space the app callbacks receive and engine_mouse_to_drag_plane
// takes.
//
// ENGINE-OWNED throughout, with one exception: an app may clear selected_node
// on a press to keep the engine's drag off a node it handles itself.
typedef struct InputState {
    bool is_dragging; // The left button is down
    // The cursor as it last moved, the script's included; off the window until it first does.
    float pointer_fb_x;
    float pointer_fb_y;
    // The left button since the previous frame ended, on the wheel's schedule: a click that went
    // down and up between two polls is both. A press the debug GUI takes is not recorded.
    bool pointer_pressed;
    bool pointer_released;
    int press_mods;    // GLFW's modifier bits held at the last press
    float center_fb_x; // Where the press landed
    float center_fb_y;
    float drag_fb_x; // The cursor's offset from the press, not a per-frame delta
    float drag_fb_y;

    struct SceneNode* selected_node; // The node the press picked, or NULL; borrowed
    vec3 drag_start_world_pos;       // The pick's hit point, world space
    vec3 drag_object_start_pos;      // The picked node's world position at the press
    float drag_plane_distance;       // Eye to hit point: the plane a drag moves in

    // The wheel since the previous frame ended: what the poll at a frame's end
    // delivered, so the next frame's update hook reads one frame's delta.
    // Scroll over a GUI panel reaches ImGui and not this.
    double scroll_dx;
    double scroll_dy;
    // The keys pressed since the previous frame ended, by GLFW key code, on the wheel's schedule:
    // a tap that went down and up between two polls of the keys is still a press. Keys a GUI
    // field takes reach ImGui and not this.
    bool key_pressed[INPUT_KEYS];
    // What was typed since the previous frame ended, on the same schedule, in the order it came:
    // order is the point, since a character then Backspace is not Backspace then the character.
    // Presses and repeats both, which is what a text field needs and key_pressed is not.
    InputKeyEvent key_events[INPUT_KEY_EVENTS];
    int key_event_count;
} InputState;

void init_input_state(InputState* state);

#endif // _INPUT_H_
