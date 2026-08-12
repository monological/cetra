#include <stdlib.h>
#include <stdio.h>

#include "common.h"
#include "ext/log.h"
#include "program.h"
#include "shadow.h"
#include "ubo.h"
#include "util.h"

// Fullscreen post-pass program helper (defined with the postfx constructors)
static ShaderProgram* create_post_program(const char* name, const char* frag_src);

ShaderProgram* create_program(const char* name) {
    ShaderProgram* program = calloc(1, sizeof(ShaderProgram));
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
    }

    // Block bindings are program state reset by re-linking; re-wire them
    program->instanced = ubo_wire_blocks(program->id);

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
    uniform_cache_shadows(program->uniforms, MAX_SHADOW_LIGHTS, SHADOW_CASCADES,
                          MAX_PUNCTUAL_SHADOW_LAYERS);

    // Clustered-forward blocks (spec 9.1): bind to the global binding points
    // and guard against C/GLSL layout drift. No-ops for programs that don't
    // declare (or strip) them. InstanceBlock is the one whose absence the
    // submitter has to know about, so its answer is kept on the program.
    program->instanced = ubo_wire_blocks(program->id);
}

ShaderProgram* create_pbr_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("pbr", pbr_vert_shader_str, pbr_frag_shader_str,
                                              NULL)) == NULL) {
        log_error("Failed to initialize PBR shader program");
        return NULL;
    }

    // pbr_vert takes its clip position from object_position.glsl, the same chunk
    // depth_prepass_vert uses, and declares `invariant gl_Position`.
    program->depth_prepass_safe = true;
    return program;
}

ShaderProgram* create_particle_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("particle", particle_vert_shader_str,
                                              particle_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize particle shader program");
        return NULL;
    }

    return program;
}

// GPU particle-sim UPDATE program (spec 5.2). Vertex-only: it captures its
// outputs into a transform-feedback buffer under GL_RASTERIZER_DISCARD and never
// rasterizes, so create_program_from_source (which requires a fragment shader)
// can't build it. Assemble it from the primitives and set the feedback varyings
// between attach and link.
ShaderProgram* create_particle_sim_program() {
    ShaderProgram* program = create_program("particle_sim");
    if (!program) {
        log_error("Failed to create particle sim program");
        return NULL;
    }

    Shader* vs = create_shader(VERTEX_SHADER, particle_sim_vert_shader_str);
    if (!vs || !compile_shader(vs)) {
        log_error("Particle sim vertex shader compilation failed");
        free_program(program);
        return NULL;
    }
    attach_shader_to_program(program, vs);

    // Capture the 5 out vec4s interleaved into one buffer. Order MUST match the
    // ParticleGpuState field layout (particle_sim.h). Must precede linking.
    const char* varyings[] = {"oCenter", "oParams", "oColor", "oVelAge", "oLife"};
    glTransformFeedbackVaryings(program->id, 5, varyings, GL_INTERLEAVED_ATTRIBS);

    if (!link_program(program)) {
        log_error("Particle sim program linking failed");
        free_program(program);
        return NULL;
    }

    setup_program_uniforms(program);
    return program;
}

ShaderProgram* create_pbr_skinned_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("pbr_skinned", pbr_skinned_vert_shader_str,
                                              pbr_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize skinned PBR shader program");
        return NULL;
    }

    // Same chunk, same invariant. Its skinning matches depth_prepass_vert's
    // because both call skin.glsl's skinMatrix on the same uniforms.
    program->depth_prepass_safe = true;
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

