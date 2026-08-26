
#ifndef _SCENE_H_
#define _SCENE_H_

#include <GLFW/glfw3.h>
#include <cglm/cglm.h>
#include <GL/glew.h>

#include "common.h"
#include "mesh.h"
#include "program.h"
#include "shader.h"
#include "light.h"
#include "camera.h"
#include "shadow.h"
#include "ibl.h"
#include "probe_set.h"
#include "animation.h"
#include "draw_list.h"
#include "decal.h"

// Forward-declared so scene.h and particle_system.h never include each other
// (particle_system.h forward-declares SceneNode in turn) -- avoids a cycle.
struct ParticleSystem;
// Directional wind field (wind.h); a scene-owned environmental object like sky.
struct Wind;
struct PostFX;
struct EmissivePanels;

/*
 * SceneNode
 */
typedef struct SceneNode {
    char* name;

    struct SceneNode* parent;
    struct SceneNode** children;

    size_t children_count;
    // Allocated slots in `children`. Doubling rather than one realloc per add,
    // because a quadtree re-parents its whole selection when the camera crosses a
    // band and a linear grow makes that quadratic.
    size_t children_cap;
    // transpose(inverse(global_transform)) upper 3x3 -- what normals need under
    // non-uniform scale. Computed where global_transform is, because the draw
    // path wants it once per node and would otherwise invert a mat4 per mesh
    // per pass for a value that changed once.
    mat3 normal_matrix;
    mat4 original_transform;
    mat4 global_transform;
    mat4 prev_global_transform; // Last frame's global_transform, for motion vectors
    // Whether prev_global_transform describes a frame this node was actually
    // drawn in. False until the first transform walk reaches it, and what that
    // buys is a node created mid-run: its prev is the identity, so a motion
    // vector taken from it reports the object as having travelled from the world
    // origin this frame. One node doing that is a smear; a region's worth of
    // them arriving at once is the whole frame.
    bool prev_valid;

    Mesh** meshes;
    size_t mesh_count;

    Light* light;

    Camera* camera;

    struct ParticleSystem* particle_system; // borrowed; owned by the Scene

    bool show_xyz;
    GLuint xyz_vao;
    GLuint xyz_vbo;
    ShaderProgram* xyz_shader_program;
} SceneNode;

// malloc
SceneNode* create_node();
// Releases the node and its whole subtree, unlinking it from its parent first --
// so any node may be freed, not only a root.
void free_node(SceneNode* node);

// build graph. Detaches `child` from any previous parent first, so a node is
// never in two children arrays -- which would be a double free, each array
// freeing what it holds.
int add_child_node(SceneNode* node, SceneNode* child);

// Detach `child` without freeing it, leaving the caller owning it. Returns -1 if
// `child` is not a child of `node`.
//
// The only way out of the array, so the order of the remaining children stays
// what the caller built: sibling order is what the draw list walks.
int remove_child_node(SceneNode* node, SceneNode* child);

// meshes
int add_mesh_to_node(SceneNode* node, Mesh* mesh);

// setters
void set_node_name(SceneNode* node, const char* name);
void set_node_light(SceneNode* node, Light* light);
void set_node_camera(SceneNode* node, Camera* camera);
// Attach a particle system (borrowed) whose world transform is this node's.
void set_node_particle_system(SceneNode* node, struct ParticleSystem* sys);

// find
SceneNode* find_node_by_name(SceneNode* root, const char* name);

// xyz
void set_show_xyz_for_nodes(SceneNode* node, bool show_xyz);

// shaders
void set_shader_program_for_nodes(SceneNode* node, ShaderProgram* program);
void set_shader_programs_for_nodes(SceneNode* node, ShaderProgram* standard,
                                   ShaderProgram* skinned);

// move
void apply_transform_to_nodes(SceneNode* node, mat4 transform);

/*
 * A box of denser air, folded into the froxel fog volume (spec 11.39).
 *
 * World-space and axis-aligned, so it is NOT a SceneNode: like water, it is a medium the
 * Scene owns and describes in world units, not a transformable citizen. A rotated or
 * parented volume is the thing this shape cannot express, and is deferred rather than
 * approximated -- an oriented box costs the shader a matrix per volume, and nothing has
 * asked for one yet.
 *
 * tint colours the SHARED lighting rather than adding radiance of its own: the box sits
 * in the same sun, sky and clustered lights as the air around it. So a volume can be
 * smoke, dust or mist, and cannot be a glow.
 */
#define SCENE_MAX_FOG_VOLUMES 8

typedef struct FogVolume {
    vec3 center;
    vec3 half_extent;
    float density; // extinction ADDED to the global medium inside the box (1/world units)
    float feather; // world units the density ramps in over, inward from every face
    vec3 tint;     // scattering colour; white leaves the surrounding air's colour alone
} FogVolume;

