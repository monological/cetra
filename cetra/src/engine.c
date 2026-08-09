#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>
#include <cglm/ray.h>
#include <cglm/box.h>

#include "scene.h"
#include "camera.h"
#include "shader.h"
#include "program.h"
#include "util.h"
#include "ext/cwalk.h" // cwk_path_set_style: pin UNIX separators (see init_engine)
#include "engine.h"
#include "light_cluster.h"
#include "transform.h"
#include "intersect.h"
#include "shadow.h"
#include "sky.h"
#include "gi_volume.h"
#include "mask_array.h"
#include "texture.h"
#include "import.h" // resolve_height_maps (POM height convention)
#include "render.h"
#include "springbone.h"

#include "ext/log.h"

// Dear ImGui via cimgui. CIMGUI_DEFINE_ENUMS_AND_STRUCTS gives the full
// struct/enum definitions to C; CIMGUI_USE_GLFW/OPENGL3 (compile defs from
// CMake) expose the GLFW + OpenGL3 backend bindings in cimgui_impl.h.
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#include "cimgui_impl.h"

/*
 * Private functions
 */
static int _create_default_shaders_for_engine(Engine* engine);
static int _setup_engine_glfw(Engine* engine);
static int _setup_engine_msaa(Engine* engine);
static int _setup_engine_gui(Engine* engine);
static void _engine_cursor_position_callback(GLFWwindow* window, double xpos, double ypos);
static void _engine_mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
static void _engine_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
static void _engine_scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
static void _engine_framebuffer_size_callback(GLFWwindow* window, int fb_width, int fb_height);
static SceneNode* _perform_engine_ray_picking(Engine* engine, double mouse_fb_x, double mouse_fb_y);
static void _destroy_msaa_attachments(Engine* engine);

// One scene-MRT color attachment. The engine drives several draw buffers into a
// single multisample framebuffer; describing each one's format + clear + write
// gate in a single list keeps create / destroy / clear iterating off one source
// of truth, so a new attachment is one row rather than an edit scattered across
// half a dozen sites (each a place to forget one).
//
// Attachment 0 is HDR scene color: always written, and cleared with the scene
// background by the shared glClear (which also clears depth/stencil), so its
// write gate is NULL and the 1..N clear loop skips it.
//
// Each row names its color attachment explicitly rather than deriving it from
// the row index: fragment-output locations 5 and 6 belong to the OIT
// accumulate targets, which live on the OIT FBO -- so the ambient-specular row
// binds attachment 7 and the scene FBO simply has no attachments 5/6.
typedef struct GBufferAttachment {
    GLuint* tex;            // storage on the Engine
    bool* this_frame;       // per-frame write gate (NULL = always written)
    GLenum internal_format; // multisample texture format
    GLenum attachment;      // GL_COLOR_ATTACHMENTn == fragment output location n
    GLfloat clear[4];       // per-frame clear color
} GBufferAttachment;

#define GBUFFER_ATTACHMENT_COUNT 6
// Draw-buffer slots the scene MRT spans: highest attachment in the table
// (7) + 1. Sizes the draw-buffer list and the GL_MAX_DRAW_BUFFERS check, so
// a table row past slot 7 fails the startup check instead of silently
// overrunning the list.
#define GBUFFER_DRAW_BUFFER_SLOTS 8

// Describe this engine's scene-MRT attachments. Pure (no GL): the
// tex/this_frame pointers alias Engine fields so create/destroy/clear share one
// description of what each attachment is. The aux buffer is full float
// (RGBA32F): fp16 quantizes linear Z into scene-scale steps on large scenes and
// GTAO reads the staircase as banded occlusion. Ambient specular is R11G11B10F:
// positive HDR only (WS_SCENE_MAX sits under the format's ~65024 ceiling), no
// alpha needed, half the bandwidth of RGBA16F.
static void _gbuffer_attachments(Engine* engine, GBufferAttachment out[GBUFFER_ATTACHMENT_COUNT]) {
    out[0] = (GBufferAttachment){
        &engine->multisample_texture, NULL, GL_RGBA16F, GL_COLOR_ATTACHMENT0,
        {0.1f, 0.1f, 0.1f, 1.0f}};
    out[1] = (GBufferAttachment){&engine->normal_multisample_texture,
                                 &engine->normals_this_frame,
                                 GL_RGBA16F,
                                 GL_COLOR_ATTACHMENT1,
                                 {0.0f, 0.0f, 0.0f, 0.0f}};
    out[2] = (GBufferAttachment){&engine->aux_multisample_texture,
                                 &engine->aux_this_frame,
                                 GL_RGBA32F,
                                 GL_COLOR_ATTACHMENT2,
                                 {0.0f, 0.0f, 0.0f, 0.0f}};
    out[3] = (GBufferAttachment){&engine->albedo_multisample_texture,
                                 &engine->albedo_this_frame,
                                 GL_RGBA8,
                                 GL_COLOR_ATTACHMENT3,
                                 {0.0f, 0.0f, 0.0f, 0.0f}};
    out[4] = (GBufferAttachment){&engine->sss_diffuse_multisample_texture,
                                 &engine->sss_this_frame,
                                 GL_RGBA16F,
                                 GL_COLOR_ATTACHMENT4,
                                 {0.0f, 0.0f, 0.0f, 0.0f}};
    out[5] = (GBufferAttachment){&engine->spec_multisample_texture,
                                 &engine->spec_this_frame,
                                 GL_R11F_G11F_B10F,
                                 GL_COLOR_ATTACHMENT7,
                                 {0.0f, 0.0f, 0.0f, 0.0f}};
}

/*
 * Engine
 */
Engine* create_engine(const char* window_title, int width, int height) {
    // Zeroed, not malloc'd: every field below is still set explicitly, but a
    // struct this wide cannot rely on each new field being remembered here --
    // and a field only ever written conditionally, like render_suspended, has
    // no other initializer at all.
    Engine* engine = calloc(1, sizeof(Engine));
    if (!engine) {
        log_error("Failed to allocate memory for engine");
        return NULL;
    }

    engine->window = NULL;
    // Before anything can read it: apps set exposure fields between
    // create_engine and init_engine, so the defaults have to be in place here
    // rather than alongside the GL resources.
    exposure_init(&engine->exposure);

    if (window_title != NULL) {
        engine->window_title = safe_strdup(window_title);
    } else {
        engine->window_title = NULL;
    }

    if (window_title && !engine->window_title) {
        log_error("Failed to allocate memory for window title");
        free(engine);
        return NULL;
    }

    engine->win_width = width;
    engine->win_height = height;
    engine->fb_width = 0;
    engine->fb_height = 0;
    engine->ss_scale = 1;     // Supersampling off by default (4x fragment cost);
                              // opt in with --ssaa 2 for beauty shots
    engine->render_scale = 1.0f; // Full render resolution by default; opt in
                                 // with --render-scale for the TAAU upscale
    engine->msaa_samples = 4; // 4x MSAA by default (runtime-toggleable)

    engine->error_callback = NULL;
    engine->mouse_button_callback = NULL;
    engine->cursor_position_callback = NULL;
    engine->key_callback = NULL;
    engine->scroll_callback = NULL;

    engine->framebuffer = 0;
    GBufferAttachment gb_init[GBUFFER_ATTACHMENT_COUNT];
    _gbuffer_attachments(engine, gb_init);
    for (int i = 0; i < GBUFFER_ATTACHMENT_COUNT; i++)
        *gb_init[i].tex = 0;
    engine->depth_renderbuffer = 0;
    engine->opaque_color_fbo = 0;
    engine->opaque_color_texture = 0;
    engine->opaque_color_w = 0;
    engine->opaque_color_h = 0;
    engine->scene_depth_fbo = 0;
    engine->scene_depth_texture = 0;
    engine->scene_depth_w = 0;
    engine->scene_depth_h = 0;
    engine->oit_fbo = 0;
    engine->oit_accum_multisample_texture = 0;
    engine->oit_revealage_multisample_texture = 0;
    engine->oit_w = 0;
    engine->oit_h = 0;
    engine->moment_fbo = 0;
    engine->moment_multisample_texture = 0;
    engine->moment_b0_multisample_texture = 0;
    engine->moment_atlas_fbo = 0;
    engine->moment_atlas_texture = 0;
    engine->moment_w = 0;
    engine->moment_h = 0;
    engine->light_cluster = NULL;
    engine->cluster_debug = false;
    engine->scene_color_this_frame = false;
    engine->normals_this_frame = false;
    engine->aux_this_frame = false;
    engine->albedo_this_frame = false;
    engine->sss_this_frame = false;
    engine->spec_this_frame = false;
    engine->oit_this_frame = false;
    engine->moments_this_frame = false;

    engine->camera = NULL;
    engine->camera_mode = CAMERA_MODE_ORBIT;

    engine->scenes = NULL;
    engine->scene_count = 0;
    engine->current_scene_index = 0;

    engine->programs = NULL;
    engine->program_count = 0;
    engine->program_map = NULL;

    engine->current_render_mode = RENDER_MODE_PBR;
    engine->specular_aa_strength = 1.0f;
    engine->energy_comp_enabled = true; // Correctness fix; ships on
    engine->refraction_enabled = true;  // Transmissive materials refract by default
    engine->clearcoat_enabled = true;   // Clearcoat lobe on; inert unless a material carries it
    engine->specular_enabled = true;    // KHR specular on; inert unless a material carries it
    engine->sheen_enabled = true;       // KHR sheen on; inert unless a material carries it
    engine->parallax_enabled = true;    // POM on; inert unless a material carries height + scale
    engine->sss_enabled = true;         // SSS on; inert unless a material carries subsurface > 0
    engine->skin_preint_enabled = true; // Pre-integrated skin on; inert unless a material carries
                                        // curvature_scale > 0
    // Order-independent transparency on, and weighted by measured absorbance
    // moments rather than the depth curve. --no-oit is the unsorted late pass.
    //
    // Costs nothing on a scene with no alpha-blend mesh: the whole path is gated
    // on oit_mesh_count, so it allocates nothing and runs no extra pass there.
    engine->oit_enabled = true;
    engine->oit_moments_enabled = true;

    glm_mat4_identity(engine->model_matrix);
    glm_mat4_identity(engine->view_matrix);
    glm_mat4_identity(engine->projection_matrix);
    glm_mat4_identity(engine->view_proj);
    glm_mat4_identity(engine->draw_projection);
    glm_mat4_identity(engine->prev_view_proj);

    engine->show_gui = false;
    engine->show_wireframe = false;
    engine->show_xyz = false;
    engine->show_fps = false;
    engine->show_camera_hud = false;
    engine->show_bones = false;
    engine->show_lights = false;
    engine->headless = false;
    engine->headless_jitter = false;
    engine->gui_frame_active = false;
    engine->screenshot_path = NULL;
    engine->screenshot_every = 0;
    engine->exit_after_frames = 0;
    engine->total_frames = 0;
    engine->user_data = NULL;

    engine->ltc = NULL;
    engine->brdf_lut = 0;
    engine->bone_program = NULL;
    engine->bone_line_vao = 0;
    engine->bone_line_vbo = 0;

    engine->shadow_catcher_program = NULL;
    engine->catcher_vao = 0;
    engine->catcher_vbo = 0;

    engine->postfx = NULL;

    init_input_state(&engine->input);

    // FPS tracking initialization
    engine->last_frame_time = 0.0;
    engine->delta_time = 0.0;
    // Load-time work (probe bakes) renders before the loop starts, at t = 0.
    engine->render_time = 0.0;
    engine->render_delta = 0.0;
    engine->render_clock = NULL;
    engine->fps = 0.0f;
    engine->fps_update_timer = 0.0f;
    engine->frame_count = 0;

    engine->async_loader = NULL;

    return engine;
}

void free_engine(Engine* engine) {
    if (!engine)
        return;

    // Free text renderer
    if (engine->text_renderer) {
        free_text_renderer(engine->text_renderer);
        engine->text_renderer = NULL;
    }

    // Free async loader before scenes (may have pending work)
    if (engine->async_loader) {
        free_async_loader(engine->async_loader);
        engine->async_loader = NULL;
    }

    if (engine->scenes) {
        for (size_t i = 0; i < engine->scene_count; ++i) {
            if (engine->scenes[i]) {
                free_scene(engine->scenes[i]);
            }
        }
        free(engine->scenes);
    }

    if (engine->programs) {
        for (size_t i = 0; i < engine->program_count; ++i) {
            if (engine->programs[i]) {
                free_program(engine->programs[i]);
            }
        }
        free(engine->programs);
    }

    if (engine->camera) {
        free_camera(engine->camera);
    }

    if (engine->screenshot_path) {
        free(engine->screenshot_path);
    }

    // GL objects must be released while the context still exists,
    // i.e. before the window is destroyed
    if (engine->postfx) {
        free_postfx(engine->postfx);
    }

    free_light_cluster_context(engine->light_cluster);
    free_ubo(engine->view_ubo);

    glDeleteFramebuffers(1, &engine->framebuffer);
    _destroy_msaa_attachments(engine); // color attachments + depth renderbuffer
    glDeleteFramebuffers(1, &engine->opaque_color_fbo);
    glDeleteTextures(1, &engine->opaque_color_texture);
    glDeleteFramebuffers(1, &engine->scene_depth_fbo);
    glDeleteTextures(1, &engine->scene_depth_texture);

    if (engine->catcher_vao)
        glDeleteVertexArrays(1, &engine->catcher_vao);
    if (engine->catcher_vbo)
        glDeleteBuffers(1, &engine->catcher_vbo);

    free_ltc_tables(engine->ltc);
    engine->ltc = NULL;

    gl_delete_texture(&engine->brdf_lut);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    igDestroyContext(NULL);

    // Destroy GLFW window
    if (engine->window) {
        glfwDestroyWindow(engine->window);
    }

    // Terminate GLFW
    glfwTerminate();

    free(engine);
}

/*
 * Setup GLFW
 */
static int _setup_engine_glfw(Engine* engine) {
    if (!engine) {
        return -1;
    }

    if (engine->error_callback) {
        glfwSetErrorCallback(engine->error_callback);
    }

    if (!glfwInit()) {
        log_error("Failed to initialize GLFW");
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES, 4); // Enable 4x MSAA
    glfwWindowHint(GLFW_DEPTH_BITS, 32);

    if (engine->headless) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    engine->window =
        glfwCreateWindow(engine->win_width, engine->win_height, engine->window_title, NULL, NULL);
    if (engine->window == NULL) {
        log_error("Failed to create GLFW window");
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(engine->window);

    // V-Sync on for normal runs, off for headless so frames run at full speed
    glfwSwapInterval(engine->headless ? 0 : 1);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (glewInit() != GLEW_OK) {
        log_error("Failed to initialize GLEW");
        glfwTerminate();
        return -1;
    }

    log_info("OpenGL %s | GLSL %s", (const char*)glGetString(GL_VERSION),
             (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));
    log_info("Renderer: %s | %s", (const char*)glGetString(GL_RENDERER),
             (const char*)glGetString(GL_VENDOR));

    engine->max_texture_image_units = get_gl_max_texture_image_units();
    engine->max_array_texture_layers = get_gl_max_array_texture_layers();
    log_info("GL sampler budget: %d fragment texture image units, %d array layers",
             engine->max_texture_image_units, engine->max_array_texture_layers);
    // The renderer binds material + engine samplers up to IBL_SKYBOX_TEXTURE_UNIT
    // (see the static-assert chain in render.c); a GPU under that is out of spec.
    if (engine->max_texture_image_units <= IBL_SKYBOX_TEXTURE_UNIT)
        log_error("GL reports only %d fragment texture units; the renderer needs %d",
                  engine->max_texture_image_units, IBL_SKYBOX_TEXTURE_UNIT + 1);

    // Both sizes from the window we actually got, not the one we asked for --
    // a window manager may grant something else, and these two writers (here
    // and the framebuffer-size callback) are the only ones.
    glfwGetWindowSize(engine->window, &(engine->win_width), &(engine->win_height));
    glfwGetFramebufferSize(engine->window, &(engine->fb_width), &(engine->fb_height));
    glViewport(0, 0, engine->fb_width, engine->fb_height);

    return 0;
}

/*
 * MSAA anti-aliasing
 *
 */
// The scene renders into a target supersampled by ss_scale and scaled by
// render_scale; the post chain brings it back to display size. This render
// (not display) resolution is shared by the MSAA allocation and the
// scene-pass viewport. The render_scale rounding lives in postfx_scaled_dim
// so this target and the postfx resolve targets cannot disagree.
void engine_render_size(const Engine* engine, int* w, int* h) {
    engine_post_size(engine, w, h);
    *w = postfx_scaled_dim(*w, engine->render_scale);
    *h = postfx_scaled_dim(*h, engine->render_scale);
}

void engine_post_size(const Engine* engine, int* w, int* h) {
    *w = engine->fb_width * engine->ss_scale;
    *h = engine->fb_height * engine->ss_scale;
}

// Clamp a requested MSAA sample count to [1, driver max]. Needs a live GL context.
static int _clamp_msaa_samples(int samples) {
    if (samples < 1)
        samples = 1;
    GLint max_color_samples = 0;
    GLint max_samples = 0;
    glGetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &max_color_samples);
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    if (max_samples > 0 && max_samples < max_color_samples)
        max_color_samples = max_samples;
    if (max_color_samples > 0 && samples > max_color_samples)
        samples = max_color_samples;
    return samples;
}

