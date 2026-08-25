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
#include "gui.h"
#include "light_cluster.h"
#include "transform.h"
#include "intersect.h"
#include "shadow.h"
#include "sky.h"
#include "water.h"
#include "gi_volume.h"
#include "probe_atlas.h"
#include "layers_vt.h"
#include "material_texture_array.h"
#include "texture.h"
#include "import.h" // resolve_height_maps (POM height convention)
#include "render.h"
#include "springbone.h"
#include "wind.h"
#include "profiler.h"

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
    engine->layers_vt_enabled = true; // composite cache on; --no-layers-vt is the bisect lever
    engine->layers_vt_res = 0;        // derived from the splat domain unless overridden
    engine->layers_vt_pages_enabled = true; // pages on; --no-layers-vt-pages is stage 1 exactly
    engine->layers_vt_feedback_enabled = true; // the vote pass; off = prediction alone
    engine->layers_vt_page_slots = 0;       // 0 = the full physical atlas
    engine->layers_vt_page_budget = 0;      // 0 = the default bakes-per-frame
    engine->layers_vt_probe_interval = 0;   // diagnostic; off unless a probe asks

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
    // On by default: the batched and unbatched paths carry the same floats into
    // the same shader arithmetic, so there is nothing to opt into.
    // --no-instancing is the escape hatch.
    engine->instancing_enabled = true;
    engine->frustum_cull_enabled = true;
    engine->morph_enabled = true;
    engine->oit_moments_enabled = true;
    engine->emissive_lights_enabled = false; // see engine.h: emissive is mostly not a lamp
    engine->lod_enabled = true;
    engine->lod_bias = 1.0f;
    // On by default (spec 11.30): -29% of the opaque pass on apps/forest, for a
    // per-pass copy and a qsort of one lane. --no-sort-opaque is the escape.
    //
    // Unlike --no-instancing this one is NOT a 0 px flag, and the reason is a
    // defect it did not cause: masked materials still blend into attachment 0
    // (engine.c:440), so reordering them changes the picture. What it changes is
    // the loss of shading that hidden leaves were contributing -- see 11.30.
    engine->opaque_sort_enabled = true;

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
    engine->depth_prepass_program = NULL;
    // Off while it is measured (spec 11.30); --depth-prepass turns it on.
    engine->depth_prepass_enabled = false;
    engine->catcher_vao = 0;
    engine->catcher_vbo = 0;

    engine->postfx = NULL;
    engine->profiler = NULL;
    engine->profiler_enabled = false;

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

    // Borrows nothing: it holds copies of DrawItems, and a DrawItem borrows its
    // mesh and node. So this is safe before or after the scenes go.
    draw_list_free(&engine->sorted_opaque);

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
    free_ubo(engine->instance_ubo);
    free_ubo(engine->vt_pages_ubo);
    free_ubo(engine->roads_ubo);
    free_layers_vt_feedback(engine->vt_feedback);

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

    // Its query objects belong to the context, so this has to happen above
    // glfwDestroyWindow like every other GL teardown in this function.
    free_profiler(engine->profiler);
    engine->profiler = NULL;

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
    engine->max_texture_size = get_gl_max_texture_size();
    log_info("GL sampler budget: %d fragment texture image units, %d array layers, "
             "max texture size %d",
             engine->max_texture_image_units, engine->max_array_texture_layers,
             engine->max_texture_size);
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

