#ifndef _ENGINE_H_
#define _ENGINE_H_

#include <stdint.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "scene.h"
#include "camera.h"
#include "shader.h"
#include "program.h"
#include "import.h"
#include "common.h"
#include "input.h"
#include "async_loader.h"
#include "text.h"
#include "postfx.h"
#include "ltc.h"

typedef enum CameraMode {
    CAMERA_MODE_FREE,  // Free movement mode
    CAMERA_MODE_ORBIT, // Orbit around a point
} CameraMode;

struct Engine;
struct LightClusterContext;
struct Profiler;
struct Ubo;

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
    GLFWwindow* window;
    char* window_title; // Title of the GLFW window

    int win_width;
    int win_height;
    int fb_width;
    int fb_height;
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
                                          // 1 = off, 2 = 2x SSAA. Changing it at runtime rebuilds
                                          // the render targets at the next frame top.
    float render_scale;                   // Render-resolution scale in [0.5, 1]: the scene + the
                                          // pre-TAA post chain rasterize at this fraction of the
                                          // post size. 1 = off. Changing it at runtime rebuilds
                                          // the render targets at the next frame top.
    bool render_suspended;                // A render-target rebuild failed; frames are skipped
                                          // until a size that allocates successfully is set
    // The size the render targets were last built AT, or last attempted at.
    // The frame-top sync compares against these, so it both detects a change
    // and declines to retry a size that already failed.
    int target_render_w, target_render_h;
    int target_post_w, target_post_h;
    // MSAA sample count REQUESTED for the scene framebuffer (1 = off, 4 = 4x).
    // Runtime-changeable via set_engine_msaa_samples.
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
    // buffers and scratch; rebuilt per render_current_scene invocation.
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
    // pass and freed once. Never the list the other passes read.
    DrawList sorted_opaque;
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

    Camera* camera;         // main camera
    CameraMode camera_mode; // Current camera mode

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
                            // render_current_scene (frustum + motion vectors); the scene draws with
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

    bool show_gui;
    bool show_wireframe;
    bool show_xyz;
    bool show_fps;
    bool show_camera_hud; // Live camera pose overlay next to the FPS readout
    bool show_bones;      // X-ray bone visualization
    bool show_lights;     // Light overlay: position cross + cull-radius wireframe
    bool headless;        // Hidden window, no vsync (set before init_engine)
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

    char* screenshot_path; // If set, save final frame here on exit (PPM)
    int screenshot_every;  // Also save numbered frames every N frames (0 = off)
    int exit_after_frames; // Close the loop after N frames (0 = off); CI/headless
    size_t total_frames;   // Monotonic frame counter for the render loop
    void* user_data;       // Opaque context for engine_run's callbacks (GLFW-style)

    // Bone visualization
    ShaderProgram* bone_program;
    GLuint bone_line_vao;
    GLuint bone_line_vbo;

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
    // NULL unless profiler_enabled asked for it before init_engine, and every
    // entry point no-ops on NULL, so an ordinary run issues no query calls and
    // keeps no counts at all.
    struct Profiler* profiler;
    bool profiler_enabled; // true = build the profiler (set before init_engine)

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
// frame's (spec 11.96). Nothing in it may draw -- there is no bound target yet.
//
// Mutating the graph LATER than this -- from `render` -- is recoverable rather
// than fatal: call scene_propagate_transforms again and the new node gets its
// global. It is still the wrong place, because everything between the two reads
// the graph as it stood here, so the shadow pass and the LOD selection will not
// see the change until the next frame.
typedef void (*EngineUpdateFunc)(Engine* engine, float dt);
typedef void (*EnginePreRenderFunc)(Engine* engine, Scene* scene);
typedef void (*EngineRenderFunc)(Engine* engine, Scene* scene);

Engine* create_engine(const char* window_title, int width, int height);
void free_engine(Engine* engine);

