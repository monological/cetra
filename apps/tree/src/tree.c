#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "cetra/common.h"
#include "cetra/mesh.h"
#include "cetra/shader.h"
#include "cetra/program.h"
#include "cetra/scene.h"
#include "cetra/util.h"
#include "cetra/engine.h"
#include "cetra/render.h"
#include "cetra/geometry.h"
#include "cetra/transform.h"
#include "cetra/light.h"
#include "cetra/texture.h"

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_KEYSTATE_BASED_INPUT
#include "cetra/ext/nuklear.h"
#include "cetra/ext/nuklear_glfw_gl3.h"

#define CYLINDER_SEGMENTS 12
#define TEXTURE_SIZE      512

/*
 * Perlin Noise Implementation
 */
static int perm[512];
static int perm_initialized = 0;

static void init_perlin(unsigned int seed) {
    srand(seed);
    int p[256];
    for (int i = 0; i < 256; i++)
        p[i] = i;
    for (int i = 255; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = p[i];
        p[i] = p[j];
        p[j] = tmp;
    }
    for (int i = 0; i < 512; i++)
        perm[i] = p[i & 255];
    perm_initialized = 1;
}

static float fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float lerp_f(float a, float b, float t) {
    return a + t * (b - a);
}

static float grad(int hash, float x, float y) {
    int h = hash & 7;
    float u = h < 4 ? x : y;
    float v = h < 4 ? y : x;
    return ((h & 1) ? -u : u) + ((h & 2) ? -2.0f * v : 2.0f * v);
}

static float perlin_noise_2d(float x, float y) {
    if (!perm_initialized)
        init_perlin(12345);

    int xi = (int)floorf(x) & 255;
    int yi = (int)floorf(y) & 255;
    float xf = x - floorf(x);
    float yf = y - floorf(y);

    float u = fade(xf);
    float v = fade(yf);

    int aa = perm[perm[xi] + yi];
    int ab = perm[perm[xi] + yi + 1];
    int ba = perm[perm[xi + 1] + yi];
    int bb = perm[perm[xi + 1] + yi + 1];

    float x1 = lerp_f(grad(aa, xf, yf), grad(ba, xf - 1, yf), u);
    float x2 = lerp_f(grad(ab, xf, yf - 1), grad(bb, xf - 1, yf - 1), u);

    return (lerp_f(x1, x2, v) + 1.0f) * 0.5f;
}

static float fbm_noise(float x, float y, int octaves, float persistence) {
    float total = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float max_value = 0.0f;

    for (int i = 0; i < octaves; i++) {
        total += perlin_noise_2d(x * frequency, y * frequency) * amplitude;
        max_value += amplitude;
        amplitude *= persistence;
        frequency *= 2.0f;
    }

    return total / max_value;
}

/*
 * Worley (Cellular) Noise for bark cracks
 */
static float worley_noise_2d(float x, float y, unsigned int seed) {
    int xi = (int)floorf(x);
    int yi = (int)floorf(y);

    float min_dist = 999.0f;

    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int cx = xi + dx;
            int cy = yi + dy;

            // Hash cell to get feature point
            unsigned int h = (unsigned int)(cx * 374761393 + cy * 668265263 + seed);
            h = (h ^ (h >> 13)) * 1274126177;

            float fx = (float)cx + (float)(h & 0xFFFF) / 65536.0f;
            float fy = (float)cy + (float)((h >> 16) & 0xFFFF) / 65536.0f;

            float dist = (x - fx) * (x - fx) + (y - fy) * (y - fy);
            if (dist < min_dist)
                min_dist = dist;
        }
    }

    return sqrtf(min_dist);
}

/*
 * Generate bark albedo texture
 */
static unsigned char* generate_bark_albedo(int width, int height) {
    unsigned char* data = malloc(width * height * 3);
    if (!data)
        return NULL;

    init_perlin(42);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float u = (float)x / width;
            float v = (float)y / height;

            // Base brown color
            float base_r = 0.35f;
            float base_g = 0.22f;
            float base_b = 0.12f;

            // Large scale color variation
            float noise1 = fbm_noise(u * 4.0f, v * 8.0f, 4, 0.5f);

            // Vertical grain stretching
            float grain = fbm_noise(u * 8.0f, v * 2.0f, 3, 0.6f);

            // Worley noise for cracks
            float crack = worley_noise_2d(u * 6.0f, v * 12.0f, 123);
            crack = fmaxf(0.0f, 1.0f - crack * 2.0f);

            // Combine
            float variation = noise1 * 0.3f + grain * 0.2f;
            float darkness = crack * 0.4f;

            float r = base_r + variation * 0.15f - darkness * 0.2f;
            float g = base_g + variation * 0.1f - darkness * 0.15f;
            float b = base_b + variation * 0.05f - darkness * 0.1f;

            // Add some reddish tones in cracks
            r += crack * 0.05f;

            // Clamp
            r = fmaxf(0.0f, fminf(1.0f, r));
            g = fmaxf(0.0f, fminf(1.0f, g));
            b = fmaxf(0.0f, fminf(1.0f, b));

            int idx = (y * width + x) * 3;
            data[idx + 0] = (unsigned char)(r * 255);
            data[idx + 1] = (unsigned char)(g * 255);
            data[idx + 2] = (unsigned char)(b * 255);
        }
    }

    return data;
}