// Delete the multisample color/normal attachments and depth renderbuffer.
// The framebuffer object itself is left intact for reuse.
static void _destroy_msaa_attachments(Engine* engine) {
    GBufferAttachment gb[GBUFFER_ATTACHMENT_COUNT];
    _gbuffer_attachments(engine, gb);
    for (int i = 0; i < GBUFFER_ATTACHMENT_COUNT; i++) {
        gl_delete_texture(gb[i].tex);
    }
    glDeleteRenderbuffers(1, &engine->depth_renderbuffer);
    engine->depth_renderbuffer = 0;
    // The OIT and moment FBOs share depth_renderbuffer; tear them down too so
    // they are rebuilt against the fresh depth on the next OIT frame (0 handles
    // when never created).
    gl_delete_fbo(&engine->oit_fbo);
    gl_delete_texture(&engine->oit_accum_multisample_texture);
    gl_delete_texture(&engine->oit_revealage_multisample_texture);
    engine->oit_w = 0;
    engine->oit_h = 0;
    gl_delete_fbo(&engine->moment_fbo);
    gl_delete_texture(&engine->moment_multisample_texture);
    gl_delete_texture(&engine->moment_b0_multisample_texture);
    gl_delete_fbo(&engine->moment_atlas_fbo);
    gl_delete_texture(&engine->moment_atlas_texture);
    engine->moment_w = 0;
    engine->moment_h = 0;
}

// Create one multisample color attachment (texture + framebuffer binding) on
// the currently-bound framebuffer. Each G-buffer target is written only when a
// consumer is active (see engine_set_scene_draw_buffers); the texture is always
// allocated so the FBO layout is stable across frames.
static void _add_msaa_color_attachment(GLuint* out_tex, GLenum internal_format, GLenum attachment,
                                       int rw, int rh, int samples) {
    glGenTextures(1, out_tex);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, *out_tex);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, internal_format, rw, rh, GL_TRUE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D_MULTISAMPLE, *out_tex, 0);
}

// (Re)create the multisample attachments on engine->framebuffer at the given
// sample count and render size. Attachment 0 is float (RGBA16F) so the scene
// accumulates in linear HDR; the G-buffer targets follow.
static int _create_msaa_attachments(Engine* engine, int rw, int rh, int samples) {
    glBindFramebuffer(GL_FRAMEBUFFER, engine->framebuffer);

    // GL 4.1 guarantees >= 8 draw buffers, but query once so a hypothetical
    // thin driver fails loudly here instead of silently dropping the last
    // attachment.
    GBufferAttachment gb[GBUFFER_ATTACHMENT_COUNT];
    _gbuffer_attachments(engine, gb);
    GLint max_draw_buffers = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &max_draw_buffers);
    if (max_draw_buffers < GBUFFER_DRAW_BUFFER_SLOTS)
        log_error("GL_MAX_DRAW_BUFFERS is %d (< %d): the last scene-MRT attachment will not bind",
                  max_draw_buffers, GBUFFER_DRAW_BUFFER_SLOTS);

    // Draw-buffer layout: 0 HDR color, 1 view normals (SSAO/SSR), 2 aux (TAA
    // motion + GTAO linear-Z), 3 albedo (SSGI), 4 skin diffuse (SSS), 7
    // ambient specular (split spec-occ). Every target is allocated each
    // (re)build for a stable FBO layout, but written only when its consumer is
    // active (see engine_set_scene_draw_buffers).
    for (int i = 0; i < GBUFFER_ATTACHMENT_COUNT; i++) {
        _add_msaa_color_attachment(gb[i].tex, gb[i].internal_format, gb[i].attachment, rw,
                                   rh, samples);
    }

    glGenRenderbuffers(1, &engine->depth_renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, engine->depth_renderbuffer);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, rw, rh);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                              engine->depth_renderbuffer);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Error: MSAA Framebuffer is not complete!");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return -1;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0); // Bind back to the default framebuffer
    return 0;
}

static int _setup_engine_msaa(Engine* engine) {
    if (!engine || !engine->window)
        return -1;

    int rw, rh;
    engine_render_size(engine, &rw, &rh);
    engine->msaa_samples = _clamp_msaa_samples(engine->msaa_samples);

    glGenFramebuffers(1, &engine->framebuffer);
    if (_create_msaa_attachments(engine, rw, rh, engine->msaa_samples) != 0) {
        _destroy_msaa_attachments(engine);
        gl_delete_fbo(&engine->framebuffer);
        return -1;
    }
    return 0;
}

// Rebuild the multisample scene attachments at the current render size and
// sample count, on the same FBO name. The one place that pairing lives, so a
// caller cannot destroy without recreating, or ignore the result -- an
// incomplete scene FBO is invisible until something draws into it.
static bool _engine_rebuild_msaa_target(Engine* engine) {
    int rw, rh;
    engine_render_size(engine, &rw, &rh);
    _destroy_msaa_attachments(engine);
    if (_create_msaa_attachments(engine, rw, rh, engine->msaa_samples) != 0) {
        log_error("Scene target rebuild failed at %dx%d (%dx MSAA)", rw, rh, engine->msaa_samples);
        return false;
    }
    return true;
}

// Bring the scene target and the whole post chain to the current
// fb_width/fb_height/ss_scale/render_scale. The MSAA scene target goes first
// and its failure is checked before postfx is touched: every G-buffer resolve
// is a multisample blit, which needs identical rects, so a half-applied
// resize is worse than none. Suspends rendering rather than limping on if
// either half fails -- a chain whose hdr target is 0 would blit into the
// default framebuffer.
static void _engine_rebuild_render_targets(Engine* engine) {
    int rw, rh;
    engine_render_size(engine, &rw, &rh);
    // Record what this attempt is FOR before making it. The sync compares
    // against these, so a failure latches on its own size and is retried only
    // when a genuinely different one is asked for. Deriving the same answer
    // from whatever the failure happened to leave behind does not work: the
    // two failure paths leave opposite states -- the MSAA one leaves postfx at
    // the old size (retry forever), the postfx one leaves it at the new size
    // (never retry).
    engine->target_render_w = rw;
    engine->target_render_h = rh;
    engine_post_size(engine, &engine->target_post_w, &engine->target_post_h);

    if (!_engine_rebuild_msaa_target(engine)) {
        engine->render_suspended = true;
        return;
    }
    if (!postfx_resize(engine->postfx, engine->fb_width, engine->fb_height, engine->ss_scale,
                       engine->render_scale)) {
        log_error("Post chain rebuild failed at %dx%d; rendering suspended", rw, rh);
        engine->render_suspended = true;
        return;
    }
    engine->render_suspended = false;
    // The lazily-sized engine targets (opaque color, scene depth, OIT) each
    // compare their stored size and rebuild themselves on next use.
    log_info("Render targets rebuilt: %dx%d render, scale %.2f", rw, rh, engine->render_scale);
}

// False when the frame cannot be drawn: a zero-area (minimized) window, or a
// rebuild that failed and left the chain without usable targets.
static bool _engine_frame_renderable(const Engine* engine) {
    return !engine->render_suspended && engine->fb_width > 0 && engine->fb_height > 0;
}

// Close out a loop iteration: advance the frame clock and honour the engine's
// own frame limit. Runs on undrawn frames too, so a headless run still exits
// at --frames N if the window is zero-area or a rebuild failed -- otherwise
// the very condition worth reporting would hang the run reporting it.
static void _engine_advance_frame(Engine* engine) {
    engine->total_frames++;
    if (engine->exit_after_frames > 0 &&
        engine->total_frames >= (size_t)engine->exit_after_frames) {
        glfwSetWindowShouldClose(engine->window, GLFW_TRUE);
    }
}

// Frame-top self-heal: if the sizes the engine now derives disagree with what
// the post chain was built at, rebuild. Keyed on the built sizes rather than
// on an event flag so that every trigger -- the GUI slider, a window resize,
// the diagnostic schedule, a future dynamic-resolution controller -- works
// without its own call site, and so a size field changing behind our back
// heals instead of desyncing. The idiom is _ensure_opaque_color_target's and
// ensure_march_targets'.
static void _engine_sync_render_targets(Engine* engine) {
    if (!engine->postfx)
        return;
    int rw, rh, pw, ph;
    engine_render_size(engine, &rw, &rh);
    engine_post_size(engine, &pw, &ph);
    // A minimized window reports 0x0, where every downstream size formula
    // degenerates (NaN aspect, +inf cluster params, incomplete framebuffers).
    // Hold the existing targets untouched; _engine_frame_renderable is what
    // skips the frame, so no flag has to be un-set when the window returns.
    if (rw < 1 || rh < 1 || pw < 1 || ph < 1)
        return;
    // Already built (or already attempted) at this size: nothing to do. Also
    // the retry policy -- a failed attempt is not repeated until something
    // actually asks for a different size.
    if (rw == engine->target_render_w && rh == engine->target_render_h &&
        pw == engine->target_post_w && ph == engine->target_post_h)
        return;
    _engine_rebuild_render_targets(engine);
}

// Change the MSAA sample count. Before init_engine this just stores the request;
// at runtime it rebuilds the multisample attachments in place (the single-sample
// post-process resolve targets are unaffected by the sample count).
void set_engine_msaa_samples(Engine* engine, int samples) {
    if (!engine)
        return;
    if (samples < 1)
        samples = 1;

    if (!engine->framebuffer) {
        engine->msaa_samples = samples; // pre-init: _setup_engine_msaa clamps
        return;
    }

    samples = _clamp_msaa_samples(samples);
    if (samples == engine->msaa_samples)
        return;
    engine->msaa_samples = samples;

    // Same latch as a resolution change: a scene target that failed to rebuild
    // is not something to keep drawing into.
    if (!_engine_rebuild_msaa_target(engine))
        engine->render_suspended = true;
}

/*
 * GUI
 *
 */

// Dark panel with an indigo (#5f5fff) accent: the accent drives every
// interactive element (title bar, buttons, collapsing headers, checkmarks,
// sliders) while the surface stays a near-black blue-tinted charcoal.
static void _apply_engine_gui_style(void) {
    ImGuiStyle* style = igGetStyle();
    igStyleColorsDark(style);

    style->WindowRounding = 6.0f;
    style->ChildRounding = 4.0f;
    style->FrameRounding = 4.0f;
    style->PopupRounding = 4.0f;
    style->GrabRounding = 4.0f;
    style->ScrollbarRounding = 4.0f;
    style->WindowBorderSize = 1.0f;
    style->FrameBorderSize = 1.0f;
    style->WindowPadding = (ImVec2){14.0f, 14.0f};
    style->FramePadding = (ImVec2){9.0f, 6.0f};
    style->ItemSpacing = (ImVec2){9.0f, 10.0f};
    style->ItemInnerSpacing = (ImVec2){8.0f, 6.0f};

    const ImVec4 accent = {0.373f, 0.373f, 1.000f, 1.00f}; // #5f5fff
    const ImVec4 accent_hi = {0.500f, 0.500f, 1.000f, 1.00f};

    ImVec4* c = style->Colors;
    c[ImGuiCol_Text] = (ImVec4){0.90f, 0.90f, 0.94f, 1.00f};
    c[ImGuiCol_TextDisabled] = (ImVec4){0.50f, 0.50f, 0.55f, 1.00f};
    c[ImGuiCol_WindowBg] = (ImVec4){0.055f, 0.055f, 0.075f, 0.97f};
    c[ImGuiCol_ChildBg] = (ImVec4){0.00f, 0.00f, 0.00f, 0.00f};
    c[ImGuiCol_PopupBg] = (ImVec4){0.070f, 0.070f, 0.095f, 0.98f};
    c[ImGuiCol_Border] = (ImVec4){0.267f, 0.267f, 0.267f, 0.50f}; // #444, inactive

    c[ImGuiCol_FrameBg] = (ImVec4){0.130f, 0.130f, 0.170f, 1.00f};
    c[ImGuiCol_FrameBgHovered] = (ImVec4){0.200f, 0.200f, 0.300f, 1.00f};
    c[ImGuiCol_FrameBgActive] = (ImVec4){0.260f, 0.260f, 0.420f, 1.00f};

    c[ImGuiCol_TitleBg] = (ImVec4){0.080f, 0.080f, 0.110f, 1.00f};
    c[ImGuiCol_TitleBgActive] = (ImVec4){0.190f, 0.190f, 0.400f, 1.00f};
    c[ImGuiCol_TitleBgCollapsed] = (ImVec4){0.080f, 0.080f, 0.110f, 0.75f};

    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hi;

    c[ImGuiCol_Button] = (ImVec4){0.373f, 0.373f, 1.000f, 0.55f};
    c[ImGuiCol_ButtonHovered] = (ImVec4){0.373f, 0.373f, 1.000f, 0.90f};
    c[ImGuiCol_ButtonActive] = accent_hi;

    c[ImGuiCol_Header] = (ImVec4){0.373f, 0.373f, 1.000f, 0.45f};
    c[ImGuiCol_HeaderHovered] = (ImVec4){0.373f, 0.373f, 1.000f, 0.70f};
    c[ImGuiCol_HeaderActive] = (ImVec4){0.373f, 0.373f, 1.000f, 0.90f};

    c[ImGuiCol_Separator] = (ImVec4){0.267f, 0.267f, 0.267f, 0.60f};
    c[ImGuiCol_SeparatorHovered] = (ImVec4){0.373f, 0.373f, 1.000f, 0.78f};
    c[ImGuiCol_SeparatorActive] = accent_hi;

    c[ImGuiCol_ResizeGrip] = (ImVec4){0.373f, 0.373f, 1.000f, 0.25f};
    c[ImGuiCol_ResizeGripHovered] = (ImVec4){0.373f, 0.373f, 1.000f, 0.55f};
    c[ImGuiCol_ResizeGripActive] = (ImVec4){0.373f, 0.373f, 1.000f, 0.90f};

    c[ImGuiCol_TextSelectedBg] = (ImVec4){0.373f, 0.373f, 1.000f, 0.40f};
}

static int _setup_engine_gui(Engine* engine) {
    if (!engine || !engine->window)
        return -1;

    // Dear ImGui, sharing the same GLFW window + GL context. install_callbacks
    // = false: the engine owns the GLFW callbacks and forwards events to the
    // ImGui backend itself (see the input callbacks). "#version 150" is the
    // core-profile GLSL the OpenGL3 backend needs on macOS.
    igCreateContext(NULL);
    ImGui_ImplGlfw_InitForOpenGL(engine->window, false);
    ImGui_ImplOpenGL3_Init("#version 150");
    _apply_engine_gui_style();

    // save engine to window so we can use it in callbacks
    glfwSetWindowUserPointer(engine->window, engine);

    glfwSetMouseButtonCallback(engine->window, _engine_mouse_button_callback);
    glfwSetCursorPosCallback(engine->window, _engine_cursor_position_callback);
    glfwSetFramebufferSizeCallback(engine->window, _engine_framebuffer_size_callback);
    glfwSetKeyCallback(engine->window, _engine_key_callback);
    // Text input has no engine-side logic, so hand it straight to the backend.
    glfwSetCharCallback(engine->window, ImGui_ImplGlfw_CharCallback);
    glfwSetScrollCallback(engine->window, _engine_scroll_callback);

    return 0;
}

/*
 * Initialize the Engine
 *
 */
