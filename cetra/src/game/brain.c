#include "brain.h"
#include "entity.h"
#include "physics.h"
#include "../ext/log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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
    b->forget = 60.0f;
    b->sight_range = 8.0f;
    b->sight_half_angle = glm_rad(120.0f);
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

int brain_find_activity(const Brain* brain, const char* name) {
    if (!brain || !name)
        return -1;
    for (int i = 0; i < brain->activity_count; i++)
        if (brain->activities[i].name && !strcmp(brain->activities[i].name, name))
            return i;
    return -1;
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
    if (a->begin && !a->begin(b, a->user)) {
        b->current = -1;
        b->cooldowns[i] = a->fail_cooldown;
        return false;
    }
    return true;
}

// Score everything, and keep or replace what runs.
static void choose(Brain* b) {
    int order[BRAIN_MAX_ACTIVITIES];
    int n = 0;
    for (int i = 0; i < b->activity_count; i++) {
        BrainActivity* a = &b->activities[i];
        b->scores[i] = a->score(b, a->user);
        if (b->scores[i] > 0.0f && (b->cooldowns[i] <= 0.0f || i == b->current))
            order[n++] = i;
    }
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

    if (b->current >= 0) {
        const BrainActivity* now = &b->activities[b->current];
        if (!b->interrupted && b->current_seconds < now->min_seconds)
            return;
        const bool kept = n == 0 || order[0] == b->current ||
                          b->scores[order[0]] < b->scores[b->current] + b->hysteresis;
        if (kept && b->scores[b->current] > 0.0f)
            return;
        stop(b, true, BRAIN_DONE);
    }
    for (int i = 0; i < n; i++)
        if (begin(b, order[i]))
            return;
}

void brain_update(Brain* brain, float dt) {
    if (!brain)
        return;
    for (int i = 0; i < brain->activity_count; i++)
        brain->cooldowns[i] = fmaxf(0.0f, brain->cooldowns[i] - dt);
    for (int i = 0; i < BRAIN_MEMORY_SLOTS; i++) {
        BrainMemory* m = &brain->memory[i];
        if (!m->entity)
            continue;
        m->seconds += dt;
        if (m->seconds > brain->forget)
            m->entity = 0;
    }

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
    return begin(brain, activity);
}

const char* brain_current_name(const Brain* brain) {
    return brain && brain->current >= 0 ? brain->activities[brain->current].name : NULL;
}

void brain_notice(Brain* brain, uint32_t entity, const vec3 where) {
    if (!brain || !entity)
        return;
    // Its own slot, else an empty one, else the one longest unseen.
    int slot = -1, oldest = 0;
    for (int i = 0; i < BRAIN_MEMORY_SLOTS && slot < 0; i++)
        if (brain->memory[i].entity == entity)
            slot = i;
    for (int i = 0; i < BRAIN_MEMORY_SLOTS && slot < 0; i++)
        if (!brain->memory[i].entity)
            slot = i;
    if (slot < 0) {
        for (int i = 1; i < BRAIN_MEMORY_SLOTS; i++)
            if (brain->memory[i].seconds > brain->memory[oldest].seconds)
                oldest = i;
        slot = oldest;
    }
    BrainMemory* m = &brain->memory[slot];
    m->entity = entity;
    glm_vec3_copy((float*)where, m->where);
    m->seconds = 0.0f;
}

bool brain_recall(const Brain* brain, uint32_t entity, vec3 where, float* seconds) {
    if (!brain || !entity)
        return false;
    for (int i = 0; i < BRAIN_MEMORY_SLOTS; i++) {
        const BrainMemory* m = &brain->memory[i];
        if (m->entity != entity)
            continue;
        if (where)
            glm_vec3_copy((float*)m->where, where);
        if (seconds)
            *seconds = m->seconds;
        return true;
    }
    return false;
}

bool brain_in_view(const Brain* brain, const vec3 eye, const vec3 forward, const vec3 point) {
    if (!brain)
        return false;
    vec3 to;
    glm_vec3_sub((float*)point, (float*)eye, to);
    const float d = glm_vec3_norm(to);
    if (d > brain->sight_range)
        return false;
    if (d < 1e-4f)
        return true;
    vec3 f;
    glm_vec3_normalize_to((float*)forward, f);
    return glm_vec3_dot(to, f) / d >= cosf(brain->sight_half_angle);
}

bool brain_can_see(const Brain* brain, struct PhysicsWorld* world, const vec3 eye, const vec3 point,
                   const struct Entity* target) {
    if (!brain || !world)
        return false;
    vec3 dir;
    glm_vec3_sub((float*)point, (float*)eye, dir);
    const float d = glm_vec3_norm(dir);
    if (d < 1e-4f)
        return true;
    glm_vec3_scale(dir, 1.0f / d, dir);
    const RigidBody* self = brain->entity ? entity_get_rigid_body(brain->entity) : NULL;
    RaycastHit hit;
    if (!physics_world_raycast_ignore(world, (float*)eye, dir, d, self, &hit) || !hit.hit)
        return true;
    // A hit at the point itself is the thing being looked at, a surface or a body.
    return (target && hit.entity == target) || hit.distance >= d - 0.02f;
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
    brain->entity = entity;
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
