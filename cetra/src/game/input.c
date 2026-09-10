#include "input.h"
#include "../engine.h"
#include "../util.h"
#include "../ext/log.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cglm/cglm.h>

// The log for a pad arriving or leaving. Presence itself is tracked by the
// poll, so a pad present before this was installed (GLFW does not fire it for
// those) is still found.
static void _joystick_callback(int jid, int event) {
    int slot = jid - GLFW_JOYSTICK_1;
    if (slot < 0 || slot >= GAME_MAX_PADS)
        return;
    if (event == GLFW_CONNECTED) {
        // The joystick name, not the gamepad name: the latter is NULL for a
        // pad GLFW has no mapping for, which is the pad worth naming.
        const char* name = glfwGetJoystickName(jid);
        log_info("gamepad %d connected: %s%s", slot, name ? name : "unnamed",
                 glfwJoystickIsGamepad(jid) ? "" : " (no mapping)");
    } else if (event == GLFW_DISCONNECTED) {
        log_info("gamepad %d disconnected", slot);
    }
}

static bool _pad_read_glfw(void* ctx, int pad, GLFWgamepadstate* out) {
    (void)ctx;
    return glfwGetGamepadState(GLFW_JOYSTICK_1 + pad, out) == GLFW_TRUE;
}

void input_init(GameInputState* input, Engine* engine) {
    if (!input || !engine) {
        log_error("input_init: NULL input or engine");
        return;
    }
    memset(input, 0, sizeof(GameInputState));
    input->engine = engine;
    input->pad_read = _pad_read_glfw;
    input->stick_dead_zone = 0.2f;
    input->trigger_dead_zone = 0.1f;

    glfwGetCursorPos(engine->window, &input->mouse_x, &input->mouse_y);
    input->mouse_prev_x = input->mouse_x;
    input->mouse_prev_y = input->mouse_y;

    glfwSetJoystickCallback(_joystick_callback);
}

void input_free(GameInputState* input) {
    if (!input)
        return;
    input_set_pad_reader(input, NULL, NULL, NULL);
}

bool input_load_gamepad_mappings(const char* path) {
    if (!path) {
        log_error("input_load_gamepad_mappings: NULL path");
        return false;
    }
    char* text = read_entire_file(path, NULL);
    if (!text) {
        log_error("gamepad mappings '%s': cannot read", path);
        return false;
    }
    // GLFW re-resolves every connected pad against the new table, so this is
    // good at any time after the engine exists, not only before a pad appears.
    bool ok = glfwUpdateGamepadMappings(text) == GLFW_TRUE;
    free(text);
    if (ok)
        log_info("gamepad mappings loaded from '%s'", path);
    else
        log_error("gamepad mappings '%s': refused by GLFW", path);
    return ok;
}

// A stick's two axes through one radial dead zone, rescaled so the zone's edge
// reads 0 and full deflection 1; a per-axis zone would make a diagonal push
// register before a straight one.
static void _stick(const float* raw, float dead, float* out) {
    float r = sqrtf(raw[0] * raw[0] + raw[1] * raw[1]);
    if (r <= dead || dead >= 1.0f) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        return;
    }
    float scale = ((glm_min(r, 1.0f) - dead) / (1.0f - dead)) / r;
    out[0] = raw[0] * scale;
    out[1] = raw[1] * scale;
}

// GLFW's trigger rests at -1 and reads 1 fully pulled; 0..1 travel, then the
// zone, rescaled the same way.
static float _trigger(float raw, float dead) {
    float t = (raw + 1.0f) * 0.5f;
    if (t <= dead || dead >= 1.0f)
        return 0.0f;
    return (glm_min(t, 1.0f) - dead) / (1.0f - dead);
}

static void _poll_pad(GameInputState* input, int slot) {
    GamePadState* p = &input->now.pads[slot];
    GLFWgamepadstate raw;
    memset(&raw, 0, sizeof(raw));
    if (!input->pad_read(input->pad_ctx, slot, &raw)) {
        // Everything it held is released, one edge each, as a real release
        // would be; every axis at rest.
        memset(p, 0, sizeof(*p));
        return;
    }
    p->connected = true;
    for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; b++)
        p->buttons[b] = raw.buttons[b] == GLFW_PRESS;
    _stick(&raw.axes[GLFW_GAMEPAD_AXIS_LEFT_X], input->stick_dead_zone,
           &p->axes[GLFW_GAMEPAD_AXIS_LEFT_X]);
    _stick(&raw.axes[GLFW_GAMEPAD_AXIS_RIGHT_X], input->stick_dead_zone,
           &p->axes[GLFW_GAMEPAD_AXIS_RIGHT_X]);
    p->axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] =
        _trigger(raw.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER], input->trigger_dead_zone);
    p->axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] =
        _trigger(raw.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER], input->trigger_dead_zone);
    // A button already held, or a stick already pushed, when the pad appears
    // is not a press.
    if (!input->prev.pads[slot].connected)
        input->prev.pads[slot] = *p;
}

