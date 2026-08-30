
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "ext/log.h"
#include "scene.h"
#include "particle_system.h"
#include "sky.h"
#include "wind.h"
#include "gi_volume.h"
#include "probe.h"
#include "probe_set.h"
#include "water.h"
#include "postfx.h"
#include "material_texture_array.h"
#include "program.h"
#include "shader.h"
#include "mesh.h"
#include "material.h"
#include "emissive_light.h"
#include "ies.h"
#include "light.h"
#include "camera.h"
#include "common.h"
#include "util.h"

/*
 * prototypes
 */
static void _set_xyz_program_for_nodes(SceneNode* node, ShaderProgram* program);

Scene* create_scene() {
    Scene* scene = malloc(sizeof(Scene));
    if (!scene) {
        log_error("Failed to allocate memory for Scene");
        return NULL;
    }
    memset(scene, 0, sizeof(Scene));

    // Not the memset zero, which for a mat4 is the matrix that collapses the
    // whole scene to a point rather than the one that leaves it alone.
    glm_mat4_identity(scene->root_transform);

    // Initialize the Scene structure
    scene->root_node = NULL;
    scene->lights = NULL;
    scene->light_count = 0;
    scene->particle_systems = NULL;
    scene->particle_system_count = 0;
    scene->cameras = NULL;
    scene->camera_count = 0;

    scene->tex_pool = create_texture_pool();

    scene->xyz_shader_program = NULL;

    scene->transparent_mesh_count = 0;
    scene->transmissive_mesh_count = 0;
    scene->materials_dirty = true;

    // Initialize shadow system
    scene->shadow_system = create_shadow_system(DEFAULT_SHADOW_MAP_SIZE);

    // Initialize IBL (NULL by default, user must load HDR)
    scene->ibl = NULL;
    scene->probe_set = NULL;
    scene->sky = NULL;
    scene->wind = NULL;
    scene->gi_volume = NULL;
    scene->water = NULL;
    glm_vec3_zero(scene->ambient_radiance); // no IBL and no authored ambient = black
    glm_vec3_zero(scene->world_origin);
    glm_vec3_zero(scene->pending_origin);
    scene->on_origin_shift = NULL;
    scene->origin_shift_ctx = NULL;
    scene->origin_shift_distance = 0.0f;
    scene->render_skybox = false;
    scene->skybox_brightness = 1.0f;
    scene->skybox_ground_projection = false;
    scene->skybox_gp_radius = 5.0f;
    scene->skybox_gp_height = 1.5f;
    scene->shadow_catcher = false;
    scene->shadow_catcher_strength = 0.55f;

    // Initialize skeletal animation
    scene->skeletons = NULL;
    scene->skeleton_count = 0;
    scene->animations = NULL;
    scene->animation_count = 0;

    return scene;
}

void free_scene(Scene* scene) {
    if (!scene) {
        return; // Nothing to free
    }

    // Releases the reference add_decal_to_scene took. The pool below holds its
    // own, so either order is safe -- materials release AFTER it for exactly
    // that reason.
    scene_clear_decals(scene);

    // Free texture pool
    if (scene->tex_pool) {
        free_texture_pool(scene->tex_pool);
        scene->tex_pool = NULL;
    }

    // Before the lights, because a panel borrows one and the registry's own
    // teardown must not outlive what it points at.
    emissive_panels_free(scene->emissive_panels);
    scene->emissive_panels = NULL;

    // Also before the lights: a light holds a profile INDEX rather than a
    // pointer (light.h), so the order is not forced -- but keeping the two
    // together is what stops a future pointer being the thing that discovers it.
    free_ies_library(scene->ies_library);
    scene->ies_library = NULL;

    // Free all lights
    if (scene->lights) {
        for (size_t i = 0; i < scene->light_count; i++) {
            if (scene->lights[i]) {
                free_light(scene->lights[i]);
            }
        }
        free(scene->lights);
        scene->lights = NULL;
    }

    // Free all particle systems (the scene owns them; nodes only borrow)
    if (scene->particle_systems) {
        for (size_t i = 0; i < scene->particle_system_count; i++) {
            if (scene->particle_systems[i]) {
                free_particle_system(scene->particle_systems[i]);
            }
        }
        free(scene->particle_systems);
        scene->particle_systems = NULL;
    }

    // Free all cameras
    if (scene->cameras) {
        for (size_t i = 0; i < scene->camera_count; i++) {
            if (scene->cameras[i]) {
                free_camera(scene->cameras[i]);
            }
        }
        free(scene->cameras);
        scene->cameras = NULL;
    }

    if (scene->materials) {
        for (size_t i = 0; i < scene->material_count; ++i) {
            if (scene->materials[i]) {
                free_material(scene->materials[i]);
            }
        }
        free(scene->materials);
    }

    // Free the root node and its subtree
    if (scene->root_node) {
        free_node(scene->root_node);
    }

    draw_list_free(&scene->draw_list);

    // Free shadow system
    if (scene->shadow_system) {
        free_shadow_system(scene->shadow_system);
        scene->shadow_system = NULL;
    }

    // Free IBL resources
    if (scene->ibl) {
        free_ibl_resources(scene->ibl);
        scene->ibl = NULL;
    }

    // Free reflection probes
    if (scene->probe_set) {
        free_reflection_probe_set(scene->probe_set);
        scene->probe_set = NULL;
    }

    // Free procedural sky
    if (scene->sky) {
        free_sky_atmosphere(scene->sky);
        scene->sky = NULL;
    }

    // Free the scene wind
    if (scene->wind) {
        free_wind(scene->wind);
        scene->wind = NULL;
    }

    // Free the GI probe volume
    if (scene->gi_volume) {
        free_gi_volume(scene->gi_volume);
        scene->gi_volume = NULL;
    }

    // Free the water surface
    if (scene->water) {
        free_water(scene->water);
        scene->water = NULL;
    }

    // Free the material mask texture array
    if (scene->material_textures) {
        free_material_texture_array(scene->material_textures);
        scene->material_textures = NULL;
    }

    // Free all skeletons
    if (scene->skeletons) {
        for (size_t i = 0; i < scene->skeleton_count; i++) {
            if (scene->skeletons[i]) {
                free_skeleton(scene->skeletons[i]);
            }
        }
        free(scene->skeletons);
        scene->skeletons = NULL;
    }

    // Free all animations
    if (scene->animations) {
        for (size_t i = 0; i < scene->animation_count; i++) {
            if (scene->animations[i]) {
                free_animation(scene->animations[i]);
            }
        }
        free(scene->animations);
        scene->animations = NULL;
    }

    // Finally, free the scene itself
    free(scene);

    return;
}

