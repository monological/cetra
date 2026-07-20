#include "particle_pool.h"

#include <stdlib.h>
#include <string.h>

ParticlePool* create_particle_pool(size_t capacity) {
    ParticlePool* pool = calloc(1, sizeof(ParticlePool));
    if (!pool)
        return NULL;

    pool->capacity = capacity;
    pool->count = 0;
    pool->position = calloc(capacity, sizeof(vec3));
    pool->velocity = calloc(capacity, sizeof(vec3));
    pool->color = calloc(capacity, sizeof(vec4));
    pool->size = calloc(capacity, sizeof(float));
    pool->rotation = calloc(capacity, sizeof(float));
    pool->age = calloc(capacity, sizeof(float));
    pool->lifetime = calloc(capacity, sizeof(float));
    pool->seed = calloc(capacity, sizeof(float));

    if (!pool->position || !pool->velocity || !pool->color || !pool->size || !pool->rotation ||
        !pool->age || !pool->lifetime || !pool->seed) {
        free_particle_pool(pool);
        return NULL;
    }
    return pool;
}

void free_particle_pool(ParticlePool* pool) {
    if (!pool)
        return;
    free(pool->position);
    free(pool->velocity);
    free(pool->color);
    free(pool->size);
    free(pool->rotation);
    free(pool->age);
    free(pool->lifetime);
    free(pool->seed);
    free(pool);
}

size_t particle_pool_spawn(ParticlePool* pool, size_t n) {
    size_t start = pool->count;
    size_t avail = pool->capacity - pool->count;
    if (n > avail)
        n = avail;

    // Zero the freshly activated slice -- recycled slots hold stale data from
    // previously-killed particles.
    memset(&pool->position[start], 0, n * sizeof(vec3));
    memset(&pool->velocity[start], 0, n * sizeof(vec3));
    memset(&pool->color[start], 0, n * sizeof(vec4));
    memset(&pool->size[start], 0, n * sizeof(float));
    memset(&pool->rotation[start], 0, n * sizeof(float));
    memset(&pool->age[start], 0, n * sizeof(float));
    memset(&pool->lifetime[start], 0, n * sizeof(float));
    memset(&pool->seed[start], 0, n * sizeof(float));

    pool->count += n;
    return start;
}

void particle_pool_kill_swap(ParticlePool* pool, size_t i) {
    if (i >= pool->count)
        return;
    size_t last = pool->count - 1;
    if (i != last) {
        glm_vec3_copy(pool->position[last], pool->position[i]);
        glm_vec3_copy(pool->velocity[last], pool->velocity[i]);
        glm_vec4_copy(pool->color[last], pool->color[i]);
        pool->size[i] = pool->size[last];
        pool->rotation[i] = pool->rotation[last];
        pool->age[i] = pool->age[last];
        pool->lifetime[i] = pool->lifetime[last];
        pool->seed[i] = pool->seed[last];
    }
    pool->count--;
}
