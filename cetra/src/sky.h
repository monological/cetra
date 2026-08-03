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

// Cloud noise fields (spec 11.0). Tiling RGBA8 volumes, CPU-baked once at
// startup (sun-independent): shape carries Perlin-Worley coverage in R and
// three inverted-Worley octaves in GBA; detail carries three finer Worley
// octaves that erode cloud edges. 128^3 + 32^3 is ~8.4 MB -- far too big to
// check in, cheap to re-derive (every voxel is a pure function of its
// coordinate and a fixed seed, so the bake threads over Z-slabs and the
// bytes are identical at any thread count).
#define SKY_CLOUD_SHAPE_SIZE  128
#define SKY_CLOUD_DETAIL_SIZE 32

typedef struct CloudLayer {
    bool enabled;     // master switch; off = no bake, no GL objects, no cost
    float coverage;   // 0..1 sky fraction the remap admits
    float cloud_type; // 0 = low flat stratus .. 1 = tall cumulus
    float density;    // extinction scale on the march
    int debug_shell;  // 1 = march pass renders shell distances (R5 probe)

    // Drift. The scroll accumulator advances once per march: a fixed 1/60
    // when headless (goldens stay byte-deterministic -- the skeletal-
    // animation clock model, never the wall clock that makes wind scenes
    // unmeasurable), render_delta otherwise. Default speed 0 = still air.
    float wind_speed_kmh;
    float wind_dir_deg; // 0 = +Z, matching the sun azimuth convention
    double scroll;      // accumulated drift time, seconds

    GLuint shape_tex;  // 128^3 RGBA8 tiling volume
    GLuint detail_tex; // 32^3  RGBA8 tiling volume
    bool noise_baked;

    // Half-internal-res march targets, written pre-scene each frame and
    // sampled by the clouds background variant. A parity ping-pong (frame &
    // 1): the march blends last frame's result in-place via ray-direction
    // reprojection, so there is no separate accumulation pass. Lazily
    // allocated.
    GLuint march_fbo[2];
    GLuint march_tex[2];
    int march_w, march_h;
    int march_read;   // index the composite samples (written this frame)
    bool march_valid; // a march ran this session (gates the composite)
    ShaderProgram* march_program;

    // The march's OWN previous camera + adjacency stamp (the froxel-fog
    // 9.5.1 shape): sky pixels carry zero velocity, so the shared
    // velocity-reprojecting accumulators cannot serve, and by the time any
    // postfx state is written engine->prev_view_proj already holds THIS
    // frame. Rotation-only view (clouds reproject by ray direction --
    // translation parallax is accepted, bounded by the blend).
    mat4 prev_view; // rotation-only world->view of the previous march
    float prev_focal[2];
    int prev_frame; // total_frames of the previous march; -1 = none
} CloudLayer;

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
    ShaderProgram* env_clouds_program;
    ShaderProgram* background_program;
    ShaderProgram* background_clouds_program;
    ShaderProgram* cloud_noise_debug_program;

    // Env-face capture (FBO/RBO) and the unit cube are borrowed from the
    // passed IBLResources at bake time (the probe sibling's reuse pattern),
    // so the sky owns none of that scaffolding itself.

    CloudLayer clouds;

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

// CPU-bake the cloud noise fields and upload them (sky_clouds.c). One-time,
// threaded, gated on clouds.enabled; logs timing like the static LUT bake.
// Sun- and coverage-independent: never re-baked after this.
int sky_bake_cloud_noise(SkyAtmosphere* sky);

// March the cloud shell for this frame's camera into the half-res target
// (sky_clouds.c). Runs in the pre-scene window -- before the main FBO
// binds -- because the background pass samples the result mid-scene.
// No-op unless clouds are enabled and the noise is baked.
void sky_clouds_march(SkyAtmosphere* sky, struct Engine* engine, mat4 view, mat4 projection);

// Re-bake everything the sun drives: sky-view LUT -> environment cubemap
// (+ mips) -> ibl_bake_from_cubemap (irradiance + GGX and Charlie
// prefilters). Populates the passed IBLResources so the whole downstream
// (skybox/IBL/probe/fog) follows the sun. Call after sky_bake_static_luts
// and after setting sun_dir. with_clouds marches the cloud layer into the
// env faces (24 steps/texel) so IBL and probes see the deck -- the
// release/startup cadence; the plain sky_bake below is the per-drag path
// and never pays it.
int sky_bake_ex(SkyAtmosphere* sky, struct IBLResources* ibl, struct Engine* engine,
                bool with_clouds);
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
// sky_bake). with_clouds composites the half-res cloud march over the sky
// via a separate program (the plain path's shader stays untouched); pass
// false during probe/GI captures -- the march texture is the main camera's.
void sky_render_background(SkyAtmosphere* sky, struct IBLResources* ibl, mat4 view, mat4 projection,
                           bool with_clouds);

// Debug: blit the LUTs into the bottom-left corner of the default
// framebuffer (transmittance above multiscatter), scaled up for
// inspection. Call after the frame is composited.
void sky_debug_blit_luts(SkyAtmosphere* sky, int screen_w, int screen_h);

#endif // _SKY_H_