void set_scene_root_node(Scene* scene, SceneNode* root_node) {
    if (!scene)
        return;
    scene->root_node = root_node;
    scene->materials_dirty = true;
}

void scene_mark_materials_dirty(Scene* scene) {
    if (scene)
        scene->materials_dirty = true;
}

/*
 * Cameras
 */

void set_scene_cameras(Scene* scene, Camera** cameras, size_t camera_count) {
    if (!scene)
        return;
    scene->cameras = cameras;
    scene->camera_count = camera_count;
}

void set_scene_lights(Scene* scene, Light** lights, size_t light_count) {
    if (!scene)
        return;
    scene->lights = lights;
    scene->light_count = light_count;
}

int add_camera_to_scene(Scene* scene, Camera* camera) {
    if (!scene || !camera)
        return -1;

    size_t new_count = scene->camera_count + 1;
    Camera** new_cameras = realloc(scene->cameras, new_count * sizeof(Camera*));
    if (!new_cameras) {
        log_error("Failed to reallocate memory for new camera");
        return -1;
    }

    scene->cameras = new_cameras;
    scene->cameras[scene->camera_count] = camera;
    scene->camera_count = new_count;
    return 0;
}

Camera* find_camera_by_name(Scene* scene, const char* name) {
    if (!scene || !name)
        return NULL;

    for (size_t i = 0; i < scene->camera_count; ++i) {
        Camera* cam = scene->cameras[i];
        if (cam && cam->name && strcmp(cam->name, name) == 0) {
            return cam;
        }
    }
    return NULL;
}

/*
 * Lights
 */
int add_light_to_scene(Scene* scene, Light* light) {
    if (!scene || !light)
        return -1;

    size_t new_count = scene->light_count + 1;
    Light** new_lights = realloc(scene->lights, new_count * sizeof(Light*));
    if (!new_lights) {
        log_error("Failed to reallocate memory for new light");
        return -1;
    }

    scene->lights = new_lights;
    scene->lights[scene->light_count] = light;
    scene->light_count = new_count;
    return 0;
}

int remove_light_from_scene(Scene* scene, Light* light) {
    if (!scene || !light)
        return -1;

    for (size_t i = 0; i < scene->light_count; ++i) {
        if (scene->lights[i] != light)
            continue;
        // Shift rather than swap: see the ordering note in scene.h.
        memmove(&scene->lights[i], &scene->lights[i + 1],
                (scene->light_count - i - 1) * sizeof(Light*));
        scene->light_count--;
        free_light(light);
        return 0;
    }
    return -1;
}

const Light* scene_key_directional(const Scene* scene, const float* surface_normal) {
    if (!scene)
        return NULL;
    const Light* best = NULL;
    float best_weight = 0.0f;
    for (size_t i = 0; i < scene->light_count; i++) {
        const Light* l = scene->lights[i];
        if (!l || l->type != LIGHT_DIRECTIONAL)
            continue;
        float weight = light_effective_intensity(l);
        if (surface_normal) {
            // Lights store the direction they SHINE; the cosine is against the
            // direction toward the source.
            vec3 toward;
            glm_vec3_negate_to((float*)l->direction, toward);
            glm_vec3_normalize(toward);
            weight *= fmaxf(glm_vec3_dot(toward, (float*)surface_normal), 0.0f);
        }
        if (weight > best_weight) {
            best_weight = weight;
            best = l;
        }
    }
    return best;
}

Light* find_light_by_name(Scene* scene, const char* name) {
    if (!scene || !name)
        return NULL;

    for (size_t i = 0; i < scene->light_count; ++i) {
        Light* light = scene->lights[i];
        if (light && light->name && strcmp(light->name, name) == 0) {
            return light;
        }
    }
    return NULL;
}

int add_particle_system_to_scene(Scene* scene, struct ParticleSystem* sys) {
    if (!scene || !sys)
        return -1;

    size_t new_count = scene->particle_system_count + 1;
    struct ParticleSystem** new_particle_systems =
        realloc(scene->particle_systems, new_count * sizeof(struct ParticleSystem*));
    if (!new_particle_systems) {
        log_error("Failed to reallocate memory for new particle system");
        return -1;
    }

    scene->particle_systems = new_particle_systems;
    scene->particle_systems[scene->particle_system_count] = sys;
    scene->particle_system_count = new_count;
    return 0;
}

void scene_update_particle_systems(Scene* scene, float dt, float t) {
    if (!scene)
        return;
    for (size_t i = 0; i < scene->particle_system_count; i++)
        particle_system_update(scene->particle_systems[i], dt, t);
}


