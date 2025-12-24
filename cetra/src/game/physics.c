#include "physics.h"
#include "entity.h"
#include "JoltC/JoltC.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Forward declarations for collision system
typedef struct CollisionEventQueue CollisionEventQueue;
static CollisionEventQueue* create_collision_event_queue(size_t initial_capacity);
static void free_collision_event_queue(CollisionEventQueue* queue);

// Forward declaration for constraint cleanup
void physics_world_remove_constraints_for_body(PhysicsWorld* world, const RigidBody* body);

// Contact listener context - defined here for sizeof
typedef struct ContactListenerContext {
    PhysicsWorld* world;
} ContactListenerContext;

static JPC_ValidateResult on_contact_validate(void* self, const JPC_Body* body1,
                                              const JPC_Body* body2, JPC_RVec3 base_offset,
                                              const JPC_CollideShapeResult* collision_result);
static void on_contact_added(void* self, const JPC_Body* body1, const JPC_Body* body2,
                             const JPC_ContactManifold* manifold, JPC_ContactSettings* settings);
static void on_contact_persisted(void* self, const JPC_Body* body1, const JPC_Body* body2,
                                 const JPC_ContactManifold* manifold,
                                 JPC_ContactSettings* settings);
static void on_contact_removed(void* self, const JPC_SubShapeIDPair* pair);

// Type conversion helpers
static inline JPC_Vec3 vec3_to_jpc(const vec3 v) {
    JPC_Vec3 result = {.x = v[0], .y = v[1], .z = v[2], ._w = 0.0f};
    return result;
}

static inline void jpc_to_vec3(JPC_Vec3 jv, vec3 out) {
    out[0] = jv.x;
    out[1] = jv.y;
    out[2] = jv.z;
}

static inline JPC_RVec3 vec3_to_jpc_r(const vec3 v) {
    JPC_RVec3 result;
    result.x = v[0];
    result.y = v[1];
    result.z = v[2];
    return result;
}

static inline void jpc_r_to_vec3(JPC_RVec3 jv, vec3 out) {
    out[0] = (float)jv.x;
    out[1] = (float)jv.y;
    out[2] = (float)jv.z;
}

static inline JPC_Quat quat_to_jpc(const versor q) {
    JPC_Quat result = {.x = q[0], .y = q[1], .z = q[2], .w = q[3]};
    return result;
}

static inline void jpc_to_quat(JPC_Quat jq, versor out) {
    out[0] = jq.x;
    out[1] = jq.y;
    out[2] = jq.z;
    out[3] = jq.w;
}

// Layer callback implementations
static unsigned int get_num_broad_phase_layers(const void* self) {
    (void)self;
    return BP_LAYER_COUNT;
}

static JPC_BroadPhaseLayer get_broad_phase_layer(const void* self, JPC_ObjectLayer layer) {
    (void)self;
    switch (layer) {
        case OBJ_LAYER_STATIC:
            return BP_LAYER_NON_MOVING;
        default:
            return BP_LAYER_MOVING;
    }
}

static bool object_vs_broadphase_should_collide(const void* self, JPC_ObjectLayer layer1,
                                                JPC_BroadPhaseLayer layer2) {
    (void)self;
    if (layer1 == OBJ_LAYER_STATIC) {
        return layer2 == BP_LAYER_MOVING;
    }
    return true;
}

static bool object_pair_should_collide(const void* self, JPC_ObjectLayer layer1,
                                       JPC_ObjectLayer layer2) {
    (void)self;
    if (layer1 == OBJ_LAYER_STATIC && layer2 == OBJ_LAYER_STATIC) {
        return false;
    }
    if (layer1 == OBJ_LAYER_TRIGGER && layer2 == OBJ_LAYER_TRIGGER) {
        return false;
    }
    return true;
}

PhysicsConfig physics_default_config(void) {
    PhysicsConfig config = {.max_bodies = 10240,
                            .num_body_mutexes = 0,
                            .max_body_pairs = 65536,
                            .max_contact_constraints = 10240,
                            .temp_allocator_size = 10 * 1024 * 1024,
                            .num_threads = -1};
    return config;
}

PhysicsWorld* create_physics_world(const PhysicsConfig* config) {
    PhysicsWorld* world = malloc(sizeof(PhysicsWorld));
    if (!world)
        return NULL;
    memset(world, 0, sizeof(PhysicsWorld));

    JPC_RegisterDefaultAllocator();
    JPC_FactoryInit();
    JPC_RegisterTypes();

    world->temp_allocator = JPC_TempAllocatorImpl_new(config->temp_allocator_size);
    if (!world->temp_allocator) {
        free(world);
        return NULL;
    }

    if (config->num_threads > 0) {
        world->job_system = JPC_JobSystemThreadPool_new3(
            JPC_MAX_PHYSICS_JOBS, JPC_MAX_PHYSICS_BARRIERS, config->num_threads);
    } else {
        world->job_system =
            JPC_JobSystemThreadPool_new2(JPC_MAX_PHYSICS_JOBS, JPC_MAX_PHYSICS_BARRIERS);
    }
    if (!world->job_system) {
        JPC_TempAllocatorImpl_delete(world->temp_allocator);
        free(world);
        return NULL;
    }

    JPC_BroadPhaseLayerInterfaceFns bp_fns = {.GetNumBroadPhaseLayers = get_num_broad_phase_layers,
                                              .GetBroadPhaseLayer = get_broad_phase_layer};
    world->broad_phase_layer_interface = JPC_BroadPhaseLayerInterface_new(NULL, bp_fns);

    JPC_ObjectVsBroadPhaseLayerFilterFns obj_bp_fns = {.ShouldCollide =
                                                           object_vs_broadphase_should_collide};
    world->object_vs_broadphase_filter = JPC_ObjectVsBroadPhaseLayerFilter_new(NULL, obj_bp_fns);

    JPC_ObjectLayerPairFilterFns obj_pair_fns = {.ShouldCollide = object_pair_should_collide};
    world->object_layer_pair_filter = JPC_ObjectLayerPairFilter_new(NULL, obj_pair_fns);

    world->physics_system = JPC_PhysicsSystem_new();
    if (!world->physics_system) {
        JPC_ObjectLayerPairFilter_delete(world->object_layer_pair_filter);
        JPC_ObjectVsBroadPhaseLayerFilter_delete(world->object_vs_broadphase_filter);
        JPC_BroadPhaseLayerInterface_delete(world->broad_phase_layer_interface);
        JPC_JobSystemThreadPool_delete(world->job_system);
        JPC_TempAllocatorImpl_delete(world->temp_allocator);
        free(world);
        return NULL;
    }

    JPC_PhysicsSystem_Init(world->physics_system, config->max_bodies, config->num_body_mutexes,
                           config->max_body_pairs, config->max_contact_constraints,
                           world->broad_phase_layer_interface, world->object_vs_broadphase_filter,
                           world->object_layer_pair_filter);

    world->body_interface = JPC_PhysicsSystem_GetBodyInterface(world->physics_system);

    // Initialize collision event queue
    world->event_queue = create_collision_event_queue(64);
    if (!world->event_queue) {
        JPC_PhysicsSystem_delete(world->physics_system);
        JPC_ObjectLayerPairFilter_delete(world->object_layer_pair_filter);
        JPC_ObjectVsBroadPhaseLayerFilter_delete(world->object_vs_broadphase_filter);
        JPC_BroadPhaseLayerInterface_delete(world->broad_phase_layer_interface);
        JPC_JobSystemThreadPool_delete(world->job_system);
        JPC_TempAllocatorImpl_delete(world->temp_allocator);
        free(world);
        return NULL;
    }

    // Create contact listener context
    ContactListenerContext* listener_ctx = malloc(sizeof(ContactListenerContext));
    if (!listener_ctx) {
        free_collision_event_queue(world->event_queue);
        JPC_PhysicsSystem_delete(world->physics_system);
        JPC_ObjectLayerPairFilter_delete(world->object_layer_pair_filter);
        JPC_ObjectVsBroadPhaseLayerFilter_delete(world->object_vs_broadphase_filter);
        JPC_BroadPhaseLayerInterface_delete(world->broad_phase_layer_interface);
        JPC_JobSystemThreadPool_delete(world->job_system);
        JPC_TempAllocatorImpl_delete(world->temp_allocator);
        free(world);
        return NULL;
    }
    listener_ctx->world = world;
    world->contact_listener_ctx = listener_ctx;

    // Create and register contact listener
    JPC_ContactListenerFns contact_fns = {.OnContactValidate = on_contact_validate,
                                          .OnContactAdded = on_contact_added,
                                          .OnContactPersisted = on_contact_persisted,
                                          .OnContactRemoved = on_contact_removed};
    world->contact_listener = JPC_ContactListener_new(listener_ctx, contact_fns);
    JPC_PhysicsSystem_SetContactListener(world->physics_system, world->contact_listener);

    world->report_stay_events = false;
    world->initialized = true;

    return world;
}

