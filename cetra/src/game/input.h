#ifndef _GAME_INPUT_H_
#define _GAME_INPUT_H_

/*
 * The game layer's input: keyboard, mouse and gamepads, polled once a frame
 * BEFORE the fixed steps, with the previous frame kept so a press and a
 * release are edges (spec 11.109).
 *
 * Because the poll is per frame and the sim step is not, an edge is per
 * FRAME: a frame that runs two fixed steps hands both the same press, and a
 * frame that runs none drops it. That is the contract every query here has
 * always had, stated once.
 *
 * A gamepad is read through a seam: one function fills GLFW's standard
 * layout for a slot, and the default asks GLFW. A second reader replays a
 * scripted pad from a text file, which is how everything above the seam is
 * verified with no controller in the room.
 */

#include <stdbool.h>
#include <stddef.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/types.h>

struct Engine;

#define GAME_MAX_PADS 4

// One gamepad in GLFW's standard layout: GLFW_GAMEPAD_BUTTON_* and
// GLFW_GAMEPAD_AXIS_* index these whatever the physical controller.
typedef struct GamePadState {
    bool connected;
    bool buttons[GLFW_GAMEPAD_BUTTON_LAST + 1];
    bool buttons_prev[GLFW_GAMEPAD_BUTTON_LAST + 1];
    // Sticks -1..1 after the radial dead zone, triggers 0..1 after theirs;
    // both rescaled so the edge of the zone reads 0 and full deflection 1.
    float axes[GLFW_GAMEPAD_AXIS_LAST + 1];
    float axes_prev[GLFW_GAMEPAD_AXIS_LAST + 1];
} GamePadState;

// Fills `out` for slot `pad` (0-based) and returns whether a pad is there.
// GLFW's trigger axes REST at -1 in this struct; a reader filling it by hand
// writes that, or an idle pad reads as half-pressed.
typedef bool (*GamepadReadFn)(void* ctx, int pad, GLFWgamepadstate* out);

/*
 * Actions. A game reads "jump" and "move_x", not a key code, and the table
 * that says which key, mouse button, pad button or pad axis each one is
 * lives in the game as data -- which is what a remapping screen would edit.
 *
 * Every action is ONE float in -1..1, from whichever of its sources is
 * largest in magnitude: a key or button contributes its scale, an axis its
 * value times its scale. Digital and analog are then one thing, and a stick
 * and a key pair are the same action rather than two code paths; a 2D move is
 * two actions read together.
 */
typedef enum InputSourceKind {
    INPUT_SRC_KEY = 0,      // code: GLFW_KEY_*; kind KEY with code 0 is an unused entry
    INPUT_SRC_MOUSE_BUTTON, // code: GLFW_MOUSE_BUTTON_*
    INPUT_SRC_PAD_BUTTON,   // code: GLFW_GAMEPAD_BUTTON_*, on pad 0
    INPUT_SRC_PAD_AXIS,     // code: GLFW_GAMEPAD_AXIS_*, on pad 0
} InputSourceKind;

typedef struct InputSource {
    InputSourceKind kind;
    int code;
    float scale; // What a held key or button reads, or an axis's multiplier; 0 = +1
} InputSource;

#define INPUT_ACTION_SOURCES 6
#define INPUT_MAX_ACTIONS    32

typedef struct InputAction {
    const char* name;
    InputSource sources[INPUT_ACTION_SOURCES]; // Unused entries zero
} InputAction;

typedef struct GameInputState {
    // ENGINE-OWNED: what the poll writes. Read freely, never write.
    bool keys[GLFW_KEY_LAST + 1];
    bool keys_prev[GLFW_KEY_LAST + 1];
    bool mouse_buttons[GLFW_MOUSE_BUTTON_LAST + 1];
    bool mouse_buttons_prev[GLFW_MOUSE_BUTTON_LAST + 1];
    double mouse_x; // Window coordinates, as GLFW reports them
    double mouse_y;
    double mouse_prev_x;
    double mouse_prev_y;
    double mouse_delta_x;
    double mouse_delta_y;
    double scroll_x; // This frame's wheel delta
    double scroll_y;
    bool shift_held; // Either shift, control, alt
    bool ctrl_held;
    bool alt_held;
    GamePadState pads[GAME_MAX_PADS];
    // Per bound action: this frame's value, and the value the devices'
    // PREVIOUS state gives -- so a pad's connect and disconnect rules reach an
    // action's edges the way they reach a button's.
    float action_values[INPUT_MAX_ACTIONS];
    float action_values_prev[INPUT_MAX_ACTIONS];
    struct Engine* engine; // Borrowed

    // BY FUNCTION: input_set_pad_reader, input_set_pad_script. The reader the
    // poll asks for each slot, with its context; the context is freed by
    // input_free when pad_ctx_free is set, which the script's is.
    GamepadReadFn pad_read;
    void* pad_ctx;
    void (*pad_ctx_free)(void* ctx);
    // BY FUNCTION: input_bind. The action table, borrowed for the life of the
    // binding, and how much of it is bound.
    const InputAction* actions;
    size_t action_count;

    // SETTINGS: plain stores. Write them directly, at any time.
    float stick_dead_zone;   // Radial, per stick; default 0.2
    float trigger_dead_zone; // Per trigger, on its 0..1 travel; default 0.1
} GameInputState;

