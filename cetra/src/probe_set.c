#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "probe_set.h"
#include "lighting_atlas.h"
#include "light_cluster.h" // GpuProbeBlock, the block this fills half of
#include "engine.h"
#include "postfx.h"
#include "profiler.h"
#include "render.h"
#include "scene.h"
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
    residency_init(&set->residency, PROBE_SET_MAX, PROBE_STREAM_MARGIN);
    set->capturing = -1;
    return set;
}

void free_reflection_probe_set(ReflectionProbeSet* set) {
    if (!set)
        return;

    for (size_t i = 0; i < set->residency.count; ++i)
        free_reflection_probe(set->probes[i]);
    free(set->probes);
    residency_free(&set->residency);
    free(set);
}

bool probe_set_add(ReflectionProbeSet* set, ReflectionProbe* probe) {
    if (!set || !probe) {
        free_reflection_probe(probe);
        return false;
    }
    if (!grow_array((void**)&set->probes, &set->probe_capacity, set->residency.count + 1,
                    sizeof(ReflectionProbe*), 8) ||
        !residency_add(&set->residency)) {
        log_error("Failed to grow the reflection probe set");
        free_reflection_probe(probe);
        return false;
    }
    set->probes[set->residency.count - 1] = probe;
    return true;
}

void probe_set_mark_dirty(ReflectionProbeSet* set) {
    if (!set)
        return;
    set->ready = false;
    set->failed = false;
    residency_forget(&set->residency, true);
    if (set->capturing >= 0)
        set->probes[set->capturing]->faces_captured = 0;
    set->capturing = -1;
}

// A probe's proxy box.
static AABB probe_box(const ReflectionProbe* probe) {
    AABB box;
    glm_vec3_copy((float*)probe->box_min, box.min);
    glm_vec3_copy((float*)probe->box_max, box.max);
    return box;
}

bool probe_set_ready_in(const ReflectionProbeSet* set, const AABB* box) {
    if (!set || set->failed)
        return true;
    for (size_t i = 0; i < set->residency.count; ++i) {
        const AABB probe = probe_box(set->probes[i]);
        if (!aabb_overlaps(&probe, box))
            continue;
        // A world of one probe publishes through `ready`, holding no column.
        const bool loaded = set->residency.count == 1
                                ? set->ready
                                : set->residency.items[i].state == RESIDENCY_LOADED;
        if (!loaded)
            return false;
    }
    return true;
}

// The column an item's texels take: its slot's, or for one holding none, a rectangle of the
// size every column has.
static AtlasRect probe_column(const LightingAtlas* atlas, const ResidencyItem* item) {
    return lighting_atlas_probe_rect(atlas, item->slot >= 0 ? item->slot : 0);
}

// Whether a probe may be captured now: a probe is not captured again, so what it photographs
// it keeps (scene_capture_ready says what waits on what).
static bool capture_ready(const ReflectionProbe* probe, const struct Engine* engine,
                          const struct Scene* scene) {
    const AABB box = probe_box(probe);
    return scene_capture_ready(engine, scene, SCENE_CAPTURE_RADIANCE, &box);
}

// A world of one probe: captured once into its own cube, bound on the prefilter unit, no
// atlas -- the path that predates sets, a face a unit of the frame's capture budget like theirs.
static void update_single(ReflectionProbeSet* set, struct Engine* engine, struct Scene* scene,
                          CaptureBudget* budget) {
    ReflectionProbe* probe = set->probes[0];
    if (set->ready || (probe->faces_captured == 0 && !capture_ready(probe, engine, scene)) ||
        !capture_budget_allows(budget))
        return;
    if (probe->faces_captured == 0)
        set->captures_total++;
    profiler_scope_begin(engine->profiler, "probe capture");
    int whole = 0;
    do {
        whole = reflection_probe_capture_faces(probe, engine, scene, capture_budget_faces(budget));
        capture_budget_spend(budget);
    } while (whole == 0 && capture_budget_allows(budget));
    profiler_scope_end(engine->profiler);
    if (whole < 0) {
        log_error("Reflection probe 0 failed to capture");
        set->failed = true;
        return;
    }
    set->ready = whole == 1;
}

