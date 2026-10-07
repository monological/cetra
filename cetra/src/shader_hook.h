#ifndef _SHADER_HOOK_H_
#define _SHADER_HOOK_H_

#include <stdbool.h>

struct Engine;
struct ShaderProgram;

/*
 * An app's own GLSL inside the lit surface (spec 13.29): a SURFACE function that decides what
 * the surface is -- its albedo, coverage, normal, roughness, metallic, AO and emission -- and the
 * engine lights it, writes its G-buffer and draws its shadow as it does any material's. A
 * material carries one by pointer (Material.shader_hook), and the variant resolver builds the
 * lit-surface variant with the hook's source spliced in, so every material sharing a hook
 * shares its programs and a material changes hooks by a plain write.
 *
 * The surface is
 *
 *     void cetraSurface(inout CetraSurface s)
 *
 * over include/surface_hook.glsl's struct, whose outputs arrive holding what the engine
 * gathered from the material, so a hook that leaves a field alone keeps the material's value.
 * It may declare its own uniforms, which a material's shader params fill (Material.
 * shader_params), and read `time` and `frame`. Sheen, clearcoat, transmission, subsurface, IOR
 * and anisotropy are not in the struct: they stay the material's own settings.
 *
 * The OFFSET, from spec 13.29's phase 5, moves the surface's vertices:
 *
 *     vec3 cetraOffset(CetraVertex v)
 *
 * returns an object-space displacement no longer than `offset_bound`, which is what the culling
 * bound, the shadow and the depth passes are told.
 */

typedef struct ShaderHookDesc {
    const char* name;    // what its programs and its errors are called
    const char* surface; // GLSL defining cetraSurface; NULL leaves the surface the material's
    const char* offset;  // GLSL defining cetraOffset; NULL moves nothing
    float offset_bound;  // metres, the longest offset cetraOffset returns
    // What it decides of a caster changes with `time` -- its offset, or the alpha it cuts a
    // shadow-casting cutout by -- so a kept shadow face draws it again each frame.
    bool animated;
} ShaderHookDesc;

typedef struct ShaderHook {
    // ENGINE-OWNED, read only: a hook is fixed once made, because every variant built from it
    // is cached under its id.
    unsigned id; // unique, never 0; the variant cache's key past the family and the mask
    char* name;
    char* surface;
    char* offset;
    float offset_bound;
    bool animated;
    // The shadow programs carrying it, the engine's: the depth program with its offset and its
    // alpha, and the absorb program with its offset when it has one. NULL casts as the plain one.
    struct ShaderProgram* shadow_depth;
    struct ShaderProgram* shadow_absorb;
} ShaderHook;

// Copies the desc's sources, test-compiles the hook into the full lit-surface variant, builds its
// shadow programs, and registers it with the engine, which frees it. NULL, the compiler's message
// logged, when it does not compile or the desc carries neither function.
ShaderHook* create_shader_hook(struct Engine* engine, const ShaderHookDesc* desc);

#endif // _SHADER_HOOK_H_
