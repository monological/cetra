#include "animator.h"
#include "rigging.h"
#include "springbone.h"
#include "ext/log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Events one update can carry to the callback. A clip authoring more crossings
// than this in one frame drops the rest, once, by name.
#define EVENTS_PER_UPDATE 32

// ============================================================================
// Lifetime
// ============================================================================

Animator* create_animator(Skeleton* skeleton) {
    if (!skeleton) {
        log_error("Cannot create Animator without skeleton");
        return NULL;
    }
    Animator* a = calloc(1, sizeof(Animator));
    if (!a) {
        log_error("Failed to allocate memory for Animator");
        return NULL;
    }
    a->state = create_animation_state(skeleton);
    if (!a->state) {
        free(a);
        return NULL;
    }
    a->speed = 1.0f;
    a->fade_weight = 1.0f;
    a->playing = true;
    a->root_bone = animation_root_bone(skeleton);
    return a;
}

void free_animator(Animator* a) {
    if (!a)
        return;
    free_animation_state(a->state);
    free(a);
}

// ============================================================================
// The blend space
// ============================================================================

static float clip_seconds(const Animation* clip) {
    return clip->duration / clip->ticks_per_second;
}

// Piecewise-linear between the two entries around `param`, clamped at the
// ends. An entry's own position gives it exactly 1 and its neighbour exactly
// 0, so a knob parked on an entry plays that clip alone.
// Into `out` rather than into the space, so a query may ask what the weights
// WOULD be for a param the last advance has not seen yet -- a game writes the
// knob in its update and the animator ticks in pre-render, so anything reading
// weights in between would otherwise read the previous frame's.
static void space_weights(const AnimatorSpace* s, float param, float* out) {
    for (int i = 0; i < ANIMATOR_SPACE_MAX; i++)
        out[i] = 0.0f;
    int last = s->count - 1;
    if (last <= 0 || param <= s->entries[0].position) {
        out[0] = 1.0f;
        return;
    }
    if (param >= s->entries[last].position) {
        out[last] = 1.0f;
        return;
    }
    int i = 0;
    while (i < last - 1 && param > s->entries[i + 1].position)
        i++;
    float p0 = s->entries[i].position;
    float span = s->entries[i + 1].position - p0;
    float t = span > 0.0f ? (param - p0) / span : 1.0f;
    out[i] = 1.0f - t;
    out[i + 1] = t;
}

// Where entry i is, in its own ticks: the clock for the reference, the same
// fraction of its own length for every other.
static float entry_time_at(const AnimatorSpace* s, int i, float ref_time) {
    if (i == s->ref)
        return ref_time;
    const Animation* ref = s->entries[s->ref].clip;
    if (ref->duration <= 0.0f)
        return 0.0f;
    return (ref_time / ref->duration) * s->entries[i].clip->duration;
}

// Advance the clock by dt seconds. With one entry this is the single-clip
// advance statement for statement: the rate factor is exactly 1.0f (one weight
// of 1.0f times a length over itself), and multiplying by it is exact.
static void space_advance(AnimatorSpace* s, float param, float dt, float speed) {
    s->prev_time = s->time;
    s->wrapped = false;
    s->ended = false;
    if (s->count == 0)
        return;

    space_weights(s, param, s->weights);
    int ref = 0;
    while (ref < s->count - 1 && s->weights[ref] <= 0.0f)
        ref++;
    if (ref != s->ref) {
        // The clock changes clips: keep the phase. The one rounding in the
        // space, and never on a path with a single entry.
        const Animation* old = s->entries[s->ref].clip;
        const Animation* now = s->entries[ref].clip;
        s->time = old->duration > 0.0f ? (s->time / old->duration) * now->duration : 0.0f;
        s->prev_time = s->time;
        s->ref = ref;
    }

    const Animation* anim = s->entries[ref].clip;
    float ref_seconds = clip_seconds(anim);
    float rate = 0.0f;
    for (int i = 0; i < s->count; i++) {
        if (s->weights[i] > 0.0f)
            rate += s->weights[i] * (ref_seconds / clip_seconds(s->entries[i].clip));
    }

    float ticks_delta = dt * anim->ticks_per_second * speed * rate;
    s->time += ticks_delta;

    if (s->looping) {
        if (anim->duration > 0.0f) {
            s->wrapped = s->time >= anim->duration || s->time < 0.0f;
            s->time = fmodf(s->time, anim->duration);
            if (s->time < 0.0f)
                s->time += anim->duration;
        }
    } else {
        if (s->time >= anim->duration) {
            s->time = anim->duration;
            s->ended = !s->finished;
            s->finished = true;
        } else if (s->time < 0.0f) {
            s->time = 0.0f;
            s->ended = !s->finished;
            s->finished = true;
        }
    }
}