void free_physics_world(PhysicsWorld* world) {
    if (!world)
        return;

    // Free all constraints first
    for (size_t i = 0; i < world->constraint_count; i++) {
        Constraint* c = world->constraints[i];
        if (c) {
            if (c->is_added) {
                JPC_PhysicsSystem_RemoveConstraint(world->physics_system, c->jolt_constraint);
                c->is_added = false;
            }
            if (c->jolt_constraint) {
                JPC_Constraint_Release(c->jolt_constraint);
            }
            free(c);
        }
    }
    free(world->constraints);
    world->constraints = NULL;
    world->constraint_count = 0;
    world->constraint_capacity = 0;

    // Clean up collision system
    if (world->contact_listener) {
        JPC_ContactListener_delete(world->contact_listener);
    }
    if (world->contact_listener_ctx) {
        free(world->contact_listener_ctx);
    }
    if (world->event_queue) {
        free_collision_event_queue(world->event_queue);
    }

    if (world->physics_system) {
        JPC_PhysicsSystem_delete(world->physics_system);
    }
    if (world->object_layer_pair_filter) {
        JPC_ObjectLayerPairFilter_delete(world->object_layer_pair_filter);
    }
    if (world->object_vs_broadphase_filter) {
        JPC_ObjectVsBroadPhaseLayerFilter_delete(world->object_vs_broadphase_filter);
    }
    if (world->broad_phase_layer_interface) {
        JPC_BroadPhaseLayerInterface_delete(world->broad_phase_layer_interface);
    }
    if (world->job_system) {
        JPC_JobSystemThreadPool_delete(world->job_system);
    }
    if (world->temp_allocator) {
        JPC_TempAllocatorImpl_delete(world->temp_allocator);
    }

    JPC_UnregisterTypes();
    JPC_FactoryDelete();

    free(world);
}

uint32_t physics_world_update(PhysicsWorld* world, float dt, int collision_steps) {
    if (!world || !world->initialized)
        return 0;

    return JPC_PhysicsSystem_Update(world->physics_system, dt, collision_steps,
                                    world->temp_allocator, (JPC_JobSystem*)world->job_system);
}

void physics_world_optimize(PhysicsWorld* world) {
    if (!world || !world->physics_system)
        return;
    JPC_PhysicsSystem_OptimizeBroadPhase(world->physics_system);
}

// Shape creation helpers
JPC_Shape* physics_create_box_shape(vec3 half_extents, float density) {
    JPC_BoxShapeSettings settings;
    JPC_BoxShapeSettings_default(&settings);
    settings.HalfExtent = vec3_to_jpc(half_extents);
    settings.Density = density > 0 ? density : 1000.0f;

    JPC_Shape* shape = NULL;
    JPC_String* error = NULL;
    if (!JPC_BoxShapeSettings_Create(&settings, &shape, &error)) {
        return NULL;
    }
    return shape;
}

JPC_Shape* physics_create_sphere_shape(float radius, float density) {
    JPC_SphereShapeSettings settings;
    JPC_SphereShapeSettings_default(&settings);
    settings.Radius = radius;
    settings.Density = density > 0 ? density : 1000.0f;

    JPC_Shape* shape = NULL;
    JPC_String* error = NULL;
    if (!JPC_SphereShapeSettings_Create(&settings, &shape, &error)) {
        return NULL;
    }
    return shape;
}

JPC_Shape* physics_create_capsule_shape(float radius, float half_height, float density) {
    JPC_CapsuleShapeSettings settings;
    JPC_CapsuleShapeSettings_default(&settings);
    settings.Radius = radius;
    settings.HalfHeightOfCylinder = half_height;
    settings.Density = density > 0 ? density : 1000.0f;

    JPC_Shape* shape = NULL;
    JPC_String* error = NULL;
    if (!JPC_CapsuleShapeSettings_Create(&settings, &shape, &error)) {
        return NULL;
    }
    return shape;
}

JPC_Shape* physics_create_cylinder_shape(float radius, float half_height, float density) {
    JPC_CylinderShapeSettings settings;
    JPC_CylinderShapeSettings_default(&settings);
    settings.Radius = radius;
    settings.HalfHeight = half_height;
    settings.Density = density > 0 ? density : 1000.0f;

    JPC_Shape* shape = NULL;
    JPC_String* error = NULL;
    if (!JPC_CylinderShapeSettings_Create(&settings, &shape, &error)) {
        return NULL;
    }
    return shape;
}

JPC_Shape* physics_create_shape_from_desc(const PhysicsShapeDesc* desc) {
    if (!desc)
        return NULL;

    switch (desc->type) {
        case SHAPE_BOX:
            return physics_create_box_shape((float*)desc->box.half_extents, desc->density);
        case SHAPE_SPHERE:
            return physics_create_sphere_shape(desc->sphere.radius, desc->density);
        case SHAPE_CAPSULE:
            return physics_create_capsule_shape(desc->capsule.radius, desc->capsule.half_height,
                                                desc->density);
        case SHAPE_CYLINDER:
            return physics_create_cylinder_shape(desc->cylinder.radius, desc->cylinder.half_height,
                                                 desc->density);
        default:
            return NULL;
    }
}