/*
 * Scene
 */

typedef struct Scene {
    SceneNode* root_node;

    Light** lights;
    size_t light_count;

    struct ParticleSystem** particle_systems; // owned (freed in free_scene)
    size_t particle_system_count;

    Camera** cameras;
    size_t camera_count;

    Material** materials;
    size_t material_count;
    bool materials_dirty; // graph may hold materials the registry has not seen

    // Derived emissive area panels (spec 11.49), owned. Opaque: the registry and
    // the local fit it holds are emissive_light.c's business, and putting the fit
    // type here would drag a lighting concept into every translation unit that
    // wants a Scene.
    //
    // NULL until the feature derives its first panel, so a scene that never asks
    // costs one branch and no allocation.
    struct EmissivePanels* emissive_panels;

    TexturePool* tex_pool;

    // IES photometric profiles a light may name (spec 11.57), cached by resolved
    // path for the texture pool's reason: two lights naming one luminaire are one
    // profile. NULL until a scene loads one, so a scene that names none costs a
    // branch and no allocation.
    struct IesLibrary* ies_library;

    // used by all nodes
    ShaderProgram* xyz_shader_program;
    ShaderProgram* outlines_shader_program;

    // The graph flattened for drawing, rebuilt once a frame. Every pass reads
    // it; nothing walks the graph to draw any more.
    //
    // Keyed on the frame index alone, and that is sufficient because the list
    // is STRUCTURAL: it holds which meshes exist and which pass draws each, not
    // where they are. Moving a node does not invalidate it -- the transform is
    // read through the node at submit. Only adding or removing geometry, or
    // changing a material's alpha mode, does, and both are seen at the next
    // frame's rebuild.
    DrawList draw_list;
    size_t transparent_mesh_count;  // Late-pass meshes seen in this frame's opaque pass
    size_t transmissive_mesh_count; // Subset with transmission > 0; gates the mid-frame
                                    // opaque-color resolve refraction samples from
    size_t oit_mesh_count;          // Subset that is ALPHA_BLEND && !transmissive; gates the
                                    // weighted-blended OIT accumulate sub-pass

    // Shadow mapping
    ShadowSystem* shadow_system;

    // Image-Based Lighting
    IBLResources* ibl;
    ReflectionProbeSet* probe_set; // local reflection probes (optional; owned)
    struct SkyAtmosphere* sky;     // procedural sky feeding ibl (optional)
    struct Wind* wind;             // dominant directional wind (optional; owned)
    struct GIVolume* gi_volume;    // indirect-diffuse probe grid (optional; owned)
    struct Water* water;           // ocean/lake surface (optional; owned)

    // Boxes of denser air, folded into the froxel volume (spec 11.39). Count 0 = none.
    FogVolume fog_volumes[SCENE_MAX_FOG_VOLUMES];
    int fog_volume_count;

    // Marks projected onto the surfaces inside them (spec 11.73). Count 0 = none,
    // which is the whole off state: nothing allocates and the shader's loop is
    // never entered.
    Decal decals[DECAL_MAX];
    int decal_count;

    // The scene's unique per-texel material images -- masks and layer maps --
    // packed into one GL_TEXTURE_2D_ARRAY (built lazily once the source textures
    // have loaded; see material_texture_array.h). dirty triggers a (re)build in
    // the render loop when the async loader is idle.
    struct MaterialTextureArray* material_textures;
    bool material_textures_dirty;
    // POM (§4.11): height maps are resolved by filename convention once the async
    // texture loader drains (so the albedo/normal paths are populated); set after
    // the one-time resolve so the render loop does not re-scan every frame.
    bool heights_resolved;

    // Uniform ambient radiance in cd/m^2, used only when no IBL is loaded. A
    // real emitter with a real unit, so it scales with nothing and tracks
    // nothing; zero (the default) means an unlit surface is black. Replaced a
    // hardcoded 3%-of-white floor that could not be expressed correctly in
    // either space -- see spec 10.1 phase 5.
    vec3 ambient_radiance;

    /*
     * Large-world origin shifting (spec 11.62).
     *
     * world_origin is the world point that STORAGE (0,0,0) stands for, and it is
     * the accumulated shift since the world was authored -- zero until something
     * moves it, so a scene that never asks is byte-identical to one from before
     * this existed. Storage plus this is the authored position, which is what
     * anything treating a coordinate as an IDENTITY rather than a location has to
     * be handed back: a hash is discontinuous, so a shifted object re-phases
     * completely rather than slightly (measured at 43% of the frame, from twelve
     * units out).
     *
     * The setter SCHEDULES and the engine applies at the frame top. A shift run
     * from wherever an app happened to call would land after some passes had read
     * positions and before others, which is the one state every rule here forbids.
     */
    vec3 world_origin;
    // The origin scheduled for the next frame top. Equal to world_origin means
    // nothing is pending, which is why there is no separate flag: the setter
    // maintains exactly that invariant, so a boolean beside it would be a second
    // statement of one fact.
    vec3 pending_origin;
    // Called after a shift has been applied, with the delta that was subtracted.
    // The honest admission that "hold nothing in world space" is not a contract
    // this engine can impose on an app: physics bodies, cached scatter positions
    // and a player's own idea of where it is are all outside the graph, and the
    // engine cannot enumerate them.
    void (*on_origin_shift)(const vec3 delta, void* ctx);
    void* origin_shift_ctx;
    // How far the camera may drift from the storage origin before the engine
    // re-centres on it, in world units. 0 (the default) never shifts, so this is
    // opt-in and every existing app is unaffected.
    //
    // A THRESHOLD rather than a target, and the two differ by more than wording:
    // shifting whenever the camera has moved at all would shift every frame, and
    // a shift is a full walk of the graph plus a physics rebuild plus a GI
    // re-arm. What sets the floor is that cost, not precision -- precision
    // degrades smoothly and forgives a late shift.
    float origin_shift_distance;

    bool render_skybox;
    float skybox_brightness;       // Linear env multiplier (tone mapping is the post pass's job)
    bool skybox_ground_projection; // Project env onto finite dome + ground
    float skybox_gp_radius;        // Dome radius in world units (meters)
    float skybox_gp_height;        // HDR capture height above ground
    bool shadow_catcher;           // Ground plane receiving shadows over the skybox
    float shadow_catcher_strength; // Shadow darkness 0..1

    // Skeletal Animation
    Skeleton** skeletons;
    size_t skeleton_count;
    Animation** animations;
    size_t animation_count;
} Scene;

