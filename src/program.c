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
    check_gl_error("create program");

    if (program->id == 0) {
        fprintf(stderr, "Failed to create program object.\n");
        return NULL;
    }

    program->shaders = NULL;
    program->shader_count = 0;

    program->render_mode_loc = -1;
    program->near_clip_loc = -1;
    program->far_clip_loc = -1;

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

/*
 * GL attach and add shader to program.
 */
void attach_program_shader(ShaderProgram* program, Shader* shader) {
    if (program && shader && shader->shaderID) {
        // Attach the shader to the program
        glAttachShader(program->id, shader->shaderID);
        check_gl_error("attach shader");

        // Reallocate the shaders array to accommodate the new shader
        size_t new_count = program->shader_count + 1;
        Shader** new_shaders = realloc(program->shaders, new_count * sizeof(Shader*));
        if (new_shaders == NULL) {
            fprintf(stderr, "Failed to allocate memory for shaders\n");
            return;
        }

        // Add the new shader to the array and update the shader count
        new_shaders[program->shader_count] = shader;
        program->shaders = new_shaders;
        program->shader_count = new_count;
    } else {
        fprintf(stderr, "Failed to attach shader %i", shader ? shader->shaderID : 0);
    }
}

GLboolean link_program(ShaderProgram* program) {
    int success;

    glLinkProgram(program->id);
        check_gl_error("link program");
    glGetProgramiv(program->id, GL_LINK_STATUS, &success);
        check_gl_error("get program iv");
    if (!success) {
        GLint logLength = 0;
        glGetProgramiv(program->id, GL_INFO_LOG_LENGTH, &logLength);
        check_gl_error("glGetProgramiv log length");

        if (logLength > 0) {
            char* log = (char*)malloc(logLength);
            if (log) {
                glGetProgramInfoLog(program->id, logLength, &logLength, log);
                check_gl_error("glGetProgramInfoLog");
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

    program->render_mode_loc = glGetUniformLocation(program->id, "renderMode");
    program->near_clip_loc = glGetUniformLocation(program->id, "nearClip");
    program->far_clip_loc = glGetUniformLocation(program->id, "farClip");

    // Existing uniforms setup
    program->model_loc = glGetUniformLocation(program->id, "model");
    program->view_loc = glGetUniformLocation(program->id, "view");
    program->proj_loc = glGetUniformLocation(program->id, "projection");
    program->cam_pos_loc = glGetUniformLocation(program->id, "camPos");
    program->time_loc = glGetUniformLocation(program->id, "time");

    // New uniforms setup
    program->albedo_loc = glGetUniformLocation(program->id, "albedo");
    program->metallic_loc = glGetUniformLocation(program->id, "metallic");
    program->roughness_loc = glGetUniformLocation(program->id, "roughness");
    program->ao_loc = glGetUniformLocation(program->id, "ao");

    // Texture uniforms setup
    program->albedo_tex_loc = glGetUniformLocation(program->id, "albedoTex");
    program->normal_tex_loc = glGetUniformLocation(program->id, "normalTex");
    program->roughness_tex_loc = glGetUniformLocation(program->id, "roughnessTex");
    program->metalness_tex_loc = glGetUniformLocation(program->id, "metalnessTex");
    program->ao_tex_loc = glGetUniformLocation(program->id, "aoTex");
    program->emissive_tex_loc = glGetUniformLocation(program->id, "emissiveTex");
    program->height_tex_loc = glGetUniformLocation(program->id, "heightTex");
    program->opacity_tex_loc = glGetUniformLocation(program->id, "opacityTex");
    program->sheen_tex_loc = glGetUniformLocation(program->id, "sheenTex");
    program->reflectance_tex_loc = glGetUniformLocation(program->id, "reflectanceTex");

    // Texture exists uniform setup
    program->albedo_tex_exists_loc = glGetUniformLocation(program->id, "albedoTexExists");
    program->normal_tex_exists_loc = glGetUniformLocation(program->id, "normalTexExists");
    program->roughness_tex_exists_loc = glGetUniformLocation(program->id, "roughnessTexExists");
    program->metalness_tex_exists_loc = glGetUniformLocation(program->id, "metalnessTexExists");
    program->ao_tex_exists_loc = glGetUniformLocation(program->id, "aoTexExists");
    program->emissive_tex_exists_loc = glGetUniformLocation(program->id, "emissiveTexExists");
    program->height_tex_exists_loc = glGetUniformLocation(program->id, "heightTexExists");
    program->opacity_tex_exists_loc = glGetUniformLocation(program->id, "opacityTexExists");
    program->sheen_tex_exists_loc = glGetUniformLocation(program->id, "sheenTexExists");
    program->reflectance_tex_exists_loc = glGetUniformLocation(program->id, "reflectanceTexExists");

    return;
}

GLboolean init_program_shader(ShaderProgram* program, const char* vert_path,
        const char* frag_path, const char* geom_path) {
    GLboolean success = GL_TRUE;

    // Load and compile the vertex shader
    if (vert_path != NULL) {
        Shader* vertex_shader = create_shader_from_path(VERTEX_SHADER, vert_path);
        if (vertex_shader && compile_shader(vertex_shader)) {
            attach_program_shader(program, vertex_shader);
        } else {
            fprintf(stderr, "Vertex shader compilation failed\n");
            success = GL_FALSE;
        }
    } else {
        fprintf(stderr, "Vertex shader path is NULL\n");
        success = GL_FALSE;
    }

    // Load and compile the fragment shader
    if (frag_path != NULL) {
        Shader* fragment_shader = create_shader_from_path(FRAGMENT_SHADER, frag_path);
        if (fragment_shader && compile_shader(fragment_shader)) {
            attach_program_shader(program, fragment_shader);
        } else {
            fprintf(stderr, "Fragment shader compilation failed\n");
            success = GL_FALSE;
        }
    } else {
        fprintf(stderr, "Fragment shader path is NULL\n");
        success = GL_FALSE;
    }

    // Load and compile the geometry shader, if path is provided
    if (geom_path != NULL) {
        Shader* geometry_shader = create_shader_from_path(GEOMETRY_SHADER, geom_path);
        if (geometry_shader && compile_shader(geometry_shader)) {
            attach_program_shader(program, geometry_shader);
        } else {
            fprintf(stderr, "Geometry shader compilation failed\n");
            success = GL_FALSE;
        }
    }

    // Link the shader program
    if (success && !link_program(program)) {
        fprintf(stderr, "Shader program linking failed\n");
        success = GL_FALSE;
    }

    setup_program_uniforms(program);

    return success;
}

GLboolean validate_program(ShaderProgram* program){
    GLboolean success = GL_TRUE;

    glValidateProgram(program->id);
    check_gl_error("validate program");

    GLint validationStatus;
    glGetProgramiv(program->id, GL_VALIDATE_STATUS, &validationStatus);
    if (validationStatus == GL_FALSE) {
        fprintf(stderr, "Shader program validation failed\n");

        // Get and print the validation log
        GLint logLength;
        glGetProgramiv(program->id, GL_INFO_LOG_LENGTH, &logLength);
        char* logMessage = malloc(sizeof(char) * logLength);
        if (logMessage) {
            glGetProgramInfoLog(program->id, logLength, NULL, logMessage);
            fprintf(stderr, "Validation log: %s\n", logMessage);
            free(logMessage);
        } else {
            fprintf(stderr, "Failed to allocate memory for validation log\n");
        }

        success = GL_FALSE;
    }
    return success;
}

void free_program(ShaderProgram* program) {
    if (program != NULL) {
        if (program->id != 0) {
            glDeleteProgram(program->id);
        }

        if (program->shaders) {
            for (size_t i = 0; i < program->shader_count; ++i) {
                if (program->shaders[i]) {
                    free_shader(program->shaders[i]);
                }
            }
            free(program->shaders);
        }

        free(program);
    }
}

