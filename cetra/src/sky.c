#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sky.h"
#include "engine.h"
#include "ibl.h"
#include "light.h"
#include "postfx.h"
#include "texture.h"
#include "util.h"
#include "ext/log.h"

// How far the aerial volume reaches in WORLD units. The atmosphere model is in
// kilometres and the scene is not, so this is the one place the two meet.
static float sky_aerial_far_units(const SkyAtmosphere* sky) {
    return SKY_AERIAL_FAR_KM * sky->world_units_per_km;
}

SkyAtmosphere* create_sky_atmosphere(void) {
    SkyAtmosphere* sky = malloc(sizeof(SkyAtmosphere));
    if (!sky) {
        log_error("Failed to allocate sky atmosphere");
        return NULL;
    }
    memset(sky, 0, sizeof(SkyAtmosphere));

    sky->enabled = true;
    sky->sun_elevation_deg = 35.0f;
    sky->sun_azimuth_deg = 135.0f;
    sky->sun_disc_deg = 0.53f;
    sky->world_units_per_km = 1000.0f; // 1 unit = 1 metre (the glTF convention)
    sky->publish_fog_ambient = true;
    sky->aerial_enabled = true;
    sky->clouds.coverage = 0.45f;
    sky->clouds.cloud_type = 0.6f;
    sky->clouds.density = 1.0f;
    sky->clouds.prev_frame = -1;
    sky_update_sun_dir(sky);

    return sky;
}

void free_sky_atmosphere(SkyAtmosphere* sky) {
    if (!sky)
        return;

    if (sky->transmittance_lut)
        glDeleteTextures(1, &sky->transmittance_lut);
    if (sky->multiscatter_lut)
        glDeleteTextures(1, &sky->multiscatter_lut);
    if (sky->sky_view_lut)
        glDeleteTextures(1, &sky->sky_view_lut);
    if (sky->aerial_lut)
        glDeleteTextures(1, &sky->aerial_lut);
    if (sky->aerial_fbo)
        glDeleteFramebuffers(1, &sky->aerial_fbo);
    if (sky->clouds.shape_tex)
        glDeleteTextures(1, &sky->clouds.shape_tex);
    if (sky->clouds.detail_tex)
        glDeleteTextures(1, &sky->clouds.detail_tex);
    for (int i = 0; i < 2; i++) {
        if (sky->clouds.march_tex[i])
            glDeleteTextures(1, &sky->clouds.march_tex[i]);
        if (sky->clouds.march_fbo[i])
            glDeleteFramebuffers(1, &sky->clouds.march_fbo[i]);
    }
    if (sky->quad_vao)
        glDeleteVertexArrays(1, &sky->quad_vao);
    if (sky->quad_vbo)
        glDeleteBuffers(1, &sky->quad_vbo);

    free(sky);
}

// Earth atmosphere constants, mirroring include/atmosphere.glsl (kilometers).
// SOURCE OF TRUTH is that include -- these C copies exist only for the
// no-readback CPU evaluations below and MUST be kept in sync with it if the
// atmosphere is ever retuned. Kept as ONE named set: two hand-copied tables
// drifting apart inside this file is the failure this block exists to prevent,
// so nothing below re-declares a coefficient.
#define SKY_RG              6360.0f
#define SKY_RT              6460.0f
#define SKY_VIEW_ALTITUDE   0.5f
#define SKY_RAYLEIGH_H      8.0f
#define SKY_MIE_H           1.2f
#define SKY_MIE_SCATTER     3.996e-3f
#define SKY_MIE_ALBEDO      0.9f
#define SKY_OZONE_CENTER    25.0f
#define SKY_OZONE_WIDTH     15.0f
#define SKY_SUN_ILLUMINANCE 3.0f
#define SKY_CPU_MARCH_STEPS 40
static const float SKY_RAYLEIGH_SCATTER[3] = {5.802e-3f, 13.558e-3f, 33.1e-3f};
static const float SKY_OZONE_ABSORB[3] = {0.650e-3f, 1.881e-3f, 0.085e-3f};