// The space's pose into `out`, `scratch` for a second entry. At most two
// entries carry weight, and they sum to 1, so one blend at the upper weight
// is the whole mix.
static void space_sample(const AnimatorSpace* s, const Skeleton* skeleton, Pose* out,
                         Pose* scratch) {
    int lo = -1, hi = -1;
    for (int i = 0; i < s->count; i++) {
        if (s->weights[i] <= 0.0f)
            continue;
        if (lo < 0)
            lo = i;
        else if (hi < 0)
            hi = i;
    }
    if (lo < 0) {
        // Unreachable as called -- space_weights always leaves a weight on a
        // non-empty space, and both callers check count first. A guard, so a
        // future caller gets the bind pose rather than uninitialised bones.
        pose_bind(skeleton, out);
        return;
    }
    animation_sample_pose(s->entries[lo].clip, skeleton, entry_time_at(s, lo, s->time), out);
    if (hi >= 0) {
        animation_sample_pose(s->entries[hi].clip, skeleton, entry_time_at(s, hi, s->time),
                              scratch);
        pose_blend(out, scratch, s->weights[hi], out);
    }
}

// ============================================================================
// Switching sources
// ============================================================================

static bool clip_on_skeleton(const Animator* a, const Animation* clip) {
    if (!clip)
        return false;
    if (clip->skeleton && clip->skeleton != a->state->skeleton) {
        log_error("Animator on '%s' refuses clip '%s', which is bound to '%s'",
                  a->state->skeleton->name, clip->name, clip->skeleton->name);
        return false;
    }
    return true;
}

// What each entry states about its own displacement, once, as the source starts.
// Measuring here rather than per frame is what keeps the per-frame cost a
// subtraction: a clip's travel is a property of the clip and the rig, and both
// are fixed for as long as the source plays.
static void space_measure_root(AnimatorSpace* s, const Skeleton* skeleton, int root_bone) {
    s->travels = false;
    s->root_read = false;
    for (int i = 0; i < s->count; i++) {
        glm_vec3_zero(s->loop_travel[i]);
        s->loop_yaw[i] = 0.0f;
        if (root_bone < 0)
            continue;
        if (animation_root_travel(s->entries[i].clip, skeleton, root_bone, s->loop_travel[i],
                                  &s->loop_yaw[i]))
            s->travels = true;
    }
}

