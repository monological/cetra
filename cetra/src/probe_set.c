#include <stdlib.h>
#include <string.h>

#include "probe_set.h"
#include "probe_atlas.h"
#include "engine.h"
#include "postfx.h"
#include "ext/log.h"

ReflectionProbeSet* create_reflection_probe_set(void) {
    ReflectionProbeSet* set = calloc(1, sizeof(ReflectionProbeSet));
    if (!set)
        log_error("Failed to allocate reflection probe set");
    return set;
}

void free_reflection_probe_set(ReflectionProbeSet* set) {
    if (!set)
        return;

    for (int i = 0; i < set->count; ++i)
        free_reflection_probe(set->probes[i]);
    free_probe_atlas(set->atlas);
    free(set);
}

bool probe_set_add(ReflectionProbeSet* set, ReflectionProbe* probe) {
    if (!set || !probe)
        return false;

    if (set->count >= PROBE_SET_MAX) {
        log_warn("Reflection probe set is full at %d; ignoring the rest", PROBE_SET_MAX);
        return false;
    }

    set->probes[set->count++] = probe;
    return true;
}

void probe_set_mark_dirty(ReflectionProbeSet* set) {
    if (set)
        set->ready = false;
}

bool probe_set_capture_all(ReflectionProbeSet* set, struct Engine* engine, struct Scene* scene,
                           const float* near_clips, const float* far_clips, const bool* env_only,
                           int row0) {
    if (!set || set->count <= 0 || !engine || !scene)
        return false;

    for (int i = 0; i < set->count; ++i) {
        set->captures_total++;
        if (reflection_probe_capture(set->probes[i], engine, scene, near_clips[i], far_clips[i],
                                     env_only[i]) != 0) {
            log_error("Reflection probe %d failed to capture; the set stays unpublished", i);
            return false;
        }
    }

    // One probe consumes its own cubemap directly on the prefilter unit, so it
    // needs no atlas and pays none of its memory.
    if (set->count >= 2) {
        set->atlas = create_probe_atlas(engine, scene, set->count, row0);
        if (!set->atlas)
            return false;

        for (int i = 0; i < set->count; ++i) {
            if (!probe_atlas_project(set->atlas, set->probes[i], i))
                return false;
            // The scratch cubes are what make a set affordable: past this point
            // the column holds everything a consumer reads, and a retained
            // 1024 chain per probe would cost more than the whole atlas.
            probe_release_capture_scratch(set->probes[i]);
        }
    }

    set->ready = true;
    return true;
}

void probe_set_shift_origin(ReflectionProbeSet* set, const vec3 delta) {
    if (!set)
        return;
    for (int i = 0; i < set->count; ++i)
        reflection_probe_shift_origin(set->probes[i], delta);
}

void probe_set_bind(const ReflectionProbeSet* set, ShaderProgram* program) {
    if (!program || !program->uniforms)
        return;

    if (probe_set_multi(set)) {
        // The blend reads the atlas and falls back to the global environment
        // for whatever weight is left over, so the prefilter unit must keep
        // holding that environment: probeEnabled stays 0 and the single-probe
        // branch is never taken.
        uniform_set_int(program->uniforms, "probeEnabled", 0);
        probe_atlas_bind(set->atlas, program, set->count);
        return;
    }

    uniform_set_int(program->uniforms, "probeCount", set ? set->count : 0);

    const ReflectionProbe* primary = probe_set_primary(set);
    if (reflection_probe_active(primary))
        bind_reflection_probe(primary, program);
    else
        uniform_set_int(program->uniforms, "probeEnabled", 0);
}

void probe_set_publish_to_postfx(const ReflectionProbeSet* set, PostFX* fx) {
    if (!fx)
        return;

    if (probe_set_multi(set)) {
        probe_atlas_publish_to_postfx(set->atlas, set, fx);
        return;
    }

    fx->probe_multi = false;
    reflection_probe_publish_to_postfx(probe_set_primary(set), fx);
}

void probe_set_probe_print(const ReflectionProbeSet* set, int frame, bool final) {
    if (!set)
        return;

    const char* mode = "none";
    if (probe_set_multi(set))
        mode = "multi";
    else if (set->count > 0)
        mode = "single";

    int aw = 0, ah = 0;
    probe_atlas_size(set->atlas, &aw, &ah);

    printf("probe-set frame=%d count=%d mode=%s atlas=%dx%d captures=%d mask_bits=%d "
           "digest=%08x\n",
           frame, set->count, mode, aw, ah, set->captures_total, set->mask_bits,
           set->mask_digest);

    if (!final)
        return;

    for (int i = 0; i < set->count; ++i) {
        const ReflectionProbe* p = set->probes[i];
        int rx = 0, ry = 0, rows = 0;
        probe_atlas_rect(set->atlas, i, &rx, &ry, &rows);
        printf("probe-set probe idx=%d pos=%.3f,%.3f,%.3f box=%.3f,%.3f,%.3f..%.3f,%.3f,%.3f "
               "rect=%d,%d rows=%d\n",
               i, p->position[0], p->position[1], p->position[2], p->box_min[0], p->box_min[1],
               p->box_min[2], p->box_max[0], p->box_max[1], p->box_max[2], rx, ry, rows);
    }
    fflush(stdout);
}
