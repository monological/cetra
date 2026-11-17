#ifndef _POSTFX_PROGRAMS_H_
#define _POSTFX_PROGRAMS_H_

#include <GL/glew.h>
#include <stdbool.h>

#include "glare.h"
#include "local_exposure.h"
#include "program.h"

// DoF tile size (half-res texels per tile texel): the tile pass reduces the
// signed CoC to per-tile far/near maxima that set the gather's kernel radius.
// Uploaded as the tileSize uniform to dof_tile and dof_gather.
#define DOF_TILE 4

// The post chain's shader programs and its constant inputs, made once per engine and shared by
// every chain the engine runs. No size lives on a program: whatever depends on a target's size is
// uploaded at the draw, so one program serves chains of any size.
typedef struct PostFXPrograms {
    ShaderProgram* bloom_bright;
    ShaderProgram* bloom_down; // pyramid 13-tap downsample
    ShaderProgram* bloom_up;   // pyramid tent upsample (additive)
    ShaderProgram* flare;
    ShaderProgram* tonemap;
    ShaderProgram* spec_occ_composite;
    ShaderProgram* gtao;
    ShaderProgram* ssao_blur;
    ShaderProgram* contact_shadow; // AO-res depth march toward the key light (spec 9.3)
    ShaderProgram* temporal_accum; // shared plain-RGBA accumulator (AO .r, fog .rgba)
    ShaderProgram* ssgi_composite;
    ShaderProgram* ssgi_accum;
    ShaderProgram* ssgi_atrous;
    ShaderProgram* ssr_atrous; // edge-aware a-trous denoise for the SSR reflection buffer
    ShaderProgram* ssr_accum;  // SSR's own accumulator: inverse-luma blend (10.7.2)
    ShaderProgram* lum_measure;
    ShaderProgram* lum_histogram;
    ShaderProgram* lum_reduce;
    ShaderProgram* ssr;
    ShaderProgram* ssr_hiz;
    ShaderProgram* upsample_tent;     // shared tent composite (bloom mips, SSR)
    ShaderProgram* ssr_fold_wet;      // SSR's fold when ground is wet (spec 13.9); NULL = the tent
    ShaderProgram* froxel_inject;     // per-cell scattering into the volume (spec 9.5)
    ShaderProgram* froxel_miss_count; // postfx_programs_ensure_miss_count
    bool froxel_miss_count_failed;    // that count could not be made; never retried
    ShaderProgram* froxel_integrate;  // front-to-back gather along each slice column
    ShaderProgram* froxel_composite;
    ShaderProgram* fog_esm; // the fog's exponential shadow array from the depth cascades
    ShaderProgram* taa_resolve;
    ShaderProgram* taau_resolve; // postfx_programs_ensure_taau
    ShaderProgram* dof_coc;
    ShaderProgram* dof_tile;
    ShaderProgram* dof_dilate;
    ShaderProgram* dof_gather;
    ShaderProgram* dof_composite;
    ShaderProgram* motion_blur;
    ShaderProgram* motion_blur_tilemax;
    ShaderProgram* motion_blur_neighbormax;
    ShaderProgram* sss_gather;
    ShaderProgram* sss_pyr_seed;
    ShaderProgram* sss_pyr_down;
    ShaderProgram* oit_resolve;
    ShaderProgram* present; // a picture copied 1:1 into a framebuffer, the window's dithered
    GlarePrograms glare;    // postfx_programs_ensure_glare
    bool glare_failed;      // could not be made; never retried
    LocalExposurePrograms local_exposure; // postfx_programs_ensure_local_exposure
    bool local_exposure_failed;           // could not be made; never retried

    GLuint noise_texture; // 4x4 random slice rotations, tiled
    // 1x1 white R16F, for a sampler whose real texture is absent this frame. Binding name 0
    // instead leaves an INCOMPLETE texture on a live sampler, which this driver substitutes
    // for per draw at a measurable cost -- so the stand-in is cheaper than the nothing it
    // stands in for. White because every current consumer reads it as a transmittance.
    GLuint white_tex;
    GLuint quad_vao;
    GLuint quad_vbo;
} PostFXPrograms;

// Compiles every program and seeds its sampler units. NULL, logged, when one cannot be made.
PostFXPrograms* create_postfx_programs(void);
void free_postfx_programs(PostFXPrograms* programs);

// The TAAU resolve, compiled the first time a chain runs below render scale 1 and kept after.
// False, logged, when it cannot be made.
bool postfx_programs_ensure_taau(PostFXPrograms* programs);
// The fog-miss probe's count, made on first use; NULL, logged once, when it cannot be.
ShaderProgram* postfx_programs_ensure_miss_count(PostFXPrograms* programs);
// Glare's and local exposure's programs, made the first time a chain wants them and kept after;
// NULL, logged once, when they cannot be.
const GlarePrograms* postfx_programs_ensure_glare(PostFXPrograms* programs);
const LocalExposurePrograms* postfx_programs_ensure_local_exposure(PostFXPrograms* programs);

#endif // _POSTFX_PROGRAMS_H_
