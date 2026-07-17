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

// 0-12 material/shadow, 14-16 IBL, 17 skybox
#define PROBE_TEXTURE_UNIT 18

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

ReflectionProbe* create_reflection_probe(void);
void free_reflection_probe(ReflectionProbe* probe);

#endif // _PROBE_H_
