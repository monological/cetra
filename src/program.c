#include <stdlib.h>
#include <stdio.h>

#include "program.h"
#include "util.h"

Program* create_program() {
    Program* program = malloc(sizeof(Program));
    if (!program) {
        fprintf(stderr, "Failed to allocate memory for shader program\n");
        return NULL;
    }
    program->programID = glCreateProgram();
        CheckOpenGLError("create program");
    if (program->programID == 0) {
        fprintf(stderr, "Failed to create program object.\n");
        return NULL;
    }
    return program;
}

void attach_shader(Program* program, Shader* shader) {
    if (program && shader && shader->shaderID) {
        glAttachShader(program->programID, shader->shaderID);
        CheckOpenGLError("attach shader");
    }else{
        fprintf(stderr, "Failedto attach shader %i", shader->shaderID);
    }
}

GLboolean link_program(Program* program) {
    int success;

    glLinkProgram(program->programID);
        CheckOpenGLError("link program");
    glGetProgramiv(program->programID, GL_LINK_STATUS, &success);
        CheckOpenGLError("get program iv");
    if (!success) {
        GLint logLength = 0;
        glGetProgramiv(program->programID, GL_INFO_LOG_LENGTH, &logLength);
        CheckOpenGLError("glGetProgramiv log length");

        if (logLength > 0) {
            char* log = (char*)malloc(logLength);
            if (log) {
                glGetProgramInfoLog(program->programID, logLength, &logLength, log);
                CheckOpenGLError("glGetProgramInfoLog");
                fprintf(stderr, "Program compilation failed: %s\n", log);
                free(log);
            } else {
                fprintf(stderr, "Failed to allocate memory for program log.\n");
            }
        } else {
            fprintf(stderr, "Program compilation failed with no additional information.\n");
        }
        return GL_FALSE;
    }

    return GL_TRUE;
}

void free_program(Program* program) {
    if (program != NULL) {
        if (program->programID != 0) {
            glDeleteProgram(program->programID);
        }
        free(program);
    }
}