/*
 * Generate bark normal map from height
 */
static unsigned char* generate_bark_normal(int width, int height) {
    unsigned char* data = malloc(width * height * 3);
    if (!data)
        return NULL;

    // First generate height map
    float* heightmap = malloc(width * height * sizeof(float));
    if (!heightmap) {
        free(data);
        return NULL;
    }

    init_perlin(42);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float u = (float)x / width;
            float v = (float)y / height;

            float noise = fbm_noise(u * 8.0f, v * 16.0f, 4, 0.5f);
            float crack = worley_noise_2d(u * 6.0f, v * 12.0f, 123);
            crack = fmaxf(0.0f, 1.0f - crack * 2.5f);

            heightmap[y * width + x] = noise * 0.6f + (1.0f - crack) * 0.4f;
        }
    }

    // Calculate normals using Sobel filter
    float strength = 2.0f;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int x0 = (x - 1 + width) % width;
            int x1 = (x + 1) % width;
            int y0 = (y - 1 + height) % height;
            int y1 = (y + 1) % height;

            float dX = heightmap[y * width + x1] - heightmap[y * width + x0];
            float dY = heightmap[y1 * width + x] - heightmap[y0 * width + x];

            vec3 normal = {-dX * strength, -dY * strength, 1.0f};
            glm_vec3_normalize(normal);

            // Convert from [-1,1] to [0,255]
            int idx = (y * width + x) * 3;
            data[idx + 0] = (unsigned char)((normal[0] * 0.5f + 0.5f) * 255);
            data[idx + 1] = (unsigned char)((normal[1] * 0.5f + 0.5f) * 255);
            data[idx + 2] = (unsigned char)((normal[2] * 0.5f + 0.5f) * 255);
        }
    }

    free(heightmap);
    return data;
}

/*
 * Helper: smoothstep function
 */
static float smoothstep(float edge0, float edge1, float x) {
    x = fmaxf(0.0f, fminf(1.0f, (x - edge0) / (edge1 - edge0)));
    return x * x * (3.0f - 2.0f * x);
}

/*
 * Generate bark roughness map
 */
static unsigned char* generate_bark_roughness(int width, int height) {
    unsigned char* data = malloc(width * height);
    if (!data)
        return NULL;

    init_perlin(42);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float u = (float)x / width;
            float v = (float)y / height;

            // High base roughness with variation
            float noise = fbm_noise(u * 6.0f, v * 12.0f, 3, 0.5f);
            float crack = worley_noise_2d(u * 6.0f, v * 12.0f, 123);
            crack = fmaxf(0.0f, 1.0f - crack * 2.0f);

            // Rougher on ridges, slightly smoother in worn cracks
            float roughness = 0.85f + noise * 0.1f - crack * 0.15f;
            roughness = fmaxf(0.5f, fminf(1.0f, roughness));

            data[y * width + x] = (unsigned char)(roughness * 255);
        }
    }

    return data;
}

/*
 * Generate leaf albedo with veins
 */
static unsigned char* generate_leaf_albedo(int width, int height) {
    unsigned char* data = malloc(width * height * 4); // RGBA for transparency
    if (!data)
        return NULL;

    init_perlin(789);

    float cx = width * 0.5f;
    float cy = height * 0.5f;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float u = (float)x / width;
            float v = (float)y / height;

            // Leaf shape - ellipse with pointed tip
            float dx = (x - cx) / (width * 0.4f);
            float dy = (y - cy) / (height * 0.45f);

            // Pointed tip at top
            float tip_factor = 1.0f + (v - 0.5f) * 0.5f;
            dx *= tip_factor;

            float dist = sqrtf(dx * dx + dy * dy);

            // Leaf mask with soft edge
            float alpha = 1.0f - smoothstep(0.8f, 1.0f, dist);

            if (alpha < 0.01f) {
                int idx = (y * width + x) * 4;
                data[idx + 0] = 0;
                data[idx + 1] = 0;
                data[idx + 2] = 0;
                data[idx + 3] = 0;
                continue;
            }

            // Base green with variation
            float noise = fbm_noise(u * 8.0f, v * 8.0f, 3, 0.5f);

            float base_r = 0.12f + noise * 0.08f;
            float base_g = 0.45f + noise * 0.15f;
            float base_b = 0.08f + noise * 0.05f;

            // Main vein (center vertical)
            float vein_dist = fabsf(u - 0.5f);
            float main_vein = expf(-vein_dist * vein_dist * 800.0f) * 0.3f;

            // Secondary veins branching from center
            float secondary_veins = 0.0f;
            for (int i = 1; i <= 6; i++) {
                float vein_y = 0.15f + (float)i * 0.12f;
                float vein_angle = 0.4f + (float)i * 0.05f;

                // Left side
                float vy_left = v - vein_y;
                float vx_left = (u - 0.5f) + vy_left * vein_angle;
                float d_left = fabsf(vy_left * cosf(vein_angle) - vx_left * sinf(vein_angle));
                if (u < 0.5f && v > vein_y && v < vein_y + 0.3f) {
                    secondary_veins += expf(-d_left * d_left * 2000.0f) * 0.15f;
                }

                // Right side
                float vx_right = (u - 0.5f) - vy_left * vein_angle;
                float d_right = fabsf(vy_left * cosf(vein_angle) + vx_right * sinf(vein_angle));
                if (u > 0.5f && v > vein_y && v < vein_y + 0.3f) {
                    secondary_veins += expf(-d_right * d_right * 2000.0f) * 0.15f;
                }
            }

            // Veins are slightly darker and more yellow-green
            float vein = main_vein + secondary_veins;
            base_r += vein * 0.1f;
            base_g -= vein * 0.1f;
            base_b -= vein * 0.02f;

            // Edge yellowing
            float edge = smoothstep(0.5f, 0.9f, dist);
            base_r += edge * 0.15f;
            base_g += edge * 0.05f;

            // Clamp
            base_r = fmaxf(0.0f, fminf(1.0f, base_r));
            base_g = fmaxf(0.0f, fminf(1.0f, base_g));
            base_b = fmaxf(0.0f, fminf(1.0f, base_b));

            int idx = (y * width + x) * 4;
            data[idx + 0] = (unsigned char)(base_r * 255);
            data[idx + 1] = (unsigned char)(base_g * 255);
            data[idx + 2] = (unsigned char)(base_b * 255);
            data[idx + 3] = (unsigned char)(alpha * 255);
        }
    }

    return data;
}

