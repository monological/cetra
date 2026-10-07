#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "ext/log.h"
#include "program.h"
#include "shader_hook.h"
#include "util.h"

ShaderHook* create_shader_hook(Engine* engine, const ShaderHookDesc* desc) {
    // Unique for the process, never 0: every variant built from a hook is cached under this, so
    // a hook freed with one engine and a new one made for the next must not share a key.
    static unsigned next_id = 0;
    if (!engine || !desc || !desc->name || (!desc->surface && !desc->offset)) {
        log_error("create_shader_hook: NULL engine or desc, no name, or neither a surface nor "
                  "an offset");
        return NULL;
    }
    ShaderHook* hook = calloc(1, sizeof(ShaderHook));
    if (!hook) {
        log_error("create_shader_hook: no memory for '%s'", desc->name);
        return NULL;
    }
    hook->id = ++next_id;
    hook->name = safe_strdup(desc->name);
    hook->surface = desc->surface ? safe_strdup(desc->surface) : NULL;
    hook->offset = desc->offset ? safe_strdup(desc->offset) : NULL;
    hook->offset_bound = desc->offset_bound;
    hook->animated = desc->animated;

    // Compiled now, into the full variant, so a hook that does not compile is refused where the
    // app made it rather than found by the resolver on some later frame. The program is kept:
    // the full variant is the one a hooked material starts on.
    ShaderProgram* full = create_pbr_program_variant(PBR_FAMILY_RIGID, PBR_FEAT_ALL, hook);
    if (!full) {
        log_error("shader hook '%s' does not compile; refused", desc->name);
        free_shader_hook(hook);
        return NULL;
    }
    engine_add_program(engine, full);

    ShaderHook** grown =
        realloc(engine->shader_hooks, (engine->shader_hook_count + 1) * sizeof(ShaderHook*));
    if (!grown) {
        log_error("create_shader_hook: no memory to register '%s'", desc->name);
        free_shader_hook(hook);
        return NULL;
    }
    engine->shader_hooks = grown;
    engine->shader_hooks[engine->shader_hook_count++] = hook;
    return hook;
}

void free_shader_hook(ShaderHook* hook) {
    if (!hook)
        return;
    free(hook->name);
    free(hook->surface);
    free(hook->offset);
    free(hook);
}
