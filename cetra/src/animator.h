#ifndef _ANIMATOR_H_
#define _ANIMATOR_H_

/*
 * The Animator (spec 12.1): what plays on a skeleton, and how one thing
 * becomes another.
 *
 * It owns an AnimationState -- the pose a node skins with -- and writes it once
 * per rendered frame from two layers:
 *
 *   BASE      a 1D blend space with ONE clock: entries at positions along a
 *             line, `param` picks the two neighbours and their weights, and
 *             every entry runs at the same phase so a foot planted at 40% of
 *             the walk is planted at 40% of the run. A single clip is a
 *             one-entry space. Switching what plays crossfades: the outgoing
 *             source keeps running and fades out over `fade_seconds`, then is
 *             dropped, so the pose after the fade is exactly what the incoming
 *             source alone produces.
 *   OVERRIDE  one clip masked to part of the body (a wave over walking legs),
 *             with its own fade in and out; a one-shot releases itself at its
 *             end.
 *
 * Events a clip carries (animation_add_event) fire from the base's
 * highest-weighted entry and from the override clip, through one callback,
 * AFTER the frame's pose is applied -- so a handler may call animator_play.
 *
 * There is no state machine here. A game decides what plays and when; this
 * fades, blends and reports. A clip's own clock is in its ticks; fades and dt
 * are in seconds, unscaled by `speed`.
 */

#include "animation.h"

#define ANIMATOR_SPACE_MAX 4

typedef struct AnimatorEntry {
    const Animation* clip;
    float position;
    // The ground speed this entry implies at speed 1, in whatever units the
    // param axis uses -- it is a denominator, so the units cancel as long as
    // they are the caller's own. 0 = unknown, which is what an entry that was
    // never measured carries and what makes animator_stride_speed say so.
    // animation_stride_speed measures one, in MODEL units, and a rig on a
    // scaled node owes the scale.
    float stride;
} AnimatorEntry;

// A source: a blend space and its clock. The clock is kept in the TICKS of
// entries[ref], the lowest-indexed entry with weight, and the other entries
// are sampled at the same fraction of their own length. With one entry the
// advance is the single-clip arithmetic exactly, which is what lets a
// one-entry space stand in for the old clip player bit for bit.
typedef struct AnimatorSpace {
    const char* name; // borrowed: the space's given name, or the clip's own
    AnimatorEntry entries[ANIMATOR_SPACE_MAX]; // sorted by position
    int count;
    float weights[ANIMATOR_SPACE_MAX]; // from `param`, each update; sum 1
    int ref;
    float time;      // ticks of entries[ref]
    float prev_time; // the tick the last advance started from (events)
    bool wrapped;    // the last advance crossed the loop point
    bool ended;      // the last advance reached a non-looping end
    bool looping;
    bool finished; // non-looping and held at its end
} AnimatorSpace;

typedef enum AnimatorLayerPhase {
    ANIMATOR_LAYER_OFF,
    ANIMATOR_LAYER_IN,
    ANIMATOR_LAYER_ON,
    ANIMATOR_LAYER_OUT
} AnimatorLayerPhase;

typedef struct AnimatorLayer {
    const Animation* clip;
    float time; // ticks of clip
    float prev_time;
    bool wrapped;
    bool ended;
    bool looping;
    float mask[MAX_BONES]; // per bone 0..1, copied at play
    float weight;          // the fade envelope, 0..1
    float fade_in, fade_out;
    AnimatorLayerPhase phase;
    bool finished; // a one-shot ran out and faded out; latched until the next play
} AnimatorLayer;

struct Animator;
typedef void (*AnimatorEventFn)(struct Animator* animator, const char* name, void* user);

typedef struct Animator {
    // ENGINE-OWNED: read freely, never write.
    AnimationState* state;  // the pose this animator writes; owned. node_set_pose takes it.
    AnimatorSpace base;     // what plays: the incoming source during a fade
    AnimatorSpace outgoing; // valid while fading && !outgoing_frozen
    Pose frozen;            // valid while fading && outgoing_frozen (see animator_play)
    bool fading;
    bool outgoing_frozen;
    float fade_seconds;
    float fade_elapsed;
    float fade_weight;    // the incoming source's weight this frame; 1 when not fading
    AnimatorSpace resume; // the source a one-shot replaced, its clock held
    bool resume_pending;
    float resume_fade;
    AnimatorLayer layer;
    bool finished; // true only on the frame a non-looping base source ended
    // The pose applied this frame. The override layer blends into it in place,
    // so this is the base layer's output only until that has run.
    Pose base_pose;
    AnimatorEventFn on_event;
    void* event_user;

    // Per-update scratch. Not state: nothing reads these between frames, and
    // they are members rather than locals because a Pose is ~6 KB and
    // animator_update is called once per rig per frame.
    Pose scratch_a; // the outgoing source, then the override's sample
    Pose scratch_b; // a blend space's second entry
    float layer_weights[MAX_BONES];

    // SETTINGS: plain stores. Everything above is the animator's own; write
    // these directly, at any time. What PLAYS is not a setting -- it goes
    // through animator_play, animator_play_space, animator_play_once,
    // animator_stop, animator_play_layer and animator_stop_layer.
    bool playing;
    float speed; // multiplies every clock's advance; fades are not scaled
    float param; // the blend space's position; clamped to its entries
} Animator;

