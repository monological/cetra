#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "cetra/common.h"
#include "cetra/mesh.h"
#include "cetra/program.h"
#include "cetra/scene.h"
#include "cetra/util.h"
#include "cetra/engine.h"
#include "cetra/import.h"
#include "cetra/geometry.h"

#define FBX_MODEL_PATH  "./models/room.fbx"
#define FBX_TEXTURE_DIR "./textures/room.fbm"

/*
 * Constants
 */
const unsigned int HEIGHT = 812;
const unsigned int WIDTH = 375;

// World-space height of the 2D view volume. Matches the framing the old perspective camera
// gave at its 300-unit distance (2 * 300 * tan(0.37 / 2)).
#define ORTHO_HEIGHT 112.3f

// Canvas panning, driven by a drag that starts on empty space rather than on a shape.
static bool is_panning = false;
static vec3 pan_start_look_at;
static vec3 pan_start_cam_pos;

/*
 * Callbacks
 */
void error_callback(int error, const char* description) {
    fprintf(stderr, "Error: %s\n", description);
}

// Slide the whole view with the cursor. Both camera and look_at move by the same vector, so
// the view direction never changes -- a rotation here would shear the flat shapes.
static void pan_canvas(Engine* engine) {
    Camera* camera = engine->camera;
    if (!camera || engine->fb_height <= 0) {
        return;
    }

    // The ortho volume maps to the framebuffer uniformly on both axes
    float units_per_pixel = camera->ortho_height / (float)engine->fb_height;
    vec3 offset = {-engine->input.drag_fb_x * units_per_pixel,
                   -engine->input.drag_fb_y * units_per_pixel, 0.0f};

    vec3 new_look_at, new_position;
    glm_vec3_add(pan_start_look_at, offset, new_look_at);
    glm_vec3_add(pan_start_cam_pos, offset, new_position);

    camera_set_look_at(camera, new_look_at);
    camera_set_position(camera, new_position);
}

void cursor_position_callback(Engine* engine, double xpos, double ypos) {
    if (!engine->input.is_dragging) {
        return;
    }

    if (engine->input.selected_node) {
        // Get current mouse position in world space on the drag plane
        vec3 current_world_pos;
        engine_mouse_to_drag_plane(engine, xpos, ypos, current_world_pos);

        // Calculate world-space delta from drag start
        vec3 world_delta;
        glm_vec3_sub(current_world_pos, engine->input.drag_start_world_pos, world_delta);

        // Calculate new object position: start position + delta
        vec3 new_pos;
        glm_vec3_add(engine->input.drag_object_start_pos, world_delta, new_pos);

        // Set the translation directly in the transform matrix
        SceneNode* node = engine->input.selected_node;
        node->original_transform[3][0] = new_pos[0];
        node->original_transform[3][1] = new_pos[1];
        // Keep Z unchanged: node->original_transform[3][2] = new_pos[2];
    } else if (is_panning) {
        pan_canvas(engine);
    }
}

void mouse_button_callback(Engine* engine, int button, int action, int mods) {
    (void)mods;

    if (button != GLFW_MOUSE_BUTTON_LEFT) {
        return;
    }

    if (action == GLFW_PRESS) {
        // A press that missed every shape grabs the canvas instead
        is_panning = (engine->input.selected_node == NULL);
        if (is_panning && engine->camera) {
            glm_vec3_copy(engine->camera->look_at, pan_start_look_at);
            glm_vec3_copy(engine->camera->position, pan_start_cam_pos);
        }
    } else if (action == GLFW_RELEASE) {
        is_panning = false;
    }
}

void key_callback(Engine* engine, int key, int scancode, int action, int mods) {
    (void)scancode;
    (void)mods;

    if (action != GLFW_PRESS) {
        return;
    }

    switch (key) {
        case GLFW_KEY_ESCAPE:
            glfwSetWindowShouldClose(engine->window, GLFW_TRUE);
            break;
        case GLFW_KEY_G:
            engine_set_show_gui(engine, !engine->show_gui);
            break;
        case GLFW_KEY_X:
            engine_set_show_xyz(engine, !engine->show_xyz);
            break;
        case GLFW_KEY_T:
            engine_set_show_wireframe(engine, !engine->show_wireframe);
            break;
        default:
            break;
    }
}

// The camera this frame's geometry is read against. The engine propagates the
// graph as soon as this returns, and the shadow pass fits its cascades to the
// camera set here.
void pre_render_callback(Engine* engine, Scene* current_scene) {
    if (!engine || !current_scene->root_node || !engine->camera)
        return;

    engine_update_view(engine);
    engine_update_projection(engine);
}

void render_scene_callback(Engine* engine, Scene* current_scene) {
    if (!engine || !current_scene->root_node)
        return;

    engine_render_scene(engine, current_scene);
}

/*
 * CETRA MAIN
 */