// RigidBody component free function
static void free_rigid_body(void* data) {
    RigidBody* rb = (RigidBody*)data;
    if (!rb)
        return;

    // Remove all constraints involving this body first
    if (rb->world) {
        physics_world_remove_constraints_for_body(rb->world, rb);
    }

    if (rb->is_added && rb->world && rb->world->body_interface) {
        JPC_BodyInterface_RemoveBody(rb->world->body_interface, rb->body_id);
        JPC_BodyInterface_DestroyBody(rb->world->body_interface, rb->body_id);
    }

    if (rb->shape) {
        JPC_Shape_Release(rb->shape);
    }

    free(rb);
}

RigidBody* entity_add_rigid_body(Entity* entity, PhysicsWorld* world,
                                 const PhysicsShapeDesc* shape_desc, PhysicsMotionType motion_type,
                                 PhysicsLayer layer) {
    if (!entity || !world || !shape_desc)
        return NULL;

    if (entity_has_component(entity, COMPONENT_RIGID_BODY)) {
        return NULL;
    }

    JPC_Shape* shape = physics_create_shape_from_desc(shape_desc);
    if (!shape)
        return NULL;
    JPC_Shape_AddRef(shape);

    RigidBody* rb = malloc(sizeof(RigidBody));
    if (!rb) {
        JPC_Shape_Release(shape);
        return NULL;
    }
    memset(rb, 0, sizeof(RigidBody));

    rb->entity = entity;
    rb->world = world;
    rb->shape = shape;
    rb->motion_type = motion_type;
    rb->layer = layer;
    rb->friction = 0.5f;
    rb->restitution = 0.0f;
    rb->linear_damping = 0.05f;
    rb->angular_damping = 0.05f;
    rb->gravity_factor = 1.0f;
    rb->allow_sleep = true;
    rb->is_sensor = false;

    JPC_BodyCreationSettings settings;
    JPC_BodyCreationSettings_default(&settings);

    settings.Position = vec3_to_jpc_r(entity->position);
    settings.Rotation = quat_to_jpc(entity->rotation);
    settings.Shape = shape;
    settings.ObjectLayer = (JPC_ObjectLayer)layer;
    settings.Friction = rb->friction;
    settings.Restitution = rb->restitution;
    settings.LinearDamping = rb->linear_damping;
    settings.AngularDamping = rb->angular_damping;
    settings.GravityFactor = rb->gravity_factor;
    settings.AllowSleeping = rb->allow_sleep;
    settings.IsSensor = rb->is_sensor;

    switch (motion_type) {
        case MOTION_STATIC:
            settings.MotionType = JPC_MOTION_TYPE_STATIC;
            break;
        case MOTION_KINEMATIC:
            settings.MotionType = JPC_MOTION_TYPE_KINEMATIC;
            settings.AllowDynamicOrKinematic = true;
            break;
        case MOTION_DYNAMIC:
            settings.MotionType = JPC_MOTION_TYPE_DYNAMIC;
            settings.AllowDynamicOrKinematic = true;
            break;
    }

    settings.UserData = (uint64_t)(uintptr_t)entity;

    JPC_Body* body = JPC_BodyInterface_CreateBody(world->body_interface, &settings);
    if (!body) {
        JPC_Shape_Release(shape);
        free(rb);
        return NULL;
    }

    rb->body_id = JPC_Body_GetID(body);
    JPC_BodyInterface_AddBody(world->body_interface, rb->body_id, JPC_ACTIVATION_ACTIVATE);
    rb->is_added = true;

    entity_add_component(entity, COMPONENT_RIGID_BODY, rb);
    entity_set_component_free(entity, COMPONENT_RIGID_BODY, free_rigid_body);

    return rb;
}

RigidBody* entity_get_rigid_body(Entity* entity) {
    if (!entity)
        return NULL;
    return (RigidBody*)entity_get_component(entity, COMPONENT_RIGID_BODY);
}

void entity_remove_rigid_body(Entity* entity) {
    if (!entity)
        return;
    entity_remove_component(entity, COMPONENT_RIGID_BODY);
}

// RigidBody physics functions
void rigid_body_add_force(RigidBody* rb, vec3 force) {
    if (!rb || !rb->is_added || rb->motion_type != MOTION_DYNAMIC)
        return;
    JPC_BodyInterface_AddForce(rb->world->body_interface, rb->body_id, vec3_to_jpc(force));
}

void rigid_body_add_force_at_point(RigidBody* rb, vec3 force, vec3 point) {
    if (!rb || !rb->is_added || rb->motion_type != MOTION_DYNAMIC)
        return;
    JPC_BodyInterface_AddForceAtPoint(rb->world->body_interface, rb->body_id, vec3_to_jpc(force),
                                      vec3_to_jpc_r(point));
}

void rigid_body_add_impulse(RigidBody* rb, vec3 impulse) {
    if (!rb || !rb->is_added || rb->motion_type != MOTION_DYNAMIC)
        return;
    JPC_BodyInterface_AddImpulse(rb->world->body_interface, rb->body_id, vec3_to_jpc(impulse));
}

void rigid_body_add_angular_impulse(RigidBody* rb, vec3 impulse) {
    if (!rb || !rb->is_added || rb->motion_type != MOTION_DYNAMIC)
        return;
    JPC_BodyInterface_AddAngularImpulse(rb->world->body_interface, rb->body_id,
                                        vec3_to_jpc(impulse));
}

void rigid_body_add_torque(RigidBody* rb, vec3 torque) {
    if (!rb || !rb->is_added || rb->motion_type != MOTION_DYNAMIC)
        return;
    JPC_BodyInterface_AddTorque(rb->world->body_interface, rb->body_id, vec3_to_jpc(torque));
}

void rigid_body_get_linear_velocity(RigidBody* rb, vec3 out) {
    if (!rb || !rb->is_added) {
        glm_vec3_zero(out);
        return;
    }
    JPC_Vec3 vel = JPC_BodyInterface_GetLinearVelocity(rb->world->body_interface, rb->body_id);
    jpc_to_vec3(vel, out);
}

void rigid_body_set_linear_velocity(RigidBody* rb, vec3 velocity) {
    if (!rb || !rb->is_added)
        return;
    JPC_BodyInterface_SetLinearVelocity(rb->world->body_interface, rb->body_id,
                                        vec3_to_jpc(velocity));
}

void rigid_body_get_angular_velocity(RigidBody* rb, vec3 out) {
    if (!rb || !rb->is_added) {
        glm_vec3_zero(out);
        return;
    }
    JPC_Vec3 vel = JPC_BodyInterface_GetAngularVelocity(rb->world->body_interface, rb->body_id);
    jpc_to_vec3(vel, out);
}

