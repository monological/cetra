#include <stdlib.h>
#include <string.h>

#include "probe_set.h"
#include "lighting_atlas.h"
#include "light_cluster.h" // GpuProbeBlock, the block this fills half of
#include "engine.h"
#include "gi_volume.h"
#include "postfx.h"
#include "stream.h"
#include "util.h"
#include "ext/log.h"

// How much nearer a non-resident probe must be than a resident one to take its column, in
// metres: a camera in a doorway between two rooms' probes would otherwise trade the last
// column back and forth on every step, and each trade is an upload.
#define PROBE_STREAM_MARGIN 1.0f

ReflectionProbeSet* create_reflection_probe_set(void) {
    ReflectionProbeSet* set = calloc(1, sizeof(ReflectionProbeSet));
    if (!set) {
        log_error("Failed to allocate reflection probe set");
        return NULL;
    }
    for (int s = 0; s < PROBE_SET_MAX; ++s)
        set->holder[s] = -1;
    return set;
}

void free_reflection_probe_set(ReflectionProbeSet* set) {
    if (!set)
        return;

    for (int i = 0; i < set->count; ++i)
        free_reflection_probe(set->probes[i]);
    free(set->probes);
    free(set->slot_of);
    free(set->home);
    free(set->distance);
    free(set);
}

bool probe_set_add(ReflectionProbeSet* set, ReflectionProbe* probe) {
    if (!set || !probe) {
        free_reflection_probe(probe);
        return false;
    }
    if (set->count == set->capacity) {
        int capacity = set->capacity ? set->capacity * 2 : 8;
        ReflectionProbe** probes =
            realloc(set->probes, sizeof(ReflectionProbe*) * (size_t)capacity);
        if (probes)
            set->probes = probes;
        int* slot_of = realloc(set->slot_of, sizeof(int) * (size_t)capacity);
        if (slot_of)
            set->slot_of = slot_of;
        float* distance = realloc(set->distance, sizeof(float) * (size_t)capacity);
        if (distance)
            set->distance = distance;
        int* home = realloc(set->home, sizeof(int) * (size_t)capacity);
        if (home)
            set->home = home;
        if (!probes || !slot_of || !distance || !home) {
            log_error("Failed to grow the reflection probe set");
            free_reflection_probe(probe);
            return false;
        }
        set->capacity = capacity;
    }
    set->probes[set->count] = probe;
    set->slot_of[set->count] = -1;
    set->home[set->count] = -1;
    set->distance[set->count] = 0.0f;
    set->count++;
    return true;
}

void probe_set_mark_dirty(ReflectionProbeSet* set) {
    if (!set)
        return;
    set->ready = false;
    set->failed = false;
    for (int i = 0; i < set->count; ++i) {
        ReflectionProbe* probe = set->probes[i];
        free(probe->kept);
        probe->kept = NULL;
        probe->loaded = false;
        probe->upload_pending = false;
    }
}

// A world of one probe: captured once into its own cube, bound on the prefilter unit, no
// atlas -- the path that predates sets, kept verbatim.
static void update_single(ReflectionProbeSet* set, struct Engine* engine, struct Scene* scene) {
    if (set->ready)
        return;
    set->captures_total++;
    if (reflection_probe_capture(set->probes[0], engine, scene) != 0) {
        log_error("Reflection probe 0 failed to capture");
        set->failed = true;
        return;
    }
    set->ready = true;
    set->opened = true;
}

void probe_set_rank(ReflectionProbeSet* set, const struct Engine* engine) {
    if (!set || set->failed || set->count < 2 || !engine)
        return;
    vec3 eye = {0.0f, 0.0f, 0.0f};
    if (engine->camera)
        glm_vec3_copy(engine->camera->position, eye);
    for (int i = 0; i < set->count; ++i) {
        const ReflectionProbe* p = set->probes[i];
        set->distance[i] = stream_box_distance(eye, p->box_min, p->box_max);
    }
    stream_assign(set->distance, set->count, PROBE_SET_MAX, PROBE_STREAM_MARGIN, set->slot_of,
                  set->holder, set->home);

    for (int i = 0; i < set->count; ++i) {
        ReflectionProbe* p = set->probes[i];
        const int s = set->slot_of[i];
        if (s == p->resident_slot)
            continue;
        p->resident_slot = s;
        p->loaded = false;
        p->upload_pending = s >= 0 && p->kept;
    }
}

