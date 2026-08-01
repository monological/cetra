#ifndef _SHADOW_H_
#define _SHADOW_H_

#include <GL/glew.h>
#include <cglm/cglm.h>
#include <stdbool.h>
#include <stddef.h>

#include "program.h"

#define MAX_SHADOW_LIGHTS       3
#define SHADOW_CASCADES         3 // Compile-time cascade ceiling (runtime: cascade_count)
#define DEFAULT_SHADOW_MAP_SIZE 2048
// Worst-case punctual layers per frame, NOT a VRAM budget: every layer is
// re-rendered each frame, so this caps scene traversals. Quantized by the 6 a
// point light needs, hence a value that buys one point light plus two of
// anything else. Erring small is deliberate -- exhausting the pool is a
// failure a log line can name, where an over-large pool costs frame time with
// no signal at all. Allocation is demand-driven, so a spot-only scene builds
// one layer.
#define MAX_PUNCTUAL_SHADOW_LAYERS 8
// Punctual map size bounds and the VRAM the array is allowed to spend.
//
// One GL_TEXTURE_2D_ARRAY carries one size for every layer, and a second array
// is not available -- the punctual array sits on texture unit 15, the last one,
// and pbr_frag already samples all 16. So the size cannot be chosen per light;
// it is chosen once per allocation, as the largest that fits the budget for the
// layers actually needed.
//
// That is the only lever left on texel density. A panel's frustum cannot be
// fitted to its scene: the caster bounds contain the light, so the cone the
// casters demand runs to 180 degrees (spec 10.4, Phase 1). Resolution works
// wherever the light sits.
//
// The asymmetry the budget produces is the useful part. A lone area panel needs
// one layer and gets the maximum; a point light needs six faces and drops to a
// size that keeps six of them affordable. Erring toward the smaller size is
// deliberate: a shadow that is coarser than it could be is a look problem, and
// half a gigabyte of depth array is a crash.
#define PUNCTUAL_SHADOW_MIN_SIZE 1024
#define PUNCTUAL_SHADOW_MAX_SIZE 4096
// DEPTH_COMPONENT24 is one 4-byte texel, so this is layers * size^2 * 4.
#define PUNCTUAL_SHADOW_VRAM_BUDGET (96u * 1024u * 1024u)
// Depth-pass polygon offset, applied to every shadow map (cascade and
// punctual share one near-side storage policy, shadow.c).
// glPolygonOffset(factor, units) pushes a fragment by
// factor * <max depth slope of the polygon> + units * <smallest resolvable
// depth difference>.
//
// The slope term is load-bearing for grazing receivers: it covers the depth
// difference between a fragment's shading point and its texel's own sample
// point, which grows with the surface's slope in the map. Dropping it to 0
// (with the receiver-plane bias carrying the offset taps) was measured and
// rejected: cornell_leak's near-edge-on wall answered with a 0.39 shadow term
// on unoccluded ground -- the screen-space plane gradient degrades exactly
// where the receiver is screen-grazing, and the raster's own slope measure
// does not. The displacement the term applies to a curved CASTER's silhouette
// facets was also measured on dir_shadow_fixture: no visible-umbra artifact
// at factor 2. Both terms in the map's own units, so no per-scene retune.
#define SHADOW_DEPTH_SLOPE_BIAS    2.0f
#define SHADOW_DEPTH_CONSTANT_BIAS 2.0f
// Engine-owned sampler units sit just above the material units (common.h).
// Packing the scalar masks into one array freed the 10-12 range, letting the
// shadow + IBL units drop below 16 so brdfLUT/skybox are no longer bound
// out of spec (GL_MAX_TEXTURE_IMAGE_UNITS = 16, valid 0-15).
#define SHADOW_MAP_TEXTURE_UNIT 10
// The punctual shadow array sits above the cascade array + IBL units (11-14);
// 15 is the last valid unit (GL_MAX_TEXTURE_IMAGE_UNITS = 16).
#define PUNCTUAL_SHADOW_MAP_TEXTURE_UNIT 15

// Forward declarations
struct Scene;
struct Engine;

