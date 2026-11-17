#include <stdio.h>
#include <stdatomic.h>
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
#include "camera_rig.h"
#include "frame_script.h"
#include "util.h"
#include "ext/cwalk.h" // cwk_path_set_style: pin UNIX separators (see _engine_init)
#include "engine.h"
#include "engine_internal.h"
#include "draw_list.h"
#include "gui.h"
#include "light_cluster.h"
#include "ubo.h"
#include "occlusion.h"
#include "transform.h"
#include "intersect.h"
#include "shadow.h"
#include "sky.h"
#include "water.h"
#include "fire.h"
#include "fire_render.h"
#include "rain.h"
#include "rain_render.h"
#include "gi_volume.h"
#include "probe_set.h"
#include "background.h"
#include "lighting_atlas.h"
#include "layers_vt.h"
#include "material_texture_array.h"
#include "texture.h"
#include "import.h" // resolve_height_maps (POM height convention)
#include "async_loader.h"
#include "loading_screen.h"
#include "text.h"
#include "ltc.h"
#include "render.h"
#include "shader_hook.h"
#include "springbone.h"
#include "wind.h"
#include "profiler.h"
#include "postfx_programs.h"
#include "scene_view_state.h"
#include "crt.h"
#include "uniform.h"

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
static Engine* _engine_alloc(const EngineConfig* cfg);
static int _engine_init(Engine* engine, const EngineConfig* cfg);
static void _engine_late_draw(void* user, const PostFXLateDraw* late);
static int _setup_engine_glfw(Engine* engine, EngineWindowMode window_mode, const char* monitor);
static int _setup_engine_gui(Engine* engine);
static void _engine_cursor_position_callback(GLFWwindow* window, double xpos, double ypos);
static void _engine_mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
static void _engine_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
static void _engine_char_callback(GLFWwindow* window, unsigned int codepoint);
static void _engine_scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
static void _engine_framebuffer_size_callback(GLFWwindow* window, int fb_width, int fb_height);
static void _engine_close_callback(GLFWwindow* window);
static void _engine_derive_view(const Engine* engine, SceneView* view);
static void _engine_input_script_frame(Engine* engine);
static void _engine_input_script_close(Engine* engine);
static void _engine_input_script_free(Engine* engine);
static bool _engine_input_script_cursor(const Engine* engine, double* fb_x, double* fb_y);
static bool _engine_input_script_running(const Engine* engine);
static SceneNode* _engine_pick(Engine* engine, double mouse_fb_x, double mouse_fb_y);

// The timed wakes held at once (spec 14.9), and how late merging two may make one.
#define ENGINE_WAKES_MAX          64
#define ENGINE_WAKE_MERGE_SECONDS 0.001

// A timed wake, on glfwGetTime's clock: when it draws, and the earliest time asked that it stands
// for, so merging never makes a caller later than a merge.
typedef struct {
    double at, from;
} EngineWakeTime;

// Idle mode's handshake (spec 14.1): what the frame's own sources and other threads tell the wait.
typedef struct EngineWake {
    atomic_bool requested;                  // engine_request_frame since the last frame began
    atomic_bool waiting;                    // the loop is waiting, so a request must wake it
    atomic_int work_out;                    // engine_begin_work calls not yet ended
    EngineWakeTime wakes[ENGINE_WAKES_MAX]; // the timed wakes not yet come, soonest first
    int wake_count;
    bool wakes_full_said; // a full list has been warned about
} EngineWake;

// The soonest timed wake, INFINITY for none.
static double _engine_wake_soonest(const EngineWake* w) {
    return w->wake_count ? w->wakes[0].at : INFINITY;
}

/*
 * Engine
 */
// The Engine with every default in place and nothing made: no window, no GL
// object, no post chain. free_engine takes one in this state.
static Engine* _engine_alloc(const EngineConfig* cfg) {
    // Zeroed, not malloc'd: every field below is still set explicitly, but a
    // struct this wide cannot rely on each new field being remembered here --
    // and a field only ever written conditionally has no other initializer at
    // all.
    Engine* engine = calloc(1, sizeof(Engine));
    if (!engine) {
        log_error("Failed to allocate memory for engine");
        return NULL;
    }
    engine->sorted_opaque = create_draw_list();
    engine->window_title = safe_strdup(cfg->title ? cfg->title : "Cetra");
    if (!engine->sorted_opaque || !engine->window_title) {
        log_error("Failed to allocate memory for engine");
        free_engine(engine);
        return NULL;
    }

    engine->window = NULL;

    engine->win_width = cfg->width > 0 ? cfg->width : 1280;
    engine->win_height = cfg->height > 0 ? cfg->height : 720;
    engine->headless = cfg->headless;
    engine->capture_budget_ms = cfg->headless ? 0.0f : ENGINE_CAPTURE_BUDGET_MS;
    engine->typical_size = 1.0f;
    engine->fb_width = 0;
    engine->fb_height = 0;
    engine->vsync = !cfg->no_vsync;
    // Always windowed here, whatever the config asked for: this field means
    // where the window IS, and there is no window yet. Setup creates one
    // windowed and then moves it through the same call a later switch uses, so
    // the mode logic exists once rather than once here and once there.
    engine->window_mode = ENGINE_WINDOW_WINDOWED;
    engine->window_monitor = NULL;
    engine->window_mode_refused = false;
    engine->windowed = (EngineWindowRect){0, 0, engine->win_width, engine->win_height};
    engine->layers_vt_enabled = true; // composite cache on; --no-layers-vt is the bisect lever
    engine->layers_vt_res = 0;        // derived from the splat domain unless overridden
    engine->layers_vt_pages_enabled = true;    // pages on; --no-layers-vt-pages is stage 1 exactly
    engine->layers_vt_feedback_enabled = true; // the vote pass; off = prediction alone
    engine->layers_vt_page_slots = 0;          // 0 = the full physical atlas
    engine->layers_vt_page_budget = 0;         // 0 = the default bakes-per-frame
    engine->layers_vt_probe_interval = 0;      // diagnostic; off unless a probe asks

    engine->error_callback = NULL;
    engine->mouse_button_callback = NULL;
    engine->cursor_position_callback = NULL;
    engine->key_callback = NULL;
    engine->scroll_callback = NULL;

    engine->views = NULL;
    engine->view_count = 0;
    engine->view_capacity = 0;
    engine->focus_view = NULL;
    engine->capture_view = NULL;

    engine->light_cluster = NULL;
    engine->cluster_debug = false;
    engine->alpha_jitter_enabled = true;

    engine->scenes = NULL;
    engine->scene_count = 0;
    engine->current_scene_index = 0;

    engine->programs = NULL;
    engine->program_count = 0;
    engine->program_map = NULL;

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
    engine->occlusion_cull_enabled = true;
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

    // A look, so off. Its settings are a console's 480 lines on a living-room set, there to be
    // seen and not to fight the picture.
    engine->crt_enabled = false;
    engine->crt_lines = 480.0f;
    engine->crt_scanlines = 0.4f;
    engine->crt_mask = 0.3f;
    engine->crt_curvature = 0.35f;
    engine->crt_bleed = 0.5f;
    engine->show_gui = false;
    engine->show_fps = false;
    engine->show_camera_hud = false;
    engine->gui_frame_active = false;
    engine->screenshot_path = NULL;
    engine->screenshot_every = 0;
    engine->exit_after_frames = 0;
    engine->total_frames = 0;
    engine->user_data = NULL;

    engine->idle = cfg->idle;
    engine->time_waited = 0.0;
    engine->wake = calloc(1, sizeof(EngineWake));
    if (!engine->wake) {
        log_error("Failed to allocate the idle wait's state");
        free_engine(engine);
        return NULL;
    }
    atomic_init(&engine->wake->requested, false);
    atomic_init(&engine->wake->waiting, false);
    atomic_init(&engine->wake->work_out, 0);

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

    engine->profiler = NULL;

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

    // One order for a whole engine and for one that failed part-way through
    // creation: every owner below tolerates NULL, and the two things that need
    // a live context -- the GL deletes and the ImGui shutdown -- are guarded
    // on the window and the ImGui context respectively.

    // Borrows nothing: it holds copies of DrawItems, and a DrawItem borrows its
    // mesh and node. So this is safe before or after the scenes go.
    free_draw_list(engine->sorted_opaque);
    free(engine->window_title);

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
    for (size_t i = 0; i < engine->shader_hook_count; ++i)
        free_shader_hook(engine->shader_hooks[i]);
    free(engine->shader_hooks);
    for (size_t i = 0; i < engine->failed_variant_count; ++i)
        free(engine->failed_variants[i]);
    free(engine->failed_variants);

    if (engine->screenshot_path) {
        free(engine->screenshot_path);
    }

    _engine_input_script_free(engine);
    free(engine->clipboard);

    // GL objects must be released while the context still exists, i.e. before
    // the window is destroyed; and there is a context to release them into
    // only if the window came up.
    if (engine->window) {
        // First, since each view's chain borrows the programs below. Freeing a view takes it off
        // the list, so the list empties from its end.
        while (engine->view_count > 0)
            free_scene_view(engine->views[engine->view_count - 1]);
        free_scene_view(engine->capture_view);
        free_postfx_programs(engine->postfx_programs);
        free_crt(engine->crt);
        gl_color_target_free(&engine->window_picture);
        free_loading_screen(engine->loading_screen);

        free_light_cluster_context(engine->light_cluster);
        free_occlusion_context(engine->occlusion);
        free_rain_renderer(engine->rain_renderer);
        free_fire_renderer(engine->fire_renderer);
        free_ubo(engine->view_ubo);
        free_ubo(engine->instance_ubo);
        free_ubo(engine->vt_pages_ubo);
        free_ubo(engine->roads_ubo);
        free_layers_vt_feedback(engine->vt_feedback);

        if (engine->catcher_vao)
            glDeleteVertexArrays(1, &engine->catcher_vao);
        if (engine->catcher_vbo)
            glDeleteBuffers(1, &engine->catcher_vbo);
        if (engine->xyz_vao)
            glDeleteVertexArrays(1, &engine->xyz_vao);
        if (engine->xyz_vbo)
            glDeleteBuffers(1, &engine->xyz_vbo);

        free_ltc_tables(engine->ltc);
        engine->ltc = NULL;

        // Its query objects belong to the context, so this has to happen above
        // glfwDestroyWindow like every other GL teardown in this function.
        free_profiler(engine->profiler);
        engine->profiler = NULL;

        gl_delete_texture(&engine->brdf_lut);

        // The backends assert on a missing context, so the ImGui teardown is
        // gated on the context and not on the window: creation can fail after
        // the window and before the GUI.
        if (igGetCurrentContext()) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            igDestroyContext(NULL);
        }

        glfwDestroyWindow(engine->window);
    }

    // Safe when GLFW never initialised, which is the window-less case.
    glfwTerminate();

    free(engine->views);
    free(engine->wake);
    free(engine);
}

/*
 * The index of the display a name selects, or -1 when this machine has none
 * attached -- which a VM or a headless session genuinely reports, so no caller
 * may assume index 0 exists. An unmatched name falls back to the primary rather
 * than failing: a player who unplugs the monitor they chose should get a window
 * they can see, not one placed somewhere that no longer exists.
 */
static int _monitor_index_by_name(const char* name) {
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    if (!monitors || count <= 0) {
        return -1;
    }
    if (name && name[0]) {
        for (int i = 0; i < count; i++) {
            const char* have = glfwGetMonitorName(monitors[i]);
            if (have && strcmp(have, name) == 0) {
                return i;
            }
        }
    }
    return 0; // GLFW puts the primary first
}

static GLFWmonitor* _monitor_by_name(const char* name) {
    const int index = _monitor_index_by_name(name);
    if (index < 0) {
        return NULL;
    }
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    return (monitors && index < count) ? monitors[index] : NULL;
}

// The one place the swap interval is set, so the value survives a mode change.
//
// Headless swaps without waiting whatever was asked, or a run's frame rate would
// depend on a display it is not using -- and it is set EXPLICITLY rather than
// skipped, because skipping leaves the context's own default to decide, and a
// default of 1 would put every golden and every timing arm behind a display's
// refresh.
void engine_apply_swap_interval(const Engine* engine) {
    if (!engine || !engine->window) {
        return;
    }
    glfwSwapInterval((engine->vsync && !engine->headless) ? 1 : 0);
}

bool engine_focus_eye(const Engine* engine, vec3 out) {
    if (!engine || !engine->focus_view)
        return false;
    glm_vec3_copy((float*)engine->focus_view->camera.position, out);
    return true;
}

/*
 * Setup GLFW
 */