void input_update(GameInputState* input) {
    if (!input || !input->engine || !input->engine->window)
        return;
    GLFWwindow* window = input->engine->window;

    input->prev = input->now;
    input->mouse_prev_x = input->mouse_x;
    input->mouse_prev_y = input->mouse_y;

    // From space, the first code GLFW defines: a lower one is an invalid enum
    // it reports to the error callback, thirty-two times a frame.
    for (int key = GLFW_KEY_SPACE; key <= GLFW_KEY_LAST; key++)
        input->now.keys[key] = glfwGetKey(window, key) == GLFW_PRESS;
    for (int button = 0; button <= GLFW_MOUSE_BUTTON_LAST; button++)
        input->now.mouse_buttons[button] = glfwGetMouseButton(window, button) == GLFW_PRESS;

    glfwGetCursorPos(window, &input->mouse_x, &input->mouse_y);
    input->mouse_delta_x = input->mouse_x - input->mouse_prev_x;
    input->mouse_delta_y = input->mouse_y - input->mouse_prev_y;

    input->scroll_x = input->engine->input.scroll_dx;
    input->scroll_y = input->engine->input.scroll_dy;

    for (int slot = 0; slot < GAME_MAX_PADS; slot++)
        _poll_pad(input, slot);
}

bool input_key_down(const GameInputState* input, int key) {
    if (key < 0 || key > GLFW_KEY_LAST)
        return false;
    return input->now.keys[key];
}

bool input_key_pressed(const GameInputState* input, int key) {
    if (key < 0 || key > GLFW_KEY_LAST)
        return false;
    return input->now.keys[key] && !input->prev.keys[key];
}

bool input_key_released(const GameInputState* input, int key) {
    if (key < 0 || key > GLFW_KEY_LAST)
        return false;
    return !input->now.keys[key] && input->prev.keys[key];
}

bool input_mouse_down(const GameInputState* input, int button) {
    if (button < 0 || button > GLFW_MOUSE_BUTTON_LAST)
        return false;
    return input->now.mouse_buttons[button];
}

bool input_mouse_pressed(const GameInputState* input, int button) {
    if (button < 0 || button > GLFW_MOUSE_BUTTON_LAST)
        return false;
    return input->now.mouse_buttons[button] && !input->prev.mouse_buttons[button];
}

bool input_mouse_released(const GameInputState* input, int button) {
    if (button < 0 || button > GLFW_MOUSE_BUTTON_LAST)
        return false;
    return !input->now.mouse_buttons[button] && input->prev.mouse_buttons[button];
}

void input_mouse_pos(const GameInputState* input, double* x, double* y) {
    if (x)
        *x = input->mouse_x;
    if (y)
        *y = input->mouse_y;
}

void input_mouse_delta(const GameInputState* input, double* dx, double* dy) {
    if (dx)
        *dx = input->mouse_delta_x;
    if (dy)
        *dy = input->mouse_delta_y;
}

void input_scroll(const GameInputState* input, double* x, double* y) {
    if (x)
        *x = input->scroll_x;
    if (y)
        *y = input->scroll_y;
}

static bool _pad_button_ok(const GameInputState* input, int pad, int button) {
    return input && pad >= 0 && pad < GAME_MAX_PADS && button >= 0 &&
           button <= GLFW_GAMEPAD_BUTTON_LAST;
}

bool input_pad_connected(const GameInputState* input, int pad) {
    return input && pad >= 0 && pad < GAME_MAX_PADS && input->now.pads[pad].connected;
}

bool input_pad_down(const GameInputState* input, int pad, int button) {
    return _pad_button_ok(input, pad, button) && input->now.pads[pad].buttons[button];
}

bool input_pad_pressed(const GameInputState* input, int pad, int button) {
    return _pad_button_ok(input, pad, button) && input->now.pads[pad].buttons[button] &&
           !input->prev.pads[pad].buttons[button];
}