/*
 * Generate leaf normal map
 */
static unsigned char* generate_leaf_normal(int width, int height) {
    unsigned char* data = malloc(width * height * 3);
    if (!data)
        return NULL;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float u = (float)x / width;
            float v = (float)y / height;

            // Base normal pointing up
            vec3 normal = {0.0f, 0.0f, 1.0f};

            // Main vein creates a ridge
            float vein_dist = (u - 0.5f);
            float main_vein_bump = expf(-vein_dist * vein_dist * 400.0f);
            normal[0] -= vein_dist * main_vein_bump * 3.0f;

            // Leaf curvature - curves down at edges
            float edge_curve = (u - 0.5f) * 0.3f;
            normal[0] += edge_curve;

            // Subtle surface bumps
            float bump = fbm_noise(u * 20.0f, v * 20.0f, 2, 0.5f) - 0.5f;
            normal[0] += bump * 0.1f;
            normal[1] += bump * 0.1f;

            glm_vec3_normalize(normal);

            int idx = (y * width + x) * 3;
            data[idx + 0] = (unsigned char)((normal[0] * 0.5f + 0.5f) * 255);
            data[idx + 1] = (unsigned char)((normal[1] * 0.5f + 0.5f) * 255);
            data[idx + 2] = (unsigned char)((normal[2] * 0.5f + 0.5f) * 255);
        }
    }

    return data;
}

/*
 * Create OpenGL texture from data
 */
static Texture* create_texture_from_data(unsigned char* data, int width, int height, GLenum format,
                                         const char* name) {
    Texture* tex = create_texture();
    if (!tex)
        return NULL;

    tex->width = width;
    tex->height = height;
    tex->internal_format = format;
    tex->data_format = format;
    tex->filepath = strdup(name);

    glGenTextures(1, &tex->id);
    glBindTexture(GL_TEXTURE_2D, tex->id);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    GLenum internal = (format == GL_RGBA) ? GL_RGBA8 : (format == GL_RED ? GL_R8 : GL_RGB8);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, width, height, 0, format, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);

    glBindTexture(GL_TEXTURE_2D, 0);

    return tex;
}

/*
 * Generate all procedural textures
 */
static Texture* bark_albedo_tex = NULL;
static Texture* bark_normal_tex = NULL;
static Texture* bark_roughness_tex = NULL;
static Texture* leaf_albedo_tex = NULL;
static Texture* leaf_normal_tex = NULL;