int init_engine(Engine* engine) {
    printf("┏┓┏┓┏┳┓┳┓┏┓\n");
    printf("┃ ┣  ┃ ┣┫┣┫\n");
    printf("┗┛┗┛ ┻ ┛┗┛┗\n");

    printf("\nInitializing Cetra Graphics Engine...\n");

    // cwalk picks its separator style from a compile-time global that defaults
    // to backslashes on Windows. The engine keeps paths forward-slashed
    // everywhere (Windows file APIs accept them), so pin UNIX style before any
    // path is normalized -- otherwise cwk_path_normalize would emit backslashes
    // that the engine's '/'-scanning path code silently fails to match. No-op on
    // macOS/Linux, which already default to UNIX.
    cwk_path_set_style(CWK_STYLE_UNIX);

    if (_setup_engine_glfw(engine) != 0) {
        log_error("Failed to initialize engine GLFW");
        return -1;
    }
    if (_setup_engine_msaa(engine) != 0) {
        log_error("Failed to initialize engine MSAA");
        return -1;
    }

    // Clustered-forward lighting (spec 9.1): owns its own UBOs + scratch
    engine->light_cluster = create_light_cluster_context();
    engine->view_ubo = create_ubo(UBO_VIEW_BLOCK_SIZE, UBO_BINDING_VIEW);
    if (!engine->light_cluster) {
        log_error("Failed to create light cluster context");
        return -1;
    }
    if (_setup_engine_gui(engine) != 0) {
        log_error("Failed to initialize engine GUI");
        return -1;
    }

    if (_create_default_shaders_for_engine(engine) != 0) {
        log_error("Failed to create default shaders for engine");
        return -1;
    }

    engine->async_loader = create_async_loader();
    if (!engine->async_loader) {
        log_error("Failed to create async loader");
        return -1;
    }

    // Initialize text renderer
    engine->text_renderer = create_text_renderer();
    if (engine->text_renderer) {
        init_text_renderer(engine->text_renderer, engine->win_width, engine->win_height);
        // Create and cache text shader program
        ShaderProgram* text_prog = create_text_program();
        if (text_prog) {
            add_shader_program_to_engine(engine, text_prog);
            engine->text_renderer->text_program = text_prog;
        }
    }

    // LTC area-light tables (spec 9.2): static fitted data, uploaded once
    engine->ltc = create_ltc_tables();
    if (!engine->ltc)
        return -1;

    engine->brdf_lut = ibl_bake_brdf_lut(engine);
    if (!engine->brdf_lut)
        return -1;

    // Initialize bone visualization
    glGenVertexArrays(1, &engine->bone_line_vao);
    glGenBuffers(1, &engine->bone_line_vbo);
    glBindVertexArray(engine->bone_line_vao);
    glBindBuffer(GL_ARRAY_BUFFER, engine->bone_line_vbo);
    // Position attribute (location 0)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    // Color attribute (location 1)
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    engine->bone_program = create_bone_program();
    if (engine->bone_program) {
        add_shader_program_to_engine(engine, engine->bone_program);
    }

    // Shadow catcher: unit quad at y=0 (scaled by planeRadius in the shader)
    engine->shadow_catcher_program = create_shadow_catcher_program();
    if (engine->shadow_catcher_program) {
        add_shader_program_to_engine(engine, engine->shadow_catcher_program);

        // CCW as seen from above (+y) so the upward face is the front face
        const float catcher_quad[] = {
            -1.0f, 0.0f, -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f,
            -1.0f, 0.0f, -1.0f, 1.0f,  0.0f, 1.0f, 1.0f, 0.0f, -1.0f,
        };
        glGenVertexArrays(1, &engine->catcher_vao);
        glGenBuffers(1, &engine->catcher_vbo);
        glBindVertexArray(engine->catcher_vao);
        glBindBuffer(GL_ARRAY_BUFFER, engine->catcher_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(catcher_quad), catcher_quad, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
    }

    // HDR post-processing (resolve + bloom + tone map). Sized at the display
    // resolution; the supersample factor enlarges the internal chain to match
    // the enlarged MSAA scene target.
    // TAAU reconstructs from the jitter, and headless suppresses the jitter
    // unless headless_jitter is set -- without it the resolve would integrate
    // one repeated sample position forever and just look permanently soft.
    // Enforced here rather than in an app because this is where all three
    // facts are known, and every headless app would otherwise have to
    // rediscover the rule.
    if (engine->headless && !engine->headless_jitter && engine->render_scale < 1.0f) {
        log_warn("render scale needs jitter under headless; rendering at full resolution");
        engine->render_scale = 1.0f;
    }
    engine->postfx = create_postfx(engine->fb_width, engine->fb_height, engine->ss_scale,
                                   engine->render_scale);
    if (!engine->postfx) {
        log_error("Failed to initialize engine post-processing");
        return -1;
    }

    postfx_set_exposure(engine->postfx, &engine->exposure);

    // Record what init just built at, or the first frame-top sync would see
    // zeroes, decide the sizes had changed, and rebuild everything once for
    // nothing -- resetting the temporal histories on frame 0 as it went.
    engine_render_size(engine, &engine->target_render_w, &engine->target_render_h);
    engine_post_size(engine, &engine->target_post_w, &engine->target_post_h);

    return 0;
}

/*
 * Callbacks
 */
void set_engine_error_callback(Engine* engine, GLFWerrorfun error_callback) {
    if (!engine)
        return;
    engine->error_callback = error_callback;
    if (engine->window) {
        glfwSetErrorCallback(error_callback);
    }
}

void set_engine_cursor_position_callback(Engine* engine,
                                         CursorPositionCallback cursor_position_callback) {
    if (!engine)
        return;
    engine->cursor_position_callback = cursor_position_callback;
}

void set_engine_mouse_button_callback(Engine* engine, MouseButtonCallback mouse_button_callback) {
    if (!engine)
        return;
    engine->mouse_button_callback = mouse_button_callback;
}

void set_engine_key_callback(Engine* engine, KeyCallback key_callback) {
    if (!engine)
        return;
    engine->key_callback = key_callback;
}

void set_engine_scroll_callback(Engine* engine, ScrollCallback scroll_callback) {
    if (!engine)
        return;
    engine->scroll_callback = scroll_callback;
}

/*
 * Mouse and Keyboard Callbacks
 */

// True when the GUI is capturing the pointer this frame, so 3D input is
// suppressed. Driven by ImGui's io capture flags.
bool engine_gui_wants_mouse(void) {
    return igGetIO_Nil()->WantCaptureMouse;
}

static bool engine_gui_wants_keyboard(void) {
    return igGetIO_Nil()->WantCaptureKeyboard;
}

// The window (or its display) changed size. Records the new sizes and the
// things derived directly from them; the render targets follow at the next
// frame top, because this fires repeatedly during a live drag and GL work
// here would be both wasted and mid-frame.
//
// This is the ONLY writer of fb_width/fb_height/win_width/win_height after
// init. That single-writer rule is what keeps "the size fields changed" and
// "the targets were rebuilt" the same event: the input callbacks used to
// refresh them too, which left the engine rendering at a size none of its
// targets had been built at.
static void _engine_framebuffer_size_callback(GLFWwindow* window, int fb_width, int fb_height) {
    if (!window)
        return;
    Engine* engine = glfwGetWindowUserPointer(window);
    if (!engine)
        return;

    engine->fb_width = fb_width;
    engine->fb_height = fb_height;
    glfwGetWindowSize(window, &engine->win_width, &engine->win_height);

    // A minimized window reports 0x0; leave the derived state alone rather
    // than computing a NaN aspect from it. _engine_sync_render_targets holds
    // the targets and engine_frame_renderable skips the frame meanwhile.
    if (fb_width <= 0 || fb_height <= 0)
        return;

    update_engine_camera_perspective(engine);
    // Window points, matching init_text_renderer -- text is authored in points
    // and the ortho must stay in the space it was set up in. Handing it
    // framebuffer pixels here would halve every string on a Retina display the
    // first time the window moved.
    if (engine->text_renderer)
        text_renderer_update_screen_size(engine->text_renderer, engine->win_width,
                                         engine->win_height);
}

static void _engine_cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
    if (!window)
        return;

    Engine* engine = glfwGetWindowUserPointer(window);
    if (!engine)
        return;

    ImGui_ImplGlfw_CursorPosCallback(window, xpos, ypos);

    if (engine_gui_wants_mouse()) {
        return;
    }

    // Map cursor to framebuffer pixels from the STORED sizes. Re-querying GLFW
    // here would make these fields change mid-frame, between the frame-top
    // resize check and the scene pass -- and a scene target that disagrees
    // with the post chain by even a frame is an invalid multisample blit. The
    // framebuffer-size callback owns them now.
    if (engine->win_width <= 0 || engine->win_height <= 0)
        return;
    xpos = ((xpos / engine->win_width) * engine->fb_width);
    ypos = (1.0 - (ypos / engine->win_height)) * engine->fb_height;

    if (engine->input.is_dragging) {
        // Calculate total offset from drag start position
        engine->input.drag_fb_x = xpos - engine->input.center_fb_x;
        engine->input.drag_fb_y = ypos - engine->input.center_fb_y;

        // Update previous position for next frame (for per-frame delta if needed)
        engine->input.prev_fb_x = xpos;
        engine->input.prev_fb_y = ypos;
    }

    if (engine->cursor_position_callback) {
        engine->cursor_position_callback(engine, xpos, ypos);
    }
}

static void _engine_mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    if (!window)
        return;

    Engine* engine = glfwGetWindowUserPointer(window);
    if (!engine)
        return;

    ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);

    double mouse_fb_x = 0.0, mouse_fb_y = 0.0;
    glfwGetCursorPos(window, &mouse_fb_x, &mouse_fb_y);

    // Stored sizes, not a fresh query -- see the cursor callback. Guarded
    // rather than returning early: a zero-area window must still be able to
    // end a drag and reach the app's own callback below.
    if (engine->win_width > 0 && engine->win_height > 0) {
        mouse_fb_x = ((mouse_fb_x / engine->win_width) * engine->fb_width);
        mouse_fb_y = ((1.0 - (mouse_fb_y / engine->win_height)) * engine->fb_height);
    }

    // A LEFT release always ends the drag and is forwarded, even over the GUI —
    // otherwise a button-up that lands on a panel leaves the camera stuck
    // orbiting. A press is acted on (drag-start, picking) only when the GUI
    // doesn't want the pointer. Either way the app callback fires exactly once.
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
        engine->input.is_dragging = false;
        engine->input.shift_held = false;
        engine->input.center_fb_x = mouse_fb_x;
        engine->input.center_fb_y = mouse_fb_y;
    } else if (engine_gui_wants_mouse()) {
        return;
    } else if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        engine->input.is_dragging = true;
        engine->input.shift_held = (mods & GLFW_MOD_SHIFT) != 0;
        engine->input.center_fb_x = mouse_fb_x;
        engine->input.center_fb_y = mouse_fb_y;
        engine->input.prev_fb_x = mouse_fb_x;
        engine->input.prev_fb_y = mouse_fb_y;

        engine->input.selected_node = _perform_engine_ray_picking(engine, mouse_fb_x, mouse_fb_y);
    }

    if (engine->mouse_button_callback) {
        engine->mouse_button_callback(engine, button, action, mods);
    }
}

static void _engine_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (!window)
        return;

    Engine* engine = glfwGetWindowUserPointer(window);
    if (!engine)
        return;

    ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);

    // Don't drive the camera / app hotkeys while the GUI has keyboard focus
    // (e.g. an active text field).
    if (engine_gui_wants_keyboard())
        return;

    if (engine->key_callback) {
        engine->key_callback(engine, key, scancode, action, mods);
    }
}

// Scroll feeds ImGui first; if the GUI isn't using the pointer, it forwards to
// the app (e.g. camera zoom).
static void _engine_scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    ImGui_ImplGlfw_ScrollCallback(window, xoffset, yoffset);

    Engine* engine = glfwGetWindowUserPointer(window);
    if (!engine || engine_gui_wants_mouse())
        return;

    if (engine->scroll_callback) {
        engine->scroll_callback(engine, xoffset, yoffset);
    }
}

/*
 * Camera
 */
void set_engine_camera(Engine* engine, Camera* camera) {
    if (!engine || !camera)
        return;

    engine->camera = camera;
}

void set_engine_camera_mode(Engine* engine, CameraMode mode) {
    if (engine) {
        engine->camera_mode = mode;
    }
}

void update_engine_camera_lookat(Engine* engine) {
    if (!engine)
        return;

    Camera* camera = engine->camera;
    if (!camera)
        return;

    glm_lookat(camera->position, camera->look_at, camera->up_vector, engine->view_matrix);
}

void update_engine_camera_perspective(Engine* engine) {
    if (!engine)
        return;

    Camera* camera = engine->camera;
    if (!camera)
        return;
    // A minimized window is 0x0, and 0/0 is a NaN that would propagate through
    // the projection into the view-proj, the frustum, and next frame's
    // reprojection. Keep the last good aspect until the window returns.
    if (engine->fb_width <= 0 || engine->fb_height <= 0)
        return;

    camera->aspect_ratio = (float)engine->fb_width / (float)engine->fb_height;
    compute_projection_matrix(camera, engine->projection_matrix);
}

/*
 * Scene
 *
 */
int add_scene_to_engine(Engine* engine, Scene* scene) {
    if (!engine || !scene)
        return -1;

    // check if scene already added to engine and return if so
    for (size_t i = 0; i < engine->scene_count; ++i) {
        if (engine->scenes[i] == scene) {
            return 0; // already added, success
        }
    }

    // Reallocate the scenes array to accommodate the new scene
    size_t new_count = engine->scene_count + 1;
    Scene** new_scenes = realloc(engine->scenes, new_count * sizeof(Scene*));
    if (!new_scenes) {
        log_error("Failed to reallocate memory for new scene");
        return -1;
    }

    // Add the new scene to the array and update the scene count
    engine->scenes = new_scenes;
    engine->scenes[engine->scene_count] = scene;
    engine->scene_count = new_count;

    return 0;
}

void set_active_scene_by_index(Engine* engine, size_t scene_index) {
    if (!engine)
        return;

    if (scene_index < engine->scene_count) {
        engine->current_scene_index = scene_index;
    } else {
        log_error("Scene index %zu is out of bounds. The engine has %zu scenes.", scene_index,
                  engine->scene_count);
    }
}

void set_active_scene_by_name(Engine* engine, const char* scene_name) {
    if (!engine || !scene_name)
        return;

    for (size_t i = 0; i < engine->scene_count; ++i) {
        if (engine->scenes[i] && strcmp(engine->scenes[i]->root_node->name, scene_name) == 0) {
            engine->current_scene_index = i;
            return;
        }
    }

    log_error("Scene named '%s' not found.", scene_name);
}

Scene* get_current_scene(const Engine* engine) {
    // Validate the engine pointer and scenes array
    if (!engine || !engine->scenes) {
        log_error("Engine or scenes array is NULL.");
        return NULL;
    }

    // Validate the current scene index
    if (engine->current_scene_index >= engine->scene_count) {
        log_error("Current scene index (%zu) is out of bounds. Total scenes: %zu.",
                  engine->current_scene_index, engine->scene_count);
        return NULL;
    }

    // Return the current scene
    return engine->scenes[engine->current_scene_index];
}

/*
 * Shaders
 *
 */