int main() {

    Engine* engine = create_engine("Cetra Engine", WIDTH, HEIGHT);

    // NO AA POLICY HERE, DELIBERATELY, and this is where one would go -- the
    // sample count has to precede engine_init, which builds the scene target.
    // This app keeps the engine's 4x MSAA and no temporal filter where the 3D
    // apps drop to one sample plus TAA (spec 11.103). Multisampling is what 2D
    // line art wants, and nothing in this scene moves, so a temporal
    // accumulator would have only its own jitter to integrate while switching
    // on the aux G-buffer, the resolve, and the eight passes keyed off
    // taa_resolving. The camera here is ORTHOGRAPHIC, which is a second
    // reason to leave the post chain alone: the engine's depth reconstruction
    // and view vector are perspective-only.
    if (engine_init(engine) != 0) {
        fprintf(stderr, "Failed to initialize engine\n");
        return -1;
    }

    engine_set_error_callback(engine, error_callback);
    engine_set_mouse_button_callback(engine, mouse_button_callback);
    engine_set_cursor_position_callback(engine, cursor_position_callback);
    engine_set_key_callback(engine, key_callback);

    /*
     * Set up shaders.
     *
     */
    ShaderProgram* pbr_shader_program = engine_get_program(engine, "pbr");
    if (!pbr_shader_program) {
        fprintf(stderr, "Failed to get PBR shader program\n");
        return -1;
    }

    ShaderProgram* shape_shader_program = engine_get_program(engine, "shape");
    if (!shape_shader_program) {
        fprintf(stderr, "Failed to get shape shader program\n");
        return -1;
    }

    ShaderProgram* xyz_shader_program = engine_get_program(engine, "xyz");
    if (!xyz_shader_program) {
        fprintf(stderr, "Failed to get xyz shader program\n");
        return -1;
    }

    /*
     * Set up materials.
     */
    Material* pbr_material = create_material();
    material_set_program(pbr_material, pbr_shader_program);

    pbr_material->albedo[0] = 1.0f; // Red
    pbr_material->albedo[1] = 0.0f; // Green
    pbr_material->albedo[2] = 0.0f; // Blue

    Material* shape_material = create_material();
    material_set_program(shape_material, shape_shader_program);

    shape_material->albedo[0] = 0.0f; // Red
    shape_material->albedo[1] = 1.0f; // Green
    shape_material->albedo[2] = 0.0f; // Blue

    /*
     * Set up camera.
     */
    // Square-on to the z=0 shape plane, so panning along world XY stays in that plane
    vec3 camera_position = {0.0f, 0.0f, 300.0f};
    vec3 look_at_point = {0.0f, 0.0f, 0.0f};
    vec3 up_vector = {0.0f, 1.0f, 0.0f};
    float near_clip = 7.0f;
    float far_clip = 10000.0f;

    Camera* camera = create_camera();

    camera_set_position(camera, camera_position);
    camera_set_look_at(camera, look_at_point);
    camera_set_up(camera, up_vector);
    camera_set_orthographic(camera, ORTHO_HEIGHT, near_clip, far_clip);

    engine_set_camera(engine, camera);

    engine_update_view(engine);
    engine_update_projection(engine);

    /*
     * Import fbx model.
     */

    Scene* scene = create_scene();
    if (!scene) {
        fprintf(stderr, "Failed to create scene\n");
        return -1;
    }
    engine_add_scene(engine, scene);

    SceneNode* root_node = create_node();
    if (!root_node) {
        fprintf(stderr, "Failed to create root node\n");
        return -1;
    }

    scene_set_root(scene, root_node);

    // No light: under the 2D preset a material's albedo is the colour on screen.
    engine_set_2d_preset(engine, scene);

    if (scene_set_xyz_program(scene, xyz_shader_program) == GL_FALSE) {
        fprintf(stderr, "Failed to set scene xyz shader program\n");
        return -1;
    }

    /*
     * mesh1: Rectangle with no corner radius and no fill
     */
    /*Mesh* mesh1 = create_mesh();
    mesh1->material = shape_material;

    Rect rectangle1 = {
        .position = {-20.0f, -20.0f, 0.0f},
        .size = {20.0f, 20.0f, 0.0f},
        .corner_radius = 0.0f,
        .line_width = 0.2f,
        .filled = false
    };
    mesh_generate_rect(mesh1, &rectangle1);
    mesh_compute_aabb(mesh1);

    SceneNode* node1 = create_node();
    node_set_name(node1, "Rectangle 1");

    node_add_mesh(node1, mesh1);*/

    /*
     * mesh2: Rectangle with no corner radius and fill
     */
    Mesh* mesh2 = create_mesh();
    mesh2->material = pbr_material;

    Rect rectangle2 = {.position = {0.0f, -20.0f, 0.0f},
                       .size = {20.0f, 20.0f, 0.0f},
                       .corner_radius = 0.0f,
                       .line_width = 2.0f,
                       .filled = true};
    mesh_generate_rect(mesh2, &rectangle2);
    mesh_compute_aabb(mesh2);

    SceneNode* node2 = create_node();
    node_set_name(node2, "Rectangle 2");
    node_add_mesh(node2, mesh2);

    /*
     * mesh3: Rectangle with corner radius and no fill
     */
    /*Mesh* mesh3 = create_mesh();
    mesh3->material = shape_material;

    Rect rectangle3 = {
        .position = {-20.0f, 20.0f, 0.0f},
        .size = {20.0f, 20.0f, 0.0f},
        .corner_radius = 2.0f,
        .line_width = 2.0f,
        .filled = false
    };
    mesh_generate_rect(mesh3, &rectangle3);
    mesh_compute_aabb(mesh3);

    SceneNode* node3 = create_node();
    node_set_name(node3, "Rectangle 3");
    node_add_mesh(node3, mesh3);*/

    /*
     * mesh4: Rectangle with corner radius and fill
     */
    Mesh* mesh4 = create_mesh();
    mesh4->material = pbr_material;

    Rect rectangle4 = {.position = {0.0f, 20.0f, 0.0f},
                       .size = {20.0f, 20.0f, 0.0f},
                       .corner_radius = 2.0f,
                       .line_width = 2.0f,
                       .filled = true};
    mesh_generate_rect(mesh4, &rectangle4);
    mesh_compute_aabb(mesh4);

    SceneNode* node4 = create_node();
    node_set_name(node4, "Rectangle 4");
    node_add_mesh(node4, mesh4);

    /*
     * mesh5: Circle with no fill
     */
    /*Mesh* mesh5 = create_mesh();
    mesh5->material = shape_material;

    Circle circle1 = {
        .position = {-20.0f, -60.0f, 0.0f},
        .radius = 10.0f,
        .line_width = 10.0f,
        .filled = false
    };

    mesh_generate_circle(mesh5, &circle1);
    mesh_compute_aabb(mesh5);

    SceneNode* node5 = create_node();
    node_set_name(node5, "Circle 1");
    node_add_mesh(node5, mesh5);
    */

    /*
     * mesh6: Circle with fill
     */
    /*Mesh* mesh6 = create_mesh();
    mesh6->material = pbr_material;

    Circle circle2 = {
        .position = {20.0f, -60.0f, 0.0f},
        .radius = 10.0f,
        .line_width = 2.0f,
        .filled = true
    };

    mesh_generate_circle(mesh6, &circle2);
    mesh_compute_aabb(mesh6);

    SceneNode* node6 = create_node();
    node_set_name(node6, "Circle 2");
    node_add_mesh(node6, mesh6);

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
    */

    /*
     * mesh7: S-Shaped Bezier Curve
     */
    /*Mesh* mesh7 = create_mesh();
    mesh7->material = shape_material;

    Curve *bez7 = create_s_bezier_curve(start7, end7, 5.0f, 2.0f);
    mesh_generate_curve(mesh7, bez7);
    mesh_compute_aabb(mesh7);
    SceneNode* node7 = create_node();
    node_set_name(node7, "Bezier Curve 1");
    node_add_mesh(node7, mesh7);
    free(bez7);

    Mesh* mesh8 = create_mesh();
    mesh8->material = shape_material;

    Curve *bez8 = create_s_bezier_curve(start8, end8, 5.0f, 2.0f);
    mesh_generate_curve(mesh8, bez8);
    mesh_compute_aabb(mesh8);
    SceneNode* node8 = create_node();
    node_set_name(node8, "Bezier Curve 2");
    node_add_mesh(node8, mesh8);
    free(bez8);

    Mesh* mesh9 = create_mesh();
    mesh9->material = shape_material;

    Curve *bez9 = create_s_bezier_curve(start9, end9, 5.0f, 2.0f);
    mesh_generate_curve(mesh9, bez9);
    mesh_compute_aabb(mesh9);
    SceneNode* node9 = create_node();
    node_set_name(node9, "Bezier Curve 3");
    node_add_mesh(node9, mesh9);
    free(bez9);

    Mesh* mesh10 = create_mesh();
    mesh10->material = shape_material;

    Curve *bez10 = create_s_bezier_curve(start10, end10, 5.0f, 2.0f);
    mesh_generate_curve(mesh10, bez10);
    mesh_compute_aabb(mesh10);
    SceneNode* node10 = create_node();
    node_set_name(node10, "Bezier Curve 4");
    node_add_mesh(node10, mesh10);
    free(bez10);*/

    // node_add_child(root_node, node1);
    node_add_child(root_node, node2);
    // node_add_child(root_node, node3);
    node_add_child(root_node, node4);
    /*node_add_child(root_node, node5);
    node_add_child(root_node, node6);
    node_add_child(root_node, node7);
    node_add_child(root_node, node8);
    node_add_child(root_node, node9);
    node_add_child(root_node, node10);*/

    assert(root_node != NULL);

    node_upload_meshes(root_node);

    scene_print(scene);

    engine_set_show_gui(engine, false);
    engine_set_show_wireframe(engine, false);
    engine_set_show_xyz(engine, false);

    engine_run(engine, NULL, pre_render_callback, render_scene_callback);

    printf("Cleaning up...\n");
    free_engine(engine);

    printf("Goodbye Friend...\n");

    return 0;
}
