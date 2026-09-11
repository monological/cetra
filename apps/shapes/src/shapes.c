#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "cetra/app.h"
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

// A drag on a shape moves it, a drag on empty canvas pans, the wheel zooms.
static CanvasController* canvas = NULL;

/*
 * Callbacks
 */
void error_callback(int error, const char* description) {
    fprintf(stderr, "Error: %s\n", description);
}

void cursor_position_callback(Engine* engine, double xpos, double ypos) {
    (void)engine;
    canvas_on_cursor(canvas, xpos, ypos);
}

void mouse_button_callback(Engine* engine, int button, int action, int mods) {
    (void)engine;
    canvas_on_button(canvas, button, action, mods);
}

void scroll_callback(Engine* engine, double xoffset, double yoffset) {
    (void)engine;
    canvas_on_scroll(canvas, xoffset, yoffset);
}

void key_callback(Engine* engine, int key, int scancode, int action, int mods) {
    (void)scancode;
    (void)mods;

    if (action != GLFW_PRESS) {
        return;
    }

    switch (key) {
        case GLFW_KEY_G:
            engine->show_gui = !engine->show_gui;
            break;
        case GLFW_KEY_X:
            engine->show_xyz = !engine->show_xyz;
            break;
        case GLFW_KEY_T:
            engine->show_wireframe = !engine->show_wireframe;
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
    engine_set_scroll_callback(engine, scroll_callback);
    engine_set_key_callback(engine, key_callback);
    canvas = create_canvas_controller(engine);

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

    engine_run(engine, NULL, NULL, NULL);

    printf("Cleaning up...\n");
    free_canvas_controller(canvas);
    free_engine(engine);

    printf("Goodbye Friend...\n");

    return 0;
}
