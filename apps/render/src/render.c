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
#include "cetra/springbone.h"
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

// Total analytic key-light intensity split across the HDR's light lobes.
// Studio flashes overpower ambient by a few stops; matching that is what
// makes dark, rough materials (near-black armor) read instead of flattening
const float KEY_LIGHT_TOTAL_INTENSITY = 10.0f;

/*
 * Command line arguments
 */
typedef struct {
    const char* model_path;
    const char* texture_dir;
    const char* hdr_path;
    const char* anim_files[MAX_ANIM_FILES];
    int anim_count;
    const char* source_skeleton_path;  // Source skeleton for retargeting
    const char* screenshot_path;       // Save final frame here (PPM)
    int screenshot_every;              // Also save numbered frames every N frames
    float fov_deg;                     // Camera FOV in degrees (0 = default 50)
    float exposure;                    // Tonemap exposure override (0 = engine default)
    float ground_radius;               // Skybox ground projection dome radius (0 = default)
    float ground_height;               // HDR capture height above ground (0 = default)
    float camera_distance;             // Camera distance override in meters (0 = auto)
    int no_ground;                     // Disable skybox ground projection
    int no_key_light;                  // Pure IBL: skip the analytic key lights
    int no_springs;                    // Disable spring-bone secondary motion
    int no_ssao;                       // Disable screen-space ambient occlusion
    int ssao_debug;                    // Show the raw SSAO buffer
    float specular_aa;                 // Specular AA strength override (-1 = default)
    int width;
    int height;
    int headless;
    int max_frames; // Exit after this many frames (0 = run forever)
    int show_bones;
    int check_stretch; // One-shot CPU skinning stretch diagnostic
    int render_mode;   // RenderMode override for debugging (-1 = PBR)
    float orbit_yaw;   // Camera yaw around the model in degrees (0 = front)
    float orbit_pitch; // Camera pitch in degrees (0 = level, 90 = top-down)
    int show_help;
} RenderArgs;

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s -m <model> [options]\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -m, --model <path>     Model file (FBX, glTF, OBJ) [required]\n");
    fprintf(stderr, "  -t, --textures <dir>   Texture directory\n");
    fprintf(stderr, "  -e, --env <path>       HDR environment map for IBL\n");
    fprintf(stderr, "  -F, --fov <degrees>    Camera field of view (default: 50)\n");
    fprintf(stderr, "  -E, --exposure <f>     Tonemap exposure (default: engine default)\n");
    fprintf(stderr, "      --ground <radius>  Ground projection dome radius (default: 5)\n");
    fprintf(stderr, "      --ground-height <m> HDR capture height above ground (default: 1.2)\n");
    fprintf(stderr, "      --no-ground        Disable HDR ground projection (infinite skybox)\n");
    fprintf(stderr, "      --no-key-light     Pure IBL lighting (no analytic lights/shadows)\n");
    fprintf(stderr, "      --no-springs       Disable spring-bone secondary motion\n");
    fprintf(stderr, "      --no-ssao          Disable screen-space ambient occlusion\n");
    fprintf(stderr, "      --ssao-debug       Show the raw SSAO buffer\n");
    fprintf(stderr, "      --specular-aa <f>  Specular anti-aliasing strength (default: 1)\n");
    fprintf(stderr, "      --no-specular-aa   Disable specular anti-aliasing\n");
    fprintf(stderr, "  -D, --distance <m>     Camera distance from model (default: auto)\n");
    fprintf(stderr, "  -a, --anim <path>      Animation file (can be repeated)\n");
    fprintf(stderr, "  -s, --source <path>    Source skeleton for retargeting (T-pose)\n");
    fprintf(stderr, "  -W, --width <int>      Window width (default: %d)\n", DEFAULT_WIDTH);
    fprintf(stderr, "  -H, --height <int>     Window height (default: %d)\n", DEFAULT_HEIGHT);
    fprintf(stderr, "  -x, --headless         Hidden window (for debugging/CI)\n");
    fprintf(stderr, "  -b, --show-bones       Enable bone X-ray overlay\n");
    fprintf(stderr, "      --check-stretch    Report triangle edges stretched by skinning\n");
    fprintf(stderr, "  -f, --frames <int>     Exit after N frames\n");
    fprintf(stderr, "  -S, --screenshot <path> Save final frame as PPM on exit\n");
    fprintf(stderr, "      --screenshot-every <N> Also save numbered frames every N frames\n");
    fprintf(stderr, "  -h, --help             Show this help message\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s -m character.fbx -t textures/\n", prog);
    fprintf(stderr, "  %s -m character.fbx -a walk.fbx -s mixamo_tpose.fbx\n", prog);
}

