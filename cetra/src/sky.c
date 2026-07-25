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
    if (sky->quad_vao)
        glDeleteVertexArrays(1, &sky->quad_vao);
    if (sky->quad_vbo)
        glDeleteBuffers(1, &sky->quad_vbo);

    free(sky);
}

void sky_update_sun_dir(SkyAtmosphere* sky) {
    if (!sky)
        return;

    float el = glm_rad(sky->sun_elevation_deg);
    float az = glm_rad(sky->sun_azimuth_deg);
    sky->sun_dir[0] = cosf(el) * sinf(az);
    sky->sun_dir[1] = sinf(el);
    sky->sun_dir[2] = cosf(el) * cosf(az);
}

// Earth atmosphere constants, mirroring the sky_* shaders (kilometers).
// SOURCE OF TRUTH is sky_transmittance_frag.glsl / sky_view_frag.glsl — these
// C copies exist only for the no-readback CPU transmittance eval and MUST be
// kept in sync with the shader profiles if the atmosphere is ever retuned.
#define SKY_RG 6360.0f
#define SKY_RT 6460.0f

// Optical-depth extinction at altitude h (km), matching the shader profiles
static void sky_extinction_at(float h, float out[3]) {
    float rayleighD = expf(-h / 8.0f);
    float mieD = expf(-h / 1.2f);
    float ozoneD = fmaxf(0.0f, 1.0f - fabsf(h - 25.0f) / 15.0f);
    const float ray[3] = {5.802e-3f, 13.558e-3f, 33.1e-3f};
    const float mie_ext = 3.996e-3f / 0.9f;
    const float ozone[3] = {0.650e-3f, 1.881e-3f, 0.085e-3f};
    for (int c = 0; c < 3; c++)
        out[c] = ray[c] * rayleighD + mie_ext * mieD + ozone[c] * ozoneD;
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
    const float r = SKY_RG + 0.5f;
    float disc = r * r * (mu * mu - 1.0f) + SKY_RT * SKY_RT;
    float dist = -r * mu + sqrtf(fmaxf(disc, 0.0f));
    const int steps = 40;
    float dt = dist / (float)steps;
    float depth[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < steps; i++) {
        float t = ((float)i + 0.5f) * dt;
        float rt = sqrtf(r * r + t * t + 2.0f * r * t * mu);
        float e[3];
        sky_extinction_at(rt - SKY_RG, e);
        for (int c = 0; c < 3; c++)
            depth[c] += e[c] * dt;
    }
    for (int c = 0; c < 3; c++)
        out_color[c] = expf(-depth[c]);
}

// Zenith sky radiance: the single-scattering integral straight up, which is
// what an isotropic medium at ground level actually receives from the sky. Same
// constants and the same no-readback CPU march as sky_sun_transmittance, with
// the sun's transmittance taken at ground level for every sample -- it varies
// with altitude, but this feeds a flat ambient term, not a shading model.
static void sky_zenith_radiance(const SkyAtmosphere* sky, vec3 out) {
    glm_vec3_zero(out);
    if (sky->sun_dir[1] <= 0.0f)
        return; // sun below the horizon: no skylight to scatter

    vec3 sun_t = {0};
    sky_sun_transmittance(sky, sun_t);

    const float SUN_ILLUMINANCE = 3.0f; // mirrors atmosphere.glsl
    const float ISOTROPIC_PHASE = 1.0f / (4.0f * 3.14159265359f);
    const float ray[3] = {5.802e-3f, 13.558e-3f, 33.1e-3f};
    const float mie_scatter = 3.996e-3f;

    const int steps = 40;
    const float dt = (SKY_RT - (SKY_RG + 0.5f)) / (float)steps;
    float trans[3] = {1.0f, 1.0f, 1.0f};
    for (int i = 0; i < steps; i++) {
        float h = 0.5f + ((float)i + 0.5f) * dt;
        float rayleigh_d = expf(-h / 8.0f);
        float mie_d = expf(-h / 1.2f);
        float e[3];
        sky_extinction_at(h, e);
        for (int c = 0; c < 3; c++) {
            float scatter = ray[c] * rayleigh_d + mie_scatter * mie_d;
            float step_t = expf(-e[c] * dt);
            float ext = fmaxf(e[c], 1e-7f);
            out[c] += trans[c] * (scatter * sun_t[c] * SUN_ILLUMINANCE * ISOTROPIC_PHASE / ext) *
                      (1.0f - step_t);
            trans[c] *= step_t;
        }
    }
}

