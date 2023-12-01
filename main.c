#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "common.h"
#include "mesh.h"
#include "shader.h"
#include "program.h"
#include "scene.h"
#include "util.h"
#include "import.h"


#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_GLFW_GL4_IMPLEMENTATION
#define NK_KEYSTATE_BASED_INPUT
#include "nuklear.h"
#include "nuklear_glfw_gl4.h"

#define MAX_VERTEX_BUFFER 512 * 1024
#define MAX_ELEMENT_BUFFER 128 * 1024

#define PBR_VERT_SHADER_PATH "/Users/neo/Desktop/graphics/engine/src/pbr_vert.glsl"
#define PBR_FRAG_SHADER_PATH "/Users/neo/Desktop/graphics/engine/src/pbr_frag.glsl"
#define AXES_VERT_SHADER_PATH "/Users/neo/Desktop/graphics/engine/src/axes_vert.glsl"
#define AXES_FRAG_SHADER_PATH "/Users/neo/Desktop/graphics/engine/src/axes_frag.glsl"

const unsigned int WIDTH = 800;
const unsigned int HEIGHT = 600;
const float ROTATE_SPEED = 0.4f; // Speed of rotation
const float RADIUS = 2.0f;
const unsigned int RINGS = 20;
const unsigned int SECTORS = 20;
const float rotationSensitivity = 0.005f;

float cameraDistance = 2000.0f;
float cameraHeightBase = 0.0f; // Base height of the camera
float cameraAngle = 0.0f;
float zoomSpeed = 0.005f;
float orbitSpeed = 0.01f;
float verticalMovementAmplitude = 5.0f; // Amplitude of vertical movement


void generateSphere(Mesh* mesh, float radius, unsigned int rings, unsigned int sectors) {
    float const R = 1.0f / (float)(rings - 1);
    float const S = 1.0f / (float)(sectors - 1);

    mesh->vertexCount = rings * sectors * 3;
    mesh->indexCount = (rings - 1) * (sectors - 1) * 6;

    mesh->vertices = malloc(mesh->vertexCount * sizeof(float));
    mesh->indices = malloc(mesh->indexCount * sizeof(unsigned int));

    unsigned int v = 0, i = 0;
    for (unsigned int r = 0; r < rings; ++r) {
        for (unsigned int s = 0; s < sectors; ++s) {
            float const y = sinf(-M_PI_2 + M_PI * r * R);
            float const x = cosf(2 * M_PI * s * S) * sinf(M_PI * r * R);
            float const z = sinf(2 * M_PI * s * S) * sinf(M_PI * r * R);

            mesh->vertices[v++] = x * radius;
            mesh->vertices[v++] = y * radius;
            mesh->vertices[v++] = z * radius;
        }
    }

    for (unsigned int r = 0; r < rings - 1; ++r) {
        for (unsigned int s = 0; s < sectors - 1; ++s) {
            mesh->indices[i++] = r * sectors + s;
            mesh->indices[i++] = r * sectors + (s + 1);
            mesh->indices[i++] = (r + 1) * sectors + (s + 1);

            mesh->indices[i++] = (r + 1) * sectors + (s + 1);
            mesh->indices[i++] = (r + 1) * sectors + s;
            mesh->indices[i++] = r * sectors + s;
        }
    }
}


void error_callback(int error, const char* description) {
    fprintf(stderr, "Error: %s\n", description);
}



bool dragging = false;
float centerX, centerY;
float dx, dy;

void cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
    int windowWidth, windowHeight, framebufferWidth, framebufferHeight;

    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    xpos = ((xpos / windowWidth) * framebufferWidth);
    ypos = ((1 - ypos / windowHeight) * framebufferHeight);


    if (dragging) {
        dx = xpos - centerX;
        dy = ypos - centerY;
    }
}