// What a loaded probe leaving residency keeps: its column, read out before it changes hands.
static uint16_t* probe_keep_column(void* user, size_t i, int slot) {
    const LightingAtlas* atlas = user;
    uint16_t* kept = lighting_atlas_keep(atlas, lighting_atlas_probe_rect(atlas, slot));
    if (!kept)
        log_warn("Reflection probe %zu: its column was not kept; it captures again when it "
                 "returns",
                 i);
    return kept;
}

// Which probes hold columns, from the camera. A loaded probe that leaves keeps its column, read
// out of `atlas` before the column changes hands.
static void probe_set_rank(ReflectionProbeSet* set, const struct Engine* engine,
                           LightingAtlas* atlas) {
    vec3 eye = {0.0f, 0.0f, 0.0f};
    if (engine->camera)
        glm_vec3_copy(engine->camera->position, eye);
    for (size_t i = 0; i < set->residency.count; ++i) {
        const AABB box = probe_box(set->probes[i]);
        set->residency.items[i].distance = sqrtf(aabb_dist_sq(&box, eye));
    }
    residency_assign(&set->residency, atlas ? probe_keep_column : NULL, atlas);
}

// The resident probe still waiting for its first capture and ready for it, nearest first, ties
// to the lower index, or -1.
static int next_capture(const ReflectionProbeSet* set, const struct Engine* engine,
                        const struct Scene* scene) {
    int best = -1;
    for (size_t i = 0; i < set->residency.count; ++i) {
        const ResidencyItem* item = &set->residency.items[i];
        if (item->slot < 0 || item->state != RESIDENCY_CAPTURE ||
            !capture_ready(set->probes[i], engine, scene))
            continue;
        if (best < 0 || item->distance < set->residency.items[best].distance)
            best = (int)i;
    }
    return best;
}

// A whole capture into its probe's column, and the cubes it was made from dropped.
static bool finish_column(ReflectionProbeSet* set, LightingAtlas* atlas, int i) {
    ReflectionProbe* probe = set->probes[i];
    ResidencyItem* item = &set->residency.items[i];
    if (!lighting_atlas_project_probe(atlas, probe, item->slot))
        return false;
    // The cubes are what make a set affordable to keep: past this point the column holds
    // everything a consumer reads.
    probe_release_capture_scratch(probe);
    residency_loaded(item);
    return true;
}

// The probe whose capture is under way, or -1. One that has lost its column since starts again
// from its first face when it next holds one.
static int capture_under_way(ReflectionProbeSet* set) {
    const int i = set->capturing;
    if (i < 0)
        return -1;
    const ResidencyItem* item = &set->residency.items[i];
    if (item->slot >= 0 && item->state == RESIDENCY_CAPTURE)
        return i;
    set->probes[i]->faces_captured = 0;
    set->capturing = -1;
    return -1;
}

static bool resident_waiting(const ReflectionProbeSet* set) {
    for (size_t i = 0; i < set->residency.count; ++i) {
        const ResidencyItem* item = &set->residency.items[i];
        if (item->slot >= 0 && item->state != RESIDENCY_LOADED)
            return true;
    }
    return false;
}