static int _create_default_shaders_for_engine(Engine* engine) {
    if (!engine)
        return -1;

    ShaderProgram* pbr_shader_program = NULL;

    if ((pbr_shader_program = create_pbr_program()) == NULL) {
        log_error("Failed to create PBR shader program");
        return -1;
    }

    add_shader_program_to_engine(engine, pbr_shader_program);

    ShaderProgram* shape_shader_program = NULL;

    if ((shape_shader_program = create_shape_program()) == NULL) {
        log_error("Failed to create shape shader program");
        return -1;
    }

    add_shader_program_to_engine(engine, shape_shader_program);

    ShaderProgram* xyz_shader_program = NULL;

    if ((xyz_shader_program = create_xyz_program()) == NULL) {
        log_error("Failed to create xyz shader program");
        return -1;
    }

    add_shader_program_to_engine(engine, xyz_shader_program);

    ShaderProgram* shadow_depth_program = NULL;

    if ((shadow_depth_program = create_shadow_depth_program()) == NULL) {
        log_error("Failed to create shadow depth shader program");
        return -1;
    }

    add_shader_program_to_engine(engine, shadow_depth_program);

    // IBL Programs
    ShaderProgram* skybox_program = create_skybox_program();
    if (skybox_program) {
        add_shader_program_to_engine(engine, skybox_program);
    }

    ShaderProgram* ibl_equirect_program = create_ibl_equirect_to_cube_program();
    if (ibl_equirect_program) {
        add_shader_program_to_engine(engine, ibl_equirect_program);
    }

    ShaderProgram* ibl_irradiance_program = create_ibl_irradiance_program();
    if (ibl_irradiance_program) {
        add_shader_program_to_engine(engine, ibl_irradiance_program);
    }

    ShaderProgram* ibl_prefilter_program = create_ibl_prefilter_program();
    if (ibl_prefilter_program) {
        add_shader_program_to_engine(engine, ibl_prefilter_program);
    }

    ShaderProgram* ibl_charlie_prefilter_program = create_ibl_charlie_prefilter_program();
    if (ibl_charlie_prefilter_program) {
        add_shader_program_to_engine(engine, ibl_charlie_prefilter_program);
    }

    ShaderProgram* ibl_brdf_program = create_ibl_brdf_program();
    if (ibl_brdf_program) {
        add_shader_program_to_engine(engine, ibl_brdf_program);
    }

    // Sky atmosphere LUT programs
    ShaderProgram* sky_transmittance_program = create_sky_transmittance_program();
    if (sky_transmittance_program) {
        add_shader_program_to_engine(engine, sky_transmittance_program);
    }

    ShaderProgram* sky_multiscatter_program = create_sky_multiscatter_program();
    if (sky_multiscatter_program) {
        add_shader_program_to_engine(engine, sky_multiscatter_program);
    }

    ShaderProgram* sky_debug_program = create_sky_debug_program();
    if (sky_debug_program) {
        add_shader_program_to_engine(engine, sky_debug_program);
    }

    ShaderProgram* sky_view_program = create_sky_view_program();
    if (sky_view_program) {
        add_shader_program_to_engine(engine, sky_view_program);
    }

    ShaderProgram* sky_env_program = create_sky_env_program();
    if (sky_env_program) {
        add_shader_program_to_engine(engine, sky_env_program);
    }

    ShaderProgram* sky_background_program = create_sky_background_program();
    if (sky_background_program) {
        add_shader_program_to_engine(engine, sky_background_program);
    }

    ShaderProgram* sky_aerial_program = create_sky_aerial_program();
    if (sky_aerial_program) {
        add_shader_program_to_engine(engine, sky_aerial_program);
    }

    ShaderProgram* cloud_noise_debug_program = create_cloud_noise_debug_program();
    if (cloud_noise_debug_program) {
        add_shader_program_to_engine(engine, cloud_noise_debug_program);
    }

    ShaderProgram* cloud_march_program = create_cloud_march_program();
    if (cloud_march_program) {
        add_shader_program_to_engine(engine, cloud_march_program);
    }

    ShaderProgram* sky_background_clouds_program = create_sky_background_clouds_program();
    if (sky_background_clouds_program) {
        add_shader_program_to_engine(engine, sky_background_clouds_program);
    }

    ShaderProgram* sky_env_clouds_program = create_sky_env_clouds_program();
    if (sky_env_clouds_program) {
        add_shader_program_to_engine(engine, sky_env_clouds_program);
    }

    ShaderProgram* mask_copy_program = create_mask_copy_program();
    if (mask_copy_program) {
        add_shader_program_to_engine(engine, mask_copy_program);
    }

    // GI probe volume. Lives on the engine rather than on PostFX -- despite
    // being a fullscreen pass -- because the volume is a Scene citizen and the
    // atlas is consumed by the scene pass, not by post.
    ShaderProgram* gi_project_program = create_gi_project_program();
    if (gi_project_program) {
        add_shader_program_to_engine(engine, gi_project_program);
    }

    return 0;
}

int add_shader_program_to_engine(Engine* engine, ShaderProgram* program) {
    if (!engine || !program) {
        log_error("Invalid input to add_shader_program_to_engine");
        return -1;
    }

    // check if program already added to engine and return if so
    for (size_t i = 0; i < engine->program_count; ++i) {
        if (engine->programs[i] == program) {
            return 0; // already added, success
        }
    }

    // Resize the programs array to accommodate the new program
    size_t new_count = engine->program_count + 1;
    ShaderProgram** new_programs = realloc(engine->programs, new_count * sizeof(ShaderProgram*));

    if (!new_programs) {
        log_error("Failed to allocate memory for new program");
        return -1;
    }

    // Add the new program to the array and update the program count
    engine->programs = new_programs;
    engine->programs[engine->program_count] = program;
    engine->program_count = new_count;

    ShaderProgram* existing;
    HASH_FIND_STR(engine->program_map, program->name, existing);
    if (!existing) {
        HASH_ADD_KEYPTR(hh, engine->program_map, program->name, strlen(program->name), program);
    }

    return 0;
}

ShaderProgram* get_engine_shader_program_by_name(Engine* engine, const char* program_name) {
    if (!engine || !program_name) {
        log_error("Invalid input to get_program_from_engine");
        return NULL;
    }

    ShaderProgram* existing;
    HASH_FIND_STR(engine->program_map, program_name, existing);
    return existing;
}

/*
 * GUI
 *
 */
void set_engine_show_gui(Engine* engine, bool show_gui) {
    if (!engine)
        return;
    engine->show_gui = show_gui;
}

void set_engine_show_fps(Engine* engine, bool show_fps) {
    if (!engine)
        return;
    engine->show_fps = show_fps;
}

void set_engine_headless(Engine* engine, bool headless) {
    if (!engine)
        return;
    engine->headless = headless;
}

void set_engine_taa_enabled(Engine* engine, bool enabled) {
    if (engine && engine->postfx)
        engine->postfx->taa_enabled = enabled;
}

void set_engine_ss_scale(Engine* engine, int ss_scale) {
    if (!engine)
        return;
    if (ss_scale < 1)
        ss_scale = 1;
    // The tone-map downsample is an exact box filter only at 2x (GL linear
    // sampling averages one 2x2 texel block); higher factors under-resolve and
    // can also exceed GL_MAX_TEXTURE_SIZE. Cap until a proper mip/N-tap
    // downsample exists.
    if (ss_scale > 2) {
        log_warn("SSAA %dx unsupported (exact downsample only at 2x); using 2x", ss_scale);
        ss_scale = 2;
    }
    engine->ss_scale = ss_scale;
}

void set_engine_render_scale(Engine* engine, float render_scale) {
    if (!engine)
        return;
    float clamped = postfx_clamp_render_scale(render_scale);
    if (clamped != render_scale) {
        log_warn("render scale %.2f outside [0.5, 1]; using %.2f", render_scale, clamped);
        render_scale = clamped;
    }
    // TAAU reconstructs from the jitter, and headless suppresses the jitter
    // unless headless_jitter is set -- without it the resolve integrates one
    // repeated sample position forever, which does not fail, it just looks
    // permanently soft. The same rule init_engine applies, re-applied because
    // this is now reachable at runtime.
    if (engine->headless && !engine->headless_jitter && render_scale < 1.0f) {
        log_warn("render scale needs jitter under headless; staying at full resolution");
        render_scale = 1.0f;
    }
    if (render_scale == engine->render_scale)
        return;
    engine->render_scale = render_scale;
    // The targets follow at the next frame top (_engine_sync_render_targets).
    // Deferred rather than rebuilt here because this is reachable from the GUI
    // panel, which draws mid-frame after the post chain has already run.
}

void set_engine_screenshot_path(Engine* engine, const char* path) {
    if (!engine)
        return;
    if (engine->screenshot_path) {
        free(engine->screenshot_path);
        engine->screenshot_path = NULL;
    }
    if (path) {
        engine->screenshot_path = safe_strdup(path);
    }
}

void set_engine_screenshot_every(Engine* engine, int every) {
    if (!engine)
        return;
    engine->screenshot_every = every > 0 ? every : 0;
}

void set_engine_exit_after_frames(Engine* engine, int frames) {
    if (!engine)
        return;
    engine->exit_after_frames = frames > 0 ? frames : 0;
}

/*
 * Build a numbered variant of a screenshot path: /tmp/shot.ppm -> /tmp/shot_000042.ppm
 */
static void _numbered_screenshot_path(const char* path, size_t frame, char* out, size_t out_size) {
    const char* dot = strrchr(path, '.');
    if (dot && dot != path) {
        snprintf(out, out_size, "%.*s_%06zu%s", (int)(dot - path), path, frame, dot);
    } else {
        snprintf(out, out_size, "%s_%06zu", path, frame);
    }
}

/*
 * Save the default framebuffer (after the post pass and GUI) to a binary
 * PPM file.
 */
static void _save_framebuffer_ppm(const Engine* engine, const char* path) {
    int w = engine->fb_width;
    int h = engine->fb_height;
    if (w <= 0 || h <= 0)
        return;

    unsigned char* pixels = malloc((size_t)w * (size_t)h * 3);
    if (!pixels) {
        log_error("Failed to allocate screenshot buffer");
        return;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels);

    FILE* f = fopen(path, "wb");
    if (!f) {
        log_error("Failed to open screenshot file: %s", path);
        free(pixels);
        return;
    }

    fprintf(f, "P6\n%d %d\n255\n", w, h);
    // GL rows are bottom-up; PPM is top-down
    for (int y = h - 1; y >= 0; y--) {
        fwrite(pixels + (size_t)y * (size_t)w * 3, 1, (size_t)w * 3, f);
    }
    fclose(f);
    free(pixels);
    log_info("Saved screenshot: %s (%dx%d)", path, w, h);
}

void engine_set_user_data(Engine* engine, void* user_data) {
    if (engine)
        engine->user_data = user_data;
}

void* engine_get_user_data(const Engine* engine) {
    return engine ? engine->user_data : NULL;
}

void engine_set_render_clock(Engine* engine, const EngineFrameClock* clock) {
    if (engine)
        engine->render_clock = clock;
}

void engine_set_render_time(Engine* engine, double time, double delta) {
    if (!engine)
        return;
    engine->render_time = time;
    engine->render_delta = delta;
}

// A checkbox that enables a group of dependent parameters. The parameters stay
// visible but greyed out and inert until the effect is on — so toggling never
// reflows the panel — and are indented one level so the grouping is obvious.
// The enabling checkbox itself stays interactive. Pair with _end_effect_group().
static void _begin_effect_group(const char* label, bool* enabled) {
    igCheckbox(label, enabled);
    igBeginDisabled(!*enabled);
    igIndent(0.0f);
}

static void _end_effect_group(void) {
    igUnindent(0.0f);
    igEndDisabled();
}

// Combo entry for one light: "1: KeyLamp (Spot)", or the type alone when the
// asset gave the light no name. The index leads because it is the only part
// guaranteed unique -- ImGui keys widgets by label, so two identically named
// lights would otherwise share one selectable.
static void _light_gui_label(char* out, size_t out_size, const Light* light, int index) {
    if (!light)
        snprintf(out, out_size, "%d: <empty>", index);
    else if (light->name && light->name[0])
        snprintf(out, out_size, "%d: %s (%s)", index, light->name, light_type_name(light->type));
    else
        snprintf(out, out_size, "%d: %s", index, light_type_name(light->type));
}

// Combo entry for one material. Same reason the index leads as for lights:
// ImGui keys widgets by label, and glTF happily ships two materials called
// "Material.001".
static const char* _material_gui_label(const Material* material, int index) {
    static char out[128];
    if (!material)
        snprintf(out, sizeof(out), "%d: <empty>", index);
    else if (material->name && material->name[0])
        snprintf(out, sizeof(out), "%d: %s", index, material->name);
    else
        snprintf(out, sizeof(out), "%d: <unnamed>", index);
    return out;
}

// The material property editor, in its own window so it can be moved, resized
// and closed. `open` is the caller's persistent flag and doubles as the close
// button's target.
static void _engine_gui_material_window(Material* material, int index, bool* open) {
    // "###" pins the window ID while the visible title tracks the selection.
    // Without it a rename or a different material would read as a new window
    // and lose the position and size the user put it in.
    char title[160];
    snprintf(title, sizeof(title), "%s###material_editor", _material_gui_label(material, index));
    if (!igBegin(title, open, 0)) {
        igEnd();
        return;
    }

    // Scope every control to the selected material. ImGui keys widgets by
    // label within a window, and these labels are the material vocabulary
    // rather than strings chosen for this panel -- so without a scope a future
    // property could silently collide with something else. Keying on the
    // selection also stops a drag on one material leaving state on the next.
    igPushID_Int(index);

    // Every row of the shared vocabulary gets a control, rather than a
    // hand-written widget per property: a table that both the scene file and
    // this window read cannot drift, and a property added for one of them turns
    // up in the other for free.
    //
    // Walked group by group in table order, so a property only has to name its
    // group to land in the right place. The first group is the one every
    // surface has and opens by default; the rest describe features a material
    // opts into, and stay shut.
    const char* shown = NULL;
    bool group_open = false;
    for (size_t i = 0; i < MATERIAL_PARAM_COUNT; i++) {
        const MaterialParam* p = &MATERIAL_PARAMS[i];
        if (!shown || strcmp(shown, p->group) != 0) {
            if (group_open)
                igUnindent(0.0f); // close the previous group's step
            shown = p->group;
            group_open = igCollapsingHeader_TreeNodeFlags(
                p->group, i == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0);
            if (group_open)
                igIndent(0.0f);
        }
        if (!group_open)
            continue;
        float v[3] = {0};
        material_param_get(material, p, v);
        bool changed = false;
        switch (p->type) {
        case MATERIAL_PARAM_VEC3:
            changed = igColorEdit3(p->key, v, ImGuiColorEditFlags_Float);
            break;
        case MATERIAL_PARAM_FLOAT:
            changed = igSliderFloat(p->key, v, p->min, p->max, "%.3f", 0);
            break;
        case MATERIAL_PARAM_INT: {
            // An enum where the table names one, not a quantity: dragging a
            // slider to reach "leaf" asks the user to know the numbering.
            int iv = (int)v[0];
            changed = p->enum_labels
                          ? igCombo_Str(p->key, &iv, p->enum_labels, 0)
                          : igSliderInt(p->key, &iv, (int)p->min, (int)p->max, "%d", 0);
            v[0] = (float)iv;
            break;
        }
        }
        if (changed)
            material_param_set(material, p, v);
    }
    if (group_open)
        igUnindent(0.0f); // the last group's step, if it ended open

    igPopID();
    igEnd();
}

// The one "environment changed on release" chain, shared by the sun sliders
// and the cloud controls so the downstream consumers cannot drift apart:
// re-bake the env WITH clouds when the layer is on (the per-drag path never
// pays that march), refresh a probe that only mirrors the sky, and re-arm
// the GI volume. A scene-captured probe (--probe-scene) is left stale --
// re-rendering the scene per release is too costly; its baked reflections
// stay as shot. The GI sweep re-arms on RELEASE, not per drag frame,
// because a sweep is one scene render per probe face -- the one cost the
// converge-then-idle cadence exists to keep off the steady state; unlike
// the probe it does not stall (spread over following frames at `rate`, old
// atlas sampleable throughout).
static void _sky_release_rebake(Engine* engine, Scene* scene, SkyAtmosphere* sky) {
    // Clouds existed this session -> the env may need the deck added or
    // purged (enabled implies noise_baked on every reachable path).
    if (sky->clouds.noise_baked)
        sky_bake_ex(sky, scene->ibl, engine, sky->clouds.enabled);
    if (scene->probe) {
        if (scene->probe->cubemap == 0)
            reflection_probe_capture(scene->probe, engine, scene, 0.1f, 100.0f, true);
        else
            log_info("Sky: scene-captured probe not refreshed on release");
    }
    gi_volume_mark_dirty(scene->gi_volume);
}