bool input_pad_released(const GameInputState* input, int pad, int button) {
    return _pad_button_ok(input, pad, button) && !input->now.pads[pad].buttons[button] &&
           input->prev.pads[pad].buttons[button];
}

float input_pad_axis(const GameInputState* input, int pad, int axis) {
    if (!input || pad < 0 || pad >= GAME_MAX_PADS || axis < 0 || axis > GLFW_GAMEPAD_AXIS_LAST)
        return 0.0f;
    return input->now.pads[pad].axes[axis];
}

const char* input_pad_name(const GameInputState* input, int pad) {
    if (!input_pad_connected(input, pad))
        return NULL;
    return glfwGetJoystickName(GLFW_JOYSTICK_1 + pad);
}

void input_set_pad_reader(GameInputState* input, GamepadReadFn read, void* ctx,
                          void (*ctx_free)(void* ctx)) {
    if (!input) {
        log_error("input_set_pad_reader: NULL input");
        return;
    }
    if (input->pad_ctx_free)
        input->pad_ctx_free(input->pad_ctx);
    input->pad_read = read ? read : _pad_read_glfw;
    input->pad_ctx = read ? ctx : NULL;
    input->pad_ctx_free = read ? ctx_free : NULL;
}

/*
 * The scripted pad.
 */

typedef struct PadScriptRange {
    int from;
    int to;
    bool off;
    GLFWgamepadstate state;
} PadScriptRange;

typedef struct PadScript {
    PadScriptRange* ranges;
    size_t count;
    size_t cap;
    int frame; // The next input_update's frame number
} PadScript;

static const char* const k_button_names[GLFW_GAMEPAD_BUTTON_LAST + 1] = {
    "a",     "b",      "x",      "y",   "lb",     "rb",    "back",  "start",
    "guide", "lstick", "rstick", "dup", "dright", "ddown", "dleft",
};
static const char* const k_axis_names[GLFW_GAMEPAD_AXIS_LAST + 1] = {
    "lx", "ly", "rx", "ry", "lt", "rt",
};

// Connected and idle, in GLFW's form: the triggers rest at -1
static const PadScriptRange k_range_idle = {
    .state.axes = {0.0f, 0.0f, 0.0f, 0.0f, -1.0f, -1.0f},
};

// One token of a script line into the range; false with the reason logged.
static bool _range_token(PadScriptRange* r, const char* tok, const char* path, int line) {
    if (strcmp(tok, "idle") == 0)
        return true;
    if (strcmp(tok, "off") == 0) {
        r->off = true;
        return true;
    }
    const char* eq = strchr(tok, '=');
    if (eq) {
        size_t n = (size_t)(eq - tok);
        for (int a = 0; a <= GLFW_GAMEPAD_AXIS_LAST; a++) {
            if (strlen(k_axis_names[a]) == n && strncmp(tok, k_axis_names[a], n) == 0) {
                char* end = NULL;
                float v = strtof(eq + 1, &end);
                if (end == eq + 1 || *end != '\0') {
                    log_error("%s:%d: axis '%s' has no number", path, line, tok);
                    return false;
                }
                bool trigger =
                    a == GLFW_GAMEPAD_AXIS_LEFT_TRIGGER || a == GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER;
                if (trigger && (v < 0.0f || v > 1.0f)) {
                    log_error("%s:%d: trigger '%s' is 0..1", path, line, tok);
                    return false;
                }
                if (!trigger && (v < -1.0f || v > 1.0f)) {
                    log_error("%s:%d: axis '%s' is -1..1", path, line, tok);
                    return false;
                }
                r->state.axes[a] = trigger ? v * 2.0f - 1.0f : v;
                return true;
            }
        }
        log_error("%s:%d: unknown axis '%s'", path, line, tok);
        return false;
    }
    for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; b++) {
        if (strcmp(tok, k_button_names[b]) == 0) {
            r->state.buttons[b] = GLFW_PRESS;
            return true;
        }
    }
    log_error("%s:%d: unknown token '%s'", path, line, tok);
    return false;
}

// The next whitespace-delimited token of a line, NUL-terminated in place;
// NULL at the end. A hand lexer, as lut.c's is: strtok's reentrant form is
// strtok_r on two of the three platforms and strtok_s on the third.
static char* _next_token(char** cursor) {
    char* p = *cursor;
    while (*p == ' ' || *p == '\t' || *p == '\r')
        p++;
    if (*p == '\0')
        return NULL;
    char* start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r')
        p++;
    if (*p)
        *p++ = '\0';
    *cursor = p;
    return start;
}