// malloc
Scene* create_scene();
void free_scene(Scene* scene);

// root
void set_scene_root_node(Scene* scene, SceneNode* root_node);

// camera
void set_scene_cameras(Scene* scene, Camera** cameras, size_t camera_count);
int add_camera_to_scene(Scene* scene, Camera* camera);
Camera* find_camera_by_name(Scene* scene, const char* name);

// light
void set_scene_lights(Scene* scene, Light** lights, size_t light_count);
int add_light_to_scene(Scene* scene, Light* light);

// Unlink and FREE. The Scene owns its lights, so an unlink-only form would leak
// by default.
//
// Order-preserving, and that is load-bearing rather than tidy: light_cluster.c
// walks this array in order and documents that order as what makes the packing --
// and so the shading loop order -- deterministic. A swap-with-last would reorder
// the list on any removal and move pixels for no reason a reader could find.
//
// OWNERSHIP HAZARD, in the same shape as the particle system's: a SceneNode
// BORROWS its light and does not free it, so removing one a node still points at
// leaves node->light dangling. The caller owns that invariant. Nothing here walks
// the graph to clear it, which would be O(nodes) per removal against a caller
// this codebase does not have -- the one caller that removes anything, the
// emissive reconcile, only ever removes lights it created, and those have no node.
int remove_light_from_scene(Scene* scene, Light* light);

Light* find_light_by_name(Scene* scene, const char* name);

/*
 * The directional DELIVERING most light here, ranked by light_effective_intensity.
 * NULL where none of them delivers anything, which is a real state and not an error --
 * an overcast night has no key.
 *
 * `surface_normal` orients the question and is the only thing consumers disagree about.
 * Pass one to rank by what reaches a surface facing that way, which brings in the cosine:
 * a grazing light can be bright and deliver almost nothing, so on a horizontal plane a
 * 20-lux light at 1 degree loses to a 10-lux sun at 35. Pass NULL for a volume or a
 * billboard, which has no preferred facing and wants radiance alone.
 *
 * Here rather than in a consumer because "which directional actually matters" is a
 * question about the SCENE, and the answers were drifting: this replaces a by-name pick in
 * water and a first-in-array scan in the particle renderer, and postfx's contact-shadow
 * march still keys off scene order among casters. A light BELOW the horizon still exists
 * with its intensity faded to zero, so any rule that does not look at radiance picks it
 * and never reaches the moon standing beside it.
 *
 * Strict > keeps the earliest in scene order on a tie, so the pick is stable.
 */
const Light* scene_key_directional(const Scene* scene, const float* surface_normal);

