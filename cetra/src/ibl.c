#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <GL/glew.h>
#include <cglm/cglm.h>

#include "ibl.h"
#include "uniform.h"
#include "engine.h"
#include "shader_strings.h"
#include "ext/log.h"

#define STB_IMAGE_IMPLEMENTATION_ALREADY_DONE
#include "ext/stb_image.h"

// Unit cube vertices for skybox and cubemap face rendering
static const float cube_vertices[] = {
    // positions
    -1.0f, 1.0f,  -1.0f, -1.0f, -1.0f, -1.0f, 1.0f,  -1.0f, -1.0f,
    1.0f,  -1.0f, -1.0f, 1.0f,  1.0f,  -1.0f, -1.0f, 1.0f,  -1.0f,

    -1.0f, -1.0f, 1.0f,  -1.0f, -1.0f, -1.0f, -1.0f, 1.0f,  -1.0f,
    -1.0f, 1.0f,  -1.0f, -1.0f, 1.0f,  1.0f,  -1.0f, -1.0f, 1.0f,

    1.0f,  -1.0f, -1.0f, 1.0f,  -1.0f, 1.0f,  1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  -1.0f, 1.0f,  -1.0f, -1.0f,

    -1.0f, -1.0f, 1.0f,  -1.0f, 1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  1.0f,  -1.0f, 1.0f,  -1.0f, -1.0f, 1.0f,

    -1.0f, 1.0f,  -1.0f, 1.0f,  1.0f,  -1.0f, 1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  -1.0f, 1.0f,  1.0f,  -1.0f, 1.0f,  -1.0f,

    -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 1.0f,  1.0f,  -1.0f, -1.0f,
    1.0f,  -1.0f, -1.0f, -1.0f, -1.0f, 1.0f,  1.0f,  -1.0f, 1.0f};

// Fullscreen quad for BRDF LUT rendering
static const float quad_vertices[] = {
    // positions        // texCoords
    -1.0f, 1.0f, 0.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 0.0f,
    1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 1.0f,  -1.0f, 0.0f, 1.0f, 0.0f,
};

// Cubemap face view matrices
static void get_cubemap_view_matrices(mat4 views[6]) {
    vec3 targets[6] = {{1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                       {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f}};

    vec3 ups[6] = {{0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
                   {0.0f, 0.0f, -1.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}};

    vec3 origin = {0.0f, 0.0f, 0.0f};

    for (int i = 0; i < 6; ++i) {
        glm_lookat(origin, targets[i], ups[i], views[i]);
    }
}

static void get_cubemap_projection(mat4 projection) {
    glm_perspective(glm_rad(90.0f), 1.0f, 0.1f, 10.0f, projection);
}

static int init_cube_vao(IBLResources* ibl) {
    if (ibl->cube_vao != 0)
        return 0;

    glGenVertexArrays(1, &ibl->cube_vao);
    glGenBuffers(1, &ibl->cube_vbo);

    glBindVertexArray(ibl->cube_vao);
    glBindBuffer(GL_ARRAY_BUFFER, ibl->cube_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cube_vertices), cube_vertices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

    glBindVertexArray(0);
    return 0;
}

static int init_quad_vao(IBLResources* ibl) {
    if (ibl->quad_vao != 0)
        return 0;

    glGenVertexArrays(1, &ibl->quad_vao);
    glGenBuffers(1, &ibl->quad_vbo);

    glBindVertexArray(ibl->quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, ibl->quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));

    glBindVertexArray(0);
    return 0;
}

static void render_cube(IBLResources* ibl) {
    glBindVertexArray(ibl->cube_vao);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);
}

