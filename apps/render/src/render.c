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
 * Mouse
 */
bool dragging = false;
float centerX;
float centerY;
float dx, dy;

/*
 * Callbacks
 */
void error_callback(int error, const char* description) {
    fprintf(stderr, "Error: %s\n", description);
}

void cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
    int windowWidth, windowHeight, framebufferWidth, framebufferHeight;

    if(!window) return;

    Engine *engine = glfwGetWindowUserPointer(window);
    if (!engine) {
        printf("Engine pointer is NULL\n");
        return;
    }

    if (!engine->nk_ctx) {
        printf("Nuklear context is NULL\n");
        return;
    }

    // Check if mouse is over any Nuklear window
    if (nk_window_is_any_hovered(engine->nk_ctx)) {
        return;
    }

    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    xpos = ((xpos / windowWidth) * framebufferWidth);
    ypos = ((1 - ypos / windowHeight) * framebufferHeight);


    if (dragging) {
        dx = xpos - centerX;
        dy = ypos - centerY;
    }
}

void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    int windowWidth, windowHeight, framebufferWidth, framebufferHeight;

    if(!window) return;

    Engine *engine = glfwGetWindowUserPointer(window);
    if (!engine) {
        printf("Engine pointer is NULL\n");
        return;
    }

    if (!engine->nk_ctx) {
        printf("Nuklear context is NULL\n");
        return;
    }

    // Check if mouse is over any Nuklear window
    if (nk_window_is_any_hovered(engine->nk_ctx)) {
        return;
    }

    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    double xpos, ypos;
    glfwGetCursorPos(window, &xpos, &ypos);

    xpos = ((xpos / windowWidth) * framebufferWidth);
    ypos = ((1 - ypos / windowHeight) * framebufferHeight);


    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        dragging = true;
        centerX = xpos;
        centerY = ypos;
        printf("dragging %i %f %f\n", dragging, xpos, ypos);
    } else if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
        dragging = false;
        printf("dragging %i %f %f\n", dragging, xpos, ypos);
    }
}

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {

    if(!window) return;

    Engine *engine = glfwGetWindowUserPointer(window);
    if (!engine) {
        printf("Engine pointer is NULL\n");
        return;
    }

    if(!engine->camera){
        printf("No camera defined.\n");
        return;
    }

    Camera *camera = engine->camera;

    float cameraSpeed = 300.0f;
    vec3 move_direction = {0.0f, 0.0f, 0.0f};
    vec3 new_position = {0.0f, 0.0f, 0.0f};

    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        vec3 forward, right;
        switch (key) {
            case GLFW_KEY_W:
            case GLFW_KEY_UP:
                // Move camera forward
                glm_vec3_sub(camera->look_at, camera->position, forward);
                glm_vec3_normalize(forward);
                glm_vec3_scale(forward, cameraSpeed, forward);
                glm_vec3_add(camera->position, forward, new_position);
                set_camera_position(camera, new_position);
                break;

            case GLFW_KEY_S:
            case GLFW_KEY_DOWN:
                // Move camera backward
                glm_vec3_sub(camera->look_at, camera->position, forward);
                glm_vec3_normalize(forward);
                glm_vec3_scale(forward, cameraSpeed, forward);
                glm_vec3_sub(camera->position, forward, new_position);
                set_camera_position(camera, new_position);
                break;

            case GLFW_KEY_A:
            case GLFW_KEY_LEFT:
                // Move camera left
                glm_vec3_sub(camera->look_at, camera->position, forward);
                glm_vec3_crossn(camera->up_vector, forward, right);
                glm_vec3_normalize(right);
                glm_vec3_scale(right, cameraSpeed, right);
                glm_vec3_add(camera->position, right, new_position);
                set_camera_position(camera, new_position);
                break;

            case GLFW_KEY_D:
            case GLFW_KEY_RIGHT:
                // Move camera right
                glm_vec3_sub(camera->look_at, camera->position, forward);
                glm_vec3_crossn(camera->up_vector, forward, right);
                glm_vec3_normalize(right);
                glm_vec3_scale(right, cameraSpeed, right);
                glm_vec3_sub(camera->position, right, new_position);
                set_camera_position(camera, new_position);
                break;
        }

    }

}