int add_material_to_scene(Scene* scene, Material* material) {
    if (!scene || !material)
        return -1;

    for (size_t i = 0; i < scene->material_count; ++i) {
        if (scene->materials[i] == material)
            return 0;
    }

    size_t new_count = scene->material_count + 1;
    Material** new_materials = realloc(scene->materials, new_count * sizeof(Material*));
    if (!new_materials) {
        log_error("Failed to allocate memory for new material");
        return -1;
    }

    scene->materials = new_materials;
    scene->materials[scene->material_count] = material;
    scene->material_count = new_count;
    scene->material_textures_dirty = true; // a new material's textures must be (re)packed
    return 0;
}

static void _register_node_materials(Scene* scene, SceneNode* node) {
    if (!node)
        return;
    for (size_t i = 0; i < node->mesh_count; i++) {
        if (node->meshes[i] && node->meshes[i]->material)
            add_material_to_scene(scene, node->meshes[i]->material);
    }
    for (size_t i = 0; i < node->children_count; i++)
        _register_node_materials(scene, node->children[i]);
}

// Register every material reachable from the graph, so the registry describes
// what the scene actually draws rather than only what an importer put there.
//
// Until this existed, `add_material_to_scene` was called from exactly one place
// -- the Assimp import path -- so a scene that built its materials in code had
// an EMPTY registry no matter how many meshes it drew. Four things read that
// registry and all four were silently wrong for such a scene: subsurface
// detection (so `apps/tree` authored subsurface on leaves and grass and never
// once ran the SSS pass), material-texture-array packing, lookup by name, and
// free_scene -- which meant those materials were also leaked, since nothing else
// in the tree calls free_material.
//
// Gated on the graph having changed, because it is otherwise a reconciliation
// loop whose only job is to discover nothing happened: the walk is recursive and
// add_material_to_scene linear-scans per mesh, so an unguarded per-frame call is
// O(meshes x materials) forever to maintain state that moves only when the graph
// does.
void scene_sync_materials(Scene* scene) {
    if (!scene || !scene->materials_dirty)
        return;
    _register_node_materials(scene, scene->root_node);
    scene->materials_dirty = false;
}

void set_scene_wind(Scene* scene, struct Wind* wind) {
    if (!scene)
        return;
    if (scene->wind && scene->wind != wind)
        free_wind(scene->wind);
    scene->wind = wind;
}

// The publish below indexes postfx's arrays by the SCENE's count, so the two capacities
// cannot diverge; the parser must not be able to author more than the scene can hold.
_Static_assert(SCENE_MAX_FOG_VOLUMES <= POSTFX_MAX_FOG_VOLUMES,
               "postfx fog-volume mirror must be at least the scene slot count");

void scene_set_origin_callback(Scene* scene, void (*on_shift)(const vec3 delta, void* ctx),
                               void* ctx) {
    if (!scene)
        return;
    scene->on_origin_shift = on_shift;
    scene->origin_shift_ctx = ctx;
}

void scene_set_world_origin(Scene* scene, const vec3 new_origin) {
    if (!scene)
        return;
    glm_vec3_copy((float*)new_origin, scene->pending_origin);
}

// Only the ROOT's own children carry an absolute: original_transform is local, so
// the walk below them already describes the same world. The caches do not -- both
// are absolutes, and prev_global_transform has to take the SAME delta as
// global_transform or the difference between them, which is what the whole motion
// pipeline reads, reports the shift as a screen-wide velocity for one frame.
static void shift_node_tree(SceneNode* node, const vec3 delta) {
    if (!node)
        return;
    glm_vec3_sub(node->global_transform[3], (float*)delta, node->global_transform[3]);
    glm_vec3_sub(node->prev_global_transform[3], (float*)delta, node->prev_global_transform[3]);
    for (size_t i = 0; i < node->children_count; ++i)
        shift_node_tree(node->children[i], delta);
}

/*
 * Every absolute the scene owns, each moved by the subsystem that owns it.
 *
 * The delegation is the point rather than tidiness. A hand-written list in one
 * file gives whoever adds a world-anchored field to probe.h or gi_volume.h no
 * reason to ever open this one; a `*_shift_origin` beside the field they just
 * added is in the file they are already editing. This is the same inversion the
 * `*_publish_to_postfx` functions already use, and for the same reason.
 *
 * Two whole categories are deliberately absent, and knowing why is what keeps the
 * list from growing back:
 *
 *  - Anything DERIVED per frame. Lights recompute global_position from their node
 *    on every walk; emissive panels are re-placed from current transforms. The
 *    graph shift below carries them.
 *  - Anything the GPU addresses per texel from a world position -- a layered
 *    material's splat rectangle, its tiling lattice. Those stay in AUTHORING
 *    space forever and the shader reconstructs (include/world_origin.glsl). A
 *    material shifted here would also have to be chased through every scene that
 *    shares it, and a mesh added after a shift would arrive carrying an
 *    unshifted one.
 */
void scene_apply_origin_delta(Scene* scene, const vec3 delta) {
    if (!scene)
        return;

    // Only the ROOT's own children carry an absolute local; the walk below them
    // already describes the same world. Start the cache recursion at the children
    // too: the root's own global is recomputed from its local by the next walk,
    // so shifting it here would be undone while its prev_ kept the shift -- one
    // frame of delta-sized velocity on anything parented directly to the root.
    if (scene->root_node) {
        for (size_t i = 0; i < scene->root_node->children_count; ++i) {
            SceneNode* child = scene->root_node->children[i];
            if (!child)
                continue;
            glm_vec3_sub(child->original_transform[3], (float*)delta,
                         child->original_transform[3]);
            shift_node_tree(child, delta);
        }
    }

    for (int i = 0; i < scene->fog_volume_count; ++i)
        glm_vec3_sub(scene->fog_volumes[i].center, (float*)delta, scene->fog_volumes[i].center);

    shadow_system_shift_origin(scene->shadow_system, delta);
    probe_set_shift_origin(scene->probe_set, delta);
    gi_volume_shift_origin(scene->gi_volume, delta);
    for (size_t i = 0; i < scene->particle_system_count; ++i)
        particle_system_shift_origin(scene->particle_systems[i], delta);

    glm_vec3_add(scene->world_origin, (float*)delta, scene->world_origin);
}