// The medium at altitude h (km): the C analogue of atmosphereAt() in
// include/atmosphere.glsl. Either output may be NULL when a caller wants only
// the other -- the two density exponentials are the expensive part and are
// shared rather than recomputed per consumer.
static void sky_medium_at(float h, float scatter[3], float extinction[3]) {
    const float rayleigh_d = expf(-h / SKY_RAYLEIGH_H);
    const float mie_d = expf(-h / SKY_MIE_H);
    const float ozone_d = fmaxf(0.0f, 1.0f - fabsf(h - SKY_OZONE_CENTER) / SKY_OZONE_WIDTH);
    for (int c = 0; c < 3; c++) {
        if (scatter)
            scatter[c] = SKY_RAYLEIGH_SCATTER[c] * rayleigh_d + SKY_MIE_SCATTER * mie_d;
        if (extinction)
            extinction[c] = SKY_RAYLEIGH_SCATTER[c] * rayleigh_d +
                            (SKY_MIE_SCATTER / SKY_MIE_ALBEDO) * mie_d +
                            SKY_OZONE_ABSORB[c] * ozone_d;
    }
}

static void sky_zenith_radiance(const SkyAtmosphere* sky, vec3 out);

void sky_update_sun_dir(SkyAtmosphere* sky) {
    if (!sky)
        return;

    float el = glm_rad(sky->sun_elevation_deg);
    float az = glm_rad(sky->sun_azimuth_deg);
    sky->sun_dir[0] = cosf(el) * sinf(az);
    sky->sun_dir[1] = sinf(el);
    sky->sun_dir[2] = cosf(el) * cosf(az);

    // Both CPU marches below depend on nothing but sun_dir, and this is its
    // single mutation point -- so they run here, once per sun move, rather than
    // from the per-frame publish.
    sky_zenith_radiance(sky, sky->zenith_radiance);
}

void sky_sun_transmittance(const SkyAtmosphere* sky, vec3 out_color) {
    if (!sky) {
        out_color[0] = out_color[1] = out_color[2] = 0.0f;
        return;
    }
    float mu = sky->sun_dir[1]; // cos(zenith) = sin(elevation)
    if (mu <= 0.0f) {
        out_color[0] = out_color[1] = out_color[2] = 0.0f;
        return;
    }

    // March from the ground observer to the top of the atmosphere along the
    // sun direction, accumulating optical depth (the same integral the
    // transmittance LUT bakes, done on the CPU so no GPU readback is needed)
    const float r = SKY_RG + SKY_VIEW_ALTITUDE;
    float disc = r * r * (mu * mu - 1.0f) + SKY_RT * SKY_RT;
    float dist = -r * mu + sqrtf(fmaxf(disc, 0.0f));
    float dt = dist / (float)SKY_CPU_MARCH_STEPS;
    float depth[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < SKY_CPU_MARCH_STEPS; i++) {
        float t = ((float)i + 0.5f) * dt;
        float rt = sqrtf(r * r + t * t + 2.0f * r * t * mu);
        float e[3];
        sky_medium_at(rt - SKY_RG, NULL, e);
        for (int c = 0; c < 3; c++)
            depth[c] += e[c] * dt;
    }
    for (int c = 0; c < 3; c++)
        out_color[c] = expf(-depth[c]);
}

// Zenith sky radiance: the single-scattering integral straight up, which is
// what an isotropic medium at ground level actually receives from the sky. The
// sun's transmittance is taken at ground level for every sample -- it varies
// with altitude, but this feeds a flat ambient term, not a shading model.
// Cached on sun move by sky_update_sun_dir; never call this per frame.
static void sky_zenith_radiance(const SkyAtmosphere* sky, vec3 out) {
    glm_vec3_zero(out);
    if (sky->sun_dir[1] <= 0.0f)
        return; // sun below the horizon: no skylight to scatter

    vec3 sun_t = {0};
    sky_sun_transmittance(sky, sun_t);

    const float isotropic_phase = 1.0f / (4.0f * GLM_PIf);
    const float dt = (SKY_RT - (SKY_RG + SKY_VIEW_ALTITUDE)) / (float)SKY_CPU_MARCH_STEPS;
    float trans[3] = {1.0f, 1.0f, 1.0f};
    for (int i = 0; i < SKY_CPU_MARCH_STEPS; i++) {
        float h = SKY_VIEW_ALTITUDE + ((float)i + 0.5f) * dt;
        float s[3], e[3];
        sky_medium_at(h, s, e);
        for (int c = 0; c < 3; c++) {
            float step_t = expf(-e[c] * dt);
            // Binds only in the topmost slices, where extinction falls to ~1e-7.
            float ext = fmaxf(e[c], 1e-7f);
            out[c] += trans[c] *
                      (s[c] * sun_t[c] * SKY_SUN_ILLUMINANCE * isotropic_phase / ext) *
                      (1.0f - step_t);
            trans[c] *= step_t;
        }
    }
}

