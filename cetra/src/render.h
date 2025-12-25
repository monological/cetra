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

void render_current_scene(Engine* engine, float time_value);

// Animation state for skinned mesh rendering
// Set before rendering to enable bone matrix upload for skinned meshes
void set_render_animation_state(AnimationState* state);
AnimationState* get_render_animation_state(void);

#endif // _RENDER_H_
