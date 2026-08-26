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
// The sheen (Charlie) chain is blur-dominated at every roughness -- the
// kernel is a wide grazing ring -- so a small chain loses nothing even
// against the 1024 GGX chain.
#define IBL_CHARLIE_PREFILTER_SIZE 256
#define IBL_CHARLIE_PREFILTER_MIPS 6

// Relocated below 16 after the scalar-mask array freed the mid material units
// (see shadow.h). All engine sampler units now bind within
// GL_MAX_TEXTURE_IMAGE_UNITS (16); brdfLUT/skybox were previously at 16/17.
#define IBL_IRRADIANCE_TEXTURE_UNIT 11
#define IBL_PREFILTER_TEXTURE_UNIT  12
#define IBL_BRDF_LUT_TEXTURE_UNIT   13
#define IBL_SKYBOX_TEXTURE_UNIT     14
// Charlie sheen environment chain, on the unit the LTC pack freed (10.7.1).
// Deliberately NOT probe-overridden: probes carry no Charlie chain, so sheen
// always reads the global environment's.
#define IBL_CHARLIE_TEXTURE_UNIT 9
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

    // Precomputed IBL textures (the BRDF LUT is engine-owned, not
    // per-environment)
    GLuint environment_cubemap;   // GL_TEXTURE_CUBE_MAP (HDR converted)
    GLuint irradiance_map;        // GL_TEXTURE_CUBE_MAP (diffuse convolution)
    GLuint prefilter_map;         // GL_TEXTURE_CUBE_MAP with mipmaps (GGX specular)
    GLuint charlie_prefilter_map; // GL_TEXTURE_CUBE_MAP with mipmaps (sheen env)

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
    ShaderProgram* charlie_prefilter_program;
    ShaderProgram* skybox_program;

    // Parameters
    float intensity;
    float max_reflection_lod;

    /*
     * Solid-angle-weighted mean radiance of the environment's UPPER hemisphere, in
     * absolute scene radiance and before `intensity` (spec 11.84).
     *
     * The CPU twin of `textureLod(prefilteredMap, +Y, maxReflectionLOD)` -- what a shader
     * reads when it wants "ambient from above" -- for the passes that need that number
     * without a sampler. Computed from the equirect already in memory during HDR load, so
     * it costs one pass over a buffer that is in cache rather than a glGetTexImage: a
     * readback at bake time would stall every frame under --day-cycle, where the env
     * re-bakes continuously.
     *
     * Zero on the procedural-sky path, which never loads an equirect. That path has its
     * own cheaper answer in SkyAtmosphere.zenith_radiance, and a consumer picks whichever
     * exists -- the two are different approximations of the same quantity.
     */
    vec3 ambient_up;

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

// The environment-independent half of the bake: irradiance, GGX prefilter,
// and the Charlie sheen chain, all from an already-populated, MIPPED
// environment_cubemap of face size env_size. Sets max_reflection_lod from
// prefilter_mips. Safe to call repeatedly (delete-before-gen) — the re-bake
// entry point for procedural environments whose content changes.
int ibl_bake_from_cubemap(IBLResources* ibl, struct Engine* engine, int env_size,
                          int prefilter_size, int prefilter_mips);

// Bake the split-sum BRDF tables (GGX A/B in RG, Charlie sheen E in blue)
// once; the caller owns the returned texture.
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

// Prefilter src_cube (which must carry a full mip chain) into *dst with the
// given filter program (the GGX "ibl_prefilter" or the Charlie
// "ibl_charlie_prefilter" -- both share the environmentMap/roughness/
// resolution contract): (re)allocates *dst with mip_levels manually-sized
// levels from dst_base_size down, roughness = mip / (mip_levels - 1).
// Requires precompute_ibl to have run (shares its capture FBO).
void ibl_prefilter_cubemap(IBLResources* ibl, ShaderProgram* program, GLuint src_cube, GLuint* dst,
                           int dst_base_size, int mip_levels);

// The 90-degree frustum every cube-face draw shares -- the sky's env faces
// and the prefilter integrating over them must agree on it.
void ibl_cubemap_projection(mat4 projection);

// Allocate a manually-mipped prefilter destination (no framebuffer involved);
// the two slices below fill one already allocated.
void ibl_create_prefilter_cubemap(GLuint* texture, int size, int num_mip_levels);

// The resumable pieces the atomic bakes are drivers over (spec 11.81): each
// re-establishes its own bindings, uniforms, viewport, cull/blend and the
// capture RBO's storage per call, so the day/night slicer can run one in
// isolation frames after any other and an atomic caller running them back to
// back executes the same arithmetic. Both leave FBO 0 bound and expect the
// shared capture FBO to keep its depth attachment (setup_capture_fbo or
// sky_env_face_slice makes it; nothing detaches it). Callers restore their
// own viewport.
void ibl_irradiance_slice(IBLResources* ibl, GLuint src_cube, GLuint dst_cube, int env_size);
void ibl_prefilter_slice(IBLResources* ibl, ShaderProgram* program, GLuint src_cube,
                         GLuint dst_cube, int dst_base_size, int mip_levels, int mip,
                         int face_first, int face_count);

#endif // _IBL_H_