void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    int windowWidth, windowHeight, framebufferWidth, framebufferHeight;

    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    double xpos, ypos;
    glfwGetCursorPos(window, &xpos, &ypos);

    xpos = ((xpos / windowWidth) * framebufferWidth);
    ypos = ((1 - ypos / windowHeight) * framebufferHeight);


    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        dragging = true;
        centerX = xpos;
        centerY = ypos;
        printf("dragging %i %f %f\n", dragging, xpos, ypos);
    } else if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
        dragging = false;
        printf("dragging %i %f %f\n", dragging, xpos, ypos);
    }
}


int main() {
    glfwSetErrorCallback(error_callback);

    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(WIDTH, HEIGHT, "Wireframe Sphere", NULL, NULL);
    if (window == NULL) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetCursorPosCallback(window, cursor_position_callback);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (glewInit() != GLEW_OK) {
        fprintf(stderr, "Failed to initialize GLEW\n");
        return -1;
    }


    struct nk_context *ctx;
    struct nk_colorf bg;
    struct nk_image img;

    ctx = nk_glfw3_init(window, NK_GLFW3_INSTALL_CALLBACKS, MAX_VERTEX_BUFFER, MAX_ELEMENT_BUFFER);
    /* Load Fonts: if none of these are loaded a default font will be used  */
    /* Load Cursor: if you uncomment cursor loading please hide the cursor */
    {struct nk_font_atlas *atlas;
    nk_glfw3_font_stash_begin(&atlas);
    /*struct nk_font *droid = nk_font_atlas_add_from_file(atlas, "../../../extra_font/DroidSans.ttf", 14, 0);*/
    /*struct nk_font *roboto = nk_font_atlas_add_from_file(atlas, "../../../extra_font/Roboto-Regular.ttf", 14, 0);*/
    /*struct nk_font *future = nk_font_atlas_add_from_file(atlas, "../../../extra_font/kenvector_future_thin.ttf", 13, 0);*/
    /*struct nk_font *clean = nk_font_atlas_add_from_file(atlas, "../../../extra_font/ProggyClean.ttf", 12, 0);*/
    /*struct nk_font *tiny = nk_font_atlas_add_from_file(atlas, "../../../extra_font/ProggyTiny.ttf", 10, 0);*/
    /*struct nk_font *cousine = nk_font_atlas_add_from_file(atlas, "../../../extra_font/Cousine-Regular.ttf", 13, 0);*/
    nk_glfw3_font_stash_end();
    /*nk_style_load_all_cursors(ctx, atlas->cursors);*/
    /*nk_style_set_font(ctx, &droid->handle);*/}

    /* Create bindless texture.
     * The index returned is not the opengl resource id.
     * IF you need the GL resource id use: nk_glfw3_get_tex_ogl_id() */
    {int tex_index = 0;
    enum {tex_width = 256, tex_height = 256};
    char pixels[tex_width * tex_height * 4];
    memset(pixels, 128, sizeof(pixels));
    tex_index = nk_glfw3_create_texture(pixels, tex_width, tex_height);
    img = nk_image_id(tex_index);}

    int framebufferWidth, framebufferHeight;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    glViewport(0, 0, framebufferWidth, framebufferHeight);
    CheckOpenGLError("view port");

    Program* pbr_shader_program = create_program();

    Shader* pbr_vertex_shader = create_shader_from_path(VERTEX, PBR_VERT_SHADER_PATH);
    Shader *pbr_geometry_shader = NULL;
    Shader* pbr_fragment_shader = create_shader_from_path(FRAGMENT, PBR_FRAG_SHADER_PATH);

    if (pbr_vertex_shader) {
        if (compile_shader(pbr_vertex_shader)) {
            attach_shader(pbr_shader_program, pbr_vertex_shader);
        } else {
            fprintf(stderr, "Vertex shader compilation failed\n");
            return -1;
        }
    }

    if (pbr_geometry_shader) {
        if (compile_shader(pbr_geometry_shader)) {
            attach_shader(pbr_shader_program, pbr_geometry_shader);
        } else {
            fprintf(stderr, "Geometry shader compilation failed\n");
            return -1;
        }
    }

    if (pbr_fragment_shader) {
        if (compile_shader(pbr_fragment_shader)) {
            attach_shader(pbr_shader_program, pbr_fragment_shader);
        } else {
            fprintf(stderr, "Fragment shader compilation failed\n");
            return -1;
        }
    }

    // Link the shader program
    if (!link_program(pbr_shader_program)) {
        fprintf(stderr, "Shader program linking failed\n");
        return -1;
    }


    Program* axes_shader_program = create_program();
    Shader* axes_vertex_shader = create_shader_from_path(VERTEX, AXES_VERT_SHADER_PATH);
    Shader* axes_fragment_shader = create_shader_from_path(FRAGMENT, AXES_FRAG_SHADER_PATH);

    if (axes_vertex_shader) {
        if (compile_shader(axes_vertex_shader)) {
            attach_shader(axes_shader_program, axes_vertex_shader);
        } else {
            fprintf(stderr, "Vertex shader compilation failed\n");
            return -1;
        }
    }

    if (axes_fragment_shader) {
        if (compile_shader(axes_fragment_shader)) {
            attach_shader(axes_shader_program, axes_fragment_shader);
        } else {
            fprintf(stderr, "Fragment shader compilation failed\n");
            return -1;
        }
    }

    // Link the shader program
    if (!link_program(axes_shader_program)) {
        fprintf(stderr, "Shader program linking failed\n");
        return -1;
    }

    
    mat4 view;
    mat4 projection;
    glm_mat4_identity(view);
    glm_mat4_identity(projection);

    vec3 cameraPosition = {0.0f, 2.0f, 300.0f};
    vec3 lookAtPoint = {0.0f, 0.0f, 0.0f};
    vec3 upVector = {0.0f, 1.0f, 0.0f};

    glm_lookat(cameraPosition, lookAtPoint, upVector, view);

    float fovRadians = glm_rad(35.0f);
    float aspectRatio = (float)framebufferWidth / (float)framebufferHeight;
    float nearClip = 0.1f;
    float farClip = 4000.0f;

    glm_perspective(fovRadians, aspectRatio, nearClip, farClip, projection);

    //generateSphere(mesh, RADIUS, RINGS, SECTORS);
    Scene *scene = import_fbx("/Users/neo/Desktop/graphics/engine/src/pilot.fbx", \
            "/Users/neo/Desktop/graphics/engine/src");
    print_scene(scene);

    // Create scene node for the sphere
    SceneNode* root_node = scene->root_node;
    assert(root_node != NULL);

    setup_node_meshes(root_node);
    set_shaders_for_node(root_node, pbr_vertex_shader, pbr_geometry_shader, pbr_fragment_shader);
    set_program_for_node(root_node, pbr_shader_program);

    setup_node_axes_buffers(root_node);
    set_axes_program_for_node(root_node, axes_shader_program);
    set_axes_shaders_for_node(root_node, axes_vertex_shader, axes_fragment_shader);

    //glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glEnable(GL_DEPTH_TEST);

    GLint camPosLoc = glGetUniformLocation(pbr_shader_program->programID, "camPos");
    glUniform3fv(camPosLoc, 1, (const GLfloat*)&cameraPosition);

    GLint timeUniformLocation = glGetUniformLocation(pbr_shader_program->programID, "time");

    while (!glfwWindowShouldClose(window)) {

        nk_glfw3_new_frame();

        /* GUI */
        if (nk_begin(ctx, "Demo", nk_rect(50, 50, 230, 250),
            NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|
            NK_WINDOW_MINIMIZABLE|NK_WINDOW_TITLE))
        {
            enum {EASY, HARD};
            static int op = EASY;
            static int property = 20;
            nk_layout_row_static(ctx, 30, 80, 1);
            if (nk_button_label(ctx, "button"))
                fprintf(stdout, "button pressed\n");

            nk_layout_row_dynamic(ctx, 30, 2);
            if (nk_option_label(ctx, "easy", op == EASY)) op = EASY;
            if (nk_option_label(ctx, "hard", op == HARD)) op = HARD;

            nk_layout_row_dynamic(ctx, 25, 1);
            nk_property_int(ctx, "Compression:", 0, &property, 100, 10, 1);

            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "background:", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 25, 1);
            if (nk_combo_begin_color(ctx, nk_rgb_cf(bg), nk_vec2(nk_widget_width(ctx),400))) {
                nk_layout_row_dynamic(ctx, 120, 1);
                bg = nk_color_picker(ctx, bg, NK_RGBA);
                nk_layout_row_dynamic(ctx, 25, 1);
                bg.r = nk_propertyf(ctx, "#R:", 0, bg.r, 1.0f, 0.01f,0.005f);
                bg.g = nk_propertyf(ctx, "#G:", 0, bg.g, 1.0f, 0.01f,0.005f);
                bg.b = nk_propertyf(ctx, "#B:", 0, bg.b, 1.0f, 0.01f,0.005f);
                bg.a = nk_propertyf(ctx, "#A:", 0, bg.a, 1.0f, 0.01f,0.005f);
                nk_combo_end(ctx);
            }
        }
        nk_end(ctx);

        /* Bindless Texture */
        if (nk_begin(ctx, "Texture", nk_rect(250, 150, 230, 250),
            NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|
            NK_WINDOW_MINIMIZABLE|NK_WINDOW_TITLE))
        {
            struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
            struct nk_rect total_space = nk_window_get_content_region(ctx);
            nk_draw_image(canvas, total_space, &img, nk_white);
        }
        nk_end(ctx);

        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        float timeValue = glfwGetTime();
        float angle = timeValue * ROTATE_SPEED;

        glUniform1f(timeUniformLocation, timeValue);

        Transform transform = {
            .position = {0.0f, 0.0f, 0.0f},
            .rotation = {0.0f, 0.0f, 0.0f},
            .scale = {1.0f, 1.0f, 1.0f}
        };

        if (!dragging) {

            // Update camera position for zoom and orbit
            cameraDistance += sin(timeValue * zoomSpeed) * 2.0f; // Reduced zoom amplitude
            cameraAngle += orbitSpeed; // Rotate around the center

            // Calculate vertical movement
            float cameraHeight = cameraHeightBase + cos(timeValue * zoomSpeed) * verticalMovementAmplitude;

            // Calculate the new camera position
            vec3 newCameraPosition = {
                cos(cameraAngle) * cameraDistance,
                cameraHeight,
                sin(cameraAngle) * cameraDistance
            };

            // Update camera view matrix
            glm_lookat(newCameraPosition, lookAtPoint, upVector, view);
            glUniform3fv(camPosLoc, 1, (const GLfloat*)&newCameraPosition);


        } else {
            float zpos = cosf(angle);
            float ypos = sinf(angle);

            transform.position[0] = 0.0f;
            transform.position[1] = -25.0f;
            transform.position[2] = 0.0f;
            transform.rotation[0] = dy * rotationSensitivity;
            transform.rotation[1] = dx * rotationSensitivity;
            transform.rotation[2] = 0.0f;
        }


        transform_node(root_node, &transform);

        mat4 identityMatrix;
        glm_mat4_identity(identityMatrix);
        update_child_nodes(root_node, identityMatrix);

        render_node(root_node, root_node->local_transform, view, projection);

        nk_glfw3_render(NK_ANTI_ALIASING_ON);
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Cleanup
    free_node(root_node);
    free_shader(pbr_vertex_shader);
    free_shader(pbr_geometry_shader);
    free_shader(pbr_fragment_shader);
    free_program(pbr_shader_program);
    free_program(axes_shader_program);
    free_shader(axes_vertex_shader);
    free_shader(axes_fragment_shader);

    nk_glfw3_shutdown();
    glfwTerminate();

    return 0;
}



