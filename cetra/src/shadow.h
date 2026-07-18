#ifndef _SHADOW_H_
#define _SHADOW_H_

#include <GL/glew.h>
#include <cglm/cglm.h>
#include <stdbool.h>
#include <stddef.h>

#include "program.h"

#define MAX_SHADOW_LIGHTS       3
#define SHADOW_CASCADES         3 // Compile-time cascade ceiling (runtime: cascade_count)
#define DEFAULT_SHADOW_MAP_SIZE 2048
#define SHADOW_MAP_TEXTURE_UNIT 13

// Forward declarations
struct Scene;
struct Engine;

typedef struct ShadowCaster {
    GLuint fbo;
    GLuint depth_texture;
    int map_size;
    float bias;
    float normal_bias;
    bool initialized;
} ShadowCaster;

typedef struct ShadowSystem {
    ShadowCaster casters[MAX_SHADOW_LIGHTS];
    size_t active_count;
    int default_map_size;
    ShaderProgram* depth_program;
    float ortho_size;
    float near_plane;
    float far_plane;
    GLuint shadow_map_array;
    bool initialized;
    bool enabled;           // Master switch: off skips the depth pass and all receives
    bool pcss_enabled;      // Contact-hardening penumbra (off = fixed 3x3 PCF)
    float pcss_softness;    // Multiplier on each light's emitter size
    int cascade_count;      // Cascades per caster (1..SHADOW_CASCADES). 1 = the classic
                            // scene-fit single map, byte-identical to the pre-CSM path;
                            // layers stride by THIS value (layer = slot*cascade_count+c)
                            // so count 1 keeps master's layer indices
    int allocated_cascades; // Cascade capacity the map array was built for; a
                            // count change triggers a rebuild
    bool csm_debug;         // Tint fragments by cascade (the split/snap acceptance tool)
    // Per-layer state, count-strided (layer = slot * cascade_count + cascade)
    mat4 cascade_matrices[MAX_SHADOW_LIGHTS * SHADOW_CASCADES];
    vec4 cascade_params[MAX_SHADOW_LIGHTS * SHADOW_CASCADES]; // width, near, far, biasScale
    float cascade_splits[SHADOW_CASCADES];                    // View-depth far bound per cascade
} ShadowSystem;

// Creation and destruction
ShadowSystem* create_shadow_system(int default_map_size);
void free_shadow_system(ShadowSystem* system);

// Shadow caster management
int init_shadow_caster(ShadowCaster* caster, int map_size);
void free_shadow_caster(ShadowCaster* caster);

// Shadow map array management
int init_shadow_map_array(ShadowSystem* system);
void free_shadow_map_array(ShadowSystem* system);

// Depth pass rendering
void begin_shadow_pass(ShadowSystem* system, size_t caster_index);
void end_shadow_pass(ShadowSystem* system);

// Light space matrix computation
void compute_directional_light_space_matrix(vec3 direction, vec3 scene_center, float ortho_size,
                                            float near_plane, float far_plane, mat4 dest);

// Camera slice for cascade fitting (kept small so shadow.c never learns
// about Engine); forward must be normalized
typedef struct CascadeCamera {
    vec3 position;
    vec3 forward;
    float fov_radians;
    float aspect_ratio;
} CascadeCamera;

// Fit one cascade: ortho box sized from the [slice_near, slice_far] view
// slice's bounding sphere (rotation-invariant -> stable under orbit), center
// snapped to shadow-texel increments in light view space, eye pushed back by
// scene_pad so out-of-slice geometry toward the light still casts. Writes the
// matrix and (width, near, far, 1.0) into out_params; the caller sets .w to
// the per-cascade bias scale.
void compute_cascade_light_space_matrix(vec3 direction, const CascadeCamera* cam, float slice_near,
                                        float slice_far, float scene_pad, int map_size, mat4 dest,
                                        vec4 out_params);

// Shadow map binding for main render pass
void bind_shadow_maps_to_program(ShadowSystem* system, ShaderProgram* program,
                                 const int* shadow_light_indices);

// Main shadow rendering function
void render_shadow_depth_pass(struct Engine* engine, struct Scene* scene);

struct PostFX;

// Flatten this frame's shadow casters + their lights into postfx's fog block
// (mirrors reflection_probe_publish_to_postfx; postfx never learns about the
// shadow system): per caster the count-strided cascade matrices, travel
// direction, and color * intensity, plus the map array handle and cascade
// count. Publishes count 0 when shadows are off or absent — fog degrades to
// ambient haze.
void shadow_publish_to_postfx(const struct Scene* scene, struct PostFX* fx);

#endif // _SHADOW_H_
