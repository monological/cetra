#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "moon_surface.h"

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
    // Stars OFF in the library: three gate arms read a sub-horizon sky and
    // assume a quiet backdrop, and off-by-default is what protects them. Apps
    // and scene files opt in.
    sky->stars_enabled = false;
    sky->stars_brightness = 1.0f;
    sky->stars_latitude_deg = 45.0f;
    sky->stars_hour_deg = 0.0f;
    // OFF for the stars' reason: the sub-horizon gate arms assume a black
    // sky, and the goldens' 0 px rests on the default being an exact zero.
    sky->night_floor_enabled = false;
    sky->night_floor_brightness = 1.0f;
    // Cycle off, clock frozen, slicer idle. day_seconds 0 by default is what
    // keeps config-perturb's enabled-flip round-trippable, and -1 is the
    // slicer's idle state -- the memset's 0 would mean "in flight at item 0".
    sky->cycle_day_seconds = 0.0f;
    sky->cycle_hour = 12.0;
    sky->slicer.item = -1;
    // OFF for the floor's and the stars' reason, and one more of its own: the
    // moon is a second CASTING directional, so a default-on moon would change
    // the caster count in every night frame in the corpus.
    sky->moon_enabled = false;
    sky->moon_brightness = 1.0f;
    sky->moon_size = 1.0f; // life size; the default frame is the physical one
    // A gibbous moon high in the north-west against the default 35/135 sun:
    // elongation 125 degrees, so a phase angle of 55 and a factor of 0.26.
    // Deliberately not degenerate -- at the sun's own angles the phase angle
    // would be 0 and the moon would sit inside the sun's disc, and at the
    // exact anti-sun point it is below the horizon whenever the sun is up.
    sky->moon_elevation_deg = 20.0f;
    sky->moon_azimuth_deg = 315.0f;
    sky->moon_earthshine = true;
    sky->moon_maria = true;
    sky->moon_glow = true;
    // Full moon when the cycle is armed: the postcard is the default, and the
    // month runs from there.
    sky->cycle_moon_offset = 12.0;
    sky->world_units_per_km = 1000.0f; // 1 unit = 1 metre (the glTF convention)
    sky->publish_fog_ambient = true;
    sky->aerial_enabled = true;
    // Coverage is a GAP fraction as far as the GROUND is concerned, which is worth knowing before
    // changing it (spec 11.41). Extinction is 25/km over a 2.5 km deck, so tau is 62.5 x density
    // and half transmittance needs density 0.011 -- any real cloud in a column blocks the sun
    // outright, exactly as a real cumulus does. Dappled light is therefore a map of the HOLES,
    // and this value decides whether there are any: measured 0.0% of the shadow map above half
    // transmittance here at a 35 degree sun, against 47.9% at 0.10. At this default the deck
    // casts a flat 32% dimming with no pattern in it; ~0.10 is where the dapple lives.
    sky->clouds.coverage = 0.45f;
    sky->clouds.cloud_type = 0.6f;
    sky->clouds.density = 1.0f;
    sky->clouds.prev_frame = -1;
    // On with the deck rather than behind its own switch: a cloud that casts no shadow is
    // wrong, not un-featured. --no-cloud-shadows clears it, for bisecting.
    sky->clouds.shadows_enabled = true;
    sky_update_sun_dir(sky);
    // moon_dir alongside sun_dir: sky_moon_phase_factor reads both, and an
    // undirected moon would read as full at the zenith rather than as absent.
    sky_update_moon(sky);

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
    if (sky->clouds.shadow_tex)
        glDeleteTextures(1, &sky->clouds.shadow_tex);
    if (sky->clouds.shadow_fbo)
        glDeleteFramebuffers(1, &sky->clouds.shadow_fbo);
    if (sky->quad_vao)
        glDeleteVertexArrays(1, &sky->quad_vao);
    if (sky->quad_vbo)
        glDeleteBuffers(1, &sky->quad_vbo);
    if (sky->lut_fbo)
        glDeleteFramebuffers(1, &sky->lut_fbo);
    if (sky->moon_surface_tex)
        glDeleteTextures(1, &sky->moon_surface_tex);
    if (sky->slicer.frozen_lut)
        glDeleteTextures(1, &sky->slicer.frozen_lut);
    if (sky->slicer.shadow_env)
        glDeleteTextures(1, &sky->slicer.shadow_env);
    if (sky->slicer.shadow_irr)
        glDeleteTextures(1, &sky->slicer.shadow_irr);
    if (sky->slicer.shadow_prefilter)
        glDeleteTextures(1, &sky->slicer.shadow_prefilter);
    if (sky->slicer.shadow_charlie)
        glDeleteTextures(1, &sky->slicer.shadow_charlie);

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

/*
 * The night-sky floor (spec 11.80): what the sky between the stars emits --
 * airglow, zodiacal light, integrated starlight. C-side ONLY, unlike the
 * atmosphere constants above: the shader receives the product premultiplied,
 * so there is no GLSL copy to keep in sync.
 *
 * The radiance is a look-calibrated constant, not physics, and the reason is
 * the same one the star ramp carries: exposure_auto_gain only ever DARKENS,
 * so no adaptation lifts a dim world and this number IS the night brightness.
 */
#define SKY_NIGHT_FLOOR_RADIANCE 0.03f
static const float SKY_NIGHT_FLOOR_COLOR[3] = {0.55f, 0.72f, 1.0f};

/*
 * The moon (spec 11.82), C-side only for the night floor's reason: the shader
 * receives products, so there is no GLSL copy to keep in sync.
 *
 * The synodic month is the one constant behind two facts, because they ARE
 * one fact: the moon returns to the same phase every 29.53 days, which makes
 * 24/29.53 = 0.813 hours the lag it gains per day (the "rises ~50 minutes
 * later" figure) and simultaneously the rate its phase evolves.
 *
 * The other two are LEVELS and are look-calibrated, not physical, for the
 * night floor's reason one comment up. Their physical references, so the gap
 * is a recorded choice rather than an accident: a full moon delivers ~0.25 lux
 * against the sun's ~120,000, a ratio of 2.1e-6; and its surface radiance is
 * ~1/400,000 of the sun's, which against the disc's own sunIntensity of 20
 * would be 5e-5. Both render black.
 */
