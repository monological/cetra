
#ifndef _APP_H_
#define _APP_H_

/*
 * App helpers: the two input adapters an app forwards its callbacks to, one for
 * a 3D viewer and one for a 2D canvas, a three-point light rig, and the input
 * gate. Neither owns the drag; the engine's input state does, and both read it
 * from there. Each is created with defaults and tuned by writing its fields.
 *
 * NEITHER IS A CAMERA, since spec 12.19. Both turn a pointer into what a
 * `camera_rig.h` rig is told, and the rig decides where the eye goes. The one
 * that used to be a camera was called `MouseDragController` -- named for its
 * input rather than its job, which is why three apps wrote their own follow
 * cameras rather than finding it.
 */

#include <stdbool.h>
#include <cglm/cglm.h>

// Forward declarations
typedef struct Engine Engine;
typedef struct Scene Scene;
typedef struct Camera Camera;

/*
 * A POINTER driving a camera rig, for a 3D viewer (spec 12.19).
 *
 * This is an input adapter and not a camera: a drag becomes an aim, a
 * shift-drag becomes an anchor, the wheel and the keys become a distance, and
 * `camera_rig.h` decides what any of that does to the eye.
 *
 * Everything it keeps is POINTER state: where the aim was when the button went
 * down, so an offset from the press is an absolute angle rather than an
 * accumulation. The drag itself lives in engine->input.
 */
typedef struct CameraDrag {
    // ENGINE-OWNED (by the adapter): the rig as the drag started.
    Engine* engine;
    struct CameraRig* rig; // borrowed; the app owns it
    float start_yaw;
    float start_pitch;
    vec3 start_anchor;

    // SETTINGS: plain stores. Write them directly, at any time.
    float sensitivity;         // Radians of aim per framebuffer pixel of drag
    float pan_per_unit;        // World units per pixel, per unit of distance
    float zoom_step;           // Distance factor per wheel notch
    float min_dist;            // The wheel will not zoom closer than this
    bool auto_orbit_enabled;   // Spin the camera on its own until the user takes it
    float auto_orbit_speed;    // Radians per second
    float auto_orbit_min_dist; // The distance breathes between these two
    float auto_orbit_max_dist;
} CameraDrag;

// Created with a windowed viewer's defaults: sensitivity 0.002, pan 0.0005 per
// unit of distance, a zoom step of 0.9, auto-orbit off. The rig is borrowed and
// must outlive this.
CameraDrag* create_camera_drag(Engine* engine, struct CameraRig* rig);
void free_camera_drag(CameraDrag* drag);

// Forwarded from the app's mouse-button callback: latches the aim a drag starts
// from.
void camera_drag_on_button(CameraDrag* drag, int button, int action, int mods);

// Once a frame: the auto-orbit, then the drag in flight as an offset from the
// latched aim (orbit, or pan with shift), then the rig placed from it. `time`
// is a wall clock, for the auto-orbit alone.
//
// It UPDATES the rig, so nothing else may update the same rig in the same
// frame: a second update advances the blend and the shake twice.
void camera_drag_update(CameraDrag* drag, float time);

// Forwarded from the app's scroll callback: the wheel zooms by zoom_step per
// notch, down to min_dist. An app that does not install a scroll callback has
// no wheel zoom -- wiring it is the app's call, as the other callbacks are.
void camera_drag_on_scroll(CameraDrag* drag, double xoffset, double yoffset);

// Keyboard camera control (WASD walks, arrows orbit/pan/zoom). True if the key
// was handled.
bool camera_drag_on_key(CameraDrag* drag, int key, int action, int mods);

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
    // The rig it writes its pose through; owned. Its ARITHMETIC is this file's
    // and always was -- a 2D camera pans and zooms an ortho height and never
    // rotates, which a 3D rig has nothing to say about. What it shares is who is
    // allowed to move the camera, so that rule has no exception (spec 12.19).
    struct CameraRig* rig;
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

// The camera pose a frame draws from, on stdout, at a width that makes a textual
// diff of two runs a bit diff.
//
// Here because two apps print it and one regex parses both: a format spelled
// twice is two places for the contract to drift, and the drift would keep
// matching. Call it from the RENDER callback, not the pre-render hook -- the
// engine applies the camera rig after that hook returns, so a trace taken there
// reports last frame's camera.
void app_trace_camera(const Engine* engine);

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
