#ifndef _IBL_H_
#define _IBL_H_

#include <GL/glew.h>
#include <cglm/cglm.h>
#include <stdbool.h>

#include "program.h"

#define IBL_CUBEMAP_SIZE         2048
#define IBL_IRRADIANCE_SIZE      32
#define IBL_PREFILTER_SIZE       1024
#define IBL_PREFILTER_MIP_LEVELS 9
#define IBL_BRDF_LUT_SIZE        512

#define IBL_IRRADIANCE_TEXTURE_UNIT 14
#define IBL_PREFILTER_TEXTURE_UNIT  15
#define IBL_BRDF_LUT_TEXTURE_UNIT   16
#define IBL_SKYBOX_TEXTURE_UNIT     17

// Max bright light lobes extracted from an HDR (matches MAX_SHADOW_LIGHTS)
#define IBL_MAX_EXTRACTED_LIGHTS 3

// Fraction of the ground-projection dome radius where the projection starts
// fading to the infinite skybox. Fed to skybox_frag.glsl as gpFadeStart;
// apps clamp camera distance to this so no reachable view shows the blend.
#define SKYBOX_GP_FADE_START 0.7f

// Forward declarations
struct Engine;

typedef struct IBLResources {
    // Source HDR environment
    GLuint hdr_texture;
    int hdr_width;
    int hdr_height;
    char* hdr_filepath;

    // Precomputed IBL textures
    GLuint environment_cubemap; // GL_TEXTURE_CUBE_MAP (HDR converted)
    GLuint irradiance_map;      // GL_TEXTURE_CUBE_MAP (diffuse convolution)
    GLuint prefilter_map;       // GL_TEXTURE_CUBE_MAP with mipmaps (specular)
    GLuint brdf_lut;            // GL_TEXTURE_2D (BRDF integration LUT)

    // FBO for rendering to cubemap faces
    GLuint capture_fbo;
    GLuint capture_rbo;

    // Cube VAO for rendering
    GLuint cube_vao;
    GLuint cube_vbo;

    // Quad VAO for BRDF LUT
    GLuint quad_vao;
    GLuint quad_vbo;

    // Precomputation shader programs
    ShaderProgram* equirect_to_cubemap_program;
    ShaderProgram* irradiance_program;
    ShaderProgram* prefilter_program;
    ShaderProgram* brdf_program;
    ShaderProgram* skybox_program;

    // Parameters
    float intensity;
    float max_reflection_lod;

    // Bright light lobes extracted from the environment during HDR load
    // (for aiming analytic shadow-casting lights). Ordered by energy,
    // energies normalized so they sum to 1.
    vec3 light_dirs[IBL_MAX_EXTRACTED_LIGHTS];
    float light_energies[IBL_MAX_EXTRACTED_LIGHTS];
    int light_count;

    // State
    bool initialized;
    bool precomputed;
} IBLResources;

// Creation and destruction
IBLResources* create_ibl_resources(void);
void free_ibl_resources(IBLResources* ibl);

// HDR loading
int load_hdr_environment(IBLResources* ibl, const char* hdr_path);

// Precomputation (run once after HDR load)
int precompute_ibl(IBLResources* ibl, struct Engine* engine);

// Skybox rendering
void render_skybox(IBLResources* ibl, mat4 view, mat4 projection, float brightness,
                   bool ground_projection, float gp_radius, float gp_height);

// Binding for PBR rendering
void bind_ibl_textures(IBLResources* ibl, ShaderProgram* program);

// Cubemap capture toolkit (shared with reflection probes)

// Six cubemap-face view matrices looking out from origin
void ibl_capture_views(vec3 origin, mat4 views[6]);

// Allocate an RGB16F cubemap (optionally mip-filtered; mips are not generated)
void ibl_create_cubemap_texture(GLuint* texture, int size, bool mipmap);

// Allocate an RGB16F cubemap with num_mip_levels manually-sized mip levels
// (render target for per-mip prefiltering)
void ibl_create_prefilter_cubemap(GLuint* texture, int size, int num_mip_levels);

// GGX-prefilter src_cube (with full mip chain, src_resolution = face size)
// into dst_cube's mip_levels levels, roughness = mip / (mip_levels - 1).
// Requires precompute_ibl to have run (shares its program and capture FBO).
void ibl_prefilter_cubemap(IBLResources* ibl, GLuint src_cube, float src_resolution,
                           GLuint dst_cube, int dst_base_size, int mip_levels);

#endif // _IBL_H_
