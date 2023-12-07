#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <GLFW/glfw3.h>

#include "util.h"
#include "cwalk.h"

void check_gl_error(const char* where) {
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "OpenGL error: %u - where: %s\n", err, where);
    }
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
    enum cwk_path_style style;
    char *unix_path, *normalized_path;
    size_t estimated_size;

    // Guess the style of the path
    style = cwk_path_guess_style(input_path);

    // Convert the path if it's a Windows path
    if (style == CWK_STYLE_WINDOWS) {
        unix_path = convert_windows_path_to_unix(input_path);
        if (unix_path == NULL) {
            fprintf(stderr, "Error converting Windows path to Unix path.\n");
            return NULL;
        }
    } else {
        // If it's not a Windows path, just duplicate the input path
        unix_path = strdup(input_path);
        if (unix_path == NULL) {
            fprintf(stderr, "Error: Memory allocation failed\n");
            return NULL;
        }
    }

    // Normalize the path
    estimated_size = cwk_path_normalize(unix_path, NULL, 0);
    normalized_path = malloc(estimated_size + 1); // +1 for null terminator
    if (normalized_path == NULL) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        free(unix_path);
        return NULL;
    }

    cwk_path_normalize(unix_path, normalized_path, estimated_size + 1);

    // Free the Unix path as it's no longer needed
    free(unix_path);

    return normalized_path; // Return the dynamically allocated normalized path
}



