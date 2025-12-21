#ifndef _UTIL_H_
#define _UTIL_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "common.h"

/*
 * OpenGL
 */
void check_gl_error(const char* where);
size_t get_gl_max_lights();

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