void probe_set_update(ReflectionProbeSet* set, struct Engine* engine, struct Scene* scene,
                      CaptureBudget* budget) {
    if (!set || set->failed || set->residency.count == 0 || !engine || !scene)
        return;

    if (set->residency.count == 1) {
        update_single(set, engine, scene, budget);
        return;
    }

    probe_set_rank(set, engine, scene->lighting_atlas);
    if (!resident_waiting(set)) {
        set->ready = true;
        return;
    }

    // Asked again next frame when refused: the atlas says why, once.
    LightingAtlas* atlas = scene_lighting_atlas(scene, engine);
    if (!atlas)
        return;
    for (size_t i = 0; i < set->residency.count; ++i) {
        ResidencyItem* item = &set->residency.items[i];
        if (item->state != RESIDENCY_UPLOAD)
            continue;
        lighting_atlas_restore(atlas, probe_column(atlas, item), item->kept);
        residency_loaded(item);
    }

    // A face at a time while the frame's capture budget allows (spec 13.32): one probe is six
    // large shaded faces and a prefilter, several hundred milliseconds, so a probe's capture may
    // span frames. The one begun is finished before another starts, nearest first.
    // Timed only on a frame that captures, for the GI scope's reason.
    bool timing = false;
    while (capture_budget_allows(budget)) {
        int i = capture_under_way(set);
        if (i < 0)
            i = next_capture(set, engine, scene);
        if (i < 0)
            break;
        if (!timing) {
            profiler_scope_begin(engine->profiler, "probe capture");
            timing = true;
        }
        if (set->capturing != i) {
            set->capturing = i;
            set->captures_total++;
        }
        const int whole = reflection_probe_capture_faces(set->probes[i], engine, scene,
                                                         capture_budget_faces(budget));
        capture_budget_spend(budget);
        if (whole < 0 || (whole == 1 && !finish_column(set, atlas, i))) {
            log_error("Reflection probe %d failed to capture; the set stops capturing", i);
            set->failed = true;
            break;
        }
        if (whole == 1)
            set->capturing = -1;
    }
    if (timing)
        profiler_scope_end(engine->profiler);
    if (!set->failed)
        set->ready = !resident_waiting(set);
}

void probe_set_atlas_needs(const ReflectionProbeSet* set, LightingAtlasLayout* layout) {
    const int count = set ? (int)set->residency.count : 0;
    if (count < 2)
        return;
    layout->probe_slots = count < PROBE_SET_MAX ? count : PROBE_SET_MAX;
    layout->probe_row0 = set->row0;
}