static int _setup_engine_glfw(Engine* engine, EngineWindowMode window_mode, const char* monitor) {
    if (!engine) {
        return -1;
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

    // Hidden until it is in the mode that was asked for, and shown below.
    // A window that appears windowed and jumps to a monitor one call later is a
    // visible flash on every launch by anyone who chose fullscreen, and hiding
    // it is what lets creation reuse the mode switch rather than repeat it.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    engine->window =
        glfwCreateWindow(engine->win_width, engine->win_height, engine->window_title, NULL, NULL);
    if (engine->window == NULL) {
        log_error("Failed to create GLFW window");
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(engine->window);

    engine_apply_swap_interval(engine);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (glewInit() != GLEW_OK) {
        log_error("Failed to initialize GLEW");
        // No window left behind: free_engine reads it as "there is a context".
        glfwDestroyWindow(engine->window);
        engine->window = NULL;
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
    // Refused here rather than left to the link: every lit program declares the light index
    // pool, and a driver too small for it fails each one with a block-size error that says nothing
    // of why.
    const GLint uniform_block_bytes = get_gl_max_uniform_block_size();
    log_info("GL uniform blocks: %d bytes", uniform_block_bytes);
    if (uniform_block_bytes < UBO_CLUSTER_INDICES_BLOCK_SIZE) {
        log_error("GL allows %d-byte uniform blocks; the light index pool needs %d",
                  uniform_block_bytes, UBO_CLUSTER_INDICES_BLOCK_SIZE);
        glfwDestroyWindow(engine->window);
        engine->window = NULL;
        glfwTerminate();
        return -1;
    }

    // Into the requested mode BEFORE the sizes are read, so what lands below is
    // the size the window will actually render at rather than the windowed one
    // it was created with. The callback is not registered yet, which is exactly
    // why this has to precede the read rather than rely on it. A windowed request
    // is the switch's own no-op, and an out-of-range one is refused there by name.
    // Fullscreen is laid over its monitor BORDERLESS for now, the same rectangle: on Windows
    // and X11 taking a monitor shows the window, and it stays hidden until the end of creation.
    // It takes the monitor there, still hidden on macOS, just before it is shown.
    engine_set_window_mode(
        engine, window_mode == ENGINE_WINDOW_FULLSCREEN ? ENGINE_WINDOW_BORDERLESS : window_mode,
        monitor);

    // Both sizes from the window we actually got, not the one we asked for --
    // a window manager may grant something else, and these two writers (here
    // and the framebuffer-size callback) are the only ones.
    glfwGetWindowSize(engine->window, &(engine->win_width), &(engine->win_height));
    glfwGetFramebufferSize(engine->window, &(engine->fb_width), &(engine->fb_height));
    glViewport(0, 0, engine->fb_width, engine->fb_height);

    return 0;
}

// False when the frame cannot be drawn: a zero-area (minimized) window. A view whose targets failed
// to build is skipped by itself.
static bool _engine_frame_renderable(const Engine* engine) {
    return engine->fb_width > 0 && engine->fb_height > 0;
}

// Events arrive only inside a poll, so by the next one the frame's hooks have read this frame's
// wheel, key presses and typing, and the next poll starts the next frame's.
static void _engine_clear_latches(Engine* engine) {
    engine->input.scroll_dx = 0.0;
    engine->input.scroll_dy = 0.0;
    memset(engine->input.key_pressed, 0, sizeof(engine->input.key_pressed));
    engine->input.key_event_count = 0;
    engine->input.pointer_pressed = false;
    engine->input.pointer_released = false;
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

// Idle mode (spec 14.1) ------------------------------------------------------------------------

// True when the scene has anything for a view to draw: an item, an environment, water, falling
// rain, a fire or a particle system. A view drawing one asks for the next frame until its average
// is finished, so under idle mode a view that does not average runs as it always has. Read once
// the frame's draw list is built.
static bool _engine_scene_draws(const Scene* scene) {
    if (!scene)
        return false;
    return (scene->draw_list && scene->draw_list->count > 0) || scene->ibl || scene->sky ||
           scene->water || rain_draws(scene->rain) || fire_system_active(scene->fire) ||
           scene->particle_system_count > 0;
}

// A loader thread finished a decode, which only a frame can deliver: an idle loop asleep in its
// wait would otherwise hold it until the next input. The loader knows nothing of the engine, so
// it takes this, with the engine as its user pointer.
static void _engine_loader_notify(void* engine) {
    engine_request_frame(engine);
}

// What the engine itself asks the next frame for, once this one is drawn: the loading screen while
// it plays; an overlay switched on or off inside the frame, after the GUI frame was latched; and
// the debug GUI while events wait in ImGui's queue, which it takes a frame at a time. The FPS
// readout and the camera HUD take no input, so only the GUI asks.
static void _engine_frame_end_requests(Engine* engine) {
    const bool gui_wanted = engine->show_gui || engine->show_fps || engine->show_camera_hud;
    const bool gui_queued = engine->show_gui && engine->gui_frame_active &&
                            igGetCurrentContext()->InputEventsQueue.Size > 0;
    // A change said or a node moved after the views were decided is drawn by the next frame.
    const Scene* scene = engine_get_scene(engine);
    const bool changed_late =
        engine->change_count + (scene ? scene->change_count : 0) != engine->decided_changes ||
        (scene && scene->moved_mask);
    if (engine_loading_screen_shown(engine) || gui_wanted != engine->gui_frame_active ||
        gui_queued || (engine->view_count > 0 && changed_late))
        engine_request_frame(engine);
}

typedef enum { IDLE_DRAW, IDLE_WAIT, IDLE_WAIT_WORK, IDLE_DONE } IdleNext;

// Under idle mode, what follows this frame: the next drawn, a wait for something to ask, a wait for
// the work out, or -- headless, with nothing asking and nothing out that will -- the end of the
// run.
//
// Everything that knows it wants a frame asks during the frame (engine_request_frame); the input
// script alone is asked here, since its frame plays after this is decided, at the end of the
// frame's events. The work out is read BEFORE the request: engine_end_work stores its request
// before it lowers its count, so this sees one or the other and never neither.
//
// Headless, work out is waited for before anything else that asks (spec 14.7), so what a frame
// began has landed by the next on any machine: a script's frame N + 1 sees the work frame N began.
// A window draws what asks at once, since input has to reach the screen within a frame. The
// texture loader's decodes are not waited for: its count falls only as a frame delivers them.
static IdleNext _engine_idle_next(const Engine* engine) {
    const EngineWake* w = engine->wake;
    const bool working = atomic_load(&w->work_out) > 0;
    if (working && engine->headless)
        return IDLE_WAIT_WORK;
    const bool pending = working || async_loader_is_busy(engine->async_loader) || w->wake_count > 0;
    if (atomic_load(&w->requested) || glfwGetTime() >= _engine_wake_soonest(w) ||
        _engine_input_script_running(engine))
        return IDLE_DRAW;
    return pending || !engine->headless ? IDLE_WAIT : IDLE_DONE;
}

/*
 * The end of a frame's events: what came in while it drew, polled; or under idle mode, a wait until
 * something asks for the next frame.
 *
 * A window's wake is an event or a request, and the frame it wakes draws it, so no callback has to
 * say it woke anything; one that changed nothing costs a frame. Headless has no events: a wake
 * there is a request, a lapsed timed wake, or nothing -- the spurious return GLFW allows, which a
 * hidden window gets once in its first tenth of a second on macOS -- so it asks again.
 *
 * The wait is added to time_waited, which every clock leaves out, and total_frames does not
 * advance: the next frame is adjacent to this one, and every history that tests for a gap keeps
 * what it had.
 *
 * Waiting for headless work, a timed wake cannot draw the next frame, so it is no deadline: the
 * wait runs until the work's end wakes it. Work that never ends would hold the run here in silence,
 * so it is said once, after ENGINE_WORK_WARN_SECONDS, and the run goes on waiting.
 *
 * A close request read here is asked of the app only when the run was not ending already: the frame
 * limit, idle mode's headless end or the app itself setting the window's flag decided that, and a
 * refusal clearing the flag must not undo it.
 */
#define ENGINE_WORK_WARN_SECONDS 10.0

// Every timed wake that has come is answered by the frame about to begin. Dropped without idle mode
// too, where nothing waits on them: the caret asks for its next blink every frame, and a list never
// emptied would fill.
static void _engine_drop_passed_wakes(EngineWake* w) {
    const double now = glfwGetTime();
    int passed = 0;
    while (passed < w->wake_count && w->wakes[passed].at <= now)
        passed++;
    w->wake_count -= passed;
    memmove(w->wakes, w->wakes + passed, (size_t)w->wake_count * sizeof(EngineWakeTime));
}

static void _engine_idle_wait(Engine* engine);

static void _engine_frame_events(Engine* engine) {
    engine->ending = glfwWindowShouldClose(engine->window);
    if (engine->idle)
        _engine_idle_wait(engine);
    else
        glfwPollEvents();
    _engine_drop_passed_wakes(engine->wake);
}

// Idle mode's end of a frame's events: the wait until something asks for the next frame, then what
// came in.
static void _engine_idle_wait(Engine* engine) {
    EngineWake* w = engine->wake;
    // Raised before anything is read, as a request stores its flag before it reads this: either the
    // request sees the loop waiting and wakes it, or the loop sees the request.
    atomic_store(&w->waiting, true);
    double warn_at = glfwGetTime() + ENGINE_WORK_WARN_SECONDS;
    IdleNext next;
    while (((next = _engine_idle_next(engine)) == IDLE_WAIT || next == IDLE_WAIT_WORK) &&
           !glfwWindowShouldClose(engine->window)) {
        const double start = glfwGetTime();
        const double until = next == IDLE_WAIT_WORK ? warn_at : _engine_wake_soonest(w);
        if (isfinite(until))
            glfwWaitEventsTimeout(until > start ? until - start : 0.0);
        else
            glfwWaitEvents();
        engine->time_waited += glfwGetTime() - start;
        if (next == IDLE_WAIT_WORK && glfwGetTime() >= warn_at) {
            log_warn("headless: work begun with engine_begin_work, %d of it, has not ended in %.0f "
                     "s; the run waits for it",
                     atomic_load(&w->work_out), ENGINE_WORK_WARN_SECONDS);
            warn_at = INFINITY;
        }
        if (!engine->headless)
            break;
    }
    atomic_store(&w->waiting, false);
    // What arrived while the frame drew, when nothing was waited for, and the empty events
    // requests posted.
    glfwPollEvents();
    // What asked is answered by the frame about to begin.
    atomic_store(&w->requested, false);
}

// A loop waiting for events sees no flag: an empty event wakes it, GLFW's one call that may be made
// from any thread. Called after the store the loop is to see, as the wait raises `waiting` before
// its reads.
static void _engine_wake(EngineWake* w) {
    if (atomic_load(&w->waiting))
        glfwPostEmptyEvent();
}

void engine_request_frame(Engine* engine) {
    if (!engine) {
        log_error("engine_request_frame: NULL engine");
        return;
    }
    atomic_store(&engine->wake->requested, true);
    _engine_wake(engine->wake);
}

// A day: past anything a frame is asked for, and short of Win32's 49-day millisecond wait.
#define ENGINE_WAKE_MAX_SECONDS 86400.0

void engine_request_frame_after(Engine* engine, double seconds) {
    if (!engine) {
        log_error("engine_request_frame_after: NULL engine");
        return;
    }
    if (isnan(seconds) || seconds > ENGINE_WAKE_MAX_SECONDS) {
        log_error("engine_request_frame_after: %g s is not a wait (at most %g)", seconds,
                  ENGINE_WAKE_MAX_SECONDS);
        return;
    }
    const double at = glfwGetTime() + fmax(seconds, 0.0);
    EngineWake* w = engine->wake;
    int k = w->wake_count;
    while (k > 0 && w->wakes[k - 1].at > at)
        k--;
    EngineWakeTime* next = k < w->wake_count ? &w->wakes[k] : NULL;
    EngineWakeTime* prev = k > 0 ? &w->wakes[k - 1] : NULL;
    // A time is one with a neighbour, at the later of the two, while that keeps every time either
    // stands for within a merge: a frame is never early and no caller more than a merge late, and a
    // caller asking for the same time every frame holds one. A full list merges it whatever the
    // wait -- with the next time held, or the latest moved to it -- late, and never lost.
    const bool near_next = next && next->at - fmin(next->from, at) <= ENGINE_WAKE_MERGE_SECONDS;
    const bool near_prev = prev && at - prev->from <= ENGINE_WAKE_MERGE_SECONDS;
    if (!near_next && !near_prev) {
        if (w->wake_count < ENGINE_WAKES_MAX) {
            memmove(w->wakes + k + 1, w->wakes + k,
                    (size_t)(w->wake_count - k) * sizeof(EngineWakeTime));
            w->wakes[k] = (EngineWakeTime){at, at};
            w->wake_count++;
            return;
        }
        if (!w->wakes_full_said) {
            log_warn("engine_request_frame_after: %d timed wakes are held, the most; one asked for "
                     "past that is merged with the next later one",
                     ENGINE_WAKES_MAX);
            w->wakes_full_said = true;
        }
    }
    if (next && (near_next || !near_prev))
        next->from = fmin(next->from, at);
    else
        prev->at = at;
}

void engine_begin_work(Engine* engine) {
    if (!engine) {
        log_error("engine_begin_work: NULL engine");
        return;
    }
    atomic_fetch_add(&engine->wake->work_out, 1);
}

void engine_end_work(Engine* engine) {
    if (!engine) {
        log_error("engine_end_work: NULL engine");
        return;
    }
    // The request is stored before the count falls, and the loop reads the count before the
    // request, so it sees either the work still out or the request already made, and never
    // neither. The one wake comes after both, so a waiting loop wakes to the count already fallen.
    atomic_store(&engine->wake->requested, true);
    atomic_int* out = &engine->wake->work_out;
    int was = atomic_load(out);
    do {
        if (was <= 0) {
            log_error("engine_end_work: no work was begun");
            return;
        }
    } while (!atomic_compare_exchange_weak(out, &was, was - 1));
    _engine_wake(engine->wake);
}

void engine_set_close_hook(Engine* engine, EngineCloseFunc fn, void* user) {
    if (!engine) {
        log_error("engine_set_close_hook: NULL engine");
        return;
    }
    engine->close_hook = fn;
    engine->close_user = user;
}

// The window asked to close, its should-close flag already set, as GLFW sets it before its
// callback: a hook keeping it open clears the flag, and the next frame says why.
static void _engine_close_asked(Engine* engine) {
    if (engine->close_hook && !engine->close_hook(engine, engine->close_user)) {
        glfwSetWindowShouldClose(engine->window, GLFW_FALSE);
        engine_request_frame(engine);
    }
}

// Asked only when the run was not ending already as the events were read (_engine_frame_events).
static void _engine_close_callback(GLFWwindow* window) {
    Engine* engine = glfwGetWindowUserPointer(window);
    if (engine && !engine->ending)
        _engine_close_asked(engine);
}

const char* const* engine_monitor_names(const Engine* engine, int* out_count) {
    // Rebuilt per call rather than cached: GLFW owns the strings, a display can
    // leave between two calls, and the list is asked for when a screen is built
    // rather than per frame.
    static const char* names[ENGINE_MAX_MONITORS];
    int count = 0;
    GLFWmonitor** monitors = (engine && engine->window) ? glfwGetMonitors(&count) : NULL;
    if (!monitors || count < 0) {
        count = 0;
    }
    if (count > ENGINE_MAX_MONITORS) {
        // The cap is the array's, and it is reported rather than hidden: a
        // picker showing fewer displays than the window can be placed on is
        // exactly the disagreement naming them was meant to prevent.
        log_warn("engine_monitor_names: %d displays attached, reporting the first %d", count,
                 ENGINE_MAX_MONITORS);
        count = ENGINE_MAX_MONITORS;
    }
    for (int i = 0; i < count; i++) {
        const char* name = glfwGetMonitorName(monitors[i]);
        names[i] = name ? name : "(unnamed)";
    }
    if (out_count) {
        *out_count = count;
    }
    return names;
}

int engine_monitor_index(const Engine* engine, const char* name) {
    if (!engine || !engine->window) {
        return -1;
    }
    return _monitor_index_by_name(name);
}

void engine_set_vsync(Engine* engine, bool vsync) {
    if (!engine) {
        log_error("engine_set_vsync: NULL engine");
        return;
    }
    engine->vsync = vsync;
    engine_apply_swap_interval(engine);
}

EngineWindowPlacement engine_window_placement(EngineWindowMode mode, EngineWindowRect monitor,
                                              EngineWindowRect saved) {
    EngineWindowRect rect = (mode == ENGINE_WINDOW_WINDOWED) ? saved : monitor;
    if (mode == ENGINE_WINDOW_FULLSCREEN) {
        // GLFW places a fullscreen window relative to the monitor it is given,
        // so the origin is the monitor's own rather than the desktop's. A
        // borderless window is placed in desktop space and keeps the offset,
        // which is what puts it on the right screen of two.
        rect.x = 0;
        rect.y = 0;
    }
    return (EngineWindowPlacement){.fullscreen = mode == ENGINE_WINDOW_FULLSCREEN,
                                   .decorated = mode != ENGINE_WINDOW_BORDERLESS,
                                   .rect = rect};
}

void engine_set_window_mode(Engine* engine, EngineWindowMode mode, const char* monitor) {
    if (!engine) {
        log_error("engine_set_window_mode: NULL engine");
        return;
    }
    if (mode < 0 || mode >= ENGINE_WINDOW_MODE_COUNT) {
        log_error("engine_set_window_mode: mode %d is not a window mode", (int)mode);
        return;
    }
    // A hidden window has no display to fill, and putting one on a monitor
    // would make the frame's size depend on the machine rather than on what was
    // asked for. Said once, because a caller that pushes a whole settings
    // struct will reach this on every edit.
    if (!engine->window || engine->headless) {
        if (mode != ENGINE_WINDOW_WINDOWED && !engine->window_mode_refused) {
            engine->window_mode_refused = true;
            log_info("engine_set_window_mode: no window to move; staying windowed");
        }
        return;
    }

    GLFWmonitor* target = NULL;
    EngineWindowRect mon = {0, 0, 0, 0};
    int refresh = 0;
    if (mode != ENGINE_WINDOW_WINDOWED) {
        target = _monitor_by_name(monitor);
        const GLFWvidmode* vm = target ? glfwGetVideoMode(target) : NULL;
        // No display, or one that will not say what mode it is in. Staying put
        // is the only honest answer: sizing to a guess strands the window at a
        // size nothing chose, which is the failure this whole path avoids.
        if (!target || !vm) {
            log_warn("engine_set_window_mode: no usable monitor; leaving the window where it is");
            return;
        }
        glfwGetMonitorPos(target, &mon.x, &mon.y);
        mon.w = vm->width;
        mon.h = vm->height;
        refresh = vm->refreshRate;
    }

    // Kept current while the window IS windowed, rather than sampled on the way
    // out. A mode change is not the only thing that moves a window -- the
    // player drags it -- so a rectangle latched once would put the window back
    // somewhere it had not been for hours. Recording it here also makes a
    // windowed-to-windowed replay land exactly where the window already is,
    // which is what keeps the early-out below an optimisation rather than the
    // only thing standing between a caller and a window that jumps.
    if (engine->window_mode == ENGINE_WINDOW_WINDOWED) {
        glfwGetWindowPos(engine->window, &engine->windowed.x, &engine->windowed.y);
        glfwGetWindowSize(engine->window, &engine->windowed.w, &engine->windowed.h);
    }

    // Already there: nothing to move, and nothing to tell GLFW.
    if (mode == engine->window_mode && target == engine->window_monitor) {
        return;
    }

    const EngineWindowPlacement at = engine_window_placement(mode, mon, engine->windowed);

    // Decoration before the move: a decorated window told to fill a monitor is
    // sized to the monitor and then given a title bar it has no room for, so
    // the frame lands one bar's height taller than the display.
    glfwSetWindowAttrib(engine->window, GLFW_DECORATED, at.decorated ? GLFW_TRUE : GLFW_FALSE);
    glfwSetWindowMonitor(engine->window, at.fullscreen ? target : NULL, at.rect.x, at.rect.y,
                         at.rect.w, at.rect.h, at.fullscreen ? refresh : 0);

    engine->window_mode = mode;
    engine->window_monitor = target;
    // The swap interval is set on the context rather than the window, so taking
    // a monitor can drop it and it has to be put back by hand.
    engine_apply_swap_interval(engine);
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
    glfwSetCharCallback(engine->window, _engine_char_callback);
    glfwSetWindowCloseCallback(engine->window, _engine_close_callback);
    // Focus and the cursor entering or leaving have no engine-side logic, so they go straight to
    // the backend, which is owed both when installed without its own callbacks.
    glfwSetWindowFocusCallback(engine->window, ImGui_ImplGlfw_WindowFocusCallback);
    glfwSetCursorEnterCallback(engine->window, ImGui_ImplGlfw_CursorEnterCallback);
    glfwSetScrollCallback(engine->window, _engine_scroll_callback);

    return 0;
}

/*
 * Initialize the Engine
 *
 */
static int _engine_init(Engine* engine, const EngineConfig* cfg) {
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

    if (_setup_engine_glfw(engine, cfg->window_mode, cfg->monitor) != 0) {
        log_error("Failed to initialize engine GLFW");
        return -1;
    }

    // Clustered-forward lighting (spec 9.1): owns its own UBOs + scratch
    engine->light_cluster = create_light_cluster_context();
    engine->occlusion = create_occlusion_context();
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

    engine->async_loader = create_async_loader(_engine_loader_notify, engine);
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
            engine_add_program(engine, text_prog);
            engine->text_renderer->text_program = text_prog;
        }
    }

    // LTC area-light tables (spec 9.2): static fitted data, uploaded once
    engine->ltc = create_ltc_tables();
    if (!engine->ltc)
        return -1;

    // Only when asked for (spec 11.27). Not fatal if it fails: losing the
    // instrument should not cost the frame it was meant to measure.
    if (cfg->profiler) {
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

    // The axis gizmo, the same layout with its constant geometry uploaded once.
    glGenVertexArrays(1, &engine->xyz_vao);
    glGenBuffers(1, &engine->xyz_vbo);
    glBindVertexArray(engine->xyz_vao);
    glBindBuffer(GL_ARRAY_BUFFER, engine->xyz_vbo);
    glBufferData(GL_ARRAY_BUFFER, xyz_vertices_size, xyz_vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    engine->bone_program = create_bone_program();
    if (engine->bone_program) {
        engine_add_program(engine, engine->bone_program);
    }

    // Shadow catcher: unit quad at y=0 (scaled by planeRadius in the shader)
    engine->shadow_catcher_program = create_shadow_catcher_program();
    if (engine->shadow_catcher_program) {
        engine_add_program(engine, engine->shadow_catcher_program);

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

    // The post chain's programs (resolve + bloom + tone map and the rest), made once for every
    // view's chain; each view builds its own targets.
    engine->postfx_programs = create_postfx_programs();
    if (!engine->postfx_programs) {
        log_error("Failed to initialize engine post-processing");
        return -1;
    }

    // Shown only once everything it draws with is built (spec 13.39): shown before the programs
    // and the post chain, it stood blank and unanswering for the second they took. A size the
    // shown window turns out to have reaches the framebuffer-size callback, registered above, and
    // the first frame's top rebuilds for it.
    if (!engine->headless) {
        // The mode asked for while still hidden, so the window first appears in it: a no-op for
        // any but fullscreen, which went on borderless above. Shown borderless and switched after,
        // it lost focus in the switch, and a fullscreen window that loses focus is minimized.
        engine_set_window_mode(engine, cfg->window_mode, cfg->monitor);
        // GLFW will not show a window already on a monitor, and shows one by focusing it, as
        // its own creation of a fullscreen window does.
        if (glfwGetWindowMonitor(engine->window))
            glfwFocusWindow(engine->window);
        else
            glfwShowWindow(engine->window);
        // Asked of the window system, not assumed: a window it would not show runs the whole
        // game with nothing on the screen.
        if (!glfwGetWindowAttrib(engine->window, GLFW_VISIBLE))
            log_warn("create_engine: the window could not be shown");
    }
    return 0;
}

/*
 * Callbacks
 */
void engine_set_error_callback(Engine* engine, GLFWerrorfun error_callback) {
    if (!engine)
        return;
    engine->error_callback = error_callback;
    if (engine->window) {
        glfwSetErrorCallback(error_callback);
    }
}

void engine_set_cursor_position_callback(Engine* engine,
                                         CursorPositionCallback cursor_position_callback) {
    if (!engine)
        return;
    engine->cursor_position_callback = cursor_position_callback;
}

void engine_set_mouse_button_callback(Engine* engine, MouseButtonCallback mouse_button_callback) {
    if (!engine)
        return;
    engine->mouse_button_callback = mouse_button_callback;
}

void engine_set_key_callback(Engine* engine, KeyCallback key_callback) {
    if (!engine)
        return;
    engine->key_callback = key_callback;
}

void engine_set_scroll_callback(Engine* engine, ScrollCallback scroll_callback) {
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
    // than computing a NaN aspect from it. Each view holds its targets and
    // _engine_frame_renderable skips the frame meanwhile.
    if (fb_width <= 0 || fb_height <= 0)
        return;

    // Events are polled at the end of the frame, so a click in the same batch
    // as this resize would otherwise ray-pick through an aspect one frame old.
    for (size_t i = 0; i < engine->view_count; i++)
        _engine_derive_view(engine, engine->views[i]);
    // Window points, matching init_text_renderer -- text is authored in points
    // and the ortho must stay in the space it was set up in. Handing it
    // framebuffer pixels here would halve every string on a Retina display the
    // first time the window moved.
    if (engine->text_renderer)
        text_renderer_update_screen_size(engine->text_renderer, engine->win_width,
                                         engine->win_height);
}

// Window coordinates to framebuffer pixels with +Y up, from the STORED sizes.
// Re-querying GLFW for them here would make the size fields change mid-frame,
// between the frame-top resize check and the scene pass -- and a scene target
// that disagrees with the post chain by even a frame is an invalid multisample
// blit. The framebuffer-size callback owns them. False while the window has no
// area.
static bool _window_to_fb(const Engine* engine, double wx, double wy, double* fb_x, double* fb_y) {
    if (engine->win_width <= 0 || engine->win_height <= 0)
        return false;
    *fb_x = (wx / engine->win_width) * engine->fb_width;
    *fb_y = (1.0 - wy / engine->win_height) * engine->fb_height;
    return true;
}

bool engine_cursor_fb(const Engine* engine, double* fb_x, double* fb_y) {
    if (!engine || !fb_x || !fb_y)
        return false;
    // A scripted pointer answers this too, or a caller that asks where the
    // cursor is gets the real one while everything else gets the script: the
    // 2D canvas zooms about the point under the cursor, and read the window
    // corner instead.
    if (engine->input_script)
        return _engine_input_script_cursor(engine, fb_x, fb_y);
    if (!engine->window)
        return false;
    double wx = 0.0, wy = 0.0;
    glfwGetCursorPos(engine->window, &wx, &wy);
    return _window_to_fb(engine, wx, wy, fb_x, fb_y);
}

bool engine_fb_to_points(const Engine* engine, double fb_x, double fb_y, double* x, double* y) {
    if (!engine || !x || !y || engine->fb_width <= 0 || engine->fb_height <= 0)
        return false;
    *x = (fb_x / engine->fb_width) * engine->win_width;
    *y = (1.0 - fb_y / engine->fb_height) * engine->win_height;
    return true;
}

/*
 * The pointer, at a FRAMEBUFFER position with +Y up.
 *
 * Each of the three is the half of its GLFW callback below that a SCRIPTED
 * pointer reaches as well (spec 12.19): everything after ImGui has seen the
 * event and the position has been converted. Splitting there rather than
 * writing InputState directly is what makes a replayed drag take the same path
 * as a real one -- the drag state, the ray picking, and the app's own forwarded
 * callback, in that order -- so an arm that asserts a scripted drag is
 * asserting the code a hand reaches.
 */
static void _engine_pointer_move(Engine* engine, double fb_x, double fb_y) {
    engine->input.pointer_fb_x = (float)fb_x;
    engine->input.pointer_fb_y = (float)fb_y;
    if (engine->input.is_dragging) {
        engine->input.drag_fb_x = (float)(fb_x - engine->input.center_fb_x);
        engine->input.drag_fb_y = (float)(fb_y - engine->input.center_fb_y);
    }

    if (engine->cursor_position_callback) {
        engine->cursor_position_callback(engine, fb_x, fb_y);
    }
}

static void _engine_cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
    if (!window)
        return;

    Engine* engine = glfwGetWindowUserPointer(window);
    if (!engine)
        return;

    ImGui_ImplGlfw_CursorPosCallback(window, xpos, ypos);

    // A script REPLACES the mouse, as --pad-script replaces the pad. ImGui still
    // sees the event above, or the debug GUI goes deaf under a script; nothing
    // below it does, or the two pointers fight over one InputState.
    if (engine->input_script || engine_gui_wants_mouse()) {
        return;
    }

    if (!_window_to_fb(engine, xpos, ypos, &xpos, &ypos))
        return;

    _engine_pointer_move(engine, xpos, ypos);
}

static void _engine_pointer_button(Engine* engine, int button, int action, int mods,
                                   double mouse_fb_x, double mouse_fb_y) {
    // A LEFT release always ends the drag and is forwarded, even over the GUI —
    // otherwise a button-up that lands on a panel leaves the camera stuck
    // orbiting. A press is acted on (drag-start, picking) only when the GUI
    // doesn't want the pointer. Either way the app callback fires exactly once.
    // A press or release is where the pointer is, whatever moves were missed: a window shown again
    // under a cursor that moved elsewhere, or moves the GUI took.
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
        engine->input.is_dragging = false;
        engine->input.pointer_released = true;
        engine->input.pointer_fb_x = engine->input.center_fb_x = (float)mouse_fb_x;
        engine->input.pointer_fb_y = engine->input.center_fb_y = (float)mouse_fb_y;
    } else if (engine_gui_wants_mouse()) {
        return;
    } else if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        engine->input.is_dragging = true;
        engine->input.pointer_pressed = true;
        engine->input.press_mods = mods;
        engine->input.pointer_fb_x = (float)mouse_fb_x;
        engine->input.pointer_fb_y = (float)mouse_fb_y;
        engine->input.center_fb_x = (float)mouse_fb_x;
        engine->input.center_fb_y = (float)mouse_fb_y;
        engine->input.drag_fb_x = 0.0f;
        engine->input.drag_fb_y = 0.0f;

        engine->input.selected_node = _engine_pick(engine, mouse_fb_x, mouse_fb_y);
    }

    if (engine->mouse_button_callback) {
        engine->mouse_button_callback(engine, button, action, mods);
    }
}

static void _engine_mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    if (!window)
        return;

    Engine* engine = glfwGetWindowUserPointer(window);
    if (!engine)
        return;

    ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);

    if (engine->input_script)
        return;

    // Not an early return on a zero-area window: it must still be able to end
    // a drag and reach the app's own callback below, at (0, 0).
    double mouse_fb_x = 0.0, mouse_fb_y = 0.0;
    engine_cursor_fb(engine, &mouse_fb_x, &mouse_fb_y);

    _engine_pointer_button(engine, button, action, mods, mouse_fb_x, mouse_fb_y);
}

