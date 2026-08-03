#ifndef _PROBE_H_
#define _PROBE_H_

#include <GL/glew.h>
#include <cglm/cglm.h>
#include <stdbool.h>

#include "ibl.h"

// Reflection probe: a local prefiltered cubemap consumed as
// parallax-corrected specular in the PBR shader and as the fallback color
// where the SSR ray misses. The content is either the global environment
// re-grounded (dome stages) or a one-shot capture of the scene (interiors).

// Sized for the sharpest consumer, not the average one: a near-mirror floor
// samples the prefilter at ~mip 0, where the face texels magnify across the
// whole reflection — too small a capture reads as blocky tiles there.
#define PROBE_CUBEMAP_SIZE         1024
#define PROBE_PREFILTER_SIZE       512
#define PROBE_PREFILTER_MIP_LEVELS 8

// Forward declarations (scene.h includes this header)
struct Engine;
struct Scene;
struct PostFX;

typedef struct ReflectionProbe {
    vec3 position; // parallax origin (world)

    // Parallax proxy AABB (world): reflection rays are intersected with this
    // box and the probe is sampled toward the intersection (Lagarde 2012)
    vec3 box_min;
    vec3 box_max;

    GLuint cubemap;     // raw scene capture, full mip chain (0 for
                        // environment-only probes)
    GLuint prefiltered; // roughness mip chain
    float max_lod;      // last prefiltered mip (the roughness->lod scale)

    float intensity;
    float box_fade; // feather the parallax correction off toward the box
                    // faces, as a fraction of the box half-extent

    bool enabled; // runtime consumption toggle
    bool debug_background;

} ReflectionProbe;

// A probe is consumable once its prefiltered chain exists; keep it off the
// scene until reflection_probe_capture succeeds and this needs no lifecycle
// flag beyond the GUI's enabled toggle.
static inline bool reflection_probe_active(const ReflectionProbe* probe) {
    return probe && probe->prefiltered && probe->enabled;
}

ReflectionProbe* create_reflection_probe(void);
void free_reflection_probe(ReflectionProbe* probe);

// One-shot capture + GGX prefilter into probe->prefiltered. Call at load,
// after lights/shadows/IBL and any scene-graph transforms are final; attach
// the probe to the scene only after this succeeds. Requires precomputed IBL.
//
// environment_only skips the scene render and prefilters the global
// environment cubemap instead: on an open dome stage the imported meshes
// are near-field heroes a single parallax box cannot place (their baked
// image smears into ghosts), and SSR already reflects them on-screen — the
// probe's job there is the grounded environment. Scene capture (into
// probe->cubemap, supersampled 2x, with the async texture loader drained
// first) is for scenes that ARE their own environment (interiors). Near/far
// are the scene-capture frustum planes (scene-scaled, chosen by the app;
// unused for environment_only).
int reflection_probe_capture(ReflectionProbe* probe, struct Engine* engine, struct Scene* scene,
                             float near_clip, float far_clip, bool environment_only);

// Bind the prefiltered probe + uniforms for a PBR draw. The fragment stage
// is at the driver's sampler limit, so the probe rebinds
// IBL_PREFILTER_TEXTURE_UNIT (call after bind_ibl_textures) and the shader
// switches the lookup with probeEnabled.
void bind_reflection_probe(const ReflectionProbe* probe, ShaderProgram* program);

// Flatten the probe (or its absence) into postfx's per-frame uniform block
// for the SSR fallback; postfx never learns about Scene.
void reflection_probe_publish_to_postfx(const ReflectionProbe* probe, struct PostFX* fx);

#endif // _PROBE_H_