// Owns a fresh AnimationState on the skeleton; NULL (logged) when the state
// cannot be made. Plays nothing until something is played.
Animator* create_animator(Skeleton* skeleton);
void free_animator(Animator* animator);

// --- Base layer ---

// Replace what plays with one clip. fade_seconds <= 0 is a hard cut, which
// resets the spring bones; a fade never does. A clip bound to another skeleton
// is refused by name.
void animator_play(Animator* animator, const Animation* clip, float fade_seconds, bool looping);

// Replace what plays with a blend space of 1..ANIMATOR_SPACE_MAX entries,
// sorted here by position. `name` is borrowed and is what animator_source_name
// reports; every entry must be on this animator's skeleton.
void animator_play_space(Animator* animator, const char* name, const AnimatorEntry* entries,
                         int count, float fade_seconds, bool looping);

// Play one clip through once, then crossfade back to whatever was playing
// before it, with the same fade. A second one-shot during the first keeps the
// original return point.
void animator_play_once(Animator* animator, const Animation* clip, float fade_seconds);

// Nothing plays: the skeleton returns to bind, springs reset.
void animator_stop(Animator* animator);

// A per-frame EDGE, like input's pressed(): true only on the frame a
// non-looping base source reached its end.
bool animator_finished(const Animator* animator);

// The base source's name: a space's given one, a clip's own, "" when nothing
// plays.
const char* animator_source_name(const Animator* animator);

// The ground speed the playing pose implies at `speed` 1, blended the way the
// pose is, in the units the entries' strides are in. 0 when nothing plays or
// any entry carrying weight has no stride -- which is the zero value, so a
// space nobody measured reports honestly rather than plausibly.
//
// Divide the speed a game wants to travel at by this and write the result to
// `speed`: that is the whole of stride matching, and the reason the division
// cannot be done outside the animator is the denominator. The space keeps ONE
// clock, in the ticks of its reference entry, and the blended pose covers
// `sum(w * stride * seconds)` of ground per unit of phase while the phase
// advances at `sum(w / seconds)` -- so the product is what a caller needs, and
// it collapses to the weighted mean of the strides only when every clip is the
// same length. A game that reaches for the mean is wrong the moment its walk
// and run differ in duration, by an amount that reads as a tuning problem.
float animator_stride_speed(const Animator* animator);

// --- Override layer ---

// Play `clip` over the base on the bones `mask` selects (bone_count floats,
// 0..1 each), fading in over fade_in. A non-looping clip fades out over
// fade_out at its end by itself; a looping one plays until stopped.
void animator_play_layer(Animator* animator, const Animation* clip, const float* mask,
                         float fade_in, float fade_out, bool looping);
void animator_stop_layer(Animator* animator, float fade_out);

// Whether the override ran out and faded out; latched until the next play.
bool animator_layer_finished(const Animator* animator);

// mask[i] = 1 for `root_bone` and everything under it, 0 elsewhere. Returns
// the bones selected; 0 and a log for a name the skeleton does not have.
int animator_mask_subtree(const Skeleton* skeleton, const char* root_bone, float* mask);

// --- Events ---

void animator_set_event_callback(Animator* animator, AnimatorEventFn fn, void* user);

// --- The frame ---

// Exactly once per rendered frame: latch the previous pose, advance every
// clock by dt seconds, sample, blend, apply, then dispatch the events crossed.
// A dt of 0 holds the pose and reads zero deformation velocity, which is what
// a paused sim wants. The once-a-frame rule is the prev-pose latch's
// (animation_snapshot_prev_pose): twice in a frame kills the skinned motion
// vectors.
void animator_update(Animator* animator, float dt);

#endif // _ANIMATOR_H_
