#ifndef _GAME_INPUT_H_
#define _GAME_INPUT_H_

#include <stdbool.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/types.h>

// Input state for polling-based input
// Tracks current and previous frame state for edge detection
typedef struct GameInputState {
    // Keyboard state
    bool keys[GLFW_KEY_LAST];
    bool keys_prev[GLFW_KEY_LAST];

    // Mouse button state
    bool mouse_buttons[GLFW_MOUSE_BUTTON_LAST];
    bool mouse_buttons_prev[GLFW_MOUSE_BUTTON_LAST];

    // Mouse position
    double mouse_x;
    double mouse_y;
    double mouse_prev_x;
    double mouse_prev_y;
    double mouse_delta_x;
    double mouse_delta_y;

    // Mouse scroll
    double scroll_x;
    double scroll_y;

    // Modifiers (updated each frame)
    bool shift_held;
    bool ctrl_held;
    bool alt_held;

    // GLFW window reference for polling
    GLFWwindow* window;
} GameInputState;

// Initialize input state with window reference
void input_init(GameInputState* input, GLFWwindow* window);

// Update input state - call at start of each frame BEFORE game logic
void input_update(GameInputState* input);

// Reset scroll accumulators - call after processing scroll
void input_reset_scroll(GameInputState* input);

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

// Get scroll delta (accumulated since last reset)
void input_scroll(const GameInputState* input, double* x, double* y);

// Helper for WASD movement direction (returns normalized vec3)
void input_wasd_direction(const GameInputState* input, vec3 out_dir);

// Helper for arrow key direction
void input_arrow_direction(const GameInputState* input, vec3 out_dir);

#endif // _GAME_INPUT_H_
