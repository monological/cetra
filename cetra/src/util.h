#ifndef _UTIL_H_
#define _UTIL_H_

#include <stdbool.h>
#include <GL/glew.h>
#include <cglm/cglm.h>

#include "common.h"

/*
 * OpenGL
 */
void check_gl_error(const char* where);
size_t get_gl_max_lights();
// Per-fragment-stage sampler count (GL 4.1 min 16; the M1 Max reports 16) and
// max array-texture layers — queried once at engine init to budget the material
// sampler units and the mask texture array explicitly.
GLint get_gl_max_texture_image_units(void);
GLint get_gl_max_array_texture_layers(void);

// Fullscreen NDC quad (loc0 vec3 position, loc1 vec2 uv, GL_TRIANGLE_STRIP).
// The caller owns and deletes the returned VAO/VBO.
void create_fullscreen_quad_vao(GLuint* vao, GLuint* vbo);
void draw_fullscreen_quad(GLuint vao);

/*
 * String
 */
void print_indentation(int depth);
char* safe_strdup(const char* s);

/*
 * Memory
 */
void* safe_realloc(void* ptr, size_t size);

/*
 * Path
 */
bool path_exists(const char* path);
bool find_existing_subpath(const char* base_dir, char** subpath_ptr);
char* convert_windows_path_to_unix(const char* windows_path);
char* convert_and_normalize_path(const char* input_path);

/*
 * Colors
 */
void convert_rgb_to_float(vec3* albedo, int r, int g, int b);
void hex_to_rgb_float(vec3* albedo, const char* hex);

#endif // _UTIL_H_
