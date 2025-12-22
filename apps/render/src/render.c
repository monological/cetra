#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "cetra/common.h"
#include "cetra/mesh.h"
#include "cetra/shader.h"
#include "cetra/program.h"
#include "cetra/scene.h"
#include "cetra/util.h"
#include "cetra/engine.h"
#include "cetra/import.h"
#include "cetra/render.h"
#include "cetra/transform.h"
#include "cetra/ibl.h"

#include "cetra/shader_strings.h"

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_KEYSTATE_BASED_INPUT
#include "cetra/ext/nuklear.h"
#include "cetra/ext/nuklear_glfw_gl3.h"

/*
 * Constants
 */
const unsigned int HEIGHT = 1080;
const unsigned int WIDTH = 1920;
const float MIN_DIST = 2000.0f;
const float MAX_DIST = 3000.0f;
const float CAM_ANGULAR_SPEED = 0.5f;
const float ORBIT_SENSITIVITY = 0.002f;

/*
 * Drag state - stored at drag start
 */
static float orbit_start_phi = 0.0f;
static float orbit_start_theta = 0.0f;
static float free_start_yaw = 0.0f;
static float free_start_pitch = 0.0f;
static float free_look_distance = 1000.0f;
static vec3 free_start_look_at = {0.0f, 0.0f, 0.0f};
static vec3 free_start_cam_pos = {0.0f, 0.0f, 0.0f};

/*
 * Callbacks
 */
void error_callback(int error, const char* description) {
    fprintf(stderr, "Error: %s\n", description);
}

void cursor_position_callback(Engine* engine, double xpos, double ypos) {
    (void)engine;
    (void)xpos;
    (void)ypos;
}

void mouse_button_callback(Engine* engine, int button, int action, int mods) {
    if (engine->input.is_dragging && engine->camera) {
        Camera* camera = engine->camera;

        if (engine->camera_mode == CAMERA_MODE_ORBIT) {
            // Calculate current phi/theta from camera's actual position
            vec3 dir;
            glm_vec3_sub(camera->position, camera->look_at, dir);
            float dist = glm_vec3_norm(dir);

            if (dist > 0.001f) {
                orbit_start_theta = asinf(dir[1] / dist);
                orbit_start_phi = atan2f(dir[2], dir[0]);
                camera->distance = dist;
            }
            // Save starting positions for Shift+drag pan
            glm_vec3_copy(camera->look_at, free_start_look_at);
            glm_vec3_copy(camera->position, free_start_cam_pos);
            free_look_distance = dist;
        } else if (engine->camera_mode == CAMERA_MODE_FREE) {
            // Calculate current phi/theta from camera position relative to look_at
            vec3 dir;
            glm_vec3_sub(camera->position, camera->look_at, dir);
            float dist = glm_vec3_norm(dir);

            if (dist > 0.001f) {
                free_look_distance = dist;
                free_start_pitch = asinf(dir[1] / dist);
                free_start_yaw = atan2f(dir[2], dir[0]);
            }
            // Save starting positions for Shift+drag pan
            glm_vec3_copy(camera->look_at, free_start_look_at);
            glm_vec3_copy(camera->position, free_start_cam_pos);
        }
    }
}

void key_callback(Engine* engine, int key, int scancode, int action, int mods) {
    (void)engine;
    (void)key;
    (void)scancode;
    (void)action;
    (void)mods;
}

