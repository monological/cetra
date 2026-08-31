// Defensive: expose the Darwin extensions get_cpu_cores uses (the BSD types
// <sys/sysctl.h> needs, and _SC_NPROCESSORS_ONLN) in case _POSIX_C_SOURCE is
// ever set on macOS again -- the build now scopes it to Linux. Must precede any
// system header.
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#if !defined(_WIN32)
#include <unistd.h> // sysconf (Linux get_cpu_cores)
#endif

// Querying the CPU count has no portable API; each platform needs its own
// header. Kept here so this is the one place in the engine that has to know.
#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/param.h>
#include <sys/sysctl.h>
#endif
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "util.h"

#include "ext/cwalk.h"
#include "ext/log.h"

void check_gl_error(const char* where) {
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        log_error("OpenGL error: %u - where: %s", err, where);
    }
}

GLint get_gl_max_texture_image_units(void) {
    GLint n = 0;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &n);
    return n;
}

GLint get_gl_max_array_texture_layers(void) {
    GLint n = 0;
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &n);
    return n;
}

GLint get_gl_max_texture_size(void) {
    GLint n = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &n);
    return n;
}

void create_fullscreen_quad_vao(GLuint* vao, GLuint* vbo) {
    // positions        // texCoords
    static const float quad_vertices[] = {
        -1.0f, 1.0f, 0.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 0.0f,
        1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 1.0f,  -1.0f, 0.0f, 1.0f, 0.0f,
    };

    glGenVertexArrays(1, vao);
    glGenBuffers(1, vbo);

    glBindVertexArray(*vao);
    glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));

    glBindVertexArray(0);
}

void draw_fullscreen_quad(GLuint vao) {
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

void gl_delete_fbo(GLuint* fbo) {
    glDeleteFramebuffers(1, fbo);
    *fbo = 0;
}

void gl_delete_texture(GLuint* tex) {
    glDeleteTextures(1, tex);
    *tex = 0;
}

GLenum gl_transfer_format(GLenum internal_format) {
    switch (internal_format) {
    case GL_R8:
    case GL_R16F:
    case GL_R32F:
        return GL_RED;
    case GL_RG16F:
    case GL_RG32F:
        return GL_RG;
    case GL_R11F_G11F_B10F:
        return GL_RGB;
    default:
        return GL_RGBA;
    }
}

void print_indentation(int depth) {
    for (int i = 0; i < depth; i++) {
        printf("    ");
    }
}

uint32_t fnv1a_bytes(const void* data, size_t bytes) {
    uint32_t h = 2166136261u;
    const uint8_t* p = (const uint8_t*)data;
    if (!p)
        return h;
    for (size_t b = 0; b < bytes; ++b) {
        h ^= p[b];
        h *= 16777619u;
    }
    return h;
}

uint64_t fnv1a64(uint64_t hash, const void* data, size_t bytes) {
    const uint8_t* p = (const uint8_t*)data;
    if (!p)
        return hash;
    for (size_t b = 0; b < bytes; ++b) {
        hash ^= p[b];
        hash *= 1099511628211ull;
    }
    return hash;
}

char* safe_strdup(const char* s) {
    if (s == NULL) {
        return NULL; // Return NULL if the input string is NULL
    }

    size_t len = strlen(s);
    char* d = (char*)malloc(len + 1); // Allocate memory for the string and null terminator

    if (d == NULL) {
        // Handle memory allocation failure
        return NULL;
    }

    memcpy(d, s, len + 1); // Use memcpy instead of strcpy to avoid potential issues
                           // and copy exactly len + 1 bytes (including null terminator)
    return d;
}

int get_cpu_cores(void) {
#if defined(_WIN32)
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return (int)sysinfo.dwNumberOfProcessors;
#elif defined(__APPLE__)
    // HW_AVAILCPU excludes cores taken offline, so prefer it and fall back to
    // the configured total. (sysconf works here too once _DARWIN_C_SOURCE is
    // set, but sysctl is the native query and needs no extra guarantees.)
    int mib[2] = {CTL_HW, HW_AVAILCPU};
    uint32_t count = 0;
    size_t len = sizeof(count);
    if (sysctl(mib, 2, &count, &len, NULL, 0) != 0 || count < 1) {
        mib[1] = HW_NCPU;
        len = sizeof(count);
        if (sysctl(mib, 2, &count, &len, NULL, 0) != 0) {
            return 1;
        }
    }
    return count > 0 ? (int)count : 1;
#elif defined(_SC_NPROCESSORS_ONLN)
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (int)count : 1;
#else
    return 1; // Unknown platform: callers treat this as "assume single core"
#endif
}

void* safe_realloc(void* ptr, size_t size) {
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    void* new_ptr = realloc(ptr, size);
    if (!new_ptr) {
        log_error("Failed to reallocate %zu bytes", size);
        // Original ptr is still valid, caller must handle
        return NULL;
    }
    return new_ptr;
}

bool grow_array(void** items, size_t* cap, size_t needed, size_t elem_size, size_t seed) {
    if (!items || !cap || elem_size == 0)
        return false;
    if (needed <= *cap)
        return true;
    size_t next = *cap ? *cap : (seed ? seed : 4);
    while (next < needed) {
        // Doubling can only overflow after the allocation it implies has already
        // failed, but the check is here rather than argued about at four call
        // sites.
        if (next > SIZE_MAX / 2)
            return false;
        next *= 2;
    }
    if (next > SIZE_MAX / elem_size)
        return false;
    void* grown = safe_realloc(*items, next * elem_size);
    if (!grown)
        return false;
    *items = grown;
    *cap = next;
    return true;
}

char* read_entire_file(const char* path, long* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return NULL;
    }
    char* buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        free(buf);
        return NULL;
    }
    buf[got] = '\0';
    if (out_len)
        *out_len = (long)got;
    return buf;
}

