#include "input.h"
#include "../engine.h"
#include "../util.h"
#include "../ext/log.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <cglm/cglm.h>

// The wheel accumulates between polls in file statics: the engine's scroll
// callback carries only the Engine, whose user data is not this layer's.
static double scroll_accum_x = 0.0;
static double scroll_accum_y = 0.0;

static void _scroll_from_engine(Engine* engine, double xoffset, double yoffset) {
    (void)engine;
    scroll_accum_x += xoffset;
    scroll_accum_y += yoffset;
}

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

    // Through the engine, whose own callback feeds ImGui first; installing
    // GLFW's directly overwrote it and the GUI never scrolled.
    engine_set_scroll_callback(engine, _scroll_from_engine);
    glfwSetJoystickCallback(_joystick_callback);
}

void input_free(GameInputState* input) {
    if (!input)
        return;
    if (input->pad_ctx_free)
        input->pad_ctx_free(input->pad_ctx);
    input->pad_ctx = NULL;
    input->pad_ctx_free = NULL;
    input->pad_read = _pad_read_glfw;
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
    float clamped = r > 1.0f ? 1.0f : r;
    float scale = ((clamped - dead) / (1.0f - dead)) / r;
    out[0] = raw[0] * scale;
    out[1] = raw[1] * scale;
}

// GLFW's trigger rests at -1 and reads 1 fully pulled; 0..1 travel, then the
// zone, rescaled the same way.
static float _trigger(float raw, float dead) {
    float t = (raw + 1.0f) * 0.5f;
    if (t <= dead || dead >= 1.0f)
        return 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    return (t - dead) / (1.0f - dead);
}

static void _poll_pad(GameInputState* input, int slot) {
    GamePadState* p = &input->pads[slot];
    memcpy(p->buttons_prev, p->buttons, sizeof(p->buttons));

    GLFWgamepadstate raw;
    memset(&raw, 0, sizeof(raw));
    bool present = input->pad_read && input->pad_read(input->pad_ctx, slot, &raw);
    if (!present) {
        // Everything it held is released, one edge each, as a real release
        // would be; every axis at rest.
        p->connected = false;
        memset(p->buttons, 0, sizeof(p->buttons));
        memset(p->axes, 0, sizeof(p->axes));
        return;
    }
    for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; b++)
        p->buttons[b] = raw.buttons[b] == GLFW_PRESS;
    if (!p->connected) {
        // A button already held when the pad appears is not a press.
        memcpy(p->buttons_prev, p->buttons, sizeof(p->buttons));
        p->connected = true;
    }
    _stick(&raw.axes[GLFW_GAMEPAD_AXIS_LEFT_X], input->stick_dead_zone,
           &p->axes[GLFW_GAMEPAD_AXIS_LEFT_X]);
    _stick(&raw.axes[GLFW_GAMEPAD_AXIS_RIGHT_X], input->stick_dead_zone,
           &p->axes[GLFW_GAMEPAD_AXIS_RIGHT_X]);
    p->axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] =
        _trigger(raw.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER], input->trigger_dead_zone);
    p->axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] =
        _trigger(raw.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER], input->trigger_dead_zone);
}

void input_update(GameInputState* input) {
    if (!input || !input->engine || !input->engine->window)
        return;
    GLFWwindow* window = input->engine->window;

    memcpy(input->keys_prev, input->keys, sizeof(input->keys));
    memcpy(input->mouse_buttons_prev, input->mouse_buttons, sizeof(input->mouse_buttons));
    input->mouse_prev_x = input->mouse_x;
    input->mouse_prev_y = input->mouse_y;

    for (int key = 0; key <= GLFW_KEY_LAST; key++)
        input->keys[key] = glfwGetKey(window, key) == GLFW_PRESS;
    for (int button = 0; button <= GLFW_MOUSE_BUTTON_LAST; button++)
        input->mouse_buttons[button] = glfwGetMouseButton(window, button) == GLFW_PRESS;

    glfwGetCursorPos(window, &input->mouse_x, &input->mouse_y);
    input->mouse_delta_x = input->mouse_x - input->mouse_prev_x;
    input->mouse_delta_y = input->mouse_y - input->mouse_prev_y;

    input->scroll_x = scroll_accum_x;
    input->scroll_y = scroll_accum_y;
    scroll_accum_x = 0.0;
    scroll_accum_y = 0.0;

    input->shift_held = input->keys[GLFW_KEY_LEFT_SHIFT] || input->keys[GLFW_KEY_RIGHT_SHIFT];
    input->ctrl_held = input->keys[GLFW_KEY_LEFT_CONTROL] || input->keys[GLFW_KEY_RIGHT_CONTROL];
    input->alt_held = input->keys[GLFW_KEY_LEFT_ALT] || input->keys[GLFW_KEY_RIGHT_ALT];

    for (int slot = 0; slot < GAME_MAX_PADS; slot++)
        _poll_pad(input, slot);
}