void render_scene_callback(Engine* engine, Scene* current_scene){
    SceneNode *root_node = current_scene->root_node;

    if(!engine || !root_node) return;

    Camera *camera = engine->camera;

    if(!camera) return;

    float time_value = glfwGetTime();
    
    Transform transform = {
        .position = {0.0f, -200.0f, 0.0f},
        .rotation = {0.0f, 0.0f, 0.0f},
        .scale = {1.0f, 1.0f, 1.0f}
    };

    if(engine->camera_mode == CAMERA_MODE_ORBIT){
        if (!dragging) {
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
            float angle = time_value * ROTATE_SPEED;
            float zpos = cosf(angle);
            float ypos = sinf(angle);

            transform.position[0] = 0.0f;
            transform.position[1] = -200.0f;
            transform.position[2] = 0.0f;
            transform.rotation[0] = dy * ROTATION_SENSITIVITY;
            transform.rotation[1] = dx * ROTATION_SENSITIVITY;
            transform.rotation[2] = 0.0f;
        }
    }else if(engine->camera_mode == CAMERA_MODE_FREE){
        update_engine_camera_lookat(engine);
        update_engine_camera_perspective(engine);
    }

    transform_node(root_node, &transform, &(engine->model_matrix));

    apply_transform_to_nodes(root_node, engine->model_matrix);

    render_scene(
        current_scene, 
        current_scene->root_node,
        camera, 
        root_node->local_transform, 
        engine->view_matrix, 
        engine->projection_matrix, 
        time_value, 
        engine->current_render_mode
    );

}

/*
 * CETRA MAIN
 */
int main(int argc, char **argv) {

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <FBX_MODEL_PATH> <FBX_TEXTURE_DIR>\n", argv[0]);
        return -1;
    }

    const char *fbx_model_path = argv[1];
    const char *fbx_texture_dir = argv[2];
    
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
    ShaderProgram* pbr_shader_program = NULL;

    if (!setup_program_shader_from_source(&pbr_shader_program, 
                pbr_vert_shader_str, pbr_frag_shader_str, NULL)) {
        fprintf(stderr, "Failed to initialize PBR shader program\n");
        return -1;
    }
    add_program_to_engine(engine, pbr_shader_program);

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
    camera->height = 1000.0f;

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

    if(!setup_scene_axes(scene)){
        fprintf(stderr, "Failed to scene axes shaders\n");
        return -1;
    }

    if(!setup_scene_outlines(scene)){
        fprintf(stderr, "Failed to scene light outlines shaders\n");
        return -1;
    }

    SceneNode* root_node = scene->root_node;
    assert(root_node != NULL);

    if(scene->light_count == 0){
        printf("No root light found so adding one...\n");
        Light *light = create_light();
        if(!light){
            fprintf(stderr, "Failed to create root light.\n");
            return -1;
        }
        set_light_name(light, "root_light");
        set_light_type(light, LIGHT_POINT);
        vec3 lightPosition = {0.0f, 0.0f, 0.0f};
        set_light_original_position(light, lightPosition);
        set_light_global_position(light, lightPosition);
        set_light_intensity(light, 500.0f);
        set_light_color(light, (vec3){0.0f, 0.0f, 100.0f});
        add_light_to_scene(scene, light);

        SceneNode *light_node = create_node();
        set_node_light(light_node, light);
        set_node_name(light_node, "root_light_node");
        set_show_axes_for_nodes(light_node, true);
        set_show_outlines_for_nodes(light_node, true);

        Transform light_transform = {
            .position = {300.0f, 300.0f, -200.00f},
            .rotation = {0.0f, 0.0f, 0.0f},
            .scale = {1.0f, 1.0f, 1.0f}
        };

        mat4 light_matrix; 
        glm_mat4_identity(light_matrix);
        transform_node(light_node, &light_transform, &light_matrix);

        add_child_node(root_node, light_node);
    }

    upload_buffers_to_gpu_for_nodes(root_node);

    set_program_for_nodes(root_node, pbr_shader_program);

    print_scene(scene);

    set_engine_show_gui(engine, true);
    set_engine_show_wireframe(engine, false);
    set_engine_show_axes(engine, true);
    set_engine_show_outlines(engine, true);

    run_engine_render_loop(engine, render_scene_callback);

    printf("Cleaning up...\n");
    free_engine(engine);

    printf("Goodbye Friend...\n");

    return 0;
}



