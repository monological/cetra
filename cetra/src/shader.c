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
    if (!source)
        return NULL;
    if (!defines || !*defines)
        return safe_strdup(source);

    // The directive must START a line. strstr alone finds the token anywhere,
    // and pbr_frag includes a chunk whose comment contains the literal word
    // "#version" -- so on a source whose real directive was missing or malformed
    // it would have spliced the block into the middle of that include.
    const char* line = source;
    const char* version = NULL;
    while (*line) {
        const char* t = line;
        while (*t == ' ' || *t == '\t')
            ++t;
        if (strncmp(t, "#version", 8) == 0) {
            version = t;
            break;
        }
        const char* nl = strchr(line, '\n');
        if (!nl)
            break;
        line = nl + 1;
    }

    const char* body = source;
    size_t head = 0;
    if (version) {
        const char* eol = strchr(version, '\n');
        if (eol) {
            head = (size_t)(eol - source) + 1;
            body = eol + 1;
        }
    }

    // The body's line numbers must not move. gen_shader_header.py preserves them
    // deliberately -- it emits a bare newline for every blank line rather than
    // dropping it, because a GLSL error reports a line number and nothing here
    // dumps the source to compare against. Shifting the body by the height of
    // the define block would put every error in this file off by that much, and
    // pbr_frag now compiles many ways, so a variant-only error is a thing that
    // can exist. The generator's own note that #line "was tried and removed"
    // does not transfer: that was about mapping errors back to N chunk files,
    // where this is one file at a constant offset.
    //
    // The body resumes at the line after #version, which is line 2.
    const char* resume = "#line 2\n";
    size_t defines_len = strlen(defines);
    size_t resume_len = strlen(resume);
    size_t body_len = strlen(body);
    // +1 for a newline after the block, so a defines string without a trailing
    // one cannot weld itself to the directive that follows it.
    size_t out_len = head + defines_len + 1 + resume_len + body_len;
    char* out = malloc(out_len + 1);
    if (!out) {
        log_error("Failed to allocate spliced shader source");
        return NULL;
    }
    char* w = out;
    memcpy(w, source, head);
    w += head;
    memcpy(w, defines, defines_len);
    w += defines_len;
    *w++ = '\n';
    memcpy(w, resume, resume_len);
    w += resume_len;
    memcpy(w, body, body_len);
    w += body_len;
    *w = '\0';
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