bool path_is_absolute(const char* path) {
    if (path[0] == '/')
        return true;
    bool drive_letter = (path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z');
    return drive_letter && path[1] == ':';
}

bool path_exists(const char* path) {
#if defined(_WIN32)
    // MSVC has neither lstat nor S_ISLNK. GetFileAttributes reports existence
    // directly; the symlink/reparse-point distinction the POSIX path draws is
    // not needed here (callers only ask "is there a file"). Narrow ASCII asset
    // paths only -- wide-char paths are a later refinement.
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat statbuf;

    // Use lstat to get info about the link itself
    if (lstat(path, &statbuf) != 0) {
        return false; // Path doesn't exist or error in lstat
    }

    // If it's not a symlink, just return true, the path exists
    if (!S_ISLNK(statbuf.st_mode)) {
        return true;
    }

    // If it is a symlink, use stat to check if the link target exists
    if (stat(path, &statbuf) != 0) {
        return false; // Broken symlink
    }

    return true;
#endif
}

/**
 * Attempts to find an existing file or directory by modifying and checking subpaths of a given
 * path.
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
        log_error("Invalid input");
        return false;
    }

    char* subpath = *subpath_ptr;
    size_t base_len = strlen(base_dir);
    size_t sub_len = strlen(subpath);
    size_t fullpath_size = base_len + sub_len + 2; // +2 for '/' and null terminator
    char* fullpath = malloc(fullpath_size);

    if (!fullpath) {
        log_error("Failed to allocate memory for fullpath.");
        return false;
    }

    bool path_found = false;
    while (*subpath != '\0') {
        snprintf(fullpath, fullpath_size, "%s/%s", base_dir, subpath);
        if (path_exists(fullpath)) {
            char* new_subpath = safe_strdup(fullpath);
            if (new_subpath) {
                free(*subpath_ptr);         // Free the original subpath
                *subpath_ptr = new_subpath; // Update the pointer to the new subpath
                path_found = true;
                break;
            } else {
                log_error("Failed to allocate memory for new subpath.");
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
        log_error("Error: Input path is NULL");
        return NULL;
    }

    int len = strlen(windows_path);
    char* unix_path = malloc(len + 1); // +1 for null terminator
    if (unix_path == NULL) {
        log_error("Error: Memory allocation failed");
        return NULL;
    }

    int i = 0, j = 0;
#if !defined(_WIN32)
    // On a non-Windows host a drive-qualified absolute path (C:\...) cannot be
    // used as-is, so drop the drive and let what remains resolve against cwd. On
    // Windows the drive is load-bearing -- keep it, just flip the separators.
    if (len > 2 && windows_path[1] == ':' && windows_path[2] == '\\') {
        i = 3;
    }
#endif

    // Convert the path
    for (; i < len; i++) {
        unix_path[j++] = (windows_path[i] == '\\') ? '/' : windows_path[i];
    }
    unix_path[j] = '\0'; // Null-terminate the Unix path

    return unix_path;
}

const char* path_last_sep(const char* path) {
    if (!path)
        return NULL;
    const char* slash = strrchr(path, '/');
#if defined(_WIN32)
    // Windows only: a backslash is a legal character in a POSIX filename, so
    // treating it as a separator there would split paths that do not have one.
    const char* back = strrchr(path, '\\');
    if (back && (!slash || back > slash))
        slash = back;
#endif
    return slash;
}

char* convert_and_normalize_path(const char* input_path) {
    if (input_path == NULL) {
        log_error("Error: Input path is NULL");
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
        log_error("Error allocating memory for path conversion.");
        return NULL;
    }

    // Normalize the path
    estimated_size = cwk_path_normalize(unix_path, NULL, 0);
    char* normalized_path = malloc(estimated_size + 1); // +1 for null terminator
    if (normalized_path == NULL) {
        log_error("Error allocating memory for normalized path.");
        free(unix_path);
        return NULL;
    }

    cwk_path_normalize(unix_path, normalized_path, estimated_size + 1);
    free(unix_path);
    return normalized_path;
}

void convert_rgb_to_float(vec3* albedo, int r, int g, int b) {
    (*albedo)[0] = r / 255.0f;
    (*albedo)[1] = g / 255.0f;
    (*albedo)[2] = b / 255.0f;
}

void hex_to_rgb_float(vec3* albedo, const char* hex) {
    if (!hex || !albedo) {
        return;
    }
    if (hex[0] == '#')
        hex++; // Skip the hash if present

    unsigned long rgb = strtoul(hex, NULL, 16); // Parse the hex value

    (*albedo)[0] = ((rgb >> 16) & 0xFF) / 255.0f; // Red component
    (*albedo)[1] = ((rgb >> 8) & 0xFF) / 255.0f;  // Green component
    (*albedo)[2] = (rgb & 0xFF) / 255.0f;         // Blue component
}
