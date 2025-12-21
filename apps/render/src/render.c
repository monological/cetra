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
const float ROTATE_SPEED = 0.4f; // Speed of rotation
const float ROTATION_SENSITIVITY = 0.005f;
const float MIN_DIST = 2000.0f;
const float MAX_DIST = 3000.0f;
const float CAM_ANGULAR_SPEED = 0.5f; // Adjust this value as needed


/*
 * Callbacks
 */
void error_callback(int error, const char* description) {
    fprintf(stderr, "Error: %s\n", description);
}

void cursor_position_callback(Engine *engine, double xpos, double ypos) {

}

void mouse_button_callback(Engine *engine, int button, int action, int mods) {
    if(engine->input.is_dragging){
        printf("dragging start %i %f %f\n", engine->input.is_dragging, engine->input.drag_fb_x, engine->input.drag_fb_y);
    }else{
        printf("dragging stop  %i %f %f\n", engine->input.is_dragging, engine->input.drag_fb_x, engine->input.drag_fb_y);
    }
}

void key_callback(Engine *engine, int key, int scancode, int action, int mods) {

}


void render_scene_callback(Engine* engine, Scene* current_scene){
    SceneNode *root_node = current_scene->root_node;

    if(!engine || !root_node) return;

    Camera *camera = engine->camera;

    if(!camera) return;

    float time_value = glfwGetTime();
    
    Transform transform = {
        .position = {0.0f, -80.0f, 0.0f},
        .rotation = {0.0f, 0.0f, 0.0f},
        .scale = {1.0f, 1.0f, 1.0f}
    };

    if(engine->camera_mode == CAMERA_MODE_ORBIT){
        if (!engine->input.is_dragging) {
            camera->amplitude = (MAX_DIST - MIN_DIST) / 2.0f; // Half the range of motion
            float midPoint = MIN_DIST + camera->amplitude; // Middle point of the motion
            camera->distance = midPoint + camera->amplitude * sin(time_value * CAM_ANGULAR_SPEED);
            camera->phi += camera->orbit_speed; // Rotate around the center

            vec3 new_camera_position = {
                cos(camera->phi) * camera->distance * cos(camera->theta),
                camera->height,
                sin(camera->phi) * camera->distance * cos(camera->theta)
            };

            set_camera_position(camera, new_camera_position);
            update_engine_camera_lookat(engine);
            update_engine_camera_perspective(engine);

        } else {
            transform.position[0] = 0.0f;
            transform.position[1] = -200.0f;
            transform.position[2] = 0.0f;
            transform.rotation[0] = engine->input.drag_fb_y * ROTATION_SENSITIVITY;
            transform.rotation[1] = engine->input.drag_fb_x * ROTATION_SENSITIVITY;
            transform.rotation[2] = 0.0f;
        }
    }else if(engine->camera_mode == CAMERA_MODE_FREE){
        update_engine_camera_lookat(engine);
        update_engine_camera_perspective(engine);
    }

    reset_and_apply_transform(&engine->model_matrix, &transform);

    apply_transform_to_nodes(root_node, engine->model_matrix);

    render_current_scene(engine, time_value);

}

void create_scene_lights(Scene *scene){
    // Key light - main light, front-right, above
    Light *key = create_light();
    if(!key){
        fprintf(stderr, "Failed to create key light.\n");
        return;
    }
    set_light_name(key, "key_light");
    set_light_type(key, LIGHT_POINT);
    vec3 key_pos = {300.0f, 400.0f, 500.0f};
    set_light_original_position(key, key_pos);
    set_light_global_position(key, key_pos);
    set_light_intensity(key, 80000.0f);
    set_light_color(key, (vec3){1.0f, 0.95f, 0.9f});
    add_light_to_scene(scene, key);

    SceneNode *key_node = create_node();
    set_node_light(key_node, key);
    set_node_name(key_node, "key_light_node");
    add_child_node(scene->root_node, key_node);

    // Fill light - softer, front-left
    Light *fill = create_light();
    if(!fill){
        fprintf(stderr, "Failed to create fill light.\n");
        return;
    }
    set_light_name(fill, "fill_light");
    set_light_type(fill, LIGHT_POINT);
    vec3 fill_pos = {-400.0f, 200.0f, 400.0f};
    set_light_original_position(fill, fill_pos);
    set_light_global_position(fill, fill_pos);
    set_light_intensity(fill, 40000.0f);
    set_light_color(fill, (vec3){0.8f, 0.85f, 1.0f});
    add_light_to_scene(scene, fill);

    SceneNode *fill_node = create_node();
    set_node_light(fill_node, fill);
    set_node_name(fill_node, "fill_light_node");
    add_child_node(scene->root_node, fill_node);

    // Rim light - behind and above for edge definition
    Light *rim = create_light();
    if(!rim){
        fprintf(stderr, "Failed to create rim light.\n");
        return;
    }
    set_light_name(rim, "rim_light");
    set_light_type(rim, LIGHT_POINT);
    vec3 rim_pos = {0.0f, 500.0f, -400.0f};
    set_light_original_position(rim, rim_pos);
    set_light_global_position(rim, rim_pos);
    set_light_intensity(rim, 60000.0f);
    set_light_color(rim, (vec3){1.0f, 1.0f, 1.0f});
    add_light_to_scene(scene, rim);

    SceneNode *rim_node = create_node();
    set_node_light(rim_node, rim);
    set_node_name(rim_node, "rim_light_node");
    add_child_node(scene->root_node, rim_node);
}

/*
 * CETRA MAIN
 */
int main(int argc, char **argv) {

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <FBX_MODEL_PATH> [FBX_TEXTURE_DIR]\n", argv[0]);
        return -1;
    }

    const char *fbx_model_path = argv[1];
    const char *fbx_texture_dir = argc > 2 ? argv[2] : NULL;
    
    Engine *engine = create_engine("Cetra Engine", WIDTH, HEIGHT);

    if(init_engine(engine) != 0){
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
    vec3 camera_position = {0.0f, 2.0f, 300.0f};
    vec3 look_at_point = {0.0f, 0.0f, 0.0f};
    vec3 up_vector = {0.0f, 1.0f, 0.0f};
    float fov_radians = 0.37;
    float near_clip = 7.0f;
    float far_clip = 10000.0f;

    Camera *camera = create_camera();

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
     * Import fbx model.
     */

    Scene* scene = create_scene_from_fbx_path(fbx_model_path, fbx_texture_dir);
    if (!scene) {
        fprintf(stderr, "Failed to import FBX model: %s\n", fbx_model_path);
        return -1;
    }

    add_scene_to_engine(engine, scene);

    if(!scene || !scene->root_node){
        fprintf(stderr, "Failed to import scene\n");
        return -1;
    }

    if(set_scene_xyz_shader_program(scene, xyz_shader_program) == GL_FALSE){
        fprintf(stderr, "Failed to set scene xyz shader program\n");
        return -1;
    }

    create_scene_lights(scene);

    upload_buffers_to_gpu_for_nodes(scene->root_node);

    set_shader_program_for_nodes(scene->root_node, pbr_shader_program);

    print_scene(scene);

    set_engine_show_gui(engine, true);
    set_engine_show_wireframe(engine, false);
    set_engine_show_xyz(engine, false);

    run_engine_render_loop(engine, render_scene_callback);

    printf("Cleaning up...\n");
    free_engine(engine);

    printf("Goodbye Friend...\n");

    return 0;
}