// What this source laid down since the last update: each entry's OWN root moved
// against its own reading, weighted the way the pose is.
//
// A wrap adds one loop back, per entry. The clock is shared, so every entry
// crosses its loop point together and `wrapped` is the whole signal; what each
// entry lays down over that loop is what it stated. Algebraically the correction
// is exact -- (end - prev) + (now - start) is (now - prev) + one loop.
//
// The yaw is summed across entries and wrapped ONCE by the caller, since a
// per-entry wrap would fold a real half-turn back into the small angle.
static void space_root_advance(AnimatorSpace* s, int root_bone, vec3 out_travel, float* out_yaw) {
    glm_vec3_zero(out_travel);
    *out_yaw = 0.0f;
    if (root_bone < 0 || !s->travels)
        return;

    const bool had = s->root_read;
    for (int i = 0; i < s->count; i++) {
        vec3 now = {0.0f, 0.0f, 0.0f};
        float yaw = 0.0f;
        if (!animation_root_at(s->entries[i].clip, root_bone, entry_time_at(s, i, s->time), now,
                               &yaw))
            continue;
        if (had && s->weights[i] > 0.0f) {
            vec3 moved;
            glm_vec3_sub(now, s->prev_root[i], moved);
            float turned = yaw - s->prev_root_yaw[i];
            if (s->wrapped) {
                glm_vec3_add(moved, s->loop_travel[i], moved);
                turned += s->loop_yaw[i];
            }
            glm_vec3_muladds(moved, s->weights[i], out_travel);
            *out_yaw += s->weights[i] * turned;
        }
        glm_vec3_copy(now, s->prev_root[i]);
        s->prev_root_yaw[i] = yaw;
    }
    s->root_read = true;
}

// `incoming` replaces the base. With a fade the current base keeps running as
// the outgoing source until the fade completes; a switch that lands mid-fade
// freezes what was on screen and fades from that instead, so any number of
// rapid switches cost two samples a frame and never pop.
static void switch_source(Animator* a, const AnimatorSpace* incoming, float fade_seconds) {
    bool cut = a->base.count == 0 || fade_seconds <= 0.0f;
    if (!cut) {
        if (a->fading && a->base_pose.skeleton) {
            a->frozen = a->base_pose;
            a->outgoing_frozen = true;
        } else {
            a->outgoing = a->base;
            a->outgoing_frozen = false;
        }
        a->fading = true;
        a->fade_seconds = fade_seconds;
        a->fade_elapsed = 0.0f;
        a->fade_weight = 0.0f;
    } else {
        a->fading = false;
        a->outgoing_frozen = false;
        a->fade_weight = 1.0f;
        // A cut teleports the targets the springs chase; snapping them is
        // what a fade, being continuous, must not do.
        if (a->state->springs)
            spring_bone_reset(a->state->springs);
    }
    a->base = *incoming;
    // What the new source states, and no reading of it yet: its first update
    // rebases rather than differencing, so a switch costs one update's travel
    // instead of reading a clip's whole length as a step.
    space_measure_root(&a->base, a->state->skeleton, a->root_bone);
    // And anything the last source laid down that nobody took is dropped here
    // rather than handed to whoever takes next. A game that drains every step
    // never has more than one step of it; a game that stops draining -- because
    // the source it was reading stopped travelling, which is what walking off a
    // ledge does -- would otherwise be handed the whole hoard on landing, in one
    // step, as a lurch.
    glm_vec3_zero(a->root_accum);
    a->root_yaw_accum = 0.0f;
}

static void one_clip_space(AnimatorSpace* s, const Animation* clip, bool looping) {
    memset(s, 0, sizeof(*s));
    s->name = clip->name;
    s->entries[0].clip = clip;
    s->entries[0].position = 0.0f;
    s->count = 1;
    s->looping = looping;
}

void animator_play(Animator* a, const Animation* clip, float fade_seconds, bool looping) {
    if (!a || !clip_on_skeleton(a, clip))
        return;
    AnimatorSpace s;
    one_clip_space(&s, clip, looping);
    a->resume_pending = false;
    switch_source(a, &s, fade_seconds);
}

