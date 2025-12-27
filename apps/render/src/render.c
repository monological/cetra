#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
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
#include "cetra/animation.h"
#include "cetra/ibl.h"
#include "cetra/app.h"

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
#define DEFAULT_WIDTH 1920
#define DEFAULT_HEIGHT 1080
#define MAX_ANIM_FILES 32

const float MIN_DIST = 2000.0f;
const float MAX_DIST = 3000.0f;
const float CAM_ANGULAR_SPEED = 0.5f;

/*
 * Command line arguments
 */
typedef struct {
    const char* model_path;
    const char* texture_dir;
    const char* hdr_path;
    const char* anim_files[MAX_ANIM_FILES];
    int anim_count;
    int width;
    int height;
    int show_help;
} RenderArgs;

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s -m <model> [options]\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -m, --model <path>     Model file (FBX, glTF, OBJ) [required]\n");
    fprintf(stderr, "  -t, --textures <dir>   Texture directory\n");
    fprintf(stderr, "  -e, --env <path>       HDR environment map for IBL\n");
    fprintf(stderr, "  -a, --anim <path>      Animation file (can be repeated)\n");
    fprintf(stderr, "  -W, --width <int>      Window width (default: %d)\n", DEFAULT_WIDTH);
    fprintf(stderr, "  -H, --height <int>     Window height (default: %d)\n", DEFAULT_HEIGHT);
    fprintf(stderr, "  -h, --help             Show this help message\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s -m character.fbx -t textures/\n", prog);
    fprintf(stderr, "  %s -m character.fbx -a walk.fbx -a run.fbx -e sky.hdr\n", prog);
}

