#ifndef _RENDER_H_
#define _RENDER_H_

#include <GLFW/glfw3.h>
#include <cglm/cglm.h>
#include <GL/glew.h>

#include "common.h"
#include "mesh.h"
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
} SceneCaptureState;

void scene_capture_begin(Engine* engine, struct Scene* scene, SceneCaptureState* saved);
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

// Animation state for skinned mesh rendering
// Set before rendering to enable bone matrix upload for skinned meshes
void set_render_animation_state(AnimationState* state);
AnimationState* get_render_animation_state(void);

// Upload skinning state ("skinned" flag + bone matrices) for a mesh from
// the active animation state; shared by the scene and shadow depth passes
void render_update_skinning_uniforms(ShaderProgram* program, const Mesh* mesh);

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