static void generate_procedural_textures(void) {
    printf("Generating procedural bark textures...\n");

    unsigned char* bark_albedo_data = generate_bark_albedo(TEXTURE_SIZE, TEXTURE_SIZE);
    if (bark_albedo_data) {
        bark_albedo_tex = create_texture_from_data(bark_albedo_data, TEXTURE_SIZE, TEXTURE_SIZE,
                                                   GL_RGB, "proc_bark_albedo");
        free(bark_albedo_data);
    }

    unsigned char* bark_normal_data = generate_bark_normal(TEXTURE_SIZE, TEXTURE_SIZE);
    if (bark_normal_data) {
        bark_normal_tex = create_texture_from_data(bark_normal_data, TEXTURE_SIZE, TEXTURE_SIZE,
                                                   GL_RGB, "proc_bark_normal");
        free(bark_normal_data);
    }

    unsigned char* bark_rough_data = generate_bark_roughness(TEXTURE_SIZE, TEXTURE_SIZE);
    if (bark_rough_data) {
        bark_roughness_tex = create_texture_from_data(bark_rough_data, TEXTURE_SIZE, TEXTURE_SIZE,
                                                      GL_RED, "proc_bark_roughness");
        free(bark_rough_data);
    }

    printf("Generating procedural leaf textures...\n");

    unsigned char* leaf_albedo_data = generate_leaf_albedo(TEXTURE_SIZE, TEXTURE_SIZE);
    if (leaf_albedo_data) {
        leaf_albedo_tex = create_texture_from_data(leaf_albedo_data, TEXTURE_SIZE, TEXTURE_SIZE,
                                                   GL_RGBA, "proc_leaf_albedo");
        free(leaf_albedo_data);
    }

    unsigned char* leaf_normal_data = generate_leaf_normal(TEXTURE_SIZE, TEXTURE_SIZE);
    if (leaf_normal_data) {
        leaf_normal_tex = create_texture_from_data(leaf_normal_data, TEXTURE_SIZE, TEXTURE_SIZE,
                                                   GL_RGB, "proc_leaf_normal");
        free(leaf_normal_data);
    }

    printf("Procedural textures generated.\n");
}

/*
 * Constants
 */
const unsigned int HEIGHT = 900;
const unsigned int WIDTH = 1400;
const float ORBIT_SENSITIVITY = 0.003f;

/*
 * Tree Parameters
 */
typedef struct {
    int max_depth;
    float trunk_length;
    float trunk_radius;
    int branches_per_node;
    float length_decay;
    float radius_decay;
    float branch_angle;
    float angle_variance;
    float twist;
    int seed;
    int show_leaves;
    float leaf_size;
    int leaves_per_tip;
} TreeParams;

/*
 * Globals
 */
static TreeParams params;
static TreeParams prev_params;
static Material* bark_material = NULL;
static Material* leaf_material = NULL;
static SceneNode* tree_root = NULL;

/*
 * Drag state - stored at drag start
 */
static float orbit_start_phi = 0.0f;
static float orbit_start_theta = 0.0f;
static float free_start_yaw = 0.0f;
static float free_start_pitch = 0.0f;
static float free_look_distance = 500.0f;
static vec3 free_start_look_at = {0.0f, 0.0f, 0.0f};
static vec3 free_start_cam_pos = {0.0f, 0.0f, 0.0f};

/*
 * Random float in range
 */
static float rand_range(float min_val, float max_val) {
    return min_val + (float)rand() / RAND_MAX * (max_val - min_val);
}

/*
 * Generate leaf cluster at branch tip
 */
static void generate_leaf_cluster(SceneNode* parent, vec3 tip_pos, vec3 direction,
                                  const TreeParams* p, Material* mat) {
    if (!p->show_leaves || p->leaves_per_tip <= 0) {
        return;
    }

    SceneNode* leaf_node = create_node();
    set_node_name(leaf_node, "leaves");

    Mesh* mesh = create_mesh();
    mesh->material = mat;

    int num_leaves = p->leaves_per_tip;
    float size = p->leaf_size;

    // Each leaf is a quad: 4 vertices, 6 indices
    mesh->vertex_count = num_leaves * 4;
    mesh->index_count = num_leaves * 6;

    mesh->vertices = malloc(mesh->vertex_count * 3 * sizeof(float));
    mesh->normals = malloc(mesh->vertex_count * 3 * sizeof(float));
    mesh->tex_coords = malloc(mesh->vertex_count * 2 * sizeof(float));
    mesh->indices = malloc(mesh->index_count * sizeof(unsigned int));

    if (!mesh->vertices || !mesh->normals || !mesh->tex_coords || !mesh->indices) {
        free_mesh(mesh);
        free_node(leaf_node);
        return;
    }

    // UV coordinates for quad corners
    float uvs[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};

    for (int i = 0; i < num_leaves; i++) {
        // Random offset from tip
        vec3 offset;
        offset[0] = rand_range(-size * 0.5f, size * 0.5f);
        offset[1] = rand_range(-size * 0.3f, size * 0.5f);
        offset[2] = rand_range(-size * 0.5f, size * 0.5f);

        vec3 center;
        glm_vec3_add(tip_pos, offset, center);

        // Random rotation angles
        float rot_x = rand_range(-0.5f, 0.5f);
        float rot_y = rand_range(0, GLM_PI * 2);
        float rot_z = rand_range(-0.3f, 0.3f);

        // Create a rotated quad
        float half = size * 0.5f;

        // Base quad corners (before rotation)
        vec3 corners[4] = {{-half, 0, -half}, {half, 0, -half}, {half, 0, half}, {-half, 0, half}};

        // Simple rotation
        mat4 rot;
        glm_mat4_identity(rot);
        glm_rotate(rot, rot_x, (vec3){1, 0, 0});
        glm_rotate(rot, rot_y, (vec3){0, 1, 0});
        glm_rotate(rot, rot_z, (vec3){0, 0, 1});

        // Normal (up vector rotated)
        vec3 normal = {0, 1, 0};
        glm_mat4_mulv3(rot, normal, 0.0f, normal);
        glm_vec3_normalize(normal);

        int base_v = i * 4;
        int base_i = i * 6;

        for (int j = 0; j < 4; j++) {
            vec3 rotated;
            glm_mat4_mulv3(rot, corners[j], 0.0f, rotated);

            mesh->vertices[(base_v + j) * 3 + 0] = center[0] + rotated[0];
            mesh->vertices[(base_v + j) * 3 + 1] = center[1] + rotated[1];
            mesh->vertices[(base_v + j) * 3 + 2] = center[2] + rotated[2];

            mesh->normals[(base_v + j) * 3 + 0] = normal[0];
            mesh->normals[(base_v + j) * 3 + 1] = normal[1];
            mesh->normals[(base_v + j) * 3 + 2] = normal[2];

            mesh->tex_coords[(base_v + j) * 2 + 0] = uvs[j][0];
            mesh->tex_coords[(base_v + j) * 2 + 1] = uvs[j][1];
        }

        // Two triangles for quad
        mesh->indices[base_i + 0] = base_v + 0;
        mesh->indices[base_i + 1] = base_v + 1;
        mesh->indices[base_i + 2] = base_v + 2;
        mesh->indices[base_i + 3] = base_v + 0;
        mesh->indices[base_i + 4] = base_v + 2;
        mesh->indices[base_i + 5] = base_v + 3;
    }

    mesh->draw_mode = TRIANGLES;
    calculate_aabb(mesh);

    add_mesh_to_node(leaf_node, mesh);
    add_child_node(parent, leaf_node);
}

