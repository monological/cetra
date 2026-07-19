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

struct Engine;
struct IBLResources;
struct Light;

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

    GLuint transmittance_lut; // 256x64  RGBA16F, baked once
    GLuint multiscatter_lut;  // 32x32   RGBA16F, baked once
    GLuint sky_view_lut;      // 192x108 RGBA16F, re-baked per sun move (M2)

    GLuint quad_vao, quad_vbo;

    ShaderProgram* transmittance_program;
    ShaderProgram* multiscatter_program;
    ShaderProgram* debug_program;
    ShaderProgram* view_program;
    ShaderProgram* env_program;
    ShaderProgram* background_program;

    // Cube-face capture scaffolding (shares the IBL toolkit's shape)
    GLuint capture_fbo, capture_rbo;
    GLuint cube_vao, cube_vbo;

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

// Draw the procedural sky as the frame background (sky-view LUT + analytic
// sun disc), replacing render_skybox in sky mode. Strips translation from
// view like the skybox path.
void sky_render_background(SkyAtmosphere* sky, mat4 view, mat4 projection);

// Debug: blit the LUTs into the bottom-left corner of the default
// framebuffer (transmittance above multiscatter), scaled up for
// inspection. Call after the frame is composited.
void sky_debug_blit_luts(SkyAtmosphere* sky, int screen_w, int screen_h);

#endif // _SKY_H_