int add_fog_volume_to_scene(Scene* scene, const FogVolume* volume) {
    if (!scene || !volume)
        return -1;
    if (scene->fog_volume_count >= SCENE_MAX_FOG_VOLUMES) {
        log_warn("scene: more than %d fog volumes; extra ignored", SCENE_MAX_FOG_VOLUMES);
        return -1;
    }
    scene->fog_volumes[scene->fog_volume_count++] = *volume;
    return 0;
}

int add_decal_to_scene(Scene* scene, const Decal* decal) {
    if (!scene || !decal)
        return -1;
    if (scene->decal_count >= DECAL_MAX) {
        log_warn("scene: more than %d decals; extra ignored", DECAL_MAX);
        return -1;
    }
    Decal* slot = &scene->decals[scene->decal_count++];
    *slot = *decal;
    texture_retain(slot->albedo_tex);
    texture_retain(slot->surface_tex);
    return 0;
}

void scene_clear_decals(Scene* scene) {
    if (!scene)
        return;
    for (int i = 0; i < scene->decal_count; i++) {
        texture_release(scene->decals[i].albedo_tex);
        texture_release(scene->decals[i].surface_tex);
        scene->decals[i].albedo_tex = NULL;
        scene->decals[i].surface_tex = NULL;
    }
    scene->decal_count = 0;
}

void scene_environment_changed(Scene* scene, struct Engine* engine) {
    if (!scene || !engine || !scene->sky)
        return;
    SkyAtmosphere* sky = scene->sky;

    // Clouds existed this session -> the env may need the deck added or purged.
    // The per-drag path never pays this march, which is why the chain is a
    // release/apply step rather than something the sliders call per frame.
    if (sky->clouds.noise_baked)
        sky_bake_ex(sky, scene->ibl, engine, sky->clouds.enabled);

    /*
     * Per probe, and the refusal is per probe too: an environment-only probe
     * re-prefilters for the price of a cube walk, a scene-captured one would
     * cost six scene renders and is left as shot.
     *
     * A SET is refused wholesale and separately, because the question is not per
     * probe there: its members have had their capture cubes released into the
     * atlas, so none of them could re-prefilter even if it wanted to, and
     * re-running the sweep is what relight will be.
     */
    const ReflectionProbeSet* probes = scene->probe_set;
    if (probe_set_multi(probes)) {
        log_info("Sky: %d-probe set not refreshed (relight is deferred)", probes->count);
    } else {
        for (int i = 0; probes && i < probes->count; ++i) {
            if (probes->probes[i]->cubemap == 0)
                reflection_probe_capture(probes->probes[i], engine, scene, 0.1f, 100.0f, true);
            else
                log_info("Sky: scene-captured probe %d not refreshed", i);
        }
    }

    // Re-armed rather than swept here: a sweep is one scene render per probe
    // face, and unlike the probe it does not stall -- it spreads over the
    // following frames at `rate` with the old atlas sampleable throughout.
    gi_volume_mark_dirty(scene->gi_volume);
}

void scene_publish_fog_volumes_to_postfx(const Scene* scene, struct PostFX* fx) {
    if (!fx)
        return;
    const int count = scene ? scene->fog_volume_count : 0;
    int live = 0;
    for (int i = 0; i < count && live < POSTFX_MAX_FOG_VOLUMES; i++) {
        const FogVolume* v = &scene->fog_volumes[i];
        // Dropped, not published as a zero: the count is what ARMS the froxel pass, so a
        // volume that cannot darken anything would otherwise buy three RGBA16F volumes, the
        // ESM cascades and 128 slice draws to render a byte-identical frame.
        if (v->density <= 0.0f)
            continue;
        // Packed here rather than in the uploader: the shader's shape is a set of vec4s, and
        // splitting the pack from the upload would leave two places that have to agree on
        // which component holds what. The feather is clamped here for the same reason -- it
        // is an invariant of the packing, and re-establishing it per froxel cell made ~1M
        // cells a frame pay for a decision that belongs to one volume.
        glm_vec3_copy((float*)v->center, fx->local_fog_center_density[live]);
        fx->local_fog_center_density[live][3] = v->density;
        glm_vec3_copy((float*)v->half_extent, fx->local_fog_extent_feather[live]);
        fx->local_fog_extent_feather[live][3] = fmaxf(v->feather, 1e-4f);
        glm_vec3_copy((float*)v->tint, fx->local_fog_tint[live]);
        live++;
    }
    fx->local_fog_count = live;
}

int add_skeleton_to_scene(Scene* scene, Skeleton* skeleton) {
    if (!scene || !skeleton)
        return -1;

    // Check if already added
    for (size_t i = 0; i < scene->skeleton_count; i++) {
        if (scene->skeletons[i] == skeleton)
            return 0;
    }

    size_t new_count = scene->skeleton_count + 1;
    Skeleton** new_skeletons = realloc(scene->skeletons, new_count * sizeof(Skeleton*));
    if (!new_skeletons) {
        log_error("Failed to allocate memory for new skeleton");
        return -1;
    }

    scene->skeletons = new_skeletons;
    scene->skeletons[scene->skeleton_count] = skeleton;
    scene->skeleton_count = new_count;
    return 0;
}

Skeleton* find_skeleton_by_name(Scene* scene, const char* name) {
    if (!scene || !name)
        return NULL;

    for (size_t i = 0; i < scene->skeleton_count; i++) {
        if (scene->skeletons[i] && scene->skeletons[i]->name &&
            strcmp(scene->skeletons[i]->name, name) == 0) {
            return scene->skeletons[i];
        }
    }
    return NULL;
}

