#include <stdlib.h>
#include <stdio.h>

#include "common.h"
#include "ext/log.h"
#include "program.h"
#include "util.h"

ShaderProgram* create_program(const char* name){
    ShaderProgram* program = malloc(sizeof(ShaderProgram));
    if (!program) {
        log_error("Failed to allocate memory for shader program");
        return NULL;
    }
    program->id = glCreateProgram();

    if (program->id == 0) {
        log_error("Failed to create program object.");
        return NULL;
    }

    if(!name){
        log_error("Shader program name is NULL");
        return NULL;
    }

    program->name = safe_strdup(name);

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

    program->line_width_loc = -1;

    return program;
}

ShaderProgram* create_program_from_paths(const char* name, const char* vert_path,
        const char* frag_path, const char* geo_path) {

    if(!name){
        log_error("Shader program name is NULL");
        return NULL;
    }

    GLboolean success = GL_TRUE;

    ShaderProgram* program = create_program(name);
    if(program == NULL){
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
    }else{
        free_program(program);
        program = NULL;
    }

    return program;
}

ShaderProgram* create_program_from_source(const char* name, const char* vert_source,
        const char* frag_source, const char* geo_source) {

    if(!name){
        log_error("Shader program name is NULL");
        return NULL;
    }

    GLboolean success = GL_TRUE;

    ShaderProgram* program = create_program(name);
    if(program == NULL){
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
    }else{
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

        if(program->name != NULL){
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

        if (program->light_position_loc) {
            free(program->light_position_loc);
        }
        if (program->light_direction_loc) {
            free(program->light_direction_loc);
        }
        if (program->light_color_loc) {
            free(program->light_color_loc);
        }
        if (program->light_specular_loc) {
            free(program->light_specular_loc);
        }
        if (program->light_ambient_loc) {
            free(program->light_ambient_loc);
        }
        if (program->light_intensity_loc) {
            free(program->light_intensity_loc);
        }
        if (program->light_constant_loc) {
            free(program->light_constant_loc);
        }
        if (program->light_linear_loc) {
            free(program->light_linear_loc);
        }
        if (program->light_quadratic_loc) {
            free(program->light_quadratic_loc);
        }
        if (program->light_cutOff_loc) {
            free(program->light_cutOff_loc);
        }
        if (program->light_outerCutOff_loc) {
            free(program->light_outerCutOff_loc);
        }
        if (program->light_type_loc) {
            free(program->light_type_loc);
        }

        free(program);
    }
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

GLboolean validate_program(ShaderProgram* program){
    GLboolean success = GL_TRUE;

    glValidateProgram(program->id);
    check_gl_error("validate program");

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


/*
 * Sets up the uniforms. Only call after compiling and linking.
 *
 */

void setup_program_uniforms(ShaderProgram* program) {
    if (program == NULL || program->id == 0) {
        log_error("Invalid shader program.");
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

    program->max_lights = get_gl_max_lights(); 

    program->light_position_loc = malloc(program->max_lights * sizeof(GLint));
    program->light_direction_loc = malloc(program->max_lights * sizeof(GLint));
    program->light_color_loc = malloc(program->max_lights * sizeof(GLint));
    program->light_specular_loc = malloc(program->max_lights * sizeof(GLint));
    program->light_ambient_loc = malloc(program->max_lights * sizeof(GLint));
    program->light_intensity_loc = malloc(program->max_lights * sizeof(GLint));
    program->light_constant_loc = malloc(program->max_lights * sizeof(GLint));
    program->light_linear_loc = malloc(program->max_lights * sizeof(GLint));
    program->light_quadratic_loc = malloc(program->max_lights * sizeof(GLint));
    program->light_cutOff_loc = malloc(program->max_lights * sizeof(GLint));
    program->light_outerCutOff_loc = malloc(program->max_lights * sizeof(GLint));
    program->light_type_loc = malloc(program->max_lights * sizeof(GLint));
    program->light_size_loc = malloc(program->max_lights * sizeof(GLint));

    // Retrieve uniform locations for light properties
    char uniformName[64];
    for (size_t i = 0; i < program->max_lights; ++i) {
        sprintf(uniformName, "lights[%zu].position", i);
        program->light_position_loc[i] = glGetUniformLocation(program->id, uniformName);

        sprintf(uniformName, "lights[%zu].direction", i);
        program->light_direction_loc[i] = glGetUniformLocation(program->id, uniformName);

        sprintf(uniformName, "lights[%zu].color", i);
        program->light_color_loc[i] = glGetUniformLocation(program->id, uniformName);

        sprintf(uniformName, "lights[%zu].specular", i);
        program->light_specular_loc[i] = glGetUniformLocation(program->id, uniformName);

        sprintf(uniformName, "lights[%zu].ambient", i);
        program->light_ambient_loc[i] = glGetUniformLocation(program->id, uniformName);

        sprintf(uniformName, "lights[%zu].intensity", i);
        program->light_intensity_loc[i] = glGetUniformLocation(program->id, uniformName);

        sprintf(uniformName, "lights[%zu].constant", i);
        program->light_constant_loc[i] = glGetUniformLocation(program->id, uniformName);

        sprintf(uniformName, "lights[%zu].linear", i);
        program->light_linear_loc[i] = glGetUniformLocation(program->id, uniformName);

        sprintf(uniformName, "lights[%zu].quadratic", i);
        program->light_quadratic_loc[i] = glGetUniformLocation(program->id, uniformName);

        sprintf(uniformName, "lights[%zu].cutOff", i);
        program->light_cutOff_loc[i] = glGetUniformLocation(program->id, uniformName);

        sprintf(uniformName, "lights[%zu].outerCutOff", i);
        program->light_outerCutOff_loc[i] = glGetUniformLocation(program->id, uniformName);

        sprintf(uniformName, "lights[%zu].type", i);
        program->light_type_loc[i] = glGetUniformLocation(program->id, uniformName);

        sprintf(uniformName, "lights[%zu].size", i);
        program->light_size_loc[i] = glGetUniformLocation(program->id, uniformName);
    }

    // Retrieve the location for the number of lights uniform
    program->num_lights_loc = glGetUniformLocation(program->id, "numLights");

    program->line_width_loc = glGetUniformLocation(program->id, "lineWidth");

    return;
}

ShaderProgram* create_pbr_program(){
    ShaderProgram* program = NULL;

    if((program = create_program_from_source("pbr", 
            pbr_vert_shader_str, pbr_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize PBR shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_shape_program(){
    ShaderProgram* program = NULL;

    if((program = create_program_from_source("shape", 
            shape_vert_shader_str, shape_frag_shader_str, shape_geo_shader_str)) == NULL) {
        log_error("Failed to initialize shape shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_xyz_program(){
    ShaderProgram* program = NULL;

    if((program = create_program_from_source("xyz", 
            xyz_vert_shader_str, xyz_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize xyz shader program");
        return NULL;
    }

    return program;
}

