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
typedef uint32_t JPC_BodyID;

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

// Physics configuration
typedef struct PhysicsConfig {
    uint32_t max_bodies;
    uint32_t num_body_mutexes;
    uint32_t max_body_pairs;
    uint32_t max_contact_constraints;
    uint32_t temp_allocator_size;
    int num_threads;
} PhysicsConfig;

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
void sync_entities_to_physics(struct EntityManager* em);

#endif // _PHYSICS_H_
