#include "input.h"
#include <string.h>
#include <cglm/cglm.h>

// Scroll callback accumulator (needs to be static for GLFW callback)
static double scroll_accum_x = 0.0;
static double scroll_accum_y = 0.0;

static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    (void)window;
    scroll_accum_x += xoffset;
    scroll_accum_y += yoffset;
}

void input_init(GameInputState* input, GLFWwindow* window) {
    memset(input, 0, sizeof(GameInputState));
    input->window = window;

    // Get initial mouse position
    glfwGetCursorPos(window, &input->mouse_x, &input->mouse_y);
    input->mouse_prev_x = input->mouse_x;
    input->mouse_prev_y = input->mouse_y;

    // Set scroll callback
    glfwSetScrollCallback(window, scroll_callback);
}

void input_update(GameInputState* input) {
    if (!input->window) {
        return;
    }

    // Copy current state to previous
    memcpy(input->keys_prev, input->keys, sizeof(input->keys));
    memcpy(input->mouse_buttons_prev, input->mouse_buttons, sizeof(input->mouse_buttons));
    input->mouse_prev_x = input->mouse_x;
    input->mouse_prev_y = input->mouse_y;

    // Poll keyboard state
    for (int key = 0; key < GLFW_KEY_LAST; key++) {
        int state = glfwGetKey(input->window, key);
        input->keys[key] = (state == GLFW_PRESS);
    }

    // Poll mouse button state
    for (int button = 0; button < GLFW_MOUSE_BUTTON_LAST; button++) {
        int state = glfwGetMouseButton(input->window, button);
        input->mouse_buttons[button] = (state == GLFW_PRESS);
    }

    // Get mouse position
    glfwGetCursorPos(input->window, &input->mouse_x, &input->mouse_y);
    input->mouse_delta_x = input->mouse_x - input->mouse_prev_x;
    input->mouse_delta_y = input->mouse_y - input->mouse_prev_y;

    // Get accumulated scroll
    input->scroll_x = scroll_accum_x;
    input->scroll_y = scroll_accum_y;

    // Update modifier keys
    input->shift_held = input->keys[GLFW_KEY_LEFT_SHIFT] || input->keys[GLFW_KEY_RIGHT_SHIFT];
    input->ctrl_held = input->keys[GLFW_KEY_LEFT_CONTROL] || input->keys[GLFW_KEY_RIGHT_CONTROL];
    input->alt_held = input->keys[GLFW_KEY_LEFT_ALT] || input->keys[GLFW_KEY_RIGHT_ALT];
}

void input_reset_scroll(GameInputState* input) {
    input->scroll_x = 0.0;
    input->scroll_y = 0.0;
    scroll_accum_x = 0.0;
    scroll_accum_y = 0.0;
}

bool input_key_down(const GameInputState* input, int key) {
    if (key < 0 || key >= GLFW_KEY_LAST) {
        return false;
    }
    return input->keys[key];
}

bool input_key_pressed(const GameInputState* input, int key) {
    if (key < 0 || key >= GLFW_KEY_LAST) {
        return false;
    }
    return input->keys[key] && !input->keys_prev[key];
}

bool input_key_released(const GameInputState* input, int key) {
    if (key < 0 || key >= GLFW_KEY_LAST) {
        return false;
    }
    return !input->keys[key] && input->keys_prev[key];
}

bool input_mouse_down(const GameInputState* input, int button) {
    if (button < 0 || button >= GLFW_MOUSE_BUTTON_LAST) {
        return false;
    }
    return input->mouse_buttons[button];
}

bool input_mouse_pressed(const GameInputState* input, int button) {
    if (button < 0 || button >= GLFW_MOUSE_BUTTON_LAST) {
        return false;
    }
    return input->mouse_buttons[button] && !input->mouse_buttons_prev[button];
}

bool input_mouse_released(const GameInputState* input, int button) {
    if (button < 0 || button >= GLFW_MOUSE_BUTTON_LAST) {
        return false;
    }
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
