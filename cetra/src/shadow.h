#ifndef _SHADOW_H_
#define _SHADOW_H_

#include <GL/glew.h>
#include <cglm/cglm.h>
#include <stdbool.h>
#include <stddef.h>

#include "program.h"

#define MAX_SHADOW_LIGHTS       3
#define DEFAULT_SHADOW_MAP_SIZE 2048
#define SHADOW_MAP_TEXTURE_UNIT 13

// Forward declarations
struct Scene;
struct Engine;

typedef struct ShadowCaster {
    GLuint fbo;
    GLuint depth_texture;
    int map_size;
    mat4 light_space_matrix;
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
    bool enabled;        // Master switch: off skips the depth pass and all receives
    bool pcss_enabled;   // Contact-hardening penumbra (off = fixed 3x3 PCF)
    float pcss_softness; // Multiplier on each light's emitter size
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

// Shadow map binding for main render pass
void bind_shadow_maps_to_program(ShadowSystem* system, ShaderProgram* program,
                                 const int* shadow_light_indices);

// Main shadow rendering function
void render_shadow_depth_pass(struct Engine* engine, struct Scene* scene);

struct PostFX;

// Flatten this frame's shadow casters + their lights into postfx's fog block
// (mirrors reflection_probe_publish_to_postfx; postfx never learns about the
// shadow system): per caster the light-space matrix, travel direction, and
// color * intensity, plus the map array handle. Publishes count 0 when
// shadows are off or absent — fog degrades to ambient haze.
void shadow_publish_to_postfx(const struct Scene* scene, struct PostFX* fx);

#endif // _SHADOW_H_
