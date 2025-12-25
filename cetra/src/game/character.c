#include "character.h"
#include "entity.h"
#include "physics.h"
#include "component.h"

#include <stdlib.h>
#include <string.h>

// Include JoltC headers
#include <JoltC/JoltC.h>
#include <JoltC/JoltCharacter.h>

// Internal context for contact listener
typedef struct CharacterContactContext {
    CharacterController* controller;
} CharacterContactContext;

// Forward declare body_entity_map_find (internal to physics.c, we'll add a public wrapper)
Entity* physics_world_find_entity_by_body_id(PhysicsWorld* world, JPC_BodyID body_id);

// Contact validate callback - return false to ignore collision with body
static bool on_character_contact_validate(void* user_data, const JPC_CharacterVirtual* character,
                                          JPC_BodyID body_id) {
    (void)character;

    CharacterContactContext* ctx = (CharacterContactContext*)user_data;
    if (!ctx || !ctx->controller)
        return true;

    CharacterController* cc = ctx->controller;

    // For dynamic constrained bodies (doors), trigger callback then ignore collision
    if (physics_world_body_has_constraint(cc->world, body_id)) {
        Entity* entity = physics_world_find_entity_by_body_id(cc->world, body_id);
        if (entity) {
            const RigidBody* rb = entity_get_rigid_body(entity);
            if (rb && rb->motion_type == MOTION_DYNAMIC) {
                // Trigger user callback so game can set motor target
                if (cc->contact_callback) {
                    vec3 pos = {0, 0, 0}; // Position not available in validate
                    vec3 normal = {0, 0, 0};
                    cc->contact_callback(cc, entity, pos, normal, cc->contact_user_data);
                }
                return false; // Ignore collision - motor opens door
            }
        }
    }

    return true; // Collide with everything else
}

// Contact listener callback
static void on_character_contact_added(void* user_data, const JPC_CharacterVirtual* character,
                                       JPC_BodyID body_id, const float contact_position[3],
                                       const float contact_normal[3],
                                       JPC_CharacterContactSettings* settings) {
    (void)character;
    (void)settings;
    CharacterContactContext* ctx = (CharacterContactContext*)user_data;
    if (!ctx || !ctx->controller)
        return;

    CharacterController* cc = ctx->controller;

    Entity* hit_entity = physics_world_find_entity_by_body_id(cc->world, body_id);

    // Call user callback if set
    if (cc->contact_callback) {
        vec3 pos, normal;
        glm_vec3_copy((float*)contact_position, pos);
        glm_vec3_copy((float*)contact_normal, normal);

        cc->contact_callback(cc, hit_entity, pos, normal, cc->contact_user_data);
    }
}

// Contact solve callback - handles collision response for dynamic bodies
static void on_character_contact_solve(void* user_data, const JPC_CharacterVirtual* character,
                                       JPC_BodyID body_id, const float contact_position[3],
                                       const float contact_normal[3],
                                       const float contact_velocity[3],
                                       const float character_velocity[3],
                                       float out_new_character_velocity[3]) {
    (void)character;
    (void)contact_velocity;

    CharacterContactContext* ctx = (CharacterContactContext*)user_data;
    if (!ctx || !ctx->controller)
        return;

    CharacterController* cc = ctx->controller;

    // Find the entity we hit
    Entity* hit_entity = physics_world_find_entity_by_body_id(cc->world, body_id);
    if (!hit_entity)
        return;

    // Get the rigid body
    RigidBody* rb = entity_get_rigid_body(hit_entity);
    if (!rb || rb->motion_type != MOTION_DYNAMIC)
        return;

    // For constrained bodies (doors), let the motor handle it
    // Game code sets motor target on contact, motor opens the door
    if (physics_world_body_has_constraint(cc->world, body_id)) {
        return;
    }

    // For loose objects (boxes), apply impulse to push them
    float vel_into_object =
        -(character_velocity[0] * contact_normal[0] + character_velocity[1] * contact_normal[1] +
          character_velocity[2] * contact_normal[2]);

    if (vel_into_object <= 0.0f)
        return;

    vec3 impulse = {-contact_normal[0] * vel_into_object * cc->config.mass,
                    -contact_normal[1] * vel_into_object * cc->config.mass,
                    -contact_normal[2] * vel_into_object * cc->config.mass};

    vec3 pos = {contact_position[0], contact_position[1], contact_position[2]};
    rigid_body_add_impulse_at_point(rb, impulse, pos);
    rigid_body_activate(rb);
}

