
#ifndef _INPUT_H_
#define _INPUT_H_

#include <stdbool.h>
#include <cglm/types.h>

struct SceneNode;

typedef struct InputState {
    bool is_dragging;
    bool shift_held;
    float center_fb_x;
    float center_fb_y;
    float prev_fb_x;
    float prev_fb_y;
    float drag_fb_x;
    float drag_fb_y;

    struct SceneNode* selected_node;
    vec3 drag_start_world_pos;
    vec3 drag_object_start_pos;
    float drag_plane_distance;
} InputState;

void init_input_state(InputState* state);

#endif // _INPUT_H_
