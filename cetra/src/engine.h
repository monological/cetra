#ifndef _ENGINE_H_
#define _ENGINE_H_

/*
 * The engine: one window with its GL context, the HDR multisampled G-buffer
 * the scene renders into, the post chain that turns it into a frame, the
 * program cache, the camera, and the frame loop that runs an app's three
 * hooks around all of it (specs 11.106-11.108).
 *
 * Created from an EngineConfig, which carries what the window and the first
 * render-target build read; NULL means every default. After that the loop
 * is engine_run, and everything the frame consults it reads at the frame top
 * from the Engine's own fields -- so an app that wants the wireframe, the FPS
 * counter, a frame limit or a clear colour writes the field and nothing else;
 * the struct's banner says which few fields are not like that.
 */

#include <stdint.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "scene.h"
#include "camera.h"
#include "program.h"
#include "common.h"
#include "input.h"
#include "postfx.h"

typedef enum CameraMode {
    CAMERA_MODE_FREE,  // Free movement mode
    CAMERA_MODE_ORBIT, // Orbit around a point
} CameraMode;

/*
 * How the window relates to a display.
 *
 * Neither non-windowed mode CHANGES a video mode: both take the monitor at the
 * mode it is already in, so nothing here can strand a display in a resolution
 * the desktop did not choose, and there is no mode to restore after a crash.
 * The performance lever is render_scale, which costs no display-mode change at
 * all -- resolution switching would be a second lever fighting the first.
 *
 * The difference between the two is only whether the monitor is TAKEN:
 * borderless is an undecorated window sized to the monitor, so alt-tab is
 * immediate; fullscreen hands GLFW the monitor and may drop the swap interval.
 *
 * A mode may be APPENDED but never inserted. These are persisted by name at
 * these indices, so a value slotted into the middle silently re-points every
 * settings file already written.
 */
typedef enum EngineWindowMode {
    ENGINE_WINDOW_WINDOWED = 0,
    ENGINE_WINDOW_FULLSCREEN, // exclusive, at the monitor's current video mode
    ENGINE_WINDOW_BORDERLESS, // undecorated window filling the monitor
    ENGINE_WINDOW_MODE_COUNT,
} EngineWindowMode;

// A window or monitor rectangle in SCREEN COORDINATES (points), which is the
// space GLFW places windows in -- not the framebuffer pixels fb_width counts.
typedef struct EngineWindowRect {
    int x, y, w, h;
} EngineWindowRect;

// Held by pointer and named only here, so their headers stay out of every
// file that includes this one. An app that reaches into one includes it.
struct Engine;
struct LightClusterContext;
struct Profiler;
struct Ubo;
typedef struct AsyncLoader AsyncLoader;
typedef struct TextRenderer TextRenderer;
typedef struct LTCTables LTCTables;

typedef void (*CursorPositionCallback)(struct Engine* engine, double xpos, double ypos);
typedef void (*MouseButtonCallback)(struct Engine* engine, int button, int action, int mods);
typedef void (*KeyCallback)(struct Engine* engine, int key, int scancode, int action, int mods);
typedef void (*ScrollCallback)(struct Engine* engine, double xoffset, double yoffset);

// An animation clock an embedder can substitute for the wall clock (see
// Engine.render_clock). The embedder owns the storage and keeps it current;
// engine_run samples it once per frame.
typedef struct EngineFrameClock {
    double time;  // the instant this frame renders at
    double delta; // advance since the previous frame; 0 if it did not advance
} EngineFrameClock;

// The fixed per-frame step every headless clock advances by: frame N of a
// headless run is instant N * this, every run, so renders compare
// frame-for-frame. Also the game framework's default sim timestep.
#define ENGINE_FIXED_FRAME_DT (1.0 / 60.0)