_Static_assert(INPUT_KEYS == GLFW_KEY_LAST + 1, "InputState.key_pressed takes every GLFW key");
#ifdef __APPLE__
_Static_assert(INPUT_MOD_SHORTCUT == GLFW_MOD_SUPER && INPUT_MOD_WORD == GLFW_MOD_ALT,
               "input.h's modifier bits are GLFW's");
#else
_Static_assert(INPUT_MOD_SHORTCUT == GLFW_MOD_CONTROL && INPUT_MOD_WORD == GLFW_MOD_CONTROL,
               "input.h's modifier bits are GLFW's");
#endif

// One typing event into the frame's list. A batch past the list's size loses the rest, which a
// hand cannot type between two frames; it is said once.
static void _engine_typed(Engine* engine, InputKeyEvent ev) {
    InputState* in = &engine->input;
    if (in->key_event_count >= INPUT_KEY_EVENTS) {
        static bool warned = false;
        if (!warned)
            log_warn("more than %d typing events between two frames: the rest are dropped",
                     INPUT_KEY_EVENTS);
        warned = true;
        return;
    }
    in->key_events[in->key_event_count++] = ev;
}

/*
 * A key and a character, the halves of their GLFW callbacks below that a scripted
 * key reaches as well (spec 14.2), as the pointer's are: everything after ImGui
 * has seen the event. The GUI's text field keeps what it takes from both, or
 * typing a name in a slider also drives the app.
 */
static void _engine_key(Engine* engine, int key, int scancode, int action, int mods) {
    if (engine_gui_wants_keyboard())
        return;

    if (action == GLFW_PRESS && key >= 0 && key < INPUT_KEYS)
        engine->input.key_pressed[key] = true;
    if (action != GLFW_RELEASE && key >= 0)
        _engine_typed(engine, (InputKeyEvent){.key = key, .mods = mods});
    if (engine->key_callback) {
        engine->key_callback(engine, key, scancode, action, mods);
    }
}

static void _engine_char(Engine* engine, unsigned int codepoint) {
    if (!engine_gui_wants_keyboard())
        _engine_typed(engine, (InputKeyEvent){.codepoint = codepoint});
}

static void _engine_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (!window)
        return;

    Engine* engine = glfwGetWindowUserPointer(window);
    if (!engine)
        return;

    ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);
    _engine_key(engine, key, scancode, action, mods);
}

static void _engine_char_callback(GLFWwindow* window, unsigned int codepoint) {
    ImGui_ImplGlfw_CharCallback(window, codepoint);

    Engine* engine = glfwGetWindowUserPointer(window);
    if (engine)
        _engine_char(engine, codepoint);
}

const char* engine_clipboard_get(Engine* engine) {
    if (!engine)
        return "";
    if (engine->headless || !engine->window)
        return engine->clipboard ? engine->clipboard : "";
    const char* text = glfwGetClipboardString(engine->window);
    return text ? text : "";
}

void engine_clipboard_set(Engine* engine, const char* text) {
    if (!engine) {
        log_error("engine_clipboard_set: NULL engine");
        return;
    }
    if (engine->headless || !engine->window) {
        free(engine->clipboard);
        engine->clipboard = safe_strdup(text ? text : "");
        return;
    }
    glfwSetClipboardString(engine->window, text ? text : "");
}

static void _engine_pointer_scroll(Engine* engine, double xoffset, double yoffset) {
    if (engine_gui_wants_mouse())
        return;

    engine->input.scroll_dx += xoffset;
    engine->input.scroll_dy += yoffset;
    if (engine->scroll_callback) {
        engine->scroll_callback(engine, xoffset, yoffset);
    }
}

// Scroll feeds ImGui first; if the GUI isn't using the pointer, it forwards to
// the app (e.g. camera zoom).
static void _engine_scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    ImGui_ImplGlfw_ScrollCallback(window, xoffset, yoffset);

    Engine* engine = glfwGetWindowUserPointer(window);
    if (!engine || engine->input_script)
        return;

    _engine_pointer_scroll(engine, xoffset, yoffset);
}

/*
 * The scripted input (specs 12.19 and 14.2).
 *
 * The gamepad's seam, for the other devices: --pad-script replaces the poll, this
 * replaces the event batch, and both replay a frame-range script through the
 * code a real device drives. It exists because the viewer camera -- the thing
 * four apps move with a drag -- had never been asserted by anything, there being
 * no way to press a mouse from a test; and typing, since 14.2, for the same reason.
 *
 * A range states the pointer's STATE over its frames, as the pad's does, and the
 * EDGES are derived here: `down` appearing is a press and `down` going is a
 * release. `shift` is read at the press only, because that is when GLFW's
 * modifier reaches InputState and a real hand cannot add it mid-drag either.
 * Typing is EVENTS instead, a key being a press and its release in one frame.
 */
