#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "probe_set.h"
#include "lighting_atlas.h"
#include "light_cluster.h" // GpuProbeBlock, the block this fills half of
#include "capture_key.h"
#include "cook.h"
#include "engine.h"
#include "gi_volume.h"
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

// A capture under way abandoned: its faces, and the cube they were drawn into, go.
static void drop_capture(ReflectionProbe* probe) {
    if (probe->faces_captured == 0)
        return;
    probe->faces_captured = 0;
    probe_release_capture_scratch(probe);
}

void probe_set_mark_dirty(ReflectionProbeSet* set) {
    if (!set)
        return;
    set->ready = false;
    set->failed = false;
    residency_forget(&set->residency, true);
    for (size_t i = 0; i < set->residency.count; ++i)
        drop_capture(set->probes[i]);
}

// A probe's proxy box.
static AABB probe_box(const ReflectionProbe* probe) {
    AABB box;
    glm_vec3_copy((float*)probe->box_min, box.min);
    glm_vec3_copy((float*)probe->box_max, box.max);
    return box;
}

typedef struct ProbeBoxQuery {
    const ReflectionProbeSet* set;
    const AABB* box;
} ProbeBoxQuery;

static bool probe_bears_on(const void* user, size_t i) {
    const ProbeBoxQuery* q = user;
    const AABB box = probe_box(q->set->probes[i]);
    return aabb_overlaps(&box, q->box);
}