int add_animation_to_scene(Scene* scene, Animation* animation) {
    if (!scene || !animation)
        return -1;

    // Check if already added
    for (size_t i = 0; i < scene->animation_count; i++) {
        if (scene->animations[i] == animation)
            return 0;
    }

    size_t new_count = scene->animation_count + 1;
    Animation** new_animations = realloc(scene->animations, new_count * sizeof(Animation*));
    if (!new_animations) {
        log_error("Failed to allocate memory for new animation");
        return -1;
    }

    scene->animations = new_animations;
    scene->animations[scene->animation_count] = animation;
    scene->animation_count = new_count;
    return 0;
}

Animation* find_animation_by_name(Scene* scene, const char* name) {
    if (!scene || !name)
        return NULL;

    for (size_t i = 0; i < scene->animation_count; i++) {
        if (scene->animations[i] && scene->animations[i]->name &&
            strcmp(scene->animations[i]->name, name) == 0) {
            return scene->animations[i];
        }
    }
    return NULL;
}

GLboolean set_scene_xyz_shader_program(Scene* scene, ShaderProgram* xyz_shader_program) {
    if (!scene || !xyz_shader_program) {
        return GL_FALSE;
    }
    scene->xyz_shader_program = xyz_shader_program;
    _set_xyz_program_for_nodes(scene->root_node, scene->xyz_shader_program);
    return GL_TRUE;
}

/*
 * Scene Node
 *
 */

SceneNode* create_node() {
    SceneNode* node = malloc(sizeof(SceneNode));
    if (!node) {
        log_error("Failed to allocate memory for scene node");
        return NULL;
    }

    node->name = NULL;
    node->parent = NULL;
    node->children = NULL;
    node->children_count = 0;
    node->children_cap = 0;
    glm_mat4_identity(node->original_transform);
    glm_mat4_identity(node->global_transform);
    glm_mat4_identity(node->prev_global_transform);
    node->prev_valid = false;
    glm_mat3_identity(node->normal_matrix);

    node->meshes = NULL;
    node->mesh_count = 0;
    node->light = NULL;
    node->camera = NULL;
    node->particle_system = NULL;

    // xyz
    node->show_xyz = true;
    glGenVertexArrays(1, &node->xyz_vao);
    glGenBuffers(1, &node->xyz_vbo);
    node->xyz_shader_program = NULL;

    return node;
}

// The subtree, WITHOUT unlinking. Split out only so the recursion does not go
// through the unlink: a child is about to have its whole array freed, so
// detaching it from a parent that is one frame from gone costs a memmove per
// node and can be quadratic on a wide group.
static void free_subtree(SceneNode* node) {
    if (!node)
        return;

    for (size_t i = 0; i < node->children_count; i++) {
        free_subtree(node->children[i]);
    }

    free(node->children);

    for (size_t i = 0; i < node->mesh_count; i++) {
        if (node->meshes[i]) {
            free_mesh(node->meshes[i]);
        }
    }
    free(node->meshes);

    if (node->name) {
        free(node->name);
    }

    // Note: Do not free camera, light, or particle_system -- they are borrowed;
    // the Scene owns them. Lifetime invariant: never free a node whose particle
    // system is still registered on the Scene, or the system's sys->node back-ref
    // dangles (free_scene frees systems before the root node, so teardown is safe).
    // Shaders and programs are usually shared and managed separately.

    free(node);
}

void free_node(SceneNode* node) {
    if (!node)
        return;
    scene_graph_touched();
    // UNLINKED first, so a caller may free any node and not only a root. It was
    // the caller's job until a quadtree started detaching and re-attaching
    // patches every frame -- at which point "free_node does not unlink" is a
    // dangling pointer in the parent's array rather than a documented contract,
    // and the one place that can honour it is this one.
    if (node->parent)
        remove_child_node(node->parent, node);
    free_subtree(node);
}

int add_child_node(SceneNode* node, SceneNode* child) {
    scene_graph_touched();
    if (!node || !child)
        return -1;

    // Detached from wherever it was, so a node cannot be in two children arrays
    // at once -- which is a double free, since each array frees what it holds.
    // Re-parenting used to be a load-time-only operation; it is now per frame.
    if (child->parent && child->parent != node)
        remove_child_node(child->parent, child);

    // Doubling rather than growing by one, because a quadtree re-parents its whole
    // selection when the camera crosses a band. import.c fills the array outright
    // and sets cap = count, so this really does arrive at an array that is
    // already exactly full.
    if (!grow_array((void**)&node->children, &node->children_cap, node->children_count + 1,
                    sizeof(SceneNode*), 4))
        return -1;

    node->children[node->children_count] = child;
    child->parent = node;
    node->children_count++;
    return 0;
}

int remove_child_node(SceneNode* node, SceneNode* child) {
    if (!node || !child)
        return -1;
    for (size_t i = 0; i < node->children_count; i++) {
        if (node->children[i] != child)
            continue;
        // Shift rather than swap with the last: sibling order is what the draw
        // list walks, and a scene that authored its nodes in an order gets to
        // keep it. The capacity is untouched, so a set that churns every frame
        // reuses one allocation.
        memmove(&node->children[i], &node->children[i + 1],
                (node->children_count - i - 1) * sizeof(SceneNode*));
        node->children_count--;
        child->parent = NULL;
        scene_graph_touched();
        return 0;
    }
    return -1;
}

