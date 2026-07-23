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

typedef enum CameraMode {
    CAMERA_MODE_FREE,  // Free movement mode
    CAMERA_MODE_ORBIT, // Orbit around a point
} CameraMode;

struct Engine;
struct LightClusterContext;

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

typedef struct Engine {
    GLFWwindow* window;
    char* window_title; // Title of the GLFW window

    int win_width;
    int win_height;
    int fb_width;
    int fb_height;
    GLint max_texture_image_units;  // GL_MAX_TEXTURE_IMAGE_UNITS (queried at init)
    GLint max_array_texture_layers; // GL_MAX_ARRAY_TEXTURE_LAYERS (mask array budget)
    int ss_scale;                   // Supersampling factor: scene + post render at ss_scale x
                                    // display resolution, box-downsampled at tone map (set
                                    // before init_engine). 1 = off, 2 = 2x SSAA.
    int msaa_samples;               // MSAA sample count for the scene framebuffer (1 = off,
                                    // 4 = 4x). Runtime-changeable via set_engine_msaa_samples.

    GLFWerrorfun error_callback;

    CursorPositionCallback cursor_position_callback;
    MouseButtonCallback mouse_button_callback;
    KeyCallback key_callback;
    ScrollCallback scroll_callback;

    GLuint framebuffer;                // Framebuffer object
    GLuint multisample_texture;        // Multisample HDR color (attachment 0)
    GLuint normal_multisample_texture; // Multisample view-space normal .xyz + SSR reflective marker
                                       // .a (0 model / -1 floor / +alpha A2C) (attachment 1)
    GLuint aux_multisample_texture;    // Multisample aux G-buffer: motion .xy + linear view-Z .z +
                                       // effective roughness .w (attachment 2)
    GLuint albedo_multisample_texture; // Multisample base color for SSGI (attachment 3)
    GLuint
        sss_diffuse_multisample_texture; // Multisample skin diffuse irradiance for SSS (attachment
                                         // 4); 0 off-skin, so it doubles as the SSS mask
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
    // Clustered-forward lighting (spec 9.1). The module owns its own GPU
    // buffers and scratch; rebuilt per render_current_scene invocation.
    struct LightClusterContext* light_cluster;
    bool cluster_debug;          // Tint fragments by cluster light count (heatmap)
    bool scene_color_this_frame; // Resolve ran this frame; transmissive draws may sample
    bool normals_this_frame;     // Attachment 1 written this frame (PBR + consumer active)
    bool aux_this_frame;         // Attachment 2 written this frame (TAA needs motion, or GTAO needs
                                 // linear-Z)
    bool albedo_this_frame;      // Attachment 3 (albedo) written this frame (SSGI active)
    bool sss_this_frame;         // Attachment 4 (skin diffuse) written this frame (SSS active)
    bool oit_this_frame;         // OIT accumulate pass ran this frame (postfx composites oit_fbo)

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

    float specular_aa_strength; // Geometric specular AA (0 disables)
    bool energy_comp_enabled;   // Multi-scatter specular energy compensation;
                                // inert without an IBL environment (needs the BRDF LUT)
    bool refraction_enabled;    // Screen-space refraction for transmissive materials;
                                // off = no resolve, and the shader treats transmission
                                // as 0 (glass renders as a plain lit surface)
    bool clearcoat_enabled;     // KHR_materials_clearcoat second specular lobe; off
                                // skips the lobe (materials with clearcoat 0 are
                                // unaffected either way)
    bool specular_enabled;      // KHR_materials_specular F0 tint + specular weight; off
                                // leaves the base dielectric BRDF unchanged (materials
                                // without the extension are unaffected either way)
    bool sheen_enabled;         // KHR_materials_sheen cloth lobe; off skips the lobe
                                // (materials with sheen color 0 are unaffected either way)
    bool parallax_enabled;      // POM height-march (§4.11); off skips the march
                                // (materials with no height map / scale 0 are unaffected)
    bool sss_enabled;           // Separable screen-space SSS; off skips the diffuse
                                // separation + blur (materials with subsurface 0 are unaffected)
    bool oit_enabled;           // Weighted-blended OIT for ALPHA_BLEND meshes (--oit); off keeps
                                // the unsorted alpha-blend late pass

    mat4 model_matrix;
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

    bool show_gui;
    bool show_wireframe;
    bool show_xyz;
    bool show_fps;
    bool show_camera_hud; // Live camera pose overlay next to the FPS readout
    bool show_bones;      // X-ray bone visualization
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
    // Both are latched by engine_run alone, after the update callback and before
    // the shadow pass. They are deliberately not last_frame_time/delta_time --
    // those are FPS bookkeeping on the wall clock, and an embedder's render
    // clock need not be the wall clock.
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

    // Shadow catcher (ground plane that receives shadows over the skybox)
    ShaderProgram* shadow_catcher_program;
    GLuint catcher_vao;
    GLuint catcher_vbo;

    // HDR post-processing (bloom + tone mapping)
    PostFX* postfx;

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

// Unified main-loop callbacks (engine_run). `update` runs once per frame before
// the render, with the real (unclamped) frame dt; pass NULL for a pure render
// loop. `render` draws the scene -- its output is scene-referred linear HDR
// (bloom/exposure/tone mapping run in the present pass; only the GUI draws after
// tone mapping).
typedef void (*EngineUpdateFunc)(Engine* engine, float dt);
typedef void (*EngineRenderFunc)(Engine* engine, Scene* scene);

Engine* create_engine(const char* window_title, int width, int height);
void free_engine(Engine* engine);

int init_engine(Engine* engine);
void set_engine_headless(Engine* engine, bool headless);
// Supersampling factor (clamped to >= 1). Call before init_engine.
void set_engine_ss_scale(Engine* engine, int ss_scale);
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

// GUI
void set_engine_show_gui(Engine* engine, bool show_gui);
void set_engine_show_fps(Engine* engine, bool show_fps);

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

// The unified main loop: owns the frame skeleton (dt, FPS, GUI frame, shadow
// pass, G-buffer setup, present, screenshot, swap). `update` runs once per frame
// before rendering (NULL for pure render loops); `render` draws the scene. The
// render apps and the game framework's run_game all drive the engine through it.
void engine_run(Engine* engine, EngineUpdateFunc update, EngineRenderFunc render);

// Substitute an animation clock for the wall clock. `clock` is borrowed and must
// outlive the loop; engine_run samples it each frame after the update callback
// and before the shadow pass. NULL restores the wall clock.
void engine_set_render_clock(Engine* engine, const EngineFrameClock* clock);

// The render (not display) resolution: the scene target is supersampled by
// ss_scale and box-downsampled at tone map, so this is the size the scene pass
// and every full-res post buffer actually rasterize into. Exported because
// callers outside engine.c kept re-deriving fb_size * ss_scale by hand, and a
// change to how render size is computed would silently miss them.
void engine_render_size(const Engine* engine, int* w, int* h);

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