// Component destructor
static void free_character_controller(void* data) {
    CharacterController* cc = (CharacterController*)data;
    if (!cc)
        return;

    // Delete contact listener
    if (cc->contact_listener) {
        JPC_CharacterContactListener_delete(cc->contact_listener);
    }

    // Delete Jolt character
    if (cc->jolt_character) {
        JPC_CharacterVirtual_delete(cc->jolt_character);
    }

    free(cc);
}

CharacterControllerConfig character_controller_default_config(void) {
    return (CharacterControllerConfig){.capsule_radius = 0.5f,
                                       .capsule_half_height = 0.5f, // Total height ~2m with radius
                                       .max_slope_angle = GLM_PIf * 50.0f / 180.0f,
                                       .mass = 70.0f,
                                       .max_strength = 100.0f,
                                       .predictive_contact_distance = 0.1f,
                                       .character_padding = 0.02f,
                                       .penetration_recovery_speed = 1.0f,
                                       .step_height = 0.4f,
                                       .step_forward_test = 0.15f,
                                       .stick_to_floor_distance = 0.5f};
}

CharacterController* entity_add_character_controller(Entity* entity, PhysicsWorld* world,
                                                     const CharacterControllerConfig* config) {
    if (!entity || !world || !config)
        return NULL;

    if (entity_has_component(entity, COMPONENT_CHARACTER))
        return NULL;

    // Create capsule shape for character
    // Note: Jolt capsule is defined by half-height of the cylinder part + radius
    const JPC_Shape* shape =
        physics_create_capsule_shape(config->capsule_radius, config->capsule_half_height,
                                     1000.0f); // Density not used for virtual character
    if (!shape)
        return NULL;

    // Create settings
    JPC_CharacterVirtualSettings* settings = JPC_CharacterVirtualSettings_new();
    if (!settings) {
        return NULL;
    }

    JPC_CharacterVirtualSettings_SetShape(settings, shape);
    JPC_CharacterVirtualSettings_SetMass(settings, config->mass);
    JPC_CharacterVirtualSettings_SetMaxStrength(settings, config->max_strength);
    JPC_CharacterVirtualSettings_SetMaxSlopeAngle(settings, config->max_slope_angle);
    JPC_CharacterVirtualSettings_SetPredictiveContactDistance(settings,
                                                              config->predictive_contact_distance);
    JPC_CharacterVirtualSettings_SetCharacterPadding(settings, config->character_padding);
    JPC_CharacterVirtualSettings_SetPenetrationRecoverySpeed(settings,
                                                             config->penetration_recovery_speed);

    // Create inner body - a kinematic body that moves with the character and pushes dynamic objects
    // Use DYNAMIC layer (same as other moving objects) as per Jolt's samples
    JPC_CharacterVirtualSettings_SetInnerBodyShape(settings, shape);
    JPC_CharacterVirtualSettings_SetInnerBodyLayer(settings, OBJ_LAYER_DYNAMIC);

    // Create character
    float position[3] = {entity->position[0], entity->position[1], entity->position[2]};
    float rotation[4] = {entity->rotation[0], entity->rotation[1], entity->rotation[2],
                         entity->rotation[3]};

    JPC_CharacterVirtual* jolt_char =
        JPC_CharacterVirtual_new(settings, position, rotation,
                                 (uint64_t)(uintptr_t)entity, // Store entity pointer as user data
                                 world->physics_system);

    JPC_CharacterVirtualSettings_delete(settings);

    if (!jolt_char) {
        return NULL;
    }

    // Allocate controller
    CharacterController* cc = calloc(1, sizeof(CharacterController));
    if (!cc) {
        JPC_CharacterVirtual_delete(jolt_char);
        return NULL;
    }

    cc->jolt_character = jolt_char;
    cc->entity = entity;
    cc->world = world;
    cc->config = *config;
    cc->enabled = true;
    cc->ground_state = CHAR_GROUND_IN_AIR;

    glm_vec3_zero(cc->velocity);
    glm_vec3_zero(cc->ground_position);
    glm_vec3_zero(cc->ground_normal);
    glm_vec3_zero(cc->ground_velocity);

    // Create contact listener context
    CharacterContactContext* ctx = malloc(sizeof(CharacterContactContext));
    if (ctx) {
        ctx->controller = cc;

        JPC_CharacterContactListenerFns fns = {
            .OnContactValidate = on_character_contact_validate,
            .OnContactAdded = on_character_contact_added,
            .OnContactPersisted =
                on_character_contact_added, // Same callback for persistent contacts
            .OnContactSolve = on_character_contact_solve};

        cc->contact_listener = JPC_CharacterContactListener_new(ctx, fns);
        if (cc->contact_listener) {
            JPC_CharacterVirtual_SetListener(jolt_char, cc->contact_listener);
        }
    }

    // Register component
    entity_add_component(entity, COMPONENT_CHARACTER, cc);
    entity_set_component_free(entity, COMPONENT_CHARACTER, free_character_controller);

    return cc;
}

