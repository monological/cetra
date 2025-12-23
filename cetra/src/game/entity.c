#include "entity.h"
#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENTITY_INITIAL_CAPACITY 64

EntityManager* create_entity_manager(struct Game* game) {
    EntityManager* em = calloc(1, sizeof(EntityManager));
    if (!em) {
        return NULL;
    }

    em->entities = calloc(ENTITY_INITIAL_CAPACITY, sizeof(Entity*));
    if (!em->entities) {
        free(em);
        return NULL;
    }

    em->capacity = ENTITY_INITIAL_CAPACITY;
    em->count = 0;
    em->next_id = 1;
    em->game = game;

    return em;
}

void free_entity_manager(EntityManager* em) {
    if (!em) {
        return;
    }

    // Destroy all entities
    for (size_t i = 0; i < em->count; i++) {
        if (em->entities[i]) {
            // Free components
            for (int c = 0; c < COMPONENT_MAX; c++) {
                if (em->entities[i]->components[c]) {
                    Component* comp = em->entities[i]->components[c];
                    if (comp->free_func && comp->data) {
                        comp->free_func(comp->data);
                    } else if (comp->data) {
                        free(comp->data);
                    }
                    free(comp);
                }
            }
            free(em->entities[i]);
        }
    }

    free(em->entities);
    free(em);
}

Entity* create_entity(EntityManager* em, const char* name) {
    if (!em) {
        return NULL;
    }

    // Grow array if needed
    if (em->count >= em->capacity) {
        size_t new_capacity = em->capacity * 2;
        Entity** new_entities = realloc(em->entities, new_capacity * sizeof(Entity*));
        if (!new_entities) {
            return NULL;
        }
        em->entities = new_entities;
        em->capacity = new_capacity;
    }

    Entity* entity = calloc(1, sizeof(Entity));
    if (!entity) {
        return NULL;
    }

    entity->id = em->next_id++;
    entity->active = true;
    entity->manager = em;

    if (name) {
        strncpy(entity->name, name, sizeof(entity->name) - 1);
        entity->name[sizeof(entity->name) - 1] = '\0';
    }

    // Default transform
    glm_vec3_zero(entity->position);
    glm_quat_identity(entity->rotation);
    glm_vec3_one(entity->scale);

    entity->component_mask = 0;
    entity->node = NULL;

    em->entities[em->count++] = entity;

    return entity;
}

void destroy_entity(EntityManager* em, Entity* entity) {
    if (!em || !entity) {
        return;
    }

    // Find and remove from array
    for (size_t i = 0; i < em->count; i++) {
        if (em->entities[i] == entity) {
            // Free components
            for (int c = 0; c < COMPONENT_MAX; c++) {
                if (entity->components[c]) {
                    Component* comp = entity->components[c];
                    if (comp->free_func && comp->data) {
                        comp->free_func(comp->data);
                    } else if (comp->data) {
                        free(comp->data);
                    }
                    free(comp);
                }
            }

            // Remove from array (swap with last)
            em->entities[i] = em->entities[em->count - 1];
            em->entities[em->count - 1] = NULL;
            em->count--;

            free(entity);
            return;
        }
    }
}

Entity* find_entity_by_name(EntityManager* em, const char* name) {
    if (!em || !name) {
        return NULL;
    }

    for (size_t i = 0; i < em->count; i++) {
        if (em->entities[i] && strcmp(em->entities[i]->name, name) == 0) {
            return em->entities[i];
        }
    }

    return NULL;
}

Entity* find_entity_by_id(EntityManager* em, uint32_t id) {
    if (!em || id == 0) {
        return NULL;
    }

    for (size_t i = 0; i < em->count; i++) {
        if (em->entities[i] && em->entities[i]->id == id) {
            return em->entities[i];
        }
    }

    return NULL;
}

void* entity_add_component(Entity* entity, ComponentType type, void* data) {
    if (!entity || type <= COMPONENT_NONE || type >= COMPONENT_MAX) {
        return NULL;
    }

    // Remove existing component of same type
    if (entity->components[type]) {
        entity_remove_component(entity, type);
    }

    Component* comp = calloc(1, sizeof(Component));
    if (!comp) {
        return NULL;
    }

    comp->type = type;
    comp->data = data;
    comp->free_func = NULL;

    entity->components[type] = comp;
    entity->component_mask |= COMPONENT_BIT(type);

    return data;
}

