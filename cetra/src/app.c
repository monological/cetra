
#include "app.h"
#include "engine.h"
#include "scene.h"
#include "camera.h"
#include "light.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Mouse Drag Controller Implementation
 */

MouseDragController* create_mouse_drag_controller(Engine* engine) {
    if (!engine) {
        return NULL;
    }

    MouseDragController* ctrl = calloc(1, sizeof(MouseDragController));
    if (!ctrl) {
        return NULL;
    }

    ctrl->engine = engine;
    ctrl->sensitivity = 0.002f;
    ctrl->auto_orbit_enabled = false;
    ctrl->auto_orbit_speed = 0.5f;
    ctrl->auto_orbit_min_dist = 2000.0f;
    ctrl->auto_orbit_max_dist = 3000.0f;

    return ctrl;
}

void free_mouse_drag_controller(MouseDragController* ctrl) {
    if (ctrl) {
        free(ctrl);
    }
}

void mouse_drag_on_button(MouseDragController* ctrl, int button, int action, int mods) {
    (void)button;
    (void)action;
    (void)mods;

    if (!ctrl || !ctrl->engine || !ctrl->engine->camera) {
        return;
    }

    Engine* engine = ctrl->engine;
    Camera* camera = engine->camera;

    // When dragging starts, capture the camera as it stands: a drag is a delta
    // from here, and the orbit parameters describe the pose by the camera's
    // own invariant.
    if (engine->input.is_dragging) {
        ctrl->start_theta = camera->theta;
        ctrl->start_phi = camera->phi;
        ctrl->start_distance = camera->distance;
        glm_vec3_copy(camera->look_at, ctrl->start_look_at);
        glm_vec3_copy(camera->position, ctrl->start_position);
    }
}

void mouse_drag_update(MouseDragController* ctrl, float time) {
    if (!ctrl || !ctrl->engine || !ctrl->engine->camera) {
        return;
    }

    Engine* engine = ctrl->engine;
    Camera* camera = engine->camera;

    // The two camera modes drag the same way: a shift-drag pans from the
    // captured pose, a plain drag orbits the target from the captured angles.
    // They differ only in what happens when nothing is dragged, which for the
    // orbit mode is the auto-orbit. Every move writes the orbit parameters and
    // lets camera_orbit place the eye, which is the one copy of that arithmetic.
    if (!engine->input.is_dragging) {
        if (engine->camera_mode == CAMERA_MODE_ORBIT && ctrl->auto_orbit_enabled) {
            float amplitude = (ctrl->auto_orbit_max_dist - ctrl->auto_orbit_min_dist) / 2.0f;
            float midPoint = ctrl->auto_orbit_min_dist + amplitude;
            camera->distance = midPoint + amplitude * sinf(time * ctrl->auto_orbit_speed);
            camera->phi += camera->orbit_speed;
            camera_orbit(camera, 0.0f, 0.0f);
        }
    } else if (engine->input.shift_held) {
        // Pan: move both camera and look_at from the captured positions
        vec3 forward, right_vec, up_vec;
        glm_vec3_sub(ctrl->start_look_at, ctrl->start_position, forward);
        glm_vec3_crossn(camera->up_vector, forward, right_vec);
        glm_vec3_normalize(right_vec);
        glm_vec3_cross(forward, right_vec, up_vec);
        glm_vec3_normalize(up_vec);

        float pan_speed = ctrl->start_distance * 0.0005f;
        vec3 pan_offset;
        glm_vec3_scale(right_vec, -engine->input.drag_fb_x * pan_speed, pan_offset);
        vec3 up_offset;
        glm_vec3_scale(up_vec, -engine->input.drag_fb_y * pan_speed, up_offset);
        glm_vec3_add(pan_offset, up_offset, pan_offset);

        // As a delta from the latched target, so the events do not accumulate
        vec3 delta;
        glm_vec3_add(ctrl->start_look_at, pan_offset, delta);
        glm_vec3_sub(delta, camera->look_at, delta);
        camera_translate(camera, delta);
    } else {
        // Orbit the target from the captured angles; camera_orbit clamps the
        // elevation away from the poles.
        camera->phi = ctrl->start_phi - engine->input.drag_fb_x * ctrl->sensitivity;
        camera->theta = ctrl->start_theta + engine->input.drag_fb_y * ctrl->sensitivity;
        camera_orbit(camera, 0.0f, 0.0f);
    }

    camera_enforce_max_distance(camera);
}