void animator_play_space(Animator* a, const char* name, const AnimatorEntry* entries, int count,
                         float fade_seconds, bool looping) {
    if (!a || !entries)
        return;
    if (count < 1 || count > ANIMATOR_SPACE_MAX) {
        log_error("Animator: a blend space holds 1..%d entries, not %d", ANIMATOR_SPACE_MAX, count);
        return;
    }
    for (int i = 0; i < count; i++) {
        if (!clip_on_skeleton(a, entries[i].clip))
            return;
    }

    AnimatorSpace s;
    memset(&s, 0, sizeof(s));
    s.name = name ? name : entries[0].clip->name;
    s.looping = looping;
    // Insertion by position; two entries at one position keep their order.
    for (int i = 0; i < count; i++) {
        int at = s.count;
        while (at > 0 && s.entries[at - 1].position > entries[i].position) {
            s.entries[at] = s.entries[at - 1];
            at--;
        }
        s.entries[at] = entries[i];
        s.count++;
    }
    a->resume_pending = false;
    switch_source(a, &s, fade_seconds);
}

void animator_play_once(Animator* a, const Animation* clip, float fade_seconds) {
    if (!a || !clip_on_skeleton(a, clip))
        return;
    // The first one-shot remembers where to return; a second during it does
    // not overwrite that with the first.
    if (!a->resume_pending && a->base.count > 0) {
        a->resume = a->base;
        a->resume_pending = true;
        a->resume_fade = fade_seconds;
    }
    AnimatorSpace s;
    one_clip_space(&s, clip, false);
    switch_source(a, &s, fade_seconds);
}

void animator_stop(Animator* a) {
    if (!a)
        return;
    memset(&a->base, 0, sizeof(a->base));
    a->fading = false;
    a->outgoing_frozen = false;
    a->fade_weight = 1.0f;
    a->resume_pending = false;
    a->layer.phase = ANIMATOR_LAYER_OFF;
    a->layer.weight = 0.0f;
    if (a->state->springs)
        spring_bone_reset(a->state->springs);
    pose_bind(a->state->skeleton, &a->base_pose);
    animation_state_apply_pose(a->state, &a->base_pose, 0.0f);
}

bool animator_finished(const Animator* a) {
    return a ? a->finished : false;
}

const char* animator_source_name(const Animator* a) {
    if (!a || a->base.count == 0 || !a->base.name)
        return "";
    return a->base.name;
}

float animator_stride_speed(const Animator* a) {
    if (!a || a->base.count == 0)
        return 0.0f;

    // The weights this param would produce, not the ones the last advance left:
    // a game writes the knob and reads this back before the animator ticks.
    float w[ANIMATOR_SPACE_MAX];
    space_weights(&a->base, a->param, w);

    // Ground per unit of PHASE, and phase per second. Neither is the answer on
    // its own: the first is what the blended pose lays down over a whole loop,
    // the second is how fast the one shared clock walks that loop, and the clock
    // runs at the blended clip LENGTH. Their product is the ground speed, in
    // whatever units the entries' strides were given in.
    float ground = 0.0f, phase_rate = 0.0f;
    bool any = false;
    for (int i = 0; i < a->base.count; i++) {
        if (w[i] <= 0.0f)
            continue;
        const float seconds = clip_seconds(a->base.entries[i].clip);
        if (seconds <= 0.0f)
            return 0.0f;
        // A strideless entry still occupies the clock, so it counts towards the
        // phase rate and contributes no ground. That is what makes an idle at
        // one end of a speed axis pull the answer down instead of erasing it.
        ground += w[i] * a->base.entries[i].stride * seconds;
        phase_rate += w[i] / seconds;
        any = any || a->base.entries[i].stride > 0.0f;
    }
    return any ? ground * phase_rate : 0.0f;
}

bool animator_root_motion(const Animator* a) {
    return a && a->root_motion && a->base.count > 0 && a->base.travels;
}

bool animator_take_root_motion(Animator* a, vec3 out_travel, float* out_yaw) {
    if (out_travel)
        glm_vec3_zero(out_travel);
    if (out_yaw)
        *out_yaw = 0.0f;
    if (!animator_root_motion(a))
        return false;
    if (out_travel)
        glm_vec3_copy(a->root_accum, out_travel);
    if (out_yaw)
        *out_yaw = a->root_yaw_accum;
    glm_vec3_zero(a->root_accum);
    a->root_yaw_accum = 0.0f;
    return true;
}

