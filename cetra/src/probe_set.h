#ifndef _PROBE_SET_H_
#define _PROBE_SET_H_

#include <stdbool.h>
#include <stdint.h>

#include "probe.h"

// The scene's reflection probes (spec 11.70). One probe is the degenerate case
// of this, not a separate path: a scene with a single probe binds it into the
// IBL prefilter unit exactly as it always did, and the shader keeps its
// original expressions. Two or more arm the atlas + per-froxel mask instead.
//
// The cap is set by VRAM rather than by the mask: a probe's atlas column is
// ~4.3 MB at the default row-0 size, where the mask that selects it is one
// byte per froxel.
#define PROBE_SET_MAX 8

struct Engine;
struct Scene;
struct PostFX;
struct ProbeAtlas;

typedef struct ReflectionProbeSet {
    ReflectionProbe* probes[PROBE_SET_MAX];
    int count;

    // Set once every probe has captured. Consumers must gate on this and not
    // on count: a half-swept set holds probes whose atlas columns were never
    // written, and blending against those shows as black rooms.
    bool ready;

    struct ProbeAtlas* atlas; // owned; NULL until the first multi-probe sweep

    // Captures attempted across the set's life. The converge-then-idle claim
    // is only worth making if it is checkable from outside the process.
    int captures_total;

    uint32_t mask_digest; // FNV-1a over the froxel masks, for determinism arms
    int mask_bits;        // froxel/probe pairs the last build marked

    bool debug_atlas; // draw the raw atlas over the composited frame
} ReflectionProbeSet;

// The probe every single-probe consumer means. NULL on an empty set, so the
// callers that used to test scene->probe keep their shape.
static inline ReflectionProbe* probe_set_primary(const ReflectionProbeSet* set) {
    return (set && set->count > 0) ? set->probes[0] : NULL;
}

// True once two or more probes have captured -- the state that arms the atlas
// lookup, the froxel masks and the blend. Below it every consumer runs the
// path it ran before this spec.
static inline bool probe_set_multi(const ReflectionProbeSet* set) {
    return set && set->ready && set->count >= 2;
}

ReflectionProbeSet* create_reflection_probe_set(void);
void free_reflection_probe_set(ReflectionProbeSet* set);

// Takes ownership. Refuses past PROBE_SET_MAX (warns once) and refuses NULL.
bool probe_set_add(ReflectionProbeSet* set, ReflectionProbe* probe);

// Capture every probe, then project each into the atlas. Sequential and
// synchronous, at load: the set is published only once all of them succeed, so
// no probe is ever photographed into another's capture and a headless run sees
// a converged set from its first frame.
//
// row0 is the atlas row-0 tile size (0 = the default). near/far are per probe,
// derived by the caller from each probe's own proxy box.
bool probe_set_capture_all(ReflectionProbeSet* set, struct Engine* engine, struct Scene* scene,
                           const float* near_clips, const float* far_clips, const bool* env_only,
                           int row0);

// Re-arm the whole set for re-capture. The seam relight will need; nothing
// calls it yet, and a scene-captured probe is deliberately left stale by the
// sun slider exactly as the single probe always was.
void probe_set_mark_dirty(ReflectionProbeSet* set);

// World-origin shift (spec 11.62): each probe's parallax origin and proxy box
// are world absolutes and move with the world. The atlas holds radiance in
// texture space and needs nothing.
void probe_set_shift_origin(ReflectionProbeSet* set, const vec3 delta);

// Bind for a PBR draw. At one probe this is bind_reflection_probe unchanged;
// above it the atlas goes on the GI atlas unit and probeEnabled stays 0, so the
// prefilter unit keeps holding the global environment the blend falls back to.
void probe_set_bind(const ReflectionProbeSet* set, ShaderProgram* program);

// Flatten into postfx's per-frame block for the SSR miss fallback.
void probe_set_publish_to_postfx(const ReflectionProbeSet* set, struct PostFX* fx);

// One line per frame plus a per-probe block at the end (--probe-set-probe).
void probe_set_probe_print(const ReflectionProbeSet* set, int frame, bool final);

#endif // _PROBE_SET_H_
