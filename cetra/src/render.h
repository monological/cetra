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

#endif // _RENDER_H_