// The main settings panel, in Dear ImGui. Sections are collapsing headers;
// effect on/off states are checkboxes and their parameters appear indented
// beneath them. Bound directly to the same engine/scene/postfx fields.
static void _engine_gui_panel(Engine* engine) {
    Camera* camera = engine->camera;
    igSetNextWindowPos((ImVec2){15, 15}, ImGuiCond_FirstUseEver, (ImVec2){0, 0});
    igSetNextWindowSize((ImVec2){320, 660}, ImGuiCond_FirstUseEver);
    if (!igBegin("Cetra", NULL, 0)) {
        igEnd();
        return;
    }

    // --- View: render mode leads (the primary control), then the display
    // overlays on one row (checkboxes so their on/off state is visible) and the
    // camera mode. Global viewport controls, so they lead the panel.
    static const char* const render_modes[] = {
        "PBR",        "Normals", "World Pos",       "Tex Coords",         "Tangent Space",
        "Flat Color", "Albedo",  "Simple Lighting", "Metallic/Roughness", "Velocity"};
    int rm = engine->current_render_mode;
    int render_mode_count = (int)(sizeof(render_modes) / sizeof(render_modes[0]));
    if (igCombo_Str_arr("Render Mode", &rm, render_modes, render_mode_count, -1))
        engine->current_render_mode = (RenderMode)rm;

    bool show_xyz = engine->show_xyz;
    if (igCheckbox("XYZ", &show_xyz))
        set_engine_show_xyz(engine, show_xyz);
    igSameLine(0, -1);
    bool wireframe = engine->show_wireframe;
    if (igCheckbox("Wireframe", &wireframe))
        set_engine_show_wireframe(engine, wireframe);
    igSameLine(0, -1);
    igCheckbox("Bones", &engine->show_bones);
    igSameLine(0, -1);
    igCheckbox("Lights", &engine->show_lights);

    if (igRadioButton_Bool("Free", engine->camera_mode == CAMERA_MODE_FREE))
        engine->camera_mode = CAMERA_MODE_FREE;
    igSameLine(0, -1);
    if (igRadioButton_Bool("Orbit", engine->camera_mode == CAMERA_MODE_ORBIT))
        engine->camera_mode = CAMERA_MODE_ORBIT;

    // --- Animation: a collapsing section like the effect stacks below, shown
    // only when a clip is loaded.
    AnimationState* anim = get_render_animation_state();
    if (anim && igCollapsingHeader_TreeNodeFlags("Animation", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (igButton(anim->playing ? "Pause Animation" : "Play Animation", (ImVec2){0, 0})) {
            if (anim->playing)
                pause_animation(anim);
            else
                play_animation(anim);
        }
        if (anim->skeleton && igButton("Recalc Bind Pose", (ImVec2){0, 0}))
            recalculate_inverse_bind_poses(anim->skeleton);

        if (anim->springs &&
            igCollapsingHeader_TreeNodeFlags("Spring Bones", ImGuiTreeNodeFlags_DefaultOpen)) {
            SpringBoneSystem* sb = anim->springs;
            if (igCheckbox("Springs Enabled", &sb->enabled) && sb->enabled)
                spring_bone_reset(sb); // re-enable snaps instead of lurching
            igSliderFloat("Stiffness", &sb->params.stiffness, 0.0f, 1.0f, "%.3f", 0);
            igSliderFloat("Damping", &sb->params.damping, 0.0f, 1.0f, "%.3f", 0);
            igSliderFloat("Gravity", &sb->params.gravity, 0.0f, 30.0f, "%.2f", 0);
        }
    }

    static int mat_sel = 0;
    static bool mat_editor_open = false;
    Scene* scene = get_current_scene(engine);

    if (scene && scene->light_count > 0 &&
        igCollapsingHeader_TreeNodeFlags("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
        // The controls below address ONE light. A single slider cannot honestly
        // show two different intensities, and writing every light on a drag
        // flattens whatever key/fill ratio the scene was authored with.
        static int sel = 0;
        if (sel < 0 || sel >= (int)scene->light_count)
            sel = 0; // a scene swap can leave the index past the new count
        if (scene->light_count > 1) {
            char label[128];
            _light_gui_label(label, sizeof(label), scene->lights[sel], sel);
            if (igBeginCombo("Light", label, 0)) {
                for (size_t i = 0; i < scene->light_count; i++) {
                    char item[128];
                    _light_gui_label(item, sizeof(item), scene->lights[i], (int)i);
                    if (igSelectable_Bool(item, (int)i == sel, 0, (ImVec2){0, 0}))
                        sel = (int)i;
                    if ((int)i == sel)
                        igSetItemDefaultFocus();
                }
                igEndCombo();
            }
        }
        Light* light = scene->lights[sel];
        if (light) {
            // Shown in the unit the light was AUTHORED in, so a lamp written as
            // 30 lm in a .cscn reads 30 lm here rather than the 2.4 cd it shades
            // as. Log scale because the useful span is four decades: a domestic
            // lamp and midday sun are both on this one slider.
            static const float UNIT_MAX[] = {
                [LIGHT_UNITS_DEFAULT] = 400.0f, // unreachable; light_display_units resolves it
                [LIGHT_UNITS_CANDELA] = 400.0f, // ~5000 lm isotropic
                [LIGHT_UNITS_LUMENS] = 5000.0f, // a bright domestic fixture
                [LIGHT_UNITS_LUX] = 120000.0f,  // noon sun ~100k lx
                [LIGHT_UNITS_NITS] = 2000.0f,
            };
            LightUnits units = light_display_units(light);
            char fmt[16];
            snprintf(fmt, sizeof(fmt), "%%.1f %s", light_units_name(units));
            float intensity = light_intensity_in_units(light);
            if (igSliderFloat("Intensity", &intensity, 0.0f, UNIT_MAX[units], fmt,
                              ImGuiSliderFlags_Logarithmic))
                set_light_intensity_units(light, intensity, units);
            if (igIsItemHovered(0))
                igSetTooltip("Photometric, in the unit the light was authored in. Point/spot "
                             "shade in candela (lumens / 4pi), sun in lux, panel in nits. "
                             "Falloff is inverse-square, bounded by Range.");

            if (light->type == LIGHT_POINT || light->type == LIGHT_SPOT) {
                float range = light->range;
                if (igSliderFloat("Range", &range, 0.0f, 100.0f, "%.1f m", 0))
                    light->range = range;
                if (igIsItemHovered(0))
                    igSetTooltip("Where the inverse-square falloff is windowed to zero. Also "
                                 "the cull radius. 0 = unbounded.");
            }
        }
        // Clustered-forward occupancy: tint each fragment by how many lights
        // its cluster carries (blue 1 .. red >= 16). Directional lights are
        // shaded unclustered (they reach every fragment), so a scene lit only
        // by a key rig + IBL has nothing to show -- surface the split and grey
        // the toggle out there, rather than letting it look broken.
        if (engine->light_cluster) {
            // Grey-out keys off the scene's area-light count, not the packed
            // count: unchecking zeroes the packed count, which would lock the
            // checkbox off.
            int n_area = 0;
            for (size_t i = 0; i < scene->light_count; i++)
                if (scene->lights[i] && scene->lights[i]->type == LIGHT_AREA)
                    n_area++;
            if (n_area == 0)
                igBeginDisabled(true);
            igCheckbox("Area Lights", &engine->light_cluster->area_lights_enabled);
            if (n_area == 0) {
                igEndDisabled();
                igSameLine(0, -1);
                igTextDisabled("(no area panels in scene)");
            }

            int n_dir = engine->light_cluster->lights.light_counts[0];
            int n_clustered = engine->light_cluster->lights.light_counts[1];
            igText("%d directional (unclustered), %d clustered", n_dir, n_clustered);
            if (n_clustered == 0)
                igBeginDisabled(true);
            igCheckbox("Cluster Heatmap", &engine->cluster_debug);
            if (n_clustered == 0) {
                igEndDisabled();
                igSameLine(0, -1);
                igTextDisabled("(needs point/spot lights)");
            }
        }
    }

    // Material selection lives in this panel; the properties live in their own
    // window (raised after this one ends). Thirty controls inside an already
    // long panel pushes everything after them off the bottom, and a material is
    // the one thing here you tune while watching the frame rather than glance
    // at -- so it gets a window that can be moved, resized and closed.
    if (scene && scene->material_count > 0 &&
        igCollapsingHeader_TreeNodeFlags("Materials", 0)) {
        igIndent(0.0f);

        // Like the light section above, this addresses ONE material: a scene
        // carries dozens and a single slider cannot honestly show two values.
        if (mat_sel < 0 || mat_sel >= (int)scene->material_count)
            mat_sel = 0; // a scene swap can leave the index past the new count
        if (igBeginCombo("Material", _material_gui_label(scene->materials[mat_sel], mat_sel), 0)) {
            for (size_t i = 0; i < scene->material_count; i++) {
                if (igSelectable_Bool(_material_gui_label(scene->materials[i], (int)i),
                                      (int)i == mat_sel, 0, (ImVec2){0, 0}))
                    mat_sel = (int)i;
                if ((int)i == mat_sel)
                    igSetItemDefaultFocus();
            }
            igEndCombo();
        }
        if (igButton("Edit Material", (ImVec2){0, 0}))
            mat_editor_open = true;
        igUnindent(0.0f);
    }

    if (scene && scene->render_skybox && scene->ibl &&
        igCollapsingHeader_TreeNodeFlags("Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (scene->sky) {
            SkyAtmosphere* sky = scene->sky;
            igSeparatorText("Sky");
            // Elevation dips a few degrees below the horizon for dusk; the
            // atmosphere fades the key light out there. A sun move re-derives
            // the direction and re-bakes the LUT/env/IBL live (cheap at this
            // env size) and retints the coupled key light through one path.
            bool sun_moved = false;
            bool sun_released = false;
            sun_moved |=
                igSliderFloat("Sun Elevation", &sky->sun_elevation_deg, -6.0f, 89.0f, "%.1f", 0);
            sun_released |= igIsItemDeactivatedAfterEdit();
            sun_moved |=
                igSliderFloat("Sun Azimuth", &sky->sun_azimuth_deg, 0.0f, 360.0f, "%.1f", 0);
            sun_released |= igIsItemDeactivatedAfterEdit();
            // Disc size feeds only the analytic background sun (sampled live);
            // it is not in the env cube, so it needs no re-bake.
            igSliderFloat("Sun Disc", &sky->sun_disc_deg, 0.1f, 3.0f, "%.2f", 0);
            if (sun_moved)
                sky_update_sun(sky, scene->ibl, engine);
            if (sun_released)
                _sky_release_rebake(engine, scene, sky);

            // Cloud layer (only offered once the noise fields exist). The
            // screen march follows every slider live; the env/IBL copy and
            // its downstream consumers update on RELEASE like the sun (the
            // with-clouds bake is the cost the drag path never pays). Wind
            // re-bakes nothing -- the env cube holds a still of the deck by
            // design.
            if (sky->clouds.noise_baked) {
                bool clouds_were_on = sky->clouds.enabled;
                bool cloud_edit = false;
                _begin_effect_group("Clouds", &sky->clouds.enabled);
                igSliderFloat("Coverage", &sky->clouds.coverage, 0.0f, 1.0f, "%.2f", 0);
                cloud_edit |= igIsItemDeactivatedAfterEdit();
                igSliderFloat("Cloud Type", &sky->clouds.cloud_type, 0.0f, 1.0f, "%.2f", 0);
                cloud_edit |= igIsItemDeactivatedAfterEdit();
                igSliderFloat("Cloud Density", &sky->clouds.density, 0.1f, 3.0f, "%.2f", 0);
                cloud_edit |= igIsItemDeactivatedAfterEdit();
                igSliderFloat("Wind km/h", &sky->clouds.wind_speed_kmh, 0.0f, 300.0f, "%.0f", 0);
                igSliderFloat("Wind Dir", &sky->clouds.wind_dir_deg, 0.0f, 360.0f, "%.0f", 0);
                _end_effect_group();
                bool toggled = clouds_were_on != sky->clouds.enabled;
                if (toggled || (cloud_edit && sky->clouds.enabled))
                    _sky_release_rebake(engine, scene, sky);
            }
        }

        _begin_effect_group("Ground Projection", &scene->skybox_ground_projection);
        // Log scale + wide range: the dome radius is a world-space distance
        // apps typically scale to the scene, anywhere from a few units
        // (meter-scale models) to thousands (large-unit assets).
        igSliderFloat("Dome Radius", &scene->skybox_gp_radius, 1.0f, 10000.0f, "%.2f",
                      ImGuiSliderFlags_Logarithmic);
        igSliderFloat("Capture Height", &scene->skybox_gp_height, 0.1f, 10.0f, "%.2f", 0);
        _end_effect_group();

        igSliderFloat("IBL Intensity", &scene->ibl->intensity, 0.0f, 4.0f, "%.2f", 0);

        if (scene->shadow_system) {
            ShadowSystem* ss = scene->shadow_system;
            _begin_effect_group("Shadows", &ss->enabled);
            // Count change takes effect next frame via the depth pass's
            // rebuild-on-change; 1 = the classic scene-fit single map.
            // AlwaysClamp: Ctrl+Click text entry must not escape the range
            // (the count sizes heap array writes in the depth pass).
            igSliderInt("Cascades", &ss->cascade_count, 1, SHADOW_CASCADES, "%d",
                        ImGuiSliderFlags_AlwaysClamp);
            igCheckbox("Cascade Tint", &ss->csm_debug);
            _begin_effect_group("PCSS", &ss->pcss_enabled);
            igSliderFloat("Shadow Softness", &ss->pcss_softness, 0.0f, 4.0f, "%.2f", 0);
            _end_effect_group();
            _end_effect_group();
        }

        _begin_effect_group("Shadow Catcher", &scene->shadow_catcher);
        igSliderFloat("Shadow Strength", &scene->shadow_catcher_strength, 0.0f, 1.0f, "%.2f", 0);
        _end_effect_group();

        // Attached probes always carry a capture: the toggle switches
        // consumption, it does not recapture
        if (scene->probe) {
            _begin_effect_group("Reflection Probe", &scene->probe->enabled);
            igSliderFloat("Probe Intensity", &scene->probe->intensity, 0.0f, 4.0f, "%.2f", 0);
            igSliderFloat("Box Fade", &scene->probe->box_fade, 0.0f, 0.5f, "%.2f", 0);
            igCheckbox("Show Capture", &scene->probe->debug_background);
            _end_effect_group();
        }

        if (scene->gi_volume) {
            GIVolume* gi = scene->gi_volume;
            _begin_effect_group("GI Probe Volume", &gi->enabled);
            igText("%dx%dx%d probes, %dx%d atlas", gi->counts[0], gi->counts[1], gi->counts[2],
                   gi->atlas_w, gi->atlas_h);
            // The one number worth watching: a converged volume reads 0 and does
            // no work at all. Anything else means captures are running.
            igText(gi->dirty_count > 0 ? "converging: %d probes left" : "converged (%d)",
                   gi->dirty_count);
            igSliderInt("Probes / Frame", &gi->rate, 0, 32, "%d", 0);
            igCheckbox("Show Atlas", &gi->debug_atlas);
            if (igButton("Recapture", (ImVec2){0, 0}))
                gi_volume_mark_dirty(gi);
            _end_effect_group();
        }
    }

    if (engine->postfx &&
        igCollapsingHeader_TreeNodeFlags("Post", ImGuiTreeNodeFlags_DefaultOpen)) {
        PostFX* fx = engine->postfx;

        igSeparatorText("Anti-aliasing");
        bool msaa = engine->msaa_samples > 1;
        if (igCheckbox("MSAA 4x", &msaa))
            set_engine_msaa_samples(engine, msaa ? 4 : 1);
        igSameLine(0, -1);
        bool taa = fx->taa_enabled;
        if (igCheckbox("TAA", &taa))
            set_engine_taa_enabled(engine, taa);
        // TAAU render scale. Applied on release, not per drag tick: each change
        // rebuilds every render target and resets the temporal histories (the
        // sun/cloud sliders defer their re-bake the same way). AlwaysClamp
        // because Ctrl+click types a value straight into an allocation size.
        static float pending_scale = 1.0f;
        igSliderFloat("Render Scale", &pending_scale, 0.5f, 1.0f, "%.2f",
                      ImGuiSliderFlags_AlwaysClamp);
        if (igIsItemDeactivatedAfterEdit())
            set_engine_render_scale(engine, pending_scale);
        else if (!igIsItemActive())
            // Not being dragged: track the live value, so a scale the setter
            // clamped or refused shows here rather than the stale request.
            pending_scale = engine->render_scale;
        if (!fx->taa_enabled && engine->render_scale < 1.0f) {
            // Without the resolve there is nothing to reconstruct with, so the
            // frame is magnified rather than upscaled: softer, not faster.
            igTextDisabled("(needs TAA to reconstruct; magnifying)");
        }

        // Index maps to the enum minus PASSTHROUGH, which is not a look and
        // stays out of the picker
        static const char* const tonemap_names[] = {"ACES", "PBR Neutral", "AgX"};
        int tm = (int)fx->tonemap_mode - 1;
        if (igCombo_Str_arr("Tonemap", &tm, tonemap_names,
                            (int)(sizeof(tonemap_names) / sizeof(tonemap_names[0])), -1))
            fx->tonemap_mode = (PostFXTonemapMode)(tm + 1);
        igCheckbox("Auto Exposure", &engine->exposure.automatic);
        // Two controls because they are two quantities, shown one at a time.
        // One slider over one range could not serve both: under a physical
        // camera the value is stops, where the linear range 0.05..8 cannot
        // reach 0 (neutral) and cannot stop down at all.
        if (engine->exposure.physical) {
            igSliderFloat("EV Bias", &engine->exposure.bias_stops, -6.0f, 6.0f, "%+.2f EV", 0);
            if (igIsItemHovered(0))
                igSetTooltip("Exposure compensation on the physical camera. Positive opens up.");
        } else {
            igSliderFloat("Exposure", &engine->exposure.multiplier, 0.05f, 8.0f, "%.2f", 0);
            if (igIsItemHovered(0))
                igSetTooltip("Linear multiplier. Authoring a post.camera in a .cscn switches "
                             "this to an EV bias instead.");
        }

        _begin_effect_group("Bloom", &fx->bloom_enabled);
        igSliderFloat("Bloom Strength", &fx->bloom_strength, 0.0f, 0.1f, "%.3f", 0);
        igSliderFloat("Bloom Threshold", &fx->bloom_threshold, 0.0f, 8.0f, "%.2f", 0);
        _end_effect_group();

        _begin_effect_group("Ambient Occlusion (GTAO)", &fx->ssao_enabled);
        // Log scale + wide range: the AO/GI reach is a world-space distance, so
        // apps scale it to the scene (meter-scale models sit near the bottom,
        // large-unit scenes in the hundreds).
        igSliderFloat("AO Radius", &fx->ssao_radius, 0.05f, 1000.0f, "%.2f",
                      ImGuiSliderFlags_Logarithmic);
        igSliderFloat("AO Strength", &fx->ssao_strength, 0.0f, 1.0f, "%.2f", 0);
        static const char* const spec_occ_names[] = {"Off", "Legacy", "Bent normal", "Split"};
        int so = (int)fx->spec_occlusion_mode;
        if (igCombo_Str_arr("Specular Occlusion", &so, spec_occ_names,
                            (int)(sizeof(spec_occ_names) / sizeof(spec_occ_names[0])), -1))
            fx->spec_occlusion_mode = (PostFXSpecOccMode)so;
        igCheckbox("AO Edge Filter", &fx->ao_edge_filter_enabled);
        _end_effect_group();

        _begin_effect_group("Contact Shadows", &fx->contact_shadows_enabled);
        igSliderFloat("CS Strength", &fx->cs_strength, 0.0f, 1.0f, "%.2f", 0);
        // Log scale + wide range: the march reach is a world-space distance the
        // app scales to the scene, like SSR Distance.
        igSliderFloat("CS Distance", &fx->cs_distance, 0.01f, 1000.0f, "%.3f",
                      ImGuiSliderFlags_Logarithmic);
        // The pass needs a shadow-casting directional (its published direction);
        // say so rather than let the toggle look inert.
        if (fx->fog_light_count == 0)
            igTextDisabled("(needs a shadow-casting directional light)");
        _end_effect_group();

        _begin_effect_group("SSR", &fx->ssr_enabled);
        igSliderFloat("SSR Strength", &fx->ssr_strength, 0.0f, 2.0f, "%.2f", 0);
        // Log scale + wide range: the march reach is a world-space distance
        // the app scales to the scene (a couple of units on meter-scale
        // models, thousands on large-unit assets)
        igSliderFloat("SSR Distance", &fx->ssr_max_distance, 1.0f, 20000.0f, "%.2f",
                      ImGuiSliderFlags_Logarithmic);
        igSliderFloat("SSR Floor Rough", &fx->ssr_floor_roughness, 0.0f, 1.0f, "%.2f", 0);
        bool ssr_full_res = fx->ssr_full_res;
        if (igCheckbox("SSR Full Res", &ssr_full_res))
            postfx_set_ssr_full_res(fx, ssr_full_res);
        igCheckbox("SSR Temporal (needs TAA)", &fx->ssr_temporal);
        igCheckbox("SSR Denoise", &fx->ssr_denoise);
        igSliderFloat("SSR Jitter", &fx->ssr_jitter, 0.0f, 0.2f, "%.3f", 0);
        _end_effect_group();

        _begin_effect_group("SSGI", &fx->ssgi_enabled);
        igSliderFloat("GI Intensity", &fx->ssgi_intensity, 0.0f, 4.0f, "%.2f", 0);
        _end_effect_group();

        _begin_effect_group("Volumetric Fog", &fx->fog_enabled);
        // Log scale + wide range: density and falloff are world-space
        // quantities apps scale to the scene (tiny values on large-unit
        // assets) — the SSR Distance slider precedent
        igSliderFloat("Fog Density", &fx->fog_density, 0.00001f, 1.0f, "%.5f",
                      ImGuiSliderFlags_Logarithmic);
        igSliderFloat("Height Falloff", &fx->fog_height_falloff, 0.05f, 5000.0f, "%.2f",
                      ImGuiSliderFlags_Logarithmic);
        igSliderFloat("Anisotropy", &fx->fog_anisotropy, -0.9f, 0.9f, "%.2f", 0);
        igSliderFloat("Sun Boost", &fx->fog_sun_boost, 0.0f, 8.0f, "%.2f", 0);
        // The volume's own depth range, which is NOT the camera's: the mapping
        // is exponential, so a near pinned to the clip plane spends most of the
        // slices on air in front of the lens. 0 derives it from Far.
        igSliderFloat("Volume Near", &fx->fog_near, 0.0f, 20.0f, "%.2f",
                      ImGuiSliderFlags_Logarithmic);
        igSliderFloat("Volume Far", &fx->fog_far, 1.0f, 5000.0f, "%.1f",
                      ImGuiSliderFlags_Logarithmic);
        igSliderFloat("Depth Distribution", &fx->fog_depth_dist, 0.25f, 4.0f, "%.2f", 0);
        igSliderFloat("Temporal Blend", &fx->fog_temporal_blend, 0.0f, 0.98f, "%.2f", 0);
        // Grid XY is what resolves a beam's silhouette; a change reallocates the
        // volumes and restarts their history at the next fog frame.
        igSliderInt("Grid X", &fx->froxel_grid_x, 40, 640, "%d", 0);
        igSliderInt("Grid Y", &fx->froxel_grid_y, 24, 360, "%d", 0);
        igSliderInt("Grid Z", &fx->froxel_grid_z, 16, 256, "%d", 0);
        // Shadow softness in the medium. k is the exponential's sharpness: high
        // converges back toward the hard binary compare, low goes soft and
        // starts leaking light through blockers. Sweeping it IS the demo.
        igCheckbox("ESM Shadows", &fx->fog_esm_enabled);
        igSliderFloat("ESM Sharpness", &fx->fog_esm_k, 5.0f, 200.0f, "%.0f",
                      ImGuiSliderFlags_Logarithmic);
        // Editing takes ownership away from the sky, which otherwise republishes
        // its own zenith radiance every frame and the picker would snap back.
        if (igColorEdit3("Fog Ambient", fx->fog_ambient, 0) && scene && scene->sky)
            scene->sky->publish_fog_ambient = false;
        _end_effect_group();

        igCheckbox("Normals G-buffer", &fx->normals_enabled);
        igSliderFloat("Spec AA", &engine->specular_aa_strength, 0.0f, 2.0f, "%.2f", 0);
        igCheckbox("Energy Comp", &engine->energy_comp_enabled);
        igCheckbox("Clearcoat", &engine->clearcoat_enabled);
        igCheckbox("Specular", &engine->specular_enabled);
        igCheckbox("Sheen", &engine->sheen_enabled);
        igCheckbox("Parallax (POM)", &engine->parallax_enabled);
        igCheckbox("Subsurface (SSS)", &engine->sss_enabled);
        igCheckbox("Skin Pre-integration", &engine->skin_preint_enabled);
        // Moment weighting is a better weight INSIDE the accumulate, not a
        // second transparency path, so it greys out with the pass it rides.
        _begin_effect_group("OIT", &engine->oit_enabled);
        igCheckbox("OIT Moment Weighting", &engine->oit_moments_enabled);
        _end_effect_group();
    }

    if (engine->postfx &&
        igCollapsingHeader_TreeNodeFlags("Finishing", ImGuiTreeNodeFlags_DefaultOpen)) {
        PostFX* fx = engine->postfx;

        _begin_effect_group("Vignette", &fx->vignette_enabled);
        igSliderFloat("Vig Strength", &fx->vignette_strength, 0.0f, 1.0f, "%.2f", 0);
        igSliderFloat("Vig Radius", &fx->vignette_radius, 0.0f, 1.0f, "%.2f", 0);
        _end_effect_group();

        _begin_effect_group("Sharpen", &fx->sharpen_enabled);
        igSliderFloat("Sharpen Amount", &fx->sharpen_strength, 0.0f, 3.0f, "%.2f", 0);
        _end_effect_group();

        _begin_effect_group("Grain", &fx->grain_enabled);
        igSliderFloat("Grain Amount", &fx->grain_strength, 0.0f, 0.3f, "%.3f", 0);
        _end_effect_group();

        _begin_effect_group("Color Grade", &fx->grade_enabled);
        igDragFloat3("Lift", fx->grade_lift, 0.005f, -1.0f, 1.0f, "%.3f", 0);
        igDragFloat3("Gamma", fx->grade_gamma, 0.01f, 0.1f, 4.0f, "%.3f", 0);
        igDragFloat3("Gain", fx->grade_gain, 0.01f, 0.0f, 4.0f, "%.3f", 0);
        _end_effect_group();

        _begin_effect_group("Depth of Field", &fx->dof_enabled);
        igCheckbox("Autofocus", &fx->dof_autofocus);
        // Manual focus distance is only meaningful when autofocus is off.
        igBeginDisabled(fx->dof_autofocus);
        igSliderFloat("Focus Dist", &fx->dof_focus_distance, 0.0f, 100.0f, "%.2f", 0);
        igEndDisabled();
        igSliderFloat("Focus Range", &fx->dof_focus_range, 0.1f, 100.0f, "%.2f", 0);
        igSliderFloat("Max CoC", &fx->dof_max_coc, 0.0f, 40.0f, "%.1f", 0);
        igSliderInt("Blades", &fx->dof_blades, 0, 9, "%d", 0);
        igSliderFloat("Rotation", &fx->dof_rotation, 0.0f, 180.0f, "%.0f deg", 0);
        _end_effect_group();

        _begin_effect_group("Motion Blur", &fx->motion_blur_enabled);
        igSliderFloat("Shutter", &fx->motion_blur_scale, 0.0f, 2.0f, "%.2f", 0);
        _end_effect_group();
    }

    if (camera && igCollapsingHeader_TreeNodeFlags("Camera Transform", 0)) {
        igDragFloat3("Look At", camera->look_at, 0.1f, -100.0f, 100.0f, "%.2f", 0);
        igDragFloat3("Up", camera->up_vector, 0.1f, -25.0f, 25.0f, "%.2f", 0);
        igSliderFloat("Distance", &camera->distance, 0.0f, 3000.0f, "%.2f", 0);
        igSliderFloat("Height", &camera->height, -2000.0f, 2000.0f, "%.1f", 0);
        igSliderFloat("Theta", &camera->theta, 0.0f, GLM_PI_2, "%.3f", 0);
        igSliderFloat("Phi", &camera->phi, 0.0f, GLM_PI_2, "%.3f", 0);
        igSliderFloat("FOV", &camera->fov_radians, 0.1f, GLM_PI, "%.3f", 0);
        igSliderFloat("Zoom Speed", &camera->zoom_speed, 0.0f, 2.0f, "%.3f", 0);
        igSliderFloat("Orbit Speed", &camera->orbit_speed, 0.0f, 0.1f, "%.4f", 0);
        igSliderFloat("Amplitude", &camera->amplitude, 0.0f, 50.0f, "%.2f", 0);
        igSliderFloat("Near Clip", &camera->near_clip, 0.01f, 100.0f, "%.3f", 0);
        igSliderFloat("Far Clip", &camera->far_clip, 0.1f, 10000.0f, "%.1f", 0);
        // Camera diagnostic: overlay the live pose next to the FPS, and print an
        // exact-repro CLI line to stdout so a grazing interactive view can be
        // reproduced headlessly (--cam-eye/--cam-target override the orbit framing).
        igCheckbox("Camera Info (HUD)", &engine->show_camera_hud);
        if (igButton("Print Camera", (ImVec2){0, 0})) {
            printf("-W %d -H %d --fov %.1f --cam-eye %.3f,%.3f,%.3f "
                   "--cam-target %.3f,%.3f,%.3f --cam-up %.3f,%.3f,%.3f\n",
                   engine->fb_width, engine->fb_height, glm_deg(camera->fov_radians),
                   camera->position[0], camera->position[1], camera->position[2],
                   camera->look_at[0], camera->look_at[1], camera->look_at[2], camera->up_vector[0],
                   camera->up_vector[1], camera->up_vector[2]);
            fflush(stdout);
        }
    }

    // Read while this window is still current, to place the material editor
    // beside it rather than on top of it.
    ImVec2 panel_pos = igGetWindowPos();
    ImVec2 panel_size = igGetWindowSize();

    igEnd();

    // A sibling window, so it opens outside the main panel rather than pushing
    // everything after it off the bottom. Raised after igEnd() because a
    // material can disappear under it -- a scene swap leaves the index stale,
    // and the bounds check has to run against whatever scene is current now.
    if (mat_editor_open && scene && scene->material_count > 0) {
        if (mat_sel < 0 || mat_sel >= (int)scene->material_count)
            mat_sel = 0;
        if (scene->materials[mat_sel]) {
            // Both FirstUseEver, so this is a starting point and never fights a
            // window the user has since moved or resized (or one restored from
            // imgui.ini).
            //
            // The size is not cosmetic. A window with no size auto-fits its
            // contents, but a slider's width is derived FROM the window width,
            // so the two settle on something far too narrow and every label
            // clips -- the property names are the widest thing here and they
            // sit to the right of each control. 470 clears the longest of them
            // at ImGui's default 65/35 split.
            igSetNextWindowPos((ImVec2){panel_pos.x + panel_size.x + 12.0f, panel_pos.y},
                               ImGuiCond_FirstUseEver, (ImVec2){0, 0});
            igSetNextWindowSize((ImVec2){470.0f, 560.0f}, ImGuiCond_FirstUseEver);
            _engine_gui_material_window(scene->materials[mat_sel], mat_sel, &mat_editor_open);
        }
    }
}

// Frameless FPS readout pinned to the top-right corner, with an optional live
// camera-pose block (show_camera_hud) beneath it. FPS-only keeps the original
// 90px/transparent placement byte-for-byte.
static void _engine_gui_fps_overlay(Engine* engine) {
    bool cam = engine->show_camera_hud && engine->camera;
    float width = cam ? 300.0f : 90.0f;
    igSetNextWindowPos((ImVec2){(float)engine->win_width - width, 10.0f}, ImGuiCond_Always,
                       (ImVec2){0, 0});
    igSetNextWindowBgAlpha(cam ? 0.35f : 0.0f);
    if (igBegin("##fps", NULL,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_AlwaysAutoResize)) {
        if (engine->show_fps)
            igText("%.1f FPS", engine->fps);
        if (cam) {
            Camera* c = engine->camera;
            igText("eye    %.1f %.1f %.1f", c->position[0], c->position[1], c->position[2]);
            igText("target %.1f %.1f %.1f", c->look_at[0], c->look_at[1], c->look_at[2]);
            igText("fov %.1f  dist %.1f  %dx%d", glm_deg(c->fov_radians),
                   glm_vec3_distance(c->position, c->look_at), engine->fb_width, engine->fb_height);
        }
    }
    igEnd();
}

// Build and render the engine's ImGui panels. NewFrame ran at the top of the
// render loop and latched gui_frame_active; this adds the windows and flushes
// the draw data, gated on the same latch so igRender always pairs with it.
static void render_engine_gui(Engine* engine) {
    if (!engine || !engine->gui_frame_active)
        return;

    if (engine->show_gui && engine->camera)
        _engine_gui_panel(engine);
    if (engine->show_fps || engine->show_camera_hud)
        _engine_gui_fps_overlay(engine);

    igRender();
    ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());
}

/*
 * Render
 */
void set_engine_show_wireframe(Engine* engine, bool show_wireframe) {
    if (!engine)
        return;

    engine->show_wireframe = show_wireframe;

    if (show_wireframe) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
    }
}