void rigid_body_set_angular_velocity(RigidBody* rb, vec3 velocity) {
    if (!rb || !rb->is_added)
        return;
    JPC_BodyInterface_SetAngularVelocity(rb->world->body_interface, rb->body_id,
                                         vec3_to_jpc(velocity));
}

void rigid_body_set_position(RigidBody* rb, vec3 position) {
    if (!rb || !rb->is_added)
        return;
    JPC_BodyInterface_SetPosition(rb->world->body_interface, rb->body_id, vec3_to_jpc_r(position),
                                  JPC_ACTIVATION_ACTIVATE);
}

void rigid_body_set_rotation(RigidBody* rb, versor rotation) {
    if (!rb || !rb->is_added)
        return;
    JPC_BodyInterface_SetRotation(rb->world->body_interface, rb->body_id, quat_to_jpc(rotation),
                                  JPC_ACTIVATION_ACTIVATE);
}

void rigid_body_move_kinematic(RigidBody* rb, vec3 target_pos, versor target_rot, float dt) {
    if (!rb || !rb->is_added || rb->motion_type != MOTION_KINEMATIC)
        return;
    JPC_BodyInterface_MoveKinematic(rb->world->body_interface, rb->body_id,
                                    vec3_to_jpc_r(target_pos), quat_to_jpc(target_rot), dt);
}

void rigid_body_activate(RigidBody* rb) {
    if (!rb || !rb->is_added)
        return;
    JPC_BodyInterface_ActivateBody(rb->world->body_interface, rb->body_id);
}

void rigid_body_deactivate(RigidBody* rb) {
    if (!rb || !rb->is_added)
        return;
    JPC_BodyInterface_DeactivateBody(rb->world->body_interface, rb->body_id);
}

bool rigid_body_is_active(RigidBody* rb) {
    if (!rb || !rb->is_added)
        return false;
    return JPC_BodyInterface_IsActive(rb->world->body_interface, rb->body_id);
}

// Transform synchronization callback
static void sync_physics_entity_callback(Entity* entity, void* user_data) {
    PhysicsWorld* world = (PhysicsWorld*)user_data;
    RigidBody* rb = entity_get_rigid_body(entity);
    if (!rb || !rb->is_added)
        return;
    // Only sync dynamic bodies - static don't move, kinematic are user-controlled
    if (rb->motion_type != MOTION_DYNAMIC)
        return;

    JPC_RVec3 pos;
    JPC_Quat rot;
    JPC_BodyInterface_GetPositionAndRotation(world->body_interface, rb->body_id, &pos, &rot);

    jpc_r_to_vec3(pos, entity->position);
    jpc_to_quat(rot, entity->rotation);
}

void sync_physics_to_entities(PhysicsWorld* world, EntityManager* em) {
    if (!world || !em)
        return;
    entity_manager_foreach_with(em, COMPONENT_BIT(COMPONENT_RIGID_BODY),
                                sync_physics_entity_callback, world);
}

typedef struct {
    float dt;
} SyncToPhysicsContext;

static void sync_entity_to_physics_callback(Entity* entity, void* user_data) {
    SyncToPhysicsContext* ctx = (SyncToPhysicsContext*)user_data;
    RigidBody* rb = entity_get_rigid_body(entity);
    if (!rb || !rb->is_added)
        return;
    if (rb->motion_type != MOTION_KINEMATIC)
        return;

    // Use MoveKinematic instead of SetPosition for proper collision response
    // This tells Jolt where we want the body to be at the end of the timestep
    JPC_BodyInterface_MoveKinematic(rb->world->body_interface, rb->body_id,
                                    vec3_to_jpc_r(entity->position), quat_to_jpc(entity->rotation),
                                    ctx->dt);
}

void sync_entities_to_physics(EntityManager* em, float dt) {
    if (!em)
        return;
    SyncToPhysicsContext ctx = {.dt = dt};
    entity_manager_foreach_with(em, COMPONENT_BIT(COMPONENT_RIGID_BODY),
                                sync_entity_to_physics_callback, &ctx);
}

// Raycasting implementation

// Helper to get entity from body ID via UserData (requires lock - use outside callbacks)
static Entity* get_entity_from_body_id(PhysicsWorld* world, JPC_BodyID body_id) {
    if (!world || !world->body_interface)
        return NULL;

    uint64_t user_data = JPC_BodyInterface_GetUserData(world->body_interface, body_id);
    return (Entity*)(uintptr_t)user_data;
}

// Helper to get entity from body pointer directly (safe in callbacks - no lock needed)
static Entity* get_entity_from_body(const JPC_Body* body) {
    if (!body)
        return NULL;
    uint64_t user_data = JPC_Body_GetUserData(body);
    return (Entity*)(uintptr_t)user_data;
}

bool physics_world_raycast(PhysicsWorld* world, vec3 origin, vec3 direction, float max_distance,
                           RaycastHit* out_hit) {
    if (!world || !world->initialized || !out_hit)
        return false;

    memset(out_hit, 0, sizeof(RaycastHit));

    // Scale direction by max_distance (JoltC uses direction length as max)
    vec3 scaled_dir;
    glm_vec3_scale(direction, max_distance, scaled_dir);

    JPC_NarrowPhaseQuery_CastRayArgs args = {
        .Ray = {.Origin = vec3_to_jpc_r(origin), .Direction = vec3_to_jpc(scaled_dir)},
        .BroadPhaseLayerFilter = NULL,
        .ObjectLayerFilter = NULL,
        .BodyFilter = NULL,
        .ShapeFilter = NULL};

    const JPC_NarrowPhaseQuery* query =
        JPC_PhysicsSystem_GetNarrowPhaseQuery(world->physics_system);

    bool hit = JPC_NarrowPhaseQuery_CastRay(query, &args);

    if (hit) {
        out_hit->hit = true;
        out_hit->fraction = args.Result.Fraction;
        out_hit->distance = max_distance * args.Result.Fraction;

        // Calculate hit position: origin + direction * distance
        glm_vec3_scale(direction, out_hit->distance, out_hit->position);
        glm_vec3_add(origin, out_hit->position, out_hit->position);

        // Get entity from body UserData
        out_hit->entity = get_entity_from_body_id(world, args.Result.BodyID);
        if (out_hit->entity) {
            out_hit->body = entity_get_rigid_body(out_hit->entity);
        }

        // Normal is zero for now (would need additional query)
        glm_vec3_zero(out_hit->normal);
    }

    return hit;
}

// Filter context for layer-based filtering
typedef struct {
    uint32_t layer_mask;
} LayerFilterContext;

static bool layer_filter_should_collide(const void* self, JPC_ObjectLayer layer) {
    const LayerFilterContext* ctx = (const LayerFilterContext*)self;
    return (ctx->layer_mask & (1u << layer)) != 0;
}

