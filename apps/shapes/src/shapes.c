#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

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

/*
 * CETRA MAIN
 */
int main() {

    // NO AA POLICY HERE, DELIBERATELY: the config's msaa_samples and taa are
    // left at their defaults. This app keeps the engine's 4x MSAA and no
    // temporal filter where the 3D apps drop to one sample plus TAA (spec
    // 11.103). Multisampling is what 2D line art wants, and nothing in this
    // scene moves, so a temporal accumulator would have only its own jitter to
    // integrate while switching on the aux G-buffer, the resolve, and the
    // eight passes keyed off taa_resolving.
    EngineConfig cfg = {.title = "Cetra Engine", .width = WIDTH, .height = HEIGHT};
    Engine* engine = create_engine(&cfg);
    if (!engine) {
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
    ShaderProgram* pbr_shader_program = engine_get_program(engine, CETRA_PROGRAM_PBR);
    if (!pbr_shader_program) {
        return -1;
    }

    ShaderProgram* xyz_shader_program = engine_get_program(engine, CETRA_PROGRAM_XYZ);
    if (!xyz_shader_program) {
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

    /*
     * Set up camera.
     */
    // Square-on to the z=0 shape plane, so panning along world XY stays in that plane
    CameraDesc camera_desc = {.position = {0.0f, 0.0f, 300.0f},
                              .ortho_height = ORTHO_HEIGHT,
                              .near = 7.0f,
                              .far = 10000.0f};
    Camera* camera = create_camera(&camera_desc);
    engine_set_camera(engine, camera);

    /*
     * Import fbx model.
     */

    Scene* scene = create_scene();
    if (!scene) {
        fprintf(stderr, "Failed to create scene\n");
        return -1;
    }
    engine_add_scene(engine, scene);
    SceneNode* root_node = scene->root_node;

    // No light: under the 2D preset a material's albedo is the colour on screen.
    engine_set_2d_preset(engine, scene);

    scene_set_xyz_program(scene, xyz_shader_program);

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

    SceneNode* node2 = create_node();
    node_set_name(node2, "Rectangle 2");
    node_add_mesh(node2, mesh2);

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

    SceneNode* node4 = create_node();
    node_set_name(node4, "Rectangle 4");
    node_add_mesh(node4, mesh4);

    node_add_child(root_node, node2);
    node_add_child(root_node, node4);

    scene_print(scene);

    engine_set_show_gui(engine, false);
    engine_set_show_wireframe(engine, false);
    engine_set_show_xyz(engine, false);

    engine_run(engine, NULL, NULL, NULL);

    printf("Cleaning up...\n");
    free_engine(engine);

    printf("Goodbye Friend...\n");

    return 0;
}