// Bake one 2D LUT with a fullscreen pass into a fresh RGBA16F texture
// (delete-before-gen, transient local FBO). The destination texture is
// created on a SCRATCH unit so it never clobbers a source LUT the caller
// bound on unit 0 (the multiscatter pass samples the transmittance LUT
// there).
#define SKY_BAKE_SCRATCH_UNIT 6
static void bake_lut_2d(SkyAtmosphere* sky, ShaderProgram* program, GLuint* texture, int width,
                        int height) {
    if (*texture)
        glDeleteTextures(1, texture);
    glActiveTexture(GL_TEXTURE0 + SKY_BAKE_SCRATCH_UNIT);
    glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_2D, *texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glActiveTexture(GL_TEXTURE0);

    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *texture, 0);

    glViewport(0, 0, width, height);
    glUseProgram(program->id);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_fullscreen_quad(sky->quad_vao);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
}

// The 3D sibling of bake_lut_2d: one draw per layer into an attachment-less
// FBO. postfx's draw_volume_slices is the same idiom but is not reusable here
// -- it hardcodes the fog FBO, the fog volume's dimensions, and reports failure
// by clearing fog_enabled.
//
// That is a shallow reason and the two loops are near-identical; the honest fix
// is a shared draw_fullscreen_quad_slices(fbo, volume, vao, w, h, depth, um)
// returning false on an incomplete FBO so each caller keeps its own failure
// policy. Not done here only to keep this branch's golden gates narrow.
//
// Allocates on first use and keeps the texture: unlike the 2D bakes this runs
// every frame, so delete-before-gen would churn a texture per frame.
static void bake_aerial_volume(SkyAtmosphere* sky, UniformManager* um) {
    if (!sky->aerial_lut) {
        sky->aerial_lut = create_texture_3d_float(SKY_AERIAL_X, SKY_AERIAL_Y, SKY_AERIAL_Z,
                                                  GL_RGBA16F, GL_RGBA, NULL);
        if (!sky->aerial_lut) {
            log_error("Failed to allocate the aerial perspective volume");
            sky->aerial_failed = true;
            return;
        }
    }
    if (!sky->aerial_fbo)
        glGenFramebuffers(1, &sky->aerial_fbo);

    glBindFramebuffer(GL_FRAMEBUFFER, sky->aerial_fbo);
    glViewport(0, 0, SKY_AERIAL_X, SKY_AERIAL_Y);
    // Bound once around the loop, not per draw: the shared draw_fullscreen_quad
    // rebinds it every call, which would be 31 redundant bind pairs here, every
    // frame. Same reason draw_volume_slices hoists it.
    glBindVertexArray(sky->quad_vao);
    for (int slice = 0; slice < SKY_AERIAL_Z; slice++) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, sky->aerial_lut, 0, slice);
        if (slice == 0 && glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            log_error("Aerial volume FBO incomplete; dropping aerial perspective");
            gl_delete_texture(&sky->aerial_lut);
            sky->aerial_failed = true;
            break;
        }
        uniform_set_int(um, "sliceIndex", slice);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void sky_update_aerial(SkyAtmosphere* sky, mat4 view, mat4 projection) {
    if (!sky || !sky->enabled || !sky->aerial_enabled || !sky->luts_baked || !sky->aerial_program ||
        sky->aerial_failed)
        return;

    mat4 inv_view;
    glm_mat4_inv(view, inv_view);

    GLint prev_fbo = 0;
    GLint prev_viewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    // Same discipline as the static bakes: a partial-channel bake output must
    // not feed the blend equation (the BRDF LUT lesson).
    GLboolean blend_was_on = glIsEnabled(GL_BLEND);
    glDisable(GL_BLEND);

    glUseProgram(sky->aerial_program->id);
    UniformManager* u = sky->aerial_program->uniforms;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sky->transmittance_lut);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sky->multiscatter_lut);
    glActiveTexture(GL_TEXTURE0);
    uniform_set_int(u, "transmittanceLut", 0);
    uniform_set_int(u, "multiscatterLut", 1);
    uniform_set_vec3(u, "sunDir", sky->sun_dir);
    uniform_set_mat4(u, "invView", (float*)inv_view);
    uniform_set_mat4(u, "projection", (float*)projection);
    uniform_set_float(u, "aerialFar", sky_aerial_far_units(sky));
    uniform_set_float(u, "unitsPerKm", sky->world_units_per_km);
    uniform_set_int(u, "aerialDepth", SKY_AERIAL_Z);

    bake_aerial_volume(sky, u);

    if (blend_was_on)
        glEnable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    check_gl_error("sky aerial volume");
}

