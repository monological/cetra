
#ifndef _UNIFORM_H_
#define _UNIFORM_H_

#include <GL/glew.h>
#include <stddef.h>

#include "ext/uthash.h"

// Audit that every uniform set happens with its own program bound -- the one
// assumption the value cache below makes that GL does not enforce. Costs a GL
// query per set, so it is a deliberate build-time opt-in rather than a debug
// default: build with -DCETRA_CHECK_UNIFORM_BINDING=1 to run it.
#ifndef CETRA_CHECK_UNIFORM_BINDING
#define CETRA_CHECK_UNIFORM_BINDING 0
#endif

typedef struct UniformBinding {
    char* name;
    GLint location;
    // Last value written through a setter, so an unchanged one costs a compare
    // instead of a driver call. Sound because uniform state belongs to the
    // PROGRAM OBJECT in GL and survives glUseProgram; every site that relinks
    // (program.c reload_program_from_paths) builds a fresh UniformManager, so
    // no cache outlives the program whose state it describes.
    //
    // It also assumes the setters are called with that program BOUND, which is
    // this codebase's per-pass idiom rather than anything GL enforces -- a set
    // issued under a different program writes that one and records it here.
    // Asserted in debug builds for exactly that reason.
    //
    // 16 floats covers every setter up to mat4. count is the validity sentinel:
    // 0 means nothing has been written, so the first write always reaches GL,
    // which matters because the cache cannot know what the driver initialised
    // the uniform to.
    float value[16];
    unsigned char count;
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

// Get cached location (caches on first call if not found).
//
// Callers that use the location to write the uniform THEMSELVES -- the ranged
// glUniform*v array uploads the setters below cannot express -- must obtain it
// here, and this invalidates the value cache for that binding as a result. The
// two ways of writing one uniform would otherwise disagree, silently, and only
// on whichever program happened to use both.
GLint uniform_location(UniformManager* mgr, const char* name);

// Setters
void uniform_set_int(UniformManager* mgr, const char* name, int value);
void uniform_set_float(UniformManager* mgr, const char* name, float value);
void uniform_set_vec2(UniformManager* mgr, const char* name, const float* value);
void uniform_set_vec3(UniformManager* mgr, const char* name, const float* value);
void uniform_set_vec4(UniformManager* mgr, const char* name, const float* value);
void uniform_set_mat3(UniformManager* mgr, const char* name, const float* value);
void uniform_set_mat4(UniformManager* mgr, const char* name, const float* value);

// `count` consecutive vec3, from `count * 3` tightly packed floats.
//
// UNCACHED, unlike every setter above, and that is a correctness requirement rather than a
// choice: the cache slot is 16 floats, so an array would run off the end of it. The redundant
// write it therefore cannot skip is the price, which is why this is for tables a material
// changes rarely and not for anything set per draw.
void uniform_set_vec3_array(UniformManager* mgr, const char* name, const float* values, int count);

// Four ints from a tightly packed array. CACHED, unlike the vec3 array above -- four
// values fit the cache slot, so a repeated upload of an unchanged set costs no GL call.
void uniform_set_ivec4(UniformManager* mgr, const char* name, const int* value);

#endif // _UNIFORM_H_