typedef struct Engine {
    // SETTINGS throughout, in feature order, except:
    //
    // ENGINE-OWNED, read only: the window and its sizes, every GL name, every
    // *_actual / *_ready / target_* record of what was built, the matrices,
    // the frame clock and counters, the program cache, the async loader, the
    // input state, and the owned subsystems (postfx, the text renderer, the
    // profiler). What an app may change among them has a function:
    // engine_set_camera, engine_add_scene and the scene selectors,
    // engine_add_program.
    //
    // BY FUNCTION: msaa_samples, ss_scale and render_scale (each clamps and
    // rebuilds the targets at the next frame top), window_mode (moves the
    // window, keeping the windowed rectangle current), vsync (re-applied when a
    // mode change drops it), screenshot_path (owned
    // string), the five callbacks and user_data (installs), and the animation
    // clock (engine_set_render_clock samples a borrowed one each frame, and
    // the overlay, engine_set_overlay),
    // engine_set_render_time sets a frame's directly for a loop that is not
    // engine_run).
    //
    // Everything else -- the feature toggles, the draw levers, the overlays,
    // the run's counts, clear_color, camera_mode, current_render_mode -- is
    // written directly and read at the frame top. The exposure block has
    // banners of its own.
    GLFWwindow* window;
    char* window_title; // Title of the GLFW window

    int win_width;
    int win_height;
    int fb_width;
    int fb_height;
    EngineWindowMode window_mode; // engine_set_window_mode
    bool vsync;                   // false = swap without waiting for the display; engine_set_vsync
    // The display the window was last placed on, NULL while windowed. Held so a
    // repeat of the mode it already has can be answered without moving it, and
    // so a change of monitor WITHIN a mode is still a move -- borderless hands
    // GLFW no monitor, so the mode alone cannot tell the two apart.
    GLFWmonitor* window_monitor;
    bool window_mode_refused; // a mode was asked for with no window to move; said once
    // Where the window sits while it is windowed, replayed when it comes back.
    // Seeded from the config and refreshed on every mode call made while
    // windowed -- a player drags a window, so a rectangle sampled once on the
    // way out would restore it to somewhere it left long ago.
    EngineWindowRect windowed;
    GLint max_texture_image_units;   // GL_MAX_TEXTURE_IMAGE_UNITS (queried at init)
    GLint max_array_texture_layers;  // GL_MAX_ARRAY_TEXTURE_LAYERS (mask array budget)
    GLint max_texture_size;          // GL_MAX_TEXTURE_SIZE (composite-cache bound)
    bool layers_vt_enabled;          // false = every layered material takes the per-texel blend
    int layers_vt_res;               // composite-cache resolution override; 0 = derived
    bool layers_vt_pages_enabled;    // false = the fallback atlas alone (stage 1 exactly)
    bool layers_vt_feedback_enabled; // false = residency runs on prediction alone
    int layers_vt_page_slots;        // physical page slots in use; 0 = all of them
    int layers_vt_page_budget;       // page bakes per frame; 0 = the default
    int layers_vt_probe_interval;    // print page residency every N frames; 0 = off
    struct LayersVtFeedback* vt_feedback; // the vote pass's targets + readback ring
    int ss_scale;                         // Supersampling factor: scene + post render at ss_scale x
                                          // display resolution, box-downsampled at tone map.
                                          // 1 = off, 2 = 2x SSAA. engine_set_ss_scale clamps it
                                          // and the targets rebuild at the next frame top.
    float render_scale;                   // Render-resolution scale in [0.5, 1]: the scene + the
                                          // pre-TAA post chain rasterize at this fraction of the
                                          // post size. 1 = off. engine_set_render_scale clamps it
                                          // and the targets rebuild at the next frame top.
    bool render_suspended;                // A render-target rebuild failed; frames are skipped
                                          // until a size that allocates successfully is set
    // The size the render targets were last built AT, or last attempted at.
    // The frame-top sync compares against these, so it both detects a change
    // and declines to retry a size that already failed.
    int target_render_w, target_render_h;
    int target_post_w, target_post_h;
    // MSAA sample count REQUESTED for the scene framebuffer (1 = off, 4 = 4x).
    // Runtime-changeable via engine_set_msaa_samples.
    //
    // A request the allocator honours at 1: a single-sample request gets a
    // plain GL_TEXTURE_2D target (spec 11.34), where it used to get a
    // multisample one the driver silently rounded up to 2. Above 1 the driver
    // may still adjust the count, so anything reasoning about what the target
    // actually holds reads msaa_samples_actual; this field is which AA mode was
    // chosen.
    int msaa_samples;
    // What the driver returned for the request above, read back from the scene
    // FBO at every build. Equal to the request at 1 by construction now; above 1
    // it is still the driver's answer, not the request's promise.
    int msaa_samples_actual;

    GLFWerrorfun error_callback;

    CursorPositionCallback cursor_position_callback;
    MouseButtonCallback mouse_button_callback;
    KeyCallback key_callback;
    ScrollCallback scroll_callback;

    GLuint framebuffer; // Framebuffer object
    // The *_multisample_texture names predate spec 11.34: at msaa_samples == 1
    // these are plain GL_TEXTURE_2D targets, multisample only above that. The
    // attachment layout and every consumer (all blit-side) are the same either
    // way, which is why the fields keep their names.
    GLuint multisample_texture;        // HDR color (attachment 0)
    GLuint normal_multisample_texture; // Multisample view-space normal .xyz + SSR reflective marker
                                       // .a (0 model / -1 floor / +alpha A2C) (attachment 1)
    GLuint aux_multisample_texture;    // Multisample aux G-buffer: motion .xy + linear view-Z .z +
                                       // effective roughness .w (attachment 2)
    GLuint albedo_multisample_texture; // Multisample base color for SSGI (attachment 3)
    GLuint
        sss_diffuse_multisample_texture; // Multisample skin diffuse irradiance for SSS (attachment
                                         // 4); 0 off-skin, so it doubles as the SSS mask
    GLuint spec_multisample_texture;     // Multisample ambient specular (attachment 7; slots 5/6
                                         // belong to the OIT FBO's attachments)
    GLuint depth_renderbuffer;           // Depth renderbuffer
    // Refraction source: mid-frame resolve of the opaque scene into a mipped
    // RGBA16F texture, created lazily on the first transmissive frame
    GLuint opaque_color_fbo;
    GLuint opaque_color_texture;
    int opaque_color_w, opaque_color_h;
    // Soft-particle source: mid-frame resolve of the scene depth into a
    // single-sample depth texture (the MSAA depth renderbuffer isn't sampleable),
    // created lazily on the first engine_resolve_scene_depth call.
    GLuint scene_depth_fbo;
    GLuint scene_depth_texture;
    int scene_depth_w, scene_depth_h;
    // Weighted-blended OIT: a lazily-created multisample FBO that shares
    // depth_renderbuffer (so transparent frags depth-test against opaque geometry
    // without writing depth). accum (color attachment 5, RGBA16F) sums premultiplied
    // weighted color; revealage (attachment 6, R16F) multiplies the alpha product.
    // Only allocated/drawn when oit_enabled; postfx resolves + composites them.
    GLuint oit_fbo;
    GLuint oit_accum_multisample_texture;
    GLuint oit_revealage_multisample_texture;
    int oit_w, oit_h;
    // Moment-based OIT (spec 11.17): a second FBO, same shape, that the
    // generation sub-pass sums absorbance moments into -- b1..b4 at attachment
    // 5, the total b0 at attachment 6 -- before the accumulate sub-pass weights
    // colour by them.
    //
    // They land in one ATLAS rather than staying two textures because the
    // accumulate reads them from pbr_frag, which already declares all sixteen
    // fragment samplers the driver allows -- and the driver counts DECLARED
    // samplers, not distinct units, so there is no seventeenth even for a unit
    // that is provably free. The moments have to arrive through the one sampler
    // that is idle in that sub-pass, which is four floats against their five.
    // Hence the atlas: twice the render height, b1..b4 in the lower half and b0
    // in the upper.
    GLuint moment_fbo;
    GLuint moment_multisample_texture;    // attachment 5: b1..b4
    GLuint moment_b0_multisample_texture; // attachment 6: b0 (.r)
    GLuint moment_atlas_fbo;
    GLuint moment_atlas_texture;
    int moment_w, moment_h;
    // Clustered-forward lighting (spec 9.1). The module owns its own GPU
    // buffers and scratch; rebuilt per engine_render_scene invocation.
    struct LightClusterContext* light_cluster;
    // CPU masked occlusion culling (spec 11.98). Fixed working set, no GL;
    // rebuilt per camera frame, skipped under captures.
    struct OcclusionContext* occlusion;
    // ViewParams (spec 10.1): the working-space contract every pass writing
    // scene radiance reads. Engine-owned rather than PostFX-owned because it
    // must be live during the SCENE passes, which run before postfx does.
    struct Ubo* view_ubo;
    // Per-instance transforms for batched draws (spec 11.28). Engine-owned for
    // the same reason as view_ubo: the scene passes and the depth pass both
    // fill it, and it must outlive either.
    struct Ubo* instance_ubo;
    // The composite cache's page table (spec 11.67). Engine-owned and single --
    // v1 pages one material per scene, which is what lets the buffer sit on its
    // binding for the context's lifetime per ubo.h's contract.
    struct Ubo* vt_pages_ubo;
    // Road segments (spec 11.68), single for the same reason: v1 carries one
    // road-bearing material per scene. The shadow copy is what makes the upload
    // by-value -- roads have three writers and no dirty flag.
    struct Ubo* roads_ubo;
    GpuRoadsBlock roads_shadow;
    bool instancing_enabled; // false = every run submits one draw per mesh
    bool lod_enabled;        // false = every draw takes LOD level 0
    float lod_bias;          // > 1 holds detail longer, < 1 drops it sooner
    // false = no frustum rejection anywhere -- camera, shadow cascades and the
    // TSM walks alike, since all four build their view through the same helper.
    // A bisect lever, not a feature: culling only ever removes geometry that
    // contributed nothing, so this is expected to be 0 px and its job is to say
    // so when it is not.
    bool frustum_cull_enabled;
    // false = no occlusion rejection (spec 11.98). The same kind of lever as
    // frustum_cull_enabled and held to the same bar: a conservative cull is
    // expected to be 0 px, and this is what says so when it is not. Camera pass
    // only, so shadow layers and captures are identical either way.
    bool occlusion_cull_enabled;
    // false = every vertex draws its own surface, never its parent's -- the
    // CDLOD morph off, in all five geometry programs at once. A bisect lever
    // whose whole purpose is to be VISIBLE: it is the only thing that makes the
    // morph reachable from a rendered frame, which the probe's own arithmetic
    // cannot be.
    bool morph_enabled;
    // true = the opaque lane is drawn coarsely front-to-back, grouped by
    // material and mesh, instead of in graph order. Opaque geometry is
    // order-independent, so this may not move a pixel; it moves what early-Z can
    // reject and how often the material block uploads.
    bool opaque_sort_enabled;
    // true = opaque geometry that cannot discard writes depth in a cheap
    // position-only pass first, so the shading pass rejects hidden fragments
    // before the uber-shader runs. Alpha-masked materials sit it out.
    bool depth_prepass_enabled;
    // Scratch for the above, owned by the engine because it is rebuilt every
    // pass and freed once. Never the list the other passes read. By pointer
    // so the list's type stays internal.
    DrawList* sorted_opaque;
    bool cluster_debug;          // Tint fragments by cluster light count (heatmap)
    bool scene_color_this_frame; // Resolve ran this frame; transmissive draws may sample
    bool normals_this_frame;     // Attachment 1 written this frame (PBR + consumer active)
    bool aux_this_frame;         // Attachment 2 written this frame (TAA needs motion, or GTAO needs
                                 // linear-Z)
    bool albedo_this_frame;      // Attachment 3 (albedo) written this frame (SSGI active)
    bool sss_this_frame;         // Attachment 4 (skin diffuse) written this frame (SSS active)
    bool spec_this_frame;        // Attachment 7 (ambient specular) written this frame (split mode)
    bool oit_this_frame;         // OIT accumulate pass ran this frame (postfx composites oit_fbo)
    bool moments_this_frame;     // Moment generation ran, so the accumulate weighted by measured
                             // transmittance rather than the depth curve (changes the composite)
    bool taa_jitter_this_frame; // The TAA accumulator is live and the projection is jittered
    // false = the jittered alpha lookup (spec 11.101) stays on the plain fetch
    // even while TAA accumulates. It exists so the crawling Moire the jitter
    // dissolves -- and the churn it cuts -- can be re-measured on demand; a
    // GUI control and a snapshot row like any other, since a session that
    // turned it off is a session the snapshot claims to reproduce.
    bool alpha_jitter_enabled;

    // The scene is being rendered into an offscreen capture target (a probe face,
    // a GI probe face) rather than into engine->framebuffer. Set for the duration
    // of the capture and restored after.
    //
    // A capture target carries colour and nothing else, and it is single-sample.
    // Any pass that reaches outside the bound target, or that keys off the SCENE
    // framebuffer's shape rather than the bound one, must consult this.
    bool capturing;

    // Narrower than `capturing`: true only while a capture whose output is
    // IRRADIANCE runs -- one whose result is ADDED to the analytic direct term
    // rather than shown to an eye. A derived emissive panel (spec 11.49) sits
    // those out, because it already delivers that light analytically; measured at
    // 1.31x on the cornell floor before it did.
    //
    // Set by scene_capture_begin from the caller's stated SceneCaptureKind, which
    // is where the two-callers-want-opposite-things argument lives (render.h).
    // Not set here by hand: it is capture policy, and render.h's own history
    // records what happened the last time a caller wrote its own.
    bool capturing_irradiance;

    Camera* camera;         // The camera the frame renders (engine_set_camera); borrowed
    CameraMode camera_mode; // Free or orbit

    Scene** scenes;             // Array of scenes managed by the engine
    size_t scene_count;         // Number of scenes
    size_t current_scene_index; // Index of the currently active scene

    ShaderProgram** programs;   // Global shader programs used across scenes
    size_t program_count;       // Count of global programs
    ShaderProgram* program_map; // name to program cache

    Material** materials;  // Global materials used across meshes
    size_t material_count; // Count of global materials

    RenderMode current_render_mode; // default is PBR

    float specular_aa_strength;   // Geometric specular AA (0 disables)
    bool energy_comp_enabled;     // Multi-scatter specular energy compensation;
                                  // inert without an IBL environment (needs the BRDF LUT)
    bool refraction_enabled;      // Screen-space refraction for transmissive materials;
                                  // off = no resolve, and the shader treats transmission
                                  // as 0 (glass renders as a plain lit surface)
    bool clearcoat_enabled;       // KHR_materials_clearcoat second specular lobe; off
                                  // skips the lobe (materials with clearcoat 0 are
                                  // unaffected either way)
    bool specular_enabled;        // KHR_materials_specular F0 tint + specular weight; off
                                  // leaves the base dielectric BRDF unchanged (materials
                                  // without the extension are unaffected either way)
    bool sheen_enabled;           // KHR_materials_sheen cloth lobe; off skips the lobe
                                  // (materials with sheen color 0 are unaffected either way)
    bool parallax_enabled;        // POM height-march (§4.11); off skips the march
                                  // (materials with no height map / scale 0 are unaffected)
    bool sss_enabled;             // Separable screen-space SSS; off skips the diffuse
                                  // separation + blur (materials with subsurface 0 are unaffected)
    bool skin_preint_enabled;     // Pre-integrated skin diffuse (§11.13); off keeps the clamped
                                  // Lambert falloff (materials with curvature_scale 0 are
                                  // unaffected either way)
    bool oit_enabled;             // Weighted-blended OIT for ALPHA_BLEND meshes (--oit); off keeps
                                  // the unsorted alpha-blend late pass
    bool oit_moments_enabled;     // Weight the OIT accumulate by absorbance moments (spec 11.17)
                                  // instead of the depth curve; inert unless oit_enabled
    bool emissive_lights_enabled; // Derive an LTC area panel from every emissive mesh
                                  // (spec 11.49). Off by default: emissive over a black base
                                  // is how a constant-colour surface is authored, so most
                                  // emissive geometry in the wild is not a lamp.

    mat4 view_matrix;
    mat4 projection_matrix; // Un-jittered truth: frustum culling, motion vectors
    mat4 view_proj;         // Un-jittered projection*view for the current frame, computed once in
                            // engine_render_scene (frustum + motion vectors); the scene draws with
    // a locally sub-pixel-jittered projection derived from projection_matrix
    mat4 draw_projection; // The projection this frame actually rasterized with (jittered under
                          // TAA, == projection_matrix otherwise). Postfx passes reconstructing
                          // positions from the depth buffer must use THIS one: marching rays
                          // with the un-jittered matrix flips marginal hits every jitter phase,
                          // and TAA bakes the flicker into stationary crosshatch.
    mat4 prev_view_proj;  // Previous frame's view_proj, for motion vectors
    // Where the camera was last frame, in storage space. The CDLOD morph is a
    // function of the camera, so a static surface really does move when the
    // camera does; this is what lets the previous-frame position report it.
    // Stashed beside prev_view_proj, from the same camera it was built from.
    vec3 prev_camera_position;
    // Whether the above has ever been written. Without it frame 0 reports the
    // camera as having travelled from the world ORIGIN, so every morphing vertex
    // gets a velocity the length of the camera's position -- the same defect
    // SceneNode.prev_valid exists for, on the other half of the same subtraction,
    // and it would be strange for the branch to answer it one way for nodes and
    // another way here.
    bool prev_camera_valid;

    // What the scene target is cleared to each frame, linear, where nothing
    // draws: the backdrop of a scene with no sky. 0.1 grey by default; write
    // it any time.
    vec3 clear_color;

    // The overlays, plain writes: the engine re-applies each from its field at
    // the frame top, so nothing has to run when one changes.
    bool show_gui;
    bool show_wireframe; // Every edge of every face, albedo-only, no culling
    bool show_xyz;       // The XYZ gizmo on every node, drawn with the scene's xyz program
    bool show_fps;
    bool show_camera_hud; // Live camera pose overlay next to the FPS readout
    bool show_bones;      // X-ray bone visualization
    bool show_lights;     // Light overlay: position cross + cull-radius wireframe
    bool headless;        // Hidden window, no vsync, fixed frame clock
    bool headless_jitter; // Apply the TAA sub-pixel jitter even in headless (non-deterministic
                          // screenshots, but lets temporal accumulation converge for verification)

    // Latched at NewFrame time: an ImGui frame is open this iteration and must
    // be closed with a matching igRender. Pairs the begin/end across the loop.
    bool gui_frame_active;

    // The frame's animation clock. Every pass that animates from time reads
    // these two, so they cannot disagree -- wind displaces the shadow caster and
    // the visible surface identically only because both read here, and the
    // previous-frame position used for motion vectors is exactly
    // render_time - render_delta.
    //
    // Both are latched by engine_run alone, once per frame, always before the
    // shadow pass. With no substituted clock the latch runs BEFORE the update
    // callback (so app hooks tick particles and animation from this frame's
    // clock), producing the wall clock live and a fixed
    // total_frames * ENGINE_FIXED_FRAME_DT headless -- which is what makes
    // headless wind/particle/animation output a pure function of the frame
    // index. A substituted clock latches AFTER the update callback instead,
    // so the embedder's sim has advanced. They are deliberately not
    // last_frame_time/delta_time -- those are FPS bookkeeping on the wall
    // clock, always, even headless.
    double render_time;
    double render_delta;

    // Substitutes an embedder's clock for the wall clock. Borrowed, sampled once
    // per frame; NULL means wall clock. The game framework points this at its
    // fixed-timestep sim clock, which is why a paused game freezes its wind.
    //
    // Sampled rather than pushed on purpose: a writer would have to reach every
    // control path through the update callback, and game_frame_update already
    // has an early return (escape key) that a store would silently skip.
    const EngineFrameClock* render_clock;

    // Drawn after tone mapping, before the debug GUI (engine_set_overlay).
    // NULL is the entire off state -- the call site is a pointer compare, so a
    // frame with no overlay is the frame that existed before this hook did.
    void (*overlay)(struct Engine* engine, void* user);
    void* overlay_user;

    // The run's settings. The path is owned (engine_set_screenshot_path); the
    // two counts are plain writes, and anything not above zero is off.
    char* screenshot_path; // If set, save final frame here on exit (PPM)
    int screenshot_every;  // Also save numbered frames every N frames
    int exit_after_frames; // Close the loop after N frames, with the final screenshot
    size_t total_frames;   // Monotonic frame counter for the render loop
    void* user_data;       // Opaque context for engine_run's callbacks (GLFW-style)

    // Bone visualization
    ShaderProgram* bone_program;
    GLuint bone_line_vao;
    GLuint bone_line_vbo;
    // The XYZ axis gizmo: one buffer of three lines, drawn under each node's
    // own transform for every node that shows it.
    GLuint xyz_vao;
    GLuint xyz_vbo;

    // LTC area-light lookup tables (spec 9.2). Static fitted data, independent
    // of scene or environment.
    LTCTables* ltc;

    // Split-sum BRDF tables (GGX A/B in RG, Charlie sheen E in blue), baked
    // once at init; environment-independent, like the LTC tables
    GLuint brdf_lut;

    // Depth prepass (spec 11.30): position-only program, built lazily on the
    // first frame the pass runs so an engine that never enables it pays nothing.
    ShaderProgram* depth_prepass_program;
    bool depth_prepass_failed; // true = compile was tried and failed; do not retry

    // Shadow catcher (ground plane that receives shadows over the skybox)
    ShaderProgram* shadow_catcher_program;
    GLuint catcher_vao;
    GLuint catcher_vbo;

    // HDR post-processing (bloom + tone mapping)
    PostFX* postfx;

    // Per-pass GPU time, CPU time and submission counts (specs 11.27, 11.28).
    // NULL unless the config asked for it, and every entry point no-ops on
    // NULL, so an ordinary run issues no query calls and keeps no counts at all.
    struct Profiler* profiler;

    // The camera's exposure -- read by render.c when it publishes ViewParams,
    // and by PostFX, which runs the metering that feeds it. On the Engine rather
    // than inside PostFX because it defines the working space the SCENE passes
    // write into, which happens before any post runs.
    //
    // By value: it is not optional, and a pointer made it look like it was.
    // Every reader then guarded, differently -- one skipped the ViewParams
    // upload entirely, which would have left preExposure at the UBO's calloc'd
    // zero and rendered a black frame as the fallback for an invariant that
    // cannot fail.
    Exposure exposure;

    InputState input;

    // FPS tracking
    double last_frame_time;
    double delta_time;
    float fps;
    float fps_update_timer;
    int frame_count;

    // Async loading
    AsyncLoader* async_loader;

    // Text rendering
    TextRenderer* text_renderer;
} Engine;

