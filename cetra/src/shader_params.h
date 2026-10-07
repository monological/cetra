#ifndef _SHADER_PARAMS_H_
#define _SHADER_PARAMS_H_

#include <stdbool.h>
#include <stdint.h>

#include <cglm/cglm.h>

struct UniformManager;

// An app shader's own uniforms (spec 13.29): each a name and a vec4, uploaded under that name
// wherever their owner's program is bound -- a material's, a post pass's. Eight because a
// shader with more than eight knobs wants a texture; the name is bounded so an entry costs no
// allocation. No GL here, so a scene file's description can carry them.
#define SHADER_PARAM_MAX  8
#define SHADER_PARAM_NAME 32
typedef struct ShaderParam {
    char name[SHADER_PARAM_NAME];
    vec4 value;
} ShaderParam;
typedef struct ShaderParams {
    ShaderParam list[SHADER_PARAM_MAX];
    int count;
} ShaderParams;

// Set a param by name, adding it the first time. False, logged, when SHADER_PARAM_MAX others are
// already held or the name is empty or does not fit.
bool shader_params_set(ShaderParams* params, const char* name, const vec4 value);
// Upload every param to `uniforms`, the bound program's. A name the program does not declare
// costs a location lookup and nothing else. Nothing resets a name an owner leaves out, so two
// owners sharing one program each set every param it reads, or the second inherits the first's.
void shader_params_upload(const ShaderParams* params, struct UniformManager* uniforms);

// The clock an app shader reads (spec 13.29), into the bound program's `uniforms`: `time` in
// seconds and `frame`, the frame's index wrapped at 2^24, past which a float no longer counts it.
void shader_clock_upload(struct UniformManager* uniforms, float time, uint64_t frame);

#endif // _SHADER_PARAMS_H_