typedef struct ShadowSystem {
    GLuint cascade_fbo; // Re-attached to each layer of shadow_map_array in turn
    // Depth bias in 0..1 map depth, shared by every caster: it is tuned against
    // the scene-fit map and cascadeParams.w renormalizes it per cascade. It was
    // once per-caster, which only meant the last caster's value won.
    float shadow_bias;
    // Shadow-casting DIRECTIONAL lights this frame, and nothing else -- it is
    // also the shader's numShadowLights and the loop bound over the cascade
    // array. It was called active_count, which reads as "casters of any kind"
    // and was twice mistaken for one: a caller gated the whole shadow bind on
    // it, silently dropping the spot map in scenes with no directional light.
    // Other light types keep their own counts.
    size_t directional_count;
    int default_map_size;
    ShaderProgram* depth_program;
    float ortho_size;
    float near_plane;
    float far_plane;
    GLuint shadow_map_array;
    bool initialized;
    bool enabled;           // Master switch: off skips the depth pass and all receives
    bool pcss_enabled;      // Contact-hardening penumbra (off = fixed 3x3 PCF)
    float pcss_softness;    // Multiplier on each light's emitter size
    int cascade_count;      // Cascades per caster (1..SHADOW_CASCADES). 1 = the classic
                            // scene-fit single map, byte-identical to the pre-CSM path;
                            // layers stride by THIS value (layer = slot*cascade_count+c)
                            // so count 1 keeps master's layer indices
    int allocated_cascades; // Cascade capacity the map array was built for; a
                            // count change triggers a rebuild
    bool csm_debug;         // Tint fragments by cascade (the split/snap acceptance tool)
    // Per-layer state, count-strided (layer = slot * cascade_count + cascade)
    mat4 cascade_matrices[MAX_SHADOW_LIGHTS * SHADOW_CASCADES];
    vec4 cascade_params[MAX_SHADOW_LIGHTS * SHADOW_CASCADES]; // width, near, far; w unused
    float cascade_splits[SHADOW_CASCADES];                    // View-depth far bound per cascade

    // Perspective shadow maps for the punctual light types, one ordinary 2D
    // layer per map, kept apart from the directional cascade array so a
    // perspective projection never disturbs the ortho affine-shadow path.
    // 2D layers rather than a depth cube: a point light's six 90-degree frusta
    // are six layers the shader picks between by dominant axis, which lets ONE
    // sampler serve spot, point and area. That matters because samplerCubeArray
    // is GLSL 400 against a uniformly-330 shader set, and unit 15 is the last
    // one there is -- a second sampler has nowhere to bind.
    GLuint punctual_map_array;
    GLuint punctual_fbo;
    int punctual_allocated_layers; // Layer capacity built; a larger need rebuilds
    // Edge length the array was actually built at, which the VRAM budget picks
    // from the layer count rather than a constant. The shader needs it too --
    // its PCF step is one texel wide -- so it is uploaded, not mirrored as a
    // GLSL literal the way it used to be.
    int punctual_map_size;
    mat4 punctual_matrices[MAX_PUNCTUAL_SHADOW_LAYERS]; // perspective proj * lookAt per layer
    // Layers actually rendered this frame, not merely requested: it is the
    // shader's punctualShadowCount, which bounds every Light.shadow_layer the
    // UBO carries, so a pass that allocated but never drew must leave it 0
    // rather than point the lookup at an undrawn layer.
    int punctual_layer_count;
    bool punctual_pool_warned; // Latch so pool exhaustion logs once, not per frame
} ShadowSystem;

// Creation and destruction
ShadowSystem* create_shadow_system(int default_map_size);
void free_shadow_system(ShadowSystem* system);

// Shadow map array management
int init_shadow_map_array(ShadowSystem* system);
void free_shadow_map_array(ShadowSystem* system);

// Depth pass rendering
void begin_shadow_pass(ShadowSystem* system, size_t caster_index);
void end_shadow_pass(ShadowSystem* system);

// Light space matrix computation
void compute_directional_light_space_matrix(vec3 direction, vec3 scene_center, float ortho_size,
                                            float near_plane, float far_plane, mat4 dest);

// Camera slice for cascade fitting (kept small so shadow.c never learns
// about Engine); forward must be normalized
typedef struct CascadeCamera {
    vec3 position;
    vec3 forward;
    float fov_radians;
    float aspect_ratio;
} CascadeCamera;

// Fit one cascade: ortho box sized from the [slice_near, slice_far] view
// slice's bounding sphere (rotation-invariant -> stable under orbit), center
// snapped to shadow-texel increments in light view space, eye pushed back by
// scene_pad so out-of-slice geometry toward the light still casts. Writes
// the matrix and (width, orthoNear, orthoFar, 1.0) into out_params; .w is a
// bias factor defaulting to no scaling (the depth pass overrides it with
// the legacy-normalized value).
void compute_cascade_light_space_matrix(vec3 direction, const CascadeCamera* cam, float slice_near,
                                        float slice_far, float scene_pad, int map_size, mat4 dest,
                                        vec4 out_params);

// Upload the cascade uniform contract (cascadeCount, cascadeSplits,
// sceneOrthoWidth, and the count-strided lightSpaceMatrix/cascadeParams
// layers as ranged array uploads) to any program that samples the shadow
// maps. Location-guarded: programs lacking a uniform skip it. The layer
// layout law lives HERE and nowhere else.
void shadow_upload_cascade_uniforms(const ShadowSystem* system, UniformManager* u);

// Shadow map binding for main render pass
void bind_shadow_maps_to_program(ShadowSystem* system, ShaderProgram* program);

// Main shadow rendering function
void render_shadow_depth_pass(struct Engine* engine, struct Scene* scene);

struct PostFX;

// Flatten this frame's shadow casters + their lights into postfx's fog block
// (mirrors reflection_probe_publish_to_postfx; postfx never learns about the
// shadow system): per caster the count-strided cascade matrices, travel
// direction, and color * intensity, plus the map array handle and cascade
// count. Publishes count 0 when shadows are off or absent — fog degrades to
// ambient haze.
void shadow_publish_to_postfx(const struct Scene* scene, struct PostFX* fx);

#endif // _SHADOW_H_