static bool _script_parse(PadScript* script, char* text, const char* path) {
    int line = 0;
    char* ln = text;
    while (ln && *ln) {
        char* next = strchr(ln, '\n');
        if (next)
            *next++ = '\0';
        line++;
        char* hash = strchr(ln, '#');
        if (hash)
            *hash = '\0';
        char* p = ln;
        while (isspace((unsigned char)*p))
            p++;
        if (*p == '\0') {
            ln = next;
            continue;
        }

        PadScriptRange r = k_range_idle;
        char* end = NULL;
        long from = strtol(p, &end, 10);
        long to = from;
        if (end == p || from < 0) {
            log_error("%s:%d: a line starts with a frame or a range", path, line);
            return false;
        }
        p = end;
        if (*p == '-') {
            to = strtol(p + 1, &end, 10);
            if (end == p + 1 || to < from) {
                log_error("%s:%d: bad range", path, line);
                return false;
            }
            p = end;
        }
        r.from = (int)from;
        r.to = (int)to;

        for (const char* tok = _next_token(&p); tok; tok = _next_token(&p)) {
            if (!_range_token(&r, tok, path, line))
                return false;
        }

        if (!grow_array((void**)&script->ranges, &script->cap, script->count + 1, sizeof(r), 8))
            return false;
        script->ranges[script->count++] = r;
        ln = next;
    }
    return true;
}

static bool _pad_read_script(void* ctx, int pad, GLFWgamepadstate* out) {
    PadScript* script = ctx;
    if (pad != 0)
        return false;
    int frame = script->frame++;
    // A frame no line covers is connected and idle; the last matching line wins.
    const PadScriptRange* r = &k_range_idle;
    for (size_t i = 0; i < script->count; i++) {
        if (frame >= script->ranges[i].from && frame <= script->ranges[i].to)
            r = &script->ranges[i];
    }
    if (r->off)
        return false;
    *out = r->state;
    return true;
}

static void _script_free(void* ctx) {
    PadScript* script = ctx;
    if (!script)
        return;
    free(script->ranges);
    free(script);
}

bool input_set_pad_script(GameInputState* input, const char* path) {
    if (!input || !path) {
        log_error("input_set_pad_script: NULL input or path");
        return false;
    }
    char* text = read_entire_file(path, NULL);
    if (!text) {
        log_error("pad script '%s': cannot read", path);
        return false;
    }
    PadScript* script = calloc(1, sizeof(PadScript));
    if (!script) {
        free(text);
        log_error("pad script '%s': out of memory", path);
        return false;
    }
    bool ok = _script_parse(script, text, path);
    free(text);
    if (!ok) {
        _script_free(script);
        return false;
    }
    input_set_pad_reader(input, _pad_read_script, script, _script_free);
    log_info("pad script '%s': %zu ranges on slot 0", path, script->count);
    return true;
}

/*
 * Actions.
 */

#define INPUT_ACTION_THRESHOLD 0.5f

static const char* const k_kind_names[] = {"none", "key", "mouse", "pad", "axis"};

// The code range of a kind; NONE has none and is skipped by every walk.
static bool _source_valid(const InputSource* s) {
    switch (s->kind) {
        case INPUT_SRC_NONE:
            return true;
        case INPUT_SRC_KEY:
            return s->code >= GLFW_KEY_SPACE && s->code <= GLFW_KEY_LAST && s->scale != 0.0f;
        case INPUT_SRC_MOUSE_BUTTON:
            return s->code >= 0 && s->code <= GLFW_MOUSE_BUTTON_LAST && s->scale != 0.0f;
        case INPUT_SRC_PAD_BUTTON:
            return s->code >= 0 && s->code <= GLFW_GAMEPAD_BUTTON_LAST && s->scale != 0.0f;
        case INPUT_SRC_PAD_AXIS:
            return s->code >= 0 && s->code <= GLFW_GAMEPAD_AXIS_LAST && s->scale != 0.0f;
    }
    return false;
}