void set_engine_show_xyz(Engine* engine, bool show_xyz) {
    if (!engine)
        return;

    engine->show_xyz = show_xyz;

    for (size_t i = 0; i < engine->scene_count; ++i) {
        Scene* scene = engine->scenes[i];
        if (scene) {
            SceneNode* root_node = scene->root_node;
            if (!root_node)
                continue;
            set_show_xyz_for_nodes(root_node, show_xyz);
        }
    }
}

void engine_present_frame(Engine* engine, RenderMode frame_mode) {
    if (!engine)
        return;

    // Only PBR frames are linear HDR and get SSAO + bloom + exposure + tone
    // mapping; debug render modes emit display-ready colors and are copied
    // unchanged. The GUI draws after so it is never tone mapped.
    // Seed the film grain off the frame counter so it animates live yet stays
    // deterministic across equal --frames runs.
    engine->postfx->frame_index = (int)engine->total_frames;
    // Depth-of-field autofocus: keep the subject sharp as the camera orbits or
    // zooms by refocusing on the point the camera looks at (the orbit target),
    // unless a manual focus distance was pinned.
    if (engine->postfx->dof_enabled && engine->postfx->dof_autofocus && engine->camera) {
        engine->postfx->dof_focus_distance =
            glm_vec3_distance(engine->camera->position, engine->camera->look_at);
    }
    // Hand the current scene's reflection probe and shadow casters to postfx
    // (SSR miss fallback / fog march) without postfx learning about Scene
    const Scene* fx_scene = get_current_scene(engine);
    reflection_probe_publish_to_postfx(fx_scene ? fx_scene->probe : NULL, engine->postfx);
    shadow_publish_to_postfx(fx_scene, engine->postfx);
    // Aerial perspective is a camera-frustum volume, so unlike the sky's other
    // LUTs it is rebuilt here every frame, immediately before it is published.
    // The unjittered projection: the bake reads only [0][0]/[1][1]/[2][2]/[3][2]
    // so TAA's jitter in [2][0]/[2][1] cannot reach it, and handing it an input
    // that churns every frame would defeat any future rebuild-elision.
    if (fx_scene && fx_scene->sky)
        sky_update_aerial(fx_scene->sky, engine->view_matrix, engine->projection_matrix);
    sky_publish_to_postfx(fx_scene ? fx_scene->sky : NULL, engine->postfx);
    const PostFXGBufferWrites writes = {.normals = engine->normals_this_frame,
                                        .aux = engine->aux_this_frame,
                                        .albedo = engine->albedo_this_frame,
                                        .sss = engine->sss_this_frame,
                                        .spec = engine->spec_this_frame,
                                        .oit_fbo =
                                            engine->oit_this_frame ? engine->oit_fbo : 0,
                                        .oit_moment_weighted = engine->moments_this_frame};
    postfx_run(engine->postfx, engine->framebuffer, 0, frame_mode == RENDER_MODE_PBR, &writes,
               engine->draw_projection, engine->view_matrix);

    // Sky LUT debug overlay onto the composited frame (an acceptance tool,
    // the csm_debug shape: a library-side flag the app/GUI toggles)
    if (fx_scene && fx_scene->sky && fx_scene->sky->debug_luts) {
        sky_debug_blit_luts(fx_scene->sky, engine->fb_width, engine->fb_height);
    }
    if (fx_scene && fx_scene->gi_volume && fx_scene->gi_volume->debug_atlas) {
        gi_volume_debug_blit(fx_scene->gi_volume, engine, engine->fb_width, engine->fb_height);
    }

    // GUI last, after tone mapping. render_engine_gui self-gates on
    // gui_frame_active, so it no-ops when no panel/overlay is enabled.
    render_engine_gui(engine);
}

