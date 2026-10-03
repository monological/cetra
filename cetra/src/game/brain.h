#ifndef _BRAIN_H_
#define _BRAIN_H_

#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * A small utility AI (spec 13.17), as the BRAIN component: the game registers ACTIVITIES,
 * each a score and three hooks, and the brain keeps one of them running -- the best-scoring
 * one, held for at least its minimum time, until something beats it by a margin or it ends.
 * What the activities MEAN is the game's; the brain knows only scores, clocks and seeds.
 *
 * Three things are decided here rather than in each game, because each is easy to get
 * subtly wrong and the failure looks like temperament rather than a bug:
 *
 *  - HYSTERESIS. A challenger must beat the running activity's score by `hysteresis`. Two
 *    activities whose scores cross slowly otherwise trade places on every re-score, and the
 *    entity dithers between them at the re-score interval.
 *  - TIES are drawn, not ordered. Scores within `tie_band` of the best are picked between by
 *    the brain's own seeded xorshift32, so equally good choices vary from visit to visit and
 *    a run with the same seed makes the same choices. Taking the first in registration order
 *    makes a creature that always does the same thing.
 *  - A FAILED begin falls through to the next best in the same tick and puts the failed one
 *    on its fail cooldown, so a choice that cannot start (no way there, say) costs neither a
 *    frame of standing still nor a retry every tick.
 *
 * The brain is ticked once per fixed step by the game loop, after the app's update. Its
 * activities must not read per-frame edges (an animator's finished flag, an input press):
 * a frame runs any number of steps, and an edge is seen by all or none of them.
 *
 * Teardown frees only the brain's own memory. It never calls an activity's `end`, because
 * components go after the app's shutdown, in component order, and an `end` reaching into
 * a sibling component or the app's state would find it gone.
 */

#define BRAIN_MAX_ACTIVITIES 16
#define BRAIN_MEMORY_SLOTS   4

typedef struct Brain Brain;
struct Entity;
struct EntityManager;
struct PhysicsWorld;

typedef enum {
    BRAIN_RUNNING = 0,
    BRAIN_DONE,   // ended as meant; the normal cooldown applies
    BRAIN_FAILED, // gave up; the fail cooldown applies
} BrainStatus;

typedef struct BrainActivity {
    const char* name;
    // How much it wants doing now, 0..1. Zero or below is never chosen.
    float (*score)(Brain* brain, void* user);
    // Start it. False when it cannot start; the next best is tried in the same tick.
    bool (*begin)(Brain* brain, void* user);
    // Each step while it runs. NULL runs until something better takes over.
    BrainStatus (*step)(Brain* brain, void* user, float dt);
    // It stopped: `interrupted` when something else took over before it ended itself.
    void (*end)(Brain* brain, void* user, bool interrupted);
    float cooldown;      // seconds after it ends before it may be chosen again
    float fail_cooldown; // the same after it fails, or fails to begin
    float min_seconds;   // it is not taken over by a better score before this
    void* user;
} BrainActivity;

// Something remembered: an entity, where it was, and how long ago.
typedef struct BrainMemory {
    uint32_t entity; // 0 is an empty slot
    vec3 where;
    float seconds; // since it was last noticed
} BrainMemory;

struct Brain {
    // ENGINE-OWNED
    BrainActivity activities[BRAIN_MAX_ACTIVITIES];
    int activity_count;
    float scores[BRAIN_MAX_ACTIVITIES];    // as last scored
    float cooldowns[BRAIN_MAX_ACTIVITIES]; // seconds left
    int current;                           // -1 for none
    float current_seconds;
    float since_scored;
    bool interrupted;
    uint32_t rng;
    BrainMemory memory[BRAIN_MEMORY_SLOTS];
    struct Entity* entity; // the owner, once added

    // BY FUNCTION: brain_add_activity, brain_start, brain_interrupt, brain_notice.

    // SETTINGS
    float interval;   // seconds between re-scorings while an activity runs
    float hysteresis; // a challenger must beat the running activity by this much
    float tie_band;   // scores this close to the best are drawn between
    float forget;     // seconds after which a memory is gone
    float sight_range;
    float sight_half_angle; // radians either side of the eye's forward
};

// A brain with no activities, seeded: the same seed makes the same choices.
Brain* create_brain(uint32_t seed);
void free_brain(Brain* brain);

// Register an activity, copied. Its index, or -1 when the brain is full.
int brain_add_activity(Brain* brain, const BrainActivity* activity);
int brain_find_activity(const Brain* brain, const char* name);

// One step: cooldowns and memories age, the running activity steps, and the brain re-scores
// when its interval is up, the activity ended, or something interrupted it.
void brain_update(Brain* brain, float dt);

// Re-score on the next step, ignoring the running activity's minimum time.
void brain_interrupt(Brain* brain);

// Stop whatever runs and start this activity now, whatever its score or cooldown. False when
// there is no such activity or its begin refuses.
bool brain_start(Brain* brain, int activity);

// The name of what runs, or NULL.
const char* brain_current_name(const Brain* brain);

// 0..1 from the brain's own sequence, for an activity's choices.
float brain_random(Brain* brain);

// Remember an entity, by id, as seen at `where` just now.
void brain_notice(Brain* brain, uint32_t entity, const vec3 where);
// Where it was and how long ago, if it is remembered at all.
bool brain_recall(const Brain* brain, uint32_t entity, vec3 where, float* seconds);

// Whether `point` is inside the brain's field of view from an eye looking along `forward`.
bool brain_in_view(const Brain* brain, const vec3 eye, const vec3 forward, const vec3 point);
// Whether nothing but `target` stands between the eye and `point`. The owner's own body is
// never in the way. A NULL target asks whether the point itself is in clear sight.
bool brain_can_see(const Brain* brain, struct PhysicsWorld* world, const vec3 eye, const vec3 point,
                   const struct Entity* target);

// The BRAIN component: the entity owns the brain from here.
Brain* entity_add_brain(struct Entity* entity, Brain* brain);
Brain* entity_get_brain(struct Entity* entity);

// Every brain, one step: the game loop's call, after the app's update.
void update_all_brains(struct EntityManager* em, float dt);

#endif // _BRAIN_H_