void* entity_get_component(Entity* entity, ComponentType type) {
    if (!entity || type <= COMPONENT_NONE || type >= COMPONENT_MAX) {
        return NULL;
    }

    if (entity->components[type]) {
        return entity->components[type]->data;
    }

    return NULL;
}

bool entity_has_component(const Entity* entity, ComponentType type) {
    if (!entity || type <= COMPONENT_NONE || type >= COMPONENT_MAX) {
        return false;
    }

    return (entity->component_mask & COMPONENT_BIT(type)) != 0;
}

void entity_remove_component(Entity* entity, ComponentType type) {
    if (!entity || type <= COMPONENT_NONE || type >= COMPONENT_MAX) {
        return;
    }

    Component* comp = entity->components[type];
    if (!comp) {
        return;
    }

    if (comp->free_func && comp->data) {
        comp->free_func(comp->data);
    } else if (comp->data) {
        free(comp->data);
    }

    free(comp);
    entity->components[type] = NULL;
    entity->component_mask &= ~COMPONENT_BIT(type);
}

void entity_set_component_free(Entity* entity, ComponentType type, void (*free_func)(void*)) {
    if (!entity || type <= COMPONENT_NONE || type >= COMPONENT_MAX) {
        return;
    }

    if (entity->components[type]) {
        entity->components[type]->free_func = free_func;
    }
}

void entity_set_position(Entity* entity, vec3 pos) {
    if (entity) {
        glm_vec3_copy(pos, entity->position);
    }
}

void entity_set_rotation(Entity* entity, versor rot) {
    if (entity) {
        glm_quat_copy(rot, entity->rotation);
    }
}

void entity_set_rotation_euler(Entity* entity, vec3 euler) {
    if (entity) {
        glm_euler_xyz_quat(euler, entity->rotation);
    }
}

void entity_set_scale(Entity* entity, vec3 scale) {
    if (entity) {
        glm_vec3_copy(scale, entity->scale);
    }
}

void entity_set_scale_uniform(Entity* entity, float scale) {
    if (entity) {
        entity->scale[0] = scale;
        entity->scale[1] = scale;
        entity->scale[2] = scale;
    }
}

void entity_translate(Entity* entity, vec3 delta) {
    if (entity) {
        glm_vec3_add(entity->position, delta, entity->position);
    }
}

void entity_rotate(Entity* entity, float angle, vec3 axis) {
    if (entity) {
        versor q;
        glm_quatv(q, angle, axis);
        glm_quat_mul(entity->rotation, q, entity->rotation);
        glm_quat_normalize(entity->rotation);
    }
}

void entity_get_transform_matrix(Entity* entity, mat4 out) {
    if (!entity) {
        glm_mat4_identity(out);
        return;
    }

    glm_mat4_identity(out);
    glm_translate(out, entity->position);
    glm_quat_rotate(out, entity->rotation, out);
    glm_scale(out, entity->scale);
}

void sync_entity_transforms(EntityManager* em) {
    if (!em) {
        return;
    }

    for (size_t i = 0; i < em->count; i++) {
        Entity* entity = em->entities[i];
        if (!entity || !entity->active || !entity->node) {
            continue;
        }

        // Build transform matrix from entity transform
        entity_get_transform_matrix(entity, entity->node->original_transform);
    }
}

void entity_manager_foreach(EntityManager* em, EntityIterFunc func, void* user_data) {
    if (!em || !func) {
        return;
    }

    for (size_t i = 0; i < em->count; i++) {
        if (em->entities[i] && em->entities[i]->active) {
            func(em->entities[i], user_data);
        }
    }
}

void entity_manager_foreach_with(EntityManager* em, uint32_t component_mask, EntityIterFunc func,
                                 void* user_data) {
    if (!em || !func) {
        return;
    }

    for (size_t i = 0; i < em->count; i++) {
        Entity* entity = em->entities[i];
        if (entity && entity->active &&
            (entity->component_mask & component_mask) == component_mask) {
            func(entity, user_data);
        }
    }
}
