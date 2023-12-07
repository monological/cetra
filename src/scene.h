
#ifndef _SCENE_H_
#define _SCENE_H_

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
    mat4 local_transform; 
    mat4 global_transform;

    Mesh** meshes;
    size_t mesh_count;

    Light* light;

    Camera* camera;
    char* camera_name;

    ShaderProgram* shader_program;

    bool show_axes;
    GLuint axesVAO;
    GLuint axesVBO;
    ShaderProgram* axes_shader_program;
} SceneNode;

// malloc
SceneNode* create_node();
void free_node(SceneNode* node);

// build graph
void add_child_node(SceneNode* node, SceneNode* child);

// setters
void set_node_name(SceneNode* node, const char* name);
void set_node_light(SceneNode* node, Light* light);
void set_node_camera(SceneNode* node, Camera* camera);

// axes
void set_show_axes_for_nodes(SceneNode* node, bool show_axes);

// shaders
void set_program_for_nodes(SceneNode* node, ShaderProgram* program);


// buffers
void upload_buffers_to_gpu_for_nodes(SceneNode* node);

// move
void transform_node(SceneNode* node, Transform* transform);
void apply_transform_to_nodes(SceneNode* node, mat4 parentTransform);

// render
void render_nodes(SceneNode* node, Camera *camera, 
        mat4 model, mat4 view, mat4 projection, 
        float time_value, RenderMode render_mode);

/*
 * Scene
 */

typedef struct Scene {
    SceneNode* root_node;

    Light** lights;
    size_t light_count;

    Camera** cameras;
    size_t camera_count;

    char* texture_directory;

    // used by all nodes
    ShaderProgram* axes_shader_program;
} Scene;

// malloc
Scene* create_scene();
void free_scene(Scene* scene);

// set
void set_scene_texture_directory(Scene* scene, const char* directory);
void set_scene_lights(Scene* scene, Light** lights, size_t light_count);
void set_scene_cameras(Scene* scene, Camera** cameras, size_t camera_count);

// find
Camera* find_camera_by_name(Scene* scene, const char* name);
Light* find_light_by_name(Scene* scene, const char* name);

// axes
void setup_scene_axes(Scene* scene);

// print
void print_scene_node(const SceneNode* node, int depth);
void print_scene(const Scene* scene);


#endif // _SCENE_H_


