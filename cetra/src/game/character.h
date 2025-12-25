#ifndef _CHARACTER_H_
#define _CHARACTER_H_

#include <stdbool.h>
#include <cglm/cglm.h>

// Forward declarations
struct Entity;
struct EntityManager;
struct PhysicsWorld;
struct CharacterController;

/// Ground state enum
typedef enum {
    CHAR_GROUND_ON_GROUND,
    CHAR_GROUND_ON_STEEP_GROUND,
    CHAR_GROUND_NOT_SUPPORTED,
    CHAR_GROUND_IN_AIR
} CharacterGroundState;

/// Character controller configuration
typedef struct CharacterControllerConfig {
    // Shape parameters
    float capsule_radius;
    float capsule_half_height;

    // Movement parameters
    float max_slope_angle; // Radians, default ~50 degrees
    float mass;            // kg, default 70
    float max_strength;    // N, default 100

    // Collision parameters
    float predictive_contact_distance;
    float character_padding;
    float penetration_recovery_speed;

    // Step parameters
    float step_height;
    float step_forward_test;

    // Floor sticking
    float stick_to_floor_distance;
} CharacterControllerConfig;

/// Contact callback
typedef void (*CharacterContactCallback)(struct CharacterController* cc, struct Entity* hit_entity,
                                         vec3 contact_position, vec3 contact_normal,
                                         void* user_data);

/// Main character controller component
typedef struct CharacterController {
    // Jolt character (JPC_CharacterVirtual*)
    void* jolt_character;

    // Contact listener (JPC_CharacterContactListener*)
    void* contact_listener;

    // Owner references
    struct Entity* entity;
    struct PhysicsWorld* world;

    // Config (cached)
    CharacterControllerConfig config;

    // Runtime state
    vec3 velocity;
    CharacterGroundState ground_state;
    vec3 ground_position;
    vec3 ground_normal;
    vec3 ground_velocity;

    // User contact callback
    CharacterContactCallback contact_callback;
    void* contact_user_data;

    // State flags
    bool enabled;
} CharacterController;

/// Get default configuration with sensible values
CharacterControllerConfig character_controller_default_config(void);

/// Add character controller to entity
CharacterController* entity_add_character_controller(struct Entity* entity,
                                                     struct PhysicsWorld* world,
                                                     const CharacterControllerConfig* config);

/// Get character controller from entity
CharacterController* entity_get_character_controller(struct Entity* entity);

/// Remove character controller from entity
void entity_remove_character_controller(struct Entity* entity);

/// Set linear velocity
void character_controller_set_velocity(CharacterController* cc, vec3 velocity);

/// Get linear velocity
void character_controller_get_velocity(const CharacterController* cc, vec3 out);

/// Add to velocity
void character_controller_add_velocity(CharacterController* cc, vec3 delta);

/// Get ground state
CharacterGroundState character_controller_get_ground_state(const CharacterController* cc);

/// Check if on ground (solid or steep)
bool character_controller_is_grounded(const CharacterController* cc);

/// Check if on steep slope
bool character_controller_is_on_steep_slope(const CharacterController* cc);

/// Get position
void character_controller_get_position(const CharacterController* cc, vec3 out);

/// Set position (teleport)
void character_controller_set_position(CharacterController* cc, vec3 position);

/// Get ground normal
void character_controller_get_ground_normal(const CharacterController* cc, vec3 out);

/// Get ground velocity (for moving platforms)
void character_controller_get_ground_velocity(const CharacterController* cc, vec3 out);

/// Set contact callback
void character_controller_set_contact_callback(CharacterController* cc,
                                               CharacterContactCallback callback, void* user_data);

/// Update character (call before physics update)
void character_controller_update(CharacterController* cc, float dt, vec3 gravity);

/// Update all character controllers in entity manager
void update_all_character_controllers(struct EntityManager* em, const struct PhysicsWorld* world,
                                      float dt, vec3 gravity);

/// Sync character positions to entities (call after physics update)
void sync_character_controllers_to_entities(struct EntityManager* em);

#endif // _CHARACTER_H_