// Unified main-loop callbacks (engine_run). `update` runs once per frame
// before the render, with the produced frame dt (wall clock live,
// ENGINE_FIXED_FRAME_DT headless) -- the sim input a fixed-step accumulator
// consumes; a hook that animates scene content reads
// engine->render_time/render_delta instead. Pass NULL for a pure render loop
// -- the engine already ticks the scene's particle systems when no render
// clock is installed. `render` draws the scene -- its output is
// scene-referred linear HDR (bloom/exposure/tone mapping run in the present
// pass; only the GUI draws after tone mapping).
// `pre_render` is where an app puts everything that has to be settled BEFORE
// the frame's geometry is read: its camera, and any node it adds, removes or
// moves. The engine propagates the graph immediately after it returns, so a
// patch attached here gets a global transform this frame, and the shadow pass
// and the LOD selection below both see this frame's positions rather than last
// frame's (spec 11.96). The frame renders the camera as it stands when the hook
// returns: the engine derives the view and projection matrices right after it,
// so a pose is written and nothing else (spec 11.107). Nothing in it may draw
// -- there is no bound target yet.
//
// Mutating the graph LATER than this -- from `render` -- is recoverable rather
// than fatal: call scene_propagate_transforms again and the new node gets its
// global. It is still the wrong place, because everything between the two reads
// the graph as it stood here, so the shadow pass and the LOD selection will not
// see the change until the next frame. A camera pose written there is simply a
// frame late.
typedef void (*EngineUpdateFunc)(Engine* engine, float dt);
typedef void (*EnginePreRenderFunc)(Engine* engine, Scene* scene);
typedef void (*EngineRenderFunc)(Engine* engine, Scene* scene);