// Bake one 2D LUT with a fullscreen pass into a fresh RGBA16F texture
// (delete-before-gen; the render_brdf_lut local-FBO shape). The destination
// texture is created on a SCRATCH unit so it never clobbers a source LUT the
// caller bound on unit 0 (the multiscatter pass samples the transmittance
// LUT there).
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
// by clearing fog_enabled. Every layered pass in this codebase (shadow
// cascades, mask-array layers, cube faces, froxel slices) owns its own loop for
// the same reason; this is the fifth.
//
// Allocates on first use and keeps the texture: unlike the 2D bakes this runs
// every frame, so delete-before-gen would churn a texture per frame.
static void bake_lut_3d(SkyAtmosphere* sky, UniformManager* um) {
    if (!sky->aerial_lut) {
        sky->aerial_lut = create_texture_3d_float(SKY_AERIAL_X, SKY_AERIAL_Y, SKY_AERIAL_Z,
                                                  GL_RGBA16F, GL_RGBA, NULL);
        if (!sky->aerial_lut) {
            log_error("Failed to allocate the aerial perspective volume");
            return;
        }
    }
    if (!sky->aerial_fbo)
        glGenFramebuffers(1, &sky->aerial_fbo);

    glBindFramebuffer(GL_FRAMEBUFFER, sky->aerial_fbo);
    glViewport(0, 0, SKY_AERIAL_X, SKY_AERIAL_Y);
    for (int slice = 0; slice < SKY_AERIAL_Z; slice++) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, sky->aerial_lut, 0, slice);
        if (slice == 0 && glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            log_error("Aerial volume FBO incomplete; dropping aerial perspective");
            glDeleteTextures(1, &sky->aerial_lut);
            sky->aerial_lut = 0;
            break;
        }
        uniform_set_int(um, "sliceIndex", slice);
        draw_fullscreen_quad(sky->quad_vao);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void sky_update_aerial(SkyAtmosphere* sky, mat4 view, mat4 projection) {
    if (!sky || !sky->enabled || !sky->luts_baked || !sky->aerial_program)
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

    bake_lut_3d(sky, u);

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
    // sky path, since its sun term already arrives via the light publish.
    // Skipped when the app owns the value (spores sets a deliberate near-black).
    if (sky && sky->enabled && sky->publish_fog_ambient)
        sky_zenith_radiance(sky, fx->fog_ambient);

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
    if (!sky->transmittance_program || !sky->multiscatter_program || !sky->debug_program) {
        log_error("Failed to get sky LUT shader programs");
        return -1;
    }

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

int sky_bake(SkyAtmosphere* sky, struct IBLResources* ibl, struct Engine* engine) {
    if (!sky || !ibl || !engine) {
        log_error("Invalid state for sky bake");
        return -1;
    }
    sky->view_program = get_engine_shader_program_by_name(engine, "sky_view");
    sky->env_program = get_engine_shader_program_by_name(engine, "sky_env");
    sky->background_program = get_engine_shader_program_by_name(engine, "sky_background");
    if (!sky->view_program || !sky->env_program || !sky->background_program) {
        log_error("Failed to get sky render programs");
        return -1;
    }
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

    glUseProgram(sky->env_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sky->sky_view_lut);
    uniform_set_int(sky->env_program->uniforms, "skyViewLut", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sky->transmittance_lut);
    uniform_set_int(sky->env_program->uniforms, "transmittanceLut", 1);
    uniform_set_vec3(sky->env_program->uniforms, "sunDir", sky->sun_dir);
    uniform_set_mat4(sky->env_program->uniforms, "projection", (float*)projection);
    for (int i = 0; i < 6; i++) {
        uniform_set_mat4(sky->env_program->uniforms, "view", (float*)views[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, ibl->environment_cubemap, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        ibl_render_unit_cube(ibl);
    }
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

    log_info("Sky bake: view+env %.1f ms, IBL %.1f ms", (t1 - t0) * 1000.0, (t2 - t1) * 1000.0);
    return 0;
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
                           mat4 projection) {
    if (!sky || !ibl || !sky->background_program || !sky->sky_view_lut)
        return;

    // Strip translation so the background sits at infinity
    mat4 rot_view;
    glm_mat4_copy(view, rot_view);
    rot_view[3][0] = 0.0f;
    rot_view[3][1] = 0.0f;
    rot_view[3][2] = 0.0f;

    glUseProgram(sky->background_program->id);
    UniformManager* u = sky->background_program->uniforms;
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

    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (depth_was)
        glEnable(GL_DEPTH_TEST);
    if (blend_was)
        glEnable(GL_BLEND);
    glUseProgram(0);
}
