#ifndef _PHYSICS_H_
#define _PHYSICS_H_

#include <stdbool.h>
#include <stdint.h>
#include <cglm/cglm.h>

#include "component.h"

// Forward declarations
struct Entity;
struct EntityManager;
struct PhysicsWorld;

// JoltC forward declarations
typedef struct JPC_PhysicsSystem JPC_PhysicsSystem;
typedef struct JPC_TempAllocatorImpl JPC_TempAllocatorImpl;
typedef struct JPC_JobSystemThreadPool JPC_JobSystemThreadPool;
typedef struct JPC_BroadPhaseLayerInterface JPC_BroadPhaseLayerInterface;
typedef struct JPC_ObjectVsBroadPhaseLayerFilter JPC_ObjectVsBroadPhaseLayerFilter;
typedef struct JPC_ObjectLayerPairFilter JPC_ObjectLayerPairFilter;
typedef struct JPC_BodyInterface JPC_BodyInterface;
typedef struct JPC_Shape JPC_Shape;
typedef struct JPC_ContactListener JPC_ContactListener;
typedef struct JPC_Constraint JPC_Constraint;
typedef uint32_t JPC_BodyID;

// Internal forward declaration
struct CollisionEventQueue;

// Broad phase layers - determines which objects can potentially collide
typedef enum { BP_LAYER_NON_MOVING = 0, BP_LAYER_MOVING = 1, BP_LAYER_COUNT } BroadPhaseLayer;

// Object layers - more granular collision filtering
typedef enum {
    OBJ_LAYER_STATIC = 0,
    OBJ_LAYER_DYNAMIC = 1,
    OBJ_LAYER_KINEMATIC = 2,
    OBJ_LAYER_TRIGGER = 3,
    OBJ_LAYER_COUNT
} PhysicsLayer;

// Motion types
typedef enum { MOTION_STATIC, MOTION_KINEMATIC, MOTION_DYNAMIC } PhysicsMotionType;

// Shape types
typedef enum { SHAPE_BOX, SHAPE_SPHERE, SHAPE_CAPSULE, SHAPE_CYLINDER } PhysicsShapeType;

// Constraint types
typedef enum {
    CONSTRAINT_FIXED,    // Weld two bodies together
    CONSTRAINT_DISTANCE, // Keep two points at fixed distance
    CONSTRAINT_HINGE,    // Revolute joint (rotate around axis)
    CONSTRAINT_SLIDER,   // Prismatic joint (slide along axis)
    CONSTRAINT_SIXDOF    // 6 degrees of freedom (advanced)
} ConstraintType;

// Motor state for motorized constraints
typedef enum {
    MOTOR_OFF,      // Motor disabled
    MOTOR_VELOCITY, // Maintain target velocity
    MOTOR_POSITION  // Reach target position
} MotorState;

// Spring settings for constraint limits
typedef struct SpringSettings {
    float frequency; // Oscillation frequency in Hz (0 = rigid)
    float damping;   // Damping ratio (0 = no damping, 1 = critical)
} SpringSettings;

// Shape description for creation
typedef struct PhysicsShapeDesc {
    PhysicsShapeType type;
    union {
        struct {
            vec3 half_extents;
        } box;
        struct {
            float radius;
        } sphere;
        struct {
            float radius;
            float half_height;
        } capsule;
        struct {
            float radius;
            float half_height;
        } cylinder;
    };
    float density;
} PhysicsShapeDesc;

// Constraint description for creation
typedef struct ConstraintDesc {
    ConstraintType type;

    // Anchor points in local body space (relative to center of mass)
    vec3 anchor_a; // Anchor on body A
    vec3 anchor_b; // Anchor on body B

    union {
        struct {
            vec3 axis_x; // Local X axis for reference frame
            vec3 axis_y; // Local Y axis for reference frame
        } fixed;

        struct {
            float min_distance;
            float max_distance;
            SpringSettings spring;
        } distance;

        struct {
            vec3 axis;       // Hinge axis (same for both bodies)
            float min_angle; // Minimum angle in radians
            float max_angle; // Maximum angle in radians
            float max_friction_torque;
        } hinge;

        struct {
            vec3 axis; // Slide axis (same for both bodies)
            float min_distance;
            float max_distance;
            float max_friction_force;
        } slider;

        struct {
            vec3 axis_x;                 // Reference frame X axis
            vec3 axis_y;                 // Reference frame Y axis
            vec3 translation_limits_min; // Min translation on each axis
            vec3 translation_limits_max; // Max translation on each axis
            vec3 rotation_limits_min;    // Min rotation on each axis (radians)
            vec3 rotation_limits_max;    // Max rotation on each axis (radians)
        } sixdof;
    };
} ConstraintDesc;