// The one place an app may draw AFTER tone mapping (engine_set_overlay). It
// runs at the end of the present pass, once the post chain has written the
// display-resolution frame, and before the debug GUI -- which is a developer's
// overlay and belongs on top of the game's.
//
// It exists because `render` is not this. That hook draws into the MSAA HDR
// scene target at RENDER resolution, so what it emits is graded, bloomed,
// grained, temporally filtered and rescaled by --render-scale: right for scene
// content, wrong for a menu, whose text should reach the display as authored
// and at the display's own resolution.
//
// It is the frame's last draw before the screenshot read-back, so what it
// emits is captured headless -- which is what lets an overlay be a golden.
typedef void (*EngineOverlayFunc)(Engine* engine, void* user);

// What an engine is created from: the window, and every setting that the
// window or the first render-target build reads. Fill the fields you mean with
// designated initialisers and leave the rest zero; zero is the default, named
// beside each field. The config exists because these settings have an ORDER --
// a hidden window is decided when the window is made, the profiler is built
// during init, the sample count sizes the first target -- and a struct that
// init reads is an order nothing can violate from outside.
typedef struct EngineConfig {
    const char* title;            // window title; NULL = "Cetra"
    int width, height;            // window size; 0 = 1280 x 720
    bool headless;                // hidden window, vsync off, fixed frame clock
    bool headless_jitter;         // keep the TAA jitter under headless (non-deterministic frames)
    bool no_vsync;                // swap without waiting for the display; headless implies it
    bool profiler;                // build the per-pass profiler
    bool taa;                     // temporal anti-aliasing, applied once the post chain is up
    int msaa_samples;             // scene target sample count; 0 = 4; 1 = off
    int ss_scale;                 // supersampling factor, clamped to [1, 2]; 0 = 1
    float render_scale;           // render-resolution scale in [0.5, 1]; 0 = 1
    EngineWindowMode window_mode; // 0 = windowed; the window is shown already in it
    const char* monitor;          // display name for a non-windowed mode; NULL = primary
} EngineConfig;

