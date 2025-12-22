#include <stdlib.h>
#include <stdio.h>

#include "common.h"
#include "ext/log.h"
#include "program.h"
#include "util.h"

ShaderProgram* create_program(const char* name) {
    ShaderProgram* program = malloc(sizeof(ShaderProgram));
    if (!program) {
        log_error("Failed to allocate memory for shader program");
        return NULL;
    }
    program->id = glCreateProgram();

    if (program->id == 0) {
        log_error("Failed to create program object.");
        free(program);
        return NULL;
    }

    if (!name) {
        log_error("Shader program name is NULL");
        glDeleteProgram(program->id);
        free(program);
        return NULL;
    }

    program->name = safe_strdup(name);
    program->shaders = NULL;
    program->shader_count = 0;
    program->uniforms = NULL;

    return program;
}

ShaderProgram* create_program_from_paths(const char* name, const char* vert_path,
                                         const char* frag_path, const char* geo_path) {

    if (!name) {
        log_error("Shader program name is NULL");
        return NULL;
    }

    GLboolean success = GL_TRUE;

    ShaderProgram* program = create_program(name);
    if (program == NULL) {
        log_error("Failed to create program by name %s", name);
        return NULL;
    }

    // Load and compile the vertex shader
    if (vert_path != NULL) {
        Shader* vertex_shader = create_shader_from_path(VERTEX_SHADER, vert_path);
        if (vertex_shader && compile_shader(vertex_shader)) {
            attach_shader_to_program(program, vertex_shader);
        } else {
            log_error("Vertex shader compilation failed");
        }
    } else {
        log_error("Vertex shader path is NULL");
        success = GL_FALSE;
    }

    // Load and compile the geometry shader, if path is provided
    if (geo_path != NULL) {
        Shader* geometry_shader = create_shader_from_path(GEOMETRY_SHADER, geo_path);
        if (geometry_shader && compile_shader(geometry_shader)) {
            attach_shader_to_program(program, geometry_shader);
        } else {
            log_error("Geometry shader compilation failed");
            success = GL_FALSE;
        }
    }

    // Load and compile the fragment shader
    if (frag_path != NULL) {
        Shader* fragment_shader = create_shader_from_path(FRAGMENT_SHADER, frag_path);
        if (fragment_shader && compile_shader(fragment_shader)) {
            attach_shader_to_program(program, fragment_shader);
        } else {
            log_error("Fragment shader compilation failed");
            success = GL_FALSE;
        }
    } else {
        log_error("Fragment shader path is NULL");
        success = GL_FALSE;
    }

    // Link the shader program
    if (success && !link_program(program)) {
        log_error("Shader program linking failed");
        success = GL_FALSE;
    }

    // Setup uniforms and other initializations as needed
    if (success) {
        setup_program_uniforms(program);
    } else {
        free_program(program);
        program = NULL;
    }

    return program;
}

ShaderProgram* create_program_from_source(const char* name, const char* vert_source,
                                          const char* frag_source, const char* geo_source) {

    if (!name) {
        log_error("Shader program name is NULL");
        return NULL;
    }

    GLboolean success = GL_TRUE;

    ShaderProgram* program = create_program(name);
    if (program == NULL) {
        log_error("Failed to create program by name %s", name);
        return NULL;
    }

    // Create and compile the vertex shader
    if (vert_source != NULL) {
        Shader* vertex_shader = create_shader(VERTEX_SHADER, vert_source);
        if (vertex_shader && compile_shader(vertex_shader)) {
            attach_shader_to_program(program, vertex_shader);
        } else {
            log_error("Vertex shader compilation failed");
            success = GL_FALSE;
        }
    } else {
        log_error("Vertex shader source is NULL");
        success = GL_FALSE;
    }

    // Create and compile the fragment shader
    if (frag_source != NULL) {
        Shader* fragment_shader = create_shader(FRAGMENT_SHADER, frag_source);
        if (fragment_shader && compile_shader(fragment_shader)) {
            attach_shader_to_program(program, fragment_shader);
        } else {
            log_error("Fragment shader compilation failed");
            success = GL_FALSE;
        }
    } else {
        log_error("Fragment shader source is NULL");
        success = GL_FALSE;
    }

    // Create and compile the geometry shader, if source is provided
    if (geo_source != NULL) {
        Shader* geometry_shader = create_shader(GEOMETRY_SHADER, geo_source);
        if (geometry_shader && compile_shader(geometry_shader)) {
            attach_shader_to_program(program, geometry_shader);
        } else {
            log_error("Geometry shader compilation failed");
            success = GL_FALSE;
        }
    }

    // Link the shader program
    if (success && !link_program(program)) {
        log_error("Shader program linking failed");
        success = GL_FALSE;
    }

    // Setup uniforms and other initializations as needed
    if (success) {
        setup_program_uniforms(program);
    } else {
        free_program(program);
        program = NULL;
    }

    return program;
}