void probe_set_fill_descriptors(const ReflectionProbeSet* set, const LightingAtlas* atlas,
                                GpuProbeBlock* out) {
    if (!set || !out)
        return;

    int aw = 0, ah = 0;
    lighting_atlas_size(atlas, &aw, &ah);
    out->atlas_params[0] = aw > 0 ? 1.0f / (float)aw : 0.0f;
    out->atlas_params[1] = ah > 0 ? 1.0f / (float)ah : 0.0f;
    out->atlas_params[2] = (float)aw;
    out->atlas_params[3] = (float)ah;

    lighting_atlas_fill_column(atlas, out->atlas_column, out->rows);

    // In the order of the probes themselves rather than of their slots: the blend sums in
    // descriptor order, and a probe readmitted to a different column must sum where it did.
    // Only a loaded probe is published, and a loaded probe holds a column.
    int n = 0;
    for (size_t i = 0; i < set->residency.count; ++i) {
        const ResidencyItem* item = &set->residency.items[i];
        if (item->state != RESIDENCY_LOADED)
            continue;
        const ReflectionProbe* probe = set->probes[i];
        GpuProbeDesc* desc = &out->descs[n++];

        glm_vec3_copy((float*)probe->position, desc->pos_intensity);
        desc->pos_intensity[3] = probe->intensity;
        glm_vec3_copy((float*)probe->box_min, desc->box_min_fade);
        desc->box_min_fade[3] = probe->box_fade;
        glm_vec3_copy((float*)probe->box_max, desc->box_max_pad);
        const AtlasRect column = lighting_atlas_probe_rect(atlas, item->slot);
        desc->column[0] = (float)column.x;
        desc->column[1] = (float)column.y;
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
    for (size_t i = 0; i < set->residency.count; ++i)
        reflection_probe_shift_origin(set->probes[i], delta);
}

void probe_set_bind(const ReflectionProbeSet* set, const LightingAtlas* atlas,
                    ShaderProgram* program) {
    if (!program || !program->uniforms)
        return;

    if (probe_set_multi(set, atlas)) {
        // The blend reads the atlas and falls back to the global environment
        // for whatever weight is left over, so the prefilter unit must keep
        // holding that environment: probeEnabled stays 0 and the single-probe
        // branch is never taken.
        uniform_set_int(program->uniforms, "probeEnabled", 0);
        return;
    }

    const ReflectionProbe* primary = probe_set_primary(set);
    if (reflection_probe_active(primary))
        bind_reflection_probe(primary, program);
    else
        uniform_set_int(program->uniforms, "probeEnabled", 0);
}

void probe_set_publish_to_postfx(const ReflectionProbeSet* set, const LightingAtlas* atlas,
                                 PostFX* fx) {
    if (!fx)
        return;

    if (probe_set_multi(set, atlas)) {
        // SSR reads the descriptors out of the same block the surface program
        // does, so all it needs published is the texture and the flag arming
        // the branch.
        fx->probe_multi = true;
        fx->probe_atlas = lighting_atlas_texture(atlas);
        fx->probe_enabled = false;
        fx->probe_cubemap = 0;
        return;
    }

    fx->probe_multi = false;
    fx->probe_atlas = 0;
    reflection_probe_publish_to_postfx(probe_set_primary(set), fx);
}

void probe_set_probe_print(const ReflectionProbeSet* set, const LightingAtlas* atlas, int frame,
                           bool final) {
    if (!set)
        return;

    // An installed set reads "pending" until the engine has captured it, which a print from
    // before the first frame's capture sees.
    const int count = (int)set->residency.count;
    const char* mode = "none";
    if (set->failed)
        mode = "failed";
    else if (count > 0 && !set->ready)
        mode = "pending";
    else if (probe_set_multi(set, atlas))
        mode = "multi";
    else if (count > 0)
        mode = "single";

    int aw = 0, ah = 0;
    lighting_atlas_size(atlas, &aw, &ah);

    printf("probe-set frame=%d count=%d mode=%s atlas=%dx%d captures=%d mask_bits=%d "
           "digest=%08x\n",
           frame, count, mode, aw, ah, set->captures_total, set->mask_bits, set->mask_digest);

    if (!final)
        return;

    for (int i = 0; i < count; ++i) {
        const ReflectionProbe* p = set->probes[i];
        const AtlasRect rect = lighting_atlas_probe_rect(atlas, set->residency.items[i].slot);
        printf("probe-set probe idx=%d pos=%.3f,%.3f,%.3f box=%.3f,%.3f,%.3f..%.3f,%.3f,%.3f "
               "rect=%d,%d rows=%d\n",
               i, p->position[0], p->position[1], p->position[2], p->box_min[0], p->box_min[1],
               p->box_min[2], p->box_max[0], p->box_max[1], p->box_max[2], rect.x, rect.y,
               atlas ? PROBE_ATLAS_ROWS : 0);
    }
    fflush(stdout);
}

// What a probe is doing, in the words --stream-probe prints.
static const char* probe_state_name(ResidencyState state) {
    switch (state) {
        case RESIDENCY_LOADED:
            return "loaded";
        case RESIDENCY_UPLOAD:
            return "uploading";
        case RESIDENCY_CAPTURE:
            return "capturing";
        case RESIDENCY_OUT:
            break;
    }
    return "out";
}

void probe_set_stream_print(const ReflectionProbeSet* set, const LightingAtlas* atlas, int frame) {
    if (!set)
        return;
    const Residency* res = &set->residency;
    printf("stream probes frame=%d probes=%zu captures=%d slots=", frame, res->count,
           set->captures_total);
    for (int s = 0; s < res->slots; ++s)
        printf(s ? ",%d" : "%d", res->holder[s]);
    printf("\n");
    for (size_t i = 0; i < res->count; ++i) {
        const ResidencyItem* item = &res->items[i];
        // A digest of the column wherever it is, which is what a capture saw: two captures of
        // one room agree on it exactly, so it says whether something was or was not
        // photographed.
        const uint32_t digest =
            item->kept || item->state == RESIDENCY_LOADED
                ? lighting_atlas_digest(atlas, probe_column(atlas, item), item->kept)
                : 0u;
        printf("stream probe idx=%zu slot=%d dist=%.2f state=%s kept=%d digest=%08x\n", i,
               item->slot, (double)item->distance, probe_state_name(item->state),
               item->kept ? 1 : 0, digest);
    }
    fflush(stdout);
}