#define INPUT_SCRIPT_TYPED 32 // typing events one line may carry

typedef struct InputScriptRange {
    FrameScriptRange range; // first, by frame_script.h's contract
    bool has_at;
    double at_x, at_y; // framebuffer pixels, +Y up: engine_cursor_fb's space
    double by_x, by_y; // added each frame of the range, where `at` is absent
    bool down;
    bool shift;
    double wheel;
    InputKeyEvent typed[INPUT_SCRIPT_TYPED]; // in the line's order
    int typed_count;
    bool close; // asks to close the window as its first frame ends
} InputScriptRange;
_Static_assert(offsetof(InputScriptRange, range) == 0,
               "frame_script.h reads the frame range off the front of this");

typedef struct InputScript {
    InputScriptRange* ranges;
    size_t count;
    int last; // the last frame any range covers; past it every frame is idle
    int frame;
    double x, y;    // where the cursor has been walked to
    bool down;      // as of the previous frame, so a change is an edge
    bool close_due; // the frame playing asks to close the window as it ends
} InputScript;

static const InputScriptRange k_input_idle = {0};

static bool _pointer_pair(const char* value, double* a, double* b) {
    char* end = NULL;
    *a = strtod(value, &end);
    if (end == value || *end != ',')
        return false;
    const char* second = end + 1;
    *b = strtod(second, &end);
    return end != second && *end == '\0';
}

static bool _script_typed(InputScriptRange* r, InputKeyEvent ev, const char* path, int line) {
    if (r->typed_count >= INPUT_SCRIPT_TYPED) {
        log_error("%s:%d: more than %d typing events on one line; spread them over frames", path,
                  line, INPUT_SCRIPT_TYPED);
        return false;
    }
    r->typed[r->typed_count++] = ev;
    return true;
}

// `type=` text: UTF-8, `\s` a space and `\\` a backslash, since a space ends the token.
static bool _script_type(InputScriptRange* r, const char* text, const char* path, int line) {
    for (const char* s = text; *s;) {
        uint32_t cp;
        if (s[0] == '\\') {
            if (s[1] != 's' && s[1] != '\\') {
                log_error("%s:%d: type= knows the escapes \\s and \\\\ only", path, line);
                return false;
            }
            cp = s[1] == 's' ? ' ' : '\\';
            s += 2;
        } else {
            cp = text_utf8_next(&s);
        }
        if (!_script_typed(r, (InputKeyEvent){.codepoint = cp}, path, line))
            return false;
    }
    return true;
}

// A key name to its GLFW code; GLFW_KEY_UNKNOWN for none.
static int _script_key_code(const char* name, size_t n) {
    if (n == 1 && name[0] >= 'a' && name[0] <= 'z')
        return GLFW_KEY_A + (name[0] - 'a');
    if (n == 1 && name[0] >= '0' && name[0] <= '9')
        return GLFW_KEY_0 + (name[0] - '0');
    // f1 to f25.
    if ((n == 2 || n == 3) && name[0] == 'f' && name[1] >= '1' && name[1] <= '9' &&
        (n == 2 || (name[2] >= '0' && name[2] <= '9'))) {
        const int k = n == 2 ? name[1] - '0' : (name[1] - '0') * 10 + (name[2] - '0');
        if (k >= 1 && k <= 25)
            return GLFW_KEY_F1 + k - 1;
    }
    static const struct {
        const char* name;
        int key;
    } named[] = {
        {"left", GLFW_KEY_LEFT},
        {"right", GLFW_KEY_RIGHT},
        {"up", GLFW_KEY_UP},
        {"down", GLFW_KEY_DOWN},
        {"home", GLFW_KEY_HOME},
        {"end", GLFW_KEY_END},
        {"pageup", GLFW_KEY_PAGE_UP},
        {"pagedown", GLFW_KEY_PAGE_DOWN},
        {"backspace", GLFW_KEY_BACKSPACE},
        {"delete", GLFW_KEY_DELETE},
        {"enter", GLFW_KEY_ENTER},
        {"tab", GLFW_KEY_TAB},
        {"escape", GLFW_KEY_ESCAPE},
        {"space", GLFW_KEY_SPACE},
        {"slash", GLFW_KEY_SLASH},
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++)
        if (strlen(named[i].name) == n && strncmp(named[i].name, name, n) == 0)
            return named[i].key;
    return GLFW_KEY_UNKNOWN;
}

// A modifier's GLFW bit; 0 for none. `mod` is the platform's shortcut modifier.
static int _script_mod_bit(const char* name, size_t n) {
    if (n == 5 && strncmp(name, "shift", 5) == 0)
        return GLFW_MOD_SHIFT;
    if (n == 4 && strncmp(name, "ctrl", 4) == 0)
        return GLFW_MOD_CONTROL;
    if (n == 3 && strncmp(name, "alt", 3) == 0)
        return GLFW_MOD_ALT;
    if (n == 5 && strncmp(name, "super", 5) == 0)
        return GLFW_MOD_SUPER;
    if (n == 3 && strncmp(name, "mod", 3) == 0)
        return INPUT_MOD_SHORTCUT;
    return 0;
}

// `key=` with its modifiers before it, joined by `+`: `key=mod+shift+z`.
static bool _script_key(InputScriptRange* r, const char* spec, const char* path, int line) {
    int mods = 0;
    const char* part = spec;
    for (const char* plus = strchr(part, '+'); plus; plus = strchr(part, '+')) {
        const int bit = _script_mod_bit(part, (size_t)(plus - part));
        if (!bit) {
            log_error("%s:%d: key=%s: '%.*s' is not a modifier", path, line, spec,
                      (int)(plus - part), part);
            return false;
        }
        mods |= bit;
        part = plus + 1;
    }
    const int key = _script_key_code(part, strlen(part));
    if (key == GLFW_KEY_UNKNOWN) {
        log_error("%s:%d: key=%s: no key is named '%s'", path, line, spec, part);
        return false;
    }
    return _script_typed(r, (InputKeyEvent){.key = key, .mods = mods}, path, line);
}

static bool _input_token(void* range, const char* tok, const char* path, int line) {
    InputScriptRange* r = range;
    if (strcmp(tok, "idle") == 0)
        return true;
    if (strcmp(tok, "down") == 0) {
        r->down = true;
        return true;
    }
    if (strcmp(tok, "shift") == 0) {
        r->shift = true;
        return true;
    }
    if (strcmp(tok, "close") == 0) {
        r->close = true;
        return true;
    }
    const char* eq = strchr(tok, '=');
    if (eq) {
        size_t n = (size_t)(eq - tok);
        if (n == 2 && strncmp(tok, "at", 2) == 0) {
            if (!_pointer_pair(eq + 1, &r->at_x, &r->at_y)) {
                log_error("%s:%d: '%s' wants at=x,y in framebuffer pixels", path, line, tok);
                return false;
            }
            r->has_at = true;
            return true;
        }
        if (n == 2 && strncmp(tok, "by", 2) == 0) {
            if (!_pointer_pair(eq + 1, &r->by_x, &r->by_y)) {
                log_error("%s:%d: '%s' wants by=dx,dy in framebuffer pixels", path, line, tok);
                return false;
            }
            return true;
        }
        if (n == 5 && strncmp(tok, "wheel", 5) == 0) {
            char* end = NULL;
            r->wheel = strtod(eq + 1, &end);
            if (end == eq + 1 || *end != '\0') {
                log_error("%s:%d: wheel '%s' has no number", path, line, tok);
                return false;
            }
            return true;
        }
        if (n == 4 && strncmp(tok, "type", 4) == 0)
            return _script_type(r, eq + 1, path, line);
        if (n == 3 && strncmp(tok, "key", 3) == 0)
            return _script_key(r, eq + 1, path, line);
    }
    log_error("%s:%d: unknown token '%s'", path, line, tok);
    return false;
}

// One frame of the script, in the order a hand produces: the cursor moves,
// then the button changes under it, then the wheel turns, then the typing.
static void _engine_input_script_frame(Engine* engine) {
    InputScript* s = engine->input_script;
    if (!s)
        return;
    const int frame = s->frame++;
    const InputScriptRange* r =
        frame_script_at(s->ranges, s->count, sizeof(*r), &k_input_idle, frame);
    s->close_due = r->close && frame == r->range.from;

    double x = r->has_at ? r->at_x : s->x + r->by_x;
    double y = r->has_at ? r->at_y : s->y + r->by_y;
    if (x != s->x || y != s->y) {
        s->x = x;
        s->y = y;
        _engine_pointer_move(engine, x, y);
    }

    if (r->down != s->down) {
        s->down = r->down;
        _engine_pointer_button(engine, GLFW_MOUSE_BUTTON_LEFT, r->down ? GLFW_PRESS : GLFW_RELEASE,
                               r->shift ? GLFW_MOD_SHIFT : 0, x, y);
    }

    if (r->wheel != 0.0)
        _engine_pointer_scroll(engine, 0.0, r->wheel);

    for (int i = 0; i < r->typed_count; i++) {
        const InputKeyEvent* ev = &r->typed[i];
        if (ev->key) {
            _engine_key(engine, ev->key, 0, GLFW_PRESS, ev->mods);
            _engine_key(engine, ev->key, 0, GLFW_RELEASE, ev->mods);
        } else {
            _engine_char(engine, ev->codepoint);
        }
    }
}

// A `close` the frame playing holds, asked as the frame ends, as the close button would ask it:
// the window's flag set first, as GLFW sets it. Not asked when the app ended the run in this frame
// already, which a refusal must not undo.
static void _engine_input_script_close(Engine* engine) {
    InputScript* s = engine->input_script;
    if (!s || !s->close_due || glfwWindowShouldClose(engine->window))
        return;
    s->close_due = false;
    glfwSetWindowShouldClose(engine->window, GLFW_TRUE);
    _engine_close_asked(engine);
}

// True while the script has a frame left to play: it is input, and idle mode draws for it.
static bool _engine_input_script_running(const Engine* engine) {
    const InputScript* s = engine->input_script;
    return s && s->frame <= s->last;
}

static bool _engine_input_script_cursor(const Engine* engine, double* fb_x, double* fb_y) {
    const InputScript* s = engine->input_script;
    if (!s)
        return false;
    *fb_x = s->x;
    *fb_y = s->y;
    return true;
}

static void _engine_input_script_free(Engine* engine) {
    InputScript* s = engine->input_script;
    if (!s)
        return;
    free(s->ranges);
    free(s);
    engine->input_script = NULL;
}

bool engine_set_input_script(Engine* engine, const char* path) {
    if (!engine || !path) {
        log_error("engine_set_input_script: NULL engine or path");
        return false;
    }
    char* text = read_entire_file(path, NULL);
    if (!text) {
        log_error("input script '%s': cannot read", path);
        return false;
    }
    InputScript* s = calloc(1, sizeof(InputScript));
    if (!s) {
        free(text);
        log_error("input script '%s': out of memory", path);
        return false;
    }
    bool ok = frame_script_parse(text, path, sizeof(InputScriptRange), &k_input_idle, _input_token,
                                 (void**)&s->ranges, &s->count);
    free(text);
    if (!ok) {
        free(s->ranges);
        free(s);
        return false;
    }
    s->last = frame_script_last(s->ranges, s->count, sizeof(*s->ranges));
    _engine_input_script_free(engine);
    engine->input_script = s;
    log_info("input script '%s': %zu ranges", path, s->count);
    return true;
}

/*
 * The views' cameras
 */
static void _fit_add_box(const AABB* box, void* fit) {
    camera_clip_fit_add(fit, box);
}

// How far a sea with no bounds is fitted out, x the typical size: the reach a camera has by
// default, since such a sea runs on to the horizon wherever the far plane is put.
#define SEA_REACH 1000.0f

// A fit_clip camera's planes, from what the view draws at rest: the scene's mesh boxes, and the
// shadow catcher's quad or the water's rectangle when one draws -- a sea with no bounds as a
// square SEA_REACH out from under the eye.
static void _engine_fit_clip(const Engine* engine, SceneView* view) {
    const Scene* scene = engine_get_scene(engine);
    if (!scene)
        return;
    Camera* camera = &view->camera;
    CameraClipFit fit;
    camera_clip_fit_begin(&fit, camera);
    scene_for_each_mesh_box(scene, _fit_add_box, &fit);
    AABB box;
    if (water_will_draw(scene->water, view)) {
        if (!water_surface_box(scene->water, &box)) {
            const float r = SEA_REACH * engine->typical_size;
            const float* eye = camera->position;
            box = (AABB){{eye[0] - r, scene->water->level, eye[2] - r},
                         {eye[0] + r, scene->water->level, eye[2] + r}};
        }
        camera_clip_fit_add(&fit, &box);
    } else if (render_shadow_catcher_box(engine, scene, view, &box)) {
        camera_clip_fit_add(&fit, &box);
    }
    camera_clip_fit_apply(camera, &fit);
}

// The view and projection matrices, and the camera's aspect, from the view's camera and its
// rectangle as they stand -- and a fitted camera's planes, which read the aspect.
static void _engine_derive_view(const Engine* engine, SceneView* view) {
    SceneViewState* vs = view->state;
    Camera* camera = &view->camera;
    camera_view_matrix(camera, vs->view_matrix);

    // A minimized window is 0x0, and 0/0 is a NaN that would propagate through
    // the projection into the view-proj, the frustum, and next frame's
    // reprojection. Keep the last good aspect until the view has an area again.
    int w, h;
    scene_view_out_size(view, &w, &h);
    if (w <= 0 || h <= 0)
        return;

    camera->aspect_ratio = (float)w / (float)h;
    if (camera->fit_clip)
        _engine_fit_clip(engine, view);
    camera_projection_matrix(camera, vs->projection_matrix);
}

// The mode a view renders with this frame: albedo under the wireframe overlay, which is for speed
// and for every edge of every face, and its settings' otherwise.
static RenderMode _view_frame_mode(const SceneView* view) {
    return view->settings.show_wireframe ? RENDER_MODE_ALBEDO : view->settings.render_mode;
}

// Whether a view can draw: shown, with an area, and with targets built for its size.
static bool _view_drawable(const SceneView* view) {
    const SceneViewState* vs = view->state;
    int w, h;
    scene_view_out_size(view, &w, &h);
    return !view->hidden && vs->postfx && !vs->render_suspended && w > 0 && h > 0;
}

SceneView* engine_view_at(const Engine* engine, double fb_x, double fb_y) {
    if (!engine)
        return NULL;
    for (size_t i = engine->view_count; i-- > 0;) {
        SceneView* view = engine->views[i];
        if (view->hidden)
            continue;
        float point[2];
        scene_view_point_from_fb(view, fb_x, fb_y, point);
        if (scene_view_contains(view, point))
            return view;
    }
    return NULL;
}

/*
 * Textures from files, decoded on the loader's threads
 */
typedef struct MaterialTextureLoad {
    Material* material;
    void (*set)(Material*, Texture*);
} MaterialTextureLoad;

static void _material_texture_loaded(Texture* texture, void* user) {
    MaterialTextureLoad* load = user;
    if (texture)
        load->set(load->material, texture);
    free(load);
}

void engine_load_material_texture(Engine* engine, TexturePool* pool, Material* material,
                                  void (*set)(Material*, Texture*), const char* path,
                                  TextureDesc desc) {
    if (!engine || !engine->async_loader || !pool || !material || !set || !path) {
        log_error("engine_load_material_texture: missing an argument");
        return;
    }
    MaterialTextureLoad* load = malloc(sizeof(MaterialTextureLoad));
    if (!load) {
        log_error("engine_load_material_texture: out of memory for '%s'", path);
        return;
    }
    *load = (MaterialTextureLoad){material, set};
    engine_load_texture(engine, pool, path, desc, _material_texture_loaded, load);
}

void engine_load_texture(Engine* engine, TexturePool* pool, const char* path, TextureDesc desc,
                         void (*got)(Texture* tex, void* user), void* user) {
    if (!engine || !engine->async_loader || !pool || !got || !path) {
        log_error("engine_load_texture: missing an argument");
        return;
    }
    load_texture_async(engine->async_loader, pool, path, desc, got, user);
}