void free_program(ShaderProgram* program) {
    if (program != NULL) {
        if (program->id != 0) {
            glDeleteProgram(program->id);
        }

        if (program->name != NULL) {
            free(program->name);
        }

        if (program->shaders) {
            for (size_t i = 0; i < program->shader_count; ++i) {
                if (program->shaders[i]) {
                    free_shader(program->shaders[i]);
                }
            }
            free(program->shaders);
        }

        if (program->uniforms) {
            free_uniform_manager(program->uniforms);
        }

        free(program);
    }
}

GLboolean reload_program_from_paths(ShaderProgram* program, const char* vert_path,
                                    const char* frag_path, const char* geo_path) {
    if (!program || !vert_path || !frag_path) {
        log_error("Invalid arguments to reload_program_from_paths");
        return GL_FALSE;
    }

    // Compile new shaders first (don't modify program until all succeed)
    Shader* new_vert = create_shader_from_path(VERTEX_SHADER, vert_path);
    if (!new_vert || !compile_shader(new_vert)) {
        log_error("Failed to compile vertex shader: %s", vert_path);
        if (new_vert)
            free_shader(new_vert);
        return GL_FALSE;
    }

    Shader* new_frag = create_shader_from_path(FRAGMENT_SHADER, frag_path);
    if (!new_frag || !compile_shader(new_frag)) {
        log_error("Failed to compile fragment shader: %s", frag_path);
        free_shader(new_vert);
        if (new_frag)
            free_shader(new_frag);
        return GL_FALSE;
    }

    Shader* new_geo = NULL;
    if (geo_path) {
        new_geo = create_shader_from_path(GEOMETRY_SHADER, geo_path);
        if (!new_geo || !compile_shader(new_geo)) {
            log_error("Failed to compile geometry shader: %s", geo_path);
            free_shader(new_vert);
            free_shader(new_frag);
            if (new_geo)
                free_shader(new_geo);
            return GL_FALSE;
        }
    }

    // All shaders compiled successfully - now modify the program
    // Detach and free old shaders
    for (size_t i = 0; i < program->shader_count; ++i) {
        if (program->shaders[i]) {
            glDetachShader(program->id, program->shaders[i]->shaderID);
            free_shader(program->shaders[i]);
        }
    }
    free(program->shaders);
    program->shaders = NULL;
    program->shader_count = 0;

    // Attach new shaders
    glAttachShader(program->id, new_vert->shaderID);
    glAttachShader(program->id, new_frag->shaderID);
    if (new_geo)
        glAttachShader(program->id, new_geo->shaderID);

    // Relink
    if (!link_program(program)) {
        log_error("Failed to relink program after shader reload");
        free_shader(new_vert);
        free_shader(new_frag);
        if (new_geo)
            free_shader(new_geo);
        return GL_FALSE;
    }

    // Store new shaders
    size_t new_count = new_geo ? 3 : 2;
    program->shaders = malloc(new_count * sizeof(Shader*));
    if (!program->shaders) {
        log_error("Failed to allocate shader array");
        free_shader(new_vert);
        free_shader(new_frag);
        if (new_geo)
            free_shader(new_geo);
        return GL_FALSE;
    }
    program->shaders[0] = new_vert;
    program->shaders[1] = new_frag;
    if (new_geo)
        program->shaders[2] = new_geo;
    program->shader_count = new_count;

    // Re-cache uniforms
    if (program->uniforms) {
        free_uniform_manager(program->uniforms);
    }
    program->uniforms = create_uniform_manager(program->id);
    if (program->uniforms) {
        uniform_cache_standard(program->uniforms);
        uniform_cache_lights(program->uniforms, get_gl_max_lights());
    }

    log_info("Reloaded shader program: %s", program->name);
    return GL_TRUE;
}