bool input_key_down(const GameInputState* input, int key) {
    if (key < 0 || key > GLFW_KEY_LAST)
        return false;
    return input->keys[key];
}

bool input_key_pressed(const GameInputState* input, int key) {
    if (key < 0 || key > GLFW_KEY_LAST)
        return false;
    return input->keys[key] && !input->keys_prev[key];
}

bool input_key_released(const GameInputState* input, int key) {
    if (key < 0 || key > GLFW_KEY_LAST)
        return false;
    return !input->keys[key] && input->keys_prev[key];
}

bool input_mouse_down(const GameInputState* input, int button) {
    if (button < 0 || button > GLFW_MOUSE_BUTTON_LAST)
        return false;
    return input->mouse_buttons[button];
}

bool input_mouse_pressed(const GameInputState* input, int button) {
    if (button < 0 || button > GLFW_MOUSE_BUTTON_LAST)
        return false;
    return input->mouse_buttons[button] && !input->mouse_buttons_prev[button];
}

bool input_mouse_released(const GameInputState* input, int button) {
    if (button < 0 || button > GLFW_MOUSE_BUTTON_LAST)
        return false;
    return !input->mouse_buttons[button] && input->mouse_buttons_prev[button];
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

static const GamePadState* _pad(const GameInputState* input, int pad) {
    if (!input || pad < 0 || pad >= GAME_MAX_PADS)
        return NULL;
    return &input->pads[pad];
}

bool input_pad_connected(const GameInputState* input, int pad) {
    const GamePadState* p = _pad(input, pad);
    return p && p->connected;
}

bool input_pad_down(const GameInputState* input, int pad, int button) {
    const GamePadState* p = _pad(input, pad);
    if (!p || button < 0 || button > GLFW_GAMEPAD_BUTTON_LAST)
        return false;
    return p->buttons[button];
}

bool input_pad_pressed(const GameInputState* input, int pad, int button) {
    const GamePadState* p = _pad(input, pad);
    if (!p || button < 0 || button > GLFW_GAMEPAD_BUTTON_LAST)
        return false;
    return p->buttons[button] && !p->buttons_prev[button];
}

bool input_pad_released(const GameInputState* input, int pad, int button) {
    const GamePadState* p = _pad(input, pad);
    if (!p || button < 0 || button > GLFW_GAMEPAD_BUTTON_LAST)
        return false;
    return !p->buttons[button] && p->buttons_prev[button];
}

float input_pad_axis(const GameInputState* input, int pad, int axis) {
    const GamePadState* p = _pad(input, pad);
    if (!p || axis < 0 || axis > GLFW_GAMEPAD_AXIS_LAST)
        return 0.0f;
    return p->axes[axis];
}

const char* input_pad_name(const GameInputState* input, int pad) {
    const GamePadState* p = _pad(input, pad);
    if (!p || !p->connected)
        return NULL;
    return glfwGetJoystickName(GLFW_JOYSTICK_1 + pad);
}

void input_set_pad_reader(GameInputState* input, GamepadReadFn read, void* ctx) {
    if (!input) {
        log_error("input_set_pad_reader: NULL input");
        return;
    }
    if (input->pad_ctx_free)
        input->pad_ctx_free(input->pad_ctx);
    input->pad_ctx_free = NULL;
    input->pad_read = read ? read : _pad_read_glfw;
    input->pad_ctx = read ? ctx : NULL;
}

/*
 * The scripted pad.
 */

typedef struct PadScriptRange {
    int from;
    int to;
    bool off;
    unsigned char buttons[GLFW_GAMEPAD_BUTTON_LAST + 1];
    float axes[GLFW_GAMEPAD_AXIS_LAST + 1]; // GLFW's form: triggers rest at -1
} PadScriptRange;

typedef struct PadScript {
    PadScriptRange* ranges;
    size_t count;
    int frame; // The next input_update's frame number
} PadScript;

static const char* const k_button_names[GLFW_GAMEPAD_BUTTON_LAST + 1] = {
    "a",     "b",      "x",      "y",   "lb",     "rb",    "back",  "start",
    "guide", "lstick", "rstick", "dup", "dright", "ddown", "dleft",
};
static const char* const k_axis_names[GLFW_GAMEPAD_AXIS_LAST + 1] = {
    "lx", "ly", "rx", "ry", "lt", "rt",
};

static void _range_idle(PadScriptRange* r) {
    memset(r->buttons, 0, sizeof(r->buttons));
    memset(r->axes, 0, sizeof(r->axes));
    r->axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = -1.0f;
    r->axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = -1.0f;
    r->off = false;
}

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
                r->axes[a] = trigger ? v * 2.0f - 1.0f : v;
                return true;
            }
        }
        log_error("%s:%d: unknown axis '%s'", path, line, tok);
        return false;
    }
    for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; b++) {
        if (strcmp(tok, k_button_names[b]) == 0) {
            r->buttons[b] = GLFW_PRESS;
            return true;
        }
    }
    log_error("%s:%d: unknown token '%s'", path, line, tok);
    return false;
}

