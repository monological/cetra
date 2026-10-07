#include <stdio.h>
#include <string.h>

#include "ext/log.h"
#include "shader_params.h"
#include "uniform.h"

bool shader_params_set(ShaderParams* params, const char* name, const vec4 value) {
    if (!params || !name || !*name) {
        log_error("shader_params_set: NULL params or an empty name");
        return false;
    }
    if (strlen(name) >= SHADER_PARAM_NAME) {
        log_error("shader param name '%s' is longer than %d characters", name,
                  SHADER_PARAM_NAME - 1);
        return false;
    }
    for (int i = 0; i < params->count; i++) {
        if (strcmp(params->list[i].name, name) == 0) {
            glm_vec4_copy((float*)value, params->list[i].value);
            return true;
        }
    }
    if (params->count >= SHADER_PARAM_MAX) {
        log_error("no room for shader param '%s': %d are already held", name, SHADER_PARAM_MAX);
        return false;
    }
    ShaderParam* p = &params->list[params->count++];
    snprintf(p->name, sizeof(p->name), "%s", name);
    glm_vec4_copy((float*)value, p->value);
    return true;
}

void shader_params_upload(const ShaderParams* params, UniformManager* uniforms) {
    if (!params || !uniforms)
        return;
    for (int i = 0; i < params->count; i++)
        uniform_set_vec4(uniforms, params->list[i].name, params->list[i].value);
}

void shader_clock_upload(UniformManager* uniforms, float time, uint64_t frame) {
    uniform_set_float(uniforms, "time", time);
    // uniform_set_int compares through a float, which is exact below 2^24.
    uniform_set_int(uniforms, "frame", (int)(frame & 0xFFFFFF));
}