// Creates the window, the GL context, the scene target and the post chain, and
// registers the built-in programs, all in one call. NULL means every default.
// Returns NULL with the reason logged when any of that fails.
Engine* create_engine(const EngineConfig* cfg);
void free_engine(Engine* engine);

// Supersampling factor (clamped to [1, 2]). The render targets are rebuilt at
// the next frame top.
void engine_set_ss_scale(Engine* engine, int ss_scale);
// Render-resolution scale (clamped to [0.5, 1]). The render targets are rebuilt
// at the next frame top. Forced to 1 in headless without headless_jitter,
// which TAAU needs to reconstruct from.
void engine_set_render_scale(Engine* engine, float render_scale);
// Re-centre the world on the camera, snapped to `lattice` (spec 11.62). Applies
// at the next frame top like the render-scale switch above, and only on X and Z.
//
// The one place the snap is spelled, so a caller driving this by hand and the
// automatic threshold cannot disagree about where the origin lands -- they did,
// and the hand-driven copy was the one missing the current origin.
void engine_recentre_on_camera(const Engine* engine, float lattice);
// MSAA sample count for the scene framebuffer (clamped to [1, driver max]).
// 1 disables MSAA. Rebuilds the multisample attachments.
void engine_set_msaa_samples(Engine* engine, int samples);