bool physics_world_raycast_filtered(PhysicsWorld* world, vec3 origin, vec3 direction,
                                    float max_distance, uint32_t layer_mask, RaycastHit* out_hit) {
    if (!world || !world->initialized || !out_hit)
        return false;

    memset(out_hit, 0, sizeof(RaycastHit));

    vec3 scaled_dir;
    glm_vec3_scale(direction, max_distance, scaled_dir);

    // Create layer filter
    LayerFilterContext filter_ctx = {.layer_mask = layer_mask};
    JPC_ObjectLayerFilterFns filter_fns = {.ShouldCollide = layer_filter_should_collide};
    JPC_ObjectLayerFilter* layer_filter = JPC_ObjectLayerFilter_new(&filter_ctx, filter_fns);

    JPC_NarrowPhaseQuery_CastRayArgs args = {
        .Ray = {.Origin = vec3_to_jpc_r(origin), .Direction = vec3_to_jpc(scaled_dir)},
        .BroadPhaseLayerFilter = NULL,
        .ObjectLayerFilter = layer_filter,
        .BodyFilter = NULL,
        .ShapeFilter = NULL};

    const JPC_NarrowPhaseQuery* query =
        JPC_PhysicsSystem_GetNarrowPhaseQuery(world->physics_system);

    bool hit = JPC_NarrowPhaseQuery_CastRay(query, &args);

    JPC_ObjectLayerFilter_delete(layer_filter);

    if (hit) {
        out_hit->hit = true;
        out_hit->fraction = args.Result.Fraction;
        out_hit->distance = max_distance * args.Result.Fraction;

        glm_vec3_scale(direction, out_hit->distance, out_hit->position);
        glm_vec3_add(origin, out_hit->position, out_hit->position);

        out_hit->entity = get_entity_from_body_id(world, args.Result.BodyID);
        if (out_hit->entity) {
            out_hit->body = entity_get_rigid_body(out_hit->entity);
        }

        glm_vec3_zero(out_hit->normal);
    }

    return hit;
}

// Filter context for ignoring specific body
typedef struct {
    JPC_BodyID ignore_id;
} BodyIgnoreContext;

static bool body_ignore_should_collide(const void* self, JPC_BodyID body_id) {
    const BodyIgnoreContext* ctx = (const BodyIgnoreContext*)self;
    return body_id != ctx->ignore_id;
}

static bool body_ignore_should_collide_locked(const void* self, const JPC_Body* body) {
    const BodyIgnoreContext* ctx = (const BodyIgnoreContext*)self;
    return JPC_Body_GetID(body) != ctx->ignore_id;
}

bool physics_world_raycast_ignore(PhysicsWorld* world, vec3 origin, vec3 direction,
                                  float max_distance, const RigidBody* ignore,
                                  RaycastHit* out_hit) {
    if (!world || !world->initialized || !out_hit)
        return false;

    // If no body to ignore, use regular raycast
    if (!ignore || !ignore->is_added) {
        return physics_world_raycast(world, origin, direction, max_distance, out_hit);
    }

    memset(out_hit, 0, sizeof(RaycastHit));

    vec3 scaled_dir;
    glm_vec3_scale(direction, max_distance, scaled_dir);

    // Create body filter
    BodyIgnoreContext filter_ctx = {.ignore_id = ignore->body_id};
    JPC_BodyFilterFns filter_fns = {.ShouldCollide = body_ignore_should_collide,
                                    .ShouldCollideLocked = body_ignore_should_collide_locked};
    JPC_BodyFilter* body_filter = JPC_BodyFilter_new(&filter_ctx, filter_fns);

    JPC_NarrowPhaseQuery_CastRayArgs args = {
        .Ray = {.Origin = vec3_to_jpc_r(origin), .Direction = vec3_to_jpc(scaled_dir)},
        .BroadPhaseLayerFilter = NULL,
        .ObjectLayerFilter = NULL,
        .BodyFilter = body_filter,
        .ShapeFilter = NULL};

    const JPC_NarrowPhaseQuery* query =
        JPC_PhysicsSystem_GetNarrowPhaseQuery(world->physics_system);

    bool hit = JPC_NarrowPhaseQuery_CastRay(query, &args);

    JPC_BodyFilter_delete(body_filter);

    if (hit) {
        out_hit->hit = true;
        out_hit->fraction = args.Result.Fraction;
        out_hit->distance = max_distance * args.Result.Fraction;

        glm_vec3_scale(direction, out_hit->distance, out_hit->position);
        glm_vec3_add(origin, out_hit->position, out_hit->position);

        out_hit->entity = get_entity_from_body_id(world, args.Result.BodyID);
        if (out_hit->entity) {
            out_hit->body = entity_get_rigid_body(out_hit->entity);
        }

        glm_vec3_zero(out_hit->normal);
    }

    return hit;
}

// Collision event queue implementation
typedef struct CollisionEventQueue {
    CollisionEvent* events;
    size_t count;
    size_t capacity;
} CollisionEventQueue;

static CollisionEventQueue* create_collision_event_queue(size_t initial_capacity) {
    CollisionEventQueue* queue = malloc(sizeof(CollisionEventQueue));
    if (!queue)
        return NULL;

    queue->events = malloc(sizeof(CollisionEvent) * initial_capacity);
    if (!queue->events) {
        free(queue);
        return NULL;
    }

    queue->count = 0;
    queue->capacity = initial_capacity;
    return queue;
}

static void free_collision_event_queue(CollisionEventQueue* queue) {
    if (!queue)
        return;
    free(queue->events);
    free(queue);
}

static void queue_collision_event(CollisionEventQueue* queue, const CollisionEvent* event) {
    if (!queue || !event)
        return;

    // Grow queue if needed
    if (queue->count >= queue->capacity) {
        size_t new_capacity = queue->capacity * 2;
        CollisionEvent* new_events = realloc(queue->events, sizeof(CollisionEvent) * new_capacity);
        if (!new_events)
            return; // Drop event if allocation fails
        queue->events = new_events;
        queue->capacity = new_capacity;
    }

    queue->events[queue->count++] = *event;
}

// Contact listener callbacks
static JPC_ValidateResult on_contact_validate(void* self, const JPC_Body* body1,
                                              const JPC_Body* body2, JPC_RVec3 base_offset,
                                              const JPC_CollideShapeResult* collision_result) {
    (void)self;
    (void)body1;
    (void)body2;
    (void)base_offset;
    (void)collision_result;
    return JPC_VALIDATE_RESULT_ACCEPT_ALL_CONTACTS;
}