void render_scene_callback(Engine* engine, Scene* current_scene) {
    SceneNode* root_node = current_scene->root_node;

    if (!engine || !root_node)
        return;

    Camera* camera = engine->camera;

    if (!camera)
        return;

    float time_value = glfwGetTime();

    Transform transform = {.position = {0.0f, -80.0f, 0.0f},
                           .rotation = {0.0f, 0.0f, 0.0f},
                           .scale = {1.0f, 1.0f, 1.0f}};

    if (engine->camera_mode == CAMERA_MODE_ORBIT) {
        if (!engine->input.is_dragging) {
            // Auto-orbit animation
            camera->amplitude = (MAX_DIST - MIN_DIST) / 2.0f;
            float midPoint = MIN_DIST + camera->amplitude;
            camera->distance = midPoint + camera->amplitude * sinf(time_value * CAM_ANGULAR_SPEED);
            camera->phi += camera->orbit_speed;

            // Use spherical coordinates for consistency
            float cos_theta = cosf(camera->theta);
            vec3 offset = {camera->distance * cos_theta * cosf(camera->phi),
                           camera->distance * sinf(camera->theta),
                           camera->distance * cos_theta * sinf(camera->phi)};

            vec3 new_camera_position;
            glm_vec3_add(camera->look_at, offset, new_camera_position);

            set_camera_position(camera, new_camera_position);
            update_engine_camera_lookat(engine);
            update_engine_camera_perspective(engine);

        } else {
            if (engine->input.shift_held) {
                // Shift+drag: Pan camera (move both camera and look_at)
                vec3 forward, right_vec, up_vec;
                glm_vec3_sub(free_start_look_at, free_start_cam_pos, forward);
                glm_vec3_crossn(camera->up_vector, forward, right_vec);
                glm_vec3_normalize(right_vec);
                glm_vec3_cross(forward, right_vec, up_vec);
                glm_vec3_normalize(up_vec);

                float pan_speed = free_look_distance * 0.0005f;
                vec3 pan_offset;
                glm_vec3_scale(right_vec, -engine->input.drag_fb_x * pan_speed, pan_offset);
                vec3 up_offset;
                glm_vec3_scale(up_vec, -engine->input.drag_fb_y * pan_speed, up_offset);
                glm_vec3_add(pan_offset, up_offset, pan_offset);

                // Move both camera and look_at from starting positions
                vec3 new_pos, new_look;
                glm_vec3_add(free_start_cam_pos, pan_offset, new_pos);
                glm_vec3_add(free_start_look_at, pan_offset, new_look);
                set_camera_position(camera, new_pos);
                set_camera_look_at(camera, new_look);
            } else {
                // Manual orbit - spherical coordinates around look_at point
                // Horizontal drag rotates around Y axis (phi)
                // Vertical drag tilts up/down (theta)
                camera->phi = orbit_start_phi - engine->input.drag_fb_x * ORBIT_SENSITIVITY;
                camera->theta = orbit_start_theta + engine->input.drag_fb_y * ORBIT_SENSITIVITY;

                // Clamp theta to avoid gimbal lock (stay within ~85 degrees of horizon)
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
            update_engine_camera_lookat(engine);
            update_engine_camera_perspective(engine);
        }
    } else if (engine->camera_mode == CAMERA_MODE_FREE) {
        if (engine->input.is_dragging) {
            if (engine->input.shift_held) {
                // Shift+drag: Pan camera (move both camera and look_at)
                vec3 forward, right_vec, up_vec;
                glm_vec3_sub(free_start_look_at, free_start_cam_pos, forward);
                glm_vec3_crossn(camera->up_vector, forward, right_vec);
                glm_vec3_normalize(right_vec);
                glm_vec3_cross(forward, right_vec, up_vec);
                glm_vec3_normalize(up_vec);

                float pan_speed = free_look_distance * 0.0005f;
                vec3 pan_offset;
                glm_vec3_scale(right_vec, -engine->input.drag_fb_x * pan_speed, pan_offset);
                vec3 up_offset;
                glm_vec3_scale(up_vec, -engine->input.drag_fb_y * pan_speed, up_offset);
                glm_vec3_add(pan_offset, up_offset, pan_offset);

                // Move both camera and look_at from starting positions
                vec3 new_pos, new_look;
                glm_vec3_add(free_start_cam_pos, pan_offset, new_pos);
                glm_vec3_add(free_start_look_at, pan_offset, new_look);
                set_camera_position(camera, new_pos);
                set_camera_look_at(camera, new_look);
            } else {
                // Regular drag: Orbit around look_at point
                float yaw = free_start_yaw - engine->input.drag_fb_x * ORBIT_SENSITIVITY;
                float pitch = free_start_pitch + engine->input.drag_fb_y * ORBIT_SENSITIVITY;

                // Clamp pitch to avoid gimbal lock
                float max_pitch = (float)M_PI_2 - 0.1f;
                if (pitch > max_pitch)
                    pitch = max_pitch;
                if (pitch < -max_pitch)
                    pitch = -max_pitch;

                // Calculate camera position on sphere around look_at point
                float cos_pitch = cosf(pitch);
                vec3 offset = {free_look_distance * cos_pitch * cosf(yaw),
                               free_look_distance * sinf(pitch),
                               free_look_distance * cos_pitch * sinf(yaw)};

                vec3 new_camera_position;
                glm_vec3_add(camera->look_at, offset, new_camera_position);
                set_camera_position(camera, new_camera_position);
            }
        }
        update_engine_camera_lookat(engine);
        update_engine_camera_perspective(engine);
    }

    reset_and_apply_transform(&engine->model_matrix, &transform);

    apply_transform_to_nodes(root_node, engine->model_matrix);

    render_current_scene(engine, time_value);
}

void create_scene_lights(Scene* scene) {
    // Key light - main light, front-right, above
    Light* key = create_light();
    if (!key) {
        fprintf(stderr, "Failed to create key light.\n");
        return;
    }
    set_light_name(key, "key_light");
    set_light_type(key, LIGHT_DIRECTIONAL);
    vec3 key_dir = {-0.4f, -0.7f, -0.6f}; // pointing into scene from front-right-above
    set_light_direction(key, key_dir);
    set_light_intensity(key, 3.0f);
    set_light_color(key, (vec3){1.0f, 0.95f, 0.9f}); // warm white
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
    vec3 fill_dir = {0.5f, -0.4f, -0.5f}; // pointing into scene from front-left
    set_light_direction(fill, fill_dir);
    set_light_intensity(fill, 1.5f);
    set_light_color(fill, (vec3){0.8f, 0.85f, 1.0f}); // cool white
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
    vec3 rim_dir = {0.0f, -0.6f, 0.8f}; // pointing from behind
    set_light_direction(rim, rim_dir);
    set_light_intensity(rim, 2.0f);
    set_light_color(rim, (vec3){1.0f, 1.0f, 1.0f}); // pure white
    add_light_to_scene(scene, rim);

    SceneNode* rim_node = create_node();
    set_node_light(rim_node, rim);
    set_node_name(rim_node, "rim_light_node");
    add_child_node(scene->root_node, rim_node);
}

/*
 * Configure iridescent visor material (pilot helmet style).
 * filmThickness: coating thickness in nanometers (300-500nm for gold/rainbow effect)
 */
void set_node_iridescent_visor(SceneNode* node, float opacity, float roughness,
                               float filmThickness) {
    if (!node)
        return;

    for (size_t i = 0; i < node->mesh_count; i++) {
        Mesh* mesh = node->meshes[i];
        if (mesh && mesh->material) {
            mesh->material->opacity = opacity;
            mesh->material->roughness = roughness;
            mesh->material->metallic = 0.0f;
            mesh->material->ior = 1.5f;
            mesh->material->filmThickness = filmThickness;
        }
    }
}

/*
 * Configure visor materials for helmet models.
 */
void configure_visor_materials(Scene* scene) {
    if (!scene || !scene->root_node)
        return;

    // Common visor node names
    const char* visor_names[] = {"VISIERE_A", "VISIERE_B", "GLASSE", "visor", "Visor", NULL};

    for (int i = 0; visor_names[i] != NULL; i++) {
        SceneNode* node = find_node_by_name(scene->root_node, visor_names[i]);
        if (node) {
            printf("Configuring iridescent visor for: %s\n", visor_names[i]);
            // Mirror-like visor: low opacity (reflective), very glossy, 520nm iridescence
            set_node_iridescent_visor(node, 0.15f, 0.005f, 520.0f);
        }
    }
}

/*
 * CETRA MAIN
 */
int main(int argc, char** argv) {

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <FBX_MODEL_PATH> [FBX_TEXTURE_DIR] [HDR_PATH]\n", argv[0]);
        return -1;
    }

    const char* fbx_model_path = argv[1];
    const char* fbx_texture_dir = argc > 2 ? argv[2] : NULL;
    const char* hdr_path = argc > 3 ? argv[3] : NULL;

    Engine* engine = create_engine("Cetra Engine", WIDTH, HEIGHT);

    if (init_engine(engine) != 0) {
        fprintf(stderr, "Failed to initialize engine\n");
        return -1;
    }

    set_engine_error_callback(engine, error_callback);
    set_engine_mouse_button_callback(engine, mouse_button_callback);
    set_engine_cursor_position_callback(engine, cursor_position_callback);
    set_engine_key_callback(engine, key_callback);

    /*
     * Set up shaders.
     *
     */
    ShaderProgram* pbr_shader_program = get_engine_shader_program_by_name(engine, "pbr");
    if (!pbr_shader_program) {
        fprintf(stderr, "Failed to get PBR shader program\n");
        return -1;
    }

    ShaderProgram* xyz_shader_program = get_engine_shader_program_by_name(engine, "xyz");
    if (!xyz_shader_program) {
        fprintf(stderr, "Failed to get xyz shader program\n");
        return -1;
    }

    /*
     * Set up camera.
     */
    vec3 camera_position = {0.0f, 150.0f, 100.0f};
    vec3 look_at_point = {0.0f, 150.0f, 0.0f};
    vec3 up_vector = {0.0f, 1.0f, 0.0f};
    float fov_radians = 0.37;
    float near_clip = 7.0f;
    float far_clip = 10000.0f;

    Camera* camera = create_camera();

    set_camera_position(camera, camera_position);
    set_camera_look_at(camera, look_at_point);
    set_camera_up_vector(camera, up_vector);
    set_camera_perspective(camera, fov_radians, near_clip, far_clip);

    update_engine_camera_lookat(engine);
    update_engine_camera_perspective(engine);

    camera->theta = 0.60f;
    camera->height = 600.0f;

    set_engine_camera(engine, camera);

    /*
     * Import fbx model with async texture loading.
     */

    Scene* scene =
        create_scene_from_fbx_path_async(fbx_model_path, fbx_texture_dir, engine->async_loader);
    if (!scene) {
        fprintf(stderr, "Failed to import FBX model: %s\n", fbx_model_path);
        return -1;
    }

    add_scene_to_engine(engine, scene);

    if (!scene || !scene->root_node) {
        fprintf(stderr, "Failed to import scene\n");
        return -1;
    }

    if (set_scene_xyz_shader_program(scene, xyz_shader_program) == GL_FALSE) {
        fprintf(stderr, "Failed to set scene xyz shader program\n");
        return -1;
    }

    configure_visor_materials(scene);

    if (hdr_path) {
        // When using IBL, add a single soft key light at reduced intensity
        Light* key = create_light();
        if (key) {
            set_light_name(key, "key_light");
            set_light_type(key, LIGHT_DIRECTIONAL);
            vec3 key_dir = {-0.4f, -0.7f, -0.6f};
            set_light_direction(key, key_dir);
            set_light_intensity(key, 1.0f);
            set_light_color(key, (vec3){1.0f, 1.0f, 1.0f});
            add_light_to_scene(scene, key);

            SceneNode* key_node = create_node();
            set_node_light(key_node, key);
            set_node_name(key_node, "key_light_node");
            add_child_node(scene->root_node, key_node);
        }

        IBLResources* ibl = create_ibl_resources();
        if (ibl && load_hdr_environment(ibl, hdr_path) == 0) {
            if (precompute_ibl(ibl, engine) == 0) {
                scene->ibl = ibl;
                scene->render_skybox = true;
                scene->skybox_exposure = 1.0f;
                printf("IBL loaded from: %s\n", hdr_path);
            } else {
                fprintf(stderr, "Failed to precompute IBL\n");
                free_ibl_resources(ibl);
            }
        } else {
            fprintf(stderr, "Failed to load HDR: %s\n", hdr_path);
            if (ibl)
                free_ibl_resources(ibl);
        }
    } else {
        // No IBL - use directional lights for illumination
        create_scene_lights(scene);
    }

    upload_buffers_to_gpu_for_nodes(scene->root_node);

    set_shader_program_for_nodes(scene->root_node, pbr_shader_program);

    print_scene(scene);

    set_engine_show_gui(engine, true);
    set_engine_show_fps(engine, true);
    set_engine_show_wireframe(engine, false);
    set_engine_show_xyz(engine, false);

    run_engine_render_loop(engine, render_scene_callback);

    printf("Cleaning up...\n");
    free_engine(engine);

    printf("Goodbye Friend...\n");

    return 0;
}