// ============================================================================
// The override layer
// ============================================================================

void animator_play_layer(Animator* a, const Animation* clip, const float* mask, float fade_in,
                         float fade_out, bool looping) {
    if (!a || !mask || !clip_on_skeleton(a, clip))
        return;
    AnimatorLayer* L = &a->layer;
    // A layer re-played mid-fade keeps its weight, so a second wave over a
    // half-faded first one continues from where the arm is.
    float weight = L->phase == ANIMATOR_LAYER_OFF ? 0.0f : L->weight;
    memset(L, 0, sizeof(*L));
    L->clip = clip;
    L->looping = looping;
    L->fade_in = fade_in;
    L->fade_out = fade_out;
    L->weight = weight;
    L->phase = ANIMATOR_LAYER_IN;
    size_t n = a->state->skeleton->bone_count;
    for (size_t i = 0; i < n; i++)
        L->mask[i] = mask[i];
}

void animator_stop_layer(Animator* a, float fade_out) {
    if (!a || a->layer.phase == ANIMATOR_LAYER_OFF)
        return;
    a->layer.fade_out = fade_out;
    a->layer.phase = ANIMATOR_LAYER_OUT;
}

bool animator_layer_finished(const Animator* a) {
    return a ? a->layer.finished : false;
}

int animator_mask_subtree(const Skeleton* skeleton, const char* root_bone, float* mask) {
    if (!skeleton || !mask)
        return 0;
    size_t n = skeleton->bone_count;
    // The lookup reads the map and declares the skeleton non-const.
    int root = skeleton_resolve_bone((Skeleton*)skeleton, root_bone);
    if (root < 0) {
        // Cleared even on refusal: a caller handed a mask it may not re-initialise, and
        // leaving last call's weights in it masks a different subtree than it asked for.
        for (size_t i = 0; i < n; i++)
            mask[i] = 0.0f;
        log_error("Skeleton '%s' has no bone '%s' to mask", skeleton->name,
                  root_bone ? root_bone : "(null)");
        return 0;
    }
    // The mark is the shared one; this layer is the name, the log and the weights.
    // uint8_t 0/1 to float 0/1 is exact, so what a blend reads is unchanged.
    uint8_t marked[MAX_BONES];
    const size_t count = skeleton_mark_subtree(skeleton, root, marked);
    for (size_t i = 0; i < n; i++)
        mask[i] = marked[i] ? 1.0f : 0.0f;
    return (int)count;
}

static void layer_advance(AnimatorLayer* L, float dt, float speed) {
    L->wrapped = false;
    L->ended = false;
    if (L->phase == ANIMATOR_LAYER_OFF)
        return;
    const Animation* clip = L->clip;
    L->prev_time = L->time;

    float raw = L->time + dt * clip->ticks_per_second * speed;
    if (L->looping) {
        if (clip->duration > 0.0f) {
            L->wrapped = raw >= clip->duration || raw < 0.0f;
            raw = fmodf(raw, clip->duration);
            if (raw < 0.0f)
                raw += clip->duration;
        }
    } else if (raw >= clip->duration) {
        L->ended = L->time < clip->duration;
        raw = clip->duration;
    } else if (raw < 0.0f) {
        raw = 0.0f;
    }
    L->time = raw;

    switch (L->phase) {
        case ANIMATOR_LAYER_IN:
            L->weight = L->fade_in > 0.0f ? L->weight + dt / L->fade_in : 1.0f;
            if (L->weight >= 1.0f) {
                L->weight = 1.0f;
                L->phase = ANIMATOR_LAYER_ON;
            }
            break;
        case ANIMATOR_LAYER_OUT:
            L->weight = L->fade_out > 0.0f ? L->weight - dt / L->fade_out : 0.0f;
            if (L->weight <= 0.0f) {
                L->weight = 0.0f;
                L->phase = ANIMATOR_LAYER_OFF;
                L->finished = true;
            }
            break;
        default:
            break;
    }
    // A one-shot releases itself: it holds its last key (the interpolators
    // clamp) while the weight fades out.
    if (L->ended && (L->phase == ANIMATOR_LAYER_IN || L->phase == ANIMATOR_LAYER_ON))
        L->phase = ANIMATOR_LAYER_OUT;
}

