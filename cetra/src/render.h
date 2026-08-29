#ifndef _RENDER_H_
#define _RENDER_H_

#include <GLFW/glfw3.h>
#include <cglm/cglm.h>
#include <GL/glew.h>

#include "common.h"
#include "draw_list.h"
#include "mesh.h"
#include "profiler.h" // SubmitStats, which submit_draw_run fills
#include "ubo.h"
#include "program.h"
#include "shader.h"
#include "light.h"
#include "camera.h"
#include "engine.h"
#include "animation.h"

// Draws the engine's current scene. Animation time comes from
// engine->render_time, latched once per frame before any pass, so this and the
// shadow depth pass cannot disagree about where wind-displaced geometry is.
void render_current_scene(Engine* engine);

struct IBLResources;
struct Scene;

// Capture policy every caller of scene_capture_faces needs, in one place.
// Previously each caller hand-wrote it; the two copies had already drifted
// apart on whether a disabled shadow system still gets baked.
//
// Two clauses, both about making the six faces agree with each other:
//   CASCADES fit the MAIN camera, so a cube face sampling them as layer 0 would
//   drop its shadows -- forced to the camera-independent single map and baked
//   once for the whole burst.
//   THE CLOCK freezes, so wind-displaced geometry is caught at rest and the
//   faces agree with the shadow map they sample rather than merely being close.
//   The delta freezes with it: an instant advanced by nothing has zero motion
//   vectors rather than describing a step it never took.
//
// What stays with the caller is what genuinely differs: how it handles the async
// loader (block at load, or skip the frame and retry), and what it does with the
// cubemap afterwards.
typedef struct SceneCaptureState {
    int cascade_count;
    bool msm_enabled;
    double render_time;
    double render_delta;
    bool irradiance;
} SceneCaptureState;

// What a capture's output MEANS, which two callers need opposite answers to.
//
// A GI probe bakes IRRADIANCE, added to the analytic direct term -- so an
// emissive surface that is also a derived area panel (spec 11.49) must sit it
// out, or the panel delivers that light and the capture delivers it again. A
// reflection probe bakes RADIANCE, which is what a mirror sees, so the same
// surface must appear. Both go through scene_capture_faces and both raise
// engine->capturing, which is exactly why that flag cannot answer this.
//
// A third clause of capture policy, and it belongs here for the reason the two
// above it do: it was briefly hand-written by one caller with its own
// save/restore, which is the shape this struct's own history warns about.
typedef enum SceneCaptureKind {
    SCENE_CAPTURE_RADIANCE = 0, // what an eye or a mirror sees
    SCENE_CAPTURE_IRRADIANCE,   // what is added to the analytic direct term
} SceneCaptureKind;

void scene_capture_begin(Engine* engine, struct Scene* scene, SceneCaptureKind kind,
                         SceneCaptureState* saved);
void scene_capture_end(Engine* engine, struct Scene* scene, const SceneCaptureState* saved);

// Render the scene into the six faces of `dst_cubemap` from `position`, at
// `face_size` per face. `dst_depth_cubemap` picks the strategy:
//
//   0        supersample into scratch and box-downsample on the blit. The
//            capture has no MSAA, and grazing-angle aliasing at its horizon
//            bakes in as stripe moire that mirror reflections magnify into
//            banded streaks. Borrows ibl->capture_fbo / capture_rbo as that
//            scratch, so `ibl` is REQUIRED here.
//   non-zero render straight into both cubemaps' faces at native size, the only
//            way to keep depth (a blit cannot carry it between differently-sized
//            targets). `ibl` is unused, which is what lets a GI capture run in a
//            scene with no HDR environment at all.
//
// Renders the ENGINE'S CURRENT SCENE, deliberately with no scene parameter:
// render_current_scene resolves the scene itself through get_current_scene, so a
// scene argument here could not be honoured and would only read as if it were.
//
// Saves and restores every piece of engine and camera state it substitutes, so a
// capture leaves the next real frame bit-identical, and raises engine->capturing
// for the duration so passes that reach outside the bound target sit out.
//
// Pair with scene_capture_begin/end, which own the policy this does not.
void scene_capture_faces(Engine* engine, struct IBLResources* ibl, const vec3 position,
                         GLuint dst_cubemap, GLuint dst_depth_cubemap, int face_size,
                         float near_clip, float far_clip);