CharacterController* entity_get_character_controller(Entity* entity) {
    if (!entity)
        return NULL;
    return (CharacterController*)entity_get_component(entity, COMPONENT_CHARACTER);
}

void entity_remove_character_controller(Entity* entity) {
    if (!entity)
        return;
    entity_remove_component(entity, COMPONENT_CHARACTER);
}

void character_controller_set_velocity(CharacterController* cc, vec3 velocity) {
    if (!cc)
        return;
    glm_vec3_copy(velocity, cc->velocity);
}

void character_controller_get_velocity(const CharacterController* cc, vec3 out) {
    if (!cc) {
        glm_vec3_zero(out);
        return;
    }
    glm_vec3_copy((float*)cc->velocity, out);
}

void character_controller_add_velocity(CharacterController* cc, vec3 delta) {
    if (!cc)
        return;
    glm_vec3_add(cc->velocity, delta, cc->velocity);
}

CharacterGroundState character_controller_get_ground_state(const CharacterController* cc) {
    if (!cc)
        return CHAR_GROUND_IN_AIR;
    return cc->ground_state;
}

bool character_controller_is_grounded(const CharacterController* cc) {
    if (!cc)
        return false;
    return cc->ground_state == CHAR_GROUND_ON_GROUND ||
           cc->ground_state == CHAR_GROUND_ON_STEEP_GROUND;
}

bool character_controller_is_on_steep_slope(const CharacterController* cc) {
    if (!cc)
        return false;
    return cc->ground_state == CHAR_GROUND_ON_STEEP_GROUND;
}

void character_controller_get_position(const CharacterController* cc, vec3 out) {
    if (!cc || !cc->jolt_character) {
        glm_vec3_zero(out);
        return;
    }
    JPC_CharacterVirtual_GetPosition(cc->jolt_character, out);
}

void character_controller_set_position(CharacterController* cc, vec3 position) {
    if (!cc || !cc->jolt_character)
        return;
    JPC_CharacterVirtual_SetPosition(cc->jolt_character, position);
}

void character_controller_get_ground_normal(const CharacterController* cc, vec3 out) {
    if (!cc) {
        glm_vec3_zero(out);
        return;
    }
    glm_vec3_copy((float*)cc->ground_normal, out);
}

void character_controller_get_ground_velocity(const CharacterController* cc, vec3 out) {
    if (!cc) {
        glm_vec3_zero(out);
        return;
    }
    glm_vec3_copy((float*)cc->ground_velocity, out);
}

