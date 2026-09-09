
#include "app.h"
#include "engine.h"
#include "scene.h"
#include "camera.h"
#include "light.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

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

void mouse_drag_on_button(MouseDragController* ctrl, int button, int action, int mods, double x,
                          double y) {
    (void)button;
    (void)action;
    (void)mods;
    (void)x;
    (void)y;

    if (!ctrl || !ctrl->engine || !ctrl->engine->camera) {
        return;
    }

    Engine* engine = ctrl->engine;
    Camera* camera = engine->camera;

    // When dragging starts, capture current camera state. The orbit parameters
    // are re-derived from the pose on screen rather than trusted: the GUI's
    // orbit sliders write them directly, and a drag continues from what is
    // shown, not from a slider.
    if (engine->input.is_dragging) {
        float dist = glm_vec3_distance(camera->position, camera->look_at);

        if (dist > 0.001f) {
            camera_sync_spherical_from_position(camera);
            if (engine->camera_mode == CAMERA_MODE_ORBIT) {
                ctrl->orbit_start_theta = camera->theta;
                ctrl->orbit_start_phi = camera->phi;
            } else if (engine->camera_mode == CAMERA_MODE_FREE) {
                ctrl->free_start_pitch = camera->theta;
                ctrl->free_start_yaw = camera->phi;
            }
        }
        // Save starting positions for Shift+drag pan
        glm_vec3_copy(camera->look_at, ctrl->free_start_look_at);
        glm_vec3_copy(camera->position, ctrl->free_start_cam_pos);
        ctrl->free_look_distance = dist;
    }
}

void mouse_drag_on_cursor(MouseDragController* ctrl, double x, double y) {
    (void)ctrl;
    (void)x;
    (void)y;
    // Currently unused - drag movement is handled in update via engine->input.drag_fb_x/y
}

void mouse_drag_set_sensitivity(MouseDragController* ctrl, float sensitivity) {
    if (ctrl) {
        ctrl->sensitivity = sensitivity;
    }
}

void mouse_drag_set_auto_orbit(MouseDragController* ctrl, bool enabled, float speed, float min_dist,
                               float max_dist) {
    if (ctrl) {
        ctrl->auto_orbit_enabled = enabled;
        ctrl->auto_orbit_speed = speed;
        ctrl->auto_orbit_min_dist = min_dist;
        ctrl->auto_orbit_max_dist = max_dist;
    }
}