#define SKY_SYNODIC_DAYS        29.530588
#define SKY_MOON_LIGHT_FRACTION 0.04f
// Calibrated so a full moon sits just under the tonemap's shoulder rather than
// through it. At 4.0 the disc clipped: every albedo variation between the seas
// and the highlands compressed into the top codes, so the surface read as a
// blank cream oval and the channels saturated unevenly, which is where the
// orange cast came from. The detail only appeared at -E 0.22, and that ratio is
// what set this.
#define SKY_MOON_DISC_RADIANCE  0.80f
// The aureole reaches this many DISC RADII at life size (~2 degrees).
#define SKY_MOON_GLOW_RADII     8.0f

// The one definition of "night": the civil-twilight fade the stars, the
// floor and the zenith ambient all share. 0 at +3 degrees and above -- an
// EXACT zero, which the daylight gate arms assert -- 1 from -8 down.
static float sky_night_factor(const SkyAtmosphere* sky) {
    return 1.0f - glm_smoothstep(-8.0f, 3.0f, sky->sun_elevation_deg);
}

// The horizon fade a sky body's LIGHT rides out on: 1 at and above +3
// degrees, an exact 0 at and below 0. Extracted for the ramp above's reason
// -- the sun has used this window since the sky shipped and the moon must use
// the SAME one, or a moonrise lights the world before the disc clears the
// horizon it is drawn behind.
//
// It is NOT sky_night_factor's window and the two are easy to confuse: that
// one runs to -8 and answers "is it dark enough to see this", while this one
// answers "is this body above the horizon". The moon's light multiplies both.
static float sky_horizon_fade(float elevation_deg) {
    return glm_clamp(elevation_deg / 3.0f, 0.0f, 1.0f);
}

// colour x base x brightness x ramp, or zero: the premultiplied floor the
// LUT bake uploads and the zenith march adds.
static void sky_night_floor(const SkyAtmosphere* sky, vec3 out) {
    glm_vec3_zero(out);
    if (!sky->night_floor_enabled)
        return;
    float s = SKY_NIGHT_FLOOR_RADIANCE * sky->night_floor_brightness * sky_night_factor(sky);
    for (int c = 0; c < 3; c++)
        out[c] = SKY_NIGHT_FLOOR_COLOR[c] * s;
}

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

// Elevation/azimuth to a unit vector, azimuth 0 = +Z increasing toward +X.
// Extracted for sky_horizon_fade's reason: the convention is a policy both sky
// bodies must share, and a second copy is where a body ends up in a mirrored
// sky that still looks like a sky.
static void sky_dir_from_angles(float elevation_deg, float azimuth_deg, vec3 out) {
    const float el = glm_rad(elevation_deg);
    const float az = glm_rad(azimuth_deg);
    out[0] = cosf(el) * sinf(az);
    out[1] = sinf(el);
    out[2] = cosf(el) * cosf(az);
}

void sky_update_sun_dir(SkyAtmosphere* sky) {
    if (!sky)
        return;

    sky_dir_from_angles(sky->sun_elevation_deg, sky->sun_azimuth_deg, sky->sun_dir);

    // Both CPU marches below depend on nothing but sun_dir, and this is its
    // single mutation point -- so they run here, once per sun move, rather than
    // from the per-frame publish.
    sky_zenith_radiance(sky, sky->zenith_radiance);
}

