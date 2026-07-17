#ifndef _PROBE_H_
#define _PROBE_H_

#include <GL/glew.h>
#include <cglm/cglm.h>
#include <stdbool.h>

#include "ibl.h"

// Reflection probe: the scene captured once into a local cubemap and
// GGX-prefiltered by roughness. Consumed as parallax-corrected specular in
// the PBR shader and as the fallback color where the SSR ray misses.

#define PROBE_CUBEMAP_SIZE         256
#define PROBE_PREFILTER_SIZE       128
#define PROBE_PREFILTER_MIP_LEVELS 6

typedef struct ReflectionProbe {
    vec3 position; // capture origin (world)

    // Parallax proxy AABB (world): reflection rays are intersected with this
    // box and the probe is sampled toward the intersection (Lagarde 2012)
    vec3 box_min;
    vec3 box_max;

    GLuint cubemap;     // raw capture, full mip chain (prefilter source)
    GLuint prefiltered; // PROBE_PREFILTER_MIP_LEVELS roughness mips

    float intensity;
    float box_fade; // feather to the global env at the box faces,
                    // as a fraction of the box half-extent

    bool enabled;  // runtime consumption toggle
    bool captured; // reflection_probe_capture has run
    bool debug_background;

} ReflectionProbe;

// Forward declarations (scene.h includes this header)
struct Engine;
struct Scene;

ReflectionProbe* create_reflection_probe(void);
void free_reflection_probe(ReflectionProbe* probe);

// One-shot scene capture into probe->cubemap + GGX prefilter into
// probe->prefiltered. Call at load, after lights/shadows/IBL and any
// scene-graph transforms are final. Near/far are the capture frustum
// planes (scene-scaled, chosen by the app). Requires precomputed IBL.
int reflection_probe_capture(ReflectionProbe* probe, struct Engine* engine, struct Scene* scene,
                             float near_clip, float far_clip);

// Bind the prefiltered probe + uniforms on PROBE_TEXTURE_UNIT for a PBR draw
void bind_reflection_probe(const ReflectionProbe* probe, ShaderProgram* program);

// Draw the raw capture as the background, in place of the skybox
void render_probe_debug_background(const ReflectionProbe* probe, IBLResources* ibl, mat4 view,
                                   mat4 projection);

#endif // _PROBE_H_
