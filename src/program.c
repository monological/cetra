#include <stdlib.h>
#include <stdio.h>

#include "program.h"
#include "util.h"

ShaderProgram* create_program() {
    ShaderProgram* program = malloc(sizeof(ShaderProgram));
    if (!program) {
        fprintf(stderr, "Failed to allocate memory for shader program\n");
        return NULL;
    }
    program->id = glCreateProgram();
    CheckOpenGLError("create program");

    if (program->id == 0) {
        fprintf(stderr, "Failed to create program object.\n");
        return NULL;
    }

    program->shaders = NULL;
    program->shader_count = 0;

    program->model_loc = -1;
    program->view_loc = -1;
    program->proj_loc = -1;
    program->cam_pos_loc = -1;
    program->time_loc = -1;

    program->albedo_loc = -1;
    program->metallic_loc = -1;
    program->roughness_loc = -1;
    program->ao_loc = -1;

    program->albedo_tex_loc = -1;
    program->normal_tex_loc = -1;
    program->roughness_tex_loc = -1;
    program->metalness_tex_loc = -1;
    program->ao_tex_loc = -1;
    program->emissive_tex_loc = -1;
    program->height_tex_loc = -1;
    program->opacity_tex_loc = -1;
    program->sheen_tex_loc = -1;
    program->reflectance_tex_loc = -1;

    program->albedo_tex_exists_loc = -1;
    program->normal_tex_exists_loc = -1;
    program->roughness_tex_exists_loc = -1;
    program->metalness_tex_exists_loc = -1;
    program->ao_tex_exists_loc = -1;
    program->emissive_tex_exists_loc = -1;
    program->height_tex_exists_loc = -1;
    program->opacity_tex_exists_loc = -1;
    program->sheen_tex_exists_loc = -1;
    program->reflectance_tex_exists_loc = -1;

    return program;
}

void attach_shader(ShaderProgram* program, Shader* shader) {
    if (program && shader && shader->shaderID) {
        glAttachShader(program->id, shader->shaderID);
        CheckOpenGLError("attach shader");
    }else{
        fprintf(stderr, "Failedto attach shader %i", shader->shaderID);
    }
}

