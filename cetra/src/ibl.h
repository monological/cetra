#ifndef _IBL_H_
#define _IBL_H_

#include <GL/glew.h>
#include <cglm/cglm.h>
#include <stdbool.h>

#include "program.h"

#define IBL_CUBEMAP_SIZE         1024
#define IBL_IRRADIANCE_SIZE      32
#define IBL_PREFILTER_SIZE       256
#define IBL_PREFILTER_MIP_LEVELS 6
#define IBL_BRDF_LUT_SIZE        512

#define IBL_IRRADIANCE_TEXTURE_UNIT 14
#define IBL_PREFILTER_TEXTURE_UNIT  15
#define IBL_BRDF_LUT_TEXTURE_UNIT   16
#define IBL_SKYBOX_TEXTURE_UNIT     17

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
void render_skybox(IBLResources* ibl, mat4 view, mat4 projection, float exposure);

// Binding for PBR rendering
void bind_ibl_textures(IBLResources* ibl, ShaderProgram* program);

#endif // _IBL_H_