static void on_contact_added(void* self, const JPC_Body* body1, const JPC_Body* body2,
                             const JPC_ContactManifold* manifold, JPC_ContactSettings* settings) {
    (void)settings;
    ContactListenerContext* ctx = (ContactListenerContext*)self;
    if (!ctx || !ctx->world || !ctx->world->event_queue)
        return;

    // Use direct body access (lock-free) instead of BodyInterface during callbacks
    Entity* e1 = get_entity_from_body(body1);
    Entity* e2 = get_entity_from_body(body2);

    CollisionEvent event = {.type = COLLISION_BEGIN,
                            .entity_a = e1,
                            .entity_b = e2,
                            .body_a = e1 ? entity_get_rigid_body(e1) : NULL,
                            .body_b = e2 ? entity_get_rigid_body(e2) : NULL,
                            .penetration_depth = manifold->PenetrationDepth};

    // Copy contact normal
    jpc_to_vec3(manifold->WorldSpaceNormal, event.contact_normal);

    // Calculate average contact point from manifold
    glm_vec3_zero(event.contact_point);
    if (manifold->RelativeContactPointsOn1.length > 0) {
        for (uint32_t i = 0; i < manifold->RelativeContactPointsOn1.length; i++) {
            vec3 pt = {0};
            jpc_to_vec3(manifold->RelativeContactPointsOn1.points[i], pt);
            glm_vec3_add(event.contact_point, pt, event.contact_point);
        }
        glm_vec3_scale(event.contact_point, 1.0f / manifold->RelativeContactPointsOn1.length,
                       event.contact_point);
        // Add base offset to get world space
        vec3 base = {0};
        jpc_r_to_vec3(manifold->BaseOffset, base);
        glm_vec3_add(event.contact_point, base, event.contact_point);
    }

    queue_collision_event(ctx->world->event_queue, &event);
}

static void on_contact_persisted(void* self, const JPC_Body* body1, const JPC_Body* body2,
                                 const JPC_ContactManifold* manifold,
                                 JPC_ContactSettings* settings) {
    (void)settings;
    ContactListenerContext* ctx = (ContactListenerContext*)self;
    if (!ctx || !ctx->world || !ctx->world->event_queue)
        return;
    if (!ctx->world->report_stay_events)
        return;

    // Use direct body access (lock-free) instead of BodyInterface during callbacks
    Entity* e1 = get_entity_from_body(body1);
    Entity* e2 = get_entity_from_body(body2);

    CollisionEvent event = {.type = COLLISION_STAY,
                            .entity_a = e1,
                            .entity_b = e2,
                            .body_a = e1 ? entity_get_rigid_body(e1) : NULL,
                            .body_b = e2 ? entity_get_rigid_body(e2) : NULL,
                            .penetration_depth = manifold->PenetrationDepth};

    jpc_to_vec3(manifold->WorldSpaceNormal, event.contact_normal);

    glm_vec3_zero(event.contact_point);
    if (manifold->RelativeContactPointsOn1.length > 0) {
        for (uint32_t i = 0; i < manifold->RelativeContactPointsOn1.length; i++) {
            vec3 pt = {0};
            jpc_to_vec3(manifold->RelativeContactPointsOn1.points[i], pt);
            glm_vec3_add(event.contact_point, pt, event.contact_point);
        }
        glm_vec3_scale(event.contact_point, 1.0f / manifold->RelativeContactPointsOn1.length,
                       event.contact_point);
        vec3 base = {0};
        jpc_r_to_vec3(manifold->BaseOffset, base);
        glm_vec3_add(event.contact_point, base, event.contact_point);
    }

    queue_collision_event(ctx->world->event_queue, &event);
}

static void on_contact_removed(void* self, const JPC_SubShapeIDPair* pair) {
    ContactListenerContext* ctx = (ContactListenerContext*)self;
    if (!ctx || !ctx->world || !ctx->world->event_queue)
        return;

    Entity* e1 = get_entity_from_body_id(ctx->world, pair->Body1ID);
    Entity* e2 = get_entity_from_body_id(ctx->world, pair->Body2ID);

    CollisionEvent event = {.type = COLLISION_END,
                            .entity_a = e1,
                            .entity_b = e2,
                            .body_a = e1 ? entity_get_rigid_body(e1) : NULL,
                            .body_b = e2 ? entity_get_rigid_body(e2) : NULL,
                            .penetration_depth = 0.0f};

    glm_vec3_zero(event.contact_point);
    glm_vec3_zero(event.contact_normal);

    queue_collision_event(ctx->world->event_queue, &event);
}

// Public collision API
void physics_world_set_collision_callback(PhysicsWorld* world, CollisionCallback callback,
                                          void* user_data) {
    if (!world)
        return;
    world->collision_callback = callback;
    world->collision_user_data = user_data;
}

void physics_world_set_report_stay_events(PhysicsWorld* world, bool enabled) {
    if (!world)
        return;
    world->report_stay_events = enabled;
}

void physics_world_process_collisions(PhysicsWorld* world) {
    if (!world || !world->event_queue || !world->collision_callback)
        return;

    CollisionEventQueue* queue = world->event_queue;
    for (size_t i = 0; i < queue->count; i++) {
        world->collision_callback(&queue->events[i], world->collision_user_data);
    }

    // Clear queue for next frame
    queue->count = 0;
}

// Helper to compute a perpendicular vector
static void compute_perpendicular(const vec3 v, vec3 out) {
    vec3 abs_v = {fabsf(v[0]), fabsf(v[1]), fabsf(v[2])};
    if (abs_v[0] <= abs_v[1] && abs_v[0] <= abs_v[2]) {
        vec3 x = {1, 0, 0};
        glm_vec3_cross((float*)v, x, out);
    } else if (abs_v[1] <= abs_v[2]) {
        vec3 y = {0, 1, 0};
        glm_vec3_cross((float*)v, y, out);
    } else {
        vec3 z = {0, 0, 1};
        glm_vec3_cross((float*)v, z, out);
    }
    glm_vec3_normalize(out);
}

// Static constraint creators
static JPC_Constraint* create_fixed_jolt_constraint(JPC_Body* body1, JPC_Body* body2,
                                                    const ConstraintDesc* desc) {
    JPC_FixedConstraintSettings settings;
    JPC_FixedConstraintSettings_default(&settings);

    settings.Space = JPC_CONSTRAINT_SPACE_LOCAL_TO_BODY_COM;
    settings.Point1 = vec3_to_jpc_r(desc->anchor_a);
    settings.Point2 = vec3_to_jpc_r(desc->anchor_b);

    // Set reference frame axes
    settings.AxisX1 = vec3_to_jpc(desc->fixed.axis_x);
    settings.AxisY1 = vec3_to_jpc(desc->fixed.axis_y);
    settings.AxisX2 = vec3_to_jpc(desc->fixed.axis_x);
    settings.AxisY2 = vec3_to_jpc(desc->fixed.axis_y);

    return JPC_FixedConstraintSettings_Create(&settings, body1, body2);
}