// More displays than anyone attaches; a machine with more is warned and sees
// the first of them.
#define ENGINE_MAX_MONITORS 8

/*
 * The display names this machine has, in GLFW's order, index 0 being the
 * primary. `out_count` receives the length, which is 0 where no display is
 * attached -- a headless session or a VM genuinely reports that, so no caller
 * may assume index 0 exists.
 *
 * Borrowed: the strings and the array are the engine's and stay valid until a
 * monitor disconnects. A name, not an index, is what a caller persists -- an
 * index renumbers the moment a display is unplugged, and the game silently
 * opens on a different screen than the one that was chosen.
 */
const char* const* engine_monitor_names(const Engine* engine, int* out_count);

/*
 * What a stored name MEANS: the index of the display answering to it, or 0 --
 * the primary -- for NULL, "", and any name nothing answers to. -1 only when no
 * display is attached at all.
 *
 * One function rather than one here and one in whatever draws the picker, or a
 * settings screen could show a display the window is not on: the fallback is
 * the half that is easy to write differently twice.
 */
int engine_monitor_index(const Engine* engine, const char* name);

/*
 * Where a window goes for a mode: a PURE function of the mode, the monitor's
 * rectangle and the geometry the window left windowed mode with. No GLFW, no
 * Engine, no display.
 *
 * Separate from the move because the two answer different questions and only
 * this one has an answer that can be stated: the move is whatever GLFW does
 * with it.
 */
