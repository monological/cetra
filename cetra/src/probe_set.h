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

// Roughness rows per probe column. Here rather than in lighting_atlas.h because
// light_cluster.h sizes the GPU block's row table by it and cannot include that
// header; lighting_atlas.h asserts the two agree.
#define PROBE_ATLAS_ROWS_MAX 8

// The GPU mirror of a set, GpuProbeBlock, lives in light_cluster.h beside the
// other std140 mirrors: half of it is the froxel masks, which that file's grid
// sizes and its build pass fills.

struct Engine;
struct Scene;
struct PostFX;
struct LightingAtlas;

typedef struct ReflectionProbeSet {
    ReflectionProbe* probes[PROBE_SET_MAX];
    int count;

    // Set once every probe has captured. Consumers must gate on this and not
    // on count: a half-swept set holds probes whose atlas columns were never
    // written, and blending against those shows as black rooms.
    bool ready;
    bool failed; // one-shot: a capture that failed is not retried every frame

    int row0; // the atlas's row-0 tile size; 0 = the default

    // The scene's lighting atlas, borrowed; NULL until the first multi-probe sweep.
    struct LightingAtlas* atlas;

    // Captures attempted across the set's life. The converge-then-idle claim
    // is only worth making if it is checkable from outside the process.
    int captures_total;

    uint32_t mask_digest; // FNV-1a over the froxel masks, for determinism arms
    int mask_bits;        // froxel/probe pairs the last build marked

    bool debug_atlas; // draw the raw atlas over the composited frame
} ReflectionProbeSet;

// The probe every single-probe consumer means. NULL on an empty set and on one
// not yet captured, so the callers that used to test scene->probe keep their
// shape and an installed set is inert until it is ready.
static inline ReflectionProbe* probe_set_primary(const ReflectionProbeSet* set) {
    return (set && set->ready && set->count > 0) ? set->probes[0] : NULL;
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

/*
 * Capture an installed set that is not yet ready: every probe, then each
 * projected into the atlas, and the set marked ready only once all of them
 * succeed. A no-op on a ready or failed set, and while the scene's GI volume
 * has its opening sweep still to run.
 *
 * That wait is the point of capturing here rather than where the set is built.
 * A capture lights what it sees with the volume only once the volume has an
 * answer, and with the environment's ambient before it, so a set captured
 * alongside the volume photographs every closed room lit by the open sky --
 * by day many times the volume's light, which every dark glossy surface then
 * reflects as a grey wash. The columns are the scene's lighting atlas's.
 *
 * Runs in the frame before the shadow pass, after the GI sweep. A set is
 * installed uncaptured and is inert until this has run; a headless run sees it
 * from the frame the volume converges in, its first frame without one.
 */
void probe_set_update(ReflectionProbeSet* set, struct Engine* engine, struct Scene* scene);

// Re-arm the whole set for re-capture at the next update. The seam relight will
// need; nothing calls it yet, and a scene-captured probe is deliberately left
// stale by the sun slider exactly as the single probe always was.
void probe_set_mark_dirty(ReflectionProbeSet* set);

// Pack what a probe IS -- position, box, intensity, where its column sits --
// into the GPU block. The froxel masks in the same block are the light grid's
// to fill; this is the half that belongs to the set.
struct GpuProbeBlock;
void probe_set_fill_descriptors(const ReflectionProbeSet* set, struct GpuProbeBlock* out);

// Take back the digest and bit count of the masks the grid just built, so the
// diagnostic can print them. Here rather than written from light_cluster.c,
// which would have to launder a const Scene* to reach these fields.
void probe_set_report_masks(ReflectionProbeSet* set, const void* masks, size_t bytes, int bits);

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
