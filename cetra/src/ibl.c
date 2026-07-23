#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <GL/glew.h>
#include <cglm/cglm.h>

#include "ibl.h"
#include "uniform.h"
#include "util.h"
#include "engine.h"
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

// Cubemap face view matrices looking out from origin
void ibl_capture_views(vec3 origin, mat4 views[6]) {
    vec3 targets[6] = {{1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                       {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f}};

    vec3 ups[6] = {{0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
                   {0.0f, 0.0f, -1.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}};

    for (int i = 0; i < 6; ++i) {
        vec3 center;
        glm_vec3_add(origin, targets[i], center);
        glm_lookat(origin, center, ups[i], views[i]);
    }
}

static void get_cubemap_projection(mat4 projection) {
    glm_perspective(glm_rad(90.0f), 1.0f, 0.1f, 10.0f, projection);
}

int ibl_init_cube_vao(IBLResources* ibl) {
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

    create_fullscreen_quad_vao(&ibl->quad_vao, &ibl->quad_vbo);
    return 0;
}

void ibl_render_unit_cube(IBLResources* ibl) {
    glBindVertexArray(ibl->cube_vao);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);
}

static void render_quad(IBLResources* ibl) {
    draw_fullscreen_quad(ibl->quad_vao);
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
    ibl->light_count = 0;
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

/*
 * Extract up to IBL_MAX_EXTRACTED_LIGHTS bright light lobes from an equirect
 * HDR by greedy angular clustering of the brightest pixels.
 *
 * Direction/uv mapping matches ibl_equirect_frag.glsl, with the image already
 * vertically flipped by stbi: row r maps to y = sin((v-0.5)*pi), column c to
 * azimuth phi = (u-0.5)*2*pi with x = cos(e)cos(phi), z = cos(e)sin(phi).
 * cos(e) weights for per-row solid angle.
 */
static void extract_light_lobes(IBLResources* ibl, const float* data, int width, int height) {
    const int stride = 4;
    const float lobe_cos = cosf(glm_rad(35.0f)); // Cluster radius: 35 degrees

    // Collect bright candidate pixels (top of the dynamic range)
    int rows = (height + stride - 1) / stride;
    int cols = (width + stride - 1) / stride;
    size_t max_candidates = (size_t)rows * (size_t)cols;
    float* cand = malloc(max_candidates * 4 * sizeof(float)); // x,y,z,energy
    if (!cand)
        return;

    float max_lum = 0.0f;
    double lum_sum = 0.0;
    size_t lum_samples = 0;
    for (int r = 0; r < height; r += stride) {
        for (int c = 0; c < width; c += stride) {
            const float* px = data + ((size_t)r * (size_t)width + (size_t)c) * 3;
            float lum = 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
            if (lum > max_lum)
                max_lum = lum;
            lum_sum += lum;
            lum_samples++;
        }
    }
    if (max_lum <= 0.0f || lum_samples == 0) {
        free(cand);
        return;
    }

    // Threshold relative to the MEAN: emitters sit far above it while lit
    // walls do not. A max-relative threshold fails here because a bare
    // bulb's hot core can be orders of magnitude brighter than a large
    // diffused source (softbox/umbrella) that carries comparable energy.
    float mean_lum = (float)(lum_sum / (double)lum_samples);
    float threshold = 5.0f * mean_lum;
    size_t count = 0;
    for (int r = 0; r < height; r += stride) {
        float v = ((float)r + 0.5f) / (float)height;
        float elev = (v - 0.5f) * (float)GLM_PI;
        float y = sinf(elev);
        float cos_e = cosf(elev);
        for (int c = 0; c < width; c += stride) {
            const float* px = data + ((size_t)r * (size_t)width + (size_t)c) * 3;
            float lum = 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
            if (lum < threshold)
                continue;
            float u = ((float)c + 0.5f) / (float)width;
            float phi = (u - 0.5f) * 2.0f * (float)GLM_PI;
            cand[count * 4 + 0] = cosf(phi) * cos_e;
            cand[count * 4 + 1] = y;
            cand[count * 4 + 2] = sinf(phi) * cos_e;
            cand[count * 4 + 3] = lum * cos_e;
            count++;
        }
    }

    // Greedy clustering: seed with the most energetic remaining pixel, absorb
    // everything within the lobe radius, repeat
    ibl->light_count = 0;
    float total_energy = 0.0f;
    while (ibl->light_count < IBL_MAX_EXTRACTED_LIGHTS) {
        int seed = -1;
        float seed_energy = 0.0f;
        for (size_t i = 0; i < count; i++) {
            if (cand[i * 4 + 3] > seed_energy) {
                seed_energy = cand[i * 4 + 3];
                seed = (int)i;
            }
        }
        if (seed < 0)
            break;

        vec3 seed_dir = {cand[seed * 4], cand[seed * 4 + 1], cand[seed * 4 + 2]};
        glm_vec3_normalize(seed_dir);

        vec3 sum = {0.0f, 0.0f, 0.0f};
        float energy = 0.0f;
        for (size_t i = 0; i < count; i++) {
            float e = cand[i * 4 + 3];
            if (e <= 0.0f)
                continue;
            vec3 d = {cand[i * 4], cand[i * 4 + 1], cand[i * 4 + 2]};
            glm_vec3_normalize(d);
            if (glm_vec3_dot(d, seed_dir) < lobe_cos)
                continue;
            glm_vec3_muladds(d, e, sum);
            energy += e;
            cand[i * 4 + 3] = 0.0f; // Absorbed
        }
        if (energy <= 0.0f || glm_vec3_norm(sum) < 1e-6f)
            break;

        // Ignore weak lobes (below 10% of the strongest)
        if (ibl->light_count > 0 && energy < 0.1f * ibl->light_energies[0])
            break;

        glm_vec3_normalize_to(sum, ibl->light_dirs[ibl->light_count]);
        ibl->light_energies[ibl->light_count] = energy;
        total_energy += energy;
        ibl->light_count++;
    }

    free(cand);

    for (int i = 0; i < ibl->light_count; i++) {
        ibl->light_energies[i] /= total_energy;
        log_info("HDR light %d: dir=(%.2f, %.2f, %.2f) energy=%.2f", i, ibl->light_dirs[i][0],
                 ibl->light_dirs[i][1], ibl->light_dirs[i][2], ibl->light_energies[i]);
    }
}

int load_hdr_environment(IBLResources* ibl, const char* hdr_path) {
    if (!ibl || !hdr_path)
        return -1;

    // Equirect HDRs are stored bottom-up relative to this engine's upload. The
    // flag is process-global in stb_image, so scope it to this one load: async
    // texture workers decode concurrently and must not inherit the flip.
    stbi_set_flip_vertically_on_load(1);

    int width, height, nrComponents;
    // Force 3 channels (RGB) to match GL_RGB format
    float* data = stbi_loadf(hdr_path, &width, &height, &nrComponents, 3);

    stbi_set_flip_vertically_on_load(0);

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

    extract_light_lobes(ibl, data, width, height);

    stbi_image_free(data);

    ibl->hdr_width = width;
    ibl->hdr_height = height;
    ibl->hdr_filepath = safe_strdup(hdr_path);
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

void ibl_create_cubemap_texture(GLuint* texture, int size, bool mipmap) {
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
    // The chain deliberately stops above 1x1; without an explicit max level
    // the texture is mipmap-incomplete and samples as black
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, num_mip_levels - 1);
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
        ibl_render_unit_cube(ibl);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void render_irradiance_convolution(IBLResources* ibl, mat4 projection, const mat4 views[6],
                                          int env_size) {
    ShaderProgram* program = ibl->irradiance_program;
    if (!program)
        return;

    glUseProgram(program->id);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, ibl->environment_cubemap);
    uniform_set_int(program->uniforms, "environmentMap", 0);
    uniform_set_mat4(program->uniforms, "projection", (float*)projection);
    // Integrate from the mip whose faces are ~64px: dense enough for the
    // convolution, coarse enough that small bright lights are pre-averaged in
    uniform_set_float(program->uniforms, "sampleMipLevel", log2f((float)env_size / 64.0f));

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
        ibl_render_unit_cube(ibl);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// GGX-prefilter an arbitrary cubemap into a manually-mipped destination it
// (re)allocates (roughness = mip / (mip_levels - 1)). Direction-only
// unit-cube render, so the source origin is irrelevant; the source face size
// drives the importance sampler's solid-angle mip selection. Uses the shared
// capture FBO/RBO and leaves FBO 0 bound; caller restores its own viewport.
void ibl_prefilter_cubemap(IBLResources* ibl, GLuint src_cube, GLuint* dst, int dst_base_size,
                           int mip_levels) {
    ShaderProgram* program = ibl->prefilter_program;
    if (!program)
        return;

    if (*dst)
        glDeleteTextures(1, dst);
    create_prefilter_cubemap(dst, dst_base_size, mip_levels);
    GLuint dst_cube = *dst;

    glUseProgram(program->id);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, src_cube);
    GLint src_resolution = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0, GL_TEXTURE_WIDTH, &src_resolution);
    uniform_set_int(program->uniforms, "environmentMap", 0);

    mat4 views[6];
    mat4 projection = {{0}};
    vec3 origin = {0.0f, 0.0f, 0.0f};
    ibl_capture_views(origin, views);
    get_cubemap_projection(projection);
    uniform_set_mat4(program->uniforms, "projection", (float*)projection);
    uniform_set_float(program->uniforms, "resolution", (float)src_resolution);

    GLboolean cull_was_enabled = glIsEnabled(GL_CULL_FACE);
    GLboolean blend_was_enabled = glIsEnabled(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    glBindFramebuffer(GL_FRAMEBUFFER, ibl->capture_fbo);

    for (int mip = 0; mip < mip_levels; ++mip) {
        int mip_size = dst_base_size >> mip;
        glBindRenderbuffer(GL_RENDERBUFFER, ibl->capture_rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, mip_size, mip_size);
        glViewport(0, 0, mip_size, mip_size);

        float roughness = (float)mip / (float)(mip_levels - 1);
        uniform_set_float(program->uniforms, "roughness", roughness);

        for (int i = 0; i < 6; ++i) {
            uniform_set_mat4(program->uniforms, "view", (float*)views[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, dst_cube, mip);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            ibl_render_unit_cube(ibl);
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (cull_was_enabled)
        glEnable(GL_CULL_FACE);
    if (blend_was_enabled)
        glEnable(GL_BLEND);
}

static void render_brdf_lut(IBLResources* ibl) {
    ShaderProgram* program = ibl->brdf_program;
    if (!program)
        return;

    // Create BRDF LUT texture (delete-before-gen keeps re-bakes leak-free)
    if (ibl->brdf_lut)
        glDeleteTextures(1, &ibl->brdf_lut);
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

// Run the environment-independent half of the IBL bake: irradiance
// convolution, GGX prefilter, and (once) the BRDF LUT, all derived from an
// already-populated, mipped ibl->environment_cubemap. env_size is that
// cubemap's face size (it drives the irradiance convolution's mip
// selection); prefilter_size/mips size the specular chain and set
// max_reflection_lod. Safe to call repeatedly (delete-before-gen
// throughout) -- this is the re-bake entry point for procedural
// environments whose content changes (a sky following its sun).
int ibl_bake_from_cubemap(IBLResources* ibl, Engine* engine, int env_size, int prefilter_size,
                          int prefilter_mips) {
    if (!ibl || !engine || ibl->environment_cubemap == 0) {
        log_error("Invalid IBL state for bake");
        return -1;
    }

    ibl->irradiance_program = get_engine_shader_program_by_name(engine, "ibl_irradiance");
    ibl->prefilter_program = get_engine_shader_program_by_name(engine, "ibl_prefilter");
    ibl->brdf_program = get_engine_shader_program_by_name(engine, "ibl_brdf");
    ibl->skybox_program = get_engine_shader_program_by_name(engine, "skybox");
    if (!ibl->irradiance_program || !ibl->prefilter_program || !ibl->brdf_program ||
        !ibl->skybox_program) {
        log_error("Failed to get IBL shader programs");
        return -1;
    }

    // Enable seamless cubemap sampling to avoid artifacts at face edges
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    setup_capture_fbo(ibl, env_size);
    ibl_init_cube_vao(ibl);
    init_quad_vao(ibl);

    mat4 capture_views[6];
    mat4 capture_projection = {{0}};
    vec3 capture_origin = {0.0f, 0.0f, 0.0f};
    ibl_capture_views(capture_origin, capture_views);
    get_cubemap_projection(capture_projection);

    GLint prev_viewport[4];
    GLint prev_framebuffer;
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_framebuffer);

    // Disable face culling for inside-cube rendering, and blending for every
    // capture pass: the BRDF LUT shader writes only RG, so with blending on
    // its undefined source alpha feeds the blend equation and the LUT comes
    // out different on every run
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    log_info("  Generating irradiance map...");
    if (ibl->irradiance_map)
        glDeleteTextures(1, &ibl->irradiance_map);
    ibl_create_cubemap_texture(&ibl->irradiance_map, IBL_IRRADIANCE_SIZE, false);
    render_irradiance_convolution(ibl, capture_projection, capture_views, env_size);

    log_info("  Generating prefiltered environment map...");
    ibl_prefilter_cubemap(ibl, ibl->environment_cubemap, &ibl->prefilter_map, prefilter_size,
                          prefilter_mips);
    ibl->max_reflection_lod = (float)(prefilter_mips - 1);

    // The BRDF LUT is environment-independent; bake it once and keep it
    if (ibl->brdf_lut == 0) {
        log_info("  Generating BRDF LUT...");
        render_brdf_lut(ibl);
    }

    // Re-enable face culling and the engine's default blending
    glEnable(GL_CULL_FACE);
    glEnable(GL_BLEND);

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
    return 0;
}

int precompute_ibl(IBLResources* ibl, Engine* engine) {
    if (!ibl || !engine || ibl->hdr_texture == 0) {
        log_error("Invalid IBL state for precomputation");
        return -1;
    }

    log_info("Starting IBL precomputation...");

    ibl->equirect_to_cubemap_program =
        get_engine_shader_program_by_name(engine, "ibl_equirect_to_cube");
    if (!ibl->equirect_to_cubemap_program) {
        log_error("Failed to get IBL shader programs");
        return -1;
    }

    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    setup_capture_fbo(ibl, IBL_CUBEMAP_SIZE);
    ibl_init_cube_vao(ibl);

    mat4 capture_views[6];
    mat4 capture_projection = {{0}};
    vec3 capture_origin = {0.0f, 0.0f, 0.0f};
    ibl_capture_views(capture_origin, capture_views);
    get_cubemap_projection(capture_projection);

    GLint prev_viewport[4];
    GLint prev_framebuffer;
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_framebuffer);

    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    // Convert equirectangular to cubemap. The mip chain is required by the
    // convolutions in the bake below: this HDR class concentrates its energy
    // in tiny, extremely bright light sources that sparse sampling of the
    // full-res faces would statistically miss (under-lighting every model
    // relative to the visible background); the mips pre-average that energy
    // so the convolution integrals actually see it.
    log_info("  Converting equirectangular to cubemap...");
    if (ibl->environment_cubemap)
        glDeleteTextures(1, &ibl->environment_cubemap);
    ibl_create_cubemap_texture(&ibl->environment_cubemap, IBL_CUBEMAP_SIZE, true);
    render_equirect_to_cubemap(ibl, capture_projection, capture_views);
    glBindTexture(GL_TEXTURE_CUBE_MAP, ibl->environment_cubemap);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);

    glEnable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_framebuffer);

    int rc = ibl_bake_from_cubemap(ibl, engine, IBL_CUBEMAP_SIZE, IBL_PREFILTER_SIZE,
                                   IBL_PREFILTER_MIP_LEVELS);
    if (rc == 0)
        log_info("IBL precomputation complete!");
    return rc;
}

static void draw_background_cube(IBLResources* ibl, GLuint cubemap, mat4 view, mat4 projection,
                                 float brightness, bool ground_projection, float gp_radius,
                                 float gp_height) {
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
    uniform_set_float(program->uniforms, "brightness", brightness);

    // Ground projection uniforms; camera world position from the inverse view
    mat4 view_inv;
    glm_mat4_inv(view, view_inv);
    vec3 cam_pos = {view_inv[3][0], view_inv[3][1], view_inv[3][2]};
    uniform_set_int(program->uniforms, "groundProjection", ground_projection ? 1 : 0);
    uniform_set_float(program->uniforms, "gpRadius", gp_radius);
    uniform_set_float(program->uniforms, "gpHeight", gp_height);
    uniform_set_float(program->uniforms, "gpFadeStart", SKYBOX_GP_FADE_START);
    uniform_set_vec3(program->uniforms, "camPos", cam_pos);

    glActiveTexture(GL_TEXTURE0 + IBL_SKYBOX_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cubemap);
    uniform_set_int(program->uniforms, "skyboxTex", IBL_SKYBOX_TEXTURE_UNIT);

    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE); // Don't write to depth buffer
    ibl_render_unit_cube(ibl);
    glDepthMask(GL_TRUE); // Restore depth writes
    glDepthFunc(GL_LESS);
}

void render_skybox(IBLResources* ibl, mat4 view, mat4 projection, float brightness,
                   bool ground_projection, float gp_radius, float gp_height) {
    if (!ibl || !ibl->precomputed || !ibl->skybox_program)
        return;
    draw_background_cube(ibl, ibl->environment_cubemap, view, projection, brightness,
                         ground_projection, gp_radius, gp_height);
}

void render_skybox_cubemap(IBLResources* ibl, GLuint cubemap, mat4 view, mat4 projection) {
    if (!ibl || !ibl->precomputed || !ibl->skybox_program || !cubemap)
        return;
    draw_background_cube(ibl, cubemap, view, projection, 1.0f, false, 0.0f, 0.0f);
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

    // Reset active texture unit
    glActiveTexture(GL_TEXTURE0);
}