void attach_shader_to_program(ShaderProgram* program, Shader* shader) {
    if (program && shader && shader->shaderID) {
        // Attach the shader to the program
        glAttachShader(program->id, shader->shaderID);
        check_gl_error("attach shader");

        // Reallocate the shaders array to accommodate the new shader
        size_t new_count = program->shader_count + 1;
        Shader** new_shaders = realloc(program->shaders, new_count * sizeof(Shader*));
        if (new_shaders == NULL) {
            log_error("Failed to allocate memory for shaders");
            return;
        }

        // Add the new shader to the array and update the shader count
        new_shaders[program->shader_count] = shader;
        program->shaders = new_shaders;
        program->shader_count = new_count;
    } else {
        log_error("Failed to attach shader %i", shader ? shader->shaderID : 0);
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

                log_error("Program %s compilation failed: %s", program->name, log);
                free(log);
            } else {
                log_error("Failed to allocate memory for program log.");
            }
        } else {
            log_error("Program compilation failed with no additional information.");
        }
        return GL_FALSE;
    }

    return GL_TRUE;
}

GLboolean validate_program(ShaderProgram* program) {
    GLboolean success = GL_TRUE;

    glValidateProgram(program->id);
    // Note: Some drivers generate spurious GL errors during validation.
    // The validation status (GL_VALIDATE_STATUS) is what actually matters.
    while (glGetError() != GL_NO_ERROR) {
    }

    GLint validationStatus;
    glGetProgramiv(program->id, GL_VALIDATE_STATUS, &validationStatus);
    if (validationStatus == GL_FALSE) {
        log_error("Shader program validation failed");

        // Get and print the validation log
        GLint logLength;
        glGetProgramiv(program->id, GL_INFO_LOG_LENGTH, &logLength);
        char* logMessage = malloc(sizeof(char) * logLength);
        if (logMessage) {
            glGetProgramInfoLog(program->id, logLength, NULL, logMessage);
            log_error("Validation log: %s", logMessage);
            free(logMessage);
        } else {
            log_error("Failed to allocate memory for validation log");
        }

        success = GL_FALSE;
    }
    return success;
}

void setup_program_uniforms(ShaderProgram* program) {
    if (program == NULL || program->id == 0) {
        log_error("Invalid shader program.");
        return;
    }

    program->uniforms = create_uniform_manager(program->id);
    if (!program->uniforms) {
        log_error("Failed to create uniform manager");
        return;
    }

    uniform_cache_standard(program->uniforms);
    uniform_cache_lights(program->uniforms, get_gl_max_lights());
    uniform_cache_shadows(program->uniforms, 3);
}

ShaderProgram* create_pbr_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("pbr", pbr_vert_shader_str, pbr_frag_shader_str,
                                              NULL)) == NULL) {
        log_error("Failed to initialize PBR shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_shape_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("shape", shape_vert_shader_str, shape_frag_shader_str,
                                              shape_geo_shader_str)) == NULL) {
        log_error("Failed to initialize shape shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_xyz_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("xyz", xyz_vert_shader_str, xyz_frag_shader_str,
                                              NULL)) == NULL) {
        log_error("Failed to initialize xyz shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_shadow_depth_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("shadow_depth", shadow_depth_vert_shader_str,
                                              shadow_depth_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize shadow depth shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_skybox_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("skybox", skybox_vert_shader_str,
                                              skybox_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize skybox shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_ibl_equirect_to_cube_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("ibl_equirect_to_cube", ibl_cubemap_vert_shader_str,
                                              ibl_equirect_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize IBL equirect-to-cube shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_ibl_irradiance_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("ibl_irradiance", ibl_cubemap_vert_shader_str,
                                              ibl_irradiance_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize IBL irradiance shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_ibl_prefilter_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("ibl_prefilter", ibl_cubemap_vert_shader_str,
                                              ibl_prefilter_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize IBL prefilter shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_ibl_brdf_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("ibl_brdf", ibl_brdf_vert_shader_str,
                                              ibl_brdf_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize IBL BRDF shader program");
        return NULL;
    }

    return program;
}