void sky_publish_to_postfx(const SkyAtmosphere* sky, struct PostFX* fx) {
    if (!fx)
        return;
    // The fog's isotropic ambient is otherwise a flat app-set grey that does not
    // respond to the sun at all -- the only part of the fog's lighting with no
    // sky path, since its sun term already arrives via the light publish. A copy,
    // not a computation: the march behind it runs once per sun move.
    //
    // publish_fog_ambient is what stops this from silently overwriting a value
    // someone else owns. The GUI colour picker clears it on edit, because a
    // control that snaps back every frame is worse than no control.
    if (sky && sky->enabled && sky->publish_fog_ambient)
        glm_vec3_copy((float*)sky->zenith_radiance, fx->fog_ambient);

    if (sky && sky->enabled && sky->aerial_lut) {
        fx->aerial_volume = sky->aerial_lut;
        fx->aerial_far = sky_aerial_far_units(sky);
        fx->aerial_slices = SKY_AERIAL_Z;
    } else {
        fx->aerial_volume = 0;
        fx->aerial_far = 0.0f;
        fx->aerial_slices = 0;
    }
}

int sky_bake_static_luts(SkyAtmosphere* sky, struct Engine* engine) {
    if (!sky || !engine) {
        log_error("Invalid state for sky LUT bake");
        return -1;
    }

    sky->transmittance_program = get_engine_shader_program_by_name(engine, "sky_transmittance");
    sky->multiscatter_program = get_engine_shader_program_by_name(engine, "sky_multiscatter");
    sky->aerial_program = get_engine_shader_program_by_name(engine, "sky_aerial");
    sky->debug_program = get_engine_shader_program_by_name(engine, "sky_debug");
    sky->cloud_noise_debug_program = get_engine_shader_program_by_name(engine, "cloud_noise_debug");
    if (!sky->transmittance_program || !sky->multiscatter_program || !sky->debug_program) {
        log_error("Failed to get sky LUT shader programs");
        return -1;
    }
    // Not fatal, unlike the three above: without it the sky still bakes and
    // draws, only aerial perspective is unavailable. Logged so that "aerial
    // perspective does nothing" has a diagnostic rather than being silent.
    if (!sky->aerial_program)
        log_error("No sky_aerial program; aerial perspective disabled");

    if (sky->quad_vao == 0)
        create_fullscreen_quad_vao(&sky->quad_vao, &sky->quad_vbo);

    GLint prev_viewport[4];
    GLint prev_framebuffer;
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_framebuffer);

    // Partial-channel bake outputs must not feed the blend equation (the
    // BRDF LUT lesson); disable for the whole bake block
    GLboolean blend_was_enabled = glIsEnabled(GL_BLEND);
    glDisable(GL_BLEND);

    double t0 = glfwGetTime();
    bake_lut_2d(sky, sky->transmittance_program, &sky->transmittance_lut, SKY_TRANSMITTANCE_W,
                SKY_TRANSMITTANCE_H);

    // The multiple-scattering integral samples the transmittance LUT
    double t1 = glfwGetTime();
    glUseProgram(sky->multiscatter_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sky->transmittance_lut);
    uniform_set_int(sky->multiscatter_program->uniforms, "transmittanceLut", 0);
    bake_lut_2d(sky, sky->multiscatter_program, &sky->multiscatter_lut, SKY_MULTISCATTER_SIZE,
                SKY_MULTISCATTER_SIZE);
    double t2 = glfwGetTime();

    if (blend_was_enabled)
        glEnable(GL_BLEND);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_framebuffer);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    sky->luts_baked = true;
    log_info("Sky static LUTs baked: transmittance %.1f ms, multiscatter %.1f ms",
             (t1 - t0) * 1000.0, (t2 - t1) * 1000.0);
    return 0;
}