void engine_finish_texture_loads(Engine* engine) {
    if (!engine || !engine->async_loader) {
        log_error("engine_finish_texture_loads: no engine or no loader");
        return;
    }
    // One upload at a time, so the screen can draw between them: an upload builds its mips and
    // compresses on this thread, and a batch of them would hold the screen still.
    while (async_loader_is_busy(engine->async_loader)) {
        if (async_loader_process_pending(engine->async_loader, 1) == 0)
            cetra_sleep_ms(1);
        engine_draw_loading_screen(engine);
    }
}

/*
 * Scene
 *
 */
void engine_add_scene(Engine* engine, Scene* scene) {
    if (!engine || !scene)
        return;

    // check if scene already added to engine and return if so
    for (size_t i = 0; i < engine->scene_count; ++i) {
        if (engine->scenes[i] == scene) {
            return; // already added
        }
    }

    // Reallocate the scenes array to accommodate the new scene
    size_t new_count = engine->scene_count + 1;
    Scene** new_scenes = realloc(engine->scenes, new_count * sizeof(Scene*));
    if (!new_scenes) {
        log_error("Failed to reallocate memory for new scene");
        return;
    }

    // Add the new scene to the array and update the scene count
    engine->scenes = new_scenes;
    engine->scenes[engine->scene_count] = scene;
    engine->scene_count = new_count;
}

void engine_set_scene_by_index(Engine* engine, size_t scene_index) {
    if (!engine)
        return;

    if (scene_index < engine->scene_count) {
        engine->current_scene_index = scene_index;
    } else {
        log_error("Scene index %zu is out of bounds. The engine has %zu scenes.", scene_index,
                  engine->scene_count);
    }
}