static JPC_Constraint* create_distance_jolt_constraint(JPC_Body* body1, JPC_Body* body2,
                                                       const ConstraintDesc* desc) {
    JPC_DistanceConstraintSettings settings;
    JPC_DistanceConstraintSettings_default(&settings);

    settings.Space = JPC_CONSTRAINT_SPACE_LOCAL_TO_BODY_COM;
    settings.Point1 = vec3_to_jpc_r(desc->anchor_a);
    settings.Point2 = vec3_to_jpc_r(desc->anchor_b);
    settings.MinDistance = desc->distance.min_distance;
    settings.MaxDistance = desc->distance.max_distance;

    // Spring settings
    if (desc->distance.spring.frequency > 0) {
        settings.LimitsSpringSettings.Mode = JPC_SPRING_MODE_FREQUENCY_AND_DAMPING;
        settings.LimitsSpringSettings.FrequencyOrStiffness = desc->distance.spring.frequency;
        settings.LimitsSpringSettings.Damping = desc->distance.spring.damping;
    }

    return (JPC_Constraint*)JPC_DistanceConstraintSettings_Create(&settings, body1, body2);
}

static JPC_Constraint* create_hinge_jolt_constraint(JPC_Body* body1, JPC_Body* body2,
                                                    const ConstraintDesc* desc) {
    JPC_HingeConstraintSettings settings;
    JPC_HingeConstraintSettings_default(&settings);

    settings.Space = JPC_CONSTRAINT_SPACE_LOCAL_TO_BODY_COM;
    settings.Point1 = vec3_to_jpc_r(desc->anchor_a);
    settings.Point2 = vec3_to_jpc_r(desc->anchor_b);
    settings.HingeAxis1 = vec3_to_jpc(desc->hinge.axis);
    settings.HingeAxis2 = vec3_to_jpc(desc->hinge.axis);

    // Compute normal axis perpendicular to hinge
    vec3 normal = {0};
    compute_perpendicular((float*)desc->hinge.axis, normal);
    settings.NormalAxis1 = vec3_to_jpc(normal);
    settings.NormalAxis2 = vec3_to_jpc(normal);

    settings.LimitsMin = desc->hinge.min_angle;
    settings.LimitsMax = desc->hinge.max_angle;
    settings.MaxFrictionTorque = desc->hinge.max_friction_torque;

    return (JPC_Constraint*)JPC_HingeConstraintSettings_Create(&settings, body1, body2);
}

static JPC_Constraint* create_slider_jolt_constraint(JPC_Body* body1, JPC_Body* body2,
                                                     const ConstraintDesc* desc) {
    JPC_SliderConstraintSettings settings;
    JPC_SliderConstraintSettings_default(&settings);

    settings.Space = JPC_CONSTRAINT_SPACE_LOCAL_TO_BODY_COM;
    settings.Point1 = vec3_to_jpc_r(desc->anchor_a);
    settings.Point2 = vec3_to_jpc_r(desc->anchor_b);
    settings.SliderAxis1 = vec3_to_jpc(desc->slider.axis);
    settings.SliderAxis2 = vec3_to_jpc(desc->slider.axis);

    // Compute normal axis perpendicular to slider
    vec3 normal = {0};
    compute_perpendicular((float*)desc->slider.axis, normal);
    settings.NormalAxis1 = vec3_to_jpc(normal);
    settings.NormalAxis2 = vec3_to_jpc(normal);

    settings.LimitsMin = desc->slider.min_distance;
    settings.LimitsMax = desc->slider.max_distance;
    settings.MaxFrictionForce = desc->slider.max_friction_force;

    return (JPC_Constraint*)JPC_SliderConstraintSettings_Create(&settings, body1, body2);
}

static JPC_Constraint* create_sixdof_jolt_constraint(JPC_Body* body1, JPC_Body* body2,
                                                     const ConstraintDesc* desc) {
    JPC_SixDOFConstraintSettings settings;
    JPC_SixDOFConstraintSettings_default(&settings);

    settings.Space = JPC_CONSTRAINT_SPACE_LOCAL_TO_BODY_COM;
    settings.Position1 = vec3_to_jpc_r(desc->anchor_a);
    settings.Position2 = vec3_to_jpc_r(desc->anchor_b);
    settings.AxisX1 = vec3_to_jpc(desc->sixdof.axis_x);
    settings.AxisY1 = vec3_to_jpc(desc->sixdof.axis_y);
    settings.AxisX2 = vec3_to_jpc(desc->sixdof.axis_x);
    settings.AxisY2 = vec3_to_jpc(desc->sixdof.axis_y);

    // Translation limits
    settings.LimitMin[0] = desc->sixdof.translation_limits_min[0];
    settings.LimitMin[1] = desc->sixdof.translation_limits_min[1];
    settings.LimitMin[2] = desc->sixdof.translation_limits_min[2];
    settings.LimitMax[0] = desc->sixdof.translation_limits_max[0];
    settings.LimitMax[1] = desc->sixdof.translation_limits_max[1];
    settings.LimitMax[2] = desc->sixdof.translation_limits_max[2];

    // Rotation limits (indices 3, 4, 5)
    settings.LimitMin[3] = desc->sixdof.rotation_limits_min[0];
    settings.LimitMin[4] = desc->sixdof.rotation_limits_min[1];
    settings.LimitMin[5] = desc->sixdof.rotation_limits_min[2];
    settings.LimitMax[3] = desc->sixdof.rotation_limits_max[0];
    settings.LimitMax[4] = desc->sixdof.rotation_limits_max[1];
    settings.LimitMax[5] = desc->sixdof.rotation_limits_max[2];

    return JPC_SixDOFConstraintSettings_Create(&settings, body1, body2);
}

// Get JPC_Body from body ID (uses write lock to get non-const body for constraint creation)
static JPC_Body* get_body_ptr(PhysicsWorld* world, JPC_BodyID body_id) {
    if (!world || !world->physics_system)
        return NULL;
    const JPC_BodyLockInterface* lock_interface =
        JPC_PhysicsSystem_GetBodyLockInterface(world->physics_system);
    JPC_BodyLockWrite* lock = JPC_BodyLockWrite_new(lock_interface, body_id);
    if (!lock || !JPC_BodyLockWrite_Succeeded(lock)) {
        if (lock)
            JPC_BodyLockWrite_delete(lock);
        return NULL;
    }
    JPC_Body* body = JPC_BodyLockWrite_GetBody(lock);
    JPC_BodyLockWrite_delete(lock);
    return body;
}