static int parse_args(int argc, char** argv, RenderArgs* args) {
    memset(args, 0, sizeof(RenderArgs));
    args->width = DEFAULT_WIDTH;
    args->height = DEFAULT_HEIGHT;
    args->specular_aa = -1.0f; // -1 = keep the engine default

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
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--source") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->source_skeleton_path = argv[i];
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
        } else if (strcmp(argv[i], "-x") == 0 || strcmp(argv[i], "--headless") == 0) {
            args->headless = 1;
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--show-bones") == 0) {
            args->show_bones = 1;
        } else if (strcmp(argv[i], "--check-stretch") == 0) {
            args->check_stretch = 1;
        } else if (strcmp(argv[i], "-F") == 0 || strcmp(argv[i], "--fov") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->fov_deg = (float)atof(argv[i]);
            if (args->fov_deg <= 0.0f || args->fov_deg >= 180.0f) {
                fprintf(stderr, "Error: invalid fov '%s' (use 1-179 degrees)\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--render-mode") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->render_mode = atoi(argv[i]);
        } else if (strcmp(argv[i], "--yaw") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->orbit_yaw = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "--pitch") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->orbit_pitch = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "-E") == 0 || strcmp(argv[i], "--exposure") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->exposure = (float)atof(argv[i]);
            if (args->exposure <= 0.0f) {
                fprintf(stderr, "Error: invalid exposure '%s'\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--no-ground") == 0) {
            args->no_ground = 1;
        } else if (strcmp(argv[i], "--ground") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->ground_radius = (float)atof(argv[i]);
            if (args->ground_radius <= 0.0f) {
                fprintf(stderr, "Error: invalid ground radius '%s'\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--ground-height") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->ground_height = (float)atof(argv[i]);
            if (args->ground_height <= 0.0f) {
                fprintf(stderr, "Error: invalid ground height '%s'\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--no-key-light") == 0) {
            args->no_key_light = 1;
        } else if (strcmp(argv[i], "--no-ssao") == 0) {
            args->no_ssao = 1;
        } else if (strcmp(argv[i], "--ssao-debug") == 0) {
            args->ssao_debug = 1;
        } else if (strcmp(argv[i], "--specular-aa") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->specular_aa = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "--no-specular-aa") == 0) {
            args->specular_aa = 0.0f;
        } else if (strcmp(argv[i], "--no-springs") == 0) {
            args->no_springs = 1;
        } else if (strcmp(argv[i], "-D") == 0 || strcmp(argv[i], "--distance") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->camera_distance = (float)atof(argv[i]);
            if (args->camera_distance <= 0.0f) {
                fprintf(stderr, "Error: invalid camera distance '%s'\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "-S") == 0 || strcmp(argv[i], "--screenshot") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->screenshot_path = argv[i];
        } else if (strcmp(argv[i], "--screenshot-every") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->screenshot_every = atoi(argv[i]);
            if (args->screenshot_every <= 0) {
                fprintf(stderr, "Error: invalid screenshot interval '%s'\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--frames") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->max_frames = atoi(argv[i]);
            if (args->max_frames <= 0) {
                fprintf(stderr, "Error: invalid frame count '%s'\n", argv[i]);
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
 * Frame limit (--frames)
 */
static int max_frames = 0;
static int frames_rendered = 0;

/*
 * Stretch diagnostic (--check-stretch)
 */
static int check_stretch = 0;


/*
 * CPU-skin a vertex with the animation state's bone matrices, mirroring the
 * pbr_skinned vertex shader including its identity fallback.
 */
static void cpu_skin_vertex(const Mesh* mesh, const AnimationState* state, size_t v, vec3 out) {
    mat4 skin = GLM_MAT4_ZERO_INIT;
    float total = 0.0f;
    for (int i = 0; i < BONES_PER_VERTEX; i++) {
        int id = mesh->bone_ids[v * BONES_PER_VERTEX + i];
        float w = mesh->bone_weights[v * BONES_PER_VERTEX + i];
        if (id >= 0 && id < MAX_BONES && w > 0.0f) {
            for (int c = 0; c < 4; c++) {
                for (int r = 0; r < 4; r++) {
                    skin[c][r] += state->bone_matrices[id][c][r] * w;
                }
            }
            total += w;
        }
    }
    vec3 pos = {mesh->vertices[v * 3], mesh->vertices[v * 3 + 1], mesh->vertices[v * 3 + 2]};
    if (total < 0.001f) {
        glm_vec3_copy(pos, out);
        return;
    }
    glm_mat4_mulv3(skin, pos, 1.0f, out);
}

/*
 * Report triangle edges whose skinned length grew far beyond their bind
 * length. Rigid geometry stays at ratio ~1; skinning defects (vertices bound
 * to the wrong bone) show up as large ratios.
 */
static void report_skinning_stretch(SceneNode* node, const AnimationState* state) {
    if (!node)
        return;

    for (size_t m = 0; m < node->mesh_count; m++) {
        Mesh* mesh = node->meshes[m];
        if (!mesh || !mesh->is_skinned || !mesh->bone_ids || !mesh->indices ||
            mesh->draw_mode != TRIANGLES)
            continue;

        float worst_ratio = 0.0f;
        size_t worst_va = 0, worst_vb = 0;
        size_t stretched_edges = 0;

        for (size_t t = 0; t + 2 < mesh->index_count; t += 3) {
            for (int e = 0; e < 3; e++) {
                size_t va = mesh->indices[t + e];
                size_t vb = mesh->indices[t + (e + 1) % 3];
                if (va >= mesh->vertex_count || vb >= mesh->vertex_count)
                    continue;

                vec3 bind_a = {mesh->vertices[va * 3], mesh->vertices[va * 3 + 1],
                               mesh->vertices[va * 3 + 2]};
                vec3 bind_b = {mesh->vertices[vb * 3], mesh->vertices[vb * 3 + 1],
                               mesh->vertices[vb * 3 + 2]};
                float bind_len = glm_vec3_distance(bind_a, bind_b);
                if (bind_len < 1e-6f)
                    continue;

                vec3 skin_a = {0.0f, 0.0f, 0.0f};
                vec3 skin_b = {0.0f, 0.0f, 0.0f};
                cpu_skin_vertex(mesh, state, va, skin_a);
                cpu_skin_vertex(mesh, state, vb, skin_b);
                float skin_len = glm_vec3_distance(skin_a, skin_b);

                float ratio = skin_len / bind_len;
                if (ratio > 3.0f)
                    stretched_edges++;
                if (ratio > worst_ratio) {
                    worst_ratio = ratio;
                    worst_va = va;
                    worst_vb = vb;
                }
            }
        }

        if (stretched_edges > 0) {
            printf("STRETCH mesh[%zu] (%zu verts): %zu edges >3x, worst=%.1fx (v%zu <-> v%zu)\n",
                   m, mesh->vertex_count, stretched_edges, worst_ratio, worst_va, worst_vb);
            for (int side = 0; side < 2; side++) {
                size_t v = side == 0 ? worst_va : worst_vb;
                printf("  v%zu bones:", v);
                for (int i = 0; i < BONES_PER_VERTEX; i++) {
                    int id = mesh->bone_ids[v * BONES_PER_VERTEX + i];
                    float w = mesh->bone_weights[v * BONES_PER_VERTEX + i];
                    if (id >= 0 && w > 0.0f && mesh->skeleton &&
                        (size_t)id < mesh->skeleton->bone_count) {
                        printf(" '%s'=%.2f", mesh->skeleton->bones[id].name, w);
                    }
                }
                printf("\n");
            }
        } else {
            printf("STRETCH mesh[%zu] (%zu verts): ok, worst=%.2fx\n", m, mesh->vertex_count,
                   worst_ratio);
        }
    }

    for (size_t c = 0; c < node->children_count; c++) {
        report_skinning_stretch(node->children[c], state);
    }
}

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

    if (max_frames > 0 && ++frames_rendered >= max_frames) {
        glfwSetWindowShouldClose(engine->window, GLFW_TRUE);
    }

    float time_value = glfwGetTime();
    float delta_time = time_value - last_frame_time;
    last_frame_time = time_value;

    // Update animation
    if (anim_state && anim_state->playing) {
        update_animation(anim_state, delta_time);
        set_render_animation_state(anim_state);

        // One-shot stretch diagnostic once the animation is mid-pose
        if (check_stretch && frames_rendered == 60) {
            printf("\n===== SKINNING STRETCH CHECK (time=%.1f ticks) =====\n",
                   anim_state->current_time);
            report_skinning_stretch(root_node, anim_state);
            printf("===== END STRETCH CHECK =====\n\n");
        }
    }

    // Sync the camera zoom limit with the ground-projection fade start every
    // frame so GUI changes to Dome Radius take effect (enforcement lives in
    // camera_enforce_max_distance, applied by mouse_drag_update).
    if (engine->camera) {
        engine->camera->max_distance =
            (current_scene->render_skybox && current_scene->skybox_ground_projection)
                ? SKYBOX_GP_FADE_START * current_scene->skybox_gp_radius
                : 0.0f;
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

    // Render skeleton bones if enabled
    if (engine->show_bones) {
        Skeleton* skel = (current_scene->skeleton_count > 0) ? current_scene->skeletons[0] : NULL;
        render_skeleton_bones(engine, skel, anim_state);
    }
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

    set_engine_headless(engine, args.headless != 0);
    set_engine_screenshot_path(engine, args.screenshot_path);
    set_engine_screenshot_every(engine, args.screenshot_every);
    max_frames = args.max_frames;
    check_stretch = args.check_stretch;

    if (init_engine(engine) != 0) {
        fprintf(stderr, "Failed to initialize engine\n");
        return -1;
    }

    if (args.exposure > 0.0f && engine->postfx) {
        engine->postfx->exposure = args.exposure;
    }
    if (args.no_ssao && engine->postfx) {
        engine->postfx->ssao_enabled = false;
    }
    if (args.ssao_debug && engine->postfx) {
        engine->postfx->ssao_debug = true;
    }
    if (args.specular_aa >= 0.0f) {
        engine->specular_aa_strength = args.specular_aa;
    }
    if (args.render_mode > 0) {
        engine->current_render_mode = (RenderMode)args.render_mode;
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
    float fov_radians = glm_rad(args.fov_deg > 0.0f ? args.fov_deg : 50.0f);
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

    // Create drag controller with auto-orbit (fixed camera in headless mode for
    // deterministic, comparable screenshots)
    drag_controller = create_mouse_drag_controller(engine);
    set_mouse_drag_auto_orbit(drag_controller, !args.headless, CAM_ANGULAR_SPEED, MIN_DIST,
                              MAX_DIST);

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
        IBLResources* ibl = create_ibl_resources();
        if (ibl && load_hdr_environment(ibl, args.hdr_path) == 0) {
            if (precompute_ibl(ibl, engine) == 0) {
                scene->ibl = ibl;
                scene->render_skybox = true;
                scene->skybox_brightness = 1.0f;
                // Ground projection on by default; --no-ground restores the
                // infinite skybox
                scene->skybox_ground_projection = !args.no_ground;
                scene->skybox_gp_radius =
                    args.ground_radius > 0.0f ? args.ground_radius : 5.0f;
                scene->skybox_gp_height =
                    args.ground_height > 0.0f ? args.ground_height : 1.2f;
                printf("IBL loaded from: %s\n", args.hdr_path);
            } else {
                fprintf(stderr, "Failed to precompute IBL\n");
                free_ibl_resources(ibl);
                ibl = NULL;
            }
        } else {
            fprintf(stderr, "Failed to load HDR: %s\n", args.hdr_path);
            if (ibl)
                free_ibl_resources(ibl);
            ibl = NULL;
        }

        // Add one soft shadow-casting key light per bright lobe extracted
        // from the HDR, so each visible studio light casts its own shadow.
        // Total analytic intensity stays ~1.0, split by lobe energy.
        // --no-key-light gives pure image-based lighting instead.
        if (!args.no_key_light) {
            int lobes = (scene->ibl && scene->ibl->light_count > 0) ? scene->ibl->light_count : 1;
            for (int i = 0; i < lobes; i++) {
                Light* key = create_light();
                if (!key)
                    continue;

                char light_name[32];
                snprintf(light_name, sizeof(light_name), "key_light_%d", i);
                set_light_name(key, light_name);
                set_light_type(key, LIGHT_DIRECTIONAL);

                vec3 key_dir = {-0.4f, -0.7f, -0.6f}; // Fallback if no lobes
                float intensity = KEY_LIGHT_TOTAL_INTENSITY;
                if (scene->ibl && scene->ibl->light_count > 0) {
                    glm_vec3_negate_to(scene->ibl->light_dirs[i], key_dir);
                    // Blend the HDR energy split toward an even split so weak
                    // fill lobes still light their side (photographic fill
                    // ratio) instead of leaving it to the rim light alone
                    float share = 0.5f * scene->ibl->light_energies[i] + 0.5f / (float)lobes;
                    intensity = share * KEY_LIGHT_TOTAL_INTENSITY;
                }
                set_light_direction(key, key_dir);
                set_light_intensity(key, intensity);
                set_light_color(key, (vec3){1.0f, 1.0f, 1.0f});
                set_light_cast_shadows(key, true);
                add_light_to_scene(scene, key);

                SceneNode* key_node = create_node();
                set_node_light(key_node, key);
                set_node_name(key_node, light_name);
                add_child_node(scene->root_node, key_node);
            }

            // Ground the model: catch the key light shadows on the
            // projected environment floor
            scene->shadow_catcher = scene->ibl != NULL;
        }
    } else {
        // No IBL - use directional lights for illumination
        create_three_point_lights(scene, 3.0f);
    }

    // Load additional animation files if provided
    // Enable retargeting by default to support Mixamo animations on custom rigs
    size_t first_cli_anim = 0; // Index of first animation loaded via -a (0 = embedded)
    if (args.anim_count > 0 && scene->skeleton_count > 0) {
        first_cli_anim = scene->animation_count;
        Skeleton* skeleton = scene->skeletons[0];

        // Load source skeleton for retargeting if provided
        Skeleton* source_skeleton = NULL;
        if (args.source_skeleton_path) {
            Scene* source_scene = create_scene_from_model_path(args.source_skeleton_path, NULL);
            if (source_scene && source_scene->skeleton_count > 0) {
                source_skeleton = source_scene->skeletons[0];
                printf("Loaded source skeleton: %zu bones from '%s'\n",
                       source_skeleton->bone_count, args.source_skeleton_path);
            } else {
                fprintf(stderr, "Warning: failed to load source skeleton '%s'\n",
                        args.source_skeleton_path);
            }
        }

        for (int i = 0; i < args.anim_count; i++) {
            int loaded = load_animations_from_file(scene, skeleton, args.anim_files[i],
                                                   true, source_skeleton);
            if (loaded < 0) {
                fprintf(stderr, "Warning: failed to load animation '%s'\n", args.anim_files[i]);
            }
        }
        printf("Total animations: %zu\n", scene->animation_count);
    } else if (args.anim_count > 0) {
        fprintf(stderr, "Warning: animation files specified but model has no skeleton\n");
    }

    // Start playing an animation if available; prefer the first one loaded via -a
    // over animations embedded in the model file
    if (scene->animation_count > 0 && scene->skeleton_count > 0) {
        size_t play_idx = (first_cli_anim < scene->animation_count) ? first_cli_anim : 0;
        anim_state = create_animation_state(scene->skeletons[0]);
        if (anim_state) {
            set_animation(anim_state, scene->animations[play_idx]);
            anim_state->looping = true;
            play_animation(anim_state);
            printf("Playing animation: %s (index %zu of %zu)\n", scene->animations[play_idx]->name,
                   play_idx, scene->animation_count);

            // Spring-bone secondary motion for chains no animation drives.
            // Only hair: the "sheath root" chain is a rigid metal belt on
            // this rig and must stay welded to its authored pose.
            anim_state->springs = create_spring_bone_system(scene->skeletons[0]);
            if (anim_state->springs) {
                int n = spring_bone_add_chains_by_prefix(anim_state->springs, "hair");
                if (n > 0) {
                    printf("Spring bones: %d chain(s) under prefix 'hair'\n", n);
                }
                if (args.no_springs) {
                    anim_state->springs->enabled = false;
                }
            }
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

    // Fit the shadow frustum to the scene; the library default (ortho 2000)
    // leaves a human-sized model with no effective shadow map resolution
    if (scene->shadow_system && scene_radius > 0.0f) {
        scene->shadow_system->ortho_size = scene_radius * 2.0f;
        scene->shadow_system->near_plane = 0.1f;
        scene->shadow_system->far_plane = scene_radius * 8.0f;
    }

    // Position camera to view the entire scene
    float camera_distance = scene_radius * 2.5f;
    if (camera_distance < 1.0f)
        camera_distance = 100.0f; // Fallback for empty scenes
    if (args.camera_distance > 0.0f)
        camera_distance = args.camera_distance;

    float yaw = glm_rad(args.orbit_yaw);
    float pitch = glm_rad(args.orbit_pitch);
    vec3 auto_cam_pos = {scene_center[0] + camera_distance * cosf(pitch) * sinf(yaw),
                         scene_center[1] + scene_radius * 0.3f + camera_distance * sinf(pitch),
                         scene_center[2] + camera_distance * cosf(pitch) * cosf(yaw)};
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
    float orbit_max = camera_distance * 2.0f;
    if (scene->skybox_ground_projection) {
        // Keep the camera where the ground projection renders at full
        // strength; no reachable view ever shows the blend toward the
        // infinite skybox. Raising Dome Radius extends the zoom range.
        camera->max_distance = SKYBOX_GP_FADE_START * scene->skybox_gp_radius;
        orbit_max = fminf(orbit_max, camera->max_distance);
    }
    set_mouse_drag_auto_orbit(drag_controller, !args.headless, CAM_ANGULAR_SPEED,
                              fminf(camera_distance * 0.5f, orbit_max), orbit_max);

    update_engine_camera_lookat(engine);

    print_scene(scene);

    set_engine_show_gui(engine, true);
    set_engine_show_fps(engine, true);
    set_engine_show_wireframe(engine, false);
    set_engine_show_xyz(engine, false);
    engine->show_bones = args.show_bones != 0;

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
