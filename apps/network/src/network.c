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

#define RADIUS       2.0f
#define RINGS        20
#define SECTORS      20
#define ROTATE_SPEED 0.4f

static const char* vertex_source =
    "#version 330 core\n"
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
    mesh_set_draw_mode(mesh, MESH_LINES);
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

    engine_update_view(engine);
    engine_update_projection(engine);
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

    EngineConfig cfg = {.title = "network", .width = 800, .height = 600, .headless = headless};
    Engine* engine = create_engine(&cfg);
    if (!engine)
        return 1;
    engine_set_exit_after_frames(engine, frames);
    if (screenshot)
        engine_set_screenshot_path(engine, screenshot);
    engine_set_show_gui(engine, false);
    engine_set_show_fps(engine, !headless);

    network_program = create_program_from_source("network", vertex_source, fragment_source, NULL);
    if (!network_program) {
        fprintf(stderr, "the sketch's shaders did not compile\n");
        return 1;
    }
    engine_add_program(engine, network_program); // the engine owns it now

    // The sketch's camera.
    CameraDesc camera = {
        .position = {0.0f, 3.0f, 2.0f}, .fov = glm_rad(45.0f), .near = 0.1f, .far = 100.0f};
    engine_set_camera(engine, create_camera(&camera));

    Scene* scene = create_scene();
    engine_add_scene(engine, scene);
    SceneNode* root = create_node();
    scene_set_root(scene, root);
    // No post effects and a linear curve, so the sine colours land as computed;
    // negative values clamp to black, as they did in the sketch.
    engine_set_2d_preset(engine, scene);

    Material* material = create_material();
    material_set_program(material, network_program);

    Mesh* mesh = create_mesh();
    mesh->material = material;
    Sphere sphere = {.position = {0.0f, 0.0f, 0.0f},
                     .radius = RADIUS,
                     .segments_lon = SECTORS,
                     .segments_lat = RINGS};
    mesh_generate_sphere(mesh, &sphere);
    triangles_to_edges(mesh);
    mesh_compute_aabb(mesh);

    sphere_node = create_node();
    node_set_name(sphere_node, "sphere");
    node_add_mesh(sphere_node, mesh);
    node_add_child(root, sphere_node);
    node_upload_meshes(root);

    engine_run(engine, NULL, pre_render, render);
    free_engine(engine);
    return 0;
}
