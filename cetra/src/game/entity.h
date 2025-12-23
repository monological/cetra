#ifndef _ENTITY_H_
#define _ENTITY_H_

#include <stdbool.h>
#include <stdint.h>
#include <GL/glew.h>
#include <cglm/cglm.h>

#include "component.h"
#include "../scene.h"

// Forward declarations
struct Game;
struct EntityManager;

// Entity - game object with transform and components
typedef struct Entity {
    uint32_t id;
    char name[64];
    bool active;

    // Transform (always present, not a component)
    vec3 position;
    versor rotation; // quaternion
    vec3 scale;

    // Components array (indexed by ComponentType)
    Component* components[COMPONENT_MAX];
    uint32_t component_mask;

    // Visual representation in scene graph
    SceneNode* node;

    // Parent entity manager
    struct EntityManager* manager;
} Entity;

// Entity manager - owns and updates all entities
typedef struct EntityManager {
    Entity** entities;
    size_t count;
    size_t capacity;
    uint32_t next_id;

    // Back-reference to game
    struct Game* game;
} EntityManager;

// EntityManager lifecycle
EntityManager* create_entity_manager(struct Game* game);
void free_entity_manager(EntityManager* em);

// Entity lifecycle
Entity* create_entity(EntityManager* em, const char* name);
void destroy_entity(EntityManager* em, Entity* entity);

// Entity lookup
Entity* find_entity_by_name(EntityManager* em, const char* name);
Entity* find_entity_by_id(EntityManager* em, uint32_t id);

// Component management
void* entity_add_component(Entity* entity, ComponentType type, void* data);
void* entity_get_component(Entity* entity, ComponentType type);
bool entity_has_component(const Entity* entity, ComponentType type);
void entity_remove_component(Entity* entity, ComponentType type);

// Set custom destructor for component data
void entity_set_component_free(Entity* entity, ComponentType type, void (*free_func)(void*));

// Transform helpers
void entity_set_position(Entity* entity, vec3 pos);
void entity_set_rotation(Entity* entity, versor rot);
void entity_set_rotation_euler(Entity* entity, vec3 euler); // pitch, yaw, roll in radians
void entity_set_scale(Entity* entity, vec3 scale);
void entity_set_scale_uniform(Entity* entity, float scale);

void entity_translate(Entity* entity, vec3 delta);
void entity_rotate(Entity* entity, float angle, vec3 axis);

// Get transform matrix
void entity_get_transform_matrix(Entity* entity, mat4 out);

// Sync entity transforms to scene nodes (call after update)
void sync_entity_transforms(EntityManager* em);

// Iterate all entities (for systems)
typedef void (*EntityIterFunc)(Entity* entity, void* user_data);
void entity_manager_foreach(EntityManager* em, EntityIterFunc func, void* user_data);

// Iterate entities with specific components
void entity_manager_foreach_with(EntityManager* em, uint32_t component_mask, EntityIterFunc func,
                                 void* user_data);

#endif // _ENTITY_H_