// The next whitespace-delimited token of a line, NUL-terminated in place;
// NULL at the end. Written out rather than strtok, which is not one function
// across the three platforms.
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

        PadScriptRange r;
        _range_idle(&r);
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

        PadScriptRange* grown = realloc(script->ranges, (script->count + 1) * sizeof(*grown));
        if (!grown) {
            log_error("%s: out of memory", path);
            return false;
        }
        script->ranges = grown;
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
    PadScriptRange idle;
    _range_idle(&idle);
    const PadScriptRange* r = &idle;
    for (size_t i = 0; i < script->count; i++) {
        if (frame >= script->ranges[i].from && frame <= script->ranges[i].to)
            r = &script->ranges[i];
    }
    if (r->off)
        return false;
    memcpy(out->buttons, r->buttons, sizeof(out->buttons));
    memcpy(out->axes, r->axes, sizeof(out->axes));
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
    input_set_pad_reader(input, _pad_read_script, script);
    input->pad_ctx_free = _script_free;
    log_info("pad script '%s': %zu ranges on slot 0", path, script->count);
    return true;
}

void input_wasd_direction(const GameInputState* input, vec3 out_dir) {
    glm_vec3_zero(out_dir);

    if (input_key_down(input, GLFW_KEY_W)) {
        out_dir[2] -= 1.0f; // Forward (-Z)
    }
    if (input_key_down(input, GLFW_KEY_S)) {
        out_dir[2] += 1.0f; // Backward (+Z)
    }
    if (input_key_down(input, GLFW_KEY_A)) {
        out_dir[0] -= 1.0f; // Left (-X)
    }
    if (input_key_down(input, GLFW_KEY_D)) {
        out_dir[0] += 1.0f; // Right (+X)
    }

    // Normalize if non-zero
    float len = glm_vec3_norm(out_dir);
    if (len > 0.0001f) {
        glm_vec3_scale(out_dir, 1.0f / len, out_dir);
    }
}

void input_arrow_direction(const GameInputState* input, vec3 out_dir) {
    glm_vec3_zero(out_dir);

    if (input_key_down(input, GLFW_KEY_UP)) {
        out_dir[2] -= 1.0f;
    }
    if (input_key_down(input, GLFW_KEY_DOWN)) {
        out_dir[2] += 1.0f;
    }
    if (input_key_down(input, GLFW_KEY_LEFT)) {
        out_dir[0] -= 1.0f;
    }
    if (input_key_down(input, GLFW_KEY_RIGHT)) {
        out_dir[0] += 1.0f;
    }

    float len = glm_vec3_norm(out_dir);
    if (len > 0.0001f) {
        glm_vec3_scale(out_dir, 1.0f / len, out_dir);
    }
}