int add_mesh_to_node(SceneNode* node, Mesh* mesh) {
    // Also covers apps that then write node->mesh_count directly: the epoch has
    // already moved by the time the next build asks.
    scene_graph_touched();
    if (!node || !mesh)
        return -1;

    size_t new_count = node->mesh_count + 1;
    Mesh** new_meshes = realloc(node->meshes, new_count * sizeof(Mesh*));
    if (!new_meshes) {
        log_error("Failed to reallocate memory for new mesh");
        return -1;
    }

    node->meshes = new_meshes;
    node->meshes[node->mesh_count] = mesh;
    node->mesh_count = new_count;
    return 0;
}

void set_node_name(SceneNode* node, const char* name) {
    if (!node || !name)
        return;
    node->name = safe_strdup(name);
}

void set_node_light(SceneNode* node, Light* light) {
    if (!node)
        return;
    node->light = light;
}

void set_node_camera(SceneNode* node, Camera* camera) {
    if (!node)
        return;
    node->camera = camera;
}

void set_node_particle_system(SceneNode* node, struct ParticleSystem* sys) {
    if (!node)
        return;
    node->particle_system = sys;
    if (sys)
        sys->node = node; // back-ref: the system's world transform is this node's
}

SceneNode* find_node_by_name(SceneNode* root, const char* name) {
    if (!root || !name)
        return NULL;

    if (root->name && strcmp(root->name, name) == 0)
        return root;

    for (size_t i = 0; i < root->children_count; i++) {
        SceneNode* found = find_node_by_name(root->children[i], name);
        if (found)
            return found;
    }

    return NULL;
}

void set_shader_program_for_nodes(SceneNode* node, ShaderProgram* program) {
    if (!node) {
        return;
    }

    for (size_t i = 0; i < node->mesh_count; ++i) {
        Mesh* mesh = node->meshes[i];

        if (mesh && mesh->material) {
            mesh->material->shader_program = program;
        }
    }

    for (size_t i = 0; i < node->children_count; ++i) {
        set_shader_program_for_nodes(node->children[i], program);
    }
}

void set_shader_programs_for_nodes(SceneNode* node, ShaderProgram* standard,
                                   ShaderProgram* skinned) {
    if (!node) {
        return;
    }

    for (size_t i = 0; i < node->mesh_count; ++i) {
        Mesh* mesh = node->meshes[i];

        if (mesh && mesh->material) {
            // Use skinned shader for meshes with bone data, standard otherwise
            if (mesh->is_skinned && skinned) {
                mesh->material->shader_program = skinned;
            } else {
                mesh->material->shader_program = standard;
            }
        }
    }

    for (size_t i = 0; i < node->children_count; ++i) {
        set_shader_programs_for_nodes(node->children[i], standard, skinned);
    }
}

static void _set_xyz_program_for_nodes(SceneNode* node, ShaderProgram* program) {
    if (!node) {
        return;
    }

    node->xyz_shader_program = program;

    for (size_t i = 0; i < node->children_count; ++i) {
        _set_xyz_program_for_nodes(node->children[i], program);
    }
}

void set_show_xyz_for_nodes(SceneNode* node, bool show_xyz) {
    if (!node)
        return;

    node->show_xyz = show_xyz;

    for (size_t i = 0; i < node->children_count; ++i) {
        set_show_xyz_for_nodes(node->children[i], show_xyz);
    }
}

static void _upload_xyz_buffers_to_gpu_for_node(SceneNode* node) {
    // Bind the Vertex Array Object (VAO)
    glBindVertexArray(node->xyz_vao);

    // Bind and set up the Vertex Buffer Object (VBO)
    glBindBuffer(GL_ARRAY_BUFFER, node->xyz_vbo);
    glBufferData(GL_ARRAY_BUFFER, xyz_vertices_size, xyz_vertices, GL_STATIC_DRAW);

    // Position attribute
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Color attribute
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Clear any stale GL errors before validation
    while (glGetError() != GL_NO_ERROR) {
    }

    // only validate if VAO is bound
    if (node->xyz_shader_program && !validate_program(node->xyz_shader_program)) {
        log_error("xyz shader program validation failed");
    }

    glBindVertexArray(0);
}

void upload_buffers_to_gpu_for_nodes(SceneNode* node) {
    if (!node)
        return;

    /*
     * Setup and upload mesh buffers.
     */
    for (size_t i = 0; i < node->mesh_count; i++) {
        if (node->meshes[i]) {
            upload_mesh_buffers_to_gpu(node->meshes[i]);
        }
    }

    _upload_xyz_buffers_to_gpu_for_node(node);

    for (size_t i = 0; i < node->children_count; i++) {
        upload_buffers_to_gpu_for_nodes(node->children[i]);
    }
}

typedef struct {
    SceneNode* node;
    mat4 parent_transform;
} TransformStackEntry;

// Rotate an authored light axis into world space (w=0: rotation only, no
// translation). A degenerate axis leaves the destination untouched rather than
// writing a NaN.
static void _rotate_light_axis(mat4 global_transform, const vec3 local, vec3 out) {
    vec3 rotated;
    glm_mat4_mulv3(global_transform, (float*)local, 0.0f, rotated);
    if (glm_vec3_norm(rotated) > 1e-6f) {
        glm_vec3_normalize(rotated);
        glm_vec3_copy(rotated, out);
    }
}

