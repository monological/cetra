#include <stdlib.h>

#include "postfx_programs.h"
#include "uniform.h"
#include "util.h"

#include "ext/log.h"

// Deterministic PRNG (xorshift32): the AO rotation noise must be
// bit-identical across runs so headless screenshots stay comparable
static float prng_float(unsigned int* state) {
    unsigned int x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return (float)(x & 0xFFFFFF) / (float)0x1000000; // [0, 1)
}

static GLuint create_ssao_noise_texture(unsigned int* rng) {
    // 4x4 random rotation vectors in the XY plane, tiled across the screen
    float noise[16 * 2];
    for (int i = 0; i < 16; i++) {
        noise[i * 2] = prng_float(rng) * 2.0f - 1.0f;
        noise[i * 2 + 1] = prng_float(rng) * 2.0f - 1.0f;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, 4, 4, 0, GL_RG, GL_FLOAT, noise);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return texture;
}

// The units froxel_inject_frag's samplers read, for either of its two programs.
static void set_inject_samplers(ShaderProgram* program) {
    glUseProgram(program->id);
    uniform_set_int(program->uniforms, "shadowMaps", 1);
    uniform_set_int(program->uniforms, "punctualShadowMaps", 2);
    uniform_set_int(program->uniforms, "historyVolume", 3);
    uniform_set_int(program->uniforms, "cloudShadowTex", 4);
}

