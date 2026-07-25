#ifndef _SKY_H_
#define _SKY_H_

#include <GL/glew.h>
#include <cglm/cglm.h>
#include <stdbool.h>

#include "program.h"

// Procedural physically-based sky (Hillaire EGSR 2020): transmittance +
// multiple-scattering + sky-view LUTs baked as fullscreen raster passes
// (GL 4.1, no compute), rendered into the engine's normal environment
// cubemap so IBL, reflections, the probe, and the fog publish all follow
// the sun for free. The atmosphere itself (Earth: Rg 6360 km / Rt 6460 km,
// Rayleigh/Mie/ozone profiles) is defined as constants inside the sky_*
// shaders — kilometers everywhere for fp16/float safety.
//
// LUT bake cadence: transmittance and multiple-scattering are
// sun-INDEPENDENT (baked once); the sky-view LUT and everything derived
// from it (environment cubemap, irradiance, prefilter) re-bake when the
// sun moves.

#define SKY_TRANSMITTANCE_W   256
#define SKY_TRANSMITTANCE_H   64
#define SKY_MULTISCATTER_SIZE 32
#define SKY_VIEW_W            192
#define SKY_VIEW_H            108
// The sky is smooth, low-frequency content: a small environment chain is
// indistinguishable from the HDR-file sizes and keeps sun-move re-bakes
// cheap. max_reflection_lod follows via ibl_bake_from_cubemap.
#define SKY_ENV_SIZE       256
#define SKY_PREFILTER_SIZE 256
#define SKY_PREFILTER_MIPS 7

// Aerial-perspective volume (spec 9.6). Small on purpose: the in-scatter it
// carries is smooth in all three axes, so resolution buys nothing a trilinear
// tap does not already give, and the volume is rebuilt every frame. RGBA16F at
// 32^3 is 256 KB.
#define SKY_AERIAL_X 32
#define SKY_AERIAL_Y 32
#define SKY_AERIAL_Z 32
// How far the volume reaches, in KILOMETRES. Past this the composite's
// CLAMP_TO_EDGE on R holds the fully integrated column, which is the right
// answer anyway -- by 128 km a surface has converged on the sky behind it.
#define SKY_AERIAL_FAR_KM 128.0f

struct Engine;
struct IBLResources;
struct Light;
struct PostFX;

typedef struct SkyAtmosphere {
    bool enabled;
    bool debug_luts; // blit the LUTs onto the composited frame

    // Sun placement (degrees; the app and GUI own these). Azimuth 0 = +Z,
    // increasing toward +X. Disc size is the angular DIAMETER.
    float sun_elevation_deg;
    float sun_azimuth_deg;
    float sun_disc_deg;
    vec3 sun_dir; // unit vector TOWARD the sun (derived; sky_update_sun_dir)

    // Optional coupling to the scene's directional key light (set by the app
    // once; NULL = pure-IBL sky). sky_apply_sun_to_light retints/redirects it
    // from the atmosphere so a sun move drives the shadows and fog too.
    struct Light* sun_light;
    float sun_base_intensity; // key-light intensity at full elevation

    // World units per kilometre. The atmosphere model is in km (Rg/Rt in
    // include/atmosphere.glsl) while a scene is in whatever units it was
    // authored in, so nothing can march a real distance without this mapping.
    // 1000 = one unit is one metre, the glTF convention. Deliberately not
    // scene-scaled: how big a scene IS in the world is an authoring fact, not
    // something to infer from its bounding radius -- a prop shot really does
    // sit under 0.1% extinction, and should look like it.
    float world_units_per_km;

    // Drive the fog's ambient in-scatter from the sky instead of leaving it at
    // the app-set default. Cleared once anyone else takes ownership of the
    // value (the GUI colour picker does, on edit).
    bool publish_fog_ambient;
    vec3 zenith_radiance; // Cached sky ambient; recomputed on sun move only

    GLuint transmittance_lut; // 256x64  RGBA16F, baked once
    GLuint multiscatter_lut;  // 32x32   RGBA16F, baked once
    GLuint sky_view_lut;      // 192x108 RGBA16F, re-baked per sun move (M2)

    // Aerial perspective (spec 9.6): a camera-frustum volume of
    // (in-scatter, transmittance). Unlike every LUT above it depends on the
    // CAMERA, so it is the one sky target rebuilt every frame; at 32^3 that is
    // 0.5 MB and 32 tiny draws.
    bool aerial_enabled; // App toggle; the volume is simply not built when off
    bool aerial_failed;  // One-shot: allocation is not retried every frame
    GLuint aerial_lut;
    GLuint aerial_fbo; // attachment-less; one layer bound per slice draw
    ShaderProgram* aerial_program;

    GLuint quad_vao, quad_vbo;

    ShaderProgram* transmittance_program;
    ShaderProgram* multiscatter_program;
    ShaderProgram* debug_program;
    ShaderProgram* view_program;
    ShaderProgram* env_program;
    ShaderProgram* background_program;

    // Env-face capture (FBO/RBO) and the unit cube are borrowed from the
    // passed IBLResources at bake time (the probe sibling's reuse pattern),
    // so the sky owns none of that scaffolding itself.

    bool luts_baked; // the static (sun-independent) LUT pair is valid
} SkyAtmosphere;

