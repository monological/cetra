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

// Render the scene into the six faces of `dst_cubemap` from `position`, at
// `face_size` per face, supersampled `ss_factor`x and box-downsampled on the blit.
//
// Renders the ENGINE'S CURRENT SCENE, deliberately with no scene parameter:
// render_current_scene resolves the scene itself through get_current_scene, so a
// scene argument here could not be honoured and would only read as if it were.
//
// Saves and restores every piece of engine and camera state it substitutes, so a
// capture leaves the next real frame bit-identical, and raises engine->capturing
// for the duration so passes that reach outside the bound target sit out.
// Borrows ibl->capture_fbo / capture_rbo as scratch and resizes the latter.
//
// The CALLER owns capture policy that is not about substituting a camera: shadow
// cascade fitting, freezing the animation clock, draining the async loader,
// building the mask array, and whatever it does with the cubemap afterwards.
void scene_capture_faces(Engine* engine, struct IBLResources* ibl, const vec3 position,
                         GLuint dst_cubemap, int face_size, int ss_factor, float near_clip,
                         float far_clip);

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
