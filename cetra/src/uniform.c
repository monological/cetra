
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "uniform.h"
#include "common.h"
#include "util.h"
#include "ext/log.h"

UniformManager* create_uniform_manager(GLuint program_id) {
    UniformManager* mgr = malloc(sizeof(UniformManager));
    if (!mgr) {
        log_error("Failed to allocate UniformManager");
        return NULL;
    }
    mgr->cache = NULL;
    mgr->program_id = program_id;
    return mgr;
}

void free_uniform_manager(UniformManager* mgr) {
    if (!mgr)
        return;

    UniformBinding *current, *tmp;
    HASH_ITER(hh, mgr->cache, current, tmp) {
        HASH_DEL(mgr->cache, current);
        free(current->name);
        free(current);
    }
    free(mgr);
}

static UniformBinding* cache_uniform(UniformManager* mgr, const char* name) {
    // calloc, not malloc: `count` is the sentinel that says nothing has been
    // written yet, and a recycled allocation carrying a plausible count and
    // value would let the first write be skipped -- on a sampler unit that
    // means the program samples whatever is bound to unit 0.
    UniformBinding* binding = calloc(1, sizeof(UniformBinding));
    if (!binding) {
        log_error("Failed to allocate UniformBinding");
        return NULL;
    }

    binding->name = safe_strdup(name);
    if (!binding->name) {
        log_error("Failed to allocate uniform name");
        free(binding);
        return NULL;
    }

    binding->location = glGetUniformLocation(mgr->program_id, name);

    HASH_ADD_KEYPTR(hh, mgr->cache, binding->name, strlen(binding->name), binding);

    return binding;
}

// The binding, without disturbing its cached value. Only the setters below use
// this; everything outside this file goes through uniform_location, which
// invalidates. Keeping this one static is what makes that invalidation an
// enforced boundary rather than a habit.
static UniformBinding* uniform_binding(UniformManager* mgr, const char* name) {
    if (!mgr || !name)
        return NULL;

    UniformBinding* found = NULL;
    HASH_FIND_STR(mgr->cache, name, found);
    if (found)
        return found;

    // Cached even for a name the program does not have, so the negative result
    // costs one glGetUniformLocation rather than one per frame.
    return cache_uniform(mgr, name);
}

GLint uniform_location(UniformManager* mgr, const char* name) {
    UniformBinding* binding = uniform_binding(mgr, name);
    if (!binding)
        return -1;
    // Handing out the location means the caller may write the uniform behind
    // the setters' back, so what is cached here is no longer known to be what
    // the program holds.
    binding->count = 0;
    return binding->location;
}

// Whether the write should be issued: the program has this uniform, and does
// not already hold this value. Records the value as a side effect, and hands
// back the location so the caller does not resolve twice.
static bool uniform_write_wanted(UniformManager* mgr, const char* name, const float* value,
                                 unsigned char count, GLint* out_location) {
    UniformBinding* binding = uniform_binding(mgr, name);
    if (!binding || binding->location < 0)
        return false;
#if CETRA_CHECK_UNIFORM_BINDING
    // glUniform writes the BOUND program, so a set issued under another one
    // updates that program and records it against this one -- after which a
    // later legitimate write here is skipped as already-held. Nothing in GL
    // catches it and no pixel necessarily moves.
    //
    // Off even in debug builds, and not merely as thrift: this is a GL query
    // per uniform set, thousands a frame, and leaving it on would put roughly
    // six percent on the frame -- which is more than most of what this file's
    // caching wins, so every timing taken with it on would be wrong.
    GLint bound = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &bound);
    if ((GLuint)bound != mgr->program_id)
        log_error("uniform '%s' set on program %u while %d is bound", name, mgr->program_id, bound);
#endif
    *out_location = binding->location;
    if (binding->count == count && memcmp(binding->value, value, count * sizeof(float)) == 0)
        return false;
    memcpy(binding->value, value, count * sizeof(float));
    binding->count = count;
    return true;
}

void uniform_cache_standard(UniformManager* mgr) {
    if (!mgr)
        return;

    // Core transform uniforms
    uniform_location(mgr, "model");
    uniform_location(mgr, "view");
    uniform_location(mgr, "projection");
    uniform_location(mgr, "camPos");
    uniform_location(mgr, "time");

    // Render settings
    uniform_location(mgr, "renderMode");

    // Material properties
    uniform_location(mgr, "albedo");
    uniform_location(mgr, "metallic");
    uniform_location(mgr, "roughness");
    uniform_location(mgr, "ao");
    uniform_location(mgr, "materialOpacity");
    uniform_location(mgr, "ior");
    uniform_location(mgr, "filmThickness");

    // Texture samplers
    uniform_location(mgr, "albedoTex");
    uniform_location(mgr, "normalTex");
    uniform_location(mgr, "roughnessTex");
    uniform_location(mgr, "metalnessTex");
    uniform_location(mgr, "aoTex");
    uniform_location(mgr, "emissiveTex");
    uniform_location(mgr, "heightTex");
    uniform_location(mgr, "opacityTex");
    uniform_location(mgr, "sheenTex");

    // Texture exists flags
    uniform_location(mgr, "albedoTexExists");
    uniform_location(mgr, "normalTexExists");
    uniform_location(mgr, "roughnessTexExists");
    uniform_location(mgr, "metalnessTexExists");
    uniform_location(mgr, "aoTexExists");
    uniform_location(mgr, "emissiveTexExists");
    uniform_location(mgr, "heightTexExists");
    uniform_location(mgr, "opacityTexExists");
    uniform_location(mgr, "sheenTexExists");

    // Misc
    uniform_location(mgr, "lineWidth");
}

