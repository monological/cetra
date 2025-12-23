
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
    Engine* engine;

    // Drag state
    bool is_dragging;
    double start_x;
    double start_y;

    // Orbit mode start state
    float orbit_start_phi;
    float orbit_start_theta;

    // Free mode start state
    float free_start_yaw;
    float free_start_pitch;
    float free_look_distance;
    vec3 free_start_look_at;
    vec3 free_start_cam_pos;

    // Configuration
    float sensitivity;

    // Auto-orbit configuration
    bool auto_orbit_enabled;
    float auto_orbit_speed;
    float auto_orbit_min_dist;
    float auto_orbit_max_dist;
} MouseDragController;

// Lifecycle
MouseDragController* create_mouse_drag_controller(Engine* engine);
void free_mouse_drag_controller(MouseDragController* ctrl);

// Input handlers (apps forward from their callbacks)
void mouse_drag_on_button(MouseDragController* ctrl, int button, int action, int mods, double x,
                          double y);
void mouse_drag_on_cursor(MouseDragController* ctrl, double x, double y);

// Update (call each frame - handles auto-orbit animation and camera updates)
void mouse_drag_update(MouseDragController* ctrl, float time);

// Keyboard input for camera control (WASD, arrows, R/F up/down, Q/E zoom)
// Returns true if the key was handled
bool camera_controller_on_key(MouseDragController* ctrl, int key, int action, int mods);

// Configuration
void set_mouse_drag_sensitivity(MouseDragController* ctrl, float sensitivity);
void set_mouse_drag_auto_orbit(MouseDragController* ctrl, bool enabled, float speed, float min_dist,
                               float max_dist);

/*
 * Light Rigs
 */

// Create standard 3-point studio lighting (key, fill, rim)
void create_three_point_lights(Scene* scene, float intensity_scale);

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