void mouse_drag_update(MouseDragController* ctrl, float time) {
    if (!ctrl || !ctrl->engine || !ctrl->engine->camera) {
        return;
    }

    Engine* engine = ctrl->engine;
    Camera* camera = engine->camera;

    if (engine->camera_mode == CAMERA_MODE_ORBIT) {
        if (!engine->input.is_dragging) {
            if (ctrl->auto_orbit_enabled) {
                // Auto-orbit animation
                float amplitude = (ctrl->auto_orbit_max_dist - ctrl->auto_orbit_min_dist) / 2.0f;
                float midPoint = ctrl->auto_orbit_min_dist + amplitude;
                camera->distance = midPoint + amplitude * sinf(time * ctrl->auto_orbit_speed);
                camera->phi += camera->orbit_speed;

                float cos_theta = cosf(camera->theta);
                vec3 offset = {camera->distance * cos_theta * cosf(camera->phi),
                               camera->distance * sinf(camera->theta),
                               camera->distance * cos_theta * sinf(camera->phi)};

                vec3 new_camera_position;
                glm_vec3_add(camera->look_at, offset, new_camera_position);
                camera_set_position(camera, new_camera_position);
            }
        } else {
            if (engine->input.shift_held) {
                // Shift+drag: Pan camera (move both camera and look_at)
                vec3 forward, right_vec, up_vec;
                glm_vec3_sub(ctrl->free_start_look_at, ctrl->free_start_cam_pos, forward);
                glm_vec3_crossn(camera->up_vector, forward, right_vec);
                glm_vec3_normalize(right_vec);
                glm_vec3_cross(forward, right_vec, up_vec);
                glm_vec3_normalize(up_vec);

                float pan_speed = ctrl->free_look_distance * 0.0005f;
                vec3 pan_offset;
                glm_vec3_scale(right_vec, -engine->input.drag_fb_x * pan_speed, pan_offset);
                vec3 up_offset;
                glm_vec3_scale(up_vec, -engine->input.drag_fb_y * pan_speed, up_offset);
                glm_vec3_add(pan_offset, up_offset, pan_offset);

                // Move both camera and look_at from starting positions
                vec3 new_pos, new_look;
                glm_vec3_add(ctrl->free_start_cam_pos, pan_offset, new_pos);
                glm_vec3_add(ctrl->free_start_look_at, pan_offset, new_look);
                camera_set_position(camera, new_pos);
                camera_set_look_at(camera, new_look);
            } else {
                // Manual orbit - spherical coordinates around look_at point
                camera->phi = ctrl->orbit_start_phi - engine->input.drag_fb_x * ctrl->sensitivity;
                camera->theta =
                    ctrl->orbit_start_theta + engine->input.drag_fb_y * ctrl->sensitivity;

                // Clamp theta to avoid gimbal lock
                float max_theta = (float)M_PI_2 - 0.1f;
                if (camera->theta > max_theta)
                    camera->theta = max_theta;
                if (camera->theta < -max_theta)
                    camera->theta = -max_theta;

                // Calculate camera position on sphere around look_at point
                float cos_theta = cosf(camera->theta);
                vec3 offset = {camera->distance * cos_theta * cosf(camera->phi),
                               camera->distance * sinf(camera->theta),
                               camera->distance * cos_theta * sinf(camera->phi)};

                vec3 new_camera_position;
                glm_vec3_add(camera->look_at, offset, new_camera_position);
                camera_set_position(camera, new_camera_position);
            }
        }
    } else if (engine->camera_mode == CAMERA_MODE_FREE) {
        if (engine->input.is_dragging) {
            if (engine->input.shift_held) {
                // Shift+drag: Pan camera (move both camera and look_at)
                vec3 forward, right_vec, up_vec;
                glm_vec3_sub(ctrl->free_start_look_at, ctrl->free_start_cam_pos, forward);
                glm_vec3_crossn(camera->up_vector, forward, right_vec);
                glm_vec3_normalize(right_vec);
                glm_vec3_cross(forward, right_vec, up_vec);
                glm_vec3_normalize(up_vec);

                float pan_speed = ctrl->free_look_distance * 0.0005f;
                vec3 pan_offset;
                glm_vec3_scale(right_vec, -engine->input.drag_fb_x * pan_speed, pan_offset);
                vec3 up_offset;
                glm_vec3_scale(up_vec, -engine->input.drag_fb_y * pan_speed, up_offset);
                glm_vec3_add(pan_offset, up_offset, pan_offset);

                // Move both camera and look_at from starting positions
                vec3 new_pos, new_look;
                glm_vec3_add(ctrl->free_start_cam_pos, pan_offset, new_pos);
                glm_vec3_add(ctrl->free_start_look_at, pan_offset, new_look);
                camera_set_position(camera, new_pos);
                camera_set_look_at(camera, new_look);
            } else {
                // Regular drag: Orbit around look_at point
                float yaw = ctrl->free_start_yaw - engine->input.drag_fb_x * ctrl->sensitivity;
                float pitch = ctrl->free_start_pitch + engine->input.drag_fb_y * ctrl->sensitivity;

                // Clamp pitch to avoid gimbal lock
                float max_pitch = (float)M_PI_2 - 0.1f;
                if (pitch > max_pitch)
                    pitch = max_pitch;
                if (pitch < -max_pitch)
                    pitch = -max_pitch;

                // Calculate camera position on sphere around look_at point
                float cos_pitch = cosf(pitch);
                vec3 offset = {ctrl->free_look_distance * cos_pitch * cosf(yaw),
                               ctrl->free_look_distance * sinf(pitch),
                               ctrl->free_look_distance * cos_pitch * sinf(yaw)};

                vec3 new_camera_position;
                glm_vec3_add(camera->look_at, offset, new_camera_position);
                camera_set_position(camera, new_camera_position);
            }
        }
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