void engine_set_scene_draw_buffers(const Engine* engine, bool with_gbuffer) {
    if (!engine)
        return;

    // The opaque pass may publish G-buffer attachments 1 (normals), 2 (aux:
    // motion .xy + linear view-Z .z), and 3 (albedo) alongside color; every other
    // pass is color only. Build the draw-buffer list left to right: GL_NONE fills
    // a skipped slot so the fragment-output-location -> attachment mapping is
    // preserved, and count extends to the highest enabled attachment. Blending is
    // enabled globally, so keep each aux target opaque via indexed disable
    // (re-issued at every pass boundary because any blanket glEnable(GL_BLEND)
    // wipes it).
    // Read each attachment's per-frame write gate from the same descriptor table
    // that drives create/clear (const cast: this only reads the this_frame flags).
    // Attachment 0 (this_frame NULL) is always written and starts the list.
    //
    // A capture target has attachment 0 and nothing else, so the whole G-buffer
    // is off for the duration -- asked HERE, once, rather than by clearing each
    // this_frame flag at the capture site. That list was hand-maintained and had
    // already fallen behind: attachment 4 (SSS) was added without one, so a
    // re-convergence sweep in an --sss scene emitted a five-attachment draw list
    // against a one-attachment FBO. Gating the loop instead is correct for
    // attachment 4 and for every attachment added after it.
    const bool gbuffer = with_gbuffer && !engine->capturing;
    GBufferAttachment gb[GBUFFER_ATTACHMENT_COUNT];
    _gbuffer_attachments((Engine*)engine, gb);
    // bufs[slot] carries GL_COLOR_ATTACHMENT0+slot or GL_NONE (0), so
    // fragment output location N always lands on attachment N. Slots 5/6 stay
    // GL_NONE here -- those locations are the OIT accumulate targets, bound on
    // the OIT FBO.
    GLenum bufs[GBUFFER_DRAW_BUFFER_SLOTS] = {GL_COLOR_ATTACHMENT0}; // rest GL_NONE (0)
    int count = 1;
    for (int i = 1; i < GBUFFER_ATTACHMENT_COUNT; i++) {
        if (gbuffer && *gb[i].this_frame) {
            int slot = (int)(gb[i].attachment - GL_COLOR_ATTACHMENT0);
            bufs[slot] = gb[i].attachment;
            glDisablei(GL_BLEND, slot);
            if (slot + 1 > count)
                count = slot + 1;
        }
    }
    glDrawBuffers(count, bufs);
}

// Refraction blurs the resolved scene by sampling coarser mips; the shader
// clamps its LOD to this, so mip generation stops here too (levels past it
// would never be read)
#define OPAQUE_COLOR_MAX_LOD 6

// Lazy allocation for the refraction source (the postfx ensure_* pattern):
// created on first transmissive frame at the internal render size and
// recreated if that ever changes. Leaves the new FBO bound on success.
static bool _ensure_opaque_color_target(Engine* engine, int rw, int rh) {
    if (engine->opaque_color_texture != 0 && engine->opaque_color_w == rw &&
        engine->opaque_color_h == rh) {
        return true;
    }
    if (engine->opaque_color_texture) {
        glDeleteTextures(1, &engine->opaque_color_texture);
        glDeleteFramebuffers(1, &engine->opaque_color_fbo);
    }
    glGenTextures(1, &engine->opaque_color_texture);
    glBindTexture(GL_TEXTURE_2D, engine->opaque_color_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, rw, rh, 0, GL_RGBA, GL_FLOAT, NULL);
    // CLAMP: refraction offsets must not wrap to the far edge; LINEAR
    // mipmap so roughness-scaled textureLod blends between blur levels
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, OPAQUE_COLOR_MAX_LOD);
    glGenFramebuffers(1, &engine->opaque_color_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, engine->opaque_color_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           engine->opaque_color_texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Opaque-color resolve framebuffer incomplete");
        gl_delete_texture(&engine->opaque_color_texture);
        gl_delete_fbo(&engine->opaque_color_fbo);
        return false;
    }
    engine->opaque_color_w = rw;
    engine->opaque_color_h = rh;
    return true;
}

// Resolve the MSAA color attachment (opaques + skybox) into the mipped
// single-sample texture transmissive surfaces sample for screen-space
// refraction. The copy is what makes sampling legal: reading the scene
// FBO while drawing into it would be a framebuffer feedback loop.
// Exit state: engine->framebuffer bound with its draw-buffer list intact
// (per-FBO state) and the active texture unit restored to 0, so the late
// pass resumes untouched. Only valid while engine->framebuffer holds the
// scene being drawn -- captures that redirect the scene into another FBO
// (the reflection probe) must disable refraction for the capture.
bool engine_resolve_opaque_color(Engine* engine) {
    int rw, rh;
    engine_render_size(engine, &rw, &rh);
    if (rw <= 0 || rh <= 0)
        return false;
    if (!_ensure_opaque_color_target(engine, rw, rh)) {
        glBindFramebuffer(GL_FRAMEBUFFER, engine->framebuffer);
        return false;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, engine->framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, engine->opaque_color_fbo);
    glBlitFramebuffer(0, 0, rw, rh, 0, 0, rw, rh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, engine->opaque_color_texture);
    glGenerateMipmap(GL_TEXTURE_2D); // box mips (to MAX_LEVEL) = the blur chain
    glBindFramebuffer(GL_FRAMEBUFFER, engine->framebuffer);
    return true;
}

// Lazy single-sample depth resolve target (mirrors _ensure_opaque_color_target).
// DEPTH24_STENCIL8 matches the multisample depth renderbuffer so the depth blit
// is format-compatible; sampled as sampler2D it returns window-space depth in .r.
static bool _ensure_scene_depth_target(Engine* engine, int rw, int rh) {
    if (engine->scene_depth_texture != 0 && engine->scene_depth_w == rw &&
        engine->scene_depth_h == rh) {
        return true;
    }
    if (engine->scene_depth_texture) {
        glDeleteTextures(1, &engine->scene_depth_texture);
        glDeleteFramebuffers(1, &engine->scene_depth_fbo);
    }
    glGenTextures(1, &engine->scene_depth_texture);
    glBindTexture(GL_TEXTURE_2D, engine->scene_depth_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, rw, rh, 0, GL_DEPTH_STENCIL,
                 GL_UNSIGNED_INT_24_8, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &engine->scene_depth_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, engine->scene_depth_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                           engine->scene_depth_texture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Scene-depth resolve framebuffer incomplete");
        gl_delete_texture(&engine->scene_depth_texture);
        gl_delete_fbo(&engine->scene_depth_fbo);
        return false;
    }
    engine->scene_depth_w = rw;
    engine->scene_depth_h = rh;
    return true;
}

GLuint engine_resolve_scene_depth(Engine* engine) {
    if (!engine)
        return 0;
    int rw, rh;
    engine_render_size(engine, &rw, &rh);
    if (rw <= 0 || rh <= 0)
        return 0;
    if (!_ensure_scene_depth_target(engine, rw, rh)) {
        glBindFramebuffer(GL_FRAMEBUFFER, engine->framebuffer);
        return 0;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, engine->framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, engine->scene_depth_fbo);
    glBlitFramebuffer(0, 0, rw, rh, 0, 0, rw, rh, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, engine->framebuffer);
    return engine->scene_depth_texture;
}

// Lazy allocation for the weighted-blended OIT targets: two multisample color
// textures -- accum (RGBA16F, attachment 5) and revealage (R16F, attachment 6) --
// on a dedicated FBO that SHARES engine->depth_renderbuffer, so transparent frags
// depth-test against opaque geometry (without writing depth). Same sample count as
// the scene depth (engine->msaa_samples is the clamped effective value) or the FBO
// would be incomplete. Recreated on a size change; torn down with the MSAA
// attachments (they share the depth renderbuffer).
static bool _ensure_oit_targets(Engine* engine, int rw, int rh) {
    if (engine->oit_fbo != 0 && engine->oit_w == rw && engine->oit_h == rh)
        return true;
    if (engine->oit_fbo) {
        glDeleteTextures(1, &engine->oit_accum_multisample_texture);
        glDeleteTextures(1, &engine->oit_revealage_multisample_texture);
        glDeleteFramebuffers(1, &engine->oit_fbo);
    }
    glGenFramebuffers(1, &engine->oit_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, engine->oit_fbo);
    _add_msaa_color_attachment(&engine->oit_accum_multisample_texture, GL_RGBA16F,
                               GL_COLOR_ATTACHMENT5, rw, rh, engine->msaa_samples);
    _add_msaa_color_attachment(&engine->oit_revealage_multisample_texture, GL_R16F,
                               GL_COLOR_ATTACHMENT6, rw, rh, engine->msaa_samples);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                              engine->depth_renderbuffer);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("OIT framebuffer incomplete");
        gl_delete_texture(&engine->oit_accum_multisample_texture);
        gl_delete_texture(&engine->oit_revealage_multisample_texture);
        gl_delete_fbo(&engine->oit_fbo);
        return false;
    }
    engine->oit_w = rw;
    engine->oit_h = rh;
    return true;
}

// Begin the weighted-blended OIT accumulate sub-pass: bind the OIT FBO (accum +
// revealage sharing the scene depth), clear accum to 0 / revealage to 1, and set
// the independent per-target blend (accum sums, revealage multiplies 1-alpha).
// Returns false if the targets couldn't be allocated (caller falls back to the
// classic unsorted late pass). Leaves depth-write to the caller (off).
bool engine_begin_oit_pass(Engine* engine) {
    int rw, rh;
    engine_render_size(engine, &rw, &rh);
    if (!_ensure_oit_targets(engine, rw, rh))
        return false;
    glBindFramebuffer(GL_FRAMEBUFFER, engine->oit_fbo);
    glViewport(0, 0, rw, rh);
    // Only slots 5 (accum) and 6 (revealage) active; the shader's AccumOut /
    // RevealageOut (locations 5/6) land there, FragColor + G-buffer are discarded.
    GLenum bufs[7] = {
        GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_COLOR_ATTACHMENT5, GL_COLOR_ATTACHMENT6};
    glDrawBuffers(7, bufs);
    const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    glClearBufferfv(GL_COLOR, 5, zero); // accum: additive identity
    glClearBufferfv(GL_COLOR, 6, one);  // revealage: full reveal before the product
    glEnablei(GL_BLEND, 5);
    glEnablei(GL_BLEND, 6);
    glBlendFunci(5, GL_ONE, GL_ONE);                  // accum += premultiplied * weight
    glBlendFunci(6, GL_ZERO, GL_ONE_MINUS_SRC_COLOR); // revealage *= (1 - alpha)
    return true;
}