// Drift check for one dynamically-indexed shader array: the GLSL sizes are
// hardcoded literals the C constants only mirror. Such an array is active over
// its full declared size, so if element [count-1] fails to resolve while [0]
// exists, the shader array is SMALLER than the C side claims and ranged uploads
// would silently truncate. `array` is the base name, e.g. "lightSpaceMatrix".
static void warn_if_array_shorter(UniformManager* mgr, const char* array, size_t count) {
    char name[80];
    snprintf(name, sizeof(name), "%s[0]", array);
    if (uniform_location(mgr, name) < 0)
        return; // absent entirely -> not this shader's contract
    snprintf(name, sizeof(name), "%s[%zu]", array, count - 1);
    if (uniform_location(mgr, name) < 0)
        log_warn("Shader %s[] smaller than %zu elements -- GLSL constants drifted from the C mirror",
                 array, count);
}

// max_shadow_lights is the caster slot count; the per-layer arrays
// (lightSpaceMatrix, cascadeParams) span slots x cascades but upload as
// ranged calls from element 0, so only that location is cached.
void uniform_cache_shadows(UniformManager* mgr, size_t max_shadow_lights, size_t max_cascades,
                           size_t max_punctual_layers) {
    if (!mgr)
        return;

    uniform_location(mgr, "shadowMaps");
    uniform_location(mgr, "numShadowLights");
    uniform_location(mgr, "shadowBias");
    uniform_location(mgr, "shadowTexelSize");
    uniform_location(mgr, "cascadeCount");
    uniform_location(mgr, "cascadeSplits");
    uniform_location(mgr, "csmDebug");
    uniform_location(mgr, "lightSpaceMatrix[0]");
    uniform_location(mgr, "cascadeParams[0]");
    uniform_location(mgr, "punctualShadowMaps");
    uniform_location(mgr, "punctualShadowCount");
    uniform_location(mgr, "punctualShadowMatrix[0]");
    uniform_location(mgr, "punctualShadowMapSize");
    uniform_location(mgr, "msmEnabled");
    uniform_location(mgr, "msmBleed");

    warn_if_array_shorter(mgr, "lightSpaceMatrix", max_shadow_lights * max_cascades);
    warn_if_array_shorter(mgr, "punctualShadowMatrix", max_punctual_layers);
}

void uniform_set_int(UniformManager* mgr, const char* name, int value) {
    // Compared as a float, which is exact for what these carry: flags, sampler
    // units and small counts, all far inside the 24-bit mantissa.
    const float as_float = (float)value;
    GLint loc;
    if (uniform_write_wanted(mgr, name, &as_float, 1, &loc))
        glUniform1i(loc, value);
}

void uniform_set_float(UniformManager* mgr, const char* name, float value) {
    GLint loc;
    if (uniform_write_wanted(mgr, name, &value, 1, &loc))
        glUniform1f(loc, value);
}

void uniform_set_vec2(UniformManager* mgr, const char* name, const float* value) {
    GLint loc;
    if (uniform_write_wanted(mgr, name, value, 2, &loc))
        glUniform2fv(loc, 1, value);
}

void uniform_set_vec3(UniformManager* mgr, const char* name, const float* value) {
    GLint loc;
    if (uniform_write_wanted(mgr, name, value, 3, &loc))
        glUniform3fv(loc, 1, value);
}

void uniform_set_vec4(UniformManager* mgr, const char* name, const float* value) {
    GLint loc;
    if (uniform_write_wanted(mgr, name, value, 4, &loc))
        glUniform4fv(loc, 1, value);
}

void uniform_set_mat3(UniformManager* mgr, const char* name, const float* value) {
    GLint loc;
    if (uniform_write_wanted(mgr, name, value, 9, &loc))
        glUniformMatrix3fv(loc, 1, GL_FALSE, value);
}

void uniform_set_mat4(UniformManager* mgr, const char* name, const float* value) {
    GLint loc;
    if (uniform_write_wanted(mgr, name, value, 16, &loc))
        glUniformMatrix4fv(loc, 1, GL_FALSE, value);
}