// Create one scene color attachment (texture + framebuffer binding) on the
// currently-bound framebuffer -- multisample above one sample, a plain
// GL_TEXTURE_2D at one. Each G-buffer target is written only when a consumer is
// active (see engine_set_scene_draw_buffers); the texture is always allocated so
// the FBO layout is stable across frames.
//
// The branch is HERE, in the one allocator, rather than at its four call sites,
// and that placement is load-bearing: the scene, OIT and moment FBOs all share
// depth_renderbuffer, and GL requires every attachment of an FBO to carry the
// same sample count. One branch on the one field every caller passes flips all
// three FBOs in lockstep by construction; four caller-side branches could
// disagree and the failure would be a quietly-incomplete OIT FBO falling back
// to the unsorted late pass.
//
// It used to be multisample even at samples == 1, which was a live cost: this
// driver refuses a 1-sample multisample target and returns a 2-sample one (see
// msaa_samples_actual), so the TAA path -- which requests one sample precisely
// to avoid MSAA -- silently rasterized two samples per pixel (spec 11.34).
//
// The single-sample texture sets sampler state where the multisample one cannot
// carry any: nothing samples these targets today (every consumer is a blit),
// but a plain texture left at the GL_NEAREST_MIPMAP_LINEAR default would be
// mip-incomplete the day something does, and that failure is black, not loud.
static void _add_scene_color_attachment(GLuint* out_tex, GLenum internal_format, GLenum attachment,
                                        int rw, int rh, int samples) {
    glGenTextures(1, out_tex);
    if (samples > 1) {
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, *out_tex);
        glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, internal_format, rw, rh,
                                GL_TRUE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D_MULTISAMPLE, *out_tex,
                               0);
        return;
    }
    glBindTexture(GL_TEXTURE_2D, *out_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal_format, rw, rh, 0,
                 gl_transfer_format(internal_format), GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, *out_tex, 0);
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
        _add_scene_color_attachment(gb[i].tex, gb[i].internal_format, gb[i].attachment, rw,
                                    rh, samples);
    }

    glGenRenderbuffers(1, &engine->depth_renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, engine->depth_renderbuffer);
    // Same branch as the colour allocator, for the same shared-depth reason: a
    // plain renderbuffer at one sample, multisample above. It stays a
    // renderbuffer either way -- it was never sampleable, which is why
    // engine_resolve_scene_depth exists.
    if (samples > 1)
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, rw, rh);
    else
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, rw, rh);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                              engine->depth_renderbuffer);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Error: MSAA Framebuffer is not complete!");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return -1;
    }

    // What the driver ACTUALLY gave us, which above one sample is not always
    // what was asked for. Read back while the target is still bound, once per
    // build, so no consumer has to repeat the query or -- worse -- trust the
    // request. The readback earned its keep before 11.34: back when a 1-sample
    // request was still allocated multisample, this driver rounded it up to 2,
    // and trusting the request had every depth-complexity figure on the TAA
    // path reading exactly twice the truth (spec 11.31). At one sample the
    // plain-texture path now makes request and answer agree by construction.
    GLint got = 0;
    glGetIntegerv(GL_SAMPLES, &got);
    engine->msaa_samples_actual = got < 1 ? 1 : got;
    if (engine->msaa_samples_actual != samples)
        log_info("Scene target: asked for %dx MSAA, driver gave %dx", samples,
                 engine->msaa_samples_actual);

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
    gui_apply_style();

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
    engine->instance_ubo = create_ubo(UBO_INSTANCES_BLOCK_SIZE, UBO_BINDING_INSTANCES);
    // Zero-filled at create, so a shader reading the page table before the
    // first residency upload sees a 0x0 grid rather than garbage.
    engine->vt_pages_ubo = create_ubo(UBO_VT_PAGES_BLOCK_SIZE, UBO_BINDING_VT_PAGES);
    // Same zero-filled contract: a shader reading roads before the first upload
    // sees a road count of 0, which is the off state.
    engine->roads_ubo = create_ubo(UBO_ROADS_BLOCK_SIZE, UBO_BINDING_ROADS);
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

    // Only when asked for (spec 11.27). Not fatal if it fails: losing the
    // instrument should not cost the frame it was meant to measure.
    if (engine->profiler_enabled) {
        engine->profiler = create_profiler();
    }

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
    postfx_set_profiler(engine->postfx, engine->profiler);

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