/*
 * Generate a branch recursively
 */
static void generate_branch(SceneNode* parent, vec3 base_pos, vec3 direction, float length,
                            float radius, int depth, TreeParams* p) {
    if (depth > p->max_depth || radius < 0.1f || length < 0.5f) {
        return;
    }

    SceneNode* branch_node = create_node();
    char name[32];
    snprintf(name, sizeof(name), "branch_d%d", depth);
    set_node_name(branch_node, name);

    // Create cylinder mesh
    Mesh* mesh = create_mesh();
    mesh->material = bark_material;

    float top_radius = radius * p->radius_decay;

    Cylinder cyl = {.position = {0, 0, 0},
                    .base_radius = radius,
                    .top_radius = top_radius,
                    .height = length,
                    .segments = CYLINDER_SEGMENTS};
    generate_cylinder_to_mesh(mesh, &cyl);
    calculate_aabb(mesh);

    add_mesh_to_node(branch_node, mesh);

    // Build transform: translate to base_pos, then rotate to align Y-axis with direction
    mat4 transform;
    glm_mat4_identity(transform);

    // Translate to base position
    glm_translate(transform, base_pos);

    // Rotate to align (0,1,0) with direction
    vec3 up = {0, 1, 0};
    vec3 dir_norm;
    glm_vec3_copy(direction, dir_norm);
    glm_vec3_normalize(dir_norm);

    float dot = glm_vec3_dot(up, dir_norm);
    if (dot < 0.9999f && dot > -0.9999f) {
        vec3 axis;
        glm_vec3_cross(up, dir_norm, axis);
        glm_vec3_normalize(axis);
        float angle = acosf(dot);
        glm_rotate(transform, angle, axis);
    } else if (dot <= -0.9999f) {
        // Flip 180 degrees
        glm_rotate(transform, GLM_PI, (vec3){1, 0, 0});
    }

    glm_mat4_copy(transform, branch_node->original_transform);

    add_child_node(parent, branch_node);

    // Calculate tip position in local space (will be transformed by parent)
    vec3 tip_local = {0, length, 0};
    vec3 tip_world;
    glm_mat4_mulv3(transform, tip_local, 1.0f, tip_world);

    // If at max depth, add leaves
    if (depth == p->max_depth) {
        generate_leaf_cluster(branch_node, tip_local, dir_norm, p, leaf_material);
        return;
    }

    // Generate child branches
    float new_length = length * p->length_decay;
    float new_radius = radius * p->radius_decay;
    float angle_rad = p->branch_angle * GLM_PI / 180.0f;
    float variance_rad = p->angle_variance * GLM_PI / 180.0f;
    float twist_rad = p->twist * GLM_PI / 180.0f;

    for (int i = 0; i < p->branches_per_node; i++) {
        // Distribute branches around the stem
        float base_twist = twist_rad + (2.0f * GLM_PI * i / p->branches_per_node);
        float actual_angle = angle_rad + rand_range(-variance_rad, variance_rad);

        // Create rotation for branch direction
        mat4 branch_rot;
        glm_mat4_identity(branch_rot);

        // Rotate around Y (twist)
        glm_rotate(branch_rot, base_twist + rand_range(-0.2f, 0.2f), (vec3){0, 1, 0});

        // Tilt away from vertical
        glm_rotate(branch_rot, actual_angle, (vec3){1, 0, 0});

        // Apply to up vector to get new direction
        vec3 new_dir = {0, 1, 0};
        glm_mat4_mulv3(branch_rot, new_dir, 0.0f, new_dir);
        glm_vec3_normalize(new_dir);

        // Branch starts at tip of current branch (in local coordinates)
        generate_branch(branch_node, tip_local, new_dir, new_length, new_radius, depth + 1, p);
    }
}

