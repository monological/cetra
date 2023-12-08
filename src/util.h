#ifndef _UTIL_H_
#define _UTIL_H_

#include <stdbool.h>

/*
 * OpenGL
 */ 
void check_gl_error(const char* where);

/*
 * String
 */
void print_indentation(int depth);

/*
 * Path
 */
bool path_exists(const char* path);
bool find_existing_subpath(const char* base_dir, char** subpath_ptr);
char* convert_windows_path_to_unix(const char* windows_path);
char* convert_and_normalize_path(const char *input_path);

#endif // _UTIL_H_