bool engine_gui_wants_keyboard(void) {
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

    // Not fatal if it fails: --msm falls back to the depth cascades, which are
    // rendered either way.
    ShaderProgram* msm_resolve_program = create_msm_resolve_program();
    if (msm_resolve_program) {
        add_shader_program_to_engine(engine, msm_resolve_program);
    }

    // Same contract as the moment resolve above: if either fails to compile,
    // shadow_build_tsm finds no program and leaves tsm_built false, which is
    // the feature off rather than a broken frame.
    ShaderProgram* shadow_absorb_program = create_shadow_absorb_program();
    if (shadow_absorb_program) {
        add_shader_program_to_engine(engine, shadow_absorb_program);
    }
    ShaderProgram* tsm_resolve_program = create_tsm_resolve_program();
    if (tsm_resolve_program) {
        add_shader_program_to_engine(engine, tsm_resolve_program);
    }

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

    ShaderProgram* cloud_shadow_program = create_cloud_shadow_program();
    if (cloud_shadow_program) {
        add_shader_program_to_engine(engine, cloud_shadow_program);
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

    ShaderProgram* layers_vt_bake_program = create_layers_vt_bake_program();
    if (layers_vt_bake_program) {
        add_shader_program_to_engine(engine, layers_vt_bake_program);
    }

    ShaderProgram* layers_vt_feedback_program = create_layers_vt_feedback_program();
    if (layers_vt_feedback_program) {
        add_shader_program_to_engine(engine, layers_vt_feedback_program);
    }

    ShaderProgram* water_program = create_water_program();
    if (water_program) {
        add_shader_program_to_engine(engine, water_program);
    }

    ShaderProgram* water_spectrum_program = create_water_spectrum_program();
    if (water_spectrum_program) {
        add_shader_program_to_engine(engine, water_spectrum_program);
    }

    ShaderProgram* water_fft_program = create_water_fft_program();
    if (water_fft_program) {
        add_shader_program_to_engine(engine, water_fft_program);
    }

    ShaderProgram* water_foam_program = create_water_foam_program();
    if (water_foam_program) {
        add_shader_program_to_engine(engine, water_foam_program);
    }

    // GI probe volume. Lives on the engine rather than on PostFX -- despite
    // being a fullscreen pass -- because the volume is a Scene citizen and the
    // atlas is consumed by the scene pass, not by post.
    ShaderProgram* gi_project_program = create_gi_project_program();
    if (gi_project_program) {
        add_shader_program_to_engine(engine, gi_project_program);
    }

    // Specular probe projection (spec 11.70), here for the same reason: the
    // atlas it writes is read by the scene pass.
    ShaderProgram* probe_project_program = create_probe_project_program();
    if (probe_project_program) {
        add_shader_program_to_engine(engine, probe_project_program);
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

void set_engine_profiler(Engine* engine, bool enabled) {
    if (!engine)
        return;
    engine->profiler_enabled = enabled;
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
    // Hand the current scene's reflection probes and shadow casters to postfx
    // (SSR miss fallback / fog march) without postfx learning about Scene
    const Scene* fx_scene = get_current_scene(engine);
    probe_set_publish_to_postfx(fx_scene ? fx_scene->probe_set : NULL, engine->postfx);
    shadow_publish_to_postfx(fx_scene, engine->postfx);
    // Aerial perspective is a camera-frustum volume, so unlike the sky's other
    // LUTs it is rebuilt here every frame, immediately before it is published.
    // The unjittered projection: the bake reads only [0][0]/[1][1]/[2][2]/[3][2]
    // so TAA's jitter in [2][0]/[2][1] cannot reach it, and handing it an input
    // that churns every frame would defeat any future rebuild-elision.
    if (fx_scene && fx_scene->sky)
        sky_update_aerial(fx_scene->sky, engine->view_matrix, engine->projection_matrix);
    sky_publish_to_postfx(fx_scene ? fx_scene->sky : NULL, engine->postfx);
    // Order-independent of the sky's: water publishes a suppression REQUEST rather than
    // clearing the aerial volume the line above filled, so neither publish can undo the
    // other. Gated on the surface actually drawing -- a debug mode or a capture that
    // skips the water pass must not leave a medium behind for the volume to integrate.
    water_publish_to_postfx(
        fx_scene && water_will_draw(fx_scene->water, engine, frame_mode) ? fx_scene->water : NULL,
        engine);
    // Unconditional, including with no scene: count 0 is the off state, and only a
    // publish every frame can reach it after a scene that had volumes goes away.
    scene_publish_fog_volumes_to_postfx(fx_scene, engine->postfx);
    const PostFXGBufferWrites writes = {.normals = engine->normals_this_frame,
                                        .aux = engine->aux_this_frame,
                                        .albedo = engine->albedo_this_frame,
                                        .sss = engine->sss_this_frame,
                                        .spec = engine->spec_this_frame,
                                        .oit_fbo =
                                            engine->oit_this_frame ? engine->oit_fbo : 0,
                                        .oit_moment_atlas = engine->moments_this_frame
                                                                ? engine->moment_atlas_texture
                                                                : 0,
                                        .oit_near_far = {
                                            engine->camera ? engine->camera->near_clip : 1.0f,
                                            engine->camera ? engine->camera->far_clip : 2.0f}};
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
    if (fx_scene && fx_scene->probe_set && fx_scene->probe_set->debug_atlas) {
        probe_atlas_debug_blit(fx_scene->probe_set->atlas, engine, engine->fb_width,
                               engine->fb_height);
    }

    // GUI last, after tone mapping. gui_render_frame self-gates on
    // gui_frame_active, so it no-ops when no panel/overlay is enabled.
    profiler_scope_begin_if(engine->profiler, engine->gui_frame_active, "gui");
    gui_render_frame(engine);
    profiler_scope_end(engine->profiler);
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
    _add_scene_color_attachment(&engine->oit_accum_multisample_texture, GL_RGBA16F,
                                GL_COLOR_ATTACHMENT5, rw, rh, engine->msaa_samples);
    _add_scene_color_attachment(&engine->oit_revealage_multisample_texture, GL_R16F,
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
    _add_scene_color_attachment(&engine->moment_multisample_texture, GL_RGBA32F,
                                GL_COLOR_ATTACHMENT5, rw, rh, engine->msaa_samples);
    // b0 carries one scalar, and above one sample it is padded to RGBA32F
    // anyway: a MULTISAMPLE resolve blit demands identical formats on both
    // sides, and it shares the atlas with the four-channel b1..b4. A
    // single-sample blit only needs compatibility, so at one sample the padding
    // goes -- a quarter of the b0 target, on the path that ships.
    _add_scene_color_attachment(&engine->moment_b0_multisample_texture,
                                engine->msaa_samples > 1 ? GL_RGBA32F : GL_R32F,
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
    // The count the targets were actually built at, not the one requested.
    // Above one sample the driver may adjust the count; and back when 1-sample
    // requests were still allocated multisample (pre-11.34), a VRAM figure that
    // believed the request understated the renderer's largest allocation on the
    // shipping path.
    int samples = engine->msaa_samples_actual;
    // Per scene pixel: b1..b4 at 16 bytes a sample, b0 at 16 above one sample
    // and 4 at one (R32F -- see the allocation), and the two-high RGBA32F atlas.
    double b0_bytes = samples > 1 ? 16.0 * samples : 4.0;
    double mb = ((double)rw * rh * (16.0 * samples + b0_bytes + 32.0)) / (1024.0 * 1024.0);
    log_info("OIT moments: %dx%d x%d fp32 + %dx%d atlas (%.0f MB)", rw, rh, samples, rw, rh * 2,
             mb);
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
// material_texture_array_ensure_built's defer-until-idle idiom: owns its own idle-check and
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

/*
 * The lattice an origin shift snaps to, derived from the drift a scene tolerates.
 *
 * SNAPPED rather than taken from the camera exactly, and the reason is arithmetic.
 * An arbitrary camera position makes the delta an arbitrary float, so every
 * position in the world takes a fresh rounding at every shift and the error
 * accumulates over a long traversal. A power-of-two lattice point is exactly
 * representable, and within a factor of two of the coordinates being moved the
 * subtraction is exact (Sterbenz) -- which covers everything near the camera,
 * where every bit matters.
 *
 * Derived from the threshold rather than fixed, so tightening one tightens the
 * other; a lattice coarser than the threshold would let the snap land back where
 * it started and never shift at all. Half the threshold also bounds the residual
 * at sqrt(3)/2 lattice < threshold, which is what stops a shift from immediately
 * qualifying for another one.
 *
 * Returns 0 for a threshold that cannot produce one, including the subnormal and
 * NaN inputs that would make log2f return -inf or NaN.
 */
static float engine_origin_lattice(float threshold) {
    if (!(threshold > 0.0f) || !isfinite(threshold))
        return 0.0f;
    float lattice = ldexpf(1.0f, (int)floorf(log2f(threshold * 0.5f)));
    return isfinite(lattice) && lattice > 0.0f ? lattice : 0.0f;
}

void engine_upload_displacement_uniforms(const Engine* engine, const Scene* scene,
                                         UniformManager* u) {
    // The CLOCK is a displacement input too, and it carries the same five-program
    // hazard the rest of this function exists for: a program that misses it
    // evaluates the wind at time 0 and puts its vertices somewhere else. It was
    // set by hand at four call sites while this function's header already claimed
    // to be the one place.
    //
    // uDeltaTime is the advance of the SAME clock, because the shader evaluates
    // the previous-frame position at time - uDeltaTime. Not the wall-clock delta:
    // under a game the sim advances in whole fixed steps and stops when paused, so
    // a wall-clock delta reports wind motion on geometry that never moved. The
    // depth-only programs declare neither; uniform_set_* caches the negative
    // lookup, so naming them there costs one hash miss each.
    uniform_set_float(u, "time", engine ? (float)engine->render_time : 0.0f);
    uniform_set_float(u, "uDeltaTime", engine ? (float)engine->render_delta : 0.0f);

    wind_upload_to_program(scene ? scene->wind : NULL,
                           scene ? (const float*)scene->world_origin : NULL, u);

    // The morph's two camera positions. With no camera they are both the origin,
    // which is a legal answer rather than a fallback: every mesh that carries no
    // morph attribute is an exact identity whatever these say, and one that does
    // is not being drawn by anything without a camera.
    const float* eye = engine && engine->camera ? engine->camera->position : GLM_VEC3_ZERO;
    // Falls back to THIS frame's eye rather than to the origin, which is what
    // "no previous frame" actually means: a camera that has not moved yet.
    const float* prev =
        engine && engine->prev_camera_valid ? (const float*)engine->prev_camera_position : eye;
    uniform_set_vec3(u, "uMorphEye", eye);
    uniform_set_vec3(u, "uMorphEyePrev", prev);
    // The diagnostic off switch, and it rides this call rather than the quadtree
    // because it has to reach all five programs to be an off switch at all --
    // suppressing the morph in the shading pass alone is the exact prepass
    // disagreement this function exists to prevent, wearing a flag's name.
    uniform_set_float(u, "uMorphOff", engine && !engine->morph_enabled ? 1.0f : 0.0f);
}

void engine_recentre_on_camera(const Engine* engine, float lattice) {
    if (!engine || !engine->camera)
        return;
    Scene* scene = get_current_scene(engine);
    if (!scene || !(lattice > 0.0f))
        return;
    // The camera is in STORAGE space and the origin is authored, so the current
    // origin has to be added back -- without it a second shift moves the world by
    // snap(camera) - world_origin and drags it toward where it started.
    //
    // Y is deliberately left alone. Nothing in this engine is unbounded
    // vertically, and several things are pinned to a world height that a vertical
    // shift would leave behind: the water plane, the shadow catcher, the sky's
    // ground projection, and any terrain whose centre is an XZ pair.
    vec3 target;
    glm_vec3_copy(scene->world_origin, target);
    target[0] += floorf(engine->camera->position[0] / lattice + 0.5f) * lattice;
    target[2] += floorf(engine->camera->position[2] / lattice + 0.5f) * lattice;
    scene_set_world_origin(scene, target);
}

// Automatic re-centring (spec 11.62 phase 5): once the camera has drifted further
// than the scene tolerates, re-centre on it.
static void engine_schedule_origin_shift(const Engine* engine, const Scene* scene) {
    if (!scene || scene->origin_shift_distance <= 0.0f || !engine->camera)
        return;
    if (glm_vec3_distance(engine->camera->position, GLM_VEC3_ZERO) <=
        scene->origin_shift_distance)
        return;
    engine_recentre_on_camera(engine, engine_origin_lattice(scene->origin_shift_distance));
}

/*
 * Apply a scheduled origin shift, the scene's half and the engine's together.
 *
 * The engine half is only what no owner republishes: the live camera, the
 * previous view-projection, and the froxel volume's stored camera. Anything that
 * IS republished every frame -- the fog volumes, the water plane, the reflection
 * probe's parallax proxy -- must be shifted at its owner instead, because a
 * correction applied to PostFX's mirror is overwritten before it is read.
 *
 * The two matrices are MATRICES and the correction is not a subtraction. We need
 * M_new * p_new == M_old * p_old with p_old = p_new + delta, so the fix is a
 * right-multiply by a translation of +delta. Subtracting delta from the matrix's
 * own translation column is a different operation and gives a different answer
 * under any rotation -- which is to say, always. Getting it wrong is one frame of
 * screen-wide velocity: every pixel reports the whole shift as motion, TAA
 * reprojects to nothing, and motion blur smears it.
 */
static void engine_apply_origin_shift(Engine* engine, Scene* scene) {
    if (!scene)
        return;
    engine_schedule_origin_shift(engine, scene);

    vec3 delta;
    glm_vec3_sub(scene->pending_origin, scene->world_origin, delta);
    if (glm_vec3_eq(delta, 0.0f))
        return;

    scene_apply_origin_delta(scene, delta);

    if (engine->camera) {
        glm_vec3_sub(engine->camera->position, delta, engine->camera->position);
        glm_vec3_sub(engine->camera->look_at, delta, engine->camera->look_at);
    }

    // Rule 1 applies to the camera's own past as much as to the world's: the
    // morph reads the difference between this eye and the previous one, so
    // shifting one and not the other reports the whole shift as a camera move
    // and re-morphs the terrain in a single frame.
    glm_vec3_sub(engine->prev_camera_position, delta, engine->prev_camera_position);

    glm_translate(engine->prev_view_proj, delta);
    if (engine->postfx)
        glm_translate(engine->postfx->froxel_prev_view, delta);

    // The captures are NOT re-armed. A rigid translation moves every probe by the
    // same delta as everything it sees, so the irradiance and radiance they hold
    // are still what a probe at that point would record -- only the address
    // changed, and their owners moved it above.

    // Last, so an app's callback sees a world that has fully moved: physics
    // bodies and cached positions are usually reconciled against the graph, and
    // reconciling against a half-shifted one is worse than not reconciling.
    if (scene->on_origin_shift)
        scene->on_origin_shift(delta, scene->origin_shift_ctx);

    // Logged because a shift is otherwise unobservable from outside the process,
    // and an arm asserting that one changed nothing is satisfied perfectly by a
    // shift that never happened.
    log_info("origin shift: delta (%.1f, %.1f, %.1f), world origin now (%.1f, %.1f, %.1f)",
             (double)delta[0], (double)delta[1], (double)delta[2],
             (double)scene->world_origin[0], (double)scene->world_origin[1],
             (double)scene->world_origin[2]);
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

        // Retires the ring slot this frame is about to reuse, before any scope
        // opens.
        profiler_begin_frame(engine->profiler);

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
            // Closes the frame the begin above opened. Skipping it would stall
            // the ring index and the latch for as long as the window stays
            // minimized, so the table would freeze rather than empty.
            profiler_end_frame(engine->profiler, engine->delta_time);
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
        // decision so gui_render_frame pairs its igRender with this NewFrame
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

        // The water's own sim, on the same principle as the particle tick above and for a
        // sharper reason: the swash film's tips are read by the opaque pass and again by the
        // late pass, so it has to advance before EITHER of them. Outside the clock branch,
        // because a substituted clock still leaves render_time and render_delta valid, and a
        // film that stops stepping under one is the frozen-swash case this placement fixes.
        Scene* water_scene = get_current_scene(engine);
        if (water_scene)
            water_update(water_scene->water, water_scene, (float)engine->render_time,
                         (float)engine->render_delta);

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

        // Origin shift (spec 11.62), after the update callback so it sees the
        // camera and physics this frame produced, and before the GI capture and
        // the shadow pass, which are the first things to read world positions.
        // Anywhere later and the first shifted frame fits its shadows and its
        // probes around an origin the rest of the frame no longer uses.
        engine_apply_origin_shift(engine, get_current_scene(engine));

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

        // The day/night cycle's tick (spec 11.81), BEFORE the GI sweep and
        // the shadow pass: the key light it rewrites is what the cascades
        // then fit, and a completed slice's swap must land before this
        // frame's first bind_ibl_textures. A structural no-op when the cycle
        // is off.
        if (shadow_scene && shadow_scene->sky && shadow_scene->ibl) {
            profiler_scope_begin_if(engine->profiler,
                                    shadow_scene->sky->slicer.item >= 0 ||
                                        shadow_scene->sky->cycle_dirty,
                                    "sky cycle");
            bool env_swapped = sky_cycle_tick(shadow_scene->sky, shadow_scene->ibl,
                                              (float)engine->render_delta);
            profiler_scope_end(engine->profiler);
            // The scene owns what re-derives from its environment; the sky
            // only reports that its chain moved.
            if (env_swapped && shadow_scene->gi_volume)
                gi_volume_mark_dirty(shadow_scene->gi_volume);
        }

        // GI probe captures, while the volume is dirty. Deliberately BEFORE the
        // shadow pass: a capture needs the camera-independent single-cascade map
        // and bakes its own, and the pass below then restores the camera-fit
        // cascades by simply overwriting them. No-op on a converged volume.
        if (shadow_scene && shadow_scene->gi_volume) {
            // Timed only while probes remain to bake. A converged volume is the
            // steady state, so an unconditional scope would file a 0.000 ms row
            // on nearly every frame of a run.
            profiler_scope_begin_if(engine->profiler,
                                        shadow_scene->gi_volume->dirty_count > 0, "gi capture");
            gi_volume_update(shadow_scene->gi_volume, engine, shadow_scene);
            profiler_scope_end(engine->profiler);
        }

        // Shadow depth pass (before main render)
        if (shadow_scene && shadow_scene->shadow_system && shadow_scene->shadow_system->enabled) {
            // Unscoped here on purpose: render_shadow_depth_pass opens its own
            // cascade/punctual/msm/tsm scopes, and an outer one would swallow
            // all four -- flat scopes mean the coarser wrapper simply wins.
            render_shadow_depth_pass(engine, shadow_scene);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, engine->framebuffer);
        // The scene target is supersampled; the present pass left the viewport
        // at display size, so set it to the full render size here.
        int rw, rh;
        engine_render_size(engine, &rw, &rh);
        glViewport(0, 0, rw, rh);
        // Published from here rather than computed inside the profiler, because
        // this is where the render size and the sample count are both settled --
        // supersample, render scale and any mid-run resolution change have all
        // been applied by now.
        //
        // msaa_samples_actual, never msaa_samples: above one sample the driver
        // may adjust the count, and GL_SAMPLES_PASSED counts what the target
        // HAS. The rule was written in scar tissue -- back when a 1-sample
        // request was allocated multisample, this driver returned 2 and every
        // depth-complexity reading on the TAA path came out at exactly twice
        // the truth until this read the count back (spec 11.31; the request is
        // honoured at 1 since 11.34).
        profiler_set_sample_budget(engine->profiler, (size_t)rw * (size_t)rh *
                                                         (size_t)engine->msaa_samples_actual);
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
        profiler_scope_begin(engine->profiler, "scene clear");
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
        profiler_scope_end(engine->profiler);

        Scene* current_scene = get_current_scene(engine);

        // Process pending async texture uploads (max 5 per frame to avoid stutter)
        if (current_scene && current_scene->tex_pool && engine->async_loader) {
            async_loader_process_pending(engine->async_loader, current_scene->tex_pool, 5);
        }

        // (Re)build the material texture array once its sources have loaded
        // (a no-op until then; masks fall back to their scalar factors).
        material_texture_array_ensure_built(current_scene, engine);

        // Roads BEFORE the composite cache, and the order is load-bearing: the
        // bake samples the road block, so an edited road must reach the GPU
        // before the key change it causes triggers the re-bake that reads it.
        material_roads_ensure(current_scene, engine);

        // The composite cache samples the array, so it is strictly after: a
        // no-op while the array is dirty, and one frame behind it at worst.
        material_layers_vt_ensure(current_scene, engine);

        // POM (§4.11): resolve height maps once the async texture loader drains.
        heights_ensure_resolved(current_scene, engine);

        if (render != NULL && current_scene != NULL) {
            render(engine, current_scene);
        }

        // The feedback vote pass (spec 11.67), after the scene so the draw
        // list is this frame's; its readback retires at fixed latency into the
        // NEXT frames' residency, which is what keeps the loop deterministic.
        if (current_scene)
            layers_vt_feedback_pass(engine, current_scene);

        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        engine->current_render_mode = saved_render_mode;

        engine_present_frame(engine, frame_mode);
        profiler_end_frame(engine->profiler, engine->delta_time);

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