// Registers with the engine's scroll and GLFW's joystick callbacks (this
// layer is the only installer of either, so a game reads scroll and pad
// presence through it). The engine must exist; call after create_engine.
void input_init(GameInputState* input, struct Engine* engine);
// Frees what the layer owns (a scripted pad's context); the struct itself is
// the caller's.
void input_free(GameInputState* input);

// Once a frame, before the fixed steps: polls every key, mouse button and pad
// slot, moves the wheel accumulator into this frame's delta, and keeps the
// previous frame for the edges.
void input_update(GameInputState* input);

// Keyboard queries
bool input_key_down(const GameInputState* input, int key);
bool input_key_pressed(const GameInputState* input, int key);
bool input_key_released(const GameInputState* input, int key);

// Mouse button queries
bool input_mouse_down(const GameInputState* input, int button);
bool input_mouse_pressed(const GameInputState* input, int button);
bool input_mouse_released(const GameInputState* input, int button);

// Get mouse position
void input_mouse_pos(const GameInputState* input, double* x, double* y);
void input_mouse_delta(const GameInputState* input, double* dx, double* dy);

// This frame's wheel delta. Scroll over a GUI panel reaches ImGui and not
// this, which is the engine's rule for every pointer event.
void input_scroll(const GameInputState* input, double* x, double* y);

// Gamepad queries, by slot 0..GAME_MAX_PADS-1 (slot 0 is GLFW's joystick 1).
// A disconnected slot reads as nothing held, every axis at rest.
bool input_pad_connected(const GameInputState* input, int pad);
bool input_pad_down(const GameInputState* input, int pad, int button);
bool input_pad_pressed(const GameInputState* input, int pad, int button);
bool input_pad_released(const GameInputState* input, int pad, int button);
float input_pad_axis(const GameInputState* input, int pad, int axis);
// The controller's name as the driver reports it, asked of GLFW each call
// (the mapping table can be reallocated under a cached pointer); NULL when
// nothing is there.
const char* input_pad_name(const GameInputState* input, int pad);

// Replace the reader the poll asks. NULL restores the default, which reads
// GLFW. `ctx` is borrowed.
void input_set_pad_reader(GameInputState* input, GamepadReadFn read, void* ctx);

// Replay a scripted pad on slot 0 from a text file: one line per inclusive
// frame range, `from-to` (or one frame) followed by the button names
// (a b x y lb rb back start guide lstick rstick dup dright ddown dleft),
// axes as lx= ly= rx= ry= in -1..1 and lt= rt= in 0..1, `idle`, or `off`
// for a disconnected pad; a frame no line covers is connected and idle; `#`
// starts a comment. A frame is one input_update. False, with the reason
// logged, for a file that is missing or will not parse -- a refusal the
// caller can act on, so a driver of the layer never mistakes an idle pad for
// a passing one.
bool input_set_pad_script(GameInputState* input, const char* path);

// Bind an action table. Borrowed: the table outlives the binding. At most
// INPUT_MAX_ACTIONS; more is refused whole, logged. The values are evaluated
// by input_update, so a table bound mid-frame reads on the next.
void input_bind(GameInputState* input, const InputAction* actions, size_t count);

// An action's value this frame, -1..1. A name the table does not have logs
// and reads 0, so a typo says so at once rather than playing as a dead key.
float input_action_value(const GameInputState* input, const char* name);
// The digital view of the same value: down is |value| > 0.5, and pressed and
// released are that threshold crossed this frame.
bool input_action_down(const GameInputState* input, const char* name);
bool input_action_pressed(const GameInputState* input, const char* name);
bool input_action_released(const GameInputState* input, const char* name);

// Two actions as a move direction on the ground plane, (x, 0, -y) so +y is
// forward, clamped to length 1 -- a diagonal on keys is not faster than a
// stick pushed straight -- with a magnitude below 1 kept, which is what a
// stick's is.
void input_action_move(const GameInputState* input, const char* x, const char* y, vec3 out);

#endif // _GAME_INPUT_H_
