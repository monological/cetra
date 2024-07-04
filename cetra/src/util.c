#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <unistd.h>
#include <math.h>

#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "util.h"
#include "ext/cwalk.h"


void check_gl_error(const char* where) {
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "OpenGL error: %u - where: %s\n", err, where);
    }
}

size_t get_gl_max_lights() {
    GLint max_uniform_components;
    glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, &max_uniform_components);

    if (max_uniform_components < USED_UNIFORM_COMPONENTS) {
        fprintf(stderr, "Insufficient uniform components available.\n");
        return 0;
    }

    size_t max_light_uniforms = max_uniform_components - USED_UNIFORM_COMPONENTS;
    size_t max_lights = max_light_uniforms / COMPONENTS_PER_LIGHT;

    return max_lights;
}

void print_indentation(int depth) {
    for (int i = 0; i < depth; i++) {
        printf("    ");
    }
}

char *safe_strdup(const char *s) {
    if (s == NULL) {
        return NULL;  // Return NULL if the input string is NULL
    }

    size_t len = strlen(s);
    char *d = (char *)malloc(len + 1);  // Allocate memory for the string and null terminator

    if (d == NULL) {
        // Handle memory allocation failure
        return NULL;
    }

    memcpy(d, s, len + 1);  // Use memcpy instead of strcpy to avoid potential issues
                             // and copy exactly len + 1 bytes (including null terminator)
    return d;
}

bool path_exists(const char* path) {
    struct stat statbuf;
    struct stat lstatbuf;

    // Use lstat to get info about the link itself
    if (lstat(path, &lstatbuf) != 0) {
        return false; // Path doesn't exist or error in lstat
    }

    // If it's not a symlink, just return true, the path exists
    if (!S_ISLNK(lstatbuf.st_mode)) {
        return true;
    }

    // If it is a symlink, use stat to check if the link target exists
    if (stat(path, &statbuf) != 0) {
        return false; // Broken symlink
    }

    return true;
}

/**
 * Attempts to find an existing file or directory by modifying and checking subpaths of a given path.
 *
 * Parameters:
 *   base_dir - The base directory path.
 *   subpath  - A modifiable string of the subpath, updated to the existing path if found.
 *
 * The function iterates through the subpath, removing leading segments and checking
 * if the resulting path exists in conjunction with the base directory. If an existing
 * path is found, it updates 'subpath' to this path and returns true, otherwise returns false.
 *
 * Note: 'subpath' should have sufficient size to store the full path.
 */
bool find_existing_subpath(const char* base_dir, char** subpath_ptr) {
    if (!base_dir || !subpath_ptr || !*subpath_ptr) {
        fprintf(stderr, "Invalid input\n");
        return false;
    }

    char* subpath = *subpath_ptr;
    size_t base_len = strlen(base_dir);
    size_t sub_len = strlen(subpath);
    size_t fullpath_size = base_len + sub_len + 2; // +2 for '/' and null terminator
    char* fullpath = malloc(fullpath_size);

    if (!fullpath) {
        fprintf(stderr, "Failed to allocate memory for fullpath.\n");
        return false;
    }

    bool path_found = false;
    while (*subpath != '\0') {
        snprintf(fullpath, fullpath_size, "%s/%s", base_dir, subpath);
        if (path_exists(fullpath)) {
            char* new_subpath = safe_strdup(fullpath);
            if (new_subpath) {
                free(*subpath_ptr);  // Free the original subpath
                *subpath_ptr = new_subpath;  // Update the pointer to the new subpath
                path_found = true;
                break;
            } else {
                fprintf(stderr, "Failed to allocate memory for new subpath.\n");
                break;
            }
        }
        char* slash_pos = strchr(subpath, '/');
        if (slash_pos) {
            memmove(subpath, slash_pos + 1, strlen(slash_pos));
        } else {
            break;
        }
    }

    free(fullpath);
    return path_found;
}

char* convert_windows_path_to_unix(const char* windows_path) {
    if (windows_path == NULL) {
        fprintf(stderr, "Error: Input path is NULL\n");
        return NULL;
    }

    int len = strlen(windows_path);
    char* unix_path = malloc(len + 1); // +1 for null terminator
    if (unix_path == NULL) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        return NULL;
    }

    int i = 0, j = 0;
    // Skip drive letter if present
    if (len > 2 && windows_path[1] == ':' && windows_path[2] == '\\') {
        i = 3;
    }

    // Convert the path
    for (; i < len; i++) {
        unix_path[j++] = (windows_path[i] == '\\') ? '/' : windows_path[i];
    }
    unix_path[j] = '\0'; // Null-terminate the Unix path

    return unix_path;
}

char* convert_and_normalize_path(const char *input_path) {
    if (input_path == NULL) {
        fprintf(stderr, "Error: Input path is NULL\n");
        return NULL;
    }

    char* unix_path = NULL;
    size_t estimated_size;
    enum cwk_path_style style = cwk_path_guess_style(input_path);

    // Convert the path if it's a Windows path
    if (style == CWK_STYLE_WINDOWS) {
        unix_path = convert_windows_path_to_unix(input_path);
    } else {
        unix_path = safe_strdup(input_path);
    }

    if (unix_path == NULL) {
        fprintf(stderr, "Error allocating memory for path conversion.\n");
        return NULL;
    }

    // Normalize the path
    estimated_size = cwk_path_normalize(unix_path, NULL, 0);
    char* normalized_path = malloc(estimated_size + 1); // +1 for null terminator
    if (normalized_path == NULL) {
        fprintf(stderr, "Error allocating memory for normalized path.\n");
        free(unix_path);
        return NULL;
    }

    cwk_path_normalize(unix_path, normalized_path, estimated_size + 1);
    free(unix_path);
    return normalized_path;
}