void input_bind(GameInputState* input, const InputAction* actions, size_t count) {
    if (!input || (count && !actions)) {
        log_error("input_bind: NULL input or table");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        for (int j = 0; j < INPUT_ACTION_SOURCES; j++) {
            const InputSource* s = &actions[i].sources[j];
            if (!_source_valid(s)) {
                log_error("input_bind: action '%s' source %d: kind %d code %d scale %g is not a "
                          "source; table refused",
                          actions[i].name ? actions[i].name : "(unnamed)", j, (int)s->kind, s->code,
                          (double)s->scale);
                return;
            }
        }
    }
    input->actions = actions;
    input->action_count = count;
}

// What one source contributes from one poll's devices. Codes were checked at
// the bind.
static float _source_value(const InputDevices* d, const InputSource* s) {
    switch (s->kind) {
        case INPUT_SRC_NONE:
            return 0.0f;
        case INPUT_SRC_KEY:
            return d->keys[s->code] ? s->scale : 0.0f;
        case INPUT_SRC_MOUSE_BUTTON:
            return d->mouse_buttons[s->code] ? s->scale : 0.0f;
        case INPUT_SRC_PAD_BUTTON:
            return d->pads[0].buttons[s->code] ? s->scale : 0.0f;
        case INPUT_SRC_PAD_AXIS:
            return d->pads[0].axes[s->code] * s->scale;
    }
    return 0.0f;
}

// Whichever source is largest in magnitude; a key at -1 and a stick at 0.3
// read -1, a stick at -0.3 alone reads -0.3.
static float _action_value(const InputDevices* d, const InputAction* action) {
    float best = 0.0f;
    for (int i = 0; i < INPUT_ACTION_SOURCES; i++) {
        float v = _source_value(d, &action->sources[i]);
        if (fabsf(v) > fabsf(best))
            best = v;
    }
    return glm_clamp(best, -1.0f, 1.0f);
}

// Once per name: a typo is read every step, and the first line says it all.
static void _log_unknown_action(const char* name) {
    static char logged[8][32];
    static int logged_count;
    for (int i = 0; i < logged_count; i++) {
        if (strncmp(logged[i], name, sizeof(logged[i]) - 1) == 0)
            return;
    }
    if (logged_count < 8)
        snprintf(logged[logged_count++], sizeof(logged[0]), "%s", name);
    log_error("input: no action named '%s'", name);
}

// The table entry for a name, or NULL, logged, for one the table lacks.
static const InputAction* _action(const GameInputState* input, const char* name) {
    if (!input || !name)
        return NULL;
    for (size_t i = 0; i < input->action_count; i++) {
        if (input->actions[i].name && strcmp(input->actions[i].name, name) == 0)
            return &input->actions[i];
    }
    _log_unknown_action(name);
    return NULL;
}

static bool _over(float value) {
    return fabsf(value) > INPUT_ACTION_THRESHOLD;
}

float input_action_value(const GameInputState* input, const char* name) {
    const InputAction* a = _action(input, name);
    return a ? _action_value(&input->now, a) : 0.0f;
}

bool input_action_down(const GameInputState* input, const char* name) {
    const InputAction* a = _action(input, name);
    return a && _over(_action_value(&input->now, a));
}

bool input_action_pressed(const GameInputState* input, const char* name) {
    const InputAction* a = _action(input, name);
    return a && _over(_action_value(&input->now, a)) && !_over(_action_value(&input->prev, a));
}

bool input_action_released(const GameInputState* input, const char* name) {
    const InputAction* a = _action(input, name);
    return a && !_over(_action_value(&input->now, a)) && _over(_action_value(&input->prev, a));
}

void input_action_move(const GameInputState* input, const char* x, const char* y, vec3 out) {
    out[0] = input_action_value(input, x);
    out[1] = 0.0f;
    out[2] = 0.0f - input_action_value(input, y);
    // Clamped, not normalised: a stick's magnitude is the walk speed.
    if (glm_vec3_norm(out) > 1.0f)
        glm_vec3_normalize(out);
}

void input_print_actions(const InputAction* actions, size_t count) {
    for (size_t i = 0; i < count; i++) {
        printf("%-8s", actions[i].name ? actions[i].name : "(unnamed)");
        for (int j = 0; j < INPUT_ACTION_SOURCES; j++) {
            const InputSource* s = &actions[i].sources[j];
            if (s->kind == INPUT_SRC_NONE)
                continue;
            const char* kind =
                s->kind > 0 && s->kind <= INPUT_SRC_PAD_AXIS ? k_kind_names[s->kind] : "?";
            printf("  %s:%d*%g", kind, s->code, (double)s->scale);
        }
        printf("\n");
    }
}
