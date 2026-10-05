#include <stdlib.h>
#include <string.h>

#include "probe_set.h"
#include "lighting_atlas.h"
#include "light_cluster.h" // GpuProbeBlock, the block this fills half of
#include "engine.h"
#include "gi_volume.h"
#include "postfx.h"
#include "util.h"
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
    if (set) {
        set->ready = false;
        set->failed = false;
    }
}

// Sequential and synchronous: the set is published only once all of them
// succeed, so no probe is ever photographed into another's capture.
static bool capture_all(ReflectionProbeSet* set, struct Engine* engine, struct Scene* scene) {
    for (int i = 0; i < set->count; ++i) {
        set->captures_total++;
        if (reflection_probe_capture(set->probes[i], engine, scene) != 0) {
            log_error("Reflection probe %d failed to capture; the set stays unpublished", i);
            return false;
        }
    }

    // One probe consumes its own cubemap directly on the prefilter unit, so it
    // needs no atlas and pays none of its memory.
    if (set->count >= 2) {
        set->atlas = lighting_atlas_sync(scene, engine);
        if (!set->atlas)
            return false;

        for (int i = 0; i < set->count; ++i) {
            if (!lighting_atlas_project_probe(set->atlas, set->probes[i], i))
                return false;
            // The scratch cubes are what make a set affordable: past this point
            // the column holds everything a consumer reads, and a retained
            // 1024 chain per probe would cost more than the whole atlas.
            probe_release_capture_scratch(set->probes[i]);
        }
    }
    return true;
}

void probe_set_update(ReflectionProbeSet* set, struct Engine* engine, struct Scene* scene) {
    if (!set || set->ready || set->failed || set->count <= 0 || !engine || !scene)
        return;
    if (gi_world_pending(scene->gi))
        return;
    if (capture_all(set, engine, scene))
        set->ready = true;
    else
        set->failed = true;
}

void probe_set_fill_descriptors(const ReflectionProbeSet* set, GpuProbeBlock* out) {
    if (!set || !out)
        return;

    out->info[0] = set->count;

    int aw = 0, ah = 0;
    lighting_atlas_size(set->atlas, &aw, &ah);
    out->atlas_params[0] = aw > 0 ? 1.0f / (float)aw : 0.0f;
    out->atlas_params[1] = ah > 0 ? 1.0f / (float)ah : 0.0f;
    out->atlas_params[2] = (float)aw;
    out->atlas_params[3] = (float)ah;

    lighting_atlas_fill_column(set->atlas, out->atlas_column, out->rows);

    for (int i = 0; i < set->count; ++i) {
        const ReflectionProbe* probe = set->probes[i];
        GpuProbeDesc* desc = &out->descs[i];

        glm_vec3_copy((float*)probe->position, desc->pos_intensity);
        desc->pos_intensity[3] = probe->intensity;
        glm_vec3_copy((float*)probe->box_min, desc->box_min_fade);
        desc->box_min_fade[3] = probe->box_fade;
        glm_vec3_copy((float*)probe->box_max, desc->box_max_pad);
        desc->column[0] = lighting_atlas_probe_column_x(set->atlas, i);
    }
}

void probe_set_report_masks(ReflectionProbeSet* set, const void* masks, size_t bytes, int bits) {
    if (!set || !masks)
        return;
    set->mask_digest = fnv1a_bytes(masks, bytes);
    set->mask_bits = bits;
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
        lighting_atlas_bind(set->atlas, program);
        return;
    }

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
        // SSR reads the descriptors out of the same block the surface program
        // does, so all it needs published is the texture and the flag arming
        // the branch.
        fx->probe_multi = true;
        fx->probe_atlas = lighting_atlas_texture(set->atlas);
        fx->probe_enabled = false;
        fx->probe_cubemap = 0;
        return;
    }

    fx->probe_multi = false;
    fx->probe_atlas = 0;
    reflection_probe_publish_to_postfx(probe_set_primary(set), fx);
}

void probe_set_probe_print(const ReflectionProbeSet* set, int frame, bool final) {
    if (!set)
        return;

    // An installed set reads "pending" until the engine has captured it, which a print from
    // before the first frame's capture sees.
    const char* mode = "none";
    if (set->failed)
        mode = "failed";
    else if (set->count > 0 && !set->ready)
        mode = "pending";
    else if (probe_set_multi(set))
        mode = "multi";
    else if (set->count > 0)
        mode = "single";

    int aw = 0, ah = 0;
    lighting_atlas_size(set->atlas, &aw, &ah);

    printf("probe-set frame=%d count=%d mode=%s atlas=%dx%d captures=%d mask_bits=%d "
           "digest=%08x\n",
           frame, set->count, mode, aw, ah, set->captures_total, set->mask_bits, set->mask_digest);

    if (!final)
        return;

    for (int i = 0; i < set->count; ++i) {
        const ReflectionProbe* p = set->probes[i];
        int rx = 0, ry = 0, rows = 0;
        lighting_atlas_probe_rect(set->atlas, i, &rx, &ry, &rows);
        printf("probe-set probe idx=%d pos=%.3f,%.3f,%.3f box=%.3f,%.3f,%.3f..%.3f,%.3f,%.3f "
               "rect=%d,%d rows=%d\n",
               i, p->position[0], p->position[1], p->position[2], p->box_min[0], p->box_min[1],
               p->box_min[2], p->box_max[0], p->box_max[1], p->box_max[2], rx, ry, rows);
    }
    fflush(stdout);
}