// The resident probe still waiting for its first capture, nearest first, or -1.
static int next_capture(const ReflectionProbeSet* set) {
    int best = -1;
    for (int s = 0; s < PROBE_SET_MAX; ++s) {
        const int i = set->holder[s];
        if (i < 0 || set->probes[i]->loaded || set->probes[i]->upload_pending)
            continue;
        if (best < 0 || set->distance[i] < set->distance[best])
            best = i;
    }
    return best;
}

// Capture one probe into its column, keep the column, and drop the cubes it was made from.
static bool capture_into_column(ReflectionProbeSet* set, int i, struct Engine* engine,
                                struct Scene* scene) {
    ReflectionProbe* probe = set->probes[i];
    const int slot = set->slot_of[i];
    set->captures_total++;
    if (reflection_probe_capture(probe, engine, scene) != 0) {
        log_error("Reflection probe %d failed to capture; the set stops capturing", i);
        return false;
    }
    if (!lighting_atlas_project_probe(set->atlas, probe, slot))
        return false;
    // The cubes are what make a set affordable to keep: past this point the column holds
    // everything a consumer reads.
    probe_release_capture_scratch(probe);

    int w = 0, h = 0;
    float corner[2];
    lighting_atlas_probe_extent(set->atlas, &w, &h);
    lighting_atlas_probe_column(set->atlas, slot, corner);
    if (!probe->kept)
        probe->kept = malloc(sizeof(uint16_t) * 4 * (size_t)w * (size_t)h);
    if (probe->kept)
        lighting_atlas_read_rect(set->atlas, (int)corner[0], (int)corner[1], w, h, probe->kept);
    else
        log_warn("Reflection probe %d: no memory to keep its column; it captures again if it "
                 "leaves",
                 i);
    probe->loaded = true;
    return true;
}

static void restore_column(ReflectionProbeSet* set, int i) {
    ReflectionProbe* probe = set->probes[i];
    int w = 0, h = 0;
    float corner[2];
    lighting_atlas_probe_extent(set->atlas, &w, &h);
    lighting_atlas_probe_column(set->atlas, set->slot_of[i], corner);
    lighting_atlas_write_rect(set->atlas, (int)corner[0], (int)corner[1], w, h, probe->kept);
    probe->upload_pending = false;
    probe->loaded = true;
}

static bool resident_waiting(const ReflectionProbeSet* set) {
    for (int s = 0; s < PROBE_SET_MAX; ++s) {
        const int i = set->holder[s];
        if (i >= 0 && !set->probes[i]->loaded)
            return true;
    }
    return false;
}

bool probe_set_capture_due(const ReflectionProbeSet* set, const struct Scene* scene) {
    if (!set || set->failed || set->count <= 0 || !scene || gi_world_pending(scene->gi))
        return false;
    if (set->count == 1)
        return !set->ready;
    return next_capture(set) >= 0;
}

