
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
    mgr->max_lights = 0;
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

GLint uniform_array_location(UniformManager* mgr, const char* array, size_t index,
                             const char* field) {
    char name[128];
    snprintf(name, sizeof(name), "%s[%zu].%s", array, index, field);
    return uniform_location(mgr, name);
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
    uniform_location(mgr, "nearClip");
    uniform_location(mgr, "farClip");

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
    uniform_location(mgr, "reflectanceTex");

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
    uniform_location(mgr, "reflectanceTexExists");

    // Misc
    uniform_location(mgr, "numLights");
    uniform_location(mgr, "lineWidth");
}

void uniform_cache_lights(UniformManager* mgr, size_t max_lights) {
    if (!mgr)
        return;

    mgr->max_lights = max_lights;

    for (size_t i = 0; i < max_lights; i++) {
        uniform_array_location(mgr, "lights", i, "position");
        uniform_array_location(mgr, "lights", i, "direction");
        uniform_array_location(mgr, "lights", i, "color");
        uniform_array_location(mgr, "lights", i, "specular");
        uniform_array_location(mgr, "lights", i, "ambient");
        uniform_array_location(mgr, "lights", i, "intensity");
        uniform_array_location(mgr, "lights", i, "constant");
        uniform_array_location(mgr, "lights", i, "linear");
        uniform_array_location(mgr, "lights", i, "quadratic");
        uniform_array_location(mgr, "lights", i, "cutOff");
        uniform_array_location(mgr, "lights", i, "outerCutOff");
        uniform_array_location(mgr, "lights", i, "type");
        uniform_array_location(mgr, "lights", i, "size");
    }
}

void uniform_cache_shadows(UniformManager* mgr, size_t max_shadow_lights) {
    if (!mgr)
        return;

    uniform_location(mgr, "shadowMaps");
    uniform_location(mgr, "numShadowLights");
    uniform_location(mgr, "shadowBias");
    uniform_location(mgr, "shadowTexelSize");

    for (size_t i = 0; i < max_shadow_lights; i++) {
        char name[64];
        snprintf(name, sizeof(name), "lightSpaceMatrix[%zu]", i);
        uniform_location(mgr, name);
        snprintf(name, sizeof(name), "shadowLightIndex[%zu]", i);
        uniform_location(mgr, name);
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

void uniform_set_vec3(UniformManager* mgr, const char* name, const float* value) {
    GLint loc = uniform_location(mgr, name);
    if (loc >= 0)
        glUniform3fv(loc, 1, value);
}

void uniform_set_mat4(UniformManager* mgr, const char* name, const float* value) {
    GLint loc = uniform_location(mgr, name);
    if (loc >= 0)
        glUniformMatrix4fv(loc, 1, GL_FALSE, value);
}