typedef struct EngineWindowPlacement {
    bool fullscreen; // hand GLFW the monitor rather than placing a window
    bool decorated;
    EngineWindowRect rect;
} EngineWindowPlacement;

EngineWindowPlacement engine_window_placement(EngineWindowMode mode, EngineWindowRect monitor,
                                              EngineWindowRect saved);

/*
 * Move the window between windowed, fullscreen and borderless. `monitor` names
 * the display for the two non-windowed modes; NULL, "", or a name no display
 * answers to all select the primary, which is what makes an unplugged monitor
 * degrade instead of stranding the window somewhere invisible.
 *
 * Idempotent: asking for the mode and monitor the window already has does
 * nothing. That matters because the geometry replayed on a windowed call is
 * what a DEPARTURE recorded, so a call that changes nothing would still move
 * the window -- and a caller pushing a whole settings struct on every edit is
 * the ordinary shape.
 *
 * Refused where there is no window to move: a hidden window has no display to
 * fill, and putting one on a monitor would make a frame's size depend on the
 * machine rather than on what was asked for.
 *
 * Writes none of win_width/win_height/fb_width/fb_height. GLFW fires the
 * framebuffer-size callback, which is their only writer after init; setting
 * them here would make that two writers and render at a size no target was
 * built at.
 */
void engine_set_window_mode(Engine* engine, EngineWindowMode mode, const char* monitor);

/*
 * The swap interval. Held rather than passed straight to GLFW because taking a
 * monitor can drop it, and a value nothing recorded cannot be put back.
 * Headless swaps without waiting whatever is asked.
 */
void engine_set_vsync(Engine* engine, bool vsync);
// The flat-colour preset for a 2D scene. Everything that describes a lens or
// an atmosphere goes off -- bloom, GTAO, SSR, vignette, dither, TAA, shadows --
// exposure pins at unity with adaptation off, the tone curve is the identity,
// and the scene's ambient radiance becomes white, under which a material's
// albedo reaches the display as authored with no light in the scene at all.
// Left alone on purpose: the sample count, since multisampling is the
// anti-aliasing 2D line art wants, the clear colour, the overlays, and any
// light the app adds on top, which then adds to the flat colour rather than
// replacing it. All of it is plain fields, so an app that wants one back
// writes it after the call. The scene half is skipped when `scene` is NULL.
void engine_set_2d_preset(Engine* engine, Scene* scene);
// Where the final frame is written on exit, PPM; NULL clears it. Owned.
void engine_set_screenshot_path(Engine* engine, const char* path);
// Installs the post-tone-map overlay draw; NULL clears it. Borrowed `user`.
void engine_set_overlay(Engine* engine, EngineOverlayFunc overlay, void* user);

