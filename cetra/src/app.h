
#ifndef _APP_H_
#define _APP_H_

#include <stdbool.h>
#include <cglm/cglm.h>

// Forward declarations
typedef struct Engine Engine;
typedef struct Scene Scene;
typedef struct Camera Camera;

/*
 * Mouse Drag Camera Controller
 *
 * Handles mouse drag for orbit and pan camera movement.
 * Supports both ORBIT and FREE camera modes.
 */
typedef struct MouseDragController {
    // ENGINE-OWNED (by the controller): the camera as the drag started; a
    // drag is a delta from here. The drag itself lives in engine->input.
    Engine* engine;
    float start_theta;
    float start_phi;
    float start_distance;
    vec3 start_look_at;
    vec3 start_position;

    // SETTINGS: plain stores. Write them directly, at any time.
    float sensitivity;         // Radians of orbit per framebuffer pixel of drag
    bool auto_orbit_enabled;   // Spin the camera on its own until the user takes it
    float auto_orbit_speed;    // Radians per second
    float auto_orbit_min_dist; // The distance breathes between these two
    float auto_orbit_max_dist;
} MouseDragController;

// Created with a windowed viewer's defaults: sensitivity 0.002, auto-orbit off.
MouseDragController* create_mouse_drag_controller(Engine* engine);
void free_mouse_drag_controller(MouseDragController* ctrl);

// Forwarded from the app's mouse-button callback: latches the camera pose a drag
// starts from. The position arguments are unused; the engine's input state
// carries the drag.
void mouse_drag_on_button(MouseDragController* ctrl, int button, int action, int mods, double x,
                          double y);

// Once a frame: the auto-orbit, then the drag in flight as a delta from the
// latched pose (orbit, or pan with shift), then the max-distance clamp.
void mouse_drag_update(MouseDragController* ctrl, float time);

// Keyboard input for camera control (WASD movement, arrows for orbit/pan/zoom)
// Returns true if the key was handled
bool mouse_drag_on_key(MouseDragController* ctrl, int key, int action, int mods);

/*
 * Light Rigs
 */

// Create standard 3-point studio lighting (key, fill, rim)
void scene_add_three_point_lights(Scene* scene, float intensity_scale);

/*
 * GUI Helpers
 */

// Returns true if safe to process 3D input (GUI not hovered)
bool app_can_process_3d_input(const Engine* engine);

/*
 * Common Callbacks
 */

// Standard GLFW error callback
void app_error_callback(int error, const char* description);

#endif // _APP_H_
