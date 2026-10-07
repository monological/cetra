#include <math.h>
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
    // A negative bound would shrink the margin wind and morph widen the culling box by.
    hook->offset_bound = fmaxf(desc->offset_bound, 0.0f);
    hook->animated = desc->animated;

    // Compiled now, so a hook that does not compile is refused where the app made it rather than
    // found by the resolver on some later frame.
    if (!pbr_hook_compiles(hook)) {
        log_error("shader hook '%s' does not compile; refused", desc->name);
        free_shader_hook(hook);
        return NULL;
    }
    // Its shadow programs depend on the hook alone, so they are made with it. The offset goes into
    // both, or the shadow stays where the surface was, so a hook whose offset does not build into
    // them is refused. The surface goes into the depth program only for the alpha that cuts a
    // foliage caster; a surface that does not build there -- one reading what only the lit surface
    // declares, its normal map, say -- is left out, and the caster is cut by its material's alpha.
    ShaderProgram* depth = create_shadow_hook_program(hook, false, true);
    hook->shadow_cuts = depth && hook->surface;
    if (!depth && hook->surface) {
        log_warn("shader hook '%s': its surface does not build into the shadow pass, so its alpha "
                 "cuts no shadow",
                 desc->name);
        if (hook->offset)
            depth = create_shadow_hook_program(hook, false, false);
    }
    ShaderProgram* absorb = hook->offset ? create_shadow_hook_program(hook, true, false) : NULL;
    if (hook->offset && (!depth || !absorb)) {
        log_error("shader hook '%s': its offset does not build into the shadow passes; refused",
                  desc->name);
        free_program(depth);
        free_program(absorb);
        free_shader_hook(hook);
        return NULL;
    }
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