ShaderProgram* create_depth_prepass_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("depth_prepass", depth_prepass_vert_shader_str,
                                              depth_prepass_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize depth prepass shader program");
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

ShaderProgram* create_ibl_charlie_prefilter_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("ibl_charlie_prefilter", ibl_cubemap_vert_shader_str,
                                              ibl_charlie_prefilter_frag_shader_str, NULL)) ==
        NULL) {
        log_error("Failed to initialize IBL Charlie prefilter shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_ibl_brdf_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("ibl_brdf", post_vert_shader_str,
                                              ibl_brdf_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize IBL BRDF shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_sky_transmittance_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("sky_transmittance", post_vert_shader_str,
                                              sky_transmittance_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize sky transmittance shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_sky_multiscatter_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("sky_multiscatter", post_vert_shader_str,
                                              sky_multiscatter_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize sky multiscatter shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_sky_debug_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("sky_debug", post_vert_shader_str,
                                              sky_debug_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize sky debug shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_sky_view_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("sky_view", post_vert_shader_str,
                                              sky_view_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize sky view shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_mask_copy_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("mask_copy", post_vert_shader_str,
                                              mask_copy_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize mask copy shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_msm_resolve_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("msm_resolve", post_vert_shader_str,
                                              msm_resolve_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize moment shadow resolve shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_shadow_absorb_program() {
    ShaderProgram* program = NULL;

    // shadow_depth's vertex stage, not a copy of it. Skinning and wind must
    // displace an absorbance caster exactly as they displace the same surface
    // in the depth pass, and the only way two files stay identical is to be
    // one file -- the copy this replaced had already drifted in its comments.
    if ((program = create_program_from_source("shadow_absorb", shadow_depth_vert_shader_str,
                                              shadow_absorb_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize translucent shadow absorb shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_tsm_resolve_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("tsm_resolve", post_vert_shader_str,
                                              tsm_resolve_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize translucent shadow resolve shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_sky_env_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("sky_env", ibl_cubemap_vert_shader_str,
                                              sky_env_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize sky env shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_sky_aerial_program() {
    return create_post_program("sky_aerial", aerial_lut_frag_shader_str);
}

ShaderProgram* create_cloud_noise_debug_program() {
    return create_post_program("cloud_noise_debug", cloud_noise_debug_frag_shader_str);
}

ShaderProgram* create_cloud_march_program() {
    return create_post_program("cloud_march", cloud_march_frag_shader_str);
}

ShaderProgram* create_sky_env_clouds_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("sky_env_clouds", ibl_cubemap_vert_shader_str,
                                              sky_env_clouds_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize sky env clouds shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_sky_background_clouds_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("sky_background_clouds", skybox_vert_shader_str,
                                              sky_background_clouds_frag_shader_str, NULL)) ==
        NULL) {
        log_error("Failed to initialize sky background clouds shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_sky_background_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("sky_background", skybox_vert_shader_str,
                                              sky_background_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize sky background shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_text_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("text", text_vert_shader_str, text_frag_shader_str,
                                              NULL)) == NULL) {
        log_error("Failed to initialize text shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_bone_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("bone", bone_vert_shader_str, bone_frag_shader_str,
                                              NULL)) == NULL) {
        log_error("Failed to initialize bone shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_shadow_catcher_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("shadow_catcher", catcher_vert_shader_str,
                                              catcher_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize shadow catcher shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_bloom_bright_program() {
    return create_post_program("bloom_bright", bloom_bright_frag_shader_str);
}

ShaderProgram* create_bloom_down_program() {
    return create_post_program("bloom_downsample", bloom_downsample_frag_shader_str);
}

// Same tent source as the SSR/fog composite, but its own program object:
// bloom re-uploads texelSize per pyramid level, the shared program's is
// set once at init
ShaderProgram* create_bloom_up_program() {
    return create_post_program("bloom_upsample", upsample_tent_frag_shader_str);
}

ShaderProgram* create_lens_flare_program() {
    return create_post_program("lens_flare", lens_flare_frag_shader_str);
}

ShaderProgram* create_tonemap_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("tonemap", post_vert_shader_str,
                                              tonemap_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize tonemap shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_spec_occ_composite_program() {
    return create_post_program("spec_occ_composite", spec_occ_composite_frag_shader_str);
}

ShaderProgram* create_gtao_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("gtao", post_vert_shader_str, gtao_frag_shader_str,
                                              NULL)) == NULL) {
        log_error("Failed to initialize GTAO shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_ssao_blur_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("ssao_blur", post_vert_shader_str,
                                              ssao_blur_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize SSAO blur shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_ssr_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("ssr", post_vert_shader_str, ssr_frag_shader_str,
                                              NULL)) == NULL) {
        log_error("Failed to initialize SSR shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_ssr_hiz_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("ssr_hiz", post_vert_shader_str,
                                              ssr_hiz_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize SSR hi-z shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_upsample_tent_program() {
    return create_post_program("upsample_tent", upsample_tent_frag_shader_str);
}

ShaderProgram* create_taa_resolve_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("taa_resolve", post_vert_shader_str,
                                              taa_resolve_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize TAA resolve shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_taau_resolve_program() {
    return create_post_program("taau_resolve", taau_resolve_frag_shader_str);
}

ShaderProgram* create_temporal_accum_program() {
    return create_post_program("temporal_accum", temporal_accum_frag_shader_str);
}

ShaderProgram* create_ssgi_composite_program() {
    return create_post_program("ssgi_composite", ssgi_composite_frag_shader_str);
}

// Fullscreen post-pass program: the shared post vertex shader plus a fragment
// source. Every postfx constructor is this call with a different pair.
static ShaderProgram* create_post_program(const char* name, const char* frag_src) {
    ShaderProgram* program = create_program_from_source(name, post_vert_shader_str, frag_src, NULL);
    if (!program)
        log_error("Failed to initialize %s shader program", name);
    return program;
}

ShaderProgram* create_ssgi_accum_program() {
    return create_post_program("ssgi_accum", ssgi_accum_frag_shader_str);
}

ShaderProgram* create_ssgi_atrous_program() {
    return create_post_program("ssgi_atrous", ssgi_atrous_frag_shader_str);
}

ShaderProgram* create_ssr_atrous_program() {
    return create_post_program("ssr_atrous", ssr_atrous_frag_shader_str);
}

ShaderProgram* create_ssr_accum_program() {
    return create_post_program("ssr_accum", ssr_accum_frag_shader_str);
}

// The froxel fog trio (spec 9.5). All three are ordinary fullscreen passes --
// the volume is written one slice per draw, so they need no geometry shader and
// share the standard post vertex shader.
ShaderProgram* create_froxel_inject_program() {
    return create_post_program("froxel_inject", froxel_inject_frag_shader_str);
}

ShaderProgram* create_froxel_integrate_program() {
    return create_post_program("froxel_integrate", froxel_integrate_frag_shader_str);
}

ShaderProgram* create_froxel_composite_program() {
    return create_post_program("froxel_composite", froxel_composite_frag_shader_str);
}

ShaderProgram* create_fog_esm_program() {
    return create_post_program("fog_esm", fog_esm_frag_shader_str);
}

// GI probe projection (spec 9.7). Writes a sub-rectangle of the probe atlas
// picked by glViewport rather than a whole target, which is why it is an
// ordinary fullscreen-quad pass despite drawing a 10x10 tile.
ShaderProgram* create_gi_project_program() {
    return create_post_program("gi_project", gi_project_frag_shader_str);
}

ShaderProgram* create_motion_blur_program() {
    return create_post_program("motion_blur", motion_blur_frag_shader_str);
}

ShaderProgram* create_motion_blur_tilemax_program() {
    return create_post_program("motion_blur_tilemax", motion_blur_tilemax_frag_shader_str);
}

ShaderProgram* create_motion_blur_neighbormax_program() {
    return create_post_program("motion_blur_neighbormax", motion_blur_neighbormax_frag_shader_str);
}

ShaderProgram* create_sss_gather_program() {
    return create_post_program("sss_gather", sss_gather_frag_shader_str);
}

ShaderProgram* create_sss_pyr_seed_program() {
    return create_post_program("sss_pyr_seed", sss_pyr_seed_frag_shader_str);
}

ShaderProgram* create_sss_pyr_down_program() {
    return create_post_program("sss_pyr_down", sss_pyr_down_frag_shader_str);
}

ShaderProgram* create_contact_shadow_program() {
    return create_post_program("contact_shadow", contact_shadow_frag_shader_str);
}

ShaderProgram* create_oit_resolve_program() {
    return create_post_program("oit_resolve", oit_resolve_frag_shader_str);
}

ShaderProgram* create_lum_measure_program() {
    return create_post_program("lum_measure", lum_measure_frag_shader_str);
}

ShaderProgram* create_dof_coc_program() {
    return create_post_program("dof_coc", dof_coc_frag_shader_str);
}

ShaderProgram* create_dof_tile_program() {
    return create_post_program("dof_tile", dof_tile_frag_shader_str);
}

ShaderProgram* create_dof_dilate_program() {
    return create_post_program("dof_dilate", dof_dilate_frag_shader_str);
}

ShaderProgram* create_dof_gather_program() {
    return create_post_program("dof_gather", dof_gather_frag_shader_str);
}

ShaderProgram* create_dof_composite_program() {
    return create_post_program("dof_composite", dof_composite_frag_shader_str);
}
