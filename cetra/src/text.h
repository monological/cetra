#ifndef _TEXT_H_
#define _TEXT_H_

#include <GL/glew.h>
#include <cglm/cglm.h>
#include <stdbool.h>
#include <uthash.h>

// Forward declarations
struct ShaderProgram;

// Glyph information for a single character
typedef struct GlyphInfo {
    int codepoint;
    float advance_x;
    float left_bearing;

    // Bounding box (pixels at base size)
    float x0, y0, x1, y1;

    // UV coordinates in atlas (normalized 0-1)
    float u0, v0, u1, v1;

    UT_hash_handle hh;
} GlyphInfo;

// Font with glyph atlas
typedef struct Font {
    char* name;
    char* filepath;

    // Atlas texture
    GLuint atlas_texture_id;
    int atlas_width;
    int atlas_height;

    // Glyph data
    GlyphInfo* glyph_cache;
    float base_size;
    float line_height;
    float ascent;
    float descent;

    // SDF parameters
    bool is_sdf;
    float sdf_spread;
    float sdf_scale;

    // stb_truetype data (kept for dynamic glyph generation)
    unsigned char* ttf_buffer;
    void* stb_font_info;

    size_t ref_count;
    UT_hash_handle hh;
} Font;

// Font pool for caching loaded fonts
typedef struct FontPool {
    Font* font_cache;
    Font** fonts;
    size_t font_count;
    size_t font_capacity;
} FontPool;

// Text vertex data (interleaved)
typedef struct TextVertex {
    float x, y, z;
    float u, v;
    float r, g, b, a;
} TextVertex;

// Text alignment
typedef enum { TEXT_ALIGN_LEFT = 0, TEXT_ALIGN_CENTER = 1, TEXT_ALIGN_RIGHT = 2 } TextAlignment;

// Text effects
typedef enum {
    TEXT_EFFECT_NONE = 0,   // Plain SDF text
    TEXT_EFFECT_GLOW = 1,   // Solid color with outer glow
    TEXT_EFFECT_PLASMA = 2, // Animated plasma swirl effect
} TextEffect;

// Effect configuration
typedef struct TextEffectConfig {
    TextEffect type;
    float time;
    union {
        struct {
            float intensity;
            float color[3];
        } glow;
        struct {
            float speed;
            float intensity;
        } plasma;
    };
} TextEffectConfig;

// Text mesh for rendering
typedef struct TextMesh {
    GLuint vao;
    GLuint vbo;
    GLuint ebo;

    TextVertex* vertices;
    unsigned int* indices;
    size_t vertex_count;
    size_t index_count;
    size_t vertex_capacity;

    Font* font;
    float font_size;

    mat4 transform;

    char* text;
    size_t text_length;

    float max_width;
    TextAlignment alignment;

    vec4 color;

    // Per-character animation
    float* char_colors;
    float* char_offsets;

    bool needs_rebuild;
    bool is_screen_space;
} TextMesh;

// Text renderer (engine-level)
typedef struct TextRenderer {
    struct ShaderProgram* text_program;
    FontPool* font_pool;

    mat4 ortho_projection;
    int screen_width;
    int screen_height;
} TextRenderer;

// FontPool
FontPool* create_font_pool(void);
void free_font_pool(FontPool* pool);

// Font loading
Font* load_font(FontPool* pool, const char* filepath, float base_size, bool use_sdf);
Font* get_font(FontPool* pool, const char* name);
void font_retain(Font* font);
void font_release(Font* font);

// Glyph access
GlyphInfo* font_get_glyph(Font* font, int codepoint);

// TextMesh
TextMesh* create_text_mesh(Font* font, const char* text, float font_size);
void free_text_mesh(TextMesh* mesh);

void text_mesh_set_text(TextMesh* mesh, const char* text);
void text_mesh_set_color(TextMesh* mesh, vec4 color);
void text_mesh_set_font_size(TextMesh* mesh, float size);
void text_mesh_set_alignment(TextMesh* mesh, TextAlignment alignment);
void text_mesh_set_max_width(TextMesh* mesh, float width);

// Per-character animation
void text_mesh_set_char_color(TextMesh* mesh, size_t index, vec4 color);
void text_mesh_set_char_offset(TextMesh* mesh, size_t index, vec3 offset);

// Transform
void text_mesh_set_position(TextMesh* mesh, vec3 position);
void text_mesh_set_transform(TextMesh* mesh, mat4 transform);

// Build and upload
void text_mesh_rebuild(TextMesh* mesh);
void text_mesh_upload(TextMesh* mesh);

// Text measurement
float text_measure_width(Font* font, const char* text, float size);
float text_measure_height(const Font* font, const char* text, float size, float max_width);
void text_measure_bounds(Font* font, const char* text, float size, float* out_x0, float* out_y0,
                         float* out_x1, float* out_y1);

// TextRenderer
TextRenderer* create_text_renderer(void);
void free_text_renderer(TextRenderer* renderer);
int init_text_renderer(TextRenderer* renderer, int screen_width, int screen_height);
void text_renderer_update_screen_size(TextRenderer* renderer, int width, int height);

// Rendering
void render_text_2d(TextRenderer* renderer, TextMesh* mesh);
void render_text_2d_effect(TextRenderer* renderer, TextMesh* mesh, const TextEffectConfig* config);
void render_text_3d(TextRenderer* renderer, TextMesh* mesh, mat4 view, mat4 projection);

// Convenience
void draw_text_2d(TextRenderer* renderer, Font* font, const char* text, float x, float y,
                  float size, vec4 color);

#endif // _TEXT_H_
