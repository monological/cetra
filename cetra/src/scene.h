
#ifndef _SCENE_H_
#define _SCENE_H_

#include <GLFW/glfw3.h>
#include <cglm/cglm.h>
#include <GL/glew.h>

#include "common.h"
#include "mesh.h"
#include "program.h"
#include "shader.h"
#include "light.h"
#include "camera.h"

/*
 * SceneNode
 */
typedef struct SceneNode {
    char* name;

    struct SceneNode* parent;
    struct SceneNode** children;

    size_t children_count;
    mat4 original_transform;
    mat4 global_transform;

    Mesh** meshes;
    size_t mesh_count;

    Light* light;

    Camera* camera;

    bool show_xyz;
    GLuint xyz_vao;
    GLuint xyz_vbo;
    ShaderProgram* xyz_shader_program;
} SceneNode;

// malloc
SceneNode* create_node();
void free_node(SceneNode* node);

// build graph
void add_child_node(SceneNode* node, SceneNode* child);

// meshes
void add_mesh_to_node(SceneNode* node, Mesh* mesh);

// setters
void set_node_name(SceneNode* node, const char* name);
void set_node_light(SceneNode* node, Light* light);
void set_node_camera(SceneNode* node, Camera* camera);

// xyz
void set_show_xyz_for_nodes(SceneNode* node, bool show_xyz);

// shaders
void set_shader_program_for_nodes(SceneNode* node, ShaderProgram* program);

// move
void apply_transform_to_nodes(SceneNode* node, mat4 transform);

/*
 * Scene
 */

typedef struct {
    Light* light;
    float distance;
} LightDistancePair;

typedef struct Scene {
    SceneNode* root_node;

    Light** lights;
    size_t light_count;

    Camera** cameras;
    size_t camera_count;

    Material** materials;
    size_t material_count;

    TexturePool* tex_pool;

    // used by all nodes
    ShaderProgram* xyz_shader_program;
    ShaderProgram* outlines_shader_program;
} Scene;

// malloc
Scene* create_scene();
void free_scene(Scene* scene);

// root
void set_scene_root_node(Scene* scene, SceneNode* root_node);

// camera
void set_scene_cameras(Scene* scene, Camera** cameras, size_t camera_count);
Camera* find_camera_by_name(Scene* scene, const char* name);

// light
void set_scene_lights(Scene* scene, Light** lights, size_t light_count);
void add_light_to_scene(Scene* scene, Light* light);
Light* find_light_by_name(Scene* scene, const char* name);
Light** get_closest_lights(Scene* scene, SceneNode* target_node, size_t max_lights,
                           size_t* returned_light_count);

// material
void add_material_to_scene(Scene* scene, Material* material);

// viz
GLboolean set_scene_xyz_shader_program(Scene* scene, ShaderProgram* xyz_shader_program);
GLboolean set_scene_outlines_shader_program(Scene* scene, ShaderProgram* outlines_shader_program);

// print
void print_scene_node(const SceneNode* node, int depth);
void print_scene(const Scene* scene);

// render
void upload_buffers_to_gpu_for_nodes(SceneNode* node);

#endif // _SCENE_H_
