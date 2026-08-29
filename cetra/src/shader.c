#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ext/log.h"
#include "shader.h"
#include "util.h"

char* _read_shader_source(const char* filePath) {
    long length = 0;
    char* buffer = read_entire_file(filePath, &length);
    if (!buffer) {
        log_error("Failed to read shader file: %s", filePath);
        return NULL;
    }
    if (length > 10 * 1024 * 1024) { // Max 10MB
        log_error("Shader file too large: %s", filePath);
        free(buffer);
        return NULL;
    }
    return buffer;
}

// The source with `defines` spliced in after its `#version` line, or a plain
// copy when there are none. Caller owns the result.
//
// A source with no `#version` gets the block at the FRONT. That is not a
// tolerated edge case -- GLSL without a version directive is 1.10, where none of
// this engine's shaders compile, so the only way to reach it is a truncated or
// misidentified string, and putting the defines first keeps the driver's error
// pointing at line 1 of the real problem rather than at a define block.
char* shader_source_with_defines(const char* source, const char* defines) {
    if (!defines || !*defines)
        return safe_strdup(source);

    const char* version = strstr(source, "#version");
    const char* body = source;
    size_t head = 0;
    if (version) {
        const char* eol = strchr(version, '\n');
        if (eol) {
            head = (size_t)(eol - source) + 1;
            body = eol + 1;
        }
    }

    size_t defines_len = strlen(defines);
    size_t body_len = strlen(body);
    // +1 for a newline after the block, so a defines string without a trailing
    // one cannot weld itself to the first line of the body.
    char* out = malloc(head + defines_len + 1 + body_len + 1);
    if (!out) {
        log_error("Failed to allocate spliced shader source");
        return NULL;
    }
    memcpy(out, source, head);
    memcpy(out + head, defines, defines_len);
    out[head + defines_len] = '\n';
    memcpy(out + head + defines_len + 1, body, body_len);
    out[head + defines_len + 1 + body_len] = '\0';
    return out;
}

Shader* create_shader_from_path(ShaderType type, const char* file_path) {
    char* source = _read_shader_source(file_path);
    if (!source)
        return NULL;

    Shader* shader = create_shader(type, source);
    free(source);

    return shader;
}

Shader* create_shader(ShaderType type, const char* source) {
    if (!source) {
        log_error("Shader source is NULL");
        return NULL;
    }

    Shader* shader = malloc(sizeof(Shader));
    if (!shader) {
        log_error("Failed to allocate memory for shader");
        return NULL;
    }

    shader->type = type;
    shader->source = safe_strdup(source);

    GLenum glType;
    switch (shader->type) {
        case VERTEX_SHADER:
            glType = GL_VERTEX_SHADER;
            break;
        case GEOMETRY_SHADER:
            glType = GL_GEOMETRY_SHADER;
            break;
        case FRAGMENT_SHADER:
            glType = GL_FRAGMENT_SHADER;
            break;
        default:
            log_error("Unknown shader type");
            free(shader->source);
            free(shader);
            return NULL;
    }

    shader->shaderID = glCreateShader(glType);

    if (shader->shaderID == 0) {
        log_error("Failed to create shader object.");
        free(shader->source);
        free(shader);
        return NULL;
    }

    return shader;
}

GLboolean compile_shader(Shader* shader) {
    if (!shader || !shader->source) {
        log_error("Invalid shader or shader source.");
        return GL_FALSE;
    }

    // Set shader source and compile
    const GLchar* source = shader->source;
    glShaderSource(shader->shaderID, 1, &source, NULL);
    check_gl_error("glShaderSource");

    glCompileShader(shader->shaderID);
    check_gl_error("glCompileShader");

    // Check compilation status
    int success;
    glGetShaderiv(shader->shaderID, GL_COMPILE_STATUS, &success);
    check_gl_error("glGetShaderiv");

    if (!success) {
        GLint logLength = 0;
        glGetShaderiv(shader->shaderID, GL_INFO_LOG_LENGTH, &logLength);
        check_gl_error("glGetShaderiv log length");

        if (logLength > 0) {
            char* log = (char*)malloc(logLength);
            if (log) {
                glGetShaderInfoLog(shader->shaderID, logLength, &logLength, log);
                check_gl_error("glGetShaderInfoLog");
                log_error("Shader compilation failed: %s", log);
                free(log);
            } else {
                log_error("Failed to allocate memory for shader log.");
            }
        } else {
            log_error("Shader compilation failed with no additional information.");
        }

        return GL_FALSE;
    }

    return GL_TRUE;
}

void free_shader(Shader* shader) {
    if (shader) {
        if (shader->shaderID != 0) {
            glDeleteShader(shader->shaderID);
        }
        if (shader->source) {
            free(shader->source);
        }
        free(shader);
    }
}