// End the OIT accumulate sub-pass: back to the scene FBO, color-only draw buffer,
// and the standard alpha blend for the refraction sub-pass and later draws.
void engine_end_oit_pass(Engine* engine) {
    int rw, rh;
    engine_render_size(engine, &rw, &rh);
    glBindFramebuffer(GL_FRAMEBUFFER, engine->framebuffer);
    glViewport(0, 0, rw, rh);
    engine_set_scene_draw_buffers(engine, false);
    // Undo the indexed blend we set on draw buffers 5/6. The scene MRT now
    // spans slot 7, but 5/6 stay structurally reserved for these OIT targets
    // (the attachment table skips them), so the disables cannot collide.
    glDisablei(GL_BLEND, 5);
    glDisablei(GL_BLEND, 6);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

// Lazy allocation for the moment-based OIT generation targets (spec 11.17):
// b1..b4 (attachment 5) and b0 (attachment 6) as multisample color textures on a
// dedicated FBO sharing engine->depth_renderbuffer, plus the single-sample atlas
// the two resolve into. Same lifetime and teardown as the OIT targets beside
// them.
//
// Both generation targets are RGBA32F, including the one that carries a single
// scalar: a multisample resolve blit demands identical formats, and both halves
// blit into the one atlas texture the accumulate pass can afford to sample (see
// the atlas note in engine.h). The atlas is twice as tall for the same reason.
static bool _ensure_moment_targets(Engine* engine, int rw, int rh) {
    if (engine->moment_fbo != 0 && engine->moment_w == rw && engine->moment_h == rh)
        return true;
    gl_delete_fbo(&engine->moment_fbo);
    gl_delete_texture(&engine->moment_multisample_texture);
    gl_delete_texture(&engine->moment_b0_multisample_texture);
    gl_delete_fbo(&engine->moment_atlas_fbo);
    gl_delete_texture(&engine->moment_atlas_texture);

    glGenFramebuffers(1, &engine->moment_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, engine->moment_fbo);
    _add_msaa_color_attachment(&engine->moment_multisample_texture, GL_RGBA32F,
                               GL_COLOR_ATTACHMENT5, rw, rh, engine->msaa_samples);
    _add_msaa_color_attachment(&engine->moment_b0_multisample_texture, GL_RGBA32F,
                               GL_COLOR_ATTACHMENT6, rw, rh, engine->msaa_samples);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                              engine->depth_renderbuffer);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    if (ok) {
        glGenTextures(1, &engine->moment_atlas_texture);
        glBindTexture(GL_TEXTURE_2D, engine->moment_atlas_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, rw, rh * 2, 0, GL_RGBA, GL_FLOAT, NULL);
        // Point sampling only: the accumulate pass reads its own pixel, and a
        // filtered tap would mix moments across a silhouette.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &engine->moment_atlas_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, engine->moment_atlas_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               engine->moment_atlas_texture, 0);
        ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    if (!ok) {
        log_error("OIT moment framebuffer incomplete");
        gl_delete_texture(&engine->moment_multisample_texture);
        gl_delete_texture(&engine->moment_b0_multisample_texture);
        gl_delete_fbo(&engine->moment_fbo);
        gl_delete_texture(&engine->moment_atlas_texture);
        gl_delete_fbo(&engine->moment_atlas_fbo);
        return false;
    }
    engine->moment_w = rw;
    engine->moment_h = rh;
    // Worth a line, because it is the largest single allocation the renderer
    // makes and it buys accuracy rather than resolution: two fp32 targets at the
    // scene's sample count, plus the resolved atlas. fp16 would halve it and
    // costs a third of the accuracy (spec 11.17).
    double mb = ((double)rw * rh * 16.0 * (2.0 * engine->msaa_samples + 2.0)) / (1024.0 * 1024.0);
    log_info("OIT moments: %dx%d x%d fp32 + %dx%d atlas (%.0f MB)", rw, rh, engine->msaa_samples,
             rw, rh * 2, mb);
    return true;
}

// Begin the moment generation sub-pass: bind the moment FBO, clear both targets
// to zero, and make both additive. Absorbance adds where transmittance
// multiplies, which is the whole reason the summary can be built in one unsorted
// pass -- so unlike the OIT accumulate beside it, BOTH slots sum here.
bool engine_begin_moment_pass(Engine* engine) {
    int rw, rh;
    engine_render_size(engine, &rw, &rh);
    if (!_ensure_moment_targets(engine, rw, rh))
        return false;
    glBindFramebuffer(GL_FRAMEBUFFER, engine->moment_fbo);
    glViewport(0, 0, rw, rh);
    GLenum bufs[7] = {
        GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_COLOR_ATTACHMENT5, GL_COLOR_ATTACHMENT6};
    glDrawBuffers(7, bufs);
    const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glClearBufferfv(GL_COLOR, 5, zero);
    glClearBufferfv(GL_COLOR, 6, zero);
    glEnablei(GL_BLEND, 5);
    glEnablei(GL_BLEND, 6);
    glBlendFunci(5, GL_ONE, GL_ONE);
    glBlendFunci(6, GL_ONE, GL_ONE);
    return true;
}

// End the moment generation sub-pass, resolving each multisample target into its
// own half of the atlas the accumulate reads: same size, different destination
// origin, which a multisample blit allows (it forbids scaling, not offsets).
// b1..b4 land in the lower half, b0 in the upper -- MBOIT_ATLAS_B0_TAP is the
// other end of that agreement.
void engine_end_moment_pass(Engine* engine) {
    int rw, rh;
    engine_render_size(engine, &rw, &rh);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, engine->moment_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, engine->moment_atlas_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT5);
    glBlitFramebuffer(0, 0, rw, rh, 0, 0, rw, rh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glReadBuffer(GL_COLOR_ATTACHMENT6);
    glBlitFramebuffer(0, 0, rw, rh, 0, rh, rw, rh * 2, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, engine->framebuffer);
    glViewport(0, 0, rw, rh);
    engine_set_scene_draw_buffers(engine, false);
    glDisablei(GL_BLEND, 5);
    glDisablei(GL_BLEND, 6);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

// POM (§4.11): resolve "<name>_height" sibling maps once the async texture
// loader drains (so albedo/normal paths are populated). Mirrors
// mask_array_ensure_built's defer-until-idle idiom: owns its own idle-check and
// the run-once flag, so the render loop reads as one symmetric call.
static void heights_ensure_resolved(Scene* scene, Engine* engine) {
    if (!scene || scene->heights_resolved)
        return;
    if (engine && engine->async_loader && async_loader_is_busy(engine->async_loader))
        return;
    resolve_height_maps(scene);
    scene->heights_resolved = true;
}

// True when any material in the scene carries subsurface > 0, i.e. the SSS pass
// has real work. Gates the whole SSS path (attachment 4 + aux + blur) so a
// non-skin scene stays byte-identical to master AND pays nothing (materials are
// static after load, so this is a cheap per-frame scan).
static bool scene_has_subsurface(const Scene* scene) {
    if (!scene)
        return false;
    for (size_t i = 0; i < scene->material_count; i++) {
        if (scene->materials[i] && scene->materials[i]->subsurface > 0.0f)
            return true;
    }
    return false;
}

void engine_run(Engine* engine, EngineUpdateFunc update, EngineRenderFunc render) {
    if (!engine)
        return;

    glEnable(GL_DEPTH_TEST);

    glCullFace(GL_BACK); // Cull back faces
    glFrontFace(GL_CCW); // Front faces are defined in counter-clockwise order

    engine->last_frame_time = glfwGetTime();

    while (!glfwWindowShouldClose(engine->window)) {
        // delta_time holds the honest (unclamped) per-frame dt; only the FPS
        // average uses a clamped copy (a game re-clamps to its own max_frame_time
        // for sim stability).
        double current_time = glfwGetTime();
        engine->delta_time = current_time - engine->last_frame_time;
        engine->last_frame_time = current_time;

        engine->frame_count++;
        double fps_dt = engine->delta_time > 0.1 ? 0.1 : engine->delta_time;
        engine->fps_update_timer += (float)fps_dt;

        if (engine->fps_update_timer >= 0.5f) {
            engine->fps = (float)engine->frame_count / engine->fps_update_timer;
            engine->frame_count = 0;
            engine->fps_update_timer = 0.0f;
        }

        // Adopt any pending resolution change (render scale, window size)
        // before this frame touches GL. Everything below assumes the scene
        // target and the post chain agree on a size.
        _engine_sync_render_targets(engine);
        if (!_engine_frame_renderable(engine)) {
            // Nothing to draw. There is no swap on this path, so nothing
            // throttles it either -- block on events rather than spinning a
            // core while a window sits minimized (GLFW's iconify pattern).
            // Headless has no events to wake on, so it polls and relies on the
            // frame limit to end the run.
            _engine_advance_frame(engine);
            if (engine->headless)
                glfwPollEvents();
            else
                glfwWaitEventsTimeout(0.1);
            continue;
        }

        // Begin the ImGui frame before the app's render_func, so both the app
        // (e.g. the tree app) and the engine panel can add windows between
        // NewFrame and the Render/RenderDrawData at present time. Latch the
        // decision so render_engine_gui pairs its igRender with this NewFrame
        // even if the panel flags are toggled mid-frame.
        engine->gui_frame_active = engine->show_gui || engine->show_fps || engine->show_camera_hud;
        if (engine->gui_frame_active) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            igNewFrame();
        }

        // The frame's produced dt: the wall clock live, a fixed step headless.
        // Headless time is computed from the integer frame index rather than
        // accumulated, so frame N is instant N/60 exactly, every run.
        double frame_dt = engine->headless ? ENGINE_FIXED_FRAME_DT : engine->delta_time;

        // Snapshot the clock pointer so the two latch sites see one decision:
        // an install/uninstall from inside `update` takes effect at the next
        // frame boundary instead of double-latching or skipping both sites.
        const EngineFrameClock* frame_clock = engine->render_clock;

        // With no substituted clock, latch the frame's animation clock BEFORE
        // the update callback so everything ticked this frame (particles
        // below, an app's animation stepping) reads this frame's clock, and
        // tick the scene's particle systems -- no framework owns the sim, so
        // the engine is the tick's natural home (the scene-citizen contract).
        if (!frame_clock) {
            double frame_time = engine->headless
                                    ? (double)engine->total_frames * ENGINE_FIXED_FRAME_DT
                                    : current_time;
            engine_set_render_time(engine, frame_time, frame_dt);
            Scene* tick_scene = get_current_scene(engine);
            if (tick_scene)
                scene_update_particle_systems(tick_scene, (float)engine->render_delta,
                                              (float)engine->render_time);
        }

        // Per-frame update (input, physics, fixed-timestep sim for game apps),
        // before the shadow pass so transform/particle updates land first.
        // Receives the produced dt, so a game's fixed-step accumulator takes
        // exactly one step per headless frame and its step count stops being
        // a function of wall time.
        if (update)
            update(engine, (float)frame_dt);

        // A substituted clock latches AFTER the update callback, so an
        // embedder's sim has advanced, and before the shadow pass, so the
        // depth and shading passes displace wind from the same instant.
        // Sampled here rather than written by the embedder because the sample
        // cannot be skipped by a control path -- an early return inside
        // `update` leaves a substituted clock simply not advanced, which is
        // the truth, instead of silently reverting this frame to the wall
        // clock.
        if (frame_clock) {
            engine_set_render_time(engine, frame_clock->time, frame_clock->delta);
        }

        // Wireframe mode: use albedo-only rendering for performance
        RenderMode saved_render_mode = engine->current_render_mode;
        if (engine->show_wireframe) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            engine->current_render_mode = RENDER_MODE_ALBEDO;
        } else {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }
        // The mode this frame actually renders with (wireframe substitutes
        // ALBEDO), used by the present pass after the mode is restored
        RenderMode frame_mode = engine->current_render_mode;

        Scene* shadow_scene = get_current_scene(engine);

        // GI probe captures, while the volume is dirty. Deliberately BEFORE the
        // shadow pass: a capture needs the camera-independent single-cascade map
        // and bakes its own, and the pass below then restores the camera-fit
        // cascades by simply overwriting them. No-op on a converged volume.
        if (shadow_scene && shadow_scene->gi_volume) {
            gi_volume_update(shadow_scene->gi_volume, engine, shadow_scene);
        }

        // Shadow depth pass (before main render)
        if (shadow_scene && shadow_scene->shadow_system && shadow_scene->shadow_system->enabled) {
            render_shadow_depth_pass(engine, shadow_scene);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, engine->framebuffer);
        // The scene target is supersampled; the present pass left the viewport
        // at display size, so set it to the full render size here.
        int rw, rh;
        engine_render_size(engine, &rw, &rh);
        glViewport(0, 0, rw, rh);
        engine->normals_this_frame =
            frame_mode == RENDER_MODE_PBR && postfx_wants_normals(engine->postfx);
        // Make the material registry describe what the graph actually draws
        // before anything reads it. An app that builds materials in code never
        // called add_material_to_scene, so its registry was empty and every
        // consumer of it -- the subsurface gate below included -- silently saw a
        // scene with no materials at all.
        scene_sync_materials(shadow_scene);
        // SSS writes attachment 4 (skin diffuse) only when it has real work: the
        // feature is on AND the scene carries a subsurface material. This keeps a
        // non-skin scene byte-identical to master (the blur/composite would add 0)
        // AND free of the pass's per-frame cost. Mirrors the albedo-on-SSGI gate.
        engine->sss_this_frame = frame_mode == RENDER_MODE_PBR && engine->sss_enabled &&
                                 scene_has_subsurface(shadow_scene);
        // SSS reads aux .z for the depth-aware profile, so it must force the aux
        // G-buffer (like motion blur), else --no-ssao + SSS reads stale/undefined Z.
        engine->aux_this_frame =
            frame_mode == RENDER_MODE_PBR &&
            (postfx_wants_aux_gbuffer(engine->postfx) || engine->sss_this_frame);
        engine->albedo_this_frame =
            frame_mode == RENDER_MODE_PBR && postfx_wants_albedo(engine->postfx);
        // Split spec-occ: ambient specular routes to attachment 7 so the post
        // chain can occlude exactly that share and fold it back. Not gated on
        // AO -- with AO off the composite still owes the scene its specular.
        engine->spec_this_frame =
            frame_mode == RENDER_MODE_PBR && postfx_wants_spec_split(engine->postfx);
        engine_set_scene_draw_buffers(engine, true);
        GBufferAttachment gb[GBUFFER_ATTACHMENT_COUNT];
        _gbuffer_attachments(engine, gb);
        glClearColor(gb[0].clear[0], gb[0].clear[1], gb[0].clear[2], gb[0].clear[3]);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        // Each written G-buffer target carries 0 on uncovered/sky/no-signal
        // texels so its consumer early-outs there (GTAO on linear-Z, SSGI on
        // albedo, SSS on skin diffuse). glClear painted them the scene
        // background above, so re-clear the written ones to 0; the skybox and
        // shadow catcher draw color-only and leave them cleared.
        for (int i = 1; i < GBUFFER_ATTACHMENT_COUNT; i++) {
            if (*gb[i].this_frame)
                glClearBufferfv(GL_COLOR, (GLint)(gb[i].attachment - GL_COLOR_ATTACHMENT0),
                                gb[i].clear);
        }

        Scene* current_scene = get_current_scene(engine);

        // Process pending async texture uploads (max 5 per frame to avoid stutter)
        if (current_scene && current_scene->tex_pool && engine->async_loader) {
            async_loader_process_pending(engine->async_loader, current_scene->tex_pool, 5);
        }

        // (Re)build the material mask array once its source masks have loaded
        // (a no-op until then; masks fall back to their scalar factors).
        mask_array_ensure_built(current_scene, engine);

        // POM (§4.11): resolve height maps once the async texture loader drains.
        heights_ensure_resolved(current_scene, engine);

        if (render != NULL && current_scene != NULL) {
            render(engine, current_scene);
        }

        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        engine->current_render_mode = saved_render_mode;

        engine_present_frame(engine, frame_mode);

        // Engine-owned frame limit (CI/headless): requests close so the
        // final-frame screenshot below fires this same iteration.
        _engine_advance_frame(engine);

        // Periodic capture: numbered frames every N frames
        if (engine->screenshot_path && engine->screenshot_every > 0 &&
            engine->total_frames % (size_t)engine->screenshot_every == 0) {
            char numbered[1024];
            _numbered_screenshot_path(engine->screenshot_path, engine->total_frames, numbered,
                                      sizeof(numbered));
            _save_framebuffer_ppm(engine, numbered);
        }

        // Capture the final frame before exit if a screenshot was requested
        if (engine->screenshot_path && glfwWindowShouldClose(engine->window)) {
            _save_framebuffer_ppm(engine, engine->screenshot_path);
        }

        glfwSwapBuffers(engine->window);
        glfwPollEvents();
    }
}

void get_mouse_world_position_on_drag_plane(Engine* engine, double mouse_fb_x, double mouse_fb_y,
                                            vec3 out_world_pos) {
    if (!engine || !engine->camera)
        return;

    // Build projection and view matrices
    mat4 projection = {0}, view;
    compute_projection_matrix(engine->camera, projection);
    glm_lookat(engine->camera->position, engine->camera->look_at, engine->camera->up_vector, view);

    // Compute ray from screen coordinates. Under ortho the ray starts at the pixel rather
    // than at the camera, so the origin has to travel with the direction.
    vec3 ray_origin = {0}, ray_dir = {0};
    if (engine->camera->is_orthographic) {
        compute_ortho_ray_from_screen((float)mouse_fb_x, (float)mouse_fb_y, engine->fb_width,
                                      engine->fb_height, projection, view, ray_origin, ray_dir);
    } else {
        glm_vec3_copy(engine->camera->position, ray_origin);
        compute_ray_from_screen((float)mouse_fb_x, (float)mouse_fb_y, engine->fb_width,
                                engine->fb_height, projection, view, ray_origin, ray_dir);
    }

    // Project ray to the stored drag plane distance
    ray_point_at_distance(ray_origin, ray_dir, engine->input.drag_plane_distance, out_world_pos);
}

static SceneNode* _perform_engine_ray_picking(Engine* engine, double mouse_fb_x,
                                              double mouse_fb_y) {
    // Need camera for ray picking
    if (!engine->camera)
        return NULL;

    // Build projection and view matrices
    mat4 projection = {0}, view;
    compute_projection_matrix(engine->camera, projection);
    glm_lookat(engine->camera->position, engine->camera->look_at, engine->camera->up_vector, view);

    // Compute ray from screen coordinates
    vec3 ray_origin = {0}, ray_dir = {0};
    if (engine->camera->is_orthographic) {
        compute_ortho_ray_from_screen((float)mouse_fb_x, (float)mouse_fb_y, engine->fb_width,
                                      engine->fb_height, projection, view, ray_origin, ray_dir);
    } else {
        glm_vec3_copy(engine->camera->position, ray_origin);
        compute_ray_from_screen((float)mouse_fb_x, (float)mouse_fb_y, engine->fb_width,
                                engine->fb_height, projection, view, ray_origin, ray_dir);
    }

    // Perform scene graph picking
    Scene* current_scene = get_current_scene(engine);
    if (!current_scene || !current_scene->root_node)
        return NULL;

    RayPickResult result = pick_scene_node(current_scene->root_node, ray_origin, ray_dir);

    if (result.hit) {
        // Store hit information for drag operations
        glm_vec3_copy(result.hit_point, engine->input.drag_start_world_pos);
        engine->input.drag_plane_distance = result.distance;

        // Store object's current position (extract from transform)
        glm_vec3_copy((vec3){result.node->global_transform[3][0],
                             result.node->global_transform[3][1],
                             result.node->global_transform[3][2]},
                      engine->input.drag_object_start_pos);
    }

    return result.node;
}
