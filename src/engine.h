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

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_KEYSTATE_BASED_INPUT
#include "nuklear.h"
#include "nuklear_glfw_gl3.h"


typedef struct Engine {
    GLFWwindow* window;
    char* window_title;      // Title of the GLFW window
    int window_width;        // Width of the window
    int window_height;       // Height of the window

    GLuint framebuffer;           // Framebuffer object
    int framebuffer_width;   // Width of the framebuffer
    int framebuffer_height;  // Height of the framebuffer

    GLuint multisample_texture;   // Multisample texture for MSAA
    GLuint depth_renderbuffer;    // Depth renderbuffer
                                  //
    GLFWerrorfun error_callback;
    GLFWmousebuttonfun mouse_button_callback;
    GLFWcursorposfun cursor_position_callback;

    Camera *camera; // main camera

    Scene** scenes;            // Array of scenes managed by the engine
    size_t scene_count;        // Number of scenes
    size_t current_scene_index;// Index of the currently active scene
    Camera* main_camera;       // Primary camera for rendering

    ShaderProgram** programs; // Global shader programs used across scenes
    size_t program_count;      // Count of global programs
    
    RenderMode current_render_mode; // default is PBR

    mat4 model_matrix;
    mat4 view_matrix;
    mat4 projection_matrix;

    // nuklear gui
    struct nk_context *nk_ctx;
    struct nk_glfw nk_glfw;
    struct nk_color bg;

    bool show_wireframe;
    bool show_axes;
} Engine;

typedef void (*RenderSceneFunc)(Engine*, Scene*);

Engine* create_engine(const char* window_title, int width, int height);
void free_engine(Engine* engine);

// glfw callbacks
void set_engine_error_callback(Engine* engine, GLFWerrorfun error_callback);
void set_engine_mouse_button_callback(Engine* engine, GLFWmousebuttonfun mouse_button_callback);
void set_engine_cursor_position_callback(Engine* engine, GLFWcursorposfun cursor_position_callback);

// setup glfw env
int setup_engine_glfw(Engine* engine);
void setup_engine_msaa(Engine *engine);

// Camera
void set_engine_camera(Engine* engine, Camera* camera);
void update_engine_camera_lookat(Engine* engine);
void update_engine_camera_perspective(Engine* engine);

// Scene
Scene* add_scene_from_fbx(Engine* engine, const char* fbx_file_path, const char* texture_directory);
void set_active_scene_by_index(Engine* engine, size_t scene_index);
void set_active_scene_by_name(Engine* engine, const char* scene_name);
Scene* get_current_scene(const Engine* engine);

// Shader Programs
void add_program_to_engine(Engine* engine, ShaderProgram* program);

// GUI
int setup_engine_gui(Engine* engine);
void render_nuklear_gui(Engine* engine);

// main render
void set_engine_show_wireframe(Engine* engine, bool show_wireframe);
void set_engine_show_axes(Engine* engine, bool show_axes);
void run_engine_render_loop(Engine* engine, RenderSceneFunc render_func);

#endif // _ENGINE_H_

