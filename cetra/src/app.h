
#ifndef _APP_H_
#define _APP_H_

/*
 * App helpers: the two input controllers an app forwards its callbacks to,
 * one for a 3D viewer and one for a 2D canvas, a three-point light rig, and
 * the input gate. Neither controller owns the drag; the engine's input state
 * does, and both read it from there. Each is created with defaults and tuned
 * by writing its fields.
 */

#include <stdbool.h>
#include <cglm/cglm.h>

// Forward declarations
typedef struct Engine Engine;
typedef struct Scene Scene;
typedef struct Camera Camera;

/*
 * Mouse Drag Camera Controller
 *
 * For a 3D viewer: a drag orbits the camera about its target, a shift-drag
 * pans, the keys walk and zoom, and an auto-orbit spins the view until the
 * user takes it. The two camera modes drag the same way and differ in what
 * happens when nothing is dragged.
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
// starts from.
void mouse_drag_on_button(MouseDragController* ctrl, int button, int action, int mods);

// Once a frame: the auto-orbit, then the drag in flight as a delta from the
// latched pose (orbit, or pan with shift), then the max-distance clamp.
void mouse_drag_update(MouseDragController* ctrl, float time);

// Keyboard input for camera control (WASD movement, arrows for orbit/pan/zoom)
// Returns true if the key was handled
bool mouse_drag_on_key(MouseDragController* ctrl, int key, int action, int mods);

/*
 * Canvas Controller
 *
 * The 2D twin of the drag controller, for a square-on orthographic camera
 * over a plane: a drag on empty space pans, a drag on a picked node moves it
 * in the plane, and the wheel zooms about the point under the cursor. An app
 * with gestures of its own (handles, corners) runs them first and hands the
 * controller what is left; the drag itself lives in engine->input.
 */
typedef struct CanvasController {
    // ENGINE-OWNED (by the controller): the pan in flight.
    Engine* engine;
    bool panning;           // A drag that started on empty space
    vec3 pan_start_look_at; // The target as the drag began

    // SETTINGS: plain stores. Write them directly, at any time.
    float zoom_min; // ortho_height range; a bound left 0 is unclamped
    float zoom_max;
    float zoom_step; // ortho_height factor per wheel notch, default 0.9
} CanvasController;

CanvasController* create_canvas_controller(Engine* engine);
void free_canvas_controller(CanvasController* ctrl);

// The world point under a framebuffer position on a square-on orthographic
// camera: look_at + (fb - fb_centre) * (ortho_height / fb_height), both axes.
void canvas_world_under_cursor(const Engine* engine, double fb_x, double fb_y, vec3 out);

// Forwarded from the app's mouse-button callback, after any picking of its
// own. A left press pans when the engine's pick found nothing; a release
// stops.
void canvas_on_button(CanvasController* ctrl, int button, int action, int mods);

// Forwarded from the app's cursor callback, in framebuffer pixels. Moves the
// picked node on the drag plane (x and y; z untouched), else pans.
void canvas_on_cursor(CanvasController* ctrl, double fb_x, double fb_y);

// Forwarded from the app's scroll callback. Zooms ortho_height by
// zoom_step^yoffset about the point under the cursor, within the range.
void canvas_on_scroll(CanvasController* ctrl, double xoffset, double yoffset);

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
