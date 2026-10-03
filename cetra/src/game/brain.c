#include "brain.h"
#include "entity.h"
#include "../ext/log.h"

#include <math.h>
#include <stdlib.h>

Brain* create_brain(uint32_t seed) {
    Brain* b = calloc(1, sizeof(Brain));
    if (!b) {
        log_error("create_brain: out of memory");
        return NULL;
    }
    b->current = -1;
    // xorshift32 has one fixed point, zero, which it never leaves.
    b->rng = seed ? seed : 0x9e3779b9u;
    b->interval = 0.5f;
    b->hysteresis = 0.1f;
    b->tie_band = 0.05f;
    return b;
}

void free_brain(Brain* brain) {
    free(brain);
}

int brain_add_activity(Brain* brain, const BrainActivity* activity) {
    if (!brain || !activity || !activity->score) {
        log_error("brain_add_activity: a null argument, or an activity with no score");
        return -1;
    }
    if (brain->activity_count >= BRAIN_MAX_ACTIVITIES) {
        log_error("brain_add_activity: the brain is full at %d activities; '%s' refused",
                  BRAIN_MAX_ACTIVITIES, activity->name ? activity->name : "?");
        return -1;
    }
    brain->activities[brain->activity_count] = *activity;
    return brain->activity_count++;
}

float brain_random(Brain* brain) {
    uint32_t x = brain->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    brain->rng = x;
    return (float)(x >> 8) * (1.0f / 16777216.0f);
}

static void stop(Brain* b, bool interrupted, BrainStatus status) {
    if (b->current < 0)
        return;
    BrainActivity* a = &b->activities[b->current];
    if (a->end)
        a->end(b, a->user, interrupted);
    b->cooldowns[b->current] = status == BRAIN_FAILED ? a->fail_cooldown : a->cooldown;
    b->current = -1;
    b->current_seconds = 0.0f;
}

static bool begin(Brain* b, int i) {
    BrainActivity* a = &b->activities[i];
    b->current = i;
    b->current_seconds = 0.0f;
    b->held = b->scores[i];
    if (a->begin && !a->begin(b, a->user)) {
        b->current = -1;
        b->cooldowns[i] = a->fail_cooldown;
        return false;
    }
    return true;
}

static void score_all(Brain* b) {
    for (int i = 0; i < b->activity_count; i++) {
        BrainActivity* a = &b->activities[i];
        b->scores[i] = a->score(b, a->user);
    }
}

// Score everything, and keep or replace what runs.
static void choose(Brain* b) {
    score_all(b);
    int order[BRAIN_MAX_ACTIVITIES];
    int n = 0;
    for (int i = 0; i < b->activity_count; i++)
        if (b->scores[i] > 0.0f && (b->cooldowns[i] <= 0.0f || i == b->current))
            order[n++] = i;
    // Best first; an insertion sort keeps equal scores in registration order, so the draw
    // below is the only thing that varies with the seed.
    for (int i = 1; i < n; i++) {
        const int k = order[i];
        int j = i - 1;
        for (; j >= 0 && b->scores[order[j]] < b->scores[k]; j--)
            order[j + 1] = order[j];
        order[j + 1] = k;
    }
    if (n > 1) {
        int band = 1;
        while (band < n && b->scores[order[0]] - b->scores[order[band]] <= b->tie_band)
            band++;
        if (band > 1) {
            const int pick = (int)(brain_random(b) * (float)band) % band;
            const int k = order[pick];
            order[pick] = order[0];
            order[0] = k;
        }
    }

    const int was = b->current;
    if (was >= 0) {
        if (!b->interrupted && b->current_seconds < b->activities[was].min_seconds)
            return;
        const float keep = fmaxf(b->scores[was], b->held);
        if (n == 0 || order[0] == was || b->scores[order[0]] < keep + b->hysteresis)
            return;
        stop(b, true, BRAIN_DONE);
    }
    for (int i = 0; i < n; i++)
        if (order[i] != was && begin(b, order[i]))
            return;
}

void brain_update(Brain* brain, float dt) {
    if (!brain)
        return;
    for (int i = 0; i < brain->activity_count; i++)
        brain->cooldowns[i] = fmaxf(0.0f, brain->cooldowns[i] - dt);
    if (brain->current >= 0) {
        BrainActivity* a = &brain->activities[brain->current];
        brain->current_seconds += dt;
        const BrainStatus status = a->step ? a->step(brain, a->user, dt) : BRAIN_RUNNING;
        if (status != BRAIN_RUNNING)
            stop(brain, false, status);
    }
    brain->since_scored += dt;
    if (brain->current < 0 || brain->interrupted || brain->since_scored >= brain->interval) {
        choose(brain);
        brain->since_scored = 0.0f;
        brain->interrupted = false;
    }
}

void brain_interrupt(Brain* brain) {
    if (brain)
        brain->interrupted = true;
}

bool brain_start(Brain* brain, int activity) {
    if (!brain || activity < 0 || activity >= brain->activity_count)
        return false;
    stop(brain, true, BRAIN_DONE);
    brain->since_scored = 0.0f;
    score_all(brain);
    if (!begin(brain, activity))
        return false;
    for (int i = 0; i < brain->activity_count; i++)
        brain->held = fmaxf(brain->held, brain->scores[i]);
    return true;
}

const char* brain_current_name(const Brain* brain) {
    return brain && brain->current >= 0 ? brain->activities[brain->current].name : NULL;
}

static void brain_component_free(void* data) {
    free_brain(data);
}

Brain* entity_add_brain(struct Entity* entity, Brain* brain) {
    if (!entity || !brain)
        return NULL;
    if (entity_has_component(entity, COMPONENT_BRAIN)) {
        log_error("Entity '%s' already has a brain", entity->name);
        return NULL;
    }
    entity_add_component(entity, COMPONENT_BRAIN, brain);
    entity_set_component_free(entity, COMPONENT_BRAIN, brain_component_free);
    return brain;
}

Brain* entity_get_brain(struct Entity* entity) {
    return entity ? (Brain*)entity_get_component(entity, COMPONENT_BRAIN) : NULL;
}

static void brain_tick_cb(Entity* entity, void* user_data) {
    brain_update(entity_get_brain(entity), *(const float*)user_data);
}

void update_all_brains(struct EntityManager* em, float dt) {
    if (em)
        entity_manager_foreach_with(em, COMPONENT_BIT(COMPONENT_BRAIN), brain_tick_cb, &dt);
}