// Draw one LUT into a screen-corner viewport rectangle with the debug
// program (a textured-quad DRAW, not a blit: the default framebuffer is
// multisample and single-sample blits into it are illegal on core profile)
static void sky_debug_draw(SkyAtmosphere* sky, GLuint lut, int x, int y, int w, int h,
                           float scale) {
    glViewport(x, y, w, h);
    glUseProgram(sky->debug_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, lut);
    uniform_set_int(sky->debug_program->uniforms, "lut", 0);
    uniform_set_float(sky->debug_program->uniforms, "scale", scale);
    draw_fullscreen_quad(sky->quad_vao);
}

// One mid-volume Z slice of a cloud-noise field, drawn as a corner tile the
// same way sky_debug_draw shows the 2D LUTs.
static void sky_debug_draw_noise(SkyAtmosphere* sky, GLuint volume, int channel, int x, int y,
                                 int w, int h) {
    glViewport(x, y, w, h);
    glUseProgram(sky->cloud_noise_debug_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, volume);
    uniform_set_int(sky->cloud_noise_debug_program->uniforms, "noiseTex", 0);
    uniform_set_float(sky->cloud_noise_debug_program->uniforms, "slice", 0.5f);
    uniform_set_int(sky->cloud_noise_debug_program->uniforms, "channel", channel);
    draw_fullscreen_quad(sky->quad_vao);
}

// Bake the sky-view LUT from the current sun (samples transmittance +
// multiscatter on units 0/1). Small fullscreen RGBA16F pass.
static void sky_bake_view_lut(SkyAtmosphere* sky) {
    // Bind the two source LUTs on units 0/1 and set the sun before the bake;
    // bake_lut_2d re-uses this program, allocates the dest on the scratch unit
    // (6), and draws — leaving these bindings intact.
    glUseProgram(sky->view_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sky->transmittance_lut);
    uniform_set_int(sky->view_program->uniforms, "transmittanceLut", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sky->multiscatter_lut);
    uniform_set_int(sky->view_program->uniforms, "multiscatterLut", 1);
    uniform_set_float(sky->view_program->uniforms, "sunCosZenith", sky->sun_dir[1]);
    glActiveTexture(GL_TEXTURE0);
    bake_lut_2d(sky, sky->view_program, &sky->sky_view_lut, SKY_VIEW_W, SKY_VIEW_H);
}