// Flatten the scene for this frame, if it has not been flattened already.
//
// One function rather than the three lines it replaces, because two of those
// lines are invariants and neither survives being retyped: the stamp decides
// when a list may be reused, and the LOD view decides what levels every pass
// will draw at. A site that spelled either differently would silently get a
// second list, or a depth map drawn at levels the camera pass disagrees with.
//
// Idempotent within a frame -- the first caller settles both, and the rest
// reuse. That ordering is load-bearing where a caller is about to substitute
// the camera: see scene_capture_begin.
void engine_build_draw_list(Engine* engine, struct Scene* scene);

// Point every lit material at the variant carrying exactly the features it can
// use (spec 11.93). Idempotent; the steady state is an integer compare per
// material.
//
// EVERY FRAME rather than on a dirty flag, and that is the design. The mask is a
// pure function of the material's own fields and two scene-wide bits, all of
// which a GUI slider can change with nothing marked dirty -- so a flag would have
// to be set by every writer of six fields across three files, and the one that
// forgot would leave a material on a variant missing a feature it had just been
// given. Recomputing makes that unrepresentable. The expensive half is compiling
// a variant, which happens once per distinct mask and is cached.
//
// CALL IT AFTER EVERY WRITER OF THE SCENE BITS IT READS, not merely before the
// readers of shader_program. Those pull in opposite directions and only the
// second half is obvious: scene_build_emissive_lights creates the LTC panels
// this scans for, so a call placed above it decides "no area lights" one line
// before the frame makes some.
//
// Membership is `ShaderProgram.pbr_features >= 0`, set by the variant builder.
// pbr_skinned is outside the family -- not because it has its own fragment
// source, which it does not, but because nothing yet builds skinned variants.
// Every skinned material therefore stays on the uber-shader.
void engine_resolve_material_variants(Engine* engine, struct Scene* scene);

// Animation state for skinned mesh rendering
// Set before rendering to enable bone matrix upload for skinned meshes
void set_render_animation_state(AnimationState* state);
AnimationState* get_render_animation_state(void);

// Upload skinning state ("skinned" flag + bone matrices) for a mesh from
// the active animation state; shared by the scene and shadow depth passes
void render_update_skinning_uniforms(ShaderProgram* program, const Mesh* mesh);

// What a pass culls against, assembled in the ONE place that knows what a cull
// view is made of -- which is also the one place `frustum_cull_enabled` is read.
// Three sites built this by hand in two different spellings, and the third only
// honoured the toggle because its caller happened to pass NULL.
//
// Call it AT the pass: the pose it reads is a process-global the app writes from
// inside its own render callback, so a view built anywhere else can describe a
// pose the pass is not about to upload.
CullView render_cull_view(const Engine* engine, const struct Scene* scene, const Frustum* frustum);

// What a draw loop has already bound, so it can skip re-binding it. Shared by
// the scene and shadow depth walkers.
//
// Every field is an identity key covering a BLOCK of work, never a single value
// -- `material` guards the whole material upload, textures included, in both
// walkers. A tracker per individual resource would be the same idea one level
// too low: it multiplies fields, and it splits one question ("is this material
// current?") into several that can disagree.
//
// The fields are written only by the operations below, which is what keeps the
// tracker and GL in step. Anything ELSE that binds a program or a VAO between
// two walks -- a fullscreen resolve between sub-passes, say -- makes every key
// here a lie, and must be followed by submit_state_reset.
typedef struct SubmitState {
    GLuint program;
    Material* material;
    GLuint vao;
} SubmitState;

// Forget everything tracked, without touching GL. Pessimistic by construction:
// claiming nothing is bound costs at most one redundant bind, where claiming
// something is bound when it is not drops the draw.
static inline void submit_state_reset(SubmitState* state) {
    state->program = 0;
    state->material = NULL;
    state->vao = 0;
}