// particle systems (scene-owned; ticked + rendered automatically by the engine)
int add_particle_system_to_scene(Scene* scene, struct ParticleSystem* sys);
// Advance every particle system's sim. Call from a fixed-timestep update (run_game
// does; an engine_run host may call it too). Rendering is automatic in
// render_current_scene.
void scene_update_particle_systems(Scene* scene, float dt, float t);

// material
int add_material_to_scene(Scene* scene, Material* material);
// Register every material reachable from the graph, if it is marked dirty.
// Idempotent and free when clean, so the engine calls it every frame.
void scene_sync_materials(Scene* scene);
// Mark the graph as having gained materials the registry has not seen.
//
// Needed because a SceneNode has no way back to its Scene, so add_mesh_to_node
// cannot mark this itself. Creating a scene and setting its root both mark it,
// which covers building a graph before the first frame -- an app that attaches
// meshes carrying NEW materials mid-run has to say so.
void scene_mark_materials_dirty(Scene* scene);

// wind (scene-owned; freed in free_scene). Replaces any existing wind.
void set_scene_wind(Scene* scene, struct Wind* wind);

// Ask for the world to be re-centred so `new_origin` becomes storage (0,0,0).
//
// Takes effect at the top of the next frame, not here -- see the field comment.
// Idempotent: asking for the origin the scene already has cancels the request
// rather than scheduling a zero shift, so a caller may say this every frame.
void scene_set_world_origin(Scene* scene, const vec3 new_origin);

// Subtract `delta` from every absolute position the SCENE owns, and advance
// world_origin. The engine calls this from its frame top as half of a shift; the
// other half is the engine's own (the camera, the previous view-projection, and
// PostFX's captured probe), which is why this is not the public entry point.
void scene_apply_origin_delta(Scene* scene, const vec3 delta);

// Register what to run after a shift, for the absolutes the engine cannot reach.
void scene_set_origin_callback(Scene* scene, void (*on_shift)(const vec3 delta, void* ctx),
                               void* ctx);

/*
 * The one "the environment changed" chain: re-bake the env cube WITH the cloud
 * deck when there is one, refresh probes that only mirror the sky, and re-arm
 * the GI sweep.
 *
 * Here rather than in sky.c because the chain is a SCENE fact: sky_bake_ex takes
 * an IBLResources and an Engine and deliberately does not know what a Scene is,
 * while this touches ibl, probe_set and gi_volume. Callers are the GUI's sun and
 * cloud controls on release, and a restored config snapshot. Everything that
 * re-derives from the sun goes through here, or the three consumers drift --
 * which is what happened while this was a file static in gui.c.
 */
void scene_environment_changed(Scene* scene, struct Engine* engine);

// fog volumes. 0 on success, -1 when the array is full, as every add_*_to_scene above.
int add_fog_volume_to_scene(Scene* scene, const FogVolume* volume);

// decals. Same contract; the caller owns resolving the textures and orienting
// the frame, so this bounds-checks, copies, and RETAINS the two images.
//
// Retaining here rather than at the caller is what makes the pair unforgettable:
// the reference is taken exactly where the Decal enters the Scene, and released
// exactly where it leaves, so a refused decal cannot orphan one and a second
// producer cannot arrive without the release. Every other consumer of a pooled
// texture retains it (material.c's sixteen setters); a decal holding a raw
// pointer was the one deviation, and a deviation nothing states is
// indistinguishable from an oversight.
int add_decal_to_scene(Scene* scene, const Decal* decal);
// Release every decal's images and empty the array. The counterpart above, and
// the only correct way to drop decals -- zeroing decal_count leaks the retains.
void scene_clear_decals(Scene* scene);
// Copy this frame's volumes onto PostFX. Mirrors water_publish_to_postfx: the medium
// hands PostFX flat POD and PostFX never learns that a Scene exists.
void scene_publish_fog_volumes_to_postfx(const Scene* scene, struct PostFX* fx);

// skeleton
int add_skeleton_to_scene(Scene* scene, Skeleton* skeleton);
Skeleton* find_skeleton_by_name(Scene* scene, const char* name);

// animation
int add_animation_to_scene(Scene* scene, Animation* animation);
Animation* find_animation_by_name(Scene* scene, const char* name);

// viz
GLboolean set_scene_xyz_shader_program(Scene* scene, ShaderProgram* xyz_shader_program);
GLboolean set_scene_outlines_shader_program(Scene* scene, ShaderProgram* outlines_shader_program);

// print
void print_scene_node(const SceneNode* node, int depth);
void print_scene(const Scene* scene);

// bounds
void compute_scene_bounds(Scene* scene, vec3 out_min, vec3 out_max);
void compute_scene_center_and_radius(Scene* scene, vec3 out_center, float* out_radius);

// render
void upload_buffers_to_gpu_for_nodes(SceneNode* node);

#endif // _SCENE_H_
