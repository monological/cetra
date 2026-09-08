// A rotating wireframe sphere on cetra, shaded by the 2024 sketch's own GLSL:
// the fragment colour is a sine of time and screen position. The sketch was
// C++ on glm with a hand-built VAO; here the sphere comes from the engine's
// generator, its triangles are reduced to unique edges and drawn as lines,
// and the sketch's two shaders ride a material as they were. The renderer
// sets model, view and projection on any program that declares them, and the
// time uniform is set here each frame.
//
// Flags: -x hides the window, -f N exits after N frames, -S path writes the
// last frame as a binary PPM.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GL/glew.h>
#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/geometry.h"
#include "cetra/material.h"
#include "cetra/mesh.h"
#include "cetra/program.h"
#include "cetra/scene.h"
#include "cetra/uniform.h"

#define RADIUS 2.0f
#define RINGS 20
#define SECTORS 20
#define ROTATE_SPEED 0.4f

static const char* vertex_source = "#version 330 core\n"
                                   "layout (location = 0) in vec3 aPos;\n"
                                   "uniform mat4 model;\n"
                                   "uniform mat4 view;\n"
                                   "uniform mat4 projection;\n"
                                   "void main() {\n"
                                   "    gl_Position = projection * view * model * vec4(aPos, 1.0);\n"
                                   "}\n";

static const char* fragment_source = "#version 330 core\n"
                                     "out vec4 FragColor;\n"
                                     "uniform float time;\n"
                                     "void main() {\n"
                                     "    float r = sin(time + gl_FragCoord.x);\n"
                                     "    float g = sin(time + gl_FragCoord.y);\n"
                                     "    float b = sin(time + gl_FragCoord.z);\n"
                                     "    FragColor = vec4(r, g, b, 1.0);\n"
                                     "}\n";

static ShaderProgram* network_program;
static SceneNode* sphere_node;

static int compare_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

// Replace a triangle index list with its unique edges, so the mesh draws as
// lines without a triangle's shared edge appearing twice.
static void triangles_to_edges(Mesh* mesh) {
    size_t tri_count = mesh->index_count / 3;
    uint64_t* keys = malloc(tri_count * 3 * sizeof(uint64_t));
    if (!keys)
        return; // left as triangles
    for (size_t t = 0; t < tri_count; t++) {
        for (int k = 0; k < 3; k++) {
            unsigned int a = mesh->indices[t * 3 + k];
            unsigned int b = mesh->indices[t * 3 + (k + 1) % 3];
            if (a > b) {
                unsigned int tmp = a;
                a = b;
                b = tmp;
            }
            keys[t * 3 + k] = ((uint64_t)a << 32) | b;
        }
    }
    qsort(keys, tri_count * 3, sizeof(uint64_t), compare_u64);

    size_t edges = 0;
    for (size_t i = 0; i < tri_count * 3; i++) {
        if (i == 0 || keys[i] != keys[i - 1])
            keys[edges++] = keys[i];
    }

    unsigned int* indices = realloc(mesh->indices, edges * 2 * sizeof(unsigned int));
    if (!indices) {
        free(keys);
        return; // the old index list is still intact
    }
    for (size_t i = 0; i < edges; i++) {
        indices[i * 2] = (unsigned int)(keys[i] >> 32);
        indices[i * 2 + 1] = (unsigned int)(keys[i] & 0xffffffffu);
    }
    free(keys);
    mesh->indices = indices;
    mesh->index_count = edges * 2;
    set_mesh_draw_mode(mesh, MESH_LINES);
}

static void pre_render(Engine* engine, Scene* scene) {
    (void)scene;
    float t = (float)engine->render_time;

    // The sketch's model matrix: a turn about (1, 1, 0), then a half scale.
    mat4 rotation, scale;
    glm_rotate_make(rotation, ROTATE_SPEED * t, (vec3){1.0f, 1.0f, 0.0f});
    glm_scale_make(scale, (vec3){0.5f, 0.5f, 0.5f});
    glm_mat4_mul(rotation, scale, sphere_node->original_transform);

    // The one uniform the renderer does not know. Set with the program bound,
    // which is the uniform cache's only requirement.
    glUseProgram(network_program->id);
    uniform_set_float(network_program->uniforms, "time", t);

    update_engine_camera_lookat(engine);
    update_engine_camera_perspective(engine);
}

static void render(Engine* engine, Scene* scene) {
    engine_render_scene(engine, scene);
}

int main(int argc, char** argv) {
    bool headless = false;
    int frames = 0;
    const char* screenshot = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-x") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-S") == 0 && i + 1 < argc) {
            screenshot = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [-x] [-f frames] [-S out.ppm]\n", argv[0]);
            return 2;
        }
    }

    Engine* engine = create_engine("network", 800, 600);
    set_engine_headless(engine, headless);
    set_engine_exit_after_frames(engine, frames);
    if (screenshot)
        set_engine_screenshot_path(engine, screenshot);
    if (init_engine(engine) != 0) {
        fprintf(stderr, "init_engine failed\n");
        return 1;
    }
    set_engine_show_gui(engine, false);
    set_engine_show_fps(engine, !headless);

    network_program = create_program_from_source("network", vertex_source, fragment_source, NULL);
    if (!network_program) {
        fprintf(stderr, "the sketch's shaders did not compile\n");
        return 1;
    }
    add_shader_program_to_engine(engine, network_program); // the engine owns it now

    // The sketch's camera.
    Camera* camera = create_camera();
    set_camera_position(camera, (vec3){0.0f, 3.0f, 2.0f});
    set_camera_look_at(camera, (vec3){0.0f, 0.0f, 0.0f});
    set_camera_up_vector(camera, (vec3){0.0f, 1.0f, 0.0f});
    set_camera_perspective(camera, glm_rad(45.0f), 0.1f, 100.0f);
    set_engine_camera(engine, camera);

    Scene* scene = create_scene();
    add_scene_to_engine(engine, scene);
    SceneNode* root = create_node();
    set_scene_root_node(scene, root);
    // No post effects and a linear curve, so the sine colours land as computed;
    // negative values clamp to black, as they did in the sketch.
    engine_set_2d_defaults(engine, scene);

    Material* material = create_material();
    set_material_shader_program(material, network_program);

    Mesh* mesh = create_mesh();
    mesh->material = material;
    Sphere sphere = {.position = {0.0f, 0.0f, 0.0f},
                     .radius = RADIUS,
                     .segments_lon = SECTORS,
                     .segments_lat = RINGS};
    generate_sphere_to_mesh(mesh, &sphere);
    triangles_to_edges(mesh);
    calculate_aabb(mesh);

    sphere_node = create_node();
    set_node_name(sphere_node, "sphere");
    add_mesh_to_node(sphere_node, mesh);
    add_child_node(root, sphere_node);
    upload_buffers_to_gpu_for_nodes(root);

    engine_run(engine, NULL, pre_render, render);
    free_engine(engine);
    return 0;
}
