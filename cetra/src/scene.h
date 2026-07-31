
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
#include "shadow.h"
#include "ibl.h"
#include "probe.h"
#include "animation.h"

// Forward-declared so scene.h and particle_system.h never include each other
// (particle_system.h forward-declares SceneNode in turn) -- avoids a cycle.
struct ParticleSystem;
// Directional wind field (wind.h); a scene-owned environmental object like sky.
struct Wind;

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
    mat4 prev_global_transform; // Last frame's global_transform, for motion vectors

    Mesh** meshes;
    size_t mesh_count;

    Light* light;

    Camera* camera;

    struct ParticleSystem* particle_system; // borrowed; owned by the Scene

    bool show_xyz;
    GLuint xyz_vao;
    GLuint xyz_vbo;
    ShaderProgram* xyz_shader_program;
} SceneNode;

// malloc
SceneNode* create_node();
void free_node(SceneNode* node);

// build graph
int add_child_node(SceneNode* node, SceneNode* child);

// meshes
int add_mesh_to_node(SceneNode* node, Mesh* mesh);

// setters
void set_node_name(SceneNode* node, const char* name);
void set_node_light(SceneNode* node, Light* light);
void set_node_camera(SceneNode* node, Camera* camera);
// Attach a particle system (borrowed) whose world transform is this node's.
void set_node_particle_system(SceneNode* node, struct ParticleSystem* sys);

// find
SceneNode* find_node_by_name(SceneNode* root, const char* name);

// xyz
void set_show_xyz_for_nodes(SceneNode* node, bool show_xyz);

// shaders
void set_shader_program_for_nodes(SceneNode* node, ShaderProgram* program);
void set_shader_programs_for_nodes(SceneNode* node, ShaderProgram* standard,
                                   ShaderProgram* skinned);

// move
void apply_transform_to_nodes(SceneNode* node, mat4 transform);

/*
 * Scene
 */

typedef struct Scene {
    SceneNode* root_node;

    Light** lights;
    size_t light_count;

    struct ParticleSystem** particle_systems; // owned (freed in free_scene)
    size_t particle_system_count;

    Camera** cameras;
    size_t camera_count;

    Material** materials;
    size_t material_count;

    TexturePool* tex_pool;

    // used by all nodes
    ShaderProgram* xyz_shader_program;
    ShaderProgram* outlines_shader_program;

    // Pre-allocated traversal stack (avoids per-frame malloc)
    SceneNode** traversal_stack;
    mat4* traversal_transforms; // Used by apply_transform_to_nodes
    size_t traversal_stack_capacity;
    size_t transparent_mesh_count;  // Late-pass meshes seen in this frame's opaque pass
    size_t transmissive_mesh_count; // Subset with transmission > 0; gates the mid-frame
                                    // opaque-color resolve refraction samples from
    size_t oit_mesh_count;          // Subset that is ALPHA_BLEND && !transmissive; gates the
                                    // weighted-blended OIT accumulate sub-pass

    // Shadow mapping
    ShadowSystem* shadow_system;

    // Image-Based Lighting
    IBLResources* ibl;
    ReflectionProbe* probe;     // local reflection probe (optional)
    struct SkyAtmosphere* sky;  // procedural sky feeding ibl (optional)
    struct Wind* wind;          // dominant directional wind (optional; owned)
    struct GIVolume* gi_volume; // indirect-diffuse probe grid (optional; owned)

    // Scalar material masks packed into one GL_TEXTURE_2D_ARRAY (built lazily
    // once the source textures have loaded; see mask_array.h). dirty triggers a
    // (re)build in the render loop when the async loader is idle.
    struct MaterialMaskArray* mask_array;
    bool mask_array_dirty;
    // POM (§4.11): height maps are resolved by filename convention once the async
    // texture loader drains (so the albedo/normal paths are populated); set after
    // the one-time resolve so the render loop does not re-scan every frame.
    bool heights_resolved;

    // Uniform ambient radiance in cd/m^2, used only when no IBL is loaded. A
    // real emitter with a real unit, so it scales with nothing and tracks
    // nothing; zero (the default) means an unlit surface is black. Replaced a
    // hardcoded 3%-of-white floor that could not be expressed correctly in
    // either space -- see spec 10.1 phase 5.
    vec3 ambient_radiance;

    bool render_skybox;
    float skybox_brightness;       // Linear env multiplier (tone mapping is the post pass's job)
    bool skybox_ground_projection; // Project env onto finite dome + ground
    float skybox_gp_radius;        // Dome radius in world units (meters)
    float skybox_gp_height;        // HDR capture height above ground
    bool shadow_catcher;           // Ground plane receiving shadows over the skybox
    float shadow_catcher_strength; // Shadow darkness 0..1

    // Skeletal Animation
    Skeleton** skeletons;
    size_t skeleton_count;
    Animation** animations;
    size_t animation_count;
} Scene;

// malloc
Scene* create_scene();
void free_scene(Scene* scene);

// root
void set_scene_root_node(Scene* scene, SceneNode* root_node);

// camera
void set_scene_cameras(Scene* scene, Camera** cameras, size_t camera_count);
int add_camera_to_scene(Scene* scene, Camera* camera);
Camera* find_camera_by_name(Scene* scene, const char* name);

// light
void set_scene_lights(Scene* scene, Light** lights, size_t light_count);
int add_light_to_scene(Scene* scene, Light* light);
Light* find_light_by_name(Scene* scene, const char* name);

// particle systems (scene-owned; ticked + rendered automatically by the engine)
int add_particle_system_to_scene(Scene* scene, struct ParticleSystem* sys);
// Advance every particle system's sim. Call from a fixed-timestep update (run_game
// does; an engine_run host may call it too). Rendering is automatic in
// render_current_scene.
void scene_update_particle_systems(Scene* scene, float dt, float t);

// material
int add_material_to_scene(Scene* scene, Material* material);

// wind (scene-owned; freed in free_scene). Replaces any existing wind.
void set_scene_wind(Scene* scene, struct Wind* wind);

// skeleton
int add_skeleton_to_scene(Scene* scene, Skeleton* skeleton);
Skeleton* find_skeleton_by_name(Scene* scene, const char* name);

// animation
int add_animation_to_scene(Scene* scene, Animation* animation);
Animation* find_animation_by_name(Scene* scene, const char* name);

// viz
GLboolean set_scene_xyz_shader_program(Scene* scene, ShaderProgram* xyz_shader_program);
GLboolean set_scene_outlines_shader_program(Scene* scene, ShaderProgram* outlines_shader_program);

// print
void print_scene_node(const SceneNode* node, int depth);
void print_scene(const Scene* scene);

// bounds
void compute_scene_bounds(Scene* scene, vec3 out_min, vec3 out_max);
void compute_scene_center_and_radius(Scene* scene, vec3 out_center, float* out_radius);

// render
void upload_buffers_to_gpu_for_nodes(SceneNode* node);

#endif // _SCENE_H_