static int parse_args(int argc, char** argv, RenderArgs* args) {
    memset(args, 0, sizeof(RenderArgs));
    args->width = DEFAULT_WIDTH;
    args->height = DEFAULT_HEIGHT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            args->show_help = 1;
            return 0;
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--model") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->model_path = argv[i];
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--textures") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->texture_dir = argv[i];
        } else if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--env") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->hdr_path = argv[i];
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--anim") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            if (args->anim_count >= MAX_ANIM_FILES) {
                fprintf(stderr, "Error: too many animation files (max %d)\n", MAX_ANIM_FILES);
                return -1;
            }
            args->anim_files[args->anim_count++] = argv[i];
        } else if (strcmp(argv[i], "-W") == 0 || strcmp(argv[i], "--width") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->width = atoi(argv[i]);
            if (args->width <= 0) {
                fprintf(stderr, "Error: invalid width '%s'\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "-H") == 0 || strcmp(argv[i], "--height") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->height = atoi(argv[i]);
            if (args->height <= 0) {
                fprintf(stderr, "Error: invalid height '%s'\n", argv[i]);
                return -1;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
            return -1;
        } else {
            // Positional argument - treat as model path for backwards compatibility
            if (!args->model_path) {
                args->model_path = argv[i];
            } else if (!args->texture_dir) {
                args->texture_dir = argv[i];
            } else if (!args->hdr_path) {
                args->hdr_path = argv[i];
            } else {
                fprintf(stderr, "Error: unexpected argument '%s'\n", argv[i]);
                return -1;
            }
        }
    }

    if (!args->show_help && !args->model_path) {
        fprintf(stderr, "Error: model path is required\n\n");
        return -1;
    }

    return 0;
}

/*
 * Mouse drag controller
 */
static MouseDragController* drag_controller = NULL;

/*
 * Animation playback state
 */
static AnimationState* anim_state = NULL;
static float last_frame_time = 0.0f;

/*
 * Callbacks
 */
void mouse_button_callback(Engine* engine, int button, int action, int mods) {
    if (drag_controller) {
        double x, y;
        glfwGetCursorPos(engine->window, &x, &y);
        mouse_drag_on_button(drag_controller, button, action, mods, x, y);
    }
}

void key_callback(Engine* engine, int key, int scancode, int action, int mods) {
    (void)scancode;

    // Camera movement (WASD, arrows, etc.)
    if (drag_controller && camera_controller_on_key(drag_controller, key, action, mods)) {
        return;
    }

    // App controls (only on press)
    if (action != GLFW_PRESS) {
        return;
    }

    switch (key) {
        case GLFW_KEY_ESCAPE:
            glfwSetWindowShouldClose(engine->window, GLFW_TRUE);
            break;
        case GLFW_KEY_G:
            set_engine_show_gui(engine, !engine->show_gui);
            break;
        case GLFW_KEY_X:
            set_engine_show_xyz(engine, !engine->show_xyz);
            break;
        case GLFW_KEY_T:
            set_engine_show_wireframe(engine, !engine->show_wireframe);
            break;
        case GLFW_KEY_1:
            engine->current_render_mode = RENDER_MODE_PBR;
            break;
        case GLFW_KEY_2:
            engine->current_render_mode = RENDER_MODE_NORMALS;
            break;
        case GLFW_KEY_3:
            engine->current_render_mode = RENDER_MODE_WORLD_POS;
            break;
        case GLFW_KEY_4:
            engine->current_render_mode = RENDER_MODE_TEX_COORDS;
            break;
        case GLFW_KEY_5:
            engine->current_render_mode = RENDER_MODE_TANGENT_SPACE;
            break;
        case GLFW_KEY_6:
            engine->current_render_mode = RENDER_MODE_FLAT_COLOR;
            break;
        default:
            break;
    }
}

void render_scene_callback(Engine* engine, Scene* current_scene) {
    SceneNode* root_node = current_scene->root_node;

    if (!engine || !root_node)
        return;

    float time_value = glfwGetTime();
    float delta_time = time_value - last_frame_time;
    last_frame_time = time_value;

    // Update animation
    if (anim_state && anim_state->playing) {
        update_animation(anim_state, delta_time);
        set_render_animation_state(anim_state);
    }

    // Update camera via drag controller
    if (drag_controller) {
        mouse_drag_update(drag_controller, time_value);
    }

    Transform transform = {.position = {0.0f, 0.0f, 0.0f},
                           .rotation = {0.0f, 0.0f, 0.0f},
                           .scale = {1.0f, 1.0f, 1.0f}};

    reset_and_apply_transform(&engine->model_matrix, &transform);

    apply_transform_to_nodes(root_node, engine->model_matrix);

    render_current_scene(engine, time_value);
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
    RenderArgs args;

    if (parse_args(argc, argv, &args) != 0) {
        print_usage(argv[0]);
        return -1;
    }

    if (args.show_help) {
        print_usage(argv[0]);
        return 0;
    }

    Engine* engine = create_engine("Cetra Engine", args.width, args.height);

    if (init_engine(engine) != 0) {
        fprintf(stderr, "Failed to initialize engine\n");
        return -1;
    }

    set_engine_error_callback(engine, app_error_callback);
    set_engine_mouse_button_callback(engine, mouse_button_callback);
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

    // Create skinned PBR shader for skeletal animation
    ShaderProgram* pbr_skinned_program = create_pbr_skinned_program();
    if (!pbr_skinned_program) {
        fprintf(stderr, "Failed to create PBR skinned shader program\n");
        return -1;
    }
    add_shader_program_to_engine(engine, pbr_skinned_program);

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

    // Create drag controller with auto-orbit
    drag_controller = create_mouse_drag_controller(engine);
    set_mouse_drag_auto_orbit(drag_controller, true, CAM_ANGULAR_SPEED, MIN_DIST, MAX_DIST);

    /*
     * Import model with async texture loading.
     */

    Scene* scene =
        create_scene_from_model_path_async(args.model_path, args.texture_dir, engine->async_loader);
    if (!scene) {
        fprintf(stderr, "Failed to import model: %s\n", args.model_path);
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

    if (args.hdr_path) {
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
        if (ibl && load_hdr_environment(ibl, args.hdr_path) == 0) {
            if (precompute_ibl(ibl, engine) == 0) {
                scene->ibl = ibl;
                scene->render_skybox = true;
                scene->skybox_exposure = 1.0f;
                printf("IBL loaded from: %s\n", args.hdr_path);
            } else {
                fprintf(stderr, "Failed to precompute IBL\n");
                free_ibl_resources(ibl);
            }
        } else {
            fprintf(stderr, "Failed to load HDR: %s\n", args.hdr_path);
            if (ibl)
                free_ibl_resources(ibl);
        }
    } else {
        // No IBL - use directional lights for illumination
        create_three_point_lights(scene, 3.0f);
    }

    // Load additional animation files if provided
    if (args.anim_count > 0 && scene->skeleton_count > 0) {
        Skeleton* skeleton = scene->skeletons[0];
        for (int i = 0; i < args.anim_count; i++) {
            int loaded = load_animations_from_file(scene, skeleton, args.anim_files[i]);
            if (loaded < 0) {
                fprintf(stderr, "Warning: failed to load animation '%s'\n", args.anim_files[i]);
            }
        }
        printf("Total animations: %zu\n", scene->animation_count);
    } else if (args.anim_count > 0) {
        fprintf(stderr, "Warning: animation files specified but model has no skeleton\n");
    }

    // Start playing the first animation if available
    if (scene->animation_count > 0 && scene->skeleton_count > 0) {
        anim_state = create_animation_state(scene->skeletons[0]);
        if (anim_state) {
            set_animation(anim_state, scene->animations[0]);
            anim_state->looping = true;
            play_animation(anim_state);
            printf("Playing animation: %s\n", scene->animations[0]->name);
        }
    }

    upload_buffers_to_gpu_for_nodes(scene->root_node);

    set_shader_programs_for_nodes(scene->root_node, pbr_shader_program, pbr_skinned_program);

    // Propagate transforms before computing bounds (needed for correct global_transform values)
    mat4 identity;
    glm_mat4_identity(identity);
    apply_transform_to_nodes(scene->root_node, identity);

    // Compute scene bounds and auto-position camera
    vec3 scene_center;
    float scene_radius;
    compute_scene_center_and_radius(scene, scene_center, &scene_radius);
    printf("Scene bounds: center=(%.2f, %.2f, %.2f), radius=%.2f\n", scene_center[0],
           scene_center[1], scene_center[2], scene_radius);

    // Position camera to view the entire scene
    float camera_distance = scene_radius * 2.5f;
    if (camera_distance < 1.0f)
        camera_distance = 100.0f; // Fallback for empty scenes

    vec3 auto_cam_pos = {scene_center[0], scene_center[1] + scene_radius * 0.3f,
                         scene_center[2] + camera_distance};
    set_camera_position(camera, auto_cam_pos);
    set_camera_look_at(camera, scene_center);

    // Adjust clip planes based on scene size
    float auto_near = fmaxf(scene_radius * 0.01f, 0.01f);
    float auto_far = scene_radius * 20.0f;
    if (auto_far < 100.0f)
        auto_far = 10000.0f;
    set_camera_perspective(camera, fov_radians, auto_near, auto_far);
    update_engine_camera_perspective(engine);
    printf("Camera clip planes: near=%.4f, far=%.2f\n", auto_near, auto_far);

    // Update orbit controller with appropriate distance
    camera->distance = camera_distance;
    camera->height = scene_center[1];
    set_mouse_drag_auto_orbit(drag_controller, true, CAM_ANGULAR_SPEED, camera_distance * 0.5f,
                              camera_distance * 2.0f);

    update_engine_camera_lookat(engine);

    print_scene(scene);

    set_engine_show_gui(engine, true);
    set_engine_show_fps(engine, true);
    set_engine_show_wireframe(engine, false);
    set_engine_show_xyz(engine, false);

    run_engine_render_loop(engine, render_scene_callback);

    printf("Cleaning up...\n");
    if (anim_state) {
        free_animation_state(anim_state);
    }
    free_mouse_drag_controller(drag_controller);
    free_engine(engine);

    printf("Goodbye Friend...\n");

    return 0;
}