PostFXPrograms* create_postfx_programs(void) {
    PostFXPrograms* p = calloc(1, sizeof(PostFXPrograms));
    if (!p) {
        log_error("Failed to allocate memory for the post chain's programs");
        return NULL;
    }

    unsigned int rng = 0x9E3779B9u;
    p->noise_texture = create_ssao_noise_texture(&rng);

    p->bloom_bright = create_bloom_bright_program();
    p->bloom_down = create_bloom_down_program();
    p->bloom_up = create_bloom_up_program();
    p->flare = create_lens_flare_program();
    p->tonemap = create_tonemap_program();
    p->spec_occ_composite = create_spec_occ_composite_program();
    p->gtao = create_gtao_program();
    p->ssao_blur = create_ssao_blur_program();
    p->temporal_accum = create_temporal_accum_program();
    p->ssgi_composite = create_ssgi_composite_program();
    p->ssgi_accum = create_ssgi_accum_program();
    p->ssgi_atrous = create_ssgi_atrous_program();
    p->ssr_atrous = create_ssr_atrous_program();
    p->ssr_accum = create_ssr_accum_program();
    p->lum_measure = create_lum_measure_program();
    p->lum_histogram = create_lum_histogram_program();
    p->lum_reduce = create_lum_reduce_program();
    p->ssr = create_ssr_program();
    p->ssr_hiz = create_ssr_hiz_program();
    p->upsample_tent = create_upsample_tent_program();
    // Optional: without it wet ground folds by the plain tent's lerp, as the catcher does.
    p->ssr_fold_wet = create_ssr_fold_wet_program();
    p->froxel_inject = create_froxel_inject_program();
    p->froxel_integrate = create_froxel_integrate_program();
    p->froxel_composite = create_froxel_composite_program();
    p->fog_esm = create_fog_esm_program();
    p->taa_resolve = create_taa_resolve_program();
    p->dof_coc = create_dof_coc_program();
    p->dof_tile = create_dof_tile_program();
    p->dof_dilate = create_dof_dilate_program();
    p->dof_gather = create_dof_gather_program();
    p->dof_composite = create_dof_composite_program();
    p->motion_blur = create_motion_blur_program();
    p->motion_blur_tilemax = create_motion_blur_tilemax_program();
    p->motion_blur_neighbormax = create_motion_blur_neighbormax_program();
    p->sss_gather = create_sss_gather_program();
    p->sss_pyr_seed = create_sss_pyr_seed_program();
    p->sss_pyr_down = create_sss_pyr_down_program();
    p->contact_shadow = create_contact_shadow_program();
    p->oit_resolve = create_oit_resolve_program();
    p->present = create_present_program();
    if (!p->sss_pyr_seed || !p->sss_pyr_down || !p->contact_shadow || !p->oit_resolve ||
        !p->present || !p->sss_gather || !p->motion_blur || !p->motion_blur_tilemax ||
        !p->motion_blur_neighbormax || !p->bloom_bright || !p->bloom_down || !p->bloom_up ||
        !p->tonemap || !p->gtao || !p->ssao_blur || !p->temporal_accum || !p->ssgi_composite ||
        !p->ssgi_accum || !p->ssgi_atrous || !p->ssr_atrous || !p->ssr_accum || !p->lum_measure ||
        !p->lum_histogram || !p->lum_reduce || !p->ssr || !p->upsample_tent || !p->taa_resolve ||
        !p->dof_coc || !p->froxel_inject || !p->froxel_integrate || !p->froxel_composite ||
        !p->dof_tile || !p->dof_dilate || !p->dof_gather || !p->dof_composite) {
        free_postfx_programs(p);
        return NULL;
    }

    // Sampler bindings never change; set them once on the program objects
    glUseProgram(p->bloom_bright->id);
    uniform_set_int(p->bloom_bright->uniforms, "hdrTex", 0);
    glUseProgram(p->bloom_down->id);
    uniform_set_int(p->bloom_down->uniforms, "srcTex", 0);
    glUseProgram(p->bloom_up->id);
    uniform_set_int(p->bloom_up->uniforms, "srcTex", 0);
    glUseProgram(p->tonemap->id);
    uniform_set_int(p->tonemap->uniforms, "hdrTex", 0);
    uniform_set_int(p->tonemap->uniforms, "bloomTex", 1);
    uniform_set_int(p->tonemap->uniforms, "aoTex", 2);
    uniform_set_int(p->tonemap->uniforms, "normalsTex", 3);
    uniform_set_int(p->tonemap->uniforms, "ssrTex", 4);
    uniform_set_int(p->tonemap->uniforms, "albedoTex", 5);
    uniform_set_int(p->tonemap->uniforms, "giTex", 6);
    // Unit 7 held the adapted-luminance texture the tonemap divided by. Exposure
    // is applied whole at the scene passes now -- but the Purkinje shift wants a
    // frame luminance again, so the metering 1x1 goes back on that same unit
    // (spec 11.83). A sampler2D, which is what the note below was insuring
    // against; the LUT staying on 11 is what makes that harmless.
    uniform_set_int(p->tonemap->uniforms, "purkinjeAdaptTex", 7);
    uniform_set_int(p->tonemap->uniforms, "auxTex", 9); // linZ + roughness for spec-occ
    uniform_set_int(p->tonemap->uniforms, "csTex", 10); // contact-shadow visibility
    // 11, not the free 7, and the reason is NOT that 7 would be an error today:
    // no sampler in this program names it, so a sampler3D there would link and
    // draw perfectly well. Unit 7 carries a BINDING (the retired adapted-
    // luminance slot, bound to 2D 0 each frame) and the INVALID_OPERATION rule
    // is about two sampler DECLARATIONS of different types sharing a unit.
    // Appending is what keeps that unrepresentable if a 2D sampler is ever
    // restored to 7 -- it is insurance, not a fix.
    uniform_set_int(p->tonemap->uniforms, "lutTex", 11);
    // Debug view 7 only: the split term is applied in the composite pass, so
    // this exists so that view can show what was applied rather than recompute
    // a second opinion about it.
    uniform_set_int(p->tonemap->uniforms, "specOccTex", 12);

    glUseProgram(p->lum_measure->id);
    uniform_set_int(p->lum_measure->uniforms, "hdrTex", 0);
    glUseProgram(p->lum_histogram->id);
    uniform_set_int(p->lum_histogram->uniforms, "lumTex", 0);
    glUseProgram(p->lum_reduce->id);
    uniform_set_int(p->lum_reduce->uniforms, "histTex", 0);

    glUseProgram(p->ssr->id);
    uniform_set_int(p->ssr->uniforms, "depthTex", 0);
    uniform_set_int(p->ssr->uniforms, "normalsTex", 1);
    uniform_set_int(p->ssr->uniforms, "hdrTex", 2);
    uniform_set_int(p->ssr->uniforms, "probeTex", 3);
    uniform_set_int(p->ssr->uniforms, "hizTex", 4);
    // The probe atlas (spec 11.70), on its own unit rather than aliasing
    // probeTex: those are two sampler TYPES, and pointing both at one image unit
    // is an INVALID_OPERATION at draw. This pass has the units to spare -- it is
    // pbr_frag that does not.
    uniform_set_int(p->ssr->uniforms, "probeAtlasTex", 5);
    // Wet ground's per-pixel roughness (spec 13.9).
    uniform_set_int(p->ssr->uniforms, "auxTex", 6);
    glUseProgram(p->ssr_hiz->id);
    uniform_set_int(p->ssr_hiz->uniforms, "srcTex", 0);
    set_inject_samplers(p->froxel_inject);

    const float white_texel = 1.0f;
    glGenTextures(1, &p->white_tex);
    glBindTexture(GL_TEXTURE_2D, p->white_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, 1, 1, 0, GL_RED, GL_FLOAT, &white_texel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(p->froxel_integrate->id);
    uniform_set_int(p->froxel_integrate->uniforms, "scatterVolume", 0);
    glUseProgram(p->froxel_composite->id);
    uniform_set_int(p->froxel_composite->uniforms, "linDepthTex", 0);
    uniform_set_int(p->froxel_composite->uniforms, "integratedVolume", 1);
    uniform_set_int(p->froxel_composite->uniforms, "layerTex", 2);
    uniform_set_int(p->froxel_composite->uniforms, "aerialVolume", 3);
    uniform_set_int(p->froxel_composite->uniforms, "momentTex", 4);

    glUseProgram(p->dof_coc->id);
    uniform_set_int(p->dof_coc->uniforms, "sceneTex", 0);
    uniform_set_int(p->dof_coc->uniforms, "depthTex", 1);
    glUseProgram(p->dof_tile->id);
    uniform_set_int(p->dof_tile->uniforms, "cocColorTex", 0);
    uniform_set_int(p->dof_tile->uniforms, "tileSize", DOF_TILE);
    glUseProgram(p->dof_dilate->id);
    uniform_set_int(p->dof_dilate->uniforms, "tileTex", 0);
    glUseProgram(p->dof_gather->id);
    uniform_set_int(p->dof_gather->uniforms, "cocColorTex", 0);
    uniform_set_int(p->dof_gather->uniforms, "tileTex", 1);
    uniform_set_int(p->dof_gather->uniforms, "tileSize", DOF_TILE);
    glUseProgram(p->dof_composite->id);
    uniform_set_int(p->dof_composite->uniforms, "sceneTex", 0);
    uniform_set_int(p->dof_composite->uniforms, "nearTex", 1);
    uniform_set_int(p->dof_composite->uniforms, "farTex", 2);
    uniform_set_int(p->dof_composite->uniforms, "depthTex", 3);

    glUseProgram(p->motion_blur_tilemax->id);
    uniform_set_int(p->motion_blur_tilemax->uniforms, "auxTex", 0);
    glUseProgram(p->motion_blur_neighbormax->id);
    uniform_set_int(p->motion_blur_neighbormax->uniforms, "tileTex", 0);
    glUseProgram(p->motion_blur->id);
    uniform_set_int(p->motion_blur->uniforms, "sceneTex", 0);
    uniform_set_int(p->motion_blur->uniforms, "neighborMaxTex", 1);
    uniform_set_int(p->motion_blur->uniforms, "velocityTex", 2);

    glUseProgram(p->sss_gather->id);
    uniform_set_int(p->sss_gather->uniforms, "pyrColor", 0);
    uniform_set_int(p->sss_gather->uniforms, "origTex", 1);
    uniform_set_int(p->sss_gather->uniforms, "auxTex", 2);
    uniform_set_int(p->sss_gather->uniforms, "depthTex", 3);
    glUseProgram(p->sss_pyr_seed->id);
    uniform_set_int(p->sss_pyr_seed->uniforms, "srcTex", 0);
    uniform_set_int(p->sss_pyr_seed->uniforms, "auxTex", 1);
    uniform_set_int(p->sss_pyr_seed->uniforms, "depthTex", 2);
    glUseProgram(p->sss_pyr_down->id);
    uniform_set_int(p->sss_pyr_down->uniforms, "srcColor", 0);
    uniform_set_int(p->sss_pyr_down->uniforms, "srcDepth", 1);
    glUseProgram(p->oit_resolve->id);
    uniform_set_int(p->oit_resolve->uniforms, "accumTex", 0);
    uniform_set_int(p->oit_resolve->uniforms, "revealageTex", 1);

    // noiseScale depends on the AO buffer's size, so the GTAO draw uploads it.
    glUseProgram(p->gtao->id);
    uniform_set_int(p->gtao->uniforms, "linDepthTex", 0);
    uniform_set_int(p->gtao->uniforms, "noiseTex", 1);
    uniform_set_int(p->gtao->uniforms, "normalsTex", 2);
    uniform_set_int(p->gtao->uniforms, "hdrTex", 3);  // SSGI radiance source
    uniform_set_int(p->gtao->uniforms, "specTex", 4); // split spec share of that radiance

    // No texelSize for either of the next two: every consumer of ssao_blur and
    // of the shared tent uploads it per draw from its own resolution, so a
    // seed here would be dead, and a dead seed reads as a sanctioned fallback
    // that a future third consumer could rely on by forgetting to upload.
    glUseProgram(p->ssao_blur->id);
    uniform_set_int(p->ssao_blur->uniforms, "aoTex", 0);
    uniform_set_int(p->ssao_blur->uniforms, "auxTex", 1); // linZ for the bilateral weight

    glUseProgram(p->contact_shadow->id);
    uniform_set_int(p->contact_shadow->uniforms, "linDepthTex", 0);
    uniform_set_int(p->contact_shadow->uniforms, "normalsTex", 1);

    glUseProgram(p->upsample_tent->id);
    uniform_set_int(p->upsample_tent->uniforms, "srcTex", 0);

    glUseProgram(p->present->id);
    uniform_set_int(p->present->uniforms, "pictureTex", 0);

    if (p->spec_occ_composite) {
        glUseProgram(p->spec_occ_composite->id);
        uniform_set_int(p->spec_occ_composite->uniforms, "specTex", 0);
        uniform_set_int(p->spec_occ_composite->uniforms, "aoTex", 1);
        uniform_set_int(p->spec_occ_composite->uniforms, "normalsTex", 2);
        uniform_set_int(p->spec_occ_composite->uniforms, "auxTex", 3);
        uniform_set_int(p->spec_occ_composite->uniforms, "specOccTex", 4);
        uniform_set_int(p->spec_occ_composite->uniforms, "ssrPrevTex", 5);
    }
    if (p->ssr_fold_wet) {
        glUseProgram(p->ssr_fold_wet->id);
        uniform_set_int(p->ssr_fold_wet->uniforms, "srcTex", 0);
        uniform_set_int(p->ssr_fold_wet->uniforms, "normalsTex", 1);
    }

    // temporal_accum, ssr_accum, and ssgi_accum are seeded entirely by
    // run_temporal_accum, which owns their unit layout and texelSize together.

    glUseProgram(p->ssgi_composite->id);
    uniform_set_int(p->ssgi_composite->uniforms, "giTex", 0);
    uniform_set_int(p->ssgi_composite->uniforms, "albedoTex", 1);

    // Both a-trous programs get texelSize per call from run_atrous (the SSR one
    // tracks the full-res toggle; the SSGI one always resolves to the AO texel).
    glUseProgram(p->ssgi_atrous->id);
    uniform_set_int(p->ssgi_atrous->uniforms, "giTex", 0);
    uniform_set_int(p->ssgi_atrous->uniforms, "linDepthTex", 1);
    uniform_set_int(p->ssgi_atrous->uniforms, "normalsTex", 2);

    glUseProgram(p->ssr_atrous->id);
    uniform_set_int(p->ssr_atrous->uniforms, "reflTex", 0);
    uniform_set_int(p->ssr_atrous->uniforms, "linDepthTex", 1);
    uniform_set_int(p->ssr_atrous->uniforms, "normalsTex", 2);
    glUseProgram(0);

    create_fullscreen_quad_vao(&p->quad_vao, &p->quad_vbo);

    check_gl_error("create_postfx_programs");
    return p;
}

void free_postfx_programs(PostFXPrograms* p) {
    if (!p)
        return;
    gl_delete_texture(&p->noise_texture);
    gl_delete_texture(&p->white_tex);

    free_program(p->bloom_bright);
    free_program(p->bloom_down);
    free_program(p->bloom_up);
    free_program(p->flare);
    free_program(p->present);
    glare_programs_release(&p->glare);
    local_exposure_programs_release(&p->local_exposure);
    free_program(p->tonemap);
    free_program(p->spec_occ_composite);
    free_program(p->gtao);
    free_program(p->ssao_blur);
    free_program(p->temporal_accum);
    free_program(p->ssgi_composite);
    free_program(p->ssgi_accum);
    free_program(p->ssr_accum);
    free_program(p->ssgi_atrous);
    free_program(p->ssr_atrous);
    free_program(p->lum_measure);
    free_program(p->lum_histogram);
    free_program(p->lum_reduce);
    free_program(p->ssr);
    free_program(p->ssr_hiz);
    free_program(p->upsample_tent);
    free_program(p->ssr_fold_wet);
    free_program(p->froxel_inject);
    free_program(p->froxel_miss_count);
    free_program(p->froxel_integrate);
    free_program(p->froxel_composite);
    free_program(p->fog_esm);
    free_program(p->taa_resolve);
    free_program(p->taau_resolve);
    free_program(p->dof_coc);
    free_program(p->dof_tile);
    free_program(p->dof_dilate);
    free_program(p->dof_gather);
    free_program(p->dof_composite);
    free_program(p->motion_blur);
    free_program(p->motion_blur_tilemax);
    free_program(p->motion_blur_neighbormax);
    free_program(p->sss_gather);
    free_program(p->sss_pyr_seed);
    free_program(p->sss_pyr_down);
    free_program(p->contact_shadow);
    free_program(p->oit_resolve);

    glDeleteVertexArrays(1, &p->quad_vao);
    glDeleteBuffers(1, &p->quad_vbo);

    free(p);
}

bool postfx_programs_ensure_taau(PostFXPrograms* p) {
    if (p->taau_resolve)
        return true;
    p->taau_resolve = create_taau_resolve_program();
    if (!p->taau_resolve) {
        log_error("Failed to compile the TAAU resolve");
        return false;
    }
    glUseProgram(p->taau_resolve->id);
    uniform_set_int(p->taau_resolve->uniforms, "currentTex", 0);
    uniform_set_int(p->taau_resolve->uniforms, "velocityTex", 1);
    uniform_set_int(p->taau_resolve->uniforms, "historyTex", 2);
    return true;
}

const GlarePrograms* postfx_programs_ensure_glare(PostFXPrograms* p) {
    if (!p->glare.source && !p->glare_failed)
        p->glare_failed = !glare_programs_init(&p->glare);
    return p->glare_failed ? NULL : &p->glare;
}

const LocalExposurePrograms* postfx_programs_ensure_local_exposure(PostFXPrograms* p) {
    if (!p->local_exposure.half && !p->local_exposure_failed)
        p->local_exposure_failed = !local_exposure_programs_init(&p->local_exposure);
    return p->local_exposure_failed ? NULL : &p->local_exposure;
}

ShaderProgram* postfx_programs_ensure_miss_count(PostFXPrograms* p) {
    if (!p->froxel_miss_count && !p->froxel_miss_count_failed) {
        p->froxel_miss_count = create_froxel_miss_count_program();
        p->froxel_miss_count_failed = p->froxel_miss_count == NULL;
        if (p->froxel_miss_count_failed)
            log_error("Fog miss probe: its count could not be made, so it prints nothing");
        else
            set_inject_samplers(p->froxel_miss_count);
    }
    return p->froxel_miss_count;
}