int init_engine(Engine* engine);
void set_engine_headless(Engine* engine, bool headless);
// Build the per-pass profiler. Must be called before init_engine, which is
// where the profiler is created; setting it afterwards does nothing.
void set_engine_profiler(Engine* engine, bool enabled);
// Supersampling factor (clamped to [1, 2]). Safe before init_engine (stored)
// or at runtime, where the render targets are rebuilt at the next frame top.
void set_engine_ss_scale(Engine* engine, int ss_scale);
// Render-resolution scale (clamped to [0.5, 1]). Safe before init_engine
// (stored) or at runtime, where the render targets are rebuilt at the next
// frame top. Forced to 1 in headless without headless_jitter, which TAAU
// needs to reconstruct from.
void set_engine_render_scale(Engine* engine, float render_scale);
// Re-centre the world on the camera, snapped to `lattice` (spec 11.62). Applies
// at the next frame top like the render-scale switch above, and only on X and Z.
//
// The one place the snap is spelled, so a caller driving this by hand and the
// automatic threshold cannot disagree about where the origin lands -- they did,
// and the hand-driven copy was the one missing the current origin.
void engine_recentre_on_camera(const Engine* engine, float lattice);
// MSAA sample count for the scene framebuffer (clamped to [1, driver max]).
// 1 disables MSAA. Safe to call before init_engine (stored) or at runtime
// (rebuilds the multisample attachments).
void set_engine_msaa_samples(Engine* engine, int samples);
// Enable/disable temporal anti-aliasing. Call after init_engine (needs postfx).
void set_engine_taa_enabled(Engine* engine, bool enabled);
void set_engine_screenshot_path(Engine* engine, const char* path);
void set_engine_screenshot_every(Engine* engine, int every);
// Exit the main loop after `frames` rendered frames (0 = run until the window
// closes). Fires the same final-frame screenshot path as a normal quit.
void set_engine_exit_after_frames(Engine* engine, int frames);

// GLFW callbacks
void set_engine_error_callback(Engine* engine, GLFWerrorfun error_callback);
void set_engine_mouse_button_callback(Engine* engine, MouseButtonCallback mouse_button_callback);
void set_engine_cursor_position_callback(Engine* engine,
                                         CursorPositionCallback cursor_position_callback);
void set_engine_key_callback(Engine* engine, KeyCallback key_callback);
void set_engine_scroll_callback(Engine* engine, ScrollCallback scroll_callback);
// True when the GUI is capturing the pointer this frame; apps gate 3D input on it.
bool engine_gui_wants_mouse(void);
// The keyboard equivalent. The engine already applies this to the app's key CALLBACK, but an
// app that POLLS key state per frame -- which is what a held movement key has to be -- needs to
// ask the same question itself, or typing in a slider also walks.
bool engine_gui_wants_keyboard(void);

// Camera
void set_engine_camera(Engine* engine, Camera* camera);
void set_engine_camera_mode(Engine* engine, CameraMode mode);
void update_engine_camera_lookat(Engine* engine);
void update_engine_camera_perspective(Engine* engine);

// Scene
int add_scene_to_engine(Engine* engine, Scene* scene);
void set_active_scene_by_index(Engine* engine, size_t scene_index);
void set_active_scene_by_name(Engine* engine, const char* scene_name);
Scene* get_current_scene(const Engine* engine);

// Shader Programs
int add_shader_program_to_engine(Engine* engine, ShaderProgram* program);
ShaderProgram* get_engine_shader_program_by_name(Engine* engine, const char* program_name);

// The lit-surface variant of `family` carrying exactly `features`, compiled and
// registered on first ask and answered from the program cache afterwards
// (spec 11.93; the family since 11.95).
//
// Here rather than in program.c because it is cache management over
// engine->programs, and program.h cannot see an Engine -- engine.h includes it,
// not the other way round. The naming rule stays in program.c, where the builder
// that has to agree with it lives.
ShaderProgram* engine_pbr_variant(Engine* engine, PbrFamily family, unsigned features);

// GUI
void set_engine_show_gui(Engine* engine, bool show_gui);
void set_engine_show_fps(Engine* engine, bool show_fps);

/*
 * Every uniform object_position.glsl's displacers read, in one call.
 *
 * The chunk lands in FIVE programs -- pbr, pbr_skinned, the depth prepass, the
 * shadow depth pass and the shadow absorb pass -- and all five must displace a
 * vertex to the same place. A shadow that leaves its caster is the mild failure;
 * the prepass's one-sided LEQUAL is the sharp one, where a surface that moved
 * differently in the two passes DISAPPEARS rather than shading wrong.
 *
 * One function rather than a call per displacer at each site, because the
 * failure mode is arithmetic: it is not "an effect is missing" but "two passes
 * disagree", which no per-effect gate can see. Spec 11.62 shipped a wind uniform
 * that reached one program of the five and the full suite was green.
 *
 * `scene` may be NULL -- wind then uploads its own off state.
 */
void engine_upload_displacement_uniforms(const Engine* engine, const Scene* scene,
                                         UniformManager* u);

// Present the frame: resolve the MSAA framebuffer through the post stack
// (bloom + tone map, or a raw copy for non-PBR frame_mode) into the default
// framebuffer, then draw the GUI on top (auto-gated on the panel flags via
// gui_frame_active). Called by engine_run.
void engine_present_frame(Engine* engine, RenderMode frame_mode);