// ============================================================================
// Events
// ============================================================================

void animator_set_event_callback(Animator* a, AnimatorEventFn fn, void* user) {
    if (!a)
        return;
    a->on_event = fn;
    a->event_user = user;
}

// The events of `clip` crossed by an advance from prev to now, in the order
// crossed: [prev, now), or across the loop point [prev, end) then [0, now).
// An advance that ends a one-shot includes its final tick, so an event at
// the very end fires once.
static void collect_events(const Animation* clip, float prev, float now, bool wrapped, bool ended,
                           const char** fired, int* count, bool* dropped) {
    for (int pass = 0; pass < 2; pass++) {
        for (size_t e = 0; e < clip->event_count; e++) {
            float t = clip->events[e].time_ticks;
            bool hit;
            if (wrapped)
                hit = pass == 0 ? t >= prev : (t < now && t < prev);
            else if (pass == 0)
                hit = t >= prev && (t < now || (ended && t <= now));
            else
                hit = false;
            if (!hit)
                continue;
            if (*count >= EVENTS_PER_UPDATE) {
                *dropped = true;
                return;
            }
            fired[(*count)++] = clip->events[e].name;
        }
        if (!wrapped)
            break;
    }
}

// ============================================================================
// The frame
// ============================================================================

// What the clips laid down this update, and the pose put back where it rests so
// nothing travels twice.
//
// Both sources contribute, each weighted the way its pose is: the incoming one by
// the fade, the outgoing one by what is left of it. A frozen outgoing source has
// stopped advancing, so it lays down nothing and is skipped.
//
// The override layer is deliberately not consulted. A masked clip is played over a
// body already going somewhere -- a wave is not a step -- and nothing measures a
// travel for it.
//
// PINNING is gated on a source stating a displacement, and that gate is what keeps
// every rig in the tree unchanged. An in-place clip is free to swing its hips and
// turn them; removing that would flatten a walk cycle's counter-rotation on every
// character in the corpus, for a correction that is only owed when the travel is
// going to the body instead.
static void root_advance(Animator* a, const Skeleton* skeleton) {
    const bool fading_live = a->fading && !a->outgoing_frozen && a->outgoing.travels;
    if (!a->root_motion || a->root_bone < 0 || !(a->base.travels || fading_live))
        return;

    vec3 moved = {0.0f, 0.0f, 0.0f};
    float turned = 0.0f;
    space_root_advance(&a->base, a->root_bone, moved, &turned);
    glm_vec3_scale(moved, a->fade_weight, moved);
    turned *= a->fade_weight;

    if (fading_live) {
        vec3 out_moved = {0.0f, 0.0f, 0.0f};
        float out_turned = 0.0f;
        space_root_advance(&a->outgoing, a->root_bone, out_moved, &out_turned);
        glm_vec3_muladds(out_moved, 1.0f - a->fade_weight, moved);
        turned += (1.0f - a->fade_weight) * out_turned;
    }

    // The shorter way round, AFTER the wrap corrections: a clip that turns the body
    // most of the way round in one update is a real turn, and wrapping before the
    // correction would read it as the small one back.
    while (turned > GLM_PIf)
        turned -= 2.0f * GLM_PIf;
    while (turned < -GLM_PIf)
        turned += 2.0f * GLM_PIf;

    glm_vec3_add(a->root_accum, moved, a->root_accum);
    a->root_yaw_accum += turned;

    animation_pose_pin_root(skeleton, &a->base_pose, a->root_bone);
}