void engine_set_scene_by_name(Engine* engine, const char* scene_name) {
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

Scene* engine_get_scene(const Engine* engine) {
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

    engine_add_program(engine, pbr_shader_program);

    ShaderProgram* shape_shader_program = NULL;

    if ((shape_shader_program = create_shape_program()) == NULL) {
        log_error("Failed to create shape shader program");
        return -1;
    }

    engine_add_program(engine, shape_shader_program);

    ShaderProgram* xyz_shader_program = NULL;

    if ((xyz_shader_program = create_xyz_program()) == NULL) {
        log_error("Failed to create xyz shader program");
        return -1;
    }

    engine_add_program(engine, xyz_shader_program);

    ShaderProgram* shadow_depth_program = NULL;

    if ((shadow_depth_program = create_shadow_depth_program()) == NULL) {
        log_error("Failed to create shadow depth shader program");
        return -1;
    }

    engine_add_program(engine, shadow_depth_program);

    // Not fatal if it fails: --msm falls back to the depth cascades, which are
    // rendered either way.
    ShaderProgram* msm_resolve_program = create_msm_resolve_program();
    if (msm_resolve_program) {
        engine_add_program(engine, msm_resolve_program);
    }

    // Same contract as the moment resolve above: if either fails to compile,
    // shadow_build_tsm finds no program and leaves tsm_built false, which is
    // the feature off rather than a broken frame.
    ShaderProgram* shadow_absorb_program = create_shadow_absorb_program();
    if (shadow_absorb_program) {
        engine_add_program(engine, shadow_absorb_program);
    }
    ShaderProgram* tsm_resolve_program = create_tsm_resolve_program();
    if (tsm_resolve_program) {
        engine_add_program(engine, tsm_resolve_program);
    }

    // IBL Programs
    ShaderProgram* skybox_program = create_skybox_program();
    if (skybox_program) {
        engine_add_program(engine, skybox_program);
    }

    ShaderProgram* ibl_equirect_program = create_ibl_equirect_to_cube_program();
    if (ibl_equirect_program) {
        engine_add_program(engine, ibl_equirect_program);
    }

    ShaderProgram* ibl_irradiance_program = create_ibl_irradiance_program();
    if (ibl_irradiance_program) {
        engine_add_program(engine, ibl_irradiance_program);
    }

    ShaderProgram* ibl_prefilter_program = create_ibl_prefilter_program();
    if (ibl_prefilter_program) {
        engine_add_program(engine, ibl_prefilter_program);
    }

    ShaderProgram* ibl_charlie_prefilter_program = create_ibl_charlie_prefilter_program();
    if (ibl_charlie_prefilter_program) {
        engine_add_program(engine, ibl_charlie_prefilter_program);
    }

    ShaderProgram* ibl_brdf_program = create_ibl_brdf_program();
    if (ibl_brdf_program) {
        engine_add_program(engine, ibl_brdf_program);
    }

    // Sky atmosphere LUT programs
    ShaderProgram* sky_transmittance_program = create_sky_transmittance_program();
    if (sky_transmittance_program) {
        engine_add_program(engine, sky_transmittance_program);
    }

    ShaderProgram* sky_multiscatter_program = create_sky_multiscatter_program();
    if (sky_multiscatter_program) {
        engine_add_program(engine, sky_multiscatter_program);
    }

    ShaderProgram* sky_debug_program = create_sky_debug_program();
    if (sky_debug_program) {
        engine_add_program(engine, sky_debug_program);
    }

    ShaderProgram* sky_view_program = create_sky_view_program();
    if (sky_view_program) {
        engine_add_program(engine, sky_view_program);
    }

    ShaderProgram* sky_env_program = create_sky_env_program();
    if (sky_env_program) {
        engine_add_program(engine, sky_env_program);
    }

    ShaderProgram* sky_background_program = create_sky_background_program();
    if (sky_background_program) {
        engine_add_program(engine, sky_background_program);
    }

    ShaderProgram* sky_aerial_program = create_sky_aerial_program();
    if (sky_aerial_program) {
        engine_add_program(engine, sky_aerial_program);
    }

    ShaderProgram* cloud_noise_debug_program = create_cloud_noise_debug_program();
    if (cloud_noise_debug_program) {
        engine_add_program(engine, cloud_noise_debug_program);
    }

    ShaderProgram* cloud_march_program = create_cloud_march_program();
    if (cloud_march_program) {
        engine_add_program(engine, cloud_march_program);
    }

    ShaderProgram* cloud_shadow_program = create_cloud_shadow_program();
    if (cloud_shadow_program) {
        engine_add_program(engine, cloud_shadow_program);
    }

    ShaderProgram* sky_background_clouds_program = create_sky_background_clouds_program();
    if (sky_background_clouds_program) {
        engine_add_program(engine, sky_background_clouds_program);
    }

    ShaderProgram* sky_env_clouds_program = create_sky_env_clouds_program();
    if (sky_env_clouds_program) {
        engine_add_program(engine, sky_env_clouds_program);
    }

    ShaderProgram* mask_copy_program = create_mask_copy_program();
    if (mask_copy_program) {
        engine_add_program(engine, mask_copy_program);
    }

    ShaderProgram* layers_vt_bake_program = create_layers_vt_bake_program();
    if (layers_vt_bake_program) {
        engine_add_program(engine, layers_vt_bake_program);
    }

    ShaderProgram* layers_vt_feedback_program = create_layers_vt_feedback_program();
    if (layers_vt_feedback_program) {
        engine_add_program(engine, layers_vt_feedback_program);
    }

    ShaderProgram* water_program = create_water_program();
    if (water_program) {
        engine_add_program(engine, water_program);
    }

    ShaderProgram* rain_program = create_rain_program();
    if (rain_program) {
        engine_add_program(engine, rain_program);
    }

    ShaderProgram* water_spectrum_program = create_water_spectrum_program();
    if (water_spectrum_program) {
        engine_add_program(engine, water_spectrum_program);
    }

    ShaderProgram* water_fft_program = create_water_fft_program();
    if (water_fft_program) {
        engine_add_program(engine, water_fft_program);
    }

    ShaderProgram* water_foam_program = create_water_foam_program();
    if (water_foam_program) {
        engine_add_program(engine, water_foam_program);
    }
    ShaderProgram* water_probe_program = create_water_probe_program();
    if (water_probe_program) {
        engine_add_program(engine, water_probe_program);
    }
    ShaderProgram* water_caustic_land_program = create_water_caustic_land_program();
    if (water_caustic_land_program) {
        engine_add_program(engine, water_caustic_land_program);
    }
    ShaderProgram* water_caustic_program = create_water_caustic_program();
    if (water_caustic_program) {
        engine_add_program(engine, water_caustic_program);
    }
    ShaderProgram* water_touch_program = create_water_touch_program();
    if (water_touch_program) {
        engine_add_program(engine, water_touch_program);
    }
    ShaderProgram* water_touch_normals_program = create_water_touch_normals_program();
    if (water_touch_normals_program) {
        engine_add_program(engine, water_touch_normals_program);
    }

    // GI probe volume. Lives on the engine rather than on PostFX -- despite
    // being a fullscreen pass -- because the volume is a Scene citizen and the
    // atlas is consumed by the scene pass, not by post.
    ShaderProgram* gi_project_program = create_gi_project_program();
    if (gi_project_program) {
        engine_add_program(engine, gi_project_program);
    }

    // Specular probe projection (spec 11.70), here for the same reason: the
    // atlas it writes is read by the scene pass.
    ShaderProgram* probe_project_program = create_probe_project_program();
    if (probe_project_program) {
        engine_add_program(engine, probe_project_program);
    }

    return 0;
}

void engine_add_program(Engine* engine, ShaderProgram* program) {
    if (!engine || !program) {
        log_error("Invalid input to engine_add_program");
        return;
    }

    // check if program already added to engine and return if so
    for (size_t i = 0; i < engine->program_count; ++i) {
        if (engine->programs[i] == program) {
            return; // already added
        }
    }

    // Resize the programs array to accommodate the new program
    size_t new_count = engine->program_count + 1;
    ShaderProgram** new_programs = realloc(engine->programs, new_count * sizeof(ShaderProgram*));

    if (!new_programs) {
        log_error("Failed to allocate memory for new program");
        return;
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
}

bool engine_add_shader_hook(Engine* engine, ShaderHook* hook) {
    ShaderHook** grown =
        realloc(engine->shader_hooks, (engine->shader_hook_count + 1) * sizeof(ShaderHook*));
    if (!grown) {
        log_error("engine_add_shader_hook: no memory to hold '%s'", hook->name);
        return false;
    }
    engine->shader_hooks = grown;
    engine->shader_hooks[engine->shader_hook_count++] = hook;
    return true;
}

ShaderProgram* engine_find_program(Engine* engine, const char* program_name) {
    if (!engine || !program_name)
        return NULL;
    ShaderProgram* existing;
    HASH_FIND_STR(engine->program_map, program_name, existing);
    return existing;
}

ShaderProgram* engine_get_program(Engine* engine, const char* program_name) {
    if (!engine || !program_name) {
        log_error("engine_get_program: NULL engine or name");
        return NULL;
    }
    ShaderProgram* program = engine_find_program(engine, program_name);
    if (!program)
        log_error("no program '%s' (%zu registered)", program_name, engine->program_count);
    return program;
}

ShaderProgram* engine_pbr_variant(Engine* engine, PbrFamily family, unsigned features,
                                  const ShaderHook* hook) {
    if (!engine)
        return NULL;

    char name[PBR_VARIANT_NAME_MAX];
    pbr_variant_name(family, features, hook, name, sizeof(name));

    ShaderProgram* program = engine_find_program(engine, name);
    if (program)
        return program;
    for (size_t i = 0; i < engine->failed_variant_count; ++i) {
        if (strcmp(engine->failed_variants[i], name) == 0)
            return NULL;
    }

    program = create_pbr_program_variant(family, features, hook);
    // A compile is the long part of a first frame, so a loading screen moves between them.
    loading_screen_tick(engine);
    if (program) {
        engine_add_program(engine, program);
        return program;
    }
    // Said once, here, where the name is: a resolver asks every frame, and a variant that did
    // not build will not build next frame either.
    if (hook) {
        log_error("lit-surface variant %s does not build; surface hook '%s' is dropped, and its "
                  "materials draw without it",
                  name, hook->name);
    } else {
        log_error("lit-surface variant %s does not build; materials that want it stay on the "
                  "programs they have",
                  name);
    }
    // A hook kept for its bounds and its shadow over a surface drawn without it would put the
    // three at odds, so a hook that fails one variant is let go by every reader.
    for (size_t i = 0; hook && i < engine->shader_hook_count; ++i) {
        if (engine->shader_hooks[i] == hook)
            engine->shader_hooks[i]->broken = true;
    }
    char* kept = safe_strdup(name);
    char** grown =
        kept ? realloc(engine->failed_variants, (engine->failed_variant_count + 1) * sizeof(char*))
             : NULL;
    if (grown) {
        engine->failed_variants = grown;
        engine->failed_variants[engine->failed_variant_count++] = kept;
    } else {
        free(kept);
    }
    return NULL;
}

Engine* create_engine(const EngineConfig* cfg) {
    static const EngineConfig none = {0};
    if (!cfg)
        cfg = &none;

    Engine* engine = _engine_alloc(cfg);
    if (!engine)
        return NULL;

    if (_engine_init(engine, cfg) != 0) {
        log_error("create_engine: the window or the GL context could not be made");
        free_engine(engine);
        return NULL;
    }
    if (cfg->typical_size != 0.0f)
        engine_set_typical_size(engine, cfg->typical_size);
    return engine;
}

void engine_set_typical_size(Engine* engine, float metres) {
    if (!engine)
        return;
    if (!(metres > 0.0f && isfinite(metres))) {
        log_warn("engine_set_typical_size: %g is not a positive length in metres; kept %g",
                 (double)metres, (double)engine->typical_size);
        return;
    }
    engine->typical_size = metres;
    for (size_t i = 0; i < engine->view_count; i++) {
        SceneView* view = engine->views[i];
        postfx_settings_reset_lengths(&view->settings.post, metres);
        if (view->rig)
            view->rig->typical_size = metres;
    }
    engine_mark_changed(engine);
}

void engine_mark_changed(Engine* engine) {
    if (engine)
        engine->change_count++;
}

void engine_set_screenshot_path(Engine* engine, const char* path) {
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

// What the engine draws after the temporal seam, published only on frames that have
// something to draw. The rain's renderer comes into being the first frame a scene rains; the
// fire's already exists, since its simulation ran at the frame top. Fire first, so rain falling
// in front of a fire draws over it and refracts it. `user` is the view whose chain is running.
static void _engine_late_draw(void* user, const PostFXLateDraw* late) {
    SceneView* view = user;
    Engine* engine = view->state->engine;
    const Scene* scene = engine_get_scene(engine);
    // An app's late-draw surfaces first (spec 13.29): fire and rain are translucent and composite
    // over whatever is behind them, a screen included.
    render_late_items(engine, scene, view, late);
    if (scene && fire_system_active(scene->fire))
        fire_render_draw(engine->fire_renderer, engine, scene, view, late);
    if (scene && rain_draws(scene->rain)) {
        if (!engine->rain_renderer)
            engine->rain_renderer = create_rain_renderer();
        rain_render_drops(engine->rain_renderer, engine, scene, view, late);
    }
}

static int _engine_find_overlay(const Engine* engine, EngineOverlayFunc draw, const void* user) {
    for (int i = 0; i < engine->overlay_count; i++)
        if (engine->overlays[i].draw == draw && engine->overlays[i].user == user)
            return i;
    return -1;
}

bool engine_add_overlay(Engine* engine, EngineOverlayFunc draw, void* user, int order) {
    if (!engine || !draw) {
        log_error("engine_add_overlay: no engine or no draw");
        return false;
    }
    if (_engine_find_overlay(engine, draw, user) >= 0) {
        log_error("engine_add_overlay: this draw and user are already added");
        return false;
    }
    if (engine->overlay_count == ENGINE_OVERLAY_MAX) {
        log_error("engine_add_overlay: all %d overlays are taken", ENGINE_OVERLAY_MAX);
        return false;
    }
    // After every entry of the same order, so equal orders draw in the order they were added.
    int at = engine->overlay_count;
    while (at > 0 && engine->overlays[at - 1].order > order) {
        engine->overlays[at] = engine->overlays[at - 1];
        at--;
    }
    engine->overlays[at] = (EngineOverlay){.draw = draw, .user = user, .order = order};
    engine->overlay_count++;
    return true;
}

void engine_remove_overlay(Engine* engine, EngineOverlayFunc draw, const void* user) {
    if (!engine)
        return;
    int at = _engine_find_overlay(engine, draw, user);
    if (at < 0)
        return;
    engine->overlay_count--;
    for (int i = at; i < engine->overlay_count; i++)
        engine->overlays[i] = engine->overlays[i + 1];
}

void engine_set_render_time(Engine* engine, double time, double delta) {
    if (!engine)
        return;
    engine->render_time = time;
    engine->render_delta = delta;
}

// Whether the CRT shows this frame, making it on first use.
static bool _engine_crt_on(Engine* engine) {
    if (!engine->crt_enabled || engine->crt_failed)
        return false;
    if (!engine->crt)
        engine->crt = create_crt();
    if (!engine->crt)
        engine->crt_failed = true;
    return engine->crt != NULL;
}

// `picture` texel for texel into the bound framebuffer with its corner at `origin`, through the
// present pass: dithered by `strength` when this is the window's last write.
static void _engine_present_picture(const Engine* engine, const GLColorTarget* picture,
                                    const int origin[2], bool dither, float strength) {
    ShaderProgram* present = engine->postfx_programs->present;
    glViewport(origin[0], origin[1], picture->w, picture->h);
    glUseProgram(present->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, picture->tex);
    uniform_set_vec2(present->uniforms, "pictureOrigin",
                     (const float[]){(float)origin[0], (float)origin[1]});
    uniform_set_int(present->uniforms, "ditherEnabled", dither ? 1 : 0);
    uniform_set_float(present->uniforms, "ditherStrength", strength);
    draw_fullscreen_quad(engine->postfx_programs->quad_vao);
    glUseProgram(0);
}

// The window's last write, when the window is drawn this frame through its picture: the picture
// through the CRT, or as it is through the present pass. That write is the 8-bit one, so it is
// the one that dithers (spec 11.24).
static void _engine_present_window(Engine* engine) {
    const GLColorTarget* picture = &engine->window_picture;
    // One dither for the whole window, the focus view's: it is the window's last write.
    const SceneView* focus = engine->focus_view;
    const bool dither = focus && focus->settings.post.dither_enabled;
    const float dither_strength = focus ? focus->settings.post.dither_strength : 0.0f;
    if (_engine_crt_on(engine)) {
        const CrtLook look = {.lines = engine->crt_lines,
                              .scanlines = engine->crt_scanlines,
                              .mask = engine->crt_mask,
                              .curvature = engine->crt_curvature,
                              .bleed = engine->crt_bleed,
                              .dither = dither,
                              .dither_strength = dither_strength};
        profiler_scope_begin(engine->profiler, "crt");
        crt_present(engine->crt, picture->tex, picture->w, picture->h,
                    engine->postfx_programs->quad_vao, &look);
        profiler_scope_end(engine->profiler);
        return;
    }
    profiler_scope_begin(engine->profiler, "present");
    const GLPassState pass = gl_pass_begin();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    _engine_present_picture(engine, picture, (const int[]){0, 0}, dither, dither_strength);
    gl_pass_end(&pass);
    profiler_scope_end(engine->profiler);
}

// Whether the window is drawn through its picture this frame: the CRT is on, or a view that draws
// would run passes after its tone map. Settled before any view's chain runs, since it decides the
// format every view's picture takes.
static bool _engine_window_through_picture(Engine* engine) {
    if (_engine_crt_on(engine))
        return true;
    for (size_t i = 0; i < engine->view_count; i++) {
        const SceneView* view = engine->views[i];
        if (_view_drawable(view) &&
            postfx_draws_after_tone_map(view->state->postfx,
                                        view->settings.render_mode == RENDER_MODE_PBR))
            return true;
    }
    return false;
}

// The framebuffer the window's frame is composed in: the window's picture when it is drawn
// through one, at the framebuffer's size, else the window. 0 also when the picture cannot be
// made, the window then taking what would have gone into it, undithered.
static GLuint _engine_window_target(Engine* engine) {
    if (!engine->window_through_picture)
        return 0;
    GLColorTarget* picture = &engine->window_picture;
    return gl_color_target_ensure(picture, engine->fb_width, engine->fb_height, "window picture")
               ? picture->fbo
               : 0;
}

// A view's finished picture into its rectangle in the bound framebuffer, as it is.
static void _engine_copy_view(const Engine* engine, const SceneView* view) {
    const SceneViewState* vs = view->state;
    const PostFX* fx = vs->postfx;
    if (!fx || !fx->picture.fbo)
        return;
    // A young average is shown as the TAA picture it began from, until it holds enough frames.
    const GLColorTarget* picture =
        vs->held_valid && view->frames_accumulated < VIEW_AVERAGE_SHOWN ? &vs->held : &fx->picture;
    _engine_present_picture(engine, picture, vs->picture_rect, false, 0.0f);
}

// A view's post chain, from its scene target into its picture, after what the
// current scene publishes to it. Only PBR frames are linear HDR and get SSAO + bloom + exposure +
// tone mapping; debug render modes emit display-ready colors and are copied unchanged.
static void _engine_run_view_chain(const Engine* engine, SceneView* view) {
    SceneViewState* vs = view->state;
    PostFX* fx = vs->postfx;
    const RenderMode frame_mode = vs->render_mode;
    // The view's own frame count: the grain's seed, so it animates live yet stays deterministic
    // across equal --frames runs, and every history's parity, so one that rested reads the side
    // it last wrote.
    fx->frame_index = (int)vs->draw_index;
    fx->time = (float)engine->render_time;
    fx->window_through_picture = engine->window_through_picture;
    fx->average_n = vs->average_n;
    // Depth-of-field autofocus: keep the subject sharp as the camera orbits or
    // zooms by refocusing on the point the camera looks at (the orbit target),
    // unless a manual focus distance was pinned.
    fx->dof_autofocus_distance = glm_vec3_distance(view->camera.position, view->camera.look_at);
    // Hand the current scene's reflection probes and shadow casters to postfx
    // (SSR miss fallback / fog march) without postfx learning about Scene
    const Scene* fx_scene = engine_get_scene(engine);
    fx->sss_profiles = fx_scene ? (const vec4*)fx_scene->sss_profiles : NULL;
    fx->sss_profile_count = fx_scene ? fx_scene->sss_profile_count : 0;
    probe_set_publish_to_postfx(fx_scene ? fx_scene->probe_set : NULL,
                                fx_scene ? fx_scene->lighting_atlas : NULL, fx);
    shadow_publish_to_postfx(fx_scene, fx);
    rain_publish_to_postfx(fx_scene ? fx_scene->rain : NULL, fx);
    fx->late_draw = fx_scene && (rain_draws(fx_scene->rain) || fire_system_active(fx_scene->fire) ||
                                 render_has_late_items(fx_scene))
                        ? _engine_late_draw
                        : NULL;
    fx->late_draw_user = view;
    // Aerial perspective is a camera-frustum volume, so unlike the sky's other
    // LUTs it is rebuilt here, for each view, immediately before it is published
    // to that view's chain. The unjittered projection: the bake reads only
    // [0][0]/[1][1]/[2][2]/[3][2] so TAA's jitter in [2][0]/[2][1] cannot reach
    // it, and handing it an input that churns every frame would defeat any
    // future rebuild-elision.
    if (fx_scene && fx_scene->sky)
        sky_update_aerial(fx_scene->sky, vs->view_matrix, vs->projection_matrix);
    sky_publish_to_postfx(fx_scene ? fx_scene->sky : NULL, fx);
    // Order-independent of the sky's: water publishes a suppression REQUEST rather than
    // clearing the aerial volume the line above filled, so neither publish can undo the
    // other. Gated on the surface actually drawing -- a debug mode that skips the water
    // pass must not leave a medium behind for the volume to integrate.
    water_publish_to_postfx(fx_scene && water_will_draw(fx_scene->water, view) ? fx_scene->water
                                                                               : NULL,
                            fx_scene, view);
    // Unconditional, including with no scene: count 0 is the off state, and only a
    // publish every frame can reach it after a scene that had volumes goes away.
    scene_publish_fog_volumes_to_postfx(fx_scene, fx);
    const PostFXGBufferWrites writes = {
        .normals = vs->normals_this_frame,
        .aux = vs->aux_this_frame,
        .albedo = vs->albedo_this_frame,
        .sss = vs->sss_this_frame,
        .spec = vs->spec_this_frame,
        .oit_fbo = vs->oit_this_frame ? vs->oit_fbo : 0,
        .oit_moment_atlas = vs->moments_this_frame ? vs->moment_atlas_texture : 0,
        .oit_near_far = {view->camera.near_clip, view->camera.far_clip}};
    postfx_run(fx, vs->framebuffer, frame_mode == RENDER_MODE_PBR, &writes, vs->draw_projection,
               vs->view_matrix);
}

// The window's part of the frame, once the views have drawn: cleared to its colour, each shown
// view's picture copied into its rectangle in list order, the debug views, the app's overlays, the
// CRT or the present pass when the window was drawn through its picture, the loading screen and
// the GUI. All draw after tone mapping, and the GUI is never tone mapped.
static void _engine_present_frame(Engine* engine) {
    // Under a loading screen that hides the whole frame (spec 13.34) the window is the screen's
    // alone: no picture under it, and no GUI over it, which a frame drawn between two of this
    // frame's pieces would show and hide by turns.
    if (loading_screen_covers(engine)) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        loading_screen_frame(engine);
        gui_discard_frame(engine);
        return;
    }

    // Everything up to the overlay draws into the window, or into the picture
    // _engine_present_window shows after them.
    const GLuint target = _engine_window_target(engine);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, engine->fb_width, engine->fb_height);
    glClearColor(engine->window_color[0], engine->window_color[1], engine->window_color[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const GLPassState pass = gl_pass_begin();
    for (size_t i = 0; i < engine->view_count; i++)
        if (_view_drawable(engine->views[i]))
            _engine_copy_view(engine, engine->views[i]);
    gl_pass_end(&pass);
    glViewport(0, 0, engine->fb_width, engine->fb_height);

    const Scene* fx_scene = engine_get_scene(engine);
    // Sky LUT debug overlay onto the composited frame (an acceptance tool,
    // the csm_debug shape: a library-side flag the app/GUI toggles)
    if (fx_scene && fx_scene->sky && fx_scene->sky->debug_luts) {
        sky_debug_blit_luts(fx_scene->sky, engine->fb_width, engine->fb_height);
    }
    // One atlas holds both, so one overlay shows both. The GI tiles hold bounced light, a
    // fraction of the direct, and are lifted 4x when they are what was asked for.
    if (fx_scene && fx_scene->gi && fx_scene->gi->debug_atlas) {
        lighting_atlas_debug_blit(fx_scene->lighting_atlas, engine, engine->fb_width,
                                  engine->fb_height, 4.0f);
    } else if (fx_scene && fx_scene->probe_set && fx_scene->probe_set->debug_atlas) {
        lighting_atlas_debug_blit(fx_scene->lighting_atlas, engine, engine->fb_width,
                                  engine->fb_height, 1.0f);
    }
    if (fx_scene && fx_scene->fire && fx_scene->fire->debug_field >= 0)
        fire_render_slice(engine->fire_renderer, fx_scene->fire, engine->fb_height);

    // The app's overlays, then the GUI: the debug panel goes over the game's own
    // screens rather than under them. Walked over a copy, so an overlay may take
    // itself or another off as it draws; one taken off before its turn is not
    // drawn, since its user may be gone with it.
    if (engine->overlay_count > 0) {
        EngineOverlay overlays[ENGINE_OVERLAY_MAX];
        int count = engine->overlay_count;
        memcpy(overlays, engine->overlays, sizeof(overlays[0]) * (size_t)count);
        profiler_scope_begin(engine->profiler, "overlay");
        for (int i = 0; i < count; i++)
            if (_engine_find_overlay(engine, overlays[i].draw, overlays[i].user) >= 0)
                overlays[i].draw(engine, overlays[i].user);
        profiler_scope_end(engine->profiler);
    }

    // The picture into the window, if it went into one; the GUI is the developer's and goes over
    // the television, sharp.
    if (target)
        _engine_present_window(engine);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    loading_screen_frame(engine);

    // GUI last, after tone mapping. gui_render_frame self-gates on
    // gui_frame_active, so it no-ops when no panel/overlay is enabled.
    profiler_scope_begin_if(engine->profiler, engine->gui_frame_active, "gui");
    gui_render_frame(engine);
    profiler_scope_end(engine->profiler);
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
                                         const SceneView* view, UniformManager* u) {
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
    //
    // The frame index rides with the clock for a surface hook (spec 13.29), which runs in the
    // shading pass and the shadow passes alike.
    shader_clock_upload(u, engine ? (float)engine->render_time : 0.0f,
                        engine ? engine->total_frames : 0);
    uniform_set_float(u, "uDeltaTime", engine ? (float)engine->render_delta : 0.0f);

    wind_upload_to_program(scene ? scene->wind : NULL,
                           scene ? (const float*)scene->world_origin : NULL, u);

    // The morph's two camera positions, the view's. With no view they are both the
    // origin, which is a legal answer rather than a fallback: every mesh that
    // carries no morph attribute is an exact identity whatever these say, and one
    // that does is not being drawn by anything without a camera.
    const float* eye = view ? view->camera.position : GLM_VEC3_ZERO;
    // Falls back to THIS frame's eye rather than to the origin, which is what
    // "no previous frame" actually means: a camera that has not moved yet.
    const float* prev = view && view->state->prev_camera_valid
                            ? (const float*)view->state->prev_camera_position
                            : eye;
    uniform_set_vec3(u, "uMorphEye", eye);
    uniform_set_vec3(u, "uMorphEyePrev", prev);
    // The diagnostic off switch, and it rides this call rather than the quadtree
    // because it has to reach all five programs to be an off switch at all --
    // suppressing the morph in the shading pass alone is the exact prepass
    // disagreement this function exists to prevent, wearing a flag's name.
    uniform_set_float(u, "uMorphOff", engine && !engine->morph_enabled ? 1.0f : 0.0f);
}

void engine_recentre_on_camera(const Engine* engine, float lattice) {
    if (!engine || !engine->focus_view)
        return;
    Scene* scene = engine_get_scene(engine);
    if (!scene || !(lattice > 0.0f))
        return;
    const Camera* camera = &engine->focus_view->camera;
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
    target[0] += floorf(camera->position[0] / lattice + 0.5f) * lattice;
    target[2] += floorf(camera->position[2] / lattice + 0.5f) * lattice;
    scene_set_world_origin(scene, target);
}

// Automatic re-centring (spec 11.62 phase 5): once the focus view's camera has drifted further
// than the scene tolerates, re-centre on it.
static void engine_schedule_origin_shift(const Engine* engine, const Scene* scene) {
    if (!scene || scene->origin_shift_distance <= 0.0f || !engine->focus_view)
        return;
    if (glm_vec3_distance((float*)engine->focus_view->camera.position, GLM_VEC3_ZERO) <=
        scene->origin_shift_distance)
        return;
    engine_recentre_on_camera(engine, engine_origin_lattice(scene->origin_shift_distance));
}

/*
 * Apply a scheduled origin shift, the scene's half and the engine's together.
 *
 * The engine half is only what no owner republishes: each view's live camera, its
 * previous view-projection, and its froxel volume's stored camera. Anything that
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

    for (size_t i = 0; i < engine->view_count; i++) {
        SceneView* view = engine->views[i];
        SceneViewState* vs = view->state;
        glm_vec3_sub(view->camera.position, delta, view->camera.position);
        glm_vec3_sub(view->camera.look_at, delta, view->camera.look_at);

        // Rule 1 applies to the camera's own past as much as to the world's: the
        // morph reads the difference between this eye and the previous one, so
        // shifting one and not the other reports the whole shift as a camera move
        // and re-morphs the terrain in a single frame.
        glm_vec3_sub(vs->prev_camera_position, delta, vs->prev_camera_position);

        glm_translate(vs->prev_view_proj, delta);
        if (vs->postfx)
            glm_translate(vs->postfx->froxel_prev_view, delta);
    }

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
             (double)delta[0], (double)delta[1], (double)delta[2], (double)scene->world_origin[0],
             (double)scene->world_origin[1], (double)scene->world_origin[2]);
}

// What the scene passes read and the world gets once a frame: the texture uploads, the material
// array, roads, the composite cache and the POM heights. Run before the frame's first scene pass,
// after that view's shadows, or at the frame's end when no view draws. What lands here is missed by
// every view not drawing a TAA frame, whose average then begins again.
static void _engine_settle_world(Engine* engine, Scene* scene) {
    const bool array_dirty = scene && scene->material_textures_dirty;
    const bool heights_resolved = !scene || scene->heights_resolved;
    const unsigned long long vt_bakes = layers_vt_bakes(scene);

    // Process pending async texture uploads (max 5 per frame to avoid stutter), each into
    // the pool it was asked for. Five delivered may leave more waiting, which the next frame
    // takes.
    const size_t delivered = async_loader_process_pending(engine->async_loader, 5);
    if (delivered == 5)
        engine_request_frame(engine);

    // (Re)build the material texture array once its sources have loaded
    // (a no-op until then; masks fall back to their scalar factors).
    material_texture_array_ensure_built(scene, engine);

    // Roads BEFORE the composite cache, and the order is load-bearing: the
    // bake samples the road block, so an edited road must reach the GPU
    // before the key change it causes triggers the re-bake that reads it.
    material_roads_ensure(scene, engine);

    // The composite cache samples the array, so it is strictly after: a
    // no-op while the array is dirty, and one frame behind it at worst.
    material_layers_vt_ensure(scene, engine);

    // POM (§4.11): resolve height maps once the async texture loader drains.
    heights_ensure_resolved(scene, engine);

    const bool landed =
        scene &&
        (delivered > 0 || (array_dirty && !scene->material_textures_dirty) ||
         (!heights_resolved && scene->heights_resolved) || layers_vt_bakes(scene) != vt_bakes);
    // A view drawing a TAA frame this frame draws it with what landed; one resting, averaging or
    // away has missed it, and starts again when it next draws.
    for (size_t i = 0; landed && i < engine->view_count; i++) {
        SceneView* view = engine->views[i];
        SceneViewState* vs = view->state;
        if (vs->draws_this_frame && vs->average_n < 0)
            continue;
        vs->missed = true;
        if (!view->hidden)
            engine_request_frame(engine);
    }
}

// The nodes a view draws, as bits: 0 is every node, as a CullView's.
static uint32_t _view_mask(const SceneView* view) {
    return view->mask ? view->mask : ~0u;
}

// Whether a node `view` hides stands within reach of a kept lamp, whose shadow takes every node.
static bool _view_hides_kept_caster(const Scene* scene, const SceneView* view) {
    const ShadowSystem* ss = scene->shadow_system;
    const DrawList* list = scene->draw_list;
    if (!ss || !list || ss->tile_block_count == 0)
        return false;
    const CullView swaying = {.wind = scene->wind};
    const uint32_t mask = _view_mask(view);
    for (size_t i = 0; i < list->count; i++) {
        const DrawItem* item = &list->items[i];
        if (item->view_mask & mask)
            continue;
        AABB box;
        if (!draw_item_bounds(item, &swaying, &box))
            continue;
        vec3 lo = {0.0f, 0.0f, 0.0f}, hi = {0.0f, 0.0f, 0.0f};
        aabb_transform(box.min, box.max, item->node->global_transform, lo, hi);
        if (shadow_tiles_reach(ss, lo, hi))
            return true;
    }
    return false;
}

/*
 * Still views (spec 14.38). A view with accumulation on draws a TAA frame whenever anything it
 * shows changed since the frame before; once nothing has, it draws accumulate_frames more, each
 * jittered and averaged in equally, and then rests -- not drawn, its picture shown as it is --
 * until something changes again. What counts as a change is below: what the engine can see for
 * itself, what the app says (scene_mark_changed, engine_mark_changed), what animates forever, and
 * the engine's own work not yet done.
 */

// What the scene does of its own accord every frame: the sky's clock and its clouds' drift, water,
// rain, fire, particles, wind, a pose, a shader hook that animates. A pose counts whether or not
// its animator runs: the engine cannot see an animator, and a still pose holds every view up.
// Reads this frame's draw list, which the caller builds first: last frame's may hold what a hook
// has freed since.
static bool _scene_animates(const Scene* scene) {
    if (!scene)
        return false;
    if (scene->water || rain_draws(scene->rain) || fire_system_active(scene->fire) ||
        scene->particle_system_count > 0 || scene->wind || sky_animates(scene->sky))
        return true;
    const DrawList* list = scene->draw_list;
    for (size_t i = 0; list && i < list->count; i++) {
        const DrawItem* item = &list->items[i];
        const ShaderHook* hook =
            item->mesh->material ? shader_hook_live(item->mesh->material->shader_hook) : NULL;
        if (item->pose || (hook && hook->animated))
            return true;
    }
    return false;
}

// The engine's own work not yet done, which changes what a view shows when it lands: textures out,
// GI or reflection probes owed a capture, a kept lamp's faces not yet drawn, the sky's re-bake.
// What the world's settle delivers is marked a change as it lands (_engine_settle_world).
static bool _engine_world_busy(const Engine* engine, const Scene* scene) {
    if (async_loader_is_busy(engine->async_loader))
        return true;
    if (!scene)
        return false;
    return gi_world_pending(scene->gi) || probe_set_pending(scene->probe_set) ||
           shadow_tiles_pending(scene->shadow_system) || sky_rebake_pending(scene->sky, scene->ibl);
}

// Whether any view, shown or not, averages when still: the one kind the scene's own motion and the
// world's work matter to.
static bool _engine_any_view_averages(const Engine* engine) {
    for (size_t i = 0; i < engine->view_count; i++)
        if (engine->views[i]->settings.accumulate_frames > 0)
            return true;
    return false;
}

// A digest of what the view's chain holds by function -- its passes, each one's switch and
// parameters, its grade -- which the settings' bytes do not carry.
static uint64_t _view_chain_digest(const SceneView* view) {
    const PostFX* fx = view->state->postfx;
    if (!fx)
        return 0;
    uint64_t h = 1469598103934665603ull ^ (uint64_t)fx->lut_texture;
    const unsigned char* bytes = (const unsigned char*)fx->passes;
    const size_t n = (size_t)fx->pass_count * sizeof(fx->passes[0]);
    for (size_t i = 0; i < n; i++)
        h = (h ^ bytes[i]) * 1099511628211ull;
    return h ^ (uint64_t)fx->pass_count;
}

// Whether anything `view` shows differs from what it showed when this was last asked, and the
// record brought up to now. Asked every frame, the view shown or not, so a change made while it is
// hidden is not lost.
static bool _view_changed(const Engine* engine, const Scene* scene, SceneView* view,
                          bool world_moving) {
    SceneViewState* vs = view->state;
    int rect[4];
    scene_view_fb_rect(view, rect);
    const uint32_t mask = _view_mask(view);
    const uint64_t changes = engine->change_count + (scene ? scene->change_count : 0);
    const uint64_t epoch = scene_graph_epoch();
    const uint64_t chain = _view_chain_digest(view);
    const bool covered = loading_screen_covers(engine);
    const bool changed =
        !vs->record.valid || world_moving || covered != vs->record.covered ||
        scene != vs->record.scene ||
        memcmp(&vs->record.settings, &view->settings, sizeof(view->settings)) != 0 ||
        memcmp(vs->record.view, vs->view_matrix, sizeof(mat4)) != 0 ||
        memcmp(vs->record.projection, vs->projection_matrix, sizeof(mat4)) != 0 ||
        memcmp(vs->record.rect, rect, sizeof(rect)) != 0 || vs->record.mask != mask ||
        vs->record.changes != changes || vs->record.epoch != epoch || vs->record.chain != chain ||
        vs->record.through != engine->window_through_picture ||
        (scene && (scene->moved_mask & mask)) || exposure_adapting(&vs->exposure);
    vs->record.valid = true;
    vs->record.scene = scene;
    vs->record.settings = view->settings;
    glm_mat4_copy(vs->view_matrix, vs->record.view);
    glm_mat4_copy(vs->projection_matrix, vs->record.projection);
    memcpy(vs->record.rect, rect, sizeof(rect));
    vs->record.mask = mask;
    vs->record.changes = changes;
    vs->record.epoch = epoch;
    vs->record.chain = chain;
    vs->record.covered = covered;
    vs->record.through = engine->window_through_picture;
    return changed;
}

// The view's picture as it stands -- the last TAA frame -- kept to show while the average is young.
// Nothing is kept for a view that has drawn no picture yet.
static void _view_hold_picture(SceneView* view) {
    SceneViewState* vs = view->state;
    const PostFX* fx = vs->postfx;
    vs->held_valid = false;
    if (!fx || !fx->picture.fbo ||
        !gl_color_target_ensure_format(&vs->held, fx->picture.w, fx->picture.h, fx->picture.format,
                                       "held picture"))
        return;
    gl_color_target_copy(&fx->picture, vs->held.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    vs->held_valid = true;
}

// The average begun again: nothing in it, nothing held, not finished.
static void _view_restart_average(SceneView* view) {
    view->finished = false;
    view->frames_accumulated = 0;
    view->state->held_valid = false;
}

// A cut: every history the view keeps dropped, its camera's last frame taken as this one so nothing
// reads as having moved across the gap, and its average begun again.
static void _view_cut(SceneView* view) {
    SceneViewState* vs = view->state;
    postfx_cut(vs->postfx);
    vs->clouds.prev_frame = -1;
    glm_mat4_mul(vs->projection_matrix, vs->view_matrix, vs->prev_view_proj);
    glm_vec3_copy(view->camera.position, vs->prev_camera_position);
    glm_vec3_zero(vs->camera_travel);
    _view_restart_average(view);
}

// What `view` does this frame: a TAA frame, the next frame of its average, or nothing. A view that
// cannot draw this frame -- hidden, sized to nothing, its targets refused -- is away: what changes
// meanwhile is kept, and the frame it draws again is a cut if anything did.
static void _engine_decide_view(const Engine* engine, const Scene* scene, SceneView* view,
                                bool world_moving) {
    SceneViewState* vs = view->state;
    vs->average_n = -1;
    const bool changed = _view_changed(engine, scene, view, world_moving);
    if (view->hidden || !_view_drawable(view)) {
        vs->away = true;
        vs->missed = vs->missed || changed;
        vs->draws_this_frame = false;
        return;
    }
    if (vs->away && (vs->missed || changed))
        _view_cut(view);
    const bool restart = changed || vs->missed;
    vs->away = false;
    vs->missed = false;
    vs->draws_this_frame = true;

    int frames = view->settings.accumulate_frames;
    if (frames > 0 && postfx_taau_active(vs->postfx)) {
        if (!vs->taau_said)
            log_info("A view averages only at render scale 1; this one keeps TAA's upscale");
        vs->taau_said = true;
        frames = 0;
    }
    // A frame under a covering loading screen draws no picture, so it is never one of an average.
    if (frames <= 0 || restart || loading_screen_covers(engine)) {
        _view_restart_average(view);
        return;
    }
    if (view->finished) {
        vs->draws_this_frame = false;
        return;
    }
    if (frames < VIEW_ACCUMULATE_MIN)
        frames = VIEW_ACCUMULATE_MIN;
    if (frames > VIEW_ACCUMULATE_MAX)
        frames = VIEW_ACCUMULATE_MAX;
    // The first frame of the average keeps the last TAA picture, shown until the average holds
    // enough frames to look better than it.
    if (view->frames_accumulated == 0)
        _view_hold_picture(view);
    vs->average_n = view->frames_accumulated;
    view->frames_accumulated++;
    view->finished = view->frames_accumulated >= frames;
}

// What one view draws that the engine only half serves, said once each: water or rain in a view
// other than the focus, whose caustic, ripple and cover windows are centred on the focus; a node
// the view hides within a kept lamp's reach, whose shadow is drawn for every view; and a stage over
// an exact background that the inverse cannot undo.
static void _engine_say_view_limits(Engine* engine, const Scene* scene, const SceneView* view) {
    if (!engine->said_focus_world && view != engine->focus_view &&
        ((scene->water && water_will_draw(scene->water, view)) || rain_draws(scene->rain))) {
        log_info("A view other than the focus draws water or rain, whose windows follow the "
                 "focus view's eye");
        engine->said_focus_world = true;
    }
    if (!engine->said_kept_hidden && _view_mask(view) != ~0u &&
        _view_hides_kept_caster(scene, view)) {
        log_info("A view hides a node within a kept lamp's reach, whose shadow every view takes");
        engine->said_kept_hidden = true;
    }
    // A stage that changes a cleared texel by where it is, or past what the inverse undoes.
    // Bloom near something bright glows onto it, as a camera's would, and the dither moves it a
    // code at most: neither is said.
    const PostFXSettings* p = &view->settings.post;
    if (view->settings.background_exact && !view->state->exact_stage_said &&
        (p->vignette_enabled || p->grade_enabled || p->local_exposure_enabled || p->grain_enabled ||
         p->purkinje_enabled || p->fog_enabled || p->sharpen_enabled ||
         (view->state->postfx && view->state->postfx->lut_texture && p->lut_strength > 0.0f) ||
         (scene->sky && scene->sky->enabled && scene->sky->aerial_enabled))) {
        log_info("A view's exact background has a stage over it the engine cannot undo -- a "
                 "vignette, a grade, a LUT, local exposure, grain, Purkinje, fog or aerial "
                 "perspective -- so it is not shown exactly");
        view->state->exact_stage_said = true;
    }
}

// One view's part of the frame, from its shadows to its post chain. `*settled` says whether the
// world has settled this frame; the first view to reach its scene pass settles it.
static void _engine_draw_view(Engine* engine, Scene* scene, SceneView* view, EngineRenderFunc draw,
                              bool* settled) {
    SceneViewState* vs = view->state;
    const RenderMode frame_mode = vs->render_mode;

    // Wireframe mode: albedo-only for speed, and every edge of every face,
    // so culling goes off with it. Both GL states are applied from the view's
    // setting here, once a frame, which is what lets show_wireframe be a plain
    // write: nothing has to run at the moment it changes.
    if (view->settings.show_wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glDisable(GL_CULL_FACE);
    }

    // Shadow depth pass, fitted to this view's camera.
    if (scene && scene->shadow_system && scene->shadow_system->enabled) {
        // Unscoped here on purpose: render_shadow_depth_pass opens its own
        // cascade/punctual/msm/tsm scopes, and an outer one would swallow
        // all four -- flat scopes mean the coarser wrapper simply wins.
        render_shadow_depth_pass(engine, scene, view);
    }
    // The rain's cover, into the punctual array after the lights' layers, once a frame: it is
    // centred on the focus view and drawn with every node, so each view would draw the same layer,
    // and its readback ring steps a frame at a time. Outside the `enabled` gate on purpose: that
    // switches shadows off, not roofs.
    if (scene && !*settled)
        shadow_render_rain_layer(engine, scene);

    glBindFramebuffer(GL_FRAMEBUFFER, vs->framebuffer);
    // The scene target is supersampled; the present pass left the viewport
    // at display size, so set it to the full render size here.
    int rw, rh;
    scene_view_render_size(view, &rw, &rh);
    glViewport(0, 0, rw, rh);
    // Published from here rather than computed inside the profiler, because
    // this is where the render size and the sample count are both settled --
    // supersample, render scale and any mid-run resolution change have all
    // been applied by now.
    //
    // msaa_samples_actual, never the request: above one sample the driver
    // may adjust the count, and GL_SAMPLES_PASSED counts what the target
    // HAS. The rule was written in scar tissue -- back when a 1-sample
    // request was allocated multisample, this driver returned 2 and every
    // depth-complexity reading on the TAA path came out at exactly twice
    // the truth until this read the count back (spec 11.31; the request is
    // honoured at 1 since 11.34).
    profiler_set_sample_budget(engine->profiler,
                               (size_t)rw * (size_t)rh * (size_t)vs->msaa_samples_actual);
    vs->normals_this_frame = frame_mode == RENDER_MODE_PBR && postfx_wants_normals(vs->postfx);
    // The frame's draw list, built here so the material registry describes
    // what the graph draws before anything reads it: the build registers
    // every material it walks past, and the subsurface gate below and the
    // mask-array build read the registry. A stamp compare when the shadow
    // pass or a capture already built it this frame, and the frame's one
    // build otherwise. An app that builds materials in code never called
    // scene_add_material, so its registry was empty and every consumer
    // silently saw a scene with no materials at all.
    engine_build_draw_list(engine, scene, view);
    if (scene)
        _engine_say_view_limits(engine, scene, view);
    // A view that has finished its average asks for nothing: it rests until something changes.
    if (_engine_scene_draws(scene) && !view->finished)
        engine_request_frame(engine);
    // SSS writes attachment 4 (skin diffuse) only when it has real work: the
    // feature is on AND the scene carries a subsurface material. This keeps a
    // non-skin scene byte-identical to master (the blur/composite would add 0)
    // AND free of the pass's per-frame cost. Mirrors the albedo-on-SSGI gate.
    vs->sss_this_frame =
        frame_mode == RENDER_MODE_PBR && engine->sss_enabled && scene_has_subsurface(scene);
    // SSS reads aux .z for the depth-aware profile, so it must force the aux
    // G-buffer (like motion blur), else --no-ssao + SSS reads stale/undefined Z.
    vs->aux_this_frame = frame_mode == RENDER_MODE_PBR &&
                         (postfx_wants_aux_gbuffer(vs->postfx) || vs->sss_this_frame);
    vs->albedo_this_frame = frame_mode == RENDER_MODE_PBR && postfx_wants_albedo(vs->postfx);
    // Split spec-occ: ambient specular routes to attachment 7 so the post
    // chain can occlude exactly that share and fold it back. Not gated on
    // AO -- with AO off the composite still owes the scene its specular.
    vs->spec_this_frame = frame_mode == RENDER_MODE_PBR && postfx_wants_spec_split(vs->postfx);
    // An exact background, turned back through the curve this frame takes: a debug view's frame
    // is copied as it is, like passthrough.
    if (view->settings.background_exact) {
        const PostFXTonemapMode mode = frame_mode == RENDER_MODE_PBR
                                           ? view->settings.post.tonemap_mode
                                           : POSTFX_TONEMAP_PASSTHROUGH;
        if (!background_working(view->settings.background, mode, vs->exact_background) &&
            !vs->exact_unreachable_said) {
            log_info("A view's exact background is not a colour its tone curve can give; it "
                     "shows the nearest");
            vs->exact_unreachable_said = true;
        }
    }
    profiler_scope_begin(engine->profiler, "scene clear");
    scene_view_clear(view);
    profiler_scope_end(engine->profiler);

    // After the first view's shadows, as the frame has always run it, and before any scene pass.
    if (!*settled) {
        _engine_settle_world(engine, scene);
        *settled = true;
    }

    if (scene)
        draw(engine, scene, view);

    // The feedback vote pass (spec 11.67), after the scene so the draw
    // list is this frame's; its readback retires at fixed latency into the
    // NEXT frames' residency, which is what keeps the loop deterministic.
    // The focus view's alone: the pages are the world's, ranked from its eye.
    if (scene && view == engine->focus_view)
        layers_vt_feedback_pass(engine, scene, view);

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    if (!loading_screen_covers(engine))
        _engine_run_view_chain(engine, view);
    if (view->settings.show_wireframe)
        glEnable(GL_CULL_FACE);
    vs->draw_index++;
}

void engine_run(Engine* engine, EngineUpdateFunc update, EnginePreRenderFunc pre_render,
                EngineRenderFunc render) {
    if (!engine)
        return;

    glEnable(GL_DEPTH_TEST);

    glCullFace(GL_BACK); // Cull back faces
    glFrontFace(GL_CCW); // Front faces are defined in counter-clockwise order

    // No hook means the scene draws itself; a hook is for what an app does
    // around that draw.
    EngineRenderFunc draw = render ? render : engine_render_scene;

    // A headless run is a capture, judged by the frames it saves, so it starts with every texture
    // its start-up asked for. Streamed in, at five uploads a frame and as fast as the machine
    // decodes, a frame N showed a room half loaded on a busy machine and a whole one on an idle
    // one. A window draws at once and lets them arrive. Before the clock is read, so the first
    // frame's delta does not hold the wait.
    if (engine->headless)
        engine_finish_texture_loads(engine);

    engine->last_frame_time = glfwGetTime() - engine->time_waited;

    while (!glfwWindowShouldClose(engine->window)) {
        // delta_time holds the honest (unclamped) per-frame dt; only the FPS
        // average uses a clamped copy (a game re-clamps to its own max_frame_time
        // for sim stability). The engine's clock is the wall clock less the time
        // idle mode spent waiting (spec 14.1), so a frame after a wait advances what
        // animates by a frame and not by the wait.
        double current_time = glfwGetTime() - engine->time_waited;
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

        // Adopt any pending resolution change (a view's samples or scales, the window's size)
        // before this frame touches GL. Everything below assumes each view's scene target and
        // post chain agree on a size.
        for (size_t i = 0; i < engine->view_count; i++)
            scene_view_ensure_targets(engine->views[i]);
        if (!_engine_frame_renderable(engine)) {
            // Nothing to draw. There is no swap on this path, so nothing
            // throttles it either -- block on events rather than spinning a
            // core while a window sits minimized (GLFW's iconify pattern).
            // Headless has no events to wake on, so it polls and relies on the
            // frame limit to end the run.
            // Closes the frame the begin above opened. Skipping it would leave
            // the frame counter and the ring slot where they are for as long as
            // the window stays minimized, after which every scope is refused as
            // a repeat and the table freezes rather than emptying.
            profiler_end_frame(engine->profiler);
            _engine_advance_frame(engine);
            // No hook reads what arrives while minimized, so none of it waits for the next frame.
            _engine_clear_latches(engine);
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
            // A GUI taking input this frame shows some of what it did -- a hover's highlight, a
            // popup opening -- only in the next, which idle mode is asked for.
            if (engine->show_gui && igGetCurrentContext()->InputEventsQueue.Size > 0)
                engine_request_frame(engine);
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
            Scene* tick_scene = engine_get_scene(engine);
            if (tick_scene)
                scene_update_particle_systems(tick_scene, (float)engine->render_delta,
                                              (float)engine->render_time);
        }

        // The water's own sim, on the same principle as the particle tick above and for a
        // sharper reason: the swash film's tips are read by the opaque pass and again by the
        // late pass, so it has to advance before EITHER of them. Outside the clock branch,
        // because a substituted clock still leaves render_time and render_delta valid, and a
        // film that stops stepping under one is the frozen-swash case this placement fixes.
        Scene* water_scene = engine_get_scene(engine);
        if (water_scene) {
            water_update(water_scene->water, water_scene, (float)engine->render_time,
                         (float)engine->render_delta);
            // Beside the water for the same reasons: the state it accumulates is read by
            // both scene passes, and a substituted clock must not freeze it.
            rain_update(water_scene->rain, water_scene->wind, (float)engine->render_time,
                        (float)engine->render_delta);
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

        // Origin shift (spec 11.62), after the update callback so it sees the
        // camera and physics this frame produced, and before the GI capture and
        // the shadow pass, which are the first things to read world positions.
        // Anywhere later and the first shifted frame fits its shadows and its
        // probes around an origin the rest of the frame no longer uses.
        engine_apply_origin_shift(engine, engine_get_scene(engine));

        // The frame's GL baseline, every polygon filled and back faces culled; a view under the
        // wireframe overlay leaves it for its own passes. Re-applied here, once a frame, so no
        // overlay has to put it back at the moment it changes.
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glEnable(GL_CULL_FACE);
        // The mode each view renders with this frame, read by everything below that asks whether
        // a view's frame is PBR -- the camera fit among them.
        for (size_t i = 0; i < engine->view_count; i++)
            engine->views[i]->state->render_mode = _view_frame_mode(engine->views[i]);

        Scene* shadow_scene = engine_get_scene(engine);

        // The day/night cycle's tick (spec 11.81), BEFORE the GI sweep and
        // the shadow pass: the key light it rewrites is what the cascades
        // then fit, and a completed slice's swap must land before this
        // frame's first bind_ibl_textures. A structural no-op when the cycle
        // is off.
        if (shadow_scene && shadow_scene->sky && shadow_scene->ibl) {
            profiler_scope_begin_if(
                engine->profiler,
                shadow_scene->sky->slicer.item >= 0 || shadow_scene->sky->cycle_dirty, "sky cycle");
            bool env_swapped =
                sky_cycle_tick(shadow_scene->sky, shadow_scene->ibl, (float)engine->render_delta);
            profiler_scope_end(engine->profiler);
            // The scene owns what re-derives from its environment; the sky
            // only reports that its chain moved.
            if (env_swapped)
                gi_world_mark_dirty(shadow_scene->gi);
        }
        // The clouds' drift, once a frame however many views march them.
        if (shadow_scene && shadow_scene->sky)
            sky_clouds_advance(shadow_scene->sky, engine->render_delta);

        // The app settles its cameras and its graph, then the engine propagates
        // that graph -- once, here, for everything downstream (spec 11.96).
        //
        // THE WINDOW IS ONE STATEMENT WIDE and all three edges are load-bearing.
        // After engine_apply_origin_shift above, which rewrites root-child
        // locals and the whole tree's cached globals; a walk before it is undone
        // by it. After sky_cycle_tick, which rewrites a sun's
        // original_direction, and the walk is what rotates that by the owning
        // node -- walk first and a node-parented sun loses its node for the
        // frame. And before the GI capture below, whose probes a converged
        // volume never re-bakes, so a capture taken from stale transforms is
        // wrong permanently rather than for a frame.
        //
        // It used to happen in the app's render callback, after all three of
        // those AND after the shadow pass, so every shadow was drawn from last
        // frame's transforms and every LOD level chosen from last frame's
        // positions.
        if (shadow_scene) {
            // The latch first and the walk second, and only the latch is
            // once-a-frame: it is the step that is not idempotent, so the engine
            // owns it and nothing else calls it. The walk that follows may be
            // re-run by anyone.
            scene_latch_prev_transforms(shadow_scene);
            if (pre_render)
                pre_render(engine, shadow_scene);
            // CPU scope: this issues no GL, like the draw-list build.
            profiler_cpu_scope_begin(engine->profiler, "transform walk");
            scene_propagate_transforms(shadow_scene);
            profiler_cpu_scope_end(engine->profiler);
        }

        // Each view's rig, then its camera as the hook left it, before the shadow pass
        // reads it. Every app used to rebuild both matrices at the end of its
        // own hook, and the three that did so only through the drag controller
        // skipped it whenever the GUI held the pointer.
        //
        // The rig's POSE lands here rather than inside the hook because that is
        // what makes the ordering the engine's to get right instead of each
        // app's: the hook turns input into a rig pose, this writes it to the
        // camera, and the derive turns that into matrices. The update itself
        // stays with the app, which is where the input is -- an action table, a
        // mouse or a script, and the rig is not allowed to know which. A view
        // with no rig is untouched.
        // Each view's targets again too: the hook may have laid the views out, and a rectangle
        // or a setting changed there is drawn at its new size this frame, not the next.
        for (size_t i = 0; i < engine->view_count; i++) {
            SceneView* view = engine->views[i];
            scene_view_ensure_targets(view);
            camera_rig_apply(view->rig, &view->camera);
            _engine_derive_view(engine, view);
        }

        // The fires: their steps, on the GPU, and what they cast handed to the lights and
        // embers they drive. After the walk, because a fire burns where its node is this frame
        // and its light is placed in its own node's frame; before the GI capture and the shadow
        // pass, so a driven light is final before anything is rendered from it.
        if (shadow_scene && shadow_scene->fire) {
            fire_update(shadow_scene->fire, shadow_scene->wind, engine->render_time);
            if (!engine->fire_renderer)
                engine->fire_renderer = create_fire_renderer();
            fire_simulate(engine->fire_renderer, engine, shadow_scene);
            fire_system_drive(shadow_scene->fire, shadow_scene->root_node,
                              (float)engine->render_delta);
        }

        // The streamed lighting: shadow tiles, GI volumes, reflection probes, ranked from the
        // focus view's eye. Deliberately BEFORE the shadow passes: a capture needs the
        // camera-independent single-cascade map and bakes its own, and each view's pass below
        // then restores its camera-fit cascades by simply overwriting them. A converged world
        // costs a ranking.
        scene_update_lighting(shadow_scene, engine);

        // The SAME scene the walk above propagated, not a fresh lookup. The
        // pre-render hook can switch the active scene, and re-reading it here
        // would draw one that no pass this frame prepared -- no transform walk,
        // no cascade fit, no GI capture. Since 11.96 there is app code between
        // the two points, so this stopped being a distinction without a
        // difference.
        Scene* current_scene = shadow_scene;

        // Every shown view that can draw, in list order, each from its shadows to its chain into
        // its picture, in the format the window's frame decides.
        engine->window_through_picture = _engine_window_through_picture(engine);
        // Which views draw at all: a still view resting draws nothing, its picture shown as it is.
        // The scene's own motion and the world's work matter only to a view that averages, and the
        // first is read off this frame's list, built here for it.
        bool world_moving = false;
        if (_engine_any_view_averages(engine)) {
            engine_build_draw_list(engine, current_scene, engine->focus_view);
            world_moving =
                _scene_animates(current_scene) || _engine_world_busy(engine, current_scene);
        }
        for (size_t i = 0; i < engine->view_count; i++)
            _engine_decide_view(engine, current_scene, engine->views[i], world_moving);
        // The moves the views have now seen; a move after this, a render hook's, is the next
        // frame's.
        if (current_scene)
            current_scene->moved_mask = 0u;
        engine->decided_changes =
            engine->change_count + (current_scene ? current_scene->change_count : 0);
        bool settled = false;
        engine->views_drawing = true;
        for (size_t i = 0; i < engine->view_count; i++) {
            SceneView* view = engine->views[i];
            if (!_view_drawable(view) || !view->state->draws_this_frame)
                continue;
            _engine_draw_view(engine, current_scene, view, draw, &settled);
        }
        engine->views_drawing = false;
        if (!settled)
            _engine_settle_world(engine, current_scene);

        _engine_present_frame(engine);
        _engine_frame_end_requests(engine);
        profiler_end_frame(engine->profiler);

        // A script's close, asked before the screenshot below, so one taken makes this frame the
        // run's last and its screenshot.
        _engine_input_script_close(engine);

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

        // Idle mode's headless end: nothing asks for another frame and nothing out will, so this
        // one is the last and its screenshot the run's.
        if (engine->idle && _engine_idle_next(engine) == IDLE_DONE)
            glfwSetWindowShouldClose(engine->window, GLFW_TRUE);

        // Capture the final frame before exit if a screenshot was requested
        if (engine->screenshot_path && glfwWindowShouldClose(engine->window)) {
            _save_framebuffer_ppm(engine, engine->screenshot_path);
        }

        glfwSwapBuffers(engine->window);
        _engine_clear_latches(engine);
        _engine_frame_events(engine);
        // A scripted batch lands where the real one just did, so its wheel and
        // typing reach the next frame's hooks on the same schedule -- and after
        // idle mode's wait, which is what makes the next frame draw its last.
        _engine_input_script_frame(engine);
    }
}

// The node under a press, through the view it lands in: the hit's point and distance and the
// node's position are kept for the drag that may follow. NULL over no view or no node.
static SceneNode* _engine_pick(Engine* engine, double mouse_fb_x, double mouse_fb_y) {
    const SceneView* view = engine_view_at(engine, mouse_fb_x, mouse_fb_y);
    Scene* current_scene = engine_get_scene(engine);
    if (!view || !current_scene || !current_scene->root_node)
        return NULL;
    float point[2];
    scene_view_point_from_fb(view, mouse_fb_x, mouse_fb_y, point);
    vec3 ray_origin = {0.0f, 0.0f, 0.0f}, ray_dir = {0.0f, 0.0f, 0.0f};
    scene_view_ray(view, point, ray_origin, ray_dir);

    RayPickResult result =
        pick_scene_node(current_scene->root_node, ray_origin, ray_dir, _view_mask(view));

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