// Forward declaration for Constraint
struct Constraint;

// Constraint wrapper
typedef struct Constraint {
    JPC_Constraint* jolt_constraint;
    ConstraintType type;
    struct PhysicsWorld* world;
    struct RigidBody* body_a;
    struct RigidBody* body_b;
    bool is_added;
    bool enabled;
} Constraint;

// Physics configuration
typedef struct PhysicsConfig {
    uint32_t max_bodies;
    uint32_t num_body_mutexes;
    uint32_t max_body_pairs;
    uint32_t max_contact_constraints;
    uint32_t temp_allocator_size;
    int num_threads;
} PhysicsConfig;

// Collision event types (defined early for use in PhysicsWorld)
typedef enum {
    COLLISION_BEGIN, // First frame of contact
    COLLISION_STAY,  // Contact persists (optional, disabled by default)
    COLLISION_END    // Contact ended
} CollisionEventType;

// Collision event data
typedef struct CollisionEvent {
    CollisionEventType type;
    struct Entity* entity_a;
    struct Entity* entity_b;
    struct RigidBody* body_a;
    struct RigidBody* body_b;
    vec3 contact_point;  // World space (average of contact points)
    vec3 contact_normal; // From A to B
    float penetration_depth;
} CollisionEvent;

// Collision callback signature
typedef void (*CollisionCallback)(const CollisionEvent* event, void* user_data);

// Main physics world wrapper
typedef struct PhysicsWorld {
    JPC_PhysicsSystem* physics_system;
    JPC_TempAllocatorImpl* temp_allocator;
    JPC_JobSystemThreadPool* job_system;
    JPC_BroadPhaseLayerInterface* broad_phase_layer_interface;
    JPC_ObjectVsBroadPhaseLayerFilter* object_vs_broadphase_filter;
    JPC_ObjectLayerPairFilter* object_layer_pair_filter;
    JPC_BodyInterface* body_interface;
    bool initialized;
    // Collision callbacks
    JPC_ContactListener* contact_listener;
    void* contact_listener_ctx; // Opaque pointer to internal context
    struct CollisionEventQueue* event_queue;
    CollisionCallback collision_callback;
    void* collision_user_data;
    bool report_stay_events;
    // Constraint storage
    Constraint** constraints;
    size_t constraint_count;
    size_t constraint_capacity;
} PhysicsWorld;

// RigidBody component data
typedef struct RigidBody {
    JPC_BodyID body_id;
    const JPC_Shape* shape;
    struct Entity* entity;
    struct PhysicsWorld* world;
    PhysicsMotionType motion_type;
    PhysicsLayer layer;
    float friction;
    float restitution;
    float linear_damping;
    float angular_damping;
    float gravity_factor;
    bool is_sensor;
    bool allow_sleep;
    bool is_added;
} RigidBody;

// Raycast hit result
typedef struct RaycastHit {
    struct Entity* entity;  // Hit entity (NULL if body has no entity)
    struct RigidBody* body; // Hit rigid body
    vec3 position;          // World-space hit point
    vec3 normal;            // Surface normal at hit point
    float distance;         // Distance from ray origin
    float fraction;         // 0.0-1.0 along ray length
    bool hit;               // True if something was hit
} RaycastHit;

// PhysicsWorld lifecycle
PhysicsConfig physics_default_config(void);
PhysicsWorld* create_physics_world(const PhysicsConfig* config);
void free_physics_world(PhysicsWorld* world);
uint32_t physics_world_update(PhysicsWorld* world, float dt, int collision_steps);
void physics_world_optimize(PhysicsWorld* world);