// Select which color attachments the scene pass writes: attachment 0 only,
// or 0 + the view-space normals target (used by SSAO/SSR). Render passes
// that emit no normals (skybox, blend, overlays) switch to 0-only.
void engine_set_scene_draw_buffers(const Engine* engine, bool with_gbuffer);
// Mid-frame resolve of the opaque scene into the mipped refraction source
// (see the definition for the lifecycle); called by render_current_scene
// between the skybox and the late pass when transmissive meshes exist.
bool engine_resolve_opaque_color(Engine* engine);
// Mid-frame resolve of the scene depth (from the multisample framebuffer) into
// a sampleable single-sample depth texture, for soft particles / other passes
// that need scene depth before postfx. Returns the depth texture (0 on failure),
// and re-binds engine->framebuffer so drawing can continue. Lazily sized to the
// render resolution.
GLuint engine_resolve_scene_depth(Engine* engine);

// Weighted-blended OIT accumulate sub-pass bracket: engine_begin_oit_pass binds
// the OIT FBO (accum + revealage, sharing the scene depth), clears it, and sets
// the independent per-target blend; engine_end_oit_pass restores the scene FBO,
// color-only draw buffer, and standard alpha blend. begin returns false if the
// targets couldn't allocate (caller falls back to the classic late pass).
bool engine_begin_oit_pass(Engine* engine);
void engine_end_oit_pass(Engine* engine);

// Moment generation sub-pass bracket (spec 11.17), run before the accumulate
// when moment weighting is on. Same shape as the accumulate bracket beside it:
// begin returns false if the targets couldn't allocate, and the caller falls
// back to the depth-curve weight.
bool engine_begin_moment_pass(Engine* engine);
void engine_end_moment_pass(Engine* engine);

// Render
void set_engine_show_wireframe(Engine* engine, bool show_wireframe);
void set_engine_show_xyz(Engine* engine, bool show_xyz);

// Opaque context for engine_run's callbacks (like glfwSetWindowUserPointer): the
// render callbacks stay untyped (Engine*, Scene*); a caller that needs its own
// state (e.g. the game framework) stashes it here and reads it back in the callback.
// NOTE: run_game reserves this slot for its Game* -- a game app must NOT set it
// (use game_set_user_data / game->user_data for app state instead).
void engine_set_user_data(Engine* engine, void* user_data);
void* engine_get_user_data(const Engine* engine);

// The unified main loop: owns the frame skeleton (dt, FPS, GUI frame, transform
// propagation, shadow pass, G-buffer setup, present, screenshot, swap). The
// three hooks run in the order they are declared -- `update` once per frame
// before anything reads the scene, `pre_render` after the sky and origin shift
// and immediately before the graph is propagated, `render` to draw. Any of them
// may be NULL. The render apps and the game framework's run_game all drive the
// engine through it.
void engine_run(Engine* engine, EngineUpdateFunc update, EnginePreRenderFunc pre_render,
                EngineRenderFunc render);

// Substitute an animation clock for the wall clock. `clock` is borrowed and must
// outlive the loop; engine_run samples it each frame after the update callback
// and before the shadow pass. NULL restores the wall clock.
void engine_set_render_clock(Engine* engine, const EngineFrameClock* clock);

// The render (not display) resolution: the scene target is supersampled by
// ss_scale, scaled by render_scale, and brought back to display size at the
// post chain's end, so this is the size the scene pass and every render-res
// post buffer actually rasterize into. Exported because callers outside
// engine.c kept re-deriving fb_size * ss_scale by hand, and a change to how
// render size is computed would silently miss them.
void engine_render_size(const Engine* engine, int* w, int* h);

// The post (pre-display) resolution: fb size x ss_scale, independent of
// render_scale -- what the chain downstream of the TAA seam runs at.
void engine_post_size(const Engine* engine, int* w, int* h);

// Set the frame's animation clock directly, for callers that do not go through
// engine_run: an app driving render_current_scene from its own loop, or a
// capture that pins the clock. `delta` is how far `time` advanced since the last
// render -- 0 for a frozen or first frame, so the wind's previous position
// equals its current one and motion vectors come out zero.
//
// A foreign loop that never calls this renders at t = 0 forever: wind holds
// still, silently. Call it once per frame BEFORE the shadow pass, or the depth
// and shading passes displace wind from different instants.
void engine_set_render_time(Engine* engine, double time, double delta);

// Drag/pick helpers
void get_mouse_world_position_on_drag_plane(Engine* engine, double mouse_fb_x, double mouse_fb_y,
                                            vec3 out_world_pos);

#endif // _ENGINE_H_
