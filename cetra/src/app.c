
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

    // When dragging starts, capture current camera state
    if (engine->input.is_dragging) {
        vec3 dir;
        glm_vec3_sub(camera->position, camera->look_at, dir);
        float dist = glm_vec3_norm(dir);

        if (dist > 0.001f) {
            if (engine->camera_mode == CAMERA_MODE_ORBIT) {
                ctrl->orbit_start_theta = asinf(dir[1] / dist);
                ctrl->orbit_start_phi = atan2f(dir[2], dir[0]);
                camera->distance = dist;
            } else if (engine->camera_mode == CAMERA_MODE_FREE) {
                ctrl->free_look_distance = dist;
                ctrl->free_start_pitch = asinf(dir[1] / dist);
                ctrl->free_start_yaw = atan2f(dir[2], dir[0]);
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

void set_mouse_drag_sensitivity(MouseDragController* ctrl, float sensitivity) {
    if (ctrl) {
        ctrl->sensitivity = sensitivity;
    }
}

void set_mouse_drag_auto_orbit(MouseDragController* ctrl, bool enabled, float speed, float min_dist,
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
                set_camera_position(camera, new_camera_position);
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
                set_camera_position(camera, new_pos);
                set_camera_look_at(camera, new_look);
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
                set_camera_position(camera, new_camera_position);
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
                set_camera_position(camera, new_pos);
                set_camera_look_at(camera, new_look);
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
                set_camera_position(camera, new_camera_position);
            }
        }
    }

    update_engine_camera_lookat(engine);
    update_engine_camera_perspective(engine);
}

bool camera_controller_on_key(MouseDragController* ctrl, int key, int action, int mods) {
    if (!ctrl || !ctrl->engine || !ctrl->engine->camera) {
        return false;
    }

    // Only handle press and repeat
    if (action != GLFW_PRESS && action != GLFW_REPEAT) {
        return false;
    }

    Engine* engine = ctrl->engine;
    Camera* camera = engine->camera;

    static const float MOVE_SPEED = 300.0f;
    static const float ORBIT_STEP = 0.1f;
    static const float PAN_SPEED = 15.0f;
    static const float ZOOM_FACTOR = 0.9f;
    static const float MIN_ZOOM_DISTANCE = 10.0f;

    switch (key) {
        case GLFW_KEY_W:
            camera_move_forward(camera, MOVE_SPEED);
            return true;

        case GLFW_KEY_S:
            camera_move_forward(camera, -MOVE_SPEED);
            return true;

        case GLFW_KEY_A:
            camera_strafe(camera, MOVE_SPEED);
            return true;

        case GLFW_KEY_D:
            camera_strafe(camera, -MOVE_SPEED);
            return true;

        case GLFW_KEY_UP:
            if (mods & GLFW_MOD_SHIFT) {
                pan_camera(camera, 0.0f, PAN_SPEED);
            } else if (engine->camera_mode == CAMERA_MODE_FREE) {
                camera_zoom_toward_target(camera, ZOOM_FACTOR, MIN_ZOOM_DISTANCE);
            } else if (engine->camera_mode == CAMERA_MODE_ORBIT) {
                orbit_camera(camera, ORBIT_STEP, 0.0f);
            }
            return true;

        case GLFW_KEY_DOWN:
            if (mods & GLFW_MOD_SHIFT) {
                pan_camera(camera, 0.0f, -PAN_SPEED);
            } else if (engine->camera_mode == CAMERA_MODE_FREE) {
                camera_zoom_toward_target(camera, 1.0f / ZOOM_FACTOR, MIN_ZOOM_DISTANCE);
            } else if (engine->camera_mode == CAMERA_MODE_ORBIT) {
                orbit_camera(camera, -ORBIT_STEP, 0.0f);
            }
            return true;

        case GLFW_KEY_LEFT:
            if (mods & GLFW_MOD_SHIFT) {
                pan_camera(camera, -PAN_SPEED, 0.0f);
            } else {
                camera_sync_spherical_from_position(camera);
                orbit_camera(camera, 0.0f, ORBIT_STEP);
            }
            return true;

        case GLFW_KEY_RIGHT:
            if (mods & GLFW_MOD_SHIFT) {
                pan_camera(camera, PAN_SPEED, 0.0f);
            } else {
                camera_sync_spherical_from_position(camera);
                orbit_camera(camera, 0.0f, -ORBIT_STEP);
            }
            return true;

        default:
            return false;
    }
}

/*
 * Light Rigs
 */

void create_three_point_lights(Scene* scene, float intensity_scale) {
    if (!scene || !scene->root_node) {
        fprintf(stderr, "create_three_point_lights: invalid scene\n");
        return;
    }

    // Key light - main light, front-right, above
    Light* key = create_light();
    if (!key) {
        fprintf(stderr, "Failed to create key light.\n");
        return;
    }
    set_light_name(key, "key_light");
    set_light_type(key, LIGHT_DIRECTIONAL);
    vec3 key_dir = {-0.4f, -0.7f, -0.6f};
    set_light_direction(key, key_dir);
    set_light_intensity(key, 3.0f * intensity_scale);
    set_light_color(key, (vec3){1.0f, 0.95f, 0.9f});
    add_light_to_scene(scene, key);

    SceneNode* key_node = create_node();
    set_node_light(key_node, key);
    set_node_name(key_node, "key_light_node");
    add_child_node(scene->root_node, key_node);

    // Fill light - softer, front-left
    Light* fill = create_light();
    if (!fill) {
        fprintf(stderr, "Failed to create fill light.\n");
        return;
    }
    set_light_name(fill, "fill_light");
    set_light_type(fill, LIGHT_DIRECTIONAL);
    vec3 fill_dir = {0.5f, -0.4f, -0.5f};
    set_light_direction(fill, fill_dir);
    set_light_intensity(fill, 1.5f * intensity_scale);
    set_light_color(fill, (vec3){0.8f, 0.85f, 1.0f});
    add_light_to_scene(scene, fill);

    SceneNode* fill_node = create_node();
    set_node_light(fill_node, fill);
    set_node_name(fill_node, "fill_light_node");
    add_child_node(scene->root_node, fill_node);

    // Rim light - behind and above for edge definition
    Light* rim = create_light();
    if (!rim) {
        fprintf(stderr, "Failed to create rim light.\n");
        return;
    }
    set_light_name(rim, "rim_light");
    set_light_type(rim, LIGHT_DIRECTIONAL);
    vec3 rim_dir = {0.0f, -0.6f, 0.8f};
    set_light_direction(rim, rim_dir);
    set_light_intensity(rim, 2.0f * intensity_scale);
    set_light_color(rim, (vec3){1.0f, 1.0f, 1.0f});
    add_light_to_scene(scene, rim);

    SceneNode* rim_node = create_node();
    set_node_light(rim_node, rim);
    set_node_name(rim_node, "rim_light_node");
    add_child_node(scene->root_node, rim_node);
}

/*
 * GUI Helpers
 */

bool app_can_process_3d_input(const Engine* engine) {
    if (!engine || !engine->nk_ctx) {
        return true;
    }
    return !nk_window_is_any_hovered(engine->nk_ctx);
}

/*
 * Common Callbacks
 */

void app_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}
