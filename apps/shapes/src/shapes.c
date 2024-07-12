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
#include "cetra/geometry.h"

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

#define FBX_MODEL_PATH "./models/room.fbx"
#define FBX_TEXTURE_DIR "./textures/room.fbm"

/*
 * Constants
 */
const unsigned int HEIGHT = 812;
const unsigned int WIDTH = 375;
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
    if(engine->mouse_is_dragging){
        printf("dragging start %i %f %f\n", engine->mouse_is_dragging, engine->mouse_drag_x, engine->mouse_drag_y);
    }else{
        printf("dragging stop  %i %f %f\n", engine->mouse_is_dragging, engine->mouse_drag_x, engine->mouse_drag_y);
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
        .position = {0.0f, 0.0f, -100.0f},
        .rotation = {0.0f, 0.0f, 0.0f},
        .scale = {1.0f, 1.0f, 1.0f}
    };

    update_engine_camera_lookat(engine);
    update_engine_camera_perspective(engine);

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
int main() {
    
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

    ShaderProgram* shape_shader_program = get_engine_shader_program_by_name(engine, "shape");
    if (!shape_shader_program) {
        fprintf(stderr, "Failed to get shape shader program\n");
        return -1;
    }

    ShaderProgram* xyz_shader_program = get_engine_shader_program_by_name(engine, "xyz");
    if (!xyz_shader_program) {
        fprintf(stderr, "Failed to get xyz shader program\n");
        return -1;
    }

    /*
     * Set up materials.
     */
    Material* pbr_material = create_material();
    set_material_shader_program(pbr_material, pbr_shader_program);

    pbr_material->albedo[0] = 1.0f; // Red
    pbr_material->albedo[1] = 0.0f; // Green
    pbr_material->albedo[2] = 0.0f; // Blue

    Material* shape_material = create_material();
    set_material_shader_program(shape_material, shape_shader_program);

    shape_material->albedo[0] = 0.0f; // Red
    shape_material->albedo[1] = 1.0f; // Green
    shape_material->albedo[2] = 0.0f; // Blue


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
    set_engine_camera_mode(engine, CAMERA_MODE_FREE);
    
    /*
     * Import fbx model.
     */

    Scene* scene = create_scene();
    if (!scene) {
        fprintf(stderr, "Failed to create scene\n");
        return -1;
    }
    add_scene_to_engine(engine, scene);

    SceneNode* root_node = create_node();
    if (!root_node) {
        fprintf(stderr, "Failed to create root node\n");
        return -1;
    }

    set_scene_root_node(scene, root_node);

    if(set_scene_xyz_shader_program(scene, xyz_shader_program) == GL_FALSE){
        fprintf(stderr, "Failed to set scene xyz shader program\n");
        return -1;
    }

    /*
     * mesh1: Rectangle with no corner radius and no fill
     */
    Mesh* mesh1 = create_mesh();
    mesh1->material = shape_material;

    Rect rectangle1 = {
        .position = {-20.0f, -20.0f, 0.0f},
        .size = {20.0f, 20.0f, 0.0f},
        .corner_radius = 0.0f,
        .line_width = 0.2f,
        .filled = false
    };
    generate_rect_to_mesh(mesh1, &rectangle1);

    add_mesh_to_node(root_node, mesh1);

    /*
     * mesh2: Rectangle with no corner radius and fill
     */
    Mesh* mesh2 = create_mesh();
    mesh2->material = pbr_material;

    Rect rectangle2 = {
        .position = {20.0f, -20.0f, 0.0f},
        .size = {20.0f, 20.0f, 0.0f},
        .corner_radius = 0.0f,
        .line_width = 2.0f,
        .filled = true
    };
    generate_rect_to_mesh(mesh2, &rectangle2);

    add_mesh_to_node(root_node, mesh2);

    /*
     * mesh3: Rectangle with corner radius and no fill
     */
    Mesh* mesh3 = create_mesh();
    mesh3->material = shape_material;

    Rect rectangle3 = {
        .position = {-20.0f, 20.0f, 0.0f},
        .size = {20.0f, 20.0f, 0.0f},
        .corner_radius = 2.0f,
        .line_width = 2.0f,
        .filled = false
    };
    generate_rect_to_mesh(mesh3, &rectangle3);

    add_mesh_to_node(root_node, mesh3);

    /*
     * mesh4: Rectangle with corner radius and fill
     */
    Mesh* mesh4 = create_mesh();
    mesh4->material = pbr_material;

    Rect rectangle4 = {
        .position = {20.0f, 20.0f, 0.0f},
        .size = {20.0f, 20.0f, 0.0f},
        .corner_radius = 2.0f,
        .line_width = 2.0f,
        .filled = true
    };
    generate_rect_to_mesh(mesh4, &rectangle4);

    add_mesh_to_node(root_node, mesh4);

    /*
     * mesh5: Circle with no fill
     */
    Mesh* mesh5 = create_mesh();
    mesh5->material = shape_material;

    Circle circle1 = {
        .position = {-20.0f, -60.0f, 0.0f},
        .radius = 10.0f,
        .line_width = 10.0f,
        .filled = false
    };
    
    generate_circle_to_mesh(mesh5, &circle1);

    add_mesh_to_node(root_node, mesh5);

    /*
     * mesh6: Circle with fill
     */
    Mesh* mesh6 = create_mesh();
    mesh6->material = pbr_material;

    Circle circle2 = {
        .position = {20.0f, -60.0f, 0.0f},
        .radius = 10.0f,
        .line_width = 2.0f,
        .filled = true
    };
    
    generate_circle_to_mesh(mesh6, &circle2);

    add_mesh_to_node(root_node, mesh6);

    // Top-Left Quadrant (Start on left, End on right, Y-Start < Y-End)
    vec3 start7 = {-35.0f, 75.0f, 0.0f}; // Starting from left, higher up
    vec3 end7 = {-25.0f, 65.0f, 0.0f};   // Ending towards right, slightly lower

    // Top-Right Quadrant (Start on right, End on left, Y-Start < Y-End)
    vec3 start8 = {35.0f, 75.0f, 0.0f};  // Starting from right, higher up
    vec3 end8 = {25.0f, 65.0f, 0.0f};    // Ending towards left, slightly lower

    // Bottom-Left Quadrant (Start on left, End on right, Y-Start > Y-End)
    vec3 start9 = {-35.0f, 45.0f, 0.0f}; // Starting from left, lower down
    vec3 end9 = {-25.0f, 55.0f, 0.0f};   // Ending towards right, slightly higher

    // Bottom-Right Quadrant (Start on right, End on left, Y-Start > Y-End)
    vec3 start10 = {35.0f, 45.0f, 0.0f}; // Starting from right, lower down
    vec3 end10 = {25.0f, 55.0f, 0.0f};   // Ending towards left, slightly higher

    /*
     * mesh7: S-Shaped Bezier Curve
     */
    Mesh* mesh7 = create_mesh();
    mesh7->material = shape_material;

    Curve *bez7 = generate_s_shaped_bezier_curve(start7, end7, 5.0f, 2.0f);
    generate_curve_to_mesh(mesh7, bez7);
    add_mesh_to_node(root_node, mesh7);
    free(bez7);

    Mesh* mesh8 = create_mesh();
    mesh8->material = shape_material;

    Curve *bez8 = generate_s_shaped_bezier_curve(start8, end8, 5.0f, 2.0f);
    generate_curve_to_mesh(mesh8, bez8);
    add_mesh_to_node(root_node, mesh8);
    free(bez8);

    Mesh* mesh9 = create_mesh();
    mesh9->material = shape_material;

    Curve *bez9 = generate_s_shaped_bezier_curve(start9, end9, 5.0f, 2.0f);
    generate_curve_to_mesh(mesh9, bez9);
    add_mesh_to_node(root_node, mesh9);
    free(bez9);

    Mesh* mesh10 = create_mesh();
    mesh10->material = shape_material;

    Curve *bez10 = generate_s_shaped_bezier_curve(start10, end10, 5.0f, 2.0f);
    generate_curve_to_mesh(mesh10, bez10);
    add_mesh_to_node(root_node, mesh10);
    free(bez10);

    assert(root_node != NULL);

    upload_buffers_to_gpu_for_nodes(root_node);

    print_scene(scene);

    set_engine_show_gui(engine, false);
    set_engine_show_wireframe(engine, false);
    set_engine_show_xyz(engine, false);

    run_engine_render_loop(engine, render_scene_callback);

    printf("Cleaning up...\n");
    free_engine(engine);

    printf("Goodbye Friend...\n");

    return 0;
}