// GLFW callbacks
void engine_set_error_callback(Engine* engine, GLFWerrorfun error_callback);
void engine_set_mouse_button_callback(Engine* engine, MouseButtonCallback mouse_button_callback);
void engine_set_cursor_position_callback(Engine* engine,
                                         CursorPositionCallback cursor_position_callback);
void engine_set_key_callback(Engine* engine, KeyCallback key_callback);
void engine_set_scroll_callback(Engine* engine, ScrollCallback scroll_callback);
// True when the GUI is capturing the pointer this frame; apps gate 3D input on it.
bool engine_gui_wants_mouse(void);
// The keyboard equivalent. The engine already applies this to the app's key CALLBACK, but an
// app that POLLS key state per frame -- which is what a held movement key has to be -- needs to
// ask the same question itself, or typing in a slider also walks.
bool engine_gui_wants_keyboard(void);

// The camera the frame renders; refuses NULL.
void engine_set_camera(Engine* engine, Camera* camera);

// Scene
void engine_add_scene(Engine* engine, Scene* scene);
void engine_set_scene_by_index(Engine* engine, size_t scene_index);
void engine_set_scene_by_name(Engine* engine, const char* scene_name);
Scene* engine_get_scene(const Engine* engine);

// Shader Programs. The engine takes ownership of an added program. The
// names create_engine registers for apps are the CETRA_PROGRAM_* constants in
// program.h; a program an app builds is registered under whatever name it
// was created with.
void engine_add_program(Engine* engine, ShaderProgram* program);
// NULL and a log line when no program has that name: the answer to a typo.
ShaderProgram* engine_get_program(Engine* engine, const char* program_name);
// NULL and silence: for asking whether an optional program exists.
ShaderProgram* engine_find_program(Engine* engine, const char* program_name);

// Opaque context for engine_run's callbacks (like glfwSetWindowUserPointer): the
// render callbacks stay untyped (Engine*, Scene*); a caller that needs its own
// state (e.g. the game framework) stashes it here and reads it back in the callback.
// NOTE: game_run reserves this slot for its Game* -- a game app must NOT set it
// (use game_set_user_data / game->user_data for app state instead).
void engine_set_user_data(Engine* engine, void* user_data);
void* engine_get_user_data(const Engine* engine);

// The unified main loop: owns the frame skeleton (dt, FPS, GUI frame, transform
// propagation, shadow pass, G-buffer setup, present, screenshot, swap). The
// three hooks run in the order they are declared -- `update` once per frame
// before anything reads the scene, `pre_render` after the sky and origin shift
// and immediately before the graph is propagated, `render` to draw. Any of them
// may be NULL, and a NULL `render` draws the scene itself (engine_render_scene);
// a hook there is for what an app does around that draw. The render apps and
// the game framework's game_run all drive the engine through it.
void engine_run(Engine* engine, EngineUpdateFunc update, EnginePreRenderFunc pre_render,
                EngineRenderFunc render);

// Draws `scene` through every scene pass, into the engine's scene target. What
// a `render` hook calls with the scene it was handed, which is the one the
// engine just propagated. Animation time comes from engine->render_time,
// latched once per frame before any pass, so this and the shadow depth pass
// cannot disagree about where wind-displaced geometry is.
void engine_render_scene(Engine* engine, Scene* scene);

// Substitute an animation clock for the wall clock. `clock` is borrowed and must
// outlive the loop; engine_run samples it each frame after the update callback
// and before the shadow pass. NULL restores the wall clock.
void engine_set_render_clock(Engine* engine, const EngineFrameClock* clock);

// Set the frame's animation clock directly, for callers that do not go through
// engine_run: an app driving engine_render_scene from its own loop, or a
// capture that pins the clock. `delta` is how far `time` advanced since the last
// render -- 0 for a frozen or first frame, so the wind's previous position
// equals its current one and motion vectors come out zero.
//
// A foreign loop that never calls this renders at t = 0 forever: wind holds
// still, silently. Call it once per frame BEFORE the shadow pass, or the depth
// and shading passes displace wind from different instants.
void engine_set_render_time(Engine* engine, double time, double delta);

// The cursor in FRAMEBUFFER pixels with +Y up -- the space the input state,
// the app callbacks and the helper below all speak -- from the window
// position and the engine's stored sizes. False, with the outputs untouched,
// while the window has no area.
bool engine_cursor_fb(const Engine* engine, double* fb_x, double* fb_y);

// The world point under a framebuffer position, on the plane through the
// press's pick at the eye's distance from it: where a dragged node goes.
void engine_mouse_to_drag_plane(Engine* engine, double mouse_fb_x, double mouse_fb_y,
                                vec3 out_world_pos);

#endif // _ENGINE_H_