void animator_update(Animator* a, float dt) {
    if (!a)
        return;

    // First and unconditional: a paused pose reads zero deformation velocity
    // rather than a frozen nonzero one.
    animation_snapshot_prev_pose(a->state);
    a->finished = false;

    if (!a->playing || a->base.count == 0)
        return;
    const Skeleton* skeleton = a->state->skeleton;

    // Clocks and envelopes
    space_advance(&a->base, a->param, dt, a->speed);
    if (a->fading) {
        if (!a->outgoing_frozen)
            space_advance(&a->outgoing, a->param, dt, a->speed);
        a->fade_elapsed += dt;
        // Completed this frame: the outgoing is dropped, not blended at 1, so
        // the pose from here on is the incoming source's alone.
        if (a->fade_elapsed + 1e-6f >= a->fade_seconds) {
            a->fading = false;
            a->outgoing_frozen = false;
            a->fade_weight = 1.0f;
        } else {
            a->fade_weight = a->fade_elapsed / a->fade_seconds;
        }
    }
    layer_advance(&a->layer, dt, a->speed);

    // The base, then the outgoing faded under it
    space_sample(&a->base, skeleton, &a->base_pose, &a->scratch_b);
    if (a->fading) {
        const Pose* out_pose = &a->frozen;
        if (!a->outgoing_frozen) {
            space_sample(&a->outgoing, skeleton, &a->scratch_a, &a->scratch_b);
            out_pose = &a->scratch_a;
        }
        pose_blend(out_pose, &a->base_pose, a->fade_weight, &a->base_pose);
    }

    // The override, on its bones. In place: pose_blend_masked may alias, and
    // copying 6 KB to a second buffer every frame bought nothing.
    if (a->layer.phase != ANIMATOR_LAYER_OFF) {
        for (size_t i = 0; i < skeleton->bone_count; i++)
            a->layer_weights[i] = a->layer.weight * a->layer.mask[i];
        animation_sample_pose(a->layer.clip, skeleton, a->layer.time, &a->scratch_a);
        pose_blend_masked(&a->base_pose, &a->scratch_a, a->layer_weights, &a->base_pose);
    }

    root_advance(a, skeleton);

    animation_state_apply_pose(a->state, &a->base_pose, dt);

    // Events, after the pose: the base's highest-weighted entry (ties to the
    // lowest index) and the override clip. The outgoing source fires nothing.
    //
    // Collected before any of them is dispatched, because a handler is allowed
    // to call animator_play -- which rewrites the very space being read. That
    // is what the buffer is for; being AFTER the pose is the dispatch's
    // position, not the buffer's doing.
    if (a->on_event) {
        const char* fired[EVENTS_PER_UPDATE];
        int count = 0;
        bool dropped = false;
        const AnimatorSpace* s = &a->base;
        int e = 0;
        for (int i = 1; i < s->count; i++) {
            if (s->weights[i] > s->weights[e])
                e = i;
        }
        collect_events(s->entries[e].clip, entry_time_at(s, e, s->prev_time),
                       entry_time_at(s, e, s->time), s->wrapped, s->ended, fired, &count, &dropped);
        if (a->layer.phase != ANIMATOR_LAYER_OFF) {
            collect_events(a->layer.clip, a->layer.prev_time, a->layer.time, a->layer.wrapped,
                           a->layer.ended, fired, &count, &dropped);
        }
        if (dropped) {
            log_warn("Animator on '%s': more than %d events crossed in one frame; the rest were "
                     "dropped",
                     skeleton->name, EVENTS_PER_UPDATE);
        }
        for (int i = 0; i < count; i++)
            a->on_event(a, fired[i], a->event_user);
    }

    // The edge, and a one-shot's way back
    if (a->base.ended) {
        a->finished = true;
        if (a->resume_pending) {
            AnimatorSpace back = a->resume;
            a->resume_pending = false;
            switch_source(a, &back, a->resume_fade);
        }
    }
}