// The transmittance march itself, in terms of the one thing it depends on:
// mu = cos(zenith) = sin(elevation) of whatever body is being looked toward.
// Extracted for the moon, which needs the same integral at its own elevation
// -- moonlight IS sunlight, so it reddens near the horizon for exactly the
// sun's reason and must do it through exactly the sun's arithmetic.
static void sky_transmittance_at(float mu, vec3 out_color) {
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

void sky_sun_transmittance(const SkyAtmosphere* sky, vec3 out_color) {
    sky_transmittance_at(sky ? sky->sun_dir[1] : -1.0f, out_color);
}

// Zenith sky radiance: the single-scattering integral straight up, which is
// what an isotropic medium at ground level actually receives from the sky. The
// sun's transmittance is taken at ground level for every sample -- it varies
// with altitude, but this feeds a flat ambient term, not a shading model.
// Cached on sun move by sky_update_sun_dir; never call this per frame.
static void sky_zenith_radiance(const SkyAtmosphere* sky, vec3 out) {
    glm_vec3_zero(out);
    // The floor first (spec 11.80): an isotropic radiance field's ambient IS
    // its radiance, and below the horizon it is most of what there is --
    // before it, night fog scattered literally nothing.
    vec3 nf = {0};
    sky_night_floor(sky, nf);
    glm_vec3_add(out, nf, out);
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
    // Allocate on first use and KEEP it. Every destination here has a fixed
    // size, and since 11.81 the view LUT is re-baked every frame of a running
    // cycle -- delete-before-gen would churn a texture and an FBO per frame,
    // which is exactly why bake_aerial_volume below keeps its own volume.
    if (!*texture) {
        glActiveTexture(GL_TEXTURE0 + SKY_BAKE_SCRATCH_UNIT);
        glGenTextures(1, texture);
        glBindTexture(GL_TEXTURE_2D, *texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glActiveTexture(GL_TEXTURE0);
    }

    if (!sky->lut_fbo)
        glGenFramebuffers(1, &sky->lut_fbo);
    GLuint fbo = sky->lut_fbo;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *texture, 0);

    // Self-contained for blend/cull since 11.81: the cycle tick bakes the
    // view LUT per frame from outside any caller-managed disable scope, and a
    // quad blended against fresh-texture garbage differs run to run.
    GLboolean cull_was = glIsEnabled(GL_CULL_FACE);
    GLboolean blend_was = glIsEnabled(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    glViewport(0, 0, width, height);
    glUseProgram(program->id);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_fullscreen_quad(sky->quad_vao);

    if (cull_was)
        glEnable(GL_CULL_FACE);
    if (blend_was)
        glEnable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
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

// Everything a consumer needs to read the deck's shadow. Derived in ONE place because two
// stages now read the same map -- the froxel medium and the three ground surfaces -- and a
// shear that disagreed between them would shadow the haze with a deck the ground is standing
// somewhere else under. tile 0 is the whole off state; the other fields are then unread.
typedef struct {
    GLuint tex;
    float tile;
    float shell_y;
    float shear[2];
    int light;
} CloudShadowTerms;

static CloudShadowTerms cloud_shadow_terms(const SkyAtmosphere* sky) {
    // The texture handle IS the "a march has happened" statement -- shadow_tex is only ever
    // created inside build_cloud_shadow, so testing it needs no second frame counter.
    CloudShadowTerms t = {0};
    t.light = -1;
    if (!sky || !sky->enabled || !sky->clouds.enabled || !sky->clouds.shadows_enabled ||
        !sky->clouds.shadow_tex || sky->sun_dir[1] <= 0.0f)
        return t;

    t.tex = sky->clouds.shadow_tex;
    t.tile = sky->clouds.shadow_tile;
    t.shell_y = sky->clouds.shadow_shell_y;
    // World XZ per unit of climb toward the sun. Derived here rather than handing over the
    // direction because it is constant for the frame, and the consumer would otherwise divide
    // by the sun's Y once per froxel cell, or once per fragment.
    t.shear[0] = sky->sun_dir[0] / sky->sun_dir[1];
    t.shear[1] = sky->sun_dir[2] / sky->sun_dir[1];
    // Which light the deck occludes, as a CSM slot. The sky knows exactly, so no consumer has
    // to recognise the sun by comparing directions. -1 when the sun is not casting (below the
    // horizon fade) and so is absent from the shadowed-light list entirely.
    t.light =
        (sky->sun_light && sky->sun_light->cast_shadows) ? sky->sun_light->shadow_map_index : -1;
    return t;
}

void sky_bind_cloud_shadow(const SkyAtmosphere* sky, ShaderProgram* program, int unit) {
    if (!program || !program->uniforms)
        return;
    CloudShadowTerms t = cloud_shadow_terms(sky);
    UniformManager* u = program->uniforms;
    uniform_set_float(u, "cloudShadowTile", t.tile);
    uniform_set_float(u, "cloudShadowShellY", t.shell_y);
    uniform_set_vec2(u, "cloudShadowShear", t.shear);
    uniform_set_int(u, "cloudShadowLight", t.light);

    // The unit is bound and named unconditionally, texture 0 and all. Leaving the
    // sampler at its default of 0 when there is no deck would point it at whatever
    // that program keeps on unit 0 -- and a samplerCube tenant there is undefined
    // shading for the whole program, which is the hazard bind_ibl_textures spells
    // out at its own call site. `light` -1 is the off state; the unit is still ours.
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glActiveTexture(GL_TEXTURE0);
    uniform_set_int(u, "cloudShadowTex", unit);
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

    // The cloud deck's sun transmittance, for the fog to shadow its sun term with.
    CloudShadowTerms cs = cloud_shadow_terms(sky);
    fx->cloud_shadow_tex = cs.tex;
    fx->cloud_shadow_tile = cs.tile;
    fx->cloud_shadow_shell_y = cs.shell_y;
    fx->cloud_shadow_shear[0] = cs.shear[0];
    fx->cloud_shadow_shear[1] = cs.shear[1];
    fx->cloud_shadow_light = cs.light;
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
// mono: the source has one channel, so replicate red rather than drawing it as a red tile.
static void sky_debug_draw(SkyAtmosphere* sky, GLuint lut, int x, int y, int w, int h,
                           float scale, bool mono) {
    glViewport(x, y, w, h);
    glUseProgram(sky->debug_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, lut);
    uniform_set_int(sky->debug_program->uniforms, "lut", 0);
    uniform_set_float(sky->debug_program->uniforms, "scale", scale);
    uniform_set_int(sky->debug_program->uniforms, "mono", mono ? 1 : 0);
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

// Whether an env bake should march the cloud deck. ONE statement of it: the
// atomic path and the slicer disagreeing here means a GUI drag (which goes
// through sky_bake, i.e. with_clouds false) leaves a cloudless env cube that
// the slicer then silently replaces with a clouded one.
static bool sky_clouds_bake_wanted(const SkyAtmosphere* sky) {
    return sky->clouds.enabled && sky->clouds.noise_baked && sky->env_clouds_program != NULL;
}

// Bake a sky-view LUT into the caller's texture from the caller's SUN
// (samples transmittance + multiscatter on units 0/1). Small fullscreen
// RGBA16F pass. Both are parameters since 11.81: the live LUT tracks the sun
// per frame while the slicer's frozen copy must hold the latched one, and a
// bake that read the live sun for both would seam the env cube's faces.
static void sky_bake_view_lut_to(SkyAtmosphere* sky, GLuint* dst, const vec3 sun_dir) {
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
    uniform_set_float(sky->view_program->uniforms, "sunCosZenith", sun_dir[1]);
    vec3 nf = {0};
    sky_night_floor(sky, nf);
    uniform_set_vec3(sky->view_program->uniforms, "nightFloor", nf);
    glActiveTexture(GL_TEXTURE0);
    bake_lut_2d(sky, sky->view_program, dst, SKY_VIEW_W, SKY_VIEW_H);
}

static void sky_bake_view_lut(SkyAtmosphere* sky) {
    sky_bake_view_lut_to(sky, &sky->sky_view_lut, sky->sun_dir);
}

// One env-cube face, self-contained (spec 11.81): FBO/RBO respec, program
// preamble and uniforms re-established per call, so the cycle slicer can run
// a face in isolation frames after any other slice and the atomic bake below
// runs the same code six times back to back. The view LUT and the sun are
// PARAMETERS because the slicer's faces must all see one latched sun while
// the live LUT keeps moving. Leaves FBO 0 bound; caller restores viewport.
static void sky_env_face_slice(SkyAtmosphere* sky, struct IBLResources* ibl, GLuint view_lut,
                               const vec3 sun_dir, GLuint dst_cube, bool clouds_bake, int face) {
    ShaderProgram* env = clouds_bake ? sky->env_clouds_program : sky->env_program;
    if (!env)
        return;

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
    mat4 projection = {{0}};
    vec3 origin = {0.0f, 0.0f, 0.0f};
    ibl_capture_views(origin, views);
    ibl_cubemap_projection(projection);

    glUseProgram(env->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, view_lut);
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
    uniform_set_vec3(env->uniforms, "sunDir", (float*)sun_dir);
    uniform_set_mat4(env->uniforms, "projection", (float*)projection);

    GLboolean cull_was = glIsEnabled(GL_CULL_FACE);
    GLboolean blend_was = glIsEnabled(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    uniform_set_mat4(env->uniforms, "view", (float*)views[face]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, dst_cube, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    ibl_render_unit_cube(ibl);

    if (clouds_bake)
        glBindTexture(GL_TEXTURE_3D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (cull_was)
        glEnable(GL_CULL_FACE);
    if (blend_was)
        glEnable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// The env cube's mip chain, one indivisible piece: a chain generated from a
// half-drawn cube is garbage, so this never splits below "all six faces are
// in".
static void sky_env_mipgen(GLuint cube) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
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
    bool clouds_bake = with_clouds && sky_clouds_bake_wanted(sky);
    // An atomic bake is the authoritative statement of the environment, so it
    // CANCELS any slice in flight: otherwise that slice's swap lands a few
    // frames later carrying content baked from the frozen LUT taken before
    // this call, and the edit that triggered the bake visibly reverts. Covers
    // the GUI's floor/cloud controls, a config restore and --cycle-rebake-at
    // alike. Re-armed dirty so a running cycle re-converges from the new sun.
    if (sky->slicer.item >= 0) {
        sky->slicer.item = -1;
        sky->cycle_dirty = sky->cycle_enabled;
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
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    // The atomic path is the same resumable pieces the cycle slicer runs,
    // back to back (spec 11.81): one arithmetic, whatever the schedule.
    double t0 = glfwGetTime();
    sky_bake_view_lut(sky);

    if (ibl->environment_cubemap)
        glDeleteTextures(1, &ibl->environment_cubemap);
    ibl_create_cubemap_texture(&ibl->environment_cubemap, SKY_ENV_SIZE, true);
    for (int i = 0; i < 6; i++)
        sky_env_face_slice(sky, ibl, sky->sky_view_lut, sky->sun_dir, ibl->environment_cubemap,
                           clouds_bake, i);
    sky_env_mipgen(ibl->environment_cubemap);
    double t1 = glfwGetTime();

    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_framebuffer);

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

/*
 * How a sky body becomes a light, stated once.
 *
 * Direction: the light travels away from the body. Colour: atmospheric
 * transmittance along the body's own air column, so both redden at the horizon
 * for one reason and through one march. Intensity: the base scaled by the
 * shared horizon fade and by whatever else the body brings. Shadows: on iff it
 * is delivering light.
 *
 * ONE function rather than two near-twins, and the reason is the last line.
 * The cast test has to be the intensity the line above it just set -- written
 * twice, the moon's copy dropped moon_brightness and a moon dialled to zero
 * still claimed a cascade slot, which costs a full scene traversal per cascade
 * every frame because the caster count never consults intensity. Sharing the
 * body is what makes that unwritable rather than merely fixed.
 *
 * The threshold is not zero. A body's own factors can be tiny without being
 * zero -- the phase law bottoms out at 2.8e-4 rather than 0, so a NEW moon
 * would otherwise cast all night at 19 stops under the sun. A thousandth of
 * the base is below anything a frame can show and far above where the law
 * flattens.
 */
#define SKY_LIGHT_CAST_FLOOR 1e-3f

static void sky_apply_body_to_light(struct Light* light, const vec3 dir, float elevation_deg,
                                    float base, float scale) {
    if (!light)
        return;

    vec3 travel;
    glm_vec3_negate_to((float*)dir, travel);
    set_light_direction(light, travel);

    vec3 color = {0};
    sky_transmittance_at(dir[1], color);
    set_light_color(light, color);

    const float intensity = base * sky_horizon_fade(elevation_deg) * scale;
    set_light_intensity(light, intensity);
    set_light_cast_shadows(light, intensity > base * SKY_LIGHT_CAST_FLOOR);
}

void sky_apply_sun_to_light(SkyAtmosphere* sky) {
    if (!sky)
        return;
    sky_apply_body_to_light(sky->sun_light, sky->sun_dir, sky->sun_elevation_deg,
                            sky->sun_base_intensity, 1.0f);
}

/*
 * Krisciunas & Schaefer 1991's brightness term, normalised to 1 at full moon:
 *
 *     10^(-0.4 * (0.026*|a| + 4e-9*a^4))       a in degrees
 *
 * NOT a Lambertian lit fraction, and the gap between them is the whole reason
 * this is a named law rather than a dot product. Measured quarter-moon light
 * is an eighth to a tenth of full; this gives 0.091 at a = 90 where cosine
 * geometry gives 0.5. The cause is real -- lunar regolith backscatters, and
 * near opposition the shadows hide behind the grains that cast them. A moon
 * lit Lambertian reads as a spotlight that never quite turns off.
 *
 * `a` is the SUN-MOON-OBSERVER angle, measured AT THE MOON: 0 at full (the
 * moon opposite the sun), 180 at new (the moon at the sun). From here the
 * moon->observer direction is -moon_dir and the moon->sun direction is
 * sun_dir, so cos(a) = -dot(moon_dir, sun_dir) -- the NEGATIVE of the
 * elongation's cosine. That sign is written out because getting it wrong
 * inverts the feature and still renders a plausible moon, and because the two
 * readings AGREE at 90 degrees, which is exactly where a ladder that does not
 * straddle it would fail to notice.
 *
 * Derived, never stored: a phase field would be a second place for this to go
 * stale, and a stale phase is silent.
 */
// cos of the phase angle: the NEGATIVE of the elongation's, see above.
static float sky_moon_cos_phase(const SkyAtmosphere* sky) {
    return glm_clamp(-glm_vec3_dot((float*)sky->moon_dir, (float*)sky->sun_dir), -1.0f, 1.0f);
}

float sky_moon_phase_factor(const SkyAtmosphere* sky) {
    if (!sky)
        return 0.0f;
    float a = glm_deg(acosf(sky_moon_cos_phase(sky)));
    float a2 = a * a;
    return powf(10.0f, -0.4f * (0.026f * a + 4.0e-9f * a2 * a2));
}

float sky_moon_lit_fraction(const SkyAtmosphere* sky) {
    if (!sky)
        return 0.0f;
    return 0.5f * (1.0f + sky_moon_cos_phase(sky));
}

/*
 * The moon's scale on the shared body policy above. Everything here is a
 * RATIO except one: the transmittance, the phase law and the night ramp are
 * all dimensionless, and SKY_MOON_LIGHT_FRACTION is the LEVEL -- a look
 * constant for the night floor's reason, since nothing lifts a dim world and
 * this number IS the moonlight. The physical ratio is 0.25 lux against
 * ~120,000, nineteen stops, which renders black.
 *
 * sky_night_factor is what keeps a daytime moon from adding a second key
 * light to a sunlit frame, and it BOUNDS the two-caster window by
 * construction: an exact zero above +3 degrees, so above that the moon cannot
 * cast and the daylight caster count is what it always was.
 */
static float sky_moon_light_scale(const SkyAtmosphere* sky) {
    if (!sky->moon_enabled)
        return 0.0f;
    return SKY_MOON_LIGHT_FRACTION * sky->moon_brightness * sky_moon_phase_factor(sky) *
           sky_night_factor(sky);
}

static void sky_apply_moon_to_light(SkyAtmosphere* sky) {
    sky_apply_body_to_light(sky->moon_light, sky->moon_dir, sky->moon_elevation_deg,
                            sky->sun_base_intensity, sky_moon_light_scale(sky));
}

void sky_update_moon(SkyAtmosphere* sky) {
    if (!sky)
        return;

    sky_dir_from_angles(sky->moon_elevation_deg, sky->moon_azimuth_deg, sky->moon_dir);
    sky_apply_moon_to_light(sky);
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

void sky_sun_path(double latitude_deg, double hour, float* out_elevation_deg,
                  float* out_azimuth_deg) {
    // Equinox path: rotate the equator's noon crossing about the celestial
    // pole by the hour angle. With P = (0, sin L, cos L) and the noon point
    // E0 = (0, cos L, -sin L), P x E0 = (-1, 0, 0), so Rodrigues collapses to
    // the closed form below. DOUBLE throughout -- see the header.
    double h = (hour - 12.0) * (M_PI / 12.0); // solar noon at 12
    double lat = latitude_deg * (M_PI / 180.0);
    double x = -sin(h);
    double y = cos(lat) * cos(h);
    double z = -sin(lat) * cos(h);
    double el = asin(fmax(-1.0, fmin(1.0, y)));
    double az = atan2(x, z); // azimuth 0 = +Z, increasing toward +X
    if (az < 0.0)
        az += 2.0 * M_PI;
    *out_elevation_deg = (float)(el * (180.0 / M_PI));
    *out_azimuth_deg = (float)(az * (180.0 / M_PI));
}

/*
 * The sliced re-bake's work schedule: the atomic bake's exact draws, in the
 * atomic bake's exact order, cut into items the tick consumes against a
 * texel-weighted budget. Generated rather than hand-written so the mip loop
 * stays one statement of the shape; the split rule -- fine mips per face,
 * coarse mips whole -- is what flattens the cost curve, since mip 0 outweighs
 * the tail by 4096x and a per-mip schedule would spike ~40 ms twice a cycle.
 */
typedef enum {
    SKY_SLICE_LUT,
    SKY_SLICE_ENV_FACE,
    SKY_SLICE_MIPGEN,
    SKY_SLICE_IRRADIANCE,
    SKY_SLICE_GGX,
    SKY_SLICE_CHARLIE,
    SKY_SLICE_SWAP,
} SkySliceKind;

typedef struct {
    unsigned char kind;
    signed char mip;
    signed char face_first;
    signed char face_count;
    int weight; // texels x a rough per-texel cost factor
} SkySliceItem;

/*
 * Weighted texels the tick spends per frame.
 *
 * The weights are a cost MODEL and two inputs are rough: the x2 prefilter
 * factor implies a GGX+Charlie cost under what 11.0 measured for the same
 * draws, and an env face is weighted independently of clouds_bake though a
 * cloud face marches 24 steps per texel. The model only has to RANK items and
 * bound a frame.
 *
 * The budget sits just under the heaviest single item (a mip-0 prefilter face
 * at 131072) so the pump's overshoot guard bites and those run alone.
 * MEASURED at 800x500 through the "sky cycle" profiler scope: 4.74 ms GPU on
 * an average slice frame here, 1.87 ms with the budget forced to 1 (one item
 * per frame, ~43 ticks instead of ~20).
 *
 * The difference is not headroom to claim: a mip-0 face is ONE item and
 * cannot be split at face granularity, so the per-frame PEAK is that face's
 * cost either way. Lowering the budget only spreads the cheap items thinner
 * and lengthens convergence. Going below that peak needs sub-face slicing,
 * which is a schedule change rather than a constant.
 */
#define SKY_SLICE_BUDGET 120000

#define SKY_SLICE_MAX_ITEMS 64
static SkySliceItem sky_slice_schedule[SKY_SLICE_MAX_ITEMS];
static int sky_slice_count = 0;

static void sky_slice_add(int kind, int mip, int face_first, int face_count, int weight) {
    if (sky_slice_count >= SKY_SLICE_MAX_ITEMS) {
        // Silent overflow of a file-static array is the one failure here that
        // corrupts rather than degrades, and the cap is only comfortable at
        // today's sizes: raising SKY_PREFILTER_MIPS walks straight into it.
        log_error("Sky slice schedule overflow (cap %d); raise SKY_SLICE_MAX_ITEMS",
                  SKY_SLICE_MAX_ITEMS);
        return;
    }
    SkySliceItem* it = &sky_slice_schedule[sky_slice_count++];
    it->kind = (unsigned char)kind;
    it->mip = (signed char)mip;
    it->face_first = (signed char)face_first;
    it->face_count = (signed char)face_count;
    it->weight = weight;
}

static void sky_slice_add_chain(int kind, int base, int mips) {
    for (int mip = 0; mip < mips; mip++) {
        int size = base >> mip;
        int face_weight = size * size * 2; // prefilter texels run importance loops
        // Split by face exactly when a whole mip would not fit the budget --
        // derived rather than written as a resolution, so changing the budget
        // moves the split with it instead of silently disagreeing.
        if (face_weight * 6 > SKY_SLICE_BUDGET) {
            for (int f = 0; f < 6; f++)
                sky_slice_add(kind, mip, f, 1, face_weight);
        } else {
            sky_slice_add(kind, mip, 0, 6, face_weight * 6);
        }
    }
}

static void sky_slice_build_schedule(void) {
    if (sky_slice_count)
        return;
    sky_slice_add(SKY_SLICE_LUT, 0, 0, 0, SKY_VIEW_W * SKY_VIEW_H);
    for (int f = 0; f < 6; f++)
        sky_slice_add(SKY_SLICE_ENV_FACE, 0, f, 1, SKY_ENV_SIZE * SKY_ENV_SIZE);
    sky_slice_add(SKY_SLICE_MIPGEN, 0, 0, 0, 90000);
    // 32^2 x 6 faces, but every texel integrates a hemisphere: weight the
    // kernel, not the resolution.
    sky_slice_add(SKY_SLICE_IRRADIANCE, 0, 0, 6, 120000);
    sky_slice_add_chain(SKY_SLICE_GGX, SKY_PREFILTER_SIZE, SKY_PREFILTER_MIPS);
    sky_slice_add_chain(SKY_SLICE_CHARLIE, IBL_CHARLIE_PREFILTER_SIZE,
                        IBL_CHARLIE_PREFILTER_MIPS);
    sky_slice_add(SKY_SLICE_SWAP, 0, 0, 0, 0);
}

// Start a sliced re-bake: latch the sun and the clouds decision, allocate the
// shadow targets. The frozen LUT is item 0's job, not this one's.
//
// Returns false and starts nothing when a program the schedule needs is
// missing: the slices themselves early-return SILENTLY, so a half-rendered
// shadow chain would be swapped in over a good live one -- where the atomic
// path returns -1 and installs nothing.
static bool sky_slicer_start(SkyAtmosphere* sky, const struct IBLResources* ibl) {
    if (!sky->view_program || !sky->env_program || !ibl->irradiance_program ||
        !ibl->prefilter_program || !ibl->charlie_prefilter_program)
        return false;
    sky_slice_build_schedule();
    glm_vec3_copy(sky->sun_dir, sky->slicer.latched_sun_dir);
    sky->slicer.clouds_bake = sky_clouds_bake_wanted(sky);
    if (sky->slicer.clouds_bake && !sky->env_clouds_program)
        return false;
    // Adopt the outgoing chain rather than allocating a new one: the two sets
    // already coexist for a whole cycle, so peak VRAM is unchanged and this
    // removes ~12.6 MB of allocate-and-free every cycle, forever. Safe because
    // the formats and sizes are identical by construction and the schedule
    // rewrites every (mip, face) of all four before the next swap.
    if (!sky->slicer.shadow_env)
        ibl_create_cubemap_texture(&sky->slicer.shadow_env, SKY_ENV_SIZE, true);
    if (!sky->slicer.shadow_irr)
        ibl_create_cubemap_texture(&sky->slicer.shadow_irr, IBL_IRRADIANCE_SIZE, false);
    if (!sky->slicer.shadow_prefilter)
        ibl_create_prefilter_cubemap(&sky->slicer.shadow_prefilter, SKY_PREFILTER_SIZE,
                                     SKY_PREFILTER_MIPS);
    if (!sky->slicer.shadow_charlie)
        ibl_create_prefilter_cubemap(&sky->slicer.shadow_charlie, IBL_CHARLIE_PREFILTER_SIZE,
                                     IBL_CHARLIE_PREFILTER_MIPS);
    // Set-and-leave, like the rest of the codebase's use of it: the slices
    // that follow all want it and nothing ever disables it.
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    sky->slicer.item = 0;
    sky->cycle_dirty = false;
    return true;
}

// Run one work item. Every piece is the atomic bake's own code aimed at the
// shadow handles. Returns true when a SWAP landed, which is the caller's cue
// to tell the scene its environment moved -- sky.c deliberately does not know
// what a Scene is (scene.h states that invariant).
static bool sky_slicer_run_item(SkyAtmosphere* sky, struct IBLResources* ibl,
                                const SkySliceItem* it) {
    switch ((SkySliceKind)it->kind) {
        case SKY_SLICE_LUT:
            // From the LATCHED sun, not the live one: every face of one cube
            // must see one sun, and leaving that to "item 0 happens to run in
            // the same tick as the start" is a seam waiting for a scheduler
            // change.
            sky_bake_view_lut_to(sky, &sky->slicer.frozen_lut, sky->slicer.latched_sun_dir);
            break;
        case SKY_SLICE_ENV_FACE:
            sky_env_face_slice(sky, ibl, sky->slicer.frozen_lut, sky->slicer.latched_sun_dir,
                               sky->slicer.shadow_env, sky->slicer.clouds_bake, it->face_first);
            break;
        case SKY_SLICE_MIPGEN:
            sky_env_mipgen(sky->slicer.shadow_env);
            break;
        case SKY_SLICE_IRRADIANCE:
            ibl_irradiance_slice(ibl, sky->slicer.shadow_env, sky->slicer.shadow_irr,
                                 SKY_ENV_SIZE);
            break;
        case SKY_SLICE_GGX:
            ibl_prefilter_slice(ibl, ibl->prefilter_program, sky->slicer.shadow_env,
                                sky->slicer.shadow_prefilter, SKY_PREFILTER_SIZE,
                                SKY_PREFILTER_MIPS, it->mip, it->face_first, it->face_count);
            break;
        case SKY_SLICE_CHARLIE:
            ibl_prefilter_slice(ibl, ibl->charlie_prefilter_program, sky->slicer.shadow_env,
                                sky->slicer.shadow_charlie, IBL_CHARLIE_PREFILTER_SIZE,
                                IBL_CHARLIE_PREFILTER_MIPS, it->mip, it->face_first,
                                it->face_count);
            break;
        case SKY_SLICE_SWAP: {
            // The two chains trade places -- together with max_reflection_lod,
            // the one non-texture field welded to the prefilter chain. The
            // outgoing set becomes the next cycle's shadow rather than dying.
            GLuint prev_env = ibl->environment_cubemap;
            GLuint prev_irr = ibl->irradiance_map;
            GLuint prev_pre = ibl->prefilter_map;
            GLuint prev_cha = ibl->charlie_prefilter_map;
            ibl->environment_cubemap = sky->slicer.shadow_env;
            ibl->irradiance_map = sky->slicer.shadow_irr;
            ibl->prefilter_map = sky->slicer.shadow_prefilter;
            ibl->charlie_prefilter_map = sky->slicer.shadow_charlie;
            ibl->max_reflection_lod = (float)(SKY_PREFILTER_MIPS - 1);
            sky->slicer.shadow_env = prev_env;
            sky->slicer.shadow_irr = prev_irr;
            sky->slicer.shadow_prefilter = prev_pre;
            sky->slicer.shadow_charlie = prev_cha;
            return true;
        }
    }
    return false;
}

void sky_cycle_request_rebake(SkyAtmosphere* sky) {
    if (!sky)
        return;
    sky->cycle_dirty = true;
    sky->slicer.item = -1; // restart from the top on the next tick
}

void sky_place_moon_from_clock(SkyAtmosphere* sky) {
    if (!sky || !sky->moon_enabled)
        return;
    sky_sun_path((double)sky->stars_latitude_deg, sky->cycle_hour - sky->cycle_moon_offset,
                 &sky->moon_elevation_deg, &sky->moon_azimuth_deg);
}

// Advance the clock and everything the sun drives per frame. CPU plus one
// small LUT bake; the heavy re-bake is the pump's job.
static void sky_cycle_advance(SkyAtmosphere* sky, float dt) {
    if (!sky->cycle_latched) {
        // The app's authored star-hour placement survives as the phase the
        // sky wheels from.
        sky->cycle_star_base = (double)sky->stars_hour_deg - (sky->cycle_hour - 12.0) * 15.0;
        sky->cycle_latched = true;
    }

    // Captured BEFORE the wrap: the moon's lag advances by a fraction of this
    // same interval, and cycle_hour's wrap destroys the evidence of how far it
    // moved.
    double advanced = 0.0;
    if (sky->cycle_day_seconds > 0.0f) {
        advanced = (24.0 / (double)sky->cycle_day_seconds) * (double)dt;
        sky->cycle_hour += advanced;
        while (sky->cycle_hour >= 24.0)
            sky->cycle_hour -= 24.0;
    }

    // The star field is a function of the HOUR, so it is written beside the
    // hour -- not inside the sun comparison below, where it would stall
    // whenever the sun's own float angles happened not to move (near noon at
    // low latitude both derivatives approach zero).
    sky->stars_hour_deg = (float)(sky->cycle_star_base + (sky->cycle_hour - 12.0) * 15.0);

    // The moon's lag, beside the hour it is a lag ON, and above the sun's
    // comparison for the star hour's reason stated one comment up.
    sky->cycle_moon_offset += advanced / SKY_SYNODIC_DAYS;
    while (sky->cycle_moon_offset >= 24.0)
        sky->cycle_moon_offset -= 24.0;

    /*
     * Derive-and-compare rather than track "did it move": float equality makes
     * a frozen, already-seeded cycle a true no-op, and a config restore that
     * changed the hour is picked up the same way.
     *
     * This is the SUN's early-out, not the function's -- it used to be a bare
     * `return` and everything below it inherited the stall the star hour was
     * hoisted above to avoid.
     */
    float el, az;
    sky_sun_path((double)sky->stars_latitude_deg, sky->cycle_hour, &el, &az);
    if (el != sky->sun_elevation_deg || az != sky->sun_azimuth_deg) {
        sky->sun_elevation_deg = el;
        sky->sun_azimuth_deg = az;
        sky_update_sun_dir(sky); // sun_dir + the zenith march
        sky_apply_sun_to_light(sky);
        sky->cycle_dirty = true;

        // The LIVE view LUT tracks the sun every tick -- the on-screen sky
        // moves continuously while the IBL converges behind it.
        sky_bake_view_lut(sky);
    }

    // No comparison guarding the moon, unlike the sun's above, because there
    // is nothing to protect: it is not in the view LUT and not in the env
    // cube, so a moon move costs two trig calls and a few float writes.
    sky_place_moon_from_clock(sky);
}

// Spend one frame's budget on the sliced re-bake. Separate from the advance
// because the two are genuinely different jobs on different clocks: a
// --cycle-rebake-at request pumps without advancing, and a frozen cycle
// advances without pumping.
static void sky_slicer_pump(SkyAtmosphere* sky, struct IBLResources* ibl, bool* out_swapped) {
    if (sky->slicer.item < 0 && !sky_slicer_start(sky, ibl))
        return;

    int spent = 0;
    while (sky->slicer.item >= 0) {
        const SkySliceItem* it = &sky_slice_schedule[sky->slicer.item];
        // Do not START an item that overshoots -- testing only "have I spent
        // the budget" lets a tick run a full item past it, and the heaviest
        // item is comparable to the budget itself, so the real ceiling would
        // be ~2x what the constant says. The `spent == 0` leg keeps a single
        // over-budget item from deadlocking the schedule.
        if (spent && spent + it->weight > SKY_SLICE_BUDGET)
            break;
        if (sky_slicer_run_item(sky, ibl, it))
            *out_swapped = true;
        spent += it->weight;
        sky->slicer.item++;
        if (sky->slicer.item >= sky_slice_count)
            sky->slicer.item = -1; // converged; restarts next tick if dirty
    }
}

bool sky_cycle_tick(SkyAtmosphere* sky, struct IBLResources* ibl, float dt) {
    if (!sky || !ibl || !sky->enabled)
        return false;
    // The slicer leans on state the startup atomic bake established; without
    // it there is nothing to slice against.
    if (!ibl->precomputed)
        return false;

    if (sky->cycle_enabled)
        sky_cycle_advance(sky, dt);
    else
        sky->cycle_latched = false;

    // Unconditional, and ABOVE the slicer's early-out rather than beside the
    // pump: the moon is not baked, so on most frames there is nothing to slice
    // and the return below is taken -- a moon update placed after it would run
    // only on baking frames and freeze in between.
    //
    // This is the whole moon update: direction, phase, tint and the light,
    // every frame, from whoever last wrote the two angles. It costs the change
    // detection and the apply hooks the sun needs, and it buys their absence.
    sky_update_moon(sky);

    // A requested re-bake pumps with the cycle OFF: that is the whole shape of
    // --cycle-rebake-at, a sliced pass over an unmoved sun.
    if (sky->slicer.item < 0 && !sky->cycle_dirty)
        return false;

    GLint prev_viewport[4];
    GLint prev_framebuffer;
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_framebuffer);

    bool swapped = false;
    sky_slicer_pump(sky, ibl, &swapped);

    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_framebuffer);
    glActiveTexture(GL_TEXTURE0);
    return swapped;
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
    // Stars fade in through the shared civil-twilight ramp. A visibility
    // ramp standing in for the exposure adaptation the engine refuses
    // (exposure_auto_gain caps at 1), not physics -- real stars are up all
    // day. Sampled live like the disc size; no bake depends on it.
    float night = sky_night_factor(sky);
    float star_intensity = sky->stars_enabled ? night * sky->stars_brightness : 0.0f;
    uniform_set_float(u, "starIntensity", star_intensity);
    // World dir -> celestial frame: tilt the celestial pole down to an
    // altitude equal to the latitude (toward +Z, the azimuth-0 north), then
    // turn about it by the hour angle. Column-major, applied as frame * dir.
    {
        mat4 tilt, spin, frame4;
        glm_rotate_make(tilt, glm_rad(sky->stars_latitude_deg - 90.0f), (vec3){1.0f, 0.0f, 0.0f});
        glm_rotate_make(spin, glm_rad(sky->stars_hour_deg), (vec3){0.0f, 1.0f, 0.0f});
        glm_mat4_mul(spin, tilt, frame4);
        mat3 frame;
        glm_mat4_pick3(frame4, frame);
        uniform_set_mat3(u, "starFrame", (float*)frame);
    }
    // The moon rides the same ramp the stars do, times the phase law -- so a
    // new moon and a daylit sky are both an exact zero and the disc block in
    // sky_radiance.glsl costs nothing. SKY_MOON_DISC_RADIANCE is the look
    // constant against sunIntensity's 20; the physical surface radiance is
    // ~1/400,000 of the sun's, which would be 5e-5 here and invisible.
    /*
     * The lunar surface. Baked on first use rather than at sky construction, so
     * a scene with no moon never pays for it, and never rebaked -- the Moon is a
     * constant body, not anything derived from the sun.
     *
     * GL_REPEAT on S because longitude wraps, CLAMP on T because latitude does
     * not: wrapping T mirrors the south pole onto the north and puts a seam
     * across the disc. MIPMAPPED, which is what keeps a small moon smooth -- the
     * crater field runs far finer than a few hundred pixels can carry, and an
     * unfiltered normal at that size boils.
     *
     * A failed bake leaves the handle at zero, which binds the default texture
     * and reads black: the disc goes dark rather than the frame going wrong.
     */
    if (!sky->moon_surface_tex) {
        unsigned char* surface = moon_surface_bake(MOON_SURFACE_W, MOON_SURFACE_H, 0);
        if (surface) {
            glGenTextures(1, &sky->moon_surface_tex);
            glBindTexture(GL_TEXTURE_2D, sky->moon_surface_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, MOON_SURFACE_W, MOON_SURFACE_H, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, surface);
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            free(surface);
        }
    }
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, sky->moon_surface_tex);
    uniform_set_int(u, "moonSurfaceTex", 3);
    uniform_set_vec3(u, "moonDir", sky->moon_dir);
    // The DRAWN radius: the shared physical angle times the moon's look scale.
    // Clamped only against a degenerate zero, which would divide by nothing in
    // the aureole's width.
    const float moon_size = fmaxf(sky->moon_size, 1e-3f);
    const float moon_radius_deg = sky->sun_disc_deg * 0.5f * moon_size;
    uniform_set_float(u, "moonCosRadius", cosf(glm_rad(moon_radius_deg)));
    /*
     * The aureole's width, and it grows as the SQUARE ROOT of the drawn size
     * rather than with it. A real halo is a couple of degrees around a
     * half-degree moon whatever the moon is doing -- roughly the radii factor
     * below -- so scaling it linearly with an oversize disc is licence chasing
     * licence: at twelve times life size that is a fifty-degree halo, which
     * floods the star field the moon is supposed to hang in front of.
     *
     * The root keeps a life-size moon physical (~2 degrees), lets an oversize
     * one keep a halo in proportion to look at, and reaches only ~7 degrees at
     * the GUI's largest setting.
     */
    uniform_set_float(u, "moonGlowAng",
                      glm_rad(moon_radius_deg / sqrtf(moon_size) * SKY_MOON_GLOW_RADII));
    uniform_set_float(u, "moonGlow", sky->moon_glow ? 1.0f : 0.0f);
    /*
     * No phase factor here, and its absence is the point. The DISC's brightness
     * distribution comes from the Lommel-Seeliger term in the shader, which
     * already dims toward new moon because that is what the geometry does --
     * multiplying by the Krisciunas-Schaefer factor as well counted the phase
     * twice and left a gibbous moon several times darker than it should be.
     *
     * The LIGHT still takes it, and must: that factor is the disc-INTEGRATED
     * brightness, which is exactly the quantity a directional needs and exactly
     * not the one a per-pixel shading model wants.
     */
    float moon_intensity =
        sky->moon_enabled ? night * sky->moon_brightness * SKY_MOON_DISC_RADIANCE : 0.0f;
    uniform_set_float(u, "moonIntensity", moon_intensity);
    uniform_set_float(u, "moonEarthshine", sky->moon_earthshine ? 1.0f : 0.0f);
    uniform_set_float(u, "moonMaria", sky->moon_maria ? 1.0f : 0.0f);
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
    sky_debug_draw(sky, sky->transmittance_lut, 10, screen_h - 10 - th, tw, th, 1.0f, false);
    int mw = SKY_MULTISCATTER_SIZE * 4;
    sky_debug_draw(sky, sky->multiscatter_lut, 10, screen_h - 20 - th - mw, mw, mw, 200.0f,
                   false);
    // Sky-view LUT below (mild boost to reveal the HDR horizon/zenith range)
    if (sky->sky_view_lut) {
        int sw = SKY_VIEW_W * 2, sh = SKY_VIEW_H * 2;
        sky_debug_draw(sky, sky->sky_view_lut, 10, screen_h - 30 - th - mw - sh, sw, sh, 0.3f,
                       false);
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

        // The sun-transmittance map below them, and the only 2D LUT in this stack that had
        // no viewer. Without it a failing cloudshadow-vary cannot be told apart from a
        // failing lookup, which is the bisect --no-cloud-shadows exists to serve.
        // Transmittance is already [0,1], so no boost; mono because it is R16F.
        if (sky->clouds.shadow_tex) {
            int hw = CLOUD_SHADOW_DEBUG_W;
            sky_debug_draw(sky, sky->clouds.shadow_tex, cx, screen_h - 30 - cw - dw - hw, hw, hw,
                           1.0f, true);
        }
    }

    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (depth_was)
        glEnable(GL_DEPTH_TEST);
    if (blend_was)
        glEnable(GL_BLEND);
    glUseProgram(0);
}
