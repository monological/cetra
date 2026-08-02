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

// Relocated below 16 after the scalar-mask array freed the mid material units
// (see shadow.h). All engine sampler units now bind within
// GL_MAX_TEXTURE_IMAGE_UNITS (16); brdfLUT/skybox were previously at 16/17.
#define IBL_IRRADIANCE_TEXTURE_UNIT 11
#define IBL_PREFILTER_TEXTURE_UNIT  12
#define IBL_BRDF_LUT_TEXTURE_UNIT   13
#define IBL_SKYBOX_TEXTURE_UNIT     14
_Static_assert(IBL_SKYBOX_TEXTURE_UNIT < 16,
               "engine sampler units must stay within GL_MAX_TEXTURE_IMAGE_UNITS");

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

    // Precomputed IBL textures (the split-sum BRDF LUT is engine-owned --
    // ibl_bake_brdf_lut -- not part of any environment's resources)
    GLuint environment_cubemap; // GL_TEXTURE_CUBE_MAP (HDR converted)
    GLuint irradiance_map;      // GL_TEXTURE_CUBE_MAP (diffuse convolution)
    GLuint prefilter_map;       // GL_TEXTURE_CUBE_MAP with mipmaps (specular)

    // FBO for rendering to cubemap faces
    GLuint capture_fbo;
    GLuint capture_rbo;

    // Cube VAO for rendering
    GLuint cube_vao;
    GLuint cube_vbo;

    // Precomputation shader programs
    ShaderProgram* equirect_to_cubemap_program;
    ShaderProgram* irradiance_program;
    ShaderProgram* prefilter_program;
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

// The environment-independent half of the bake: irradiance + GGX prefilter
// from an already-populated, MIPPED environment_cubemap of face size
// env_size. Sets max_reflection_lod from prefilter_mips. Safe to call
// repeatedly (delete-before-gen) — the re-bake entry point for procedural
// environments whose content changes.
int ibl_bake_from_cubemap(IBLResources* ibl, struct Engine* engine, int env_size,
                          int prefilter_size, int prefilter_mips);

// Bake the split-sum BRDF tables (GGX A/B in RG, Charlie sheen directional
// albedo E in B) once and return the texture. Engine-owned: pure BRDF
// integration with no environment dependence, and the sheen albedo scaling
// reads E in scenes that never load one.
GLuint ibl_bake_brdf_lut(struct Engine* engine);

// Skybox rendering
void render_skybox(IBLResources* ibl, mat4 view, mat4 projection, float brightness,
                   bool ground_projection, float gp_radius, float gp_height);

// Draw an arbitrary cubemap as the background with the skybox machinery
// (no ground projection, unit brightness) — the probe debug view
void render_skybox_cubemap(IBLResources* ibl, GLuint cubemap, mat4 view, mat4 projection);

// Binding for PBR rendering
void bind_ibl_textures(IBLResources* ibl, ShaderProgram* program);

// Cubemap capture toolkit (shared with reflection probes and the sky)

// Six cubemap-face view matrices looking out from origin
void ibl_capture_views(vec3 origin, mat4 views[6]);

// Lazily create the shared position-only unit-cube VAO (no-op if present) and
// draw it (36 verts, culling-agnostic) — reused for cubemap-face and skybox
// draws so callers need not carry their own cube.
int ibl_init_cube_vao(IBLResources* ibl);
void ibl_render_unit_cube(IBLResources* ibl);

// Allocate an RGB16F cubemap (optionally mip-filtered; mips are not generated)
void ibl_create_cubemap_texture(GLuint* texture, int size, bool mipmap);

// GGX-prefilter src_cube (which must carry a full mip chain) into *dst:
// (re)allocates *dst with mip_levels manually-sized levels from
// dst_base_size down, roughness = mip / (mip_levels - 1). Requires
// precompute_ibl to have run (shares its program and capture FBO).
void ibl_prefilter_cubemap(IBLResources* ibl, GLuint src_cube, GLuint* dst, int dst_base_size,
                           int mip_levels);

#endif // _IBL_H_