// True when the program actually changed, i.e. when its per-program uniforms
// need uploading. Drops the material key on a switch: uniform state belongs to
// the program object, so a block uploaded to the old program says nothing about
// what the new one holds.
static inline bool submit_use_program(SubmitState* state, GLuint program_id) {
    if (state->program == program_id)
        return false;
    glUseProgram(program_id);
    state->program = program_id;
    state->material = NULL;
    return true;
}

// True when the caller must upload this material's block. Claims it either way,
// so an early return between this and the upload would silently skip it.
static inline bool submit_take_material(SubmitState* state, Material* material) {
    if (state->material == material)
        return false;
    state->material = material;
    return true;
}

// One chunk of per-instance transforms, mirroring InstanceBlock in
// include/instancing.glsl. The C layout is validated against the driver's
// reported block size at link, so a drift between the two reports rather than
// reading the wrong floats.
typedef struct InstanceChunk {
    mat4 model[UBO_INSTANCE_MAX];
    mat4 prev_model[UBO_INSTANCE_MAX];
    mat4 normal[UBO_INSTANCE_MAX]; // upper 3x3 read as mat3; see instancing.glsl
} InstanceChunk;

_Static_assert(sizeof(InstanceChunk) == UBO_INSTANCES_BLOCK_SIZE,
               "InstanceChunk must match the std140 block size the shader declares");

// Pack a run's transforms into the block and send it.
//
// `shading` is what the two consumers disagree about. The depth stage takes
// uInstModel alone, so filling prev_model and the normal matrix for it is
// stores no shader can observe -- two thirds of the block, on the pass that
// issues most of the batches. It still SENDS them, for the reason ubo_upload
// gives; this only skips writing them.
//
// `first` indexes `list->items`; the run is `run` consecutive entries from it.
void instance_chunk_upload(Ubo* ubo, InstanceChunk* chunk, const DrawList* list, size_t first,
                           size_t run, bool shading);

// The same, for a pass that submits its items in an order of its own: `first`
// and `run` index `order`, and `order` maps those positions to `list->items`.
//
// Two entry points rather than a nullable `order`, because the index space of
// `first` is what changes between them and a NULL test cannot say so. Passing a
// list-space `first` alongside an order reads entries that are all valid
// indices, fills the block with the wrong transforms, and submits the same draw
// and instance counts -- a wrong picture no counter can see.
void instance_chunk_upload_ordered(Ubo* ubo, InstanceChunk* chunk, const DrawList* list,
                                   const size_t* order, size_t order_count, size_t first,
                                   size_t run, bool shading);

// One draw of `instances` copies of `item`, plus the counters that describe it.
//
// Shared because "what a draw is" must not be able to differ between the passes:
// the depth prepass writes depth the shading pass then tests against, and the
// shadow pass writes depth the same geometry is shaded under. Three copies of
// this had already appeared, including three of `triangles += index_count / 3 *
// instances` -- a counter every submission gate reads.
//
// `two_sided` is RESOLVED BY THE CALLER rather than read from the item's flags,
// because the policy genuinely differs: the translucent shadow set runs with
// culling off for its whole traversal, so a per-item toggle there would
// re-enable it mid-pass and halve the absorbance of everything after the first
// two-sided caster.
void submit_draw_run(SubmitState* state, UniformManager* u, const DrawItem* item, size_t instances,
                     bool two_sided, SubmitStats* stats);

static inline void submit_bind_vao(SubmitState* state, GLuint vao) {
    if (state->vao != vao) {
        glBindVertexArray(vao);
        state->vao = vao;
    }
}

// Bone X-ray visualization
// Renders skeleton bones as colored lines overlaid on the model
// If anim_state is provided, shows animated pose in red
// If skeleton is provided, shows bind pose in green
void render_skeleton_bones(Engine* engine, Skeleton* skeleton, AnimationState* anim_state);

// X-ray light overlay: position cross per light (tinted by the light's color)
// plus a wireframe sphere at its clustered-lighting cull radius. Called by
// render_current_scene when engine->show_lights is set.
void render_light_overlay(Engine* engine, Scene* scene);

#endif // _RENDER_H_