static void render_quad(IBLResources* ibl) {
    glBindVertexArray(ibl->quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

IBLResources* create_ibl_resources(void) {
    IBLResources* ibl = malloc(sizeof(IBLResources));
    if (!ibl) {
        log_error("Failed to allocate IBL resources");
        return NULL;
    }
    memset(ibl, 0, sizeof(IBLResources));

    ibl->intensity = 1.0f;
    ibl->max_reflection_lod = (float)(IBL_PREFILTER_MIP_LEVELS - 1);
    ibl->initialized = false;
    ibl->precomputed = false;

    return ibl;
}

void free_ibl_resources(IBLResources* ibl) {
    if (!ibl)
        return;

    if (ibl->hdr_texture)
        glDeleteTextures(1, &ibl->hdr_texture);
    if (ibl->environment_cubemap)
        glDeleteTextures(1, &ibl->environment_cubemap);
    if (ibl->irradiance_map)
        glDeleteTextures(1, &ibl->irradiance_map);
    if (ibl->prefilter_map)
        glDeleteTextures(1, &ibl->prefilter_map);
    if (ibl->brdf_lut)
        glDeleteTextures(1, &ibl->brdf_lut);

    if (ibl->capture_fbo)
        glDeleteFramebuffers(1, &ibl->capture_fbo);
    if (ibl->capture_rbo)
        glDeleteRenderbuffers(1, &ibl->capture_rbo);

    if (ibl->cube_vao)
        glDeleteVertexArrays(1, &ibl->cube_vao);
    if (ibl->cube_vbo)
        glDeleteBuffers(1, &ibl->cube_vbo);
    if (ibl->quad_vao)
        glDeleteVertexArrays(1, &ibl->quad_vao);
    if (ibl->quad_vbo)
        glDeleteBuffers(1, &ibl->quad_vbo);

    if (ibl->hdr_filepath)
        free(ibl->hdr_filepath);

    free(ibl);
}

int load_hdr_environment(IBLResources* ibl, const char* hdr_path) {
    if (!ibl || !hdr_path)
        return -1;

    stbi_set_flip_vertically_on_load(1);

    int width, height, nrComponents;
    // Force 3 channels (RGB) to match GL_RGB format
    float* data = stbi_loadf(hdr_path, &width, &height, &nrComponents, 3);

    if (!data) {
        log_error("Failed to load HDR image: %s", hdr_path);
        return -1;
    }

    log_info("Loaded HDR: %s (%dx%d, requested 3 channels)", hdr_path, width, height);

    glGenTextures(1, &ibl->hdr_texture);
    glBindTexture(GL_TEXTURE_2D, ibl->hdr_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_FLOAT, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    stbi_image_free(data);

    ibl->hdr_width = width;
    ibl->hdr_height = height;
    ibl->hdr_filepath = strdup(hdr_path);
    ibl->initialized = true;

    return 0;
}

static int setup_capture_fbo(IBLResources* ibl, int size) {
    if (ibl->capture_fbo == 0) {
        glGenFramebuffers(1, &ibl->capture_fbo);
    }
    if (ibl->capture_rbo == 0) {
        glGenRenderbuffers(1, &ibl->capture_rbo);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, ibl->capture_fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, ibl->capture_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size, size);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              ibl->capture_rbo);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;
}

static void create_cubemap_texture(GLuint* texture, int size, bool mipmap) {
    glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_CUBE_MAP, *texture);

    for (int i = 0; i < 6; ++i) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGB16F, size, size, 0, GL_RGB,
                     GL_FLOAT, NULL);
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    if (mipmap) {
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        // Don't call glGenerateMipmap here - we'll render to each mip level manually
    } else {
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

// Create prefilter cubemap with manually allocated mip levels
static void create_prefilter_cubemap(GLuint* texture, int size, int num_mip_levels) {
    glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_CUBE_MAP, *texture);

    // Allocate storage for each mip level and face
    for (int mip = 0; mip < num_mip_levels; ++mip) {
        int mip_size = size >> mip;
        for (int face = 0; face < 6; ++face) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, mip, GL_RGB16F, mip_size, mip_size,
                         0, GL_RGB, GL_FLOAT, NULL);
        }
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

static void render_equirect_to_cubemap(IBLResources* ibl, mat4 projection, const mat4 views[6]) {
    ShaderProgram* program = ibl->equirect_to_cubemap_program;
    if (!program)
        return;

    glUseProgram(program->id);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ibl->hdr_texture);
    uniform_set_int(program->uniforms, "equirectangularMap", 0);
    uniform_set_mat4(program->uniforms, "projection", (float*)projection);

    glViewport(0, 0, IBL_CUBEMAP_SIZE, IBL_CUBEMAP_SIZE);
    glBindFramebuffer(GL_FRAMEBUFFER, ibl->capture_fbo);

    for (int i = 0; i < 6; ++i) {
        uniform_set_mat4(program->uniforms, "view", (float*)views[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, ibl->environment_cubemap, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        render_cube(ibl);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void render_irradiance_convolution(IBLResources* ibl, mat4 projection, const mat4 views[6]) {
    ShaderProgram* program = ibl->irradiance_program;
    if (!program)
        return;

    glUseProgram(program->id);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, ibl->environment_cubemap);
    uniform_set_int(program->uniforms, "environmentMap", 0);
    uniform_set_mat4(program->uniforms, "projection", (float*)projection);

    glViewport(0, 0, IBL_IRRADIANCE_SIZE, IBL_IRRADIANCE_SIZE);
    glBindFramebuffer(GL_FRAMEBUFFER, ibl->capture_fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, ibl->capture_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, IBL_IRRADIANCE_SIZE,
                          IBL_IRRADIANCE_SIZE);

    for (int i = 0; i < 6; ++i) {
        uniform_set_mat4(program->uniforms, "view", (float*)views[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, ibl->irradiance_map, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        render_cube(ibl);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void render_prefilter_convolution(IBLResources* ibl, mat4 projection, const mat4 views[6]) {
    ShaderProgram* program = ibl->prefilter_program;
    if (!program)
        return;

    glUseProgram(program->id);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, ibl->environment_cubemap);
    uniform_set_int(program->uniforms, "environmentMap", 0);
    uniform_set_mat4(program->uniforms, "projection", (float*)projection);

    glBindFramebuffer(GL_FRAMEBUFFER, ibl->capture_fbo);

    for (int mip = 0; mip < IBL_PREFILTER_MIP_LEVELS; ++mip) {
        int mip_size = IBL_PREFILTER_SIZE >> mip;
        glBindRenderbuffer(GL_RENDERBUFFER, ibl->capture_rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, mip_size, mip_size);
        glViewport(0, 0, mip_size, mip_size);

        float roughness = (float)mip / (float)(IBL_PREFILTER_MIP_LEVELS - 1);
        uniform_set_float(program->uniforms, "roughness", roughness);

        for (int i = 0; i < 6; ++i) {
            uniform_set_mat4(program->uniforms, "view", (float*)views[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, ibl->prefilter_map, mip);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            render_cube(ibl);
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void render_brdf_lut(IBLResources* ibl) {
    ShaderProgram* program = ibl->brdf_program;
    if (!program)
        return;

    // Create BRDF LUT texture
    glGenTextures(1, &ibl->brdf_lut);
    glBindTexture(GL_TEXTURE_2D, ibl->brdf_lut);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, IBL_BRDF_LUT_SIZE, IBL_BRDF_LUT_SIZE, 0, GL_RG,
                 GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Setup FBO for BRDF rendering
    GLuint brdf_fbo;
    glGenFramebuffers(1, &brdf_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, brdf_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ibl->brdf_lut, 0);

    glViewport(0, 0, IBL_BRDF_LUT_SIZE, IBL_BRDF_LUT_SIZE);
    glUseProgram(program->id);
    glClear(GL_COLOR_BUFFER_BIT);

    render_quad(ibl);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &brdf_fbo);
}

int precompute_ibl(IBLResources* ibl, Engine* engine) {
    if (!ibl || !engine || ibl->hdr_texture == 0) {
        log_error("Invalid IBL state for precomputation");
        return -1;
    }

    log_info("Starting IBL precomputation...");

    // Get or create shader programs
    ibl->equirect_to_cubemap_program =
        get_engine_shader_program_by_name(engine, "ibl_equirect_to_cube");
    ibl->irradiance_program = get_engine_shader_program_by_name(engine, "ibl_irradiance");
    ibl->prefilter_program = get_engine_shader_program_by_name(engine, "ibl_prefilter");
    ibl->brdf_program = get_engine_shader_program_by_name(engine, "ibl_brdf");
    ibl->skybox_program = get_engine_shader_program_by_name(engine, "skybox");

    if (!ibl->equirect_to_cubemap_program || !ibl->irradiance_program || !ibl->prefilter_program ||
        !ibl->brdf_program || !ibl->skybox_program) {
        log_error("Failed to get IBL shader programs");
        return -1;
    }

    // Enable seamless cubemap sampling to avoid artifacts at face edges
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    // Setup FBO and VAOs
    setup_capture_fbo(ibl, IBL_CUBEMAP_SIZE);
    init_cube_vao(ibl);
    init_quad_vao(ibl);

    // Get view matrices
    mat4 capture_views[6];
    mat4 capture_projection = {{0}};
    get_cubemap_view_matrices(capture_views);
    get_cubemap_projection(capture_projection);

    // Save current viewport and framebuffer
    GLint prev_viewport[4];
    GLint prev_framebuffer;
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_framebuffer);

    // Disable face culling for inside-cube rendering
    glDisable(GL_CULL_FACE);

    // Step 1: Convert equirectangular to cubemap
    log_info("  Converting equirectangular to cubemap...");
    create_cubemap_texture(&ibl->environment_cubemap, IBL_CUBEMAP_SIZE, false);
    render_equirect_to_cubemap(ibl, capture_projection, capture_views);

    // Step 2: Generate irradiance map
    log_info("  Generating irradiance map...");
    create_cubemap_texture(&ibl->irradiance_map, IBL_IRRADIANCE_SIZE, false);
    render_irradiance_convolution(ibl, capture_projection, capture_views);

    // Step 3: Generate prefiltered map with mipmaps
    log_info("  Generating prefiltered environment map...");
    create_prefilter_cubemap(&ibl->prefilter_map, IBL_PREFILTER_SIZE, IBL_PREFILTER_MIP_LEVELS);
    render_prefilter_convolution(ibl, capture_projection, capture_views);

    // Step 4: Generate BRDF LUT
    log_info("  Generating BRDF LUT...");
    render_brdf_lut(ibl);

    // Re-enable face culling
    glEnable(GL_CULL_FACE);

    // Restore viewport and framebuffer
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_framebuffer);

    // Reset GL state to avoid polluting subsequent rendering
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);

    ibl->precomputed = true;
    log_info("IBL precomputation complete!");

    return 0;
}

void render_skybox(IBLResources* ibl, mat4 view, mat4 projection, float exposure) {
    if (!ibl || !ibl->precomputed || !ibl->skybox_program)
        return;

    ShaderProgram* program = ibl->skybox_program;
    glUseProgram(program->id);

    // Remove translation from view matrix
    mat4 view_no_translation;
    glm_mat4_copy(view, view_no_translation);
    view_no_translation[3][0] = 0.0f;
    view_no_translation[3][1] = 0.0f;
    view_no_translation[3][2] = 0.0f;

    uniform_set_mat4(program->uniforms, "view", (float*)view_no_translation);
    uniform_set_mat4(program->uniforms, "projection", (float*)projection);
    uniform_set_float(program->uniforms, "exposure", exposure);

    glActiveTexture(GL_TEXTURE0 + IBL_SKYBOX_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_CUBE_MAP, ibl->environment_cubemap);
    uniform_set_int(program->uniforms, "skyboxTex", IBL_SKYBOX_TEXTURE_UNIT);

    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE); // Don't write to depth buffer
    render_cube(ibl);
    glDepthMask(GL_TRUE); // Restore depth writes
    glDepthFunc(GL_LESS);
}

void bind_ibl_textures(IBLResources* ibl, ShaderProgram* program) {
    if (!ibl || !ibl->precomputed || !program || !program->uniforms)
        return;

    UniformManager* u = program->uniforms;

    // Bind irradiance map
    glActiveTexture(GL_TEXTURE0 + IBL_IRRADIANCE_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_CUBE_MAP, ibl->irradiance_map);
    uniform_set_int(u, "irradianceMap", IBL_IRRADIANCE_TEXTURE_UNIT);

    // Bind prefiltered environment map
    glActiveTexture(GL_TEXTURE0 + IBL_PREFILTER_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_CUBE_MAP, ibl->prefilter_map);
    uniform_set_int(u, "prefilteredMap", IBL_PREFILTER_TEXTURE_UNIT);

    // Bind BRDF LUT
    glActiveTexture(GL_TEXTURE0 + IBL_BRDF_LUT_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D, ibl->brdf_lut);
    uniform_set_int(u, "brdfLUT", IBL_BRDF_LUT_TEXTURE_UNIT);

    // Set IBL parameters
    uniform_set_int(u, "iblEnabled", 1);
    uniform_set_float(u, "iblIntensity", ibl->intensity);
    uniform_set_float(u, "maxReflectionLOD", ibl->max_reflection_lod);
}