Constraint* create_constraint(PhysicsWorld* world, RigidBody* body_a, RigidBody* body_b,
                              const ConstraintDesc* desc) {
    if (!world || !body_a || !body_b || !desc)
        return NULL;
    if (!body_a->is_added || !body_b->is_added)
        return NULL;

    // Get JoltC body pointers
    JPC_Body* jolt_a = get_body_ptr(world, body_a->body_id);
    JPC_Body* jolt_b = get_body_ptr(world, body_b->body_id);
    if (!jolt_a || !jolt_b)
        return NULL;

    // Create JoltC constraint based on type
    JPC_Constraint* jolt_c = NULL;
    switch (desc->type) {
        case CONSTRAINT_FIXED:
            jolt_c = create_fixed_jolt_constraint(jolt_a, jolt_b, desc);
            break;
        case CONSTRAINT_DISTANCE:
            jolt_c = create_distance_jolt_constraint(jolt_a, jolt_b, desc);
            break;
        case CONSTRAINT_HINGE:
            jolt_c = create_hinge_jolt_constraint(jolt_a, jolt_b, desc);
            break;
        case CONSTRAINT_SLIDER:
            jolt_c = create_slider_jolt_constraint(jolt_a, jolt_b, desc);
            break;
        case CONSTRAINT_SIXDOF:
            jolt_c = create_sixdof_jolt_constraint(jolt_a, jolt_b, desc);
            break;
        default:
            return NULL;
    }
    if (!jolt_c)
        return NULL;

    // Allocate wrapper
    Constraint* c = malloc(sizeof(Constraint));
    if (!c) {
        JPC_Constraint_Release(jolt_c);
        return NULL;
    }
    memset(c, 0, sizeof(Constraint));

    c->jolt_constraint = jolt_c;
    c->type = desc->type;
    c->world = world;
    c->body_a = body_a;
    c->body_b = body_b;
    c->enabled = true;
    c->is_added = false;

    return c;
}

void free_constraint(Constraint* c) {
    if (!c)
        return;

    // Remove from world if still added
    if (c->is_added && c->world) {
        physics_world_remove_constraint(c->world, c);
    }

    // Release JoltC constraint
    if (c->jolt_constraint) {
        JPC_Constraint_Release(c->jolt_constraint);
    }

    free(c);
}

void physics_world_add_constraint(PhysicsWorld* world, Constraint* c) {
    if (!world || !c || c->is_added)
        return;

    // Add to Jolt physics system
    JPC_PhysicsSystem_AddConstraint(world->physics_system, c->jolt_constraint);
    c->is_added = true;

    // Store in world's constraint array
    if (world->constraint_count >= world->constraint_capacity) {
        size_t new_cap = world->constraint_capacity == 0 ? 16 : world->constraint_capacity * 2;
        Constraint** new_arr = realloc(world->constraints, sizeof(Constraint*) * new_cap);
        if (!new_arr)
            return;
        world->constraints = new_arr;
        world->constraint_capacity = new_cap;
    }
    world->constraints[world->constraint_count++] = c;
}

void physics_world_remove_constraint(PhysicsWorld* world, Constraint* c) {
    if (!world || !c || !c->is_added)
        return;

    // Remove from Jolt physics system
    JPC_PhysicsSystem_RemoveConstraint(world->physics_system, c->jolt_constraint);
    c->is_added = false;

    // Remove from world's constraint array
    for (size_t i = 0; i < world->constraint_count; i++) {
        if (world->constraints[i] == c) {
            world->constraints[i] = world->constraints[world->constraint_count - 1];
            world->constraint_count--;
            break;
        }
    }
}

void physics_world_remove_constraints_for_body(PhysicsWorld* world, const RigidBody* body) {
    if (!world || !body)
        return;

    // Iterate backwards to safely remove while iterating
    for (size_t i = world->constraint_count; i > 0; i--) {
        Constraint* c = world->constraints[i - 1];
        if (c->body_a == body || c->body_b == body) {
            free_constraint(c);
        }
    }
}

void constraint_set_enabled(Constraint* c, bool enabled) {
    if (!c || !c->jolt_constraint)
        return;
    JPC_Constraint_SetEnabled(c->jolt_constraint, enabled);
    c->enabled = enabled;
}

bool constraint_is_enabled(const Constraint* c) {
    if (!c)
        return false;
    return c->enabled;
}

// Hinge motor control
void constraint_hinge_set_motor_state(Constraint* c, MotorState state) {
    if (!c || c->type != CONSTRAINT_HINGE || !c->jolt_constraint)
        return;
    JPC_MotorState jpc_state;
    switch (state) {
        case MOTOR_OFF:
            jpc_state = JPC_MOTOR_STATE_OFF;
            break;
        case MOTOR_VELOCITY:
            jpc_state = JPC_MOTOR_STATE_VELOCITY;
            break;
        case MOTOR_POSITION:
            jpc_state = JPC_MOTOR_STATE_POSITION;
            break;
        default:
            return;
    }
    JPC_HingeConstraint_SetMotorState((JPC_HingeConstraint*)c->jolt_constraint, jpc_state);
}

void constraint_hinge_set_target_velocity(Constraint* c, float velocity) {
    if (!c || c->type != CONSTRAINT_HINGE || !c->jolt_constraint)
        return;
    JPC_HingeConstraint_SetTargetAngularVelocity((JPC_HingeConstraint*)c->jolt_constraint,
                                                 velocity);
}

void constraint_hinge_set_target_angle(Constraint* c, float angle) {
    if (!c || c->type != CONSTRAINT_HINGE || !c->jolt_constraint)
        return;
    JPC_HingeConstraint_SetTargetAngle((JPC_HingeConstraint*)c->jolt_constraint, angle);
}

float constraint_hinge_get_current_angle(const Constraint* c) {
    if (!c || c->type != CONSTRAINT_HINGE || !c->jolt_constraint)
        return 0.0f;
    return JPC_HingeConstraint_GetTargetAngle((JPC_HingeConstraint*)c->jolt_constraint);
}

// Slider motor control
void constraint_slider_set_motor_state(Constraint* c, MotorState state) {
    if (!c || c->type != CONSTRAINT_SLIDER || !c->jolt_constraint)
        return;
    JPC_MotorState jpc_state;
    switch (state) {
        case MOTOR_OFF:
            jpc_state = JPC_MOTOR_STATE_OFF;
            break;
        case MOTOR_VELOCITY:
            jpc_state = JPC_MOTOR_STATE_VELOCITY;
            break;
        case MOTOR_POSITION:
            jpc_state = JPC_MOTOR_STATE_POSITION;
            break;
        default:
            return;
    }
    JPC_SliderConstraint_SetMotorState((JPC_SliderConstraint*)c->jolt_constraint, jpc_state);
}

void constraint_slider_set_target_velocity(Constraint* c, float velocity) {
    if (!c || c->type != CONSTRAINT_SLIDER || !c->jolt_constraint)
        return;
    JPC_SliderConstraint_SetTargetVelocity((JPC_SliderConstraint*)c->jolt_constraint, velocity);
}

void constraint_slider_set_target_position(Constraint* c, float position) {
    if (!c || c->type != CONSTRAINT_SLIDER || !c->jolt_constraint)
        return;
    JPC_SliderConstraint_SetTargetPosition((JPC_SliderConstraint*)c->jolt_constraint, position);
}

float constraint_slider_get_current_position(const Constraint* c) {
    if (!c || c->type != CONSTRAINT_SLIDER || !c->jolt_constraint)
        return 0.0f;
    return JPC_SliderConstraint_GetTargetPosition((JPC_SliderConstraint*)c->jolt_constraint);
}
