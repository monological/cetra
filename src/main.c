#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "common.h"
#include "mesh.h"
#include "shader.h"
#include "program.h"
#include "scene.h"
#include "util.h"
#include "engine.h"
#include "import.h"

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_KEYSTATE_BASED_INPUT
#include "ext/nuklear.h"
#include "ext/nuklear_glfw_gl3.h"

#define PBR_VERT_SHADER_PATH "./shaders/pbr_vert.glsl"
#define PBR_FRAG_SHADER_PATH "./shaders/pbr_frag.glsl"
#define AXES_VERT_SHADER_PATH "./shaders/axes_vert.glsl"
#define AXES_FRAG_SHADER_PATH "./shaders/axes_frag.glsl"

#define FBX_MODEL_PATH "./models/room.fbx"
#define FBX_TEXTURE_DIR "./textures/room.fbm"

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

void render_scene_callback(Engine* engine, Scene* current_scene){
    SceneNode *root_node = current_scene->root_node;

    if(!engine || !root_node) return;

    Camera *camera = engine->camera;

    if(!camera) return;

    float time_value = glfwGetTime();
    float angle = time_value * ROTATE_SPEED;
    camera->amplitude = (MAX_DIST - MIN_DIST) / 2.0f; // Half the range of motion
    float midPoint = MIN_DIST + camera->amplitude; // Middle point of the motion

    Transform transform = {
        .position = {0.0f, -200.0f, 0.0f},
        .rotation = {0.0f, 0.0f, 0.0f},
        .scale = {1.0f, 1.0f, 1.0f}
    };

    if (!dragging) {
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
        float zpos = cosf(angle);
        float ypos = sinf(angle);

        transform.position[0] = 0.0f;
        transform.position[1] = -25.0f;
        transform.position[2] = 0.0f;
        transform.rotation[0] = dy * ROTATION_SENSITIVITY;
        transform.rotation[1] = dx * ROTATION_SENSITIVITY;
        transform.rotation[2] = 0.0f;
    }

    transform_node(root_node, &transform);

    apply_transform_to_nodes(root_node, engine->model_matrix);

    render_nodes(root_node, camera, 
        root_node->local_transform, 
        engine->view_matrix, engine->projection_matrix, 
        time_value, 
        engine->current_render_mode
    );

}

/*
 * CETRA MAIN
 */
int main() {
    
    printf("┏┓┏┓┏┳┓┳┓┏┓\n");
    printf("┃ ┣  ┃ ┣┫┣┫\n");
    printf("┗┛┗┛ ┻ ┛┗┛┗\n");

    printf("\nInitializing Cetra Graphics Engine...\n");
    
    Engine *engine = create_engine("Cetra Engine", WIDTH, HEIGHT);

    setup_engine_glfw(engine);
    setup_engine_msaa(engine);

    setup_engine_gui(engine);

    set_engine_error_callback(engine, error_callback);
    set_engine_mouse_button_callback(engine, mouse_button_callback);
    set_engine_cursor_position_callback(engine, cursor_position_callback);

    /*
     * Set up shaders.
     *
     */
    ShaderProgram* pbr_shader_program = create_program();
    if (!init_program_shader(pbr_shader_program, 
                PBR_VERT_SHADER_PATH, PBR_FRAG_SHADER_PATH, NULL)) {
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

    Scene* scene = create_scene_from_fbx_path(FBX_MODEL_PATH, FBX_TEXTURE_DIR);
    if (!scene) {
        fprintf(stderr, "Failed to import FBX model: %s\n", FBX_MODEL_PATH);
        return -1;
    }

    add_scene_to_engine(engine, scene);

    if(!scene || !scene->root_node){
        fprintf(stderr, "Failed to import scene\n");
        return -1;
    }

    setup_scene_axes(scene);

    SceneNode* root_node = scene->root_node;
    assert(root_node != NULL);

    vec3 lightPosition = {0.0, 0.0, 2000.00};
    if(root_node->light == NULL){
        printf("No root light found so adding one...\n");
        Light *light = create_light();
        set_light_name(light, "root");
        set_light_type(light, LIGHT_POINT);
        set_light_position(light, lightPosition);
        set_light_intensity(light, 10.0f);
        set_light_color(light, (vec3){1.0f, 0.7f, 0.7f});
        set_node_light(root_node, light);
    }

    upload_buffers_to_gpu_for_nodes(root_node);

    set_program_for_nodes(root_node, pbr_shader_program);

    print_scene(scene);

    set_engine_show_wireframe(engine, false);
    set_engine_show_axes(engine, true);

    run_engine_render_loop(engine, render_scene_callback);

    printf("Cleaning up...\n");
    free_engine(engine);

    printf("Goodbye Friend...\n");

    return 0;
}



