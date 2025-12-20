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

void render_current_scene(Engine* engine, float time_value);

#endif // _RENDER_H_