// Shape creation helpers
JPC_Shape* physics_create_box_shape(vec3 half_extents, float density);
JPC_Shape* physics_create_sphere_shape(float radius, float density);
JPC_Shape* physics_create_capsule_shape(float radius, float half_height, float density);
JPC_Shape* physics_create_cylinder_shape(float radius, float half_height, float density);
JPC_Shape* physics_create_shape_from_desc(const PhysicsShapeDesc* desc);

// RigidBody component API
RigidBody* entity_add_rigid_body(struct Entity* entity, PhysicsWorld* world,
                                 const PhysicsShapeDesc* shape_desc, PhysicsMotionType motion_type,
                                 PhysicsLayer layer);
RigidBody* entity_get_rigid_body(struct Entity* entity);
void entity_remove_rigid_body(struct Entity* entity);

// RigidBody physics functions
void rigid_body_add_force(RigidBody* rb, vec3 force);
void rigid_body_add_force_at_point(RigidBody* rb, vec3 force, vec3 point);
void rigid_body_add_impulse(RigidBody* rb, vec3 impulse);
void rigid_body_add_angular_impulse(RigidBody* rb, vec3 impulse);
void rigid_body_add_torque(RigidBody* rb, vec3 torque);

// Velocity access
void rigid_body_get_linear_velocity(RigidBody* rb, vec3 out);
void rigid_body_set_linear_velocity(RigidBody* rb, vec3 velocity);
void rigid_body_get_angular_velocity(RigidBody* rb, vec3 out);
void rigid_body_set_angular_velocity(RigidBody* rb, vec3 velocity);

// Transform access (for kinematic bodies)
void rigid_body_set_position(RigidBody* rb, vec3 position);
void rigid_body_set_rotation(RigidBody* rb, versor rotation);
void rigid_body_move_kinematic(RigidBody* rb, vec3 target_pos, versor target_rot, float dt);

// Activation
void rigid_body_activate(RigidBody* rb);
void rigid_body_deactivate(RigidBody* rb);
bool rigid_body_is_active(RigidBody* rb);

// Transform synchronization
void sync_physics_to_entities(PhysicsWorld* world, struct EntityManager* em);
void sync_entities_to_physics(struct EntityManager* em, float dt);

// Raycasting
bool physics_world_raycast(PhysicsWorld* world, vec3 origin, vec3 direction, float max_distance,
                           RaycastHit* out_hit);
bool physics_world_raycast_filtered(PhysicsWorld* world, vec3 origin, vec3 direction,
                                    float max_distance, uint32_t layer_mask, RaycastHit* out_hit);
bool physics_world_raycast_ignore(PhysicsWorld* world, vec3 origin, vec3 direction,
                                  float max_distance, const RigidBody* ignore, RaycastHit* out_hit);

// Collision callbacks
void physics_world_set_collision_callback(PhysicsWorld* world, CollisionCallback callback,
                                          void* user_data);
void physics_world_set_report_stay_events(PhysicsWorld* world, bool enabled);
void physics_world_process_collisions(PhysicsWorld* world);

// Constraint creation/destruction
Constraint* create_constraint(PhysicsWorld* world, RigidBody* body_a, RigidBody* body_b,
                              const ConstraintDesc* desc);
void free_constraint(Constraint* constraint);

// Constraint world management
void physics_world_add_constraint(PhysicsWorld* world, Constraint* constraint);
void physics_world_remove_constraint(PhysicsWorld* world, Constraint* constraint);
void physics_world_remove_constraints_for_body(PhysicsWorld* world, const RigidBody* body);

// Constraint enable/disable
void constraint_set_enabled(Constraint* c, bool enabled);
bool constraint_is_enabled(const Constraint* c);

// Hinge constraint motor control
void constraint_hinge_set_motor_state(Constraint* c, MotorState state);
void constraint_hinge_set_target_velocity(Constraint* c, float velocity);
void constraint_hinge_set_target_angle(Constraint* c, float angle);
float constraint_hinge_get_current_angle(const Constraint* c);

// Slider constraint motor control
void constraint_slider_set_motor_state(Constraint* c, MotorState state);
void constraint_slider_set_target_velocity(Constraint* c, float velocity);
void constraint_slider_set_target_position(Constraint* c, float position);
float constraint_slider_get_current_position(const Constraint* c);

#endif // _PHYSICS_H_
