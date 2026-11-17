#ifndef _SCENE_VIEW_H_
#define _SCENE_VIEW_H_

#include <stdbool.h>
#include <stdint.h>

#include "camera.h"
#include "common.h"
#include "exposure.h"
#include "postfx.h"

struct CameraRig;
struct Engine;
struct SceneTags;
struct ShaderProgram;

// The classes a view draws apart (SceneNode.tag_class); class 0 is none, and its row unread.
#define SCENE_TAG_CLASSES 256

// How a view draws one class of surface (spec 14.39). All zeros = as the material.
typedef struct SceneTagClass {
    // A matte paint in place of the material's colour, 0xRRGGBBAA in the display's sRGB, the alpha
    // its share; 0 = the material's. A tag's own colour is drawn over it.
    uint32_t colour;
    float see_through; // 0 = solid, 1 = gone; what is not solid casts no shadow
    float flat;  // the paint's share drawn unlit at the view's exposure, so it reads in shadow
    bool hidden; // drawn in no pass of this view, shadows included
} SceneTagClass;

/*
 * A scene view: one camera onto the engine's current scene, drawn into its own buffers and shown in
 * a rectangle of the window (spec 14.38). An app makes the views it shows -- the engine starts with
 * none -- adds each with engine_add_view, and writes its settings at any time.
 */

// What a view does, as plain values an app writes at any time. Nothing here is the engine's or
// holds a GPU object, so two are compared byte for byte; the engine starts from
// engine_init_view_settings.
typedef struct SceneViewSettings {
    PostFXSettings post;       // the post chain
    ExposureSettings exposure; // the camera's exposure
    int samples;               // scene target sample count, 1..the driver's most; 4 by default
    int ss_scale;              // supersampling, 1 or 2
    float render_scale;        // the scene's share of the post size, 0.5..1; 1 headless without
                               // headless_jitter, which the upscale reconstructs from
    // What a pixel the scene draws nothing in is cleared to: a working-space value, carried by the
    // chain like any surface; or, background_exact, the display colour wanted on screen, which the
    // engine turns back through the view's tone curve and display encode to the value it clears to.
    vec3 background;
    bool background_exact;
    RenderMode render_mode; // PBR, or a debug view of one input
    bool show_wireframe;    // every edge of every face, albedo-only, no culling
    bool show_xyz;          // the XYZ gizmo on every node
    bool show_bones;        // X-ray bone visualization
    bool show_lights;       // light overlay: position cross + cull-radius wireframe
    bool headless_jitter;   // TAA's sub-pixel jitter even headless (non-deterministic frames)
    // 0 = the view draws every frame under TAA. 16..64 = once nothing it shows has changed for a
    // frame it averages this many jittered frames, equally weighted, then rests until something
    // does; render scale 1 only.
    int accumulate_frames;
    // Tags (spec 14.39). How far a surface not lit fades: 0 = none, 1 = gone. It turns see-through
    // as it fades, untagged surfaces with the rest, and casts no shadow.
    float tag_fade;
    float outline_points; // an outline's width, window points
    float outline_hidden; // an outline's strength where what it rings is hidden, 0..1
    SceneTagClass tag_classes[SCENE_TAG_CLASSES];
} SceneViewSettings;

typedef struct SceneViewDesc {
    CameraDesc camera;
    const SceneViewSettings* settings; // NULL = engine_init_view_settings's
    bool fills_window;                 // the rectangle below is then unused
    float x, y, width, height;         // window points, top-left, +Y down, as the UI
    uint32_t mask;                     // 0 = every bit
} SceneViewDesc;

typedef struct SceneView {
    // SETTINGS: plain stores. Write them directly, at any time.
    Camera camera;             // by value; its pose and projection are the app's
    bool fills_window;         // true = the rectangle follows the window
    float x, y, width, height; // window points, top-left, +Y down, as the UI
    uint32_t mask;             // draws the nodes it shares a bit with
    bool hidden;               // kept, neither drawn nor shown
    struct SceneTags* tags;    // borrowed, and shared by any view; NULL = every tag as drawn
    SceneViewSettings settings;

    // BY FUNCTION: the camera rig (scene_view_set_rig), borrowed.
    struct CameraRig* rig;

    // ENGINE-OWNED: read, never write.
    int frames_accumulated; // jittered frames in the average it shows; 0 under TAA
    bool finished;          // the average is complete: the view rests, its picture shown as it is
    struct SceneViewState* state;
} SceneView;

// The settings a view starts from: every feature at its default, the engine's tuning lengths at its
// typical size. An app fills these, changes what it means, and hands them to create_scene_view.
void engine_init_view_settings(const struct Engine* engine, SceneViewSettings* settings);

// NULL desc is a view of the default camera filling the window, with the default settings. The view
// is built at once, its buffers and its post chain included, so it needs the engine's context and
// is freed before the engine. NULL, logged, when its buffers cannot be made.
SceneView* create_scene_view(struct Engine* engine, const SceneViewDesc* desc);
// Removes an added view first; refused, logged and kept, as engine_remove_view refuses.
void free_scene_view(SceneView* view);

// The engine takes the view and draws it every frame, after the views already added; the first
// added is the focus until another is named. False, logged, for a view of another engine or one
// already added. A view added again has missed what changed while it was out: its first frame is a
// cut.
bool engine_add_view(struct Engine* engine, SceneView* view);
// Hands an added view back, still built, for the caller to free or add again. Removing the focus
// passes it to the first view left. NULL when the view was not added, and, logged, while the frame
// draws its views (from a render hook), which would pull a view out from under the loop.
SceneView* engine_remove_view(struct Engine* engine, SceneView* view);
// The view whose eye ranks and centres the world's work: the lighting caches, the terrain pages,
// the rain's cover and the water's windows, the origin shift, the listener. One of the added views.
// A hidden focus ranks the world from its camera as posed, but the terrain pages' votes come from
// its draw, so it casts none.
void engine_set_focus_view(struct Engine* engine, SceneView* view);

// The rig that poses the view's camera each frame, or NULL for none: the engine applies it after
// the app's pre-render hook. Borrowed; given the engine's typical size.
void scene_view_set_rig(SceneView* view, struct CameraRig* rig);

// A flat 2D view: no bloom, glare, local exposure, AO, SSR, vignette, dither or TAA, a linear tone
// map and unit exposure, so an authored colour reaches the screen as written. The scene half is
// scene_set_2d_preset.
void scene_view_set_2d_preset(SceneView* view);

// The view's colour grade from a .cube file, and back to none. False, logged, when it cannot load.
bool scene_view_load_lut(SceneView* view, const char* path);
void scene_view_clear_lut(SceneView* view);
// A post pass of the app's at `at` in this view's chain; the engine owns the program from then on.
struct PostFXPass* scene_view_add_pass(SceneView* view, PostFXLocation at,
                                       struct ShaderProgram* program);

// A framebuffer position in the window (input.h's pointer space) as a point in window points
// relative to the view's top-left corner, the space the two functions below take.
void scene_view_point_from_fb(const SceneView* view, double fb_x, double fb_y, float out[2]);
// Whether the point lies inside the view.
bool scene_view_contains(const SceneView* view, const float point[2]);
// The ray through a point of the view, from its camera as last derived: the eye under perspective,
// the point on the near plane under ortho. Unit direction.
void scene_view_ray(const SceneView* view, const float point[2], vec3 origin, vec3 dir);
// The world point under a point of the view, on the plane through the press's pick at the eye's
// distance from it: where a dragged node goes.
void scene_view_drag_point(const SceneView* view, const float point[2], vec3 out_world_pos);

#endif // _SCENE_VIEW_H_
