#ifndef _PROBE_H_
#define _PROBE_H_

#include <GL/glew.h>
#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdint.h>

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

    // The scene capture's frustum planes, scene-scaled and chosen by whoever
    // places the probe; unused by an environment-only probe.
    float near_clip, far_clip;

    // Prefilter the global environment instead of rendering the scene: on an
    // open dome stage the imported meshes are near-field heroes a single
    // parallax box cannot place (their baked image smears into ghosts), and SSR
    // already reflects them on-screen -- the probe's job there is the grounded
    // environment. A scene capture is for scenes that ARE their own environment
    // (interiors).
    bool environment_only;

    bool enabled; // runtime consumption toggle
    bool debug_background;

    // Streaming, in a set of two or more (spec 13.24). `kept` is the probe's atlas column as
    // captured, RGBA half floats, so one that leaves residency and comes back is uploaded
    // rather than captured; NULL until its first capture. `resident_slot` is the column that
    // holds it, or is about to; `loaded` says its texels are there now.
    uint16_t* kept;
    int resident_slot;
    bool upload_pending;
    bool loaded;
} ReflectionProbe;

// A probe is consumable once its prefiltered chain exists and it is switched on.
static inline bool reflection_probe_active(const ReflectionProbe* probe) {
    return probe && probe->prefiltered && probe->enabled;
}

ReflectionProbe* create_reflection_probe(void);
void free_reflection_probe(ReflectionProbe* probe);

// Capture + GGX prefilter into probe->prefiltered, at the probe's own planes.
// A scene capture renders into probe->cubemap, supersampled 2x, with the async
// texture loader drained first. Requires precomputed IBL.
int reflection_probe_capture(ReflectionProbe* probe, struct Engine* engine, struct Scene* scene);

// Drop the raw scene capture AND the prefiltered chain. For a probe whose radiance has been
// resampled into the atlas both are spent: the capture is ~50 MB at PROBE_CUBEMAP_SIZE and the
// chain ~12.6 MB, against the ~4 MB column that replaced them, and nothing reads either again.
// The single-probe path keeps both -- it samples the chain, and --probe-debug renders the
// capture.
void probe_release_capture_scratch(ReflectionProbe* probe);

// Bind the prefiltered probe + uniforms for a PBR draw. The fragment stage
// is at the driver's sampler limit, so the probe rebinds
// IBL_PREFILTER_TEXTURE_UNIT (call after bind_ibl_textures) and the shader
// switches the lookup with probeEnabled.
void bind_reflection_probe(const ReflectionProbe* probe, ShaderProgram* program);

// Flatten the probe (or its absence) into postfx's per-frame uniform block
// for the SSR fallback; postfx never learns about Scene.
void reflection_probe_publish_to_postfx(const ReflectionProbe* probe, struct PostFX* fx);

// Re-express the parallax origin and proxy box after a world-origin shift
// (spec 11.62). Both are world absolutes, and both are re-published to PostFX
// and re-bound to the surface program every frame -- so this has to happen HERE,
// at the owner. Correcting either copy downstream is overwritten before it is read.
//
// The CAPTURE is not invalidated: a rigid translation moves the probe by exactly
// the delta that moved everything it sees, so the radiance it recorded is still
// what a mirror at that point would show. Only the address changed.
void reflection_probe_shift_origin(ReflectionProbe* probe, const vec3 delta);

#endif // _PROBE_H_