// The traversal itself. Static since 11.96: both load-time callers went through
// scene_propagate_transforms once the walk stopped needing an unstamped variant,
// so "where does the root sit" has exactly one answer.
static void apply_transform_to_nodes(SceneNode* root, mat4 transform) {
    if (!root)
        return;

    // Iterative traversal using explicit stack
    size_t stack_capacity = 64;
    size_t stack_size = 0;
    TransformStackEntry* stack = malloc(stack_capacity * sizeof(TransformStackEntry));
    if (!stack) {
        log_error("Failed to allocate transform stack");
        return;
    }

    // Push root node
    stack[stack_size].node = root;
    glm_mat4_copy(transform, stack[stack_size].parent_transform);
    stack_size++;

    while (stack_size > 0) {
        // Pop from stack
        stack_size--;
        SceneNode* node = stack[stack_size].node;
        mat4 parent_transform;
        glm_mat4_copy(stack[stack_size].parent_transform, parent_transform);

        glm_mat4_mul(parent_transform, node->original_transform, node->global_transform);

        // Only for nodes that actually moved. scene_latch_prev_transforms left
        // prev_global_transform holding the previous frame's value, so this
        // comparison IS "did this node move", and it is bit-exact rather than a
        // tolerance.
        // Worth the compare because bones are SceneNodes: a rigged model is
        // mostly nodes whose transform is recomputed and mostly unchanged, and
        // an unconditional inverse there costs more than the draw path it saves.
        //
        // On a node's FIRST visit the comparison is against the identity
        // create_node left, so a static node gets its one normal matrix here.
        // That is why the prev_valid seed below has to come after this and not
        // before it: seeding first makes every first visit compare equal, and a
        // node that never moves again never gets a normal matrix at all.
        //
        // Re-running this whole function is harmless. The second pass recomputes
        // the same global from the same inputs, compares it against the same
        // prev, and finds prev_valid already set -- which is what makes the walk
        // safe to call again after a late graph change (spec 11.96).
        if (memcmp(node->global_transform, node->prev_global_transform, sizeof(mat4)) != 0) {
            mat4 inv_global;
            glm_mat4_inv(node->global_transform, inv_global);
            glm_mat4_pick3t(inv_global, node->normal_matrix);
        }

        // A node reached for the first time has no previous frame, and the
        // identity it was created with is not one -- it would report the object
        // as having arrived from the world origin. Standing still is the truth.
        if (!node->prev_valid) {
            glm_mat4_copy(node->global_transform, node->prev_global_transform);
            node->prev_valid = true;
        }

        // Update light position and direction if present
        if (node->light) {
            vec3 light_position;
            glm_mat4_mulv3(node->global_transform, node->light->original_position, 1.0f,
                           light_position);
            glm_vec3_copy(light_position, node->light->global_position);
            // Aim axes: the authored local direction so imported directional/
            // spot lights point where the file says rather than along their
            // import-local axis, and the area-panel height axis so a rotated
            // node spins the panel with it (spec 9.2).
            _rotate_light_axis(node->global_transform, node->light->original_direction,
                               node->light->direction);
            _rotate_light_axis(node->global_transform, node->light->original_up,
                               node->light->up);
        }

        // Push children (in reverse order to maintain left-to-right traversal)
        for (size_t i = node->children_count; i > 0; i--) {
            if (node->children[i - 1]) {
                // Grow stack if needed
                if (stack_size >= stack_capacity) {
                    stack_capacity *= 2;
                    TransformStackEntry* new_stack =
                        realloc(stack, stack_capacity * sizeof(TransformStackEntry));
                    if (!new_stack) {
                        log_error("Failed to grow transform stack");
                        free(stack);
                        return;
                    }
                    stack = new_stack;
                }

                stack[stack_size].node = node->children[i - 1];
                glm_mat4_copy(node->global_transform, stack[stack_size].parent_transform);
                stack_size++;
            }
        }
    }

    free(stack);
}

static void _latch_prev_transform(SceneNode* node) {
    if (!node)
        return;
    glm_mat4_copy(node->global_transform, node->prev_global_transform);
    for (size_t i = 0; i < node->children_count; i++)
        _latch_prev_transform(node->children[i]);
}

void scene_latch_prev_transforms(Scene* scene) {
    if (!scene || !scene->root_node)
        return;
    _latch_prev_transform(scene->root_node);
}

void scene_propagate_transforms(Scene* scene) {
    if (!scene || !scene->root_node)
        return;

    // Seeded from the scene's own root transform, which used to arrive as a
    // matrix each app composed and handed in -- making "where is the scene" a
    // thing seven callers could answer differently, and five of them answered
    // it with a freshly built identity every frame.
    apply_transform_to_nodes(scene->root_node, scene->root_transform);
}

static void _transform_probe_node(const SceneNode* node, int frame, int* named, int* moved) {
    if (!node)
        return;

    if (node->name) {
        // The SAME bit-exact compare the walk itself uses to decide whether a
        // node's normal matrix needs rebuilding. Not a tolerance: the question
        // is whether the two matrices are the same object, and a node that
        // genuinely did not move answers exactly.
        bool has_moved = memcmp(node->global_transform, node->prev_global_transform,
                                sizeof(mat4)) != 0;
        vec3 step;
        glm_vec3_sub((float*)node->global_transform[3], (float*)node->prev_global_transform[3],
                     step);
        printf("transform-probe node frame=%d moved=%d valid=%d step=%.6f pos=%.6f,%.6f,%.6f "
               "name=%s\n",
               frame, has_moved ? 1 : 0, node->prev_valid ? 1 : 0, glm_vec3_norm(step),
               node->global_transform[3][0], node->global_transform[3][1],
               node->global_transform[3][2], node->name);
        *named += 1;
        if (has_moved)
            *moved += 1;
    }

    for (size_t i = 0; i < node->children_count; i++)
        _transform_probe_node(node->children[i], frame, named, moved);
}

// Whether this frame's walk left each named node a previous pose DISTINCT from
// its current one (spec 11.96).
//
// It exists because the failure it watches for is invisible to every other
// instrument in this repository. Walking the graph twice in one frame sets
// prev := global on the second pass, so `moved` reads 0 for everything, every
// motion vector goes to zero, and TAA stops reprojecting -- while the corpus
// stays green, because all 29 goldens are static scenes under a static camera
// where prev == global is already the correct answer.
//
// `name` LAST, per texture_pool_probe's rule: a k=v reader splits on
// whitespace, so a name with a space in it can only corrupt itself.
void scene_transform_probe(const Scene* scene, int frame) {
    if (!scene || !scene->root_node) {
        printf("transform-probe none frame=%d\n", frame);
        return;
    }

    int named = 0, moved = 0;
    _transform_probe_node(scene->root_node, frame, &named, &moved);
    printf("transform-probe total frame=%d named=%d moved=%d\n", frame, named, moved);
}