void probe_set_update(ReflectionProbeSet* set, struct Engine* engine, struct Scene* scene) {
    if (!set || set->failed || set->count <= 0 || !engine || !scene)
        return;

    // A capture lights what it sees with the volume only once the volume has an
    // answer, and with the environment's ambient before it, so a probe captured
    // alongside the volume photographs every closed room lit by the open sky --
    // by day many times the volume's light, which every dark glossy surface then
    // reflects as a grey wash.
    const bool gi_waiting = gi_world_pending(scene->gi);

    if (set->count == 1) {
        if (!gi_waiting)
            update_single(set, engine, scene);
        return;
    }

    if (!resident_waiting(set)) {
        set->ready = true;
        return;
    }

    set->atlas = lighting_atlas_sync(scene, engine);
    if (!set->atlas) {
        set->failed = true;
        return;
    }
    for (int s = 0; s < PROBE_SET_MAX; ++s) {
        const int i = set->holder[s];
        if (i >= 0 && set->probes[i]->upload_pending)
            restore_column(set, i);
    }

    // At load every resident probe captures in the one frame, so a set is whole from its
    // first frame; a probe that comes into range later captures alone, a frame's worth of
    // six scene renders and a prefilter.
    if (!gi_waiting) {
        const bool all = !set->opened;
        for (int i = next_capture(set); i >= 0; i = all ? next_capture(set) : -1) {
            if (!capture_into_column(set, i, engine, scene)) {
                set->failed = true;
                return;
            }
        }
        set->opened = true;
    }
    set->ready = !resident_waiting(set);
}

void probe_set_fill_descriptors(const ReflectionProbeSet* set, GpuProbeBlock* out) {
    if (!set || !out)
        return;

    int aw = 0, ah = 0;
    lighting_atlas_size(set->atlas, &aw, &ah);
    out->atlas_params[0] = aw > 0 ? 1.0f / (float)aw : 0.0f;
    out->atlas_params[1] = ah > 0 ? 1.0f / (float)ah : 0.0f;
    out->atlas_params[2] = (float)aw;
    out->atlas_params[3] = (float)ah;

    lighting_atlas_fill_column(set->atlas, out->atlas_column, out->rows);

    // In the order of the probes themselves rather than of their slots: the blend sums in
    // descriptor order, and a probe readmitted to a different column must sum where it did.
    int order[PROBE_SET_MAX];
    int n = 0;
    for (int s = 0; s < PROBE_SET_MAX; ++s) {
        const int i = set->holder[s];
        if (i < 0 || !set->probes[i]->loaded)
            continue;
        int at = n++;
        while (at > 0 && set->holder[order[at - 1]] > i) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = s;
    }
    for (int d = 0; d < n; ++d) {
        const int s = order[d];
        const ReflectionProbe* probe = set->probes[set->holder[s]];
        GpuProbeDesc* desc = &out->descs[d];

        glm_vec3_copy((float*)probe->position, desc->pos_intensity);
        desc->pos_intensity[3] = probe->intensity;
        glm_vec3_copy((float*)probe->box_min, desc->box_min_fade);
        desc->box_min_fade[3] = probe->box_fade;
        glm_vec3_copy((float*)probe->box_max, desc->box_max_pad);
        lighting_atlas_probe_column(set->atlas, s, desc->column);
    }
    out->info[0] = n;
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
        lighting_atlas_probe_rect(set->atlas, set->slot_of[i], &rx, &ry, &rows);
        printf("probe-set probe idx=%d pos=%.3f,%.3f,%.3f box=%.3f,%.3f,%.3f..%.3f,%.3f,%.3f "
               "rect=%d,%d rows=%d\n",
               i, p->position[0], p->position[1], p->position[2], p->box_min[0], p->box_min[1],
               p->box_min[2], p->box_max[0], p->box_max[1], p->box_max[2], rx, ry, rows);
    }
    fflush(stdout);
}

void probe_set_stream_print(const ReflectionProbeSet* set, int frame) {
    if (!set)
        return;
    printf("stream probes frame=%d probes=%d captures=%d slots=", frame, set->count,
           set->captures_total);
    for (int s = 0; s < PROBE_SET_MAX; ++s)
        printf(s ? ",%d" : "%d", set->holder[s]);
    printf("\n");
    for (int i = 0; i < set->count; ++i) {
        const ReflectionProbe* p = set->probes[i];
        const char* state = p->loaded              ? "loaded"
                            : p->upload_pending    ? "uploading"
                            : set->slot_of[i] >= 0 ? "capturing"
                                                   : "out";
        printf("stream probe idx=%d slot=%d dist=%.2f state=%s kept=%d\n", i, set->slot_of[i],
               (double)set->distance[i], state, p->kept ? 1 : 0);
    }
    fflush(stdout);
}
