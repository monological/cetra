
#ifndef _SCENE_H_
#define _SCENE_H_

#include <cglm/cglm.h>
#include <GL/glew.h>

#include "mesh.h"
#include "program.h"
#include "shader.h"
#include "light.h"
#include "camera.h"

typedef struct {
    vec3 position;
    vec3 rotation;
    vec3 scale;
} Transform;

typedef struct SceneNode {
    char* name;

    struct SceneNode* parent;
    struct SceneNode** children;

    size_t children_count;
    mat4 original_transform; 
    mat4 local_transform; 
    mat4 global_transform;

    Mesh** meshes;
    size_t mesh_count;

    Light* light;
    char* light_name;

    Camera* camera;
    char* camera_name;

    Program* shader_program;
    Shader* vertex_shader;
    Shader* geometry_shader;
    Shader* fragment_shader;

    bool should_draw_axes;
    GLuint axesVAO;
    GLuint axesVBO;
    Program* axes_shader_program;
    Shader* axes_vertex_shader;
    Shader* axes_fragment_shader;
} SceneNode;

SceneNode* create_node();
void set_node_name(SceneNode* node, const char* name);
void set_node_light(SceneNode* node, Light* light);
void set_node_light_name(SceneNode* node, const char* name);
void set_node_camera(SceneNode* node, Camera* camera);
void set_node_camera_name(SceneNode* node, const char* name);

void set_program_for_node(SceneNode* node, Program* program);
void set_shaders_for_node(SceneNode* node, Shader* vertex, Shader* geometry, Shader* fragment);
void set_axes_program_for_node(SceneNode* node, Program* program);
void set_axes_shaders_for_node(SceneNode* node, Shader* vertex, Shader* fragment);
void set_should_draw_axes(SceneNode* node, bool should_draw_axes);

void setup_node_axes_buffers(SceneNode* node);
void draw_node_axes(SceneNode* node, const mat4 model, const mat4 view, const mat4 projection);
void add_child_node(SceneNode* self, SceneNode* child);
void render_node(SceneNode* self, mat4 model, mat4 view, mat4 projection);
void transform_node(SceneNode* node, Transform* transform);
void update_child_nodes(SceneNode* node, mat4 parentTransform);
void setup_node_meshes(SceneNode* node);
void free_node(SceneNode* node);

typedef struct Scene {
    SceneNode* root_node;

    Light** lights;
    size_t light_count;

    Camera** cameras;
    size_t camera_count;

    char* textureDirectory;
} Scene;

Scene* create_scene();
void set_scene_texture_directory(Scene* scene, const char* directory);
void set_scene_lights(Scene* scene, Light** lights, size_t light_count);
void set_scene_cameras(Scene* scene, Camera** cameras, size_t camera_count);
Camera* find_camera_by_name(Scene* scene, const char* name);
Light* find_light_by_name(Scene* scene, const char* name);
void free_scene(Scene* scene);


void print_scene_node(const SceneNode* node, int depth);
void print_scene(const Scene* scene);


#endif // _SCENE_H_