void print_scene_node(const SceneNode* node, int depth) {
    if (!node)
        return;

    print_indentation(depth);
    printf("Node: %s | Children: %zu | Meshes: %zu | Light: %s | Camera: %s\n",
           node->name ? node->name : "Unnamed", node->children_count, node->mesh_count,
           node->light ? (node->light->name ? node->light->name : "Unnamed Light") : "None",
           node->camera ? (node->camera->name ? node->camera->name : "Unnamed Camera") : "None");

    for (size_t i = 0; i < node->children_count; i++) {
        print_scene_node(node->children[i], depth + 1);
    }
}

void print_scene_lights(const Scene* scene) {
    if (!scene)
        return;
    for (size_t i = 0; i < scene->light_count; i++) {
        print_light(scene->lights[i]);
    }
}

void print_scene(const Scene* scene) {
    if (!scene)
        return;

    printf("Scene | Lights: %zu | Cameras: %zu | Textures: '%s'\n", scene->light_count,
           scene->camera_count,
           scene->tex_pool ? (scene->tex_pool->directory ? scene->tex_pool->directory : "None")
                           : "None");

    print_scene_lights(scene);
    print_scene_node(scene->root_node, 0);
}


// The scene's world bound, accumulated over every mesh's eight transformed
// corners.
//
// The corners are enumerated rather than run through aabb_transform, and that is
// deliberate: aabb_transform is the Arvo centre/extents bound, a conservative
// SUPERSET of the true hull under rotation, so adopting it here would enlarge
// the scene box and move the GI volume fit, the reflection probe's proxy, the
// fog anchor and the camera framing with it. Same reason the two are not merged.
//
// It accumulates through aabb_add_point, which folds via cglm's glm_min rather
// than fminf. They differ on NaN -- fminf returns the non-NaN operand, glm_min
// returns the second -- so a NaN corner now poisons the box instead of being
// quietly dropped. Nothing in the corpus produces one, and a scene box built
// from a NaN transform is better loud than plausible.
static void _compute_node_bounds(SceneNode* node, AABB* bounds) {
    if (!node)
        return;

    for (size_t i = 0; i < node->mesh_count; i++) {
        Mesh* mesh = node->meshes[i];
        if (!mesh || mesh->vertex_count == 0)
            continue;

        // Transform mesh AABB corners by node's global transform
        vec3 corners[8];
        AABB* aabb = &mesh->aabb;

        corners[0][0] = aabb->min[0];
        corners[0][1] = aabb->min[1];
        corners[0][2] = aabb->min[2];
        corners[1][0] = aabb->max[0];
        corners[1][1] = aabb->min[1];
        corners[1][2] = aabb->min[2];
        corners[2][0] = aabb->min[0];
        corners[2][1] = aabb->max[1];
        corners[2][2] = aabb->min[2];
        corners[3][0] = aabb->max[0];
        corners[3][1] = aabb->max[1];
        corners[3][2] = aabb->min[2];
        corners[4][0] = aabb->min[0];
        corners[4][1] = aabb->min[1];
        corners[4][2] = aabb->max[2];
        corners[5][0] = aabb->max[0];
        corners[5][1] = aabb->min[1];
        corners[5][2] = aabb->max[2];
        corners[6][0] = aabb->min[0];
        corners[6][1] = aabb->max[1];
        corners[6][2] = aabb->max[2];
        corners[7][0] = aabb->max[0];
        corners[7][1] = aabb->max[1];
        corners[7][2] = aabb->max[2];

        for (int c = 0; c < 8; c++) {
            vec4 corner4 = {corners[c][0], corners[c][1], corners[c][2], 1.0f};
            vec4 world_corner;
            glm_mat4_mulv(node->global_transform, corner4, world_corner);
            aabb_add_point(bounds, world_corner);
        }
    }

    for (size_t i = 0; i < node->children_count; i++) {
        _compute_node_bounds(node->children[i], bounds);
    }
}

void compute_scene_bounds(Scene* scene, vec3 out_min, vec3 out_max) {
    if (!scene || !scene->root_node) {
        glm_vec3_zero(out_min);
        glm_vec3_zero(out_max);
        return;
    }

    // The empty sentinel replaces the `bool* initialized` out-param this used to
    // thread through the walk -- the same flag-beside-a-box idiom 11.53's review
    // deleted from Mesh, and the last one in the tree.
    AABB bounds;
    aabb_empty(&bounds);
    _compute_node_bounds(scene->root_node, &bounds);

    if (aabb_is_empty(&bounds)) {
        glm_vec3_zero(out_min);
        glm_vec3_zero(out_max);
        return;
    }
    glm_vec3_copy(bounds.min, out_min);
    glm_vec3_copy(bounds.max, out_max);
}

void compute_scene_center_and_radius(Scene* scene, vec3 out_center, float* out_radius) {
    vec3 scene_min = {0}, scene_max = {0};
    compute_scene_bounds(scene, scene_min, scene_max);

    // Center is midpoint of min and max
    glm_vec3_add(scene_min, scene_max, out_center);
    glm_vec3_scale(out_center, 0.5f, out_center);

    // Radius is half the diagonal
    vec3 diagonal;
    glm_vec3_sub(scene_max, scene_min, diagonal);
    *out_radius = glm_vec3_norm(diagonal) * 0.5f;
}
