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
// Per-fragment-stage sampler count (GL 4.1 min 16; the M1 Max reports 16) and
// max array-texture layers — queried once at engine init to budget the material
// sampler units and the mask texture array explicitly.
GLint get_gl_max_texture_image_units(void);
GLint get_gl_max_array_texture_layers(void);
GLint get_gl_max_texture_size(void);

// Fullscreen NDC quad (loc0 vec3 position, loc1 vec2 uv, GL_TRIANGLE_STRIP).
// The caller owns and deletes the returned VAO/VBO.
void create_fullscreen_quad_vao(GLuint* vao, GLuint* vbo);
void draw_fullscreen_quad(GLuint vao);

// Delete a GL object and zero the handle. The zeroing is what makes these
// worth a helper: a handle holding a deleted name is a double-delete once the
// driver recycles it, and any "is this allocated?" test on the handle reads
// true for a buffer that is gone -- postfx's TAAU seam dispatches on exactly
// such a test, so a stale post_fbo there is a live-looking buffer that no
// longer exists. glDelete* on 0 is a no-op, so these are also safe over a
// partially-constructed object.
//
// Not worth converting a delete-then-immediately-regenerate site: the next
// line overwrites the handle, so the zero says nothing (ibl.c and probe.c
// have several).
void gl_delete_fbo(GLuint* fbo);
void gl_delete_texture(GLuint* tex);

// The transfer format glTexImage2D wants beside an internal format when the
// data pointer is NULL -- a formality the spec still validates for base
// compatibility. ONE mapping for the two allocators that need it (postfx's
// render targets and the engine's scene attachments), so a new format is added
// once rather than remembered twice.
GLenum gl_transfer_format(GLenum internal_format);

/*
 * String
 */
void print_indentation(int depth);
char* safe_strdup(const char* s);

/*
 * Memory
 */
void* safe_realloc(void* ptr, size_t size);

// Ensure `*items` has room for `needed` elements of `elem_size`, doubling `*cap`
// until it does. False leaves both untouched, so a caller that checks it has lost
// nothing; the allocation failure is logged by safe_realloc.
//
// One helper for what was four hand-rolled doubling loops with three different
// seed capacities and two overflow policies, none of which agreed and none of
// which reported. The SEED stays a parameter because it is the one part that is
// genuinely per-caller -- a scene node's children start at 4 and a terrain
// quadtree's selection at 64, and neither wants the other's.
bool grow_array(void** items, size_t* cap, size_t needed, size_t elem_size, size_t seed);

/*
 * System
 */
// Logical CPU count, or 1 if the platform cannot be queried. Callers sizing a
// thread pool should leave headroom rather than spawning one thread per core --
// the main thread still has work to do while they run.
int get_cpu_cores(void);

/*
 * Path
 */
bool path_exists(const char* path);

// True for an absolute path under the engine's forward-slash convention: a
// leading '/', or a drive-qualified path (C:/...). cwk_path_is_absolute is not
// used because cwalk is pinned to UNIX style and would not recognize a drive.
bool path_is_absolute(const char* path);

// Read a whole file into a NUL-terminated malloc'd buffer (caller frees).
// Returns NULL on open/short-read failure; *out_len (optional) gets the byte
// count excluding the terminator.
char* read_entire_file(const char* path, long* out_len);
bool find_existing_subpath(const char* base_dir, char** subpath_ptr);
char* convert_windows_path_to_unix(const char* windows_path);
char* convert_and_normalize_path(const char* input_path);

/*
 * Colors
 */
void convert_rgb_to_float(vec3* albedo, int r, int g, int b);
void hex_to_rgb_float(vec3* albedo, const char* hex);

#endif // _UTIL_H_
