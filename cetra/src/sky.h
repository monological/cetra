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
// Side of the cloud-shadow debug tile. Fixed rather than a multiple of the map's own 256,
// which would overflow the column it shares with the noise fields.
#define CLOUD_SHADOW_DEBUG_W 192

typedef struct CloudLayer {
    bool enabled;     // master switch; off = no bake, no GL objects, no cost
    float coverage;   // 0..1 sky fraction the remap admits
    float cloud_type; // 0 = low flat stratus .. 1 = tall cumulus
    float density;    // extinction scale on the march

    // Drift. The scroll accumulator advances by render_delta once per march;
    // the engine's frame clock makes that a fixed step headless (goldens stay
    // byte-deterministic) and honestly frozen under a paused embedder clock.
    // Default speed 0 = still air.
    float wind_speed_kmh;
    float wind_dir_deg; // 0 = +Z, matching the sun azimuth convention
    double scroll;      // accumulated drift time, seconds

    GLuint shape_tex;  // 128^3 RGBA8 tiling volume
    GLuint detail_tex; // 32^3  RGBA8 tiling volume
    bool noise_baked;

    // Half-internal-res march targets, written each frame and sampled by
    // the clouds background variant. A parity ping-pong (frame & 1): the
    // march blends last frame's result in-place via ray-direction
    // reprojection, so there is no separate accumulation pass. Lazily
    // allocated.
    GLuint march_fbo[2];
    GLuint march_tex[2];
    int march_w, march_h;
    ShaderProgram* march_program;

    // The march's OWN previous camera + adjacency stamp (the froxel-fog
    // 9.5.1 shape): sky pixels carry zero velocity, so the shared
    // velocity-reprojecting accumulators cannot serve, and by the time any
    // postfx state is written engine->prev_view_proj already holds THIS
    // frame. Rotation-only view (clouds reproject by ray direction --
    // translation parallax is accepted, bounded by the blend). prev_frame
    // is the SINGLE march-history record: -1 = no march yet (gates the
    // composite), otherwise the frame of the last march, whose parity is
    // the texture index it wrote.
    mat4 prev_view; // rotation-only world->view of the previous march
    float prev_focal[2];
    int prev_frame;

    /*
     * Sun transmittance through the deck, as a 2D map the froxel fog reads (spec 11.39).
     *
     * Built in the same call as the march and from the same wind offset, which is the reason
     * it lives here rather than beside the fog that consumes it: the drift clock advances
     * exactly once per frame, so a second pass computing its own offset would put the shadow
     * a frame out of step with the deck it belongs to.
     *
     * WORLD-anchored while the deck is camera-anchored -- see cloud_shadow_frag.glsl for why a
     * shadow cannot follow the camera when the sky may. It needs no window and no camera
     * following: with detail off the density field is exactly periodic over the shape noise's
     * own tile, so ONE tile sampled GL_REPEAT is the whole world, not a crop of it.
     */
    bool shadows_enabled; // rides the master switch; --no-cloud-shadows clears it
    GLuint shadow_fbo;
    GLuint shadow_tex; // R16F transmittance, 1 = full sun, wrapped
    ShaderProgram* shadow_program;
    // The inputs the last build used. The map is a pure function of these -- deliberately
    // camera-free, which is what lets it be world-anchored AND what makes this cache exact.
    // Wind is still by default, so without it a static sky re-marches an identical 256^2
    // texture every frame for the life of the run.
    float shadow_inputs[8];
    bool shadow_built;
    // Published to PostFX in WORLD units, so the consumer's shear needs no conversion.
    float shadow_tile;    // world units the map's period covers
    float shadow_shell_y; // world Y of the shell bottom, the altitude the map is indexed at
} CloudLayer;

struct Engine;
struct IBLResources;
struct Light;
struct PostFX;

