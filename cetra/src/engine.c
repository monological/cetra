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
#include "engine.h"
#include "transform.h"
#include "intersect.h"
#include "shadow.h"
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
static SceneNode* _perform_engine_ray_picking(Engine* engine, double mouse_fb_x, double mouse_fb_y);

/*
 * Engine
 */
Engine* create_engine(const char* window_title, int width, int height) {
    Engine* engine = malloc(sizeof(Engine));
    if (!engine) {
        log_error("Failed to allocate memory for engine");
        return NULL;
    }

    engine->window = NULL;

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
    engine->ss_scale = 1; // Supersampling off by default (4x fragment cost);
                          // opt in with --ssaa 2 for beauty shots
    engine->msaa_samples = 4; // 4x MSAA by default (runtime-toggleable)

    engine->error_callback = NULL;
    engine->mouse_button_callback = NULL;
    engine->cursor_position_callback = NULL;
    engine->key_callback = NULL;
    engine->scroll_callback = NULL;

    engine->framebuffer = 0;
    engine->multisample_texture = 0;
    engine->normal_multisample_texture = 0;
    engine->depth_renderbuffer = 0;
    engine->normals_this_frame = false;

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

    glm_mat4_identity(engine->model_matrix);
    glm_mat4_identity(engine->view_matrix);
    glm_mat4_identity(engine->projection_matrix);

    engine->show_gui = false;
    engine->show_wireframe = false;
    engine->show_xyz = false;
    engine->show_fps = false;
    engine->show_bones = false;
    engine->headless = false;
    engine->gui_frame_active = false;
    engine->screenshot_path = NULL;
    engine->screenshot_every = 0;
    engine->total_frames = 0;

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

    glDeleteFramebuffers(1, &engine->framebuffer);
    glDeleteTextures(1, &engine->multisample_texture);
    glDeleteTextures(1, &engine->normal_multisample_texture);
    glDeleteRenderbuffers(1, &engine->depth_renderbuffer);

    if (engine->catcher_vao)
        glDeleteVertexArrays(1, &engine->catcher_vao);
    if (engine->catcher_vbo)
        glDeleteBuffers(1, &engine->catcher_vbo);

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

    glfwGetFramebufferSize(engine->window, &(engine->fb_width), &(engine->fb_height));
    glViewport(0, 0, engine->fb_width, engine->fb_height);

    return 0;
}

/*
 * MSAA anti-aliasing
 *
 */
// The scene renders into a target supersampled by ss_scale; the post chain
// box-downsamples to display size at tone map. This render (not display)
// resolution is shared by the MSAA allocation and the scene-pass viewport.
static void engine_render_size(const Engine* engine, int* w, int* h) {
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
    glDeleteTextures(1, &engine->multisample_texture);
    glDeleteTextures(1, &engine->normal_multisample_texture);
    glDeleteRenderbuffers(1, &engine->depth_renderbuffer);
    engine->multisample_texture = 0;
    engine->normal_multisample_texture = 0;
    engine->depth_renderbuffer = 0;
}