int sky_bake_ex(SkyAtmosphere* sky, struct IBLResources* ibl, struct Engine* engine,
                bool with_clouds) {
    if (!sky || !ibl || !engine) {
        log_error("Invalid state for sky bake");
        return -1;
    }
    sky->view_program = get_engine_shader_program_by_name(engine, "sky_view");
    sky->env_program = get_engine_shader_program_by_name(engine, "sky_env");
    sky->env_clouds_program = get_engine_shader_program_by_name(engine, "sky_env_clouds");
    sky->background_program = get_engine_shader_program_by_name(engine, "sky_background");
    sky->background_clouds_program =
        get_engine_shader_program_by_name(engine, "sky_background_clouds");
    if (!sky->view_program || !sky->env_program || !sky->background_program) {
        log_error("Failed to get sky render programs");
        return -1;
    }
    // Non-fatal like the aerial program: without it the sky renders, only
    // the cloud composite is unavailable.
    if (sky->clouds.enabled && !sky->background_clouds_program)
        log_error("No sky_background_clouds program; cloud composite disabled");

    // The cadence split: a with-clouds bake marches 24 steps per env texel,
    // which belongs on slider RELEASE and startup, never on the per-drag
    // re-bake this function also serves.
    bool clouds_bake = with_clouds && sky->clouds.enabled && sky->clouds.noise_baked &&
                       sky->env_clouds_program != NULL;
    if (sky->quad_vao == 0)
        create_fullscreen_quad_vao(&sky->quad_vao, &sky->quad_vbo);
    // Reuse the IBL toolkit's unit cube for the six env-face draws (the probe
    // sibling reuses ibl's capture scaffolding the same way).
    ibl_init_cube_vao(ibl);

    GLint prev_viewport[4];
    GLint prev_framebuffer;
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_framebuffer);
    GLboolean cull_was = glIsEnabled(GL_CULL_FACE);
    GLboolean blend_was = glIsEnabled(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    double t0 = glfwGetTime();
    sky_bake_view_lut(sky);

    // Render the sky into the environment cubemap (6 faces), then mip it so
    // the IBL convolutions can pre-average
    if (ibl->environment_cubemap)
        glDeleteTextures(1, &ibl->environment_cubemap);
    ibl_create_cubemap_texture(&ibl->environment_cubemap, SKY_ENV_SIZE, true);

    // Reuse the IBL capture FBO/RBO (created here if this is the first bake;
    // ibl_bake_from_cubemap below reuses the same handles). Matches probe.c.
    if (ibl->capture_fbo == 0)
        glGenFramebuffers(1, &ibl->capture_fbo);
    if (ibl->capture_rbo == 0)
        glGenRenderbuffers(1, &ibl->capture_rbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ibl->capture_fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, ibl->capture_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, SKY_ENV_SIZE, SKY_ENV_SIZE);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              ibl->capture_rbo);
    glViewport(0, 0, SKY_ENV_SIZE, SKY_ENV_SIZE);

    mat4 views[6];
    mat4 projection;
    vec3 origin = {0.0f, 0.0f, 0.0f};
    ibl_capture_views(origin, views);
    glm_perspective(glm_rad(90.0f), 1.0f, 0.1f, 10.0f, projection);

    ShaderProgram* env = clouds_bake ? sky->env_clouds_program : sky->env_program;
    glUseProgram(env->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sky->sky_view_lut);
    uniform_set_int(env->uniforms, "skyViewLut", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sky->transmittance_lut);
    uniform_set_int(env->uniforms, "transmittanceLut", 1);
    if (clouds_bake) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_3D, sky->clouds.shape_tex);
        uniform_set_int(env->uniforms, "shapeTex", 2);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_3D, sky->clouds.detail_tex);
        uniform_set_int(env->uniforms, "detailTex", 3);
        uniform_set_float(env->uniforms, "coverage", sky->clouds.coverage);
        uniform_set_float(env->uniforms, "cloudType", sky->clouds.cloud_type);
        uniform_set_float(env->uniforms, "densityScale", sky->clouds.density);
    }
    uniform_set_vec3(env->uniforms, "sunDir", sky->sun_dir);
    uniform_set_mat4(env->uniforms, "projection", (float*)projection);
    for (int i = 0; i < 6; i++) {
        uniform_set_mat4(env->uniforms, "view", (float*)views[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, ibl->environment_cubemap, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        ibl_render_unit_cube(ibl);
    }
    if (clouds_bake)
        glBindTexture(GL_TEXTURE_3D, 0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, ibl->environment_cubemap);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    double t1 = glfwGetTime();

    if (cull_was)
        glEnable(GL_CULL_FACE);
    if (blend_was)
        glEnable(GL_BLEND);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_framebuffer);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    // The rest of the IBL chain (irradiance + prefilter) from the sky cube
    if (ibl_bake_from_cubemap(ibl, engine, SKY_ENV_SIZE, SKY_PREFILTER_SIZE, SKY_PREFILTER_MIPS) !=
        0)
        return -1;
    double t2 = glfwGetTime();

    log_info("Sky bake%s: view+env %.1f ms, IBL %.1f ms", clouds_bake ? " (clouds)" : "",
             (t1 - t0) * 1000.0, (t2 - t1) * 1000.0);
    return 0;
}

int sky_bake(SkyAtmosphere* sky, struct IBLResources* ibl, struct Engine* engine) {
    return sky_bake_ex(sky, ibl, engine, false);
}

void sky_apply_sun_to_light(SkyAtmosphere* sky) {
    if (!sky || !sky->sun_light)
        return;

    // Direction: the light travels away from the sun. Color: atmospheric
    // transmittance toward the disc (warm near the horizon). Intensity: the
    // base scaled by a fade that reaches zero as the disc sinks below the
    // horizon, so a night sky casts no direct light (and no shadow).
    vec3 travel;
    glm_vec3_negate_to(sky->sun_dir, travel);
    set_light_direction(sky->sun_light, travel);

    vec3 color = {0};
    sky_sun_transmittance(sky, color);
    set_light_color(sky->sun_light, color);

    float fade = glm_clamp(sky->sun_elevation_deg / 3.0f, 0.0f, 1.0f);
    set_light_intensity(sky->sun_light, sky->sun_base_intensity * fade);
    set_light_cast_shadows(sky->sun_light, fade > 0.0f);
}

int sky_update_sun(SkyAtmosphere* sky, struct IBLResources* ibl, struct Engine* engine) {
    if (!sky)
        return -1;
    sky_update_sun_dir(sky);
    if (sky_bake(sky, ibl, engine) != 0)
        return -1;
    sky_apply_sun_to_light(sky);
    return 0;
}