typedef struct SkyAtmosphere {
    bool enabled;
    bool debug_luts; // blit the LUTs onto the composited frame

    // Sun placement (degrees; the app and GUI own these). Azimuth 0 = +Z,
    // increasing toward +X. Disc size is the angular DIAMETER, and since
    // 11.82 it sizes BOTH sky bodies: the moon subtends 0.52 degrees against
    // the sun's 0.53, which is why total eclipses work at all, and a second
    // field for that difference would be a knob nobody can see.
    float sun_elevation_deg;
    float sun_azimuth_deg;
    float sun_disc_deg;
    vec3 sun_dir; // unit vector TOWARD the sun (derived; sky_update_sun_dir)

    // Night star field (spec 11.79), screen-space background only -- never the
    // env cube or the IBL, the sun-disc rule. Visibility ramps in through
    // civil twilight from the sun elevation; the ramp stands in for an
    // exposure adaptation the engine deliberately does not perform
    // (exposure_auto_gain only ever darkens). The latitude sets the celestial
    // pole's altitude and the hour turns the sky about it.
    //
    // The stars_ prefix is now HISTORY, not scope: since 11.81 the latitude is
    // the observer's, read by sky_sun_path to place the SUN on the same
    // celestial frame, and stars_hour_deg is written by the cycle's tick. The
    // authored spellings (environment.stars.latitude, the sky.stars_latitude
    // snapshot row) are what pin the C names -- renaming those breaks files.
    bool stars_enabled;
    float stars_brightness; // star radiance scale at full night (1 = default)
    float stars_latitude_deg;
    float stars_hour_deg;

    // The night-sky floor (spec 11.80): the sky between the stars, and what a
    // moonless world is lit by. Baked into the sky-view LUT, so it reaches
    // the background, the env cube, the IBL and the cloud ambient through
    // the samplers they already have; the same civil-twilight ramp as the
    // stars fades it in. Editing either field requires the sun-move re-bake
    // chain (sky_update_sun), not a live uniform.
    bool night_floor_enabled;
    float night_floor_brightness; // scale on the baked radiance (1 = default)

    // The day/night cycle (spec 11.81). One clock: cycle_hour (0-24, solar
    // noon at 12, a DOUBLE so the gate's Python twin reproduces it exactly)
    // drives the sun along the celestial equator of the stars' own latitude
    // frame, and the star field's hour advances in lock-step. day_seconds is
    // real seconds per full day; 0 = frozen (the clock holds, and once
    // converged the tick is a structural no-op -- which is also what keeps
    // config-perturb's enabled-flip round-trippable).
    bool cycle_enabled;
    float cycle_day_seconds;
    double cycle_hour;
    // Owned by the tick while the cycle runs: the star-hour offset latched on
    // the enable edge, so an app-authored band placement survives as the
    // phase the sky wheels from.
    bool cycle_latched;
    double cycle_star_base;
    // True when the sun has moved past what the last completed sliced bake
    // captured; cleared when a fresh slice cycle starts.
    bool cycle_dirty;
    // Hours the moon LAGS the sun, advancing at the synodic rate while the
    // clock runs (24 hours of lag per 29.53 simulated days). One rate behind
    // two facts, because they are the same fact: the moon transits ~50
    // minutes later each day, and its phase evolves through the month. 0 =
    // new (the moon at the sun), 12 = full (opposite it).
    double cycle_moon_offset;

    /*
     * The moon (spec 11.82). Mirrors the sun above, with two asymmetries that
     * are the whole reason this block is short.
     *
     * The PHASE is derived from the two directions and stored nowhere
     * (sky_moon_phase_factor): it is what decides the terminator, the lit
     * fraction and the brightness together, so a stored copy would be a
     * second place for it to go stale -- and a stale phase still renders a
     * plausible moon, which is the worst kind of wrong. The disc SIZE is
     * sun_disc_deg, for the reason stated up there.
     *
     * Nothing here is baked. The disc is analytic in the two background
     * shaders only and the energy ships as moon_light -- the sun-disc firefly
     * rule (sky_env_frag.glsl) for the third time, after the sun and the
     * stars -- so every field is a live uniform or a per-frame light rewrite,
     * and none of them re-bake anything. That is what lets sky_update_moon
     * run unconditionally instead of needing change detection.
     */
    bool moon_enabled;
    float moon_brightness; // the ONE look scale; drives the disc AND the light
    float moon_elevation_deg;
    float moon_azimuth_deg;
    vec3 moon_dir; // unit vector TOWARD the moon (derived; sky_update_moon)
    // Optional coupling to a second directional, the sun_light pattern below.
    // NULL = the disc without the light, which is a legitimate configuration:
    // an app that wants a moon in the sky and no second caster just never
    // sets it.
    struct Light* moon_light;

    /*
     * The time-sliced env re-bake (spec 11.81). The atomic bake costs ~0.11 s
     * -- ~90% of it the two prefilter chains, NOT the six env faces -- so the
     * cycle spreads the same work items across frames into SHADOW textures
     * and swaps them into IBLResources atomically. `item` is the cursor into
     * the generated work schedule (-1 = idle); the frozen LUT is what the six
     * env faces sample, because all faces of one cube must see ONE sun while
     * the live sky_view_lut keeps tracking the sun per frame.
     */
    struct {
        int item; // -1 idle, else next work item
        GLuint frozen_lut;
        GLuint shadow_env;
        GLuint shadow_irr;
        GLuint shadow_prefilter;
        GLuint shadow_charlie;
        vec3 latched_sun_dir;
        bool clouds_bake;
    } slicer;

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
    GLuint lut_fbo; // reused by every 2D LUT bake; the view LUT re-bakes per frame

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

// Derive moon_dir from moon_elevation_deg / moon_azimuth_deg and push the
// result onto moon_light. Called UNCONDITIONALLY once per frame.
//
// Paying four trig calls and a pow every frame is what lets the CLI, a scene
// file, the GUI sliders, a config restore and the cycle all write the two
// angles with no change detection, no deferred re-bake flags and no apply
// hooks. It is affordable only because nothing the moon touches is baked --
// and it is NECESSARY rather than merely convenient, because the moon's
// brightness is a function of the SUN's direction, so every site that moves
// the sun also changes the moon.
void sky_update_moon(SkyAtmosphere* sky);

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

// March the cloud shell into the half-res target (sky_clouds.c). Call once
// per frame with the SAME view/projection the sky background will
// composite with -- a camera latched anywhere earlier in the frame is one
// app-update stale, which turns the temporal reprojection into an identity
// map. Binds its own FBO and restores the caller's GL state; no-op unless
// clouds are enabled and the noise is baked.
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

// Apply the current moon to the coupled second directional (sky->moon_light),
// the sun's policy above with the phase folded in. No-op when no light is
// coupled. Reached through sky_update_moon rather than called directly, so no
// site that moves the sun can forget that the moon's brightness depends on it.
void sky_apply_moon_to_light(SkyAtmosphere* sky);

// The moon's brightness as a fraction of a full moon's, from the current sun
// and moon directions. 1 at full, ~0.09 at quarter, ~0 at new -- a real moon
// is NOT a Lambertian sphere, and this is the whole visual signature.
float sky_moon_phase_factor(const SkyAtmosphere* sky);

// One "the sun moved" entry point: re-derive sun_dir, re-bake everything the
// sun drives (sky_bake) and retint the coupled key light. Used by the GUI's
// dynamic sun; cheap enough (small env) to run live per slider change.
int sky_update_sun(SkyAtmosphere* sky, struct IBLResources* ibl, struct Engine* engine);

// The sun's position at `hour` (0-24, solar noon 12) on the celestial
// equator of the given latitude -- the equinox path, which is what makes
// noon altitude 90-lat and sunrise/set land at 6/18. DOUBLE domain
// throughout, converting to float only at the end: the cycle-quiesce gate
// arm holds this against a Python twin computed in doubles, and a float32
// formula loses the bit-identity that comparison rests on.
void sky_sun_path(double latitude_deg, double hour, float* out_elevation_deg,
                  float* out_azimuth_deg);

// The per-frame heart of the day/night cycle (spec 11.81), called by the
// engine loop BEFORE the GI sweep and the shadow pass -- the key light it
// rewrites is what the cascades then fit, and a completed slice's swap must
// land before the frame's first bind_ibl_textures. A structural no-op when
// the cycle is off or frozen-and-converged.
// Returns true when a sliced re-bake COMPLETED this frame, which is the
// caller's cue to re-derive whatever else the environment feeds (the GI
// volume). Reported rather than done here because sky.c deliberately does not
// know what a Scene is -- see scene_environment_changed's own comment.
bool sky_cycle_tick(SkyAtmosphere* sky, struct IBLResources* ibl, float dt);

// Ask the slicer for a fresh pass over the CURRENT sun, without moving it --
// the diagnostic `--cycle-rebake-at` drives this, and it is what lets a
// sliced bake be compared against the startup atomic one with no hour
// arithmetic anywhere in the comparison.
void sky_cycle_request_rebake(SkyAtmosphere* sky);

// Rebuild the aerial-perspective volume for this frame's camera. Unlike the
// other bakes this is per-frame, because the volume is the camera's frustum.
// Requires the static LUTs; a no-op without them.
void sky_update_aerial(SkyAtmosphere* sky, mat4 view, mat4 projection);

// Flatten the aerial volume (or its absence) into postfx's per-frame state,
// the shadow/probe publish shape; postfx never learns about Sky. A zero handle
// is the single "no aerial perspective" state consumers rely on.
void sky_publish_to_postfx(const SkyAtmosphere* sky, struct PostFX* fx);

// Bind the cloud deck's sun-transmittance map to `unit` and upload the shell terms that
// address it. Sky owns the "is there a deck" test, so a call site cannot get the off state
// wrong: with no deck the light slot uploads as -1 and every consumer returns full sun.
void sky_bind_cloud_shadow(const SkyAtmosphere* sky, ShaderProgram* program, int unit);

// Where a program with a spare unit takes that map. 14 is the skybox/GI atlas number, which
// neither consumer samples -- chosen over 11 because water routes through the shared
// bind_ibl_textures, and that points irradianceMap at 11 whether water declares it or not.
// pbr_frag is the exception and reaches the map through TEXUNIT_SCENE_COLOR by alias, having
// no seventeenth declaration to spend; that choice lives with the tenant routing in render.c.
#define SKY_CLOUD_SHADOW_UNIT 14

// Draw the procedural sky as the frame background (sky-view LUT + analytic
// sun disc), replacing render_skybox in sky mode. Strips translation from
// view like the skybox path. Borrows the unit cube from ibl (populated during
// sky_bake). main_camera = this draw is not a capture: the cloud composite
// (when the layer is enabled and has marched) samples a screen-space
// texture that only the main camera's rays match, so capture passes pass
// false and get the plain sky.
void sky_render_background(SkyAtmosphere* sky, struct IBLResources* ibl, mat4 view, mat4 projection,
                           bool main_camera);

// Debug: blit the LUTs into the bottom-left corner of the default
// framebuffer (transmittance above multiscatter), scaled up for
// inspection. Call after the frame is composited.
void sky_debug_blit_luts(SkyAtmosphere* sky, int screen_w, int screen_h);

#endif // _SKY_H_
