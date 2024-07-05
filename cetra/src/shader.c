#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ext/log.h"
#include "shader.h"
#include "util.h"

char* _read_shader_source(const char* filePath) {
    FILE* file = fopen(filePath, "r");
    if (!file) {
        log_error("Failed to open shader file: %s\n", filePath);
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    if (length == -1) {
        perror("Error occurred in ftell");
        fclose(file);
        return NULL;
    }
    fseek(file, 0, SEEK_SET);

    char* buffer = malloc(length + 1);
    if (!buffer) {
        log_error("Failed to allocate memory for shader source\n");
        fclose(file);
        return NULL;
    }

    size_t readSize = fread(buffer, 1, length, file);
    if (readSize != length) {
        log_error("Error reading shader file: %s\n", filePath);
        free(buffer);
        fclose(file);
        return NULL;
    }
    buffer[length] = '\0';
    fclose(file);

    return buffer;
}


Shader* create_shader_from_path(ShaderType type, const char* file_path) {
    char* source = _read_shader_source(file_path);
    if (!source) return NULL;

    Shader* shader = create_shader(type, source);
    free(source);

    return shader;
}

Shader* create_shader(ShaderType type, const char* source) {
    Shader* shader = malloc(sizeof(Shader));
    if (!shader) {
        log_error("Failed to allocate memory for shader\n");
        return NULL;
    }

    if(!source){
        log_error("Shader source is NULL\n");
        return NULL;
    }

    shader->type = type;
    shader->source = safe_strdup(source);

    GLenum glType;
    switch (shader->type) {
        case VERTEX_SHADER:   glType = GL_VERTEX_SHADER; break;
        case GEOMETRY_SHADER: glType = GL_GEOMETRY_SHADER; break;
        case FRAGMENT_SHADER: glType = GL_FRAGMENT_SHADER; break;
        default:
            log_error("Unknown shader type\n");
            return GL_FALSE;
    }

    shader->shaderID = glCreateShader(glType);

    if (shader->shaderID == 0) {
        log_error("Failed to create shader object.\n");
        return NULL;
    }

    return shader;
}

GLboolean compile_shader(Shader* shader) {
    if (!shader || !shader->source) {
        log_error("Invalid shader or shader source.\n");
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
                log_error("Shader compilation failed: %s\n", log);
                free(log);
            } else {
                log_error("Failed to allocate memory for shader log.\n");
            }
        } else {
            log_error("Shader compilation failed with no additional information.\n");
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
        if(shader->source){
            free(shader->source);
        }
        free(shader);
    }
}