// (Re)create the multisample attachments on engine->framebuffer at the given
// sample count and render size. The color target is float (RGBA16F) so the
// scene accumulates in linear HDR; the post stack tone maps it to the display.
static int _create_msaa_attachments(Engine* engine, int rw, int rh, int samples) {
    glBindFramebuffer(GL_FRAMEBUFFER, engine->framebuffer);

    glGenTextures(1, &engine->multisample_texture);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, engine->multisample_texture);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_RGBA16F, rw, rh, GL_TRUE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE,
                           engine->multisample_texture, 0);

    // Attachment 1: view-space normals (xyz) + roughness (a) for SSAO/SSR.
    // Always allocated, only written when a consumer is active (see
    // engine_set_scene_draw_buffers). RGBA16F so the MSAA resolve blit's
    // format-match rule is satisfied against the float resolve target.
    glGenTextures(1, &engine->normal_multisample_texture);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, engine->normal_multisample_texture);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_RGBA16F, rw, rh, GL_TRUE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D_MULTISAMPLE,
                           engine->normal_multisample_texture, 0);

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
        glDeleteFramebuffers(1, &engine->framebuffer);
        engine->framebuffer = 0;
        return -1;
    }
    return 0;
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

    int rw, rh;
    engine_render_size(engine, &rw, &rh);
    _destroy_msaa_attachments(engine);
    _create_msaa_attachments(engine, rw, rh, samples);
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

    if (_setup_engine_glfw(engine) != 0) {
        log_error("Failed to initialize engine GLFW");
        return -1;
    }
    if (_setup_engine_msaa(engine) != 0) {
        log_error("Failed to initialize engine MSAA");
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
    engine->postfx = create_postfx(engine->fb_width, engine->fb_height, engine->ss_scale);
    if (!engine->postfx) {
        log_error("Failed to initialize engine post-processing");
        return -1;
    }

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

    glfwGetWindowSize(window, &engine->win_width, &engine->win_height);
    glfwGetFramebufferSize(window, &engine->fb_width, &engine->fb_height);

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

    glfwGetWindowSize(window, &engine->win_width, &engine->win_height);
    glfwGetFramebufferSize(window, &engine->fb_width, &engine->fb_height);

    double mouse_fb_x, mouse_fb_y;
    glfwGetCursorPos(window, &mouse_fb_x, &mouse_fb_y);

    mouse_fb_x = ((mouse_fb_x / engine->win_width) * engine->fb_width);
    mouse_fb_y = ((1.0 - (mouse_fb_y / engine->win_height)) * engine->fb_height);

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

    camera->aspect_ratio = (float)engine->fb_width / (float)engine->fb_height;
    glm_perspective(camera->fov_radians, camera->aspect_ratio, camera->near_clip, camera->far_clip,
                    engine->projection_matrix);
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

    ShaderProgram* ibl_brdf_program = create_ibl_brdf_program();
    if (ibl_brdf_program) {
        add_shader_program_to_engine(engine, ibl_brdf_program);
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

void set_engine_screenshot_path(Engine* engine, const char* path) {
    if (!engine)
        return;
    if (engine->screenshot_path) {
        free(engine->screenshot_path);
        engine->screenshot_path = NULL;
    }
    if (path) {
        engine->screenshot_path = strdup(path);
    }
}

void set_engine_screenshot_every(Engine* engine, int every) {
    if (!engine)
        return;
    engine->screenshot_every = every > 0 ? every : 0;
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
        "PBR",        "Normals",         "World Pos",
        "Tex Coords", "Tangent Space",   "Flat Color",
        "Albedo",     "Simple Lighting", "Metallic/Roughness"};
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

    if (igRadioButton_Bool("Free", engine->camera_mode == CAMERA_MODE_FREE))
        engine->camera_mode = CAMERA_MODE_FREE;
    igSameLine(0, -1);
    if (igRadioButton_Bool("Orbit", engine->camera_mode == CAMERA_MODE_ORBIT))
        engine->camera_mode = CAMERA_MODE_ORBIT;

    // --- Animation: a collapsing section like the effect stacks below, shown
    // only when a clip is loaded.
    AnimationState* anim = get_render_animation_state();
    if (anim &&
        igCollapsingHeader_TreeNodeFlags("Animation", ImGuiTreeNodeFlags_DefaultOpen)) {
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

    Scene* scene = get_current_scene(engine);
    if (scene && scene->light_count > 0 &&
        igCollapsingHeader_TreeNodeFlags("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
        static float light_intensity = 3.0f;
        if (igSliderFloat("Intensity", &light_intensity, 0.0f, 20.0f, "%.2f", 0)) {
            for (size_t i = 0; i < scene->light_count; i++)
                if (scene->lights[i])
                    scene->lights[i]->intensity = light_intensity;
        }
    }

    if (scene && scene->render_skybox && scene->ibl &&
        igCollapsingHeader_TreeNodeFlags("Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
        _begin_effect_group("Ground Projection", &scene->skybox_ground_projection);
        igSliderFloat("Dome Radius", &scene->skybox_gp_radius, 1.0f, 100.0f, "%.2f", 0);
        igSliderFloat("Capture Height", &scene->skybox_gp_height, 0.1f, 10.0f, "%.2f", 0);
        _end_effect_group();

        igSliderFloat("IBL Intensity", &scene->ibl->intensity, 0.0f, 4.0f, "%.2f", 0);

        if (scene->shadow_system) {
            ShadowSystem* ss = scene->shadow_system;
            _begin_effect_group("Shadows", &ss->enabled);
            _begin_effect_group("PCSS", &ss->pcss_enabled);
            igSliderFloat("Shadow Softness", &ss->pcss_softness, 0.0f, 4.0f, "%.2f", 0);
            _end_effect_group();
            _end_effect_group();
        }

        _begin_effect_group("Shadow Catcher", &scene->shadow_catcher);
        igSliderFloat("Shadow Strength", &scene->shadow_catcher_strength, 0.0f, 1.0f, "%.2f", 0);
        _end_effect_group();
    }

    if (engine->postfx &&
        igCollapsingHeader_TreeNodeFlags("Post", ImGuiTreeNodeFlags_DefaultOpen)) {
        PostFX* fx = engine->postfx;

        igSeparatorText("Anti-aliasing");
        bool msaa = engine->msaa_samples > 1;
        if (igCheckbox("MSAA 4x", &msaa))
            set_engine_msaa_samples(engine, msaa ? 4 : 1);
        igSameLine(0, -1);
        igCheckbox("TAA", &fx->taa_enabled);

        bool aces = fx->tonemap_mode == POSTFX_TONEMAP_ACES;
        if (igCheckbox("ACES Tonemap", &aces))
            fx->tonemap_mode = aces ? POSTFX_TONEMAP_ACES : POSTFX_TONEMAP_NEUTRAL;
        igSliderFloat("Exposure", &fx->exposure, 0.05f, 8.0f, "%.2f", 0);

        _begin_effect_group("Bloom", &fx->bloom_enabled);
        igSliderFloat("Bloom Strength", &fx->bloom_strength, 0.0f, 1.0f, "%.3f", 0);
        igSliderFloat("Bloom Threshold", &fx->bloom_threshold, 0.0f, 8.0f, "%.2f", 0);
        _end_effect_group();

        _begin_effect_group("SSAO", &fx->ssao_enabled);
        igSliderFloat("SSAO Radius", &fx->ssao_radius, 0.05f, 5.0f, "%.2f", 0);
        igSliderFloat("SSAO Strength", &fx->ssao_strength, 0.0f, 1.0f, "%.2f", 0);
        _end_effect_group();

        _begin_effect_group("SSR", &fx->ssr_enabled);
        igSliderFloat("SSR Strength", &fx->ssr_strength, 0.0f, 2.0f, "%.2f", 0);
        igSliderFloat("SSR Distance", &fx->ssr_max_distance, 1.0f, 50.0f, "%.2f", 0);
        igSliderFloat("SSR Floor Rough", &fx->ssr_floor_roughness, 0.0f, 1.0f, "%.2f", 0);
        _end_effect_group();

        igCheckbox("Normals G-buffer", &fx->normals_enabled);
        igSliderFloat("Spec AA", &engine->specular_aa_strength, 0.0f, 2.0f, "%.2f", 0);
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
    }

    igEnd();
}

// Frameless FPS readout pinned to the top-right corner.
static void _engine_gui_fps_overlay(Engine* engine) {
    igSetNextWindowPos((ImVec2){(float)engine->win_width - 90.0f, 10.0f}, ImGuiCond_Always,
                       (ImVec2){0, 0});
    igSetNextWindowBgAlpha(0.0f);
    if (igBegin("##fps", NULL,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_AlwaysAutoResize)) {
        igText("%.1f FPS", engine->fps);
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
    if (engine->show_fps)
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

void engine_present_frame(Engine* engine, RenderMode frame_mode, bool draw_gui) {
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
    postfx_run(engine->postfx, engine->framebuffer, 0, frame_mode == RENDER_MODE_PBR,
               engine->normals_this_frame, engine->projection_matrix);

    if (draw_gui) {
        render_engine_gui(engine);
    }
}

void engine_set_scene_draw_buffers(const Engine* engine, bool with_normals) {
    if (!engine)
        return;

    if (with_normals && engine->normals_this_frame) {
        const GLenum both[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(2, both);
        // Blending is enabled globally; keep the normals target opaque.
        // Indexed state is wiped by any blanket glEnable(GL_BLEND), so
        // re-issue it at every pass boundary rather than once at init.
        glDisablei(GL_BLEND, 1);
    } else {
        const GLenum color_only[] = {GL_COLOR_ATTACHMENT0};
        glDrawBuffers(1, color_only);
    }
}

void run_engine_render_loop(Engine* engine, RenderSceneFunc render_func) {
    if (!engine)
        return;

    glEnable(GL_DEPTH_TEST);

    glCullFace(GL_BACK); // Cull back faces
    glFrontFace(GL_CCW); // Front faces are defined in counter-clockwise order

    engine->last_frame_time = glfwGetTime();

    while (!glfwWindowShouldClose(engine->window)) {
        // Calculate delta time and FPS
        double current_time = glfwGetTime();
        engine->delta_time = current_time - engine->last_frame_time;
        engine->last_frame_time = current_time;

        // Clamp delta time to prevent physics explosions on frame drops
        if (engine->delta_time > 0.1)
            engine->delta_time = 0.1;

        engine->frame_count++;
        engine->fps_update_timer += (float)engine->delta_time;

        if (engine->fps_update_timer >= 0.5f) {
            engine->fps = (float)engine->frame_count / engine->fps_update_timer;
            engine->frame_count = 0;
            engine->fps_update_timer = 0.0f;
        }

        // Begin the ImGui frame before the app's render_func, so both the app
        // (e.g. the tree app) and the engine panel can add windows between
        // NewFrame and the Render/RenderDrawData at present time. Latch the
        // decision so render_engine_gui pairs its igRender with this NewFrame
        // even if the panel flags are toggled mid-frame.
        engine->gui_frame_active = engine->show_gui || engine->show_fps;
        if (engine->gui_frame_active) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            igNewFrame();
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

        // Shadow depth pass (before main render)
        Scene* shadow_scene = get_current_scene(engine);
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
        engine_set_scene_draw_buffers(engine, true);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        if (engine->normals_this_frame) {
            const GLfloat zero_normal[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            glClearBufferfv(GL_COLOR, 1, zero_normal);
        }

        Scene* current_scene = get_current_scene(engine);

        // Process pending async texture uploads (max 5 per frame to avoid stutter)
        if (current_scene && current_scene->tex_pool && engine->async_loader) {
            async_loader_process_pending(engine->async_loader, current_scene->tex_pool, 5);
        }

        if (render_func != NULL && current_scene != NULL) {
            render_func(engine, current_scene);
        }

        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        engine->current_render_mode = saved_render_mode;

        engine_present_frame(engine, frame_mode, true);

        engine->total_frames++;

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
    mat4 projection, view;
    glm_perspective(engine->camera->fov_radians, engine->camera->aspect_ratio,
                    engine->camera->near_clip, engine->camera->far_clip, projection);
    glm_lookat(engine->camera->position, engine->camera->look_at, engine->camera->up_vector, view);

    // Compute ray from screen coordinates
    vec3 ray_dir = {0};
    compute_ray_from_screen((float)mouse_fb_x, (float)mouse_fb_y, engine->fb_width,
                            engine->fb_height, projection, view, engine->camera->position, ray_dir);

    // Project ray to the stored drag plane distance
    ray_point_at_distance(engine->camera->position, ray_dir, engine->input.drag_plane_distance,
                          out_world_pos);
}

static SceneNode* _perform_engine_ray_picking(Engine* engine, double mouse_fb_x,
                                              double mouse_fb_y) {
    // Need camera for ray picking
    if (!engine->camera)
        return NULL;

    // Build projection and view matrices
    mat4 projection, view;
    glm_perspective(engine->camera->fov_radians, engine->camera->aspect_ratio,
                    engine->camera->near_clip, engine->camera->far_clip, projection);
    glm_lookat(engine->camera->position, engine->camera->look_at, engine->camera->up_vector, view);

    // Compute ray from screen coordinates
    vec3 ray_origin, ray_dir = {0};
    glm_vec3_copy(engine->camera->position, ray_origin);
    compute_ray_from_screen((float)mouse_fb_x, (float)mouse_fb_y, engine->fb_width,
                            engine->fb_height, projection, view, ray_origin, ray_dir);

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
