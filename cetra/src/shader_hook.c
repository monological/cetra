#include <stdlib.h>
#include <string.h>

#include "engine_internal.h"
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
    hook->surface = safe_strdup(desc->surface);
    hook->offset = safe_strdup(desc->offset);
    hook->offset_bound = desc->offset_bound;
    hook->animated = desc->animated;

    // Compiled now, so a hook that does not compile is refused where the app made it rather than
    // found by the resolver on some later frame.
    if (!pbr_hook_compiles(hook)) {
        log_error("shader hook '%s' does not compile; refused", desc->name);
        free_shader_hook(hook);
        return NULL;
    }
    // Its shadow programs depend on the hook alone, so they are made with it: the depth program
    // takes an offset or a surface's alpha, the absorb program only an offset.
    ShaderProgram* depth = create_shadow_hook_program(hook, false);
    ShaderProgram* absorb = hook->offset ? create_shadow_hook_program(hook, true) : NULL;
    if (!engine_add_shader_hook(engine, hook)) {
        free_program(depth);
        free_program(absorb);
        free_shader_hook(hook);
        return NULL;
    }
    hook->shadow_depth = depth;
    hook->shadow_absorb = absorb;
    if (depth)
        engine_add_program(engine, depth);
    if (absorb)
        engine_add_program(engine, absorb);
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
