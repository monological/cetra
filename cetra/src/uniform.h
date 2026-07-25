
#ifndef _UNIFORM_H_
#define _UNIFORM_H_

#include <GL/glew.h>
#include <stddef.h>

#include "ext/uthash.h"

typedef struct UniformBinding {
    char* name;
    GLint location;
    UT_hash_handle hh;
} UniformBinding;

typedef struct UniformManager {
    UniformBinding* cache;
    GLuint program_id;
} UniformManager;

UniformManager* create_uniform_manager(GLuint program_id);
void free_uniform_manager(UniformManager* manager);

// Cache uniforms at setup time
void uniform_cache_standard(UniformManager* mgr);
void uniform_cache_shadows(UniformManager* mgr, size_t max_shadow_lights, size_t max_cascades,
                           size_t max_punctual_layers);

// Get cached location (caches on first call if not found)
GLint uniform_location(UniformManager* mgr, const char* name);

// Setters
void uniform_set_int(UniformManager* mgr, const char* name, int value);
void uniform_set_float(UniformManager* mgr, const char* name, float value);
void uniform_set_vec2(UniformManager* mgr, const char* name, const float* value);
void uniform_set_vec3(UniformManager* mgr, const char* name, const float* value);
void uniform_set_vec4(UniformManager* mgr, const char* name, const float* value);
void uniform_set_mat3(UniformManager* mgr, const char* name, const float* value);
void uniform_set_mat4(UniformManager* mgr, const char* name, const float* value);

#endif // _UNIFORM_H_
