
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

static GLint cache_uniform(UniformManager* mgr, const char* name) {
    UniformBinding* binding = malloc(sizeof(UniformBinding));
    if (!binding) {
        log_error("Failed to allocate UniformBinding");
        return -1;
    }

    binding->name = safe_strdup(name);
    if (!binding->name) {
        log_error("Failed to allocate uniform name");
        free(binding);
        return -1;
    }

    binding->location = glGetUniformLocation(mgr->program_id, name);

    HASH_ADD_KEYPTR(hh, mgr->cache, binding->name, strlen(binding->name), binding);

    return binding->location;
}

GLint uniform_location(UniformManager* mgr, const char* name) {
    if (!mgr || !name)
        return -1;

    UniformBinding* found = NULL;
    HASH_FIND_STR(mgr->cache, name, found);

    if (found)
        return found->location;

    return cache_uniform(mgr, name);
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

// max_shadow_lights is the caster slot count; the per-layer arrays
// (lightSpaceMatrix, cascadeParams) span slots x cascades but upload as
// ranged calls from element 0, so only that location is cached.
void uniform_cache_shadows(UniformManager* mgr, size_t max_shadow_lights, size_t max_cascades) {
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

    // Drift check: the GLSL array sizes are hardcoded literals the C
    // constants only mirror. A dynamically-indexed array is active over its
    // full declared size, so if the last layer's name fails to resolve
    // while element 0 exists, the shader arrays are SMALLER than the C
    // side claims and ranged uploads would silently truncate.
    char name[64];
    snprintf(name, sizeof(name), "lightSpaceMatrix[%zu]", max_shadow_lights * max_cascades - 1);
    if (uniform_location(mgr, "lightSpaceMatrix[0]") >= 0 && uniform_location(mgr, name) < 0) {
        log_warn("Shader lightSpaceMatrix[] smaller than %zu layers -- "
                 "GLSL cascade constants drifted from the C mirrors",
                 max_shadow_lights * max_cascades);
    }
}

void uniform_set_int(UniformManager* mgr, const char* name, int value) {
    GLint loc = uniform_location(mgr, name);
    if (loc >= 0)
        glUniform1i(loc, value);
}

void uniform_set_float(UniformManager* mgr, const char* name, float value) {
    GLint loc = uniform_location(mgr, name);
    if (loc >= 0)
        glUniform1f(loc, value);
}

void uniform_set_vec2(UniformManager* mgr, const char* name, const float* value) {
    GLint loc = uniform_location(mgr, name);
    if (loc >= 0)
        glUniform2fv(loc, 1, value);
}

void uniform_set_vec3(UniformManager* mgr, const char* name, const float* value) {
    GLint loc = uniform_location(mgr, name);
    if (loc >= 0)
        glUniform3fv(loc, 1, value);
}

void uniform_set_vec4(UniformManager* mgr, const char* name, const float* value) {
    GLint loc = uniform_location(mgr, name);
    if (loc >= 0)
        glUniform4fv(loc, 1, value);
}

void uniform_set_mat3(UniformManager* mgr, const char* name, const float* value) {
    GLint loc = uniform_location(mgr, name);
    if (loc >= 0)
        glUniformMatrix3fv(loc, 1, GL_FALSE, value);
}

void uniform_set_mat4(UniformManager* mgr, const char* name, const float* value) {
    GLint loc = uniform_location(mgr, name);
    if (loc >= 0)
        glUniformMatrix4fv(loc, 1, GL_FALSE, value);
}