GLboolean link_program(ShaderProgram* program) {
    int success;

    glLinkProgram(program->id);
        CheckOpenGLError("link program");
    glGetProgramiv(program->id, GL_LINK_STATUS, &success);
        CheckOpenGLError("get program iv");
    if (!success) {
        GLint logLength = 0;
        glGetProgramiv(program->id, GL_INFO_LOG_LENGTH, &logLength);
        CheckOpenGLError("glGetProgramiv log length");

        if (logLength > 0) {
            char* log = (char*)malloc(logLength);
            if (log) {
                glGetProgramInfoLog(program->id, logLength, &logLength, log);
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

/*
 * Sets up the uniforms. Only call after compiling and linking.
 *
 */

void setup_program_uniforms(ShaderProgram* program) {
    if (program == NULL || program->id == 0) {
        fprintf(stderr, "Invalid shader program.\n");
        return;
    }

    // Existing uniforms setup
    program->model_loc = glGetUniformLocation(program->id, "model");
    if (program->model_loc == -1) fprintf(stderr, "Uniform 'model' not found.\n");

    program->view_loc = glGetUniformLocation(program->id, "view");
    if (program->view_loc == -1) fprintf(stderr, "Uniform 'view' not found.\n");

    program->proj_loc = glGetUniformLocation(program->id, "projection");
    if (program->proj_loc == -1) fprintf(stderr, "Uniform 'projection' not found.\n");

    program->cam_pos_loc = glGetUniformLocation(program->id, "camPos");
    if (program->cam_pos_loc == -1) fprintf(stderr, "Uniform 'camPos' not found.\n");

    program->time_loc = glGetUniformLocation(program->id, "time");
    if (program->time_loc == -1) fprintf(stderr, "Uniform 'time' not found.\n");

    // New uniforms setup
    program->albedo_loc = glGetUniformLocation(program->id, "albedo");
    if (program->albedo_loc == -1) fprintf(stderr, "Uniform 'albedo' not found.\n");

    program->metallic_loc = glGetUniformLocation(program->id, "metallic");
    if (program->metallic_loc == -1) fprintf(stderr, "Uniform 'metallic' not found.\n");

    program->roughness_loc = glGetUniformLocation(program->id, "roughness");
    if (program->roughness_loc == -1) fprintf(stderr, "Uniform 'roughness' not found.\n");

    program->ao_loc = glGetUniformLocation(program->id, "ao");
    if (program->ao_loc == -1) fprintf(stderr, "Uniform 'ao' not found.\n");

    // Texture uniforms setup
    program->albedo_tex_loc = glGetUniformLocation(program->id, "albedoTex");
    if (program->albedo_tex_loc == -1) fprintf(stderr, "Uniform 'albedoTex' not found.\n");

    program->normal_tex_loc = glGetUniformLocation(program->id, "normalTex");
    if (program->normal_tex_loc == -1) fprintf(stderr, "Uniform 'normalTex' not found.\n");

    program->roughness_tex_loc = glGetUniformLocation(program->id, "roughnessTex");
    if (program->roughness_tex_loc == -1) fprintf(stderr, "Uniform 'roughnessTex' not found.\n");

    program->metalness_tex_loc = glGetUniformLocation(program->id, "metalnessTex");
    if (program->metalness_tex_loc == -1) fprintf(stderr, "Uniform 'metalnessTex' not found.\n");

    program->ao_tex_loc = glGetUniformLocation(program->id, "aoTex");
    if (program->ao_tex_loc == -1) fprintf(stderr, "Uniform 'aoTex' not found.\n");

    program->emissive_tex_loc = glGetUniformLocation(program->id, "emissiveTex");
    if (program->emissive_tex_loc == -1) fprintf(stderr, "Uniform 'emissiveTex' not found.\n");

    program->height_tex_loc = glGetUniformLocation(program->id, "heightTex");
    if (program->height_tex_loc == -1) fprintf(stderr, "Uniform 'heightTex' not found.\n");

    program->opacity_tex_loc = glGetUniformLocation(program->id, "opacityTex");
    if (program->opacity_tex_loc == -1) fprintf(stderr, "Uniform 'opacityTex' not found.\n");

    program->sheen_tex_loc = glGetUniformLocation(program->id, "sheenTex");
    if (program->sheen_tex_loc == -1) fprintf(stderr, "Uniform 'sheenTex' not found.\n");

    program->reflectance_tex_loc = glGetUniformLocation(program->id, "reflectanceTex");
    if (program->reflectance_tex_loc == -1) fprintf(stderr, "Uniform 'reflectanceTex' not found.\n");

    // Texture exists uniform setup
    program->albedo_tex_exists_loc = glGetUniformLocation(program->id, "albedoTexExists");
    if (program->albedo_tex_exists_loc == -1) fprintf(stderr, "Uniform 'albedoTexExists' not found.\n");

    program->normal_tex_exists_loc = glGetUniformLocation(program->id, "normalTexExists");
    if (program->normal_tex_exists_loc == -1) fprintf(stderr, "Uniform 'normalTexExists' not found.\n");

    program->roughness_tex_exists_loc = glGetUniformLocation(program->id, "roughnessTexExists");
    if (program->roughness_tex_exists_loc == -1) fprintf(stderr, "Uniform 'roughnessTexExists' not found.\n");

    program->metalness_tex_exists_loc = glGetUniformLocation(program->id, "metalnessTexExists");
    if (program->metalness_tex_exists_loc == -1) fprintf(stderr, "Uniform 'metalnessTexExists' not found.\n");

    program->ao_tex_exists_loc = glGetUniformLocation(program->id, "aoTexExists");
    if (program->ao_tex_exists_loc == -1) fprintf(stderr, "Uniform 'aoTexExists' not found.\n");

    program->emissive_tex_exists_loc = glGetUniformLocation(program->id, "emissiveTexExists");
    if (program->emissive_tex_exists_loc == -1) fprintf(stderr, "Uniform 'emissiveTexExists' not found.\n");

    program->height_tex_exists_loc = glGetUniformLocation(program->id, "heightTexExists");
    if (program->height_tex_exists_loc == -1) fprintf(stderr, "Uniform 'heightTexExists' not found.\n");

    program->opacity_tex_exists_loc = glGetUniformLocation(program->id, "opacityTexExists");
    if (program->opacity_tex_exists_loc == -1) fprintf(stderr, "Uniform 'opacityTexExists' not found.\n");

    program->sheen_tex_exists_loc = glGetUniformLocation(program->id, "sheenTexExists");
    if (program->sheen_tex_exists_loc == -1) fprintf(stderr, "Uniform 'sheenTexExists' not found.\n");

    program->reflectance_tex_exists_loc = glGetUniformLocation(program->id, "reflectanceTexExists");
    if (program->reflectance_tex_exists_loc == -1) fprintf(stderr, "Uniform 'reflectanceTexExists' not found.\n");
}


GLboolean init_shader_program(ShaderProgram* program, const char* vert_path,
        const char* frag_path, const char* geom_path) {
    GLboolean success = GL_TRUE;

    // Load and compile the vertex shader
    if (vert_path) {
        Shader* vertex_shader = create_shader_from_path(VERTEX, vert_path);
        if (vertex_shader && compile_shader(vertex_shader)) {
            attach_shader(program, vertex_shader);
        } else {
            fprintf(stderr, "Vertex shader compilation failed\n");
            success = GL_FALSE;
        }
        if (vertex_shader) free_shader(vertex_shader);
    } else {
        fprintf(stderr, "Vertex shader path is NULL\n");
        success = GL_FALSE;
    }

    // Load and compile the fragment shader
    if (frag_path) {
        Shader* fragment_shader = create_shader_from_path(FRAGMENT, frag_path);
        if (fragment_shader && compile_shader(fragment_shader)) {
            attach_shader(program, fragment_shader);
        } else {
            fprintf(stderr, "Fragment shader compilation failed\n");
            success = GL_FALSE;
        }
        if (fragment_shader) free_shader(fragment_shader);
    } else {
        fprintf(stderr, "Fragment shader path is NULL\n");
        success = GL_FALSE;
    }

    // Load and compile the geometry shader, if path is provided
    if (geom_path) {
        Shader* geometry_shader = create_shader_from_path(GEOMETRY, geom_path);
        if (geometry_shader && compile_shader(geometry_shader)) {
            attach_shader(program, geometry_shader);
        } else {
            fprintf(stderr, "Geometry shader compilation failed\n");
            success = GL_FALSE;
        }
        if (geometry_shader) free_shader(geometry_shader);
    }

    // Link the shader program
    if (success && !link_program(program)) {
        fprintf(stderr, "Shader program linking failed\n");
        success = GL_FALSE;
    }

    glValidateProgram(program->id);
    CheckOpenGLError("validate program");

    setup_program_uniforms(program);

    return success;
}

void free_program(ShaderProgram* program) {
    if (program != NULL) {
        if (program->id != 0) {
            glDeleteProgram(program->id);
        }

        // Free shaders
        if (program->shaders) {
            for (size_t i = 0; i < program->shader_count; ++i) {
                if (program->shaders[i]) {
                    // Assuming there is a function to free a shader
                    free_shader(program->shaders[i]);
                }
            }
            free(program->shaders);
        }

        free(program);
    }
}
