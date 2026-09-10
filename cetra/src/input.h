#ifndef _INPUT_H_
#define _INPUT_H_

#include <stdbool.h>
#include <cglm/types.h>

struct SceneNode;

// The engine's own reading of the pointer, written by its GLFW callbacks
// before the app's are called. Positions are FRAMEBUFFER pixels with +Y up,
// which is the space the app callbacks receive and engine_mouse_to_drag_plane
// takes.
//
// ENGINE-OWNED throughout, with one exception: an app may clear selected_node
// on a press to keep the engine's drag off a node it handles itself.
typedef struct InputState {
    bool is_dragging;  // The left button is down
    bool shift_held;   // Shift was down at the press
    float center_fb_x; // Where the press landed
    float center_fb_y;
    float drag_fb_x; // The cursor's offset from the press, not a per-frame delta
    float drag_fb_y;

    struct SceneNode* selected_node; // The node the press picked, or NULL; borrowed
    vec3 drag_start_world_pos;       // The pick's hit point, world space
    vec3 drag_object_start_pos;      // The picked node's world position at the press
    float drag_plane_distance;       // Eye to hit point: the plane a drag moves in

    // The wheel since the previous frame ended: what the poll at a frame's end
    // delivered, so the next frame's update hook reads one frame's delta.
    // Scroll over a GUI panel reaches ImGui and not this.
    double scroll_dx;
    double scroll_dy;
} InputState;

void init_input_state(InputState* state);

#endif // _INPUT_H_