SkyAtmosphere* create_sky_atmosphere(void);
void free_sky_atmosphere(SkyAtmosphere* sky);

// Derive sun_dir from sun_elevation_deg / sun_azimuth_deg
void sky_update_sun_dir(SkyAtmosphere* sky);

// CPU evaluation of the atmospheric transmittance toward the sun at the
// current sun elevation (the same integral the transmittance LUT bakes),
// giving the sun's color: white at the zenith, reddening toward the
// horizon. Zero below the horizon. Used to tint the directional key light
// so the analytic sun matches the visible sky. No GPU readback.
void sky_sun_transmittance(const SkyAtmosphere* sky, vec3 out_color);

// Bake the sun-independent LUTs (transmittance, then multiple-scattering
// which samples it). One-time; logs timings. Requires the sky_* programs
// to be registered with the engine.
int sky_bake_static_luts(SkyAtmosphere* sky, struct Engine* engine);

// Re-bake everything the sun drives: sky-view LUT -> environment cubemap
// (+ mips) -> ibl_bake_from_cubemap (irradiance + prefilter). Populates the
// passed IBLResources so the whole downstream (skybox/IBL/probe/fog) follows
// the sun. Call after sky_bake_static_luts and after setting sun_dir.
int sky_bake(SkyAtmosphere* sky, struct IBLResources* ibl, struct Engine* engine);

// Apply the current sun to the coupled key light (sky->sun_light): direction
// away from the sun, color from atmospheric transmittance, intensity faded to
// zero as the disc sinks below the horizon (shadows off once it fades out).
// No-op when no light is coupled. The single owner of the sun->light policy,
// shared by the app's setup and the GUI's live re-bake.
void sky_apply_sun_to_light(SkyAtmosphere* sky);

// One "the sun moved" entry point: re-derive sun_dir, re-bake everything the
// sun drives (sky_bake) and retint the coupled key light. Used by the GUI's
// dynamic sun; cheap enough (small env) to run live per slider change.
int sky_update_sun(SkyAtmosphere* sky, struct IBLResources* ibl, struct Engine* engine);

// Rebuild the aerial-perspective volume for this frame's camera. Unlike the
// other bakes this is per-frame, because the volume is the camera's frustum.
// Requires the static LUTs; a no-op without them.
void sky_update_aerial(SkyAtmosphere* sky, mat4 view, mat4 projection);

// Flatten the aerial volume (or its absence) into postfx's per-frame state,
// the shadow/probe publish shape; postfx never learns about Sky. A zero handle
// is the single "no aerial perspective" state consumers rely on.
void sky_publish_to_postfx(const SkyAtmosphere* sky, struct PostFX* fx);

// Draw the procedural sky as the frame background (sky-view LUT + analytic
// sun disc), replacing render_skybox in sky mode. Strips translation from
// view like the skybox path. Borrows the unit cube from ibl (populated during
// sky_bake).
void sky_render_background(SkyAtmosphere* sky, struct IBLResources* ibl, mat4 view,
                           mat4 projection);

// Debug: blit the LUTs into the bottom-left corner of the default
// framebuffer (transmittance above multiscatter), scaled up for
// inspection. Call after the frame is composited.
void sky_debug_blit_luts(SkyAtmosphere* sky, int screen_w, int screen_h);

#endif // _SKY_H_