bool probe_set_ready_in(const ReflectionProbeSet* set, const AABB* box) {
    if (!set || set->failed)
        return true;
    const ProbeBoxQuery q = {set, box};
    // A world of one probe publishes through `ready`, holding no column.
    if (set->residency.count == 1)
        return set->ready || !probe_bears_on(&q, 0);
    return residency_loaded_where(&set->residency, probe_bears_on, &q);
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

// The probe to draw a face of next, or -1. A capture under way comes first, since one is finished
// before another starts, and is waited for while its box may not be captured -- every face it
// still owes must see what its first did. Otherwise the nearest resident probe ready to begin,
// ties to the lower index. A world of one probe holds no column: its probe until it is ready.
// A capture that has lost its column is dropped.
static int next_face(ReflectionProbeSet* set, const struct Engine* engine,
                     const struct Scene* scene) {
    if (set->residency.count == 1)
        return !set->ready && capture_ready(set->probes[0], engine, scene) ? 0 : -1;
    int best = -1;
    for (size_t i = 0; i < set->residency.count; ++i) {
        const ResidencyItem* item = &set->residency.items[i];
        ReflectionProbe* probe = set->probes[i];
        if (item->slot < 0 || item->state != RESIDENCY_CAPTURE) {
            drop_capture(probe);
            continue;
        }
        const bool ready = capture_ready(probe, engine, scene);
        if (probe->faces_captured > 0)
            return ready ? (int)i : -1;
        if (ready && (best < 0 || item->distance < set->residency.items[best].distance))
            best = (int)i;
    }
    return best;
}

// The cook's key for probe `i`'s capture (spec 13.42): the scene it sees out to its far plane,
// the probe and the column it fills, and the GI it is lit by, each volume by the key it was swept
// under.
static CookKey probe_key(const ReflectionProbeSet* set, size_t i, struct Engine* engine,
                         struct Scene* scene) {
    const ReflectionProbe* probe = set->probes[i];
    CookKey key = cook_key("probe-column/1");
    char label[COOK_NAME_MAX];
    snprintf(label, sizeof(label), "probe-%zu", i);
    cook_key_label(&key, label);
    AABB reach = {{probe->position[0], probe->position[1], probe->position[2]},
                  {probe->position[0], probe->position[1], probe->position[2]}};
    aabb_expand(&reach, probe->far_clip);
    scene_capture_fold(engine, scene, &reach, &key);
    cook_key_f32s(&key, probe->position, 3);
    cook_key_f32s(&key, probe->box_min, 3);
    cook_key_f32s(&key, probe->box_max, 3);
    const float terms[] = {probe->intensity, probe->box_fade, probe->near_clip, probe->far_clip};
    cook_key_f32s(&key, terms, (int)(sizeof(terms) / sizeof(terms[0])));
    cook_key_bool(&key, probe->environment_only);
    const int column[] = {set->row0, PROBE_ATLAS_ROWS, PROBE_CUBEMAP_SIZE};
    for (size_t c = 0; c < sizeof(column) / sizeof(column[0]); c++)
        cook_key_i32(&key, column[c]);
    gi_world_fold(scene->gi, &reach, SIZE_MAX, &key);
    return key;
}

// Probe `i`'s capture from the cook, keyed as it is about to begin, inside the burst (spec
// 13.42): its column into its slot and the probe loaded, true; or false, the key kept for the
// store, and the capture taken live.
static bool probe_fetch(ReflectionProbeSet* set, size_t i, struct Engine* engine,
                        struct Scene* scene, const LightingAtlas* atlas) {
    ResidencyItem* item = &set->residency.items[i];
    const CookKey key = probe_key(set, i, engine, scene);
    item->cook_hash = key.valid ? key.hash : 0;
    if (!key.valid) {
        if (cook_enabled())
            log_info("probe-cook probe=%zu result=unkeyable: something it sees cannot say what it "
                     "is",
                     i);
        return false;
    }
    if (!lighting_atlas_cook_fetch(atlas, probe_column(atlas, item), &key))
        return false;
    residency_loaded(item);
    return true;
}

// A published probe's column into the cook, under the key its capture began with -- and only if
// the scene still folds to it.
static void probe_store(const ReflectionProbeSet* set, size_t i, struct Engine* engine,
                        struct Scene* scene, const LightingAtlas* atlas) {
    const ResidencyItem* item = &set->residency.items[i];
    if (!item->cook_hash)
        return;
    const CookKey key = probe_key(set, i, engine, scene);
    if (!key.valid || key.hash != item->cook_hash) {
        log_info("probe-cook probe=%zu result=unstable: the scene changed while it was captured",
                 i);
        return;
    }
    lighting_atlas_cook_store(atlas, probe_column(atlas, item), &key);
}

// A whole capture published: into its column, the cubes it was made from dropped -- or, for a
// world of one probe, bound as it is.
static bool publish(ReflectionProbeSet* set, LightingAtlas* atlas, int i) {
    if (set->residency.count == 1) {
        set->ready = true;
        return true;
    }
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

    const bool multi = set->residency.count > 1;
    LightingAtlas* atlas = NULL;
    if (multi) {
        probe_set_rank(set, engine, scene->lighting_atlas);
        if (!resident_waiting(set)) {
            set->ready = true;
            return;
        }
        // Asked again next frame when refused: the atlas says why, once.
        atlas = scene_lighting_atlas(scene, engine);
        if (!atlas)
            return;
        for (size_t i = 0; i < set->residency.count; ++i) {
            ResidencyItem* item = &set->residency.items[i];
            if (item->state != RESIDENCY_UPLOAD)
                continue;
            lighting_atlas_restore(atlas, probe_column(atlas, item), item->kept);
            residency_loaded(item);
        }
    }

    // A face a unit of the frame's capture budget (spec 13.32): a probe is six large shaded faces
    // and a prefilter, several hundred milliseconds, so its capture may span frames. One burst for
    // the frame's faces, opened at the first: its shadow pass serves every face drawn in it, and
    // between two frames the frame's own shadow pass overwrites the maps it baked. Timed only on
    // a frame that captures, for the GI scope's reason.
    SceneCaptureState saved_capture;
    bool burst = false;
    for (int i; (i = next_face(set, engine, scene)) >= 0 && capture_budget_take(budget, !burst);) {
        if (!burst) {
            profiler_scope_begin(engine->profiler, "probe capture");
            // RADIANCE: this is what a mirror sees, so an emissive surface that is also a derived
            // panel must still appear in it (render.h).
            scene_capture_begin(engine, scene, SCENE_CAPTURE_RADIANCE, &saved_capture);
            burst = true;
        }
        ReflectionProbe* probe = set->probes[i];
        // Keyed inside the burst, where the scene is at rest: a column the cook holds is loaded
        // instead of captured, and one it does not is stored once published.
        const bool cook = multi && scene->cook_lighting;
        if (probe->faces_captured == 0) {
            if (cook && probe_fetch(set, (size_t)i, engine, scene, atlas))
                continue;
            set->captures_total++;
        }
        const int whole = reflection_probe_capture_face(probe, engine, scene);
        if (whole < 0 || (whole == 1 && !publish(set, atlas, i))) {
            log_error("Reflection probe %d failed to capture; the set stops capturing", i);
            set->failed = true;
            break;
        }
        if (whole == 1 && cook)
            probe_store(set, (size_t)i, engine, scene, atlas);
    }
    if (burst) {
        scene_capture_end(engine, scene, &saved_capture);
        profiler_scope_end(engine->profiler);
    }
    if (multi && !set->failed)
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
        printf("stream probe idx=%zu slot=%d dist=%.2f state=%s faces=%d kept=%d digest=%08x\n", i,
               item->slot, (double)item->distance, probe_state_name(item->state),
               set->probes[i]->faces_captured, item->kept ? 1 : 0, digest);
    }
    fflush(stdout);
}
