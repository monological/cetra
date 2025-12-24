#include "physics.h"
#include "entity.h"
#include "JoltC/JoltC.h"

#include <stdlib.h>
#include <string.h>

// Type conversion helpers
static inline JPC_Vec3 vec3_to_jpc(vec3 v) {
    JPC_Vec3 result = {.x = v[0], .y = v[1], .z = v[2], ._w = 0.0f};
    return result;
}

static inline void jpc_to_vec3(JPC_Vec3 jv, vec3 out) {
    out[0] = jv.x;
    out[1] = jv.y;
    out[2] = jv.z;
}

static inline JPC_RVec3 vec3_to_jpc_r(vec3 v) {
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

static inline JPC_Quat quat_to_jpc(versor q) {
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
    world->initialized = true;

    return world;
}

void free_physics_world(PhysicsWorld* world) {
    if (!world)
        return;

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

// Helper to get entity from body ID via UserData
static Entity* get_entity_from_body_id(PhysicsWorld* world, JPC_BodyID body_id) {
    if (!world || !world->body_interface)
        return NULL;

    uint64_t user_data = JPC_BodyInterface_GetUserData(world->body_interface, body_id);
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