bool mouse_drag_on_key(MouseDragController* ctrl, int key, int action, int mods) {
    if (!ctrl || !ctrl->engine || !ctrl->engine->camera) {
        return false;
    }

    // Only handle press and repeat
    if (action != GLFW_PRESS && action != GLFW_REPEAT) {
        return false;
    }

    Engine* engine = ctrl->engine;
    Camera* camera = engine->camera;

    // Scale movement speeds relative to camera distance for consistent feel across model sizes
    float base_distance = fmaxf(camera->distance, 1.0f);
    float move_speed = base_distance * 0.1f; // 10% of distance per keypress
    float pan_speed = base_distance * 0.05f; // 5% of distance per keypress
    float min_zoom = base_distance * 0.1f;   // Can zoom to 10% of initial distance

    static const float ORBIT_STEP = 0.1f;
    static const float ZOOM_FACTOR = 0.9f;

    switch (key) {
        case GLFW_KEY_W:
            camera_move_forward(camera, move_speed);
            return true;

        case GLFW_KEY_S:
            camera_move_forward(camera, -move_speed);
            return true;

        case GLFW_KEY_A:
            camera_strafe(camera, move_speed);
            return true;

        case GLFW_KEY_D:
            camera_strafe(camera, -move_speed);
            return true;

        case GLFW_KEY_UP:
            if (mods & GLFW_MOD_SHIFT) {
                camera_pan(camera, 0.0f, pan_speed);
            } else if (engine->camera_mode == CAMERA_MODE_FREE) {
                camera_zoom_toward_target(camera, ZOOM_FACTOR, min_zoom);
            } else if (engine->camera_mode == CAMERA_MODE_ORBIT) {
                camera_orbit(camera, ORBIT_STEP, 0.0f);
            }
            return true;

        case GLFW_KEY_DOWN:
            if (mods & GLFW_MOD_SHIFT) {
                camera_pan(camera, 0.0f, -pan_speed);
            } else if (engine->camera_mode == CAMERA_MODE_FREE) {
                camera_zoom_toward_target(camera, 1.0f / ZOOM_FACTOR, min_zoom);
            } else if (engine->camera_mode == CAMERA_MODE_ORBIT) {
                camera_orbit(camera, -ORBIT_STEP, 0.0f);
            }
            return true;

        case GLFW_KEY_LEFT:
            if (mods & GLFW_MOD_SHIFT) {
                camera_pan(camera, -pan_speed, 0.0f);
            } else {
                camera_orbit(camera, 0.0f, ORBIT_STEP);
            }
            return true;

        case GLFW_KEY_RIGHT:
            if (mods & GLFW_MOD_SHIFT) {
                camera_pan(camera, pan_speed, 0.0f);
            } else {
                camera_orbit(camera, 0.0f, -ORBIT_STEP);
            }
            return true;

        default:
            return false;
    }
}

/*
 * Canvas Controller Implementation
 */

CanvasController* create_canvas_controller(Engine* engine) {
    if (!engine) {
        return NULL;
    }
    CanvasController* ctrl = calloc(1, sizeof(CanvasController));
    if (!ctrl) {
        return NULL;
    }
    ctrl->engine = engine;
    ctrl->zoom_step = 0.9f;
    return ctrl;
}

void free_canvas_controller(CanvasController* ctrl) {
    free(ctrl);
}

void canvas_world_under_cursor(const Engine* engine, double fb_x, double fb_y, vec3 out) {
    glm_vec3_zero(out);
    if (!engine || !engine->camera || engine->fb_height <= 0) {
        return;
    }
    const Camera* camera = engine->camera;
    float units_per_pixel = camera->ortho_height / (float)engine->fb_height;
    out[0] = camera->look_at[0] + (float)(fb_x - engine->fb_width * 0.5) * units_per_pixel;
    out[1] = camera->look_at[1] + (float)(fb_y - engine->fb_height * 0.5) * units_per_pixel;
    out[2] = camera->look_at[2];
}

void canvas_on_button(CanvasController* ctrl, int button, int action, int mods) {
    (void)mods;
    if (!ctrl || !ctrl->engine || button != GLFW_MOUSE_BUTTON_LEFT) {
        return;
    }
    Engine* engine = ctrl->engine;
    if (action == GLFW_RELEASE) {
        ctrl->panning = false;
        return;
    }
    // The engine has already picked the node under the press, if any.
    ctrl->panning = engine->input.selected_node == NULL && engine->camera != NULL;
    if (ctrl->panning) {
        glm_vec3_copy(engine->camera->look_at, ctrl->pan_start_look_at);
    }
}