/*
 * Free all tree nodes (but not lights)
 */
static void free_tree_nodes(SceneNode* root) {
    if (!root) {
        return;
    }

    // Free children that are part of tree (not light nodes)
    for (size_t i = 0; i < root->children_count;) {
        SceneNode* child = root->children[i];
        if (child->light != NULL) {
            // Skip light nodes
            i++;
        } else {
            // Free this branch
            free_node(child);
            // Shift remaining children
            for (size_t j = i; j < root->children_count - 1; j++) {
                root->children[j] = root->children[j + 1];
            }
            root->children_count--;
        }
    }

    tree_root = NULL;
}

/*
 * Regenerate tree
 */
static void regenerate_tree(Scene* scene, TreeParams* p) {
    free_tree_nodes(scene->root_node);

    srand(p->seed);

    tree_root = create_node();
    set_node_name(tree_root, "tree_root");
    add_child_node(scene->root_node, tree_root);

    vec3 origin = {0, 0, 0};
    vec3 up = {0, 1, 0};

    generate_branch(tree_root, origin, up, p->trunk_length, p->trunk_radius, 0, p);

    upload_buffers_to_gpu_for_nodes(scene->root_node);
}

/*
 * Render tree parameters GUI
 */
static void render_tree_gui(const Engine* engine, Scene* scene) {
    struct nk_context* ctx = engine->nk_ctx;

    if (nk_begin(ctx, "Tree Parameters", nk_rect(15, 15, 260, 540),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {

        nk_layout_row_dynamic(ctx, 25, 1);

        // Seed
        nk_label(ctx, "Random Seed", NK_TEXT_LEFT);
        nk_property_int(ctx, "#Seed:", 0, &params.seed, 9999, 1, 1);

        nk_layout_row_dynamic(ctx, 10, 1);
        nk_spacing(ctx, 1);

        // Structure
        nk_label(ctx, "Structure", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 25, 1);
        nk_property_int(ctx, "#Max Depth:", 1, &params.max_depth, 7, 1, 1);
        nk_property_int(ctx, "#Branches:", 1, &params.branches_per_node, 5, 1, 1);

        nk_layout_row_dynamic(ctx, 10, 1);
        nk_spacing(ctx, 1);

        // Dimensions
        nk_label(ctx, "Dimensions", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 25, 1);
        nk_property_float(ctx, "#Trunk Len:", 10.0f, &params.trunk_length, 200.0f, 5.0f, 1.0f);
        nk_property_float(ctx, "#Trunk Rad:", 1.0f, &params.trunk_radius, 30.0f, 1.0f, 1.0f);

        nk_layout_row_dynamic(ctx, 10, 1);
        nk_spacing(ctx, 1);

        // Decay
        nk_label(ctx, "Decay", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 25, 1);
        nk_property_float(ctx, "#Len Decay:", 0.3f, &params.length_decay, 0.95f, 0.02f, 2.0f);
        nk_property_float(ctx, "#Rad Decay:", 0.3f, &params.radius_decay, 0.95f, 0.02f, 2.0f);

        nk_layout_row_dynamic(ctx, 10, 1);
        nk_spacing(ctx, 1);

        // Angles
        nk_label(ctx, "Angles", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 25, 1);
        nk_property_float(ctx, "#Angle:", 5.0f, &params.branch_angle, 90.0f, 2.0f, 1.0f);
        nk_property_float(ctx, "#Variance:", 0.0f, &params.angle_variance, 45.0f, 2.0f, 1.0f);
        nk_property_float(ctx, "#Twist:", 0.0f, &params.twist, 180.0f, 5.0f, 1.0f);

        nk_layout_row_dynamic(ctx, 10, 1);
        nk_spacing(ctx, 1);

        // Leaves
        nk_label(ctx, "Leaves", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 25, 1);
        nk_checkbox_label(ctx, "Show Leaves", &params.show_leaves);
        nk_property_float(ctx, "#Leaf Size:", 1.0f, &params.leaf_size, 30.0f, 1.0f, 1.0f);
        nk_property_int(ctx, "#Leaves/Tip:", 1, &params.leaves_per_tip, 15, 1, 1);
    }
    nk_end(ctx);
}

/*
 * Callbacks
 */
void mouse_button_callback(Engine* engine, int button, int action, int mods) {
    if (engine->input.is_dragging && engine->camera) {
        Camera* camera = engine->camera;

        // Calculate current angles from camera's actual position
        vec3 dir;
        glm_vec3_sub(camera->position, camera->look_at, dir);
        float dist = glm_vec3_norm(dir);

        if (dist > 0.001f) {
            orbit_start_theta = asinf(dir[1] / dist);
            orbit_start_phi = atan2f(dir[2], dir[0]);
            camera->distance = dist;
            free_look_distance = dist;
            free_start_pitch = orbit_start_theta;
            free_start_yaw = orbit_start_phi;
        }
        // Save starting positions for Shift+drag pan
        glm_vec3_copy(camera->look_at, free_start_look_at);
        glm_vec3_copy(camera->position, free_start_cam_pos);
    }
}

void render_scene_callback(Engine* engine, Scene* scene) {
    if (!engine || !scene || !scene->root_node) {
        return;
    }

    Camera* camera = engine->camera;
    if (!camera) {
        return;
    }

    // Render custom GUI first
    render_tree_gui(engine, scene);

    // Check for parameter changes
    if (memcmp(&params, &prev_params, sizeof(TreeParams)) != 0) {
        regenerate_tree(scene, &params);
        memcpy(&prev_params, &params, sizeof(TreeParams));
    }

    // Handle camera - check if not hovering over GUI
    if (engine->input.is_dragging && !nk_window_is_any_hovered(engine->nk_ctx)) {
        if (engine->input.shift_held) {
            // Shift+drag: Pan camera (move both camera and look_at)
            vec3 forward, right_vec, up_vec;
            glm_vec3_sub(free_start_look_at, free_start_cam_pos, forward);
            glm_vec3_crossn(camera->up_vector, forward, right_vec);
            glm_vec3_normalize(right_vec);
            glm_vec3_cross(forward, right_vec, up_vec);
            glm_vec3_normalize(up_vec);

            float pan_speed = free_look_distance * 0.0005f;
            vec3 pan_offset;
            glm_vec3_scale(right_vec, -engine->input.drag_fb_x * pan_speed, pan_offset);
            vec3 up_offset;
            glm_vec3_scale(up_vec, -engine->input.drag_fb_y * pan_speed, up_offset);
            glm_vec3_add(pan_offset, up_offset, pan_offset);

            // Move both camera and look_at from starting positions
            vec3 new_pos, new_look;
            glm_vec3_add(free_start_cam_pos, pan_offset, new_pos);
            glm_vec3_add(free_start_look_at, pan_offset, new_look);
            set_camera_position(camera, new_pos);
            set_camera_look_at(camera, new_look);
        } else {
            // Regular drag: Orbit around look_at point
            float yaw = free_start_yaw - engine->input.drag_fb_x * ORBIT_SENSITIVITY;
            float pitch = free_start_pitch + engine->input.drag_fb_y * ORBIT_SENSITIVITY;

            // Clamp pitch to avoid gimbal lock
            float max_pitch = (float)M_PI_2 - 0.1f;
            if (pitch > max_pitch)
                pitch = max_pitch;
            if (pitch < -max_pitch)
                pitch = -max_pitch;

            // Calculate camera position on sphere around look_at point
            float cos_pitch = cosf(pitch);
            vec3 offset = {free_look_distance * cos_pitch * cosf(yaw),
                           free_look_distance * sinf(pitch),
                           free_look_distance * cos_pitch * sinf(yaw)};

            vec3 new_camera_position;
            glm_vec3_add(camera->look_at, offset, new_camera_position);
            set_camera_position(camera, new_camera_position);
        }
    }

    update_engine_camera_lookat(engine);
    update_engine_camera_perspective(engine);

    // Apply transforms
    Transform t = {.position = {0, 0, 0}, .rotation = {0, 0, 0}, .scale = {1, 1, 1}};
    reset_and_apply_transform(&engine->model_matrix, &t);
    apply_transform_to_nodes(scene->root_node, engine->model_matrix);

    render_current_scene(engine, glfwGetTime());
}

/*
 * Create scene lights
 */
void create_scene_lights(Scene* scene) {
    // Key light - main directional-style light with shadows
    Light* key = create_light();
    set_light_name(key, "key_light");
    set_light_type(key, LIGHT_DIRECTIONAL);
    vec3 key_dir = {-0.5f, -0.8f, -0.3f};
    set_light_direction(key, key_dir);
    vec3 key_pos = {200.0f, 400.0f, 300.0f};
    set_light_original_position(key, key_pos);
    set_light_global_position(key, key_pos);
    set_light_intensity(key, 3.0f);
    set_light_color(key, (vec3){1.0f, 0.95f, 0.9f});
    set_light_cast_shadows(key, true);
    add_light_to_scene(scene, key);

    SceneNode* key_node = create_node();
    set_node_light(key_node, key);
    set_node_name(key_node, "key_light_node");
    add_child_node(scene->root_node, key_node);

    // Fill light - softer, opposite side
    Light* fill = create_light();
    set_light_name(fill, "fill_light");
    set_light_type(fill, LIGHT_DIRECTIONAL);
    vec3 fill_dir = {0.6f, -0.4f, 0.3f};
    set_light_direction(fill, fill_dir);
    vec3 fill_pos = {-250.0f, 200.0f, 200.0f};
    set_light_original_position(fill, fill_pos);
    set_light_global_position(fill, fill_pos);
    set_light_intensity(fill, 1.2f);
    set_light_color(fill, (vec3){0.7f, 0.85f, 1.0f});
    add_light_to_scene(scene, fill);

    SceneNode* fill_node = create_node();
    set_node_light(fill_node, fill);
    set_node_name(fill_node, "fill_light_node");
    add_child_node(scene->root_node, fill_node);

    // Rim light - back light for edge definition
    Light* rim = create_light();
    set_light_name(rim, "rim_light");
    set_light_type(rim, LIGHT_DIRECTIONAL);
    vec3 rim_dir = {0.0f, -0.3f, 0.9f};
    set_light_direction(rim, rim_dir);
    vec3 rim_pos = {0.0f, 300.0f, -300.0f};
    set_light_original_position(rim, rim_pos);
    set_light_global_position(rim, rim_pos);
    set_light_intensity(rim, 1.5f);
    set_light_color(rim, (vec3){1.0f, 0.98f, 0.95f});
    add_light_to_scene(scene, rim);

    SceneNode* rim_node = create_node();
    set_node_light(rim_node, rim);
    set_node_name(rim_node, "rim_light_node");
    add_child_node(scene->root_node, rim_node);
}

/*
 * Main
 */
int main(void) {
    Engine* engine = create_engine("Procedural Tree", WIDTH, HEIGHT);

    if (init_engine(engine) != 0) {
        fprintf(stderr, "Failed to initialize engine\n");
        return -1;
    }

    set_engine_mouse_button_callback(engine, mouse_button_callback);

    // Get shaders
    ShaderProgram* pbr_program = get_engine_shader_program_by_name(engine, "pbr");
    if (!pbr_program) {
        fprintf(stderr, "Failed to get PBR shader\n");
        return -1;
    }

    ShaderProgram* xyz_program = get_engine_shader_program_by_name(engine, "xyz");

    // Create materials
    // Generate procedural textures
    generate_procedural_textures();

    // Create bark material with procedural textures
    bark_material = create_material();
    bark_material->albedo[0] = 1.0f; // White base, texture provides color
    bark_material->albedo[1] = 1.0f;
    bark_material->albedo[2] = 1.0f;
    bark_material->roughness = 0.75f;
    bark_material->metallic = 0.0f;
    bark_material->ao = 1.0f;
    set_material_shader_program(bark_material, pbr_program);

    // Apply bark textures
    if (bark_albedo_tex) {
        set_material_albedo_tex(bark_material, bark_albedo_tex);
    }
    if (bark_normal_tex) {
        set_material_normal_tex(bark_material, bark_normal_tex);
    }
    if (bark_roughness_tex) {
        set_material_roughness_tex(bark_material, bark_roughness_tex);
    }

    // Create leaf material with procedural textures
    leaf_material = create_material();
    leaf_material->albedo[0] = 1.0f; // White base, texture provides color
    leaf_material->albedo[1] = 1.0f;
    leaf_material->albedo[2] = 1.0f;
    leaf_material->roughness = 0.4f; // Leaves have some sheen
    leaf_material->metallic = 0.0f;
    leaf_material->ao = 1.0f;
    set_material_shader_program(leaf_material, pbr_program);

    // Apply leaf textures
    if (leaf_albedo_tex) {
        set_material_albedo_tex(leaf_material, leaf_albedo_tex);
    }
    if (leaf_normal_tex) {
        set_material_normal_tex(leaf_material, leaf_normal_tex);
    }

    // Create camera - positioned to see full tree
    Camera* camera = create_camera();
    vec3 cam_pos = {0.0f, 180.0f, 550.0f};
    vec3 look_at = {0.0f, 100.0f, 0.0f};
    vec3 up = {0.0f, 1.0f, 0.0f};
    set_camera_position(camera, cam_pos);
    set_camera_look_at(camera, look_at);
    set_camera_up_vector(camera, up);
    set_camera_perspective(camera, 0.5f, 1.0f, 5000.0f);
    set_engine_camera(engine, camera);
    camera->distance = 550.0f;

    // Create scene
    Scene* scene = create_scene();
    SceneNode* root = create_node();
    set_node_name(root, "root");
    set_scene_root_node(scene, root);
    add_scene_to_engine(engine, scene);

    if (xyz_program) {
        set_scene_xyz_shader_program(scene, xyz_program);
    }

    create_scene_lights(scene);

    // Initialize tree parameters
    params.max_depth = 4;
    params.trunk_length = 80.0f;
    params.trunk_radius = 8.0f;
    params.branches_per_node = 3;
    params.length_decay = 0.7f;
    params.radius_decay = 0.65f;
    params.branch_angle = 35.0f;
    params.angle_variance = 15.0f;
    params.twist = 45.0f;
    params.seed = 42;
    params.show_leaves = 1;
    params.leaf_size = 8.0f;
    params.leaves_per_tip = 5;

    // Force initial generation
    memset(&prev_params, 0, sizeof(TreeParams));

    set_engine_show_gui(engine, true);
    set_engine_show_fps(engine, true);
    set_engine_show_wireframe(engine, false);
    set_engine_show_xyz(engine, false);

    run_engine_render_loop(engine, render_scene_callback);

    printf("Cleaning up...\n");
    free_engine(engine);

    printf("Goodbye!\n");
    return 0;
}