void sky_render_background(SkyAtmosphere* sky, struct IBLResources* ibl, mat4 view,
                           mat4 projection, bool main_camera) {
    if (!sky || !ibl || !sky->background_program || !sky->sky_view_lut)
        return;

    // Strip translation so the background sits at infinity
    mat4 rot_view;
    glm_mat4_copy(view, rot_view);
    rot_view[3][0] = 0.0f;
    rot_view[3][1] = 0.0f;
    rot_view[3][2] = 0.0f;

    // Clouds bind a SEPARATE program rather than a uniform branch in the
    // plain shader, so the off path never carries a cloud sampler. The
    // subsystem gates itself (enabled + a march recorded); the caller's
    // main_camera says only what it alone knows -- that this draw is not a
    // capture, whose cube-face cameras the screen-space march texture would
    // be wrong for.
    bool clouds = main_camera && sky->clouds.enabled && sky->background_clouds_program &&
                  sky->clouds.prev_frame >= 0;
    ShaderProgram* prog = clouds ? sky->background_clouds_program : sky->background_program;

    glUseProgram(prog->id);
    UniformManager* u = prog->uniforms;
    uniform_set_mat4(u, "view", (float*)rot_view);
    uniform_set_mat4(u, "projection", (float*)projection);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sky->sky_view_lut);
    uniform_set_int(u, "skyViewLut", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sky->transmittance_lut);
    uniform_set_int(u, "transmittanceLut", 1);
    uniform_set_vec3(u, "sunDir", sky->sun_dir);
    float cos_radius = cosf(glm_rad(sky->sun_disc_deg * 0.5f));
    uniform_set_float(u, "sunCosRadius", cos_radius);
    uniform_set_float(u, "sunIntensity", 20.0f);
    if (clouds) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, sky->clouds.march_tex[sky->clouds.prev_frame & 1]);
        uniform_set_int(u, "cloudTex", 2);
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        const float screen_size[2] = {(float)vp[2], (float)vp[3]};
        uniform_set_vec2(u, "screenSize", (float*)screen_size);
    }

    GLboolean cull_was = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_CULL_FACE);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    ibl_render_unit_cube(ibl);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    if (cull_was)
        glEnable(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0);
}

void sky_debug_blit_luts(SkyAtmosphere* sky, int screen_w, int screen_h) {
    if (!sky || !sky->luts_baked || !sky->debug_program)
        return;
    (void)screen_w;

    GLint prev_viewport[4];
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend_was = glIsEnabled(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    // Transmittance (wide strip, [0,1] so unscaled) above the multiscatter
    // square. Psi is a normalized per-sun-illuminance transfer factor with
    // very small stored values, so the multiscatter view is heavily boosted.
    int tw = SKY_TRANSMITTANCE_W * 2, th = SKY_TRANSMITTANCE_H * 2;
    sky_debug_draw(sky, sky->transmittance_lut, 10, screen_h - 10 - th, tw, th, 1.0f);
    int mw = SKY_MULTISCATTER_SIZE * 4;
    sky_debug_draw(sky, sky->multiscatter_lut, 10, screen_h - 20 - th - mw, mw, mw, 200.0f);
    // Sky-view LUT below (mild boost to reveal the HDR horizon/zenith range)
    if (sky->sky_view_lut) {
        int sw = SKY_VIEW_W * 2, sh = SKY_VIEW_H * 2;
        sky_debug_draw(sky, sky->sky_view_lut, 10, screen_h - 30 - th - mw - sh, sw, sh, 0.3f);
    }

    // Cloud-noise fields, mid-volume slices: the shape's Perlin-Worley above
    // the detail field's first octave, in the column right of the LUTs.
    if (sky->clouds.noise_baked && sky->cloud_noise_debug_program) {
        int cx = 20 + tw;
        int cw = SKY_CLOUD_SHAPE_SIZE * 2;
        int dw = SKY_CLOUD_DETAIL_SIZE * 4;
        sky_debug_draw_noise(sky, sky->clouds.shape_tex, 0, cx, screen_h - 10 - cw, cw, cw);
        sky_debug_draw_noise(sky, sky->clouds.detail_tex, 0, cx, screen_h - 20 - cw - dw, dw, dw);
        glBindTexture(GL_TEXTURE_3D, 0);
    }

    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (depth_was)
        glEnable(GL_DEPTH_TEST);
    if (blend_was)
        glEnable(GL_BLEND);
    glUseProgram(0);
}