void character_controller_set_contact_callback(CharacterController* cc,
                                               CharacterContactCallback callback, void* user_data) {
    if (!cc)
        return;
    cc->contact_callback = callback;
    cc->contact_user_data = user_data;
}

void character_controller_update(CharacterController* cc, float dt, vec3 gravity) {
    if (!cc || !cc->enabled || !cc->jolt_character || !cc->world)
        return;

    // Set velocity on Jolt character
    JPC_CharacterVirtual_SetLinearVelocity(cc->jolt_character, cc->velocity);

    // Create extended update settings
    JPC_ExtendedUpdateSettings ext_settings;
    JPC_ExtendedUpdateSettings_default(&ext_settings);

    // Configure step parameters
    ext_settings.stick_to_floor_step_down[0] = 0.0f;
    ext_settings.stick_to_floor_step_down[1] = -cc->config.stick_to_floor_distance;
    ext_settings.stick_to_floor_step_down[2] = 0.0f;

    ext_settings.walk_stairs_step_up[0] = 0.0f;
    ext_settings.walk_stairs_step_up[1] = cc->config.step_height;
    ext_settings.walk_stairs_step_up[2] = 0.0f;

    ext_settings.walk_stairs_step_forward_test = cc->config.step_forward_test;

    // Extended update handles: Update + StickToFloor + WalkStairs
    JPC_CharacterVirtual_ExtendedUpdate(cc->jolt_character, dt, gravity, &ext_settings,
                                        cc->world->physics_system, cc->world->temp_allocator);

    // Get updated velocity
    JPC_CharacterVirtual_GetLinearVelocity(cc->jolt_character, cc->velocity);

    // Update ground state (convert from JPC_GroundState enum values)
    JPC_GroundState jolt_state = JPC_CharacterVirtual_GetGroundState(cc->jolt_character);
    switch (jolt_state) {
        case JPC_CHARACTER_GROUND_STATE_ON_GROUND:
            cc->ground_state = CHAR_GROUND_ON_GROUND;
            break;
        case JPC_CHARACTER_GROUND_STATE_ON_STEEP_GROUND:
            cc->ground_state = CHAR_GROUND_ON_STEEP_GROUND;
            break;
        case JPC_CHARACTER_GROUND_STATE_NOT_SUPPORTED:
            cc->ground_state = CHAR_GROUND_NOT_SUPPORTED;
            break;
        case JPC_CHARACTER_GROUND_STATE_IN_AIR:
        default:
            cc->ground_state = CHAR_GROUND_IN_AIR;
            break;
    }

    // Cache ground info
    JPC_CharacterVirtual_GetGroundPosition(cc->jolt_character, cc->ground_position);
    JPC_CharacterVirtual_GetGroundNormal(cc->jolt_character, cc->ground_normal);
    JPC_CharacterVirtual_GetGroundVelocity(cc->jolt_character, cc->ground_velocity);
}

void update_all_character_controllers(EntityManager* em, const PhysicsWorld* world, float dt,
                                      vec3 gravity) {
    if (!em || !world)
        return;

    for (size_t i = 0; i < em->count; i++) {
        Entity* e = em->entities[i];
        if (!e || !e->active)
            continue;

        CharacterController* cc = entity_get_character_controller(e);
        if (cc) {
            character_controller_update(cc, dt, gravity);
        }
    }
}

void sync_character_controllers_to_entities(EntityManager* em) {
    if (!em)
        return;

    for (size_t i = 0; i < em->count; i++) {
        Entity* e = em->entities[i];
        if (!e || !e->active)
            continue;

        CharacterController* cc = entity_get_character_controller(e);
        if (!cc || !cc->jolt_character)
            continue;

        // Get position from Jolt character
        float pos[3];
        JPC_CharacterVirtual_GetPosition(cc->jolt_character, pos);

        // Update entity position
        glm_vec3_copy(pos, e->position);

        // Get rotation from Jolt character
        float rot[4];
        JPC_CharacterVirtual_GetRotation(cc->jolt_character, rot);
        glm_vec4_copy(rot, e->rotation);
    }
}