// Slide the whole view with the cursor: the world point that was under the
// press stays under it. The eye and the target move together, so the view
// direction never changes.
static void _canvas_pan(CanvasController* ctrl) {
    Engine* engine = ctrl->engine;
    Camera* camera = engine->camera;
    if (!camera) {
        return;
    }
    vec3 press = GLM_VEC3_ZERO_INIT, now = GLM_VEC3_ZERO_INIT, offset, delta;
    canvas_world_under_cursor(engine, engine->input.center_fb_x, engine->input.center_fb_y, press);
    canvas_world_under_cursor(engine, engine->input.center_fb_x + engine->input.drag_fb_x,
                              engine->input.center_fb_y + engine->input.drag_fb_y, now);
    glm_vec3_sub(press, now, offset);
    // As a delta from the latched target rather than from the current one,
    // so a sequence of motion events does not accumulate rounding.
    glm_vec3_add(ctrl->pan_start_look_at, offset, delta);
    glm_vec3_sub(delta, camera->look_at, delta);
    camera_translate(camera, delta);
}

void canvas_on_cursor(CanvasController* ctrl, double fb_x, double fb_y) {
    if (!ctrl || !ctrl->engine) {
        return;
    }
    Engine* engine = ctrl->engine;
    if (!engine->input.is_dragging) {
        return;
    }
    SceneNode* node = engine->input.selected_node;
    if (node) {
        // Where the cursor is on the drag plane, as a delta from where the
        // press was, applied to where the node was: x and y only, since the
        // plane is the board and the node's depth is its own. on_plane is
        // seeded because the unproject leaves it alone with no camera.
        vec3 on_plane = GLM_VEC3_ZERO_INIT, delta, target;
        engine_mouse_to_drag_plane(engine, fb_x, fb_y, on_plane);
        glm_vec3_sub(on_plane, engine->input.drag_start_world_pos, delta);
        glm_vec3_add(engine->input.drag_object_start_pos, delta, target);
        target[2] = node->original_transform[3][2];
        node_set_position(node, target);
    } else if (ctrl->panning) {
        _canvas_pan(ctrl);
    }
}

// Zoom about the cursor: the world point under it stays under it.
void canvas_on_scroll(CanvasController* ctrl, double xoffset, double yoffset) {
    (void)xoffset;
    if (!ctrl || !ctrl->engine || !ctrl->engine->camera) {
        return;
    }
    Engine* engine = ctrl->engine;
    Camera* camera = engine->camera;
    float h = camera->ortho_height;
    double fb_x = 0.0, fb_y = 0.0;
    if (h <= 0.0f || !engine_cursor_fb(engine, &fb_x, &fb_y)) {
        return;
    }
    vec3 anchor = GLM_VEC3_ZERO_INIT;
    canvas_world_under_cursor(engine, fb_x, fb_y, anchor);

    // Each bound applies on its own, so a range with one end set is one-sided.
    float h_new = h * powf(ctrl->zoom_step, (float)yoffset);
    if (ctrl->zoom_min > 0.0f) {
        h_new = fmaxf(h_new, ctrl->zoom_min);
    }
    if (ctrl->zoom_max > 0.0f) {
        h_new = fminf(h_new, ctrl->zoom_max);
    }
    float k = 1.0f - h_new / h;

    vec3 shift = {(anchor[0] - camera->look_at[0]) * k, (anchor[1] - camera->look_at[1]) * k, 0.0f};
    camera_translate(camera, shift);
    camera->ortho_height = h_new;
}

/*
 * Light Rigs
 */

void scene_add_three_point_lights(Scene* scene, float intensity_scale) {
    if (!scene || !scene->root_node) {
        fprintf(stderr, "scene_add_three_point_lights: invalid scene\n");
        return;
    }

    // Key from front-right above, fill softer from front-left, rim from behind
    // and above for edge definition.
    const LightDesc rig[] = {
        {.name = "key_light",
         .direction = {-0.4f, -0.7f, -0.6f},
         .color = {1.0f, 0.95f, 0.9f},
         .intensity = 3.0f * intensity_scale},
        {.name = "fill_light",
         .direction = {0.5f, -0.4f, -0.5f},
         .color = {0.8f, 0.85f, 1.0f},
         .intensity = 1.5f * intensity_scale},
        {.name = "rim_light",
         .direction = {0.0f, -0.6f, 0.8f},
         .intensity = 2.0f * intensity_scale},
    };
    for (size_t i = 0; i < sizeof(rig) / sizeof(rig[0]); i++) {
        Light* light = create_light(&rig[i]);
        if (!light) {
            fprintf(stderr, "Failed to create %s.\n", rig[i].name);
            return;
        }
        scene_add_light(scene, light);
        char node_name[32];
        snprintf(node_name, sizeof(node_name), "%s_node", rig[i].name);
        SceneNode* node = create_node();
        node_set_light(node, light);
        node_set_name(node, node_name);
        node_add_child(scene->root_node, node);
    }
}

/*
 * GUI Helpers
 */

bool app_can_process_3d_input(const Engine* engine) {
    if (!engine) {
        return true;
    }
    return !engine_gui_wants_mouse();
}

/*
 * Common Callbacks
 */

void app_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}
