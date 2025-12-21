
#include "input.h"
#include <cglm/cglm.h>

void init_input_state(InputState* state) {
    if (!state)
        return;

    state->is_dragging = false;
    state->shift_held = false;
    state->center_fb_x = 0.0f;
    state->center_fb_y = 0.0f;
    state->prev_fb_x = 0.0f;
    state->prev_fb_y = 0.0f;
    state->drag_fb_x = 0.0f;
    state->drag_fb_y = 0.0f;

    state->selected_node = NULL;
    glm_vec3_zero(state->drag_start_world_pos);
    glm_vec3_zero(state->drag_object_start_pos);
    state->drag_plane_distance = 0.0f;
}
