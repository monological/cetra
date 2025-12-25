#include "text.h"
#include "program.h"
#include "uniform.h"
#include "util.h"
#include "common.h"
#include "ext/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STB_RECT_PACK_IMPLEMENTATION
#include "ext/stb_rect_pack.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "ext/stb_truetype.h"

// Default character set for atlas generation
static const char* DEFAULT_CHARSET = " !\"#$%&'()*+,-./0123456789:;<=>?@"
                                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
                                     "abcdefghijklmnopqrstuvwxyz{|}~";

// --- FontPool ---

FontPool* create_font_pool(void) {
    FontPool* pool = calloc(1, sizeof(FontPool));
    if (!pool)
        return NULL;

    pool->font_capacity = 16;
    pool->fonts = calloc(pool->font_capacity, sizeof(Font*));
    if (!pool->fonts) {
        free(pool);
        return NULL;
    }

    return pool;
}

void free_font_pool(FontPool* pool) {
    if (!pool)
        return;

    // Free all fonts
    Font* font;
    Font* tmp;
    HASH_ITER(hh, pool->font_cache, font, tmp) {
        HASH_DEL(pool->font_cache, font);

        // Free glyph cache
        GlyphInfo* glyph;
        GlyphInfo* gtmp;
        HASH_ITER(hh, font->glyph_cache, glyph, gtmp) {
            HASH_DEL(font->glyph_cache, glyph);
            free(glyph);
        }

        if (font->atlas_texture_id)
            glDeleteTextures(1, &font->atlas_texture_id);
        free(font->name);
        free(font->filepath);
        free(font->ttf_buffer);
        free(font->stb_font_info);
        free(font);
    }

    free(pool->fonts);
    free(pool);
}

// --- Font Loading ---

static void generate_glyph_atlas(Font* font, const char* charset) {
    if (!font || !font->stb_font_info)
        return;

    const stbtt_fontinfo* info = (const stbtt_fontinfo*)font->stb_font_info;
    float scale = stbtt_ScaleForPixelHeight(info, font->base_size);

    // Get font metrics
    int ascent_i, descent_i, line_gap;
    stbtt_GetFontVMetrics(info, &ascent_i, &descent_i, &line_gap);
    font->ascent = ascent_i * scale;
    font->descent = descent_i * scale;
    font->line_height = (ascent_i - descent_i + line_gap) * scale;

    // Calculate atlas size (start with 512, grow if needed)
    int atlas_size = 512;
    int padding = font->is_sdf ? (int)(font->sdf_spread + 1) : 2;
    int on_edge_value = 180;
    float pixel_dist_scale = font->is_sdf ? (float)on_edge_value / font->sdf_spread : 0;

    // Allocate atlas
    unsigned char* atlas_data = calloc(atlas_size * atlas_size, 1);
    if (!atlas_data)
        return;

    int cursor_x = padding;
    int cursor_y = padding;
    int row_height = 0;

    // Process each character
    size_t charset_len = strlen(charset);
    for (size_t i = 0; i < charset_len; i++) {
        int codepoint = (unsigned char)charset[i];

        // Get glyph metrics
        int advance, left_bearing;
        stbtt_GetCodepointHMetrics(info, codepoint, &advance, &left_bearing);

        int x0, y0, x1, y1;
        if (font->is_sdf) {
            int w, h;
            stbtt_GetCodepointSDF(info, scale, codepoint, (int)font->sdf_spread, on_edge_value,
                                  pixel_dist_scale, &w, &h, &x0, &y0);
            // Adjust to get bounding box
            x1 = x0 + w;
            y1 = y0 + h;
        } else {
            stbtt_GetCodepointBitmapBox(info, codepoint, scale, scale, &x0, &y0, &x1, &y1);
        }

        int glyph_w = x1 - x0;
        int glyph_h = y1 - y0;

        // Check if we need to wrap to next row
        if (cursor_x + glyph_w + padding > atlas_size) {
            cursor_x = padding;
            cursor_y += row_height + padding;
            row_height = 0;
        }

        // Check if we need bigger atlas
        if (cursor_y + glyph_h + padding > atlas_size) {
            // Reallocate with larger size
            int new_size = atlas_size * 2;
            unsigned char* new_data = calloc(new_size * new_size, 1);
            if (!new_data) {
                free(atlas_data);
                return;
            }
            // Copy old data
            for (int row = 0; row < atlas_size; row++) {
                memcpy(new_data + row * new_size, atlas_data + row * atlas_size, atlas_size);
            }
            free(atlas_data);
            atlas_data = new_data;
            atlas_size = new_size;
        }

        // Render glyph to atlas
        if (glyph_w > 0 && glyph_h > 0) {
            unsigned char* dest = atlas_data + cursor_y * atlas_size + cursor_x;

            if (font->is_sdf) {
                int w, h, xoff, yoff;
                unsigned char* sdf_data =
                    stbtt_GetCodepointSDF(info, scale, codepoint, (int)font->sdf_spread,
                                          on_edge_value, pixel_dist_scale, &w, &h, &xoff, &yoff);
                if (sdf_data) {
                    for (int row = 0; row < h; row++) {
                        memcpy(dest + row * atlas_size, sdf_data + row * w, w);
                    }
                    stbtt_FreeSDF(sdf_data, NULL);
                }
            } else {
                stbtt_MakeCodepointBitmap(info, dest, glyph_w, glyph_h, atlas_size, scale, scale,
                                          codepoint);
            }
        }

        // Create glyph info
        GlyphInfo* glyph = calloc(1, sizeof(GlyphInfo));
        if (glyph) {
            glyph->codepoint = codepoint;
            glyph->advance_x = advance * scale;
            glyph->left_bearing = left_bearing * scale;
            glyph->x0 = (float)x0;
            glyph->y0 = (float)y0;
            glyph->x1 = (float)x1;
            glyph->y1 = (float)y1;

            // UV coordinates (normalized)
            glyph->u0 = (float)cursor_x / atlas_size;
            glyph->v0 = (float)cursor_y / atlas_size;
            glyph->u1 = (float)(cursor_x + glyph_w) / atlas_size;
            glyph->v1 = (float)(cursor_y + glyph_h) / atlas_size;

            HASH_ADD_INT(font->glyph_cache, codepoint, glyph);
        }

        cursor_x += glyph_w + padding;
        if (glyph_h > row_height)
            row_height = glyph_h;
    }

    // Create OpenGL texture
    glGenTextures(1, &font->atlas_texture_id);
    glBindTexture(GL_TEXTURE_2D, font->atlas_texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, atlas_size, atlas_size, 0, GL_RED, GL_UNSIGNED_BYTE,
                 atlas_data);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Enable single-channel swizzle for proper alpha
    GLint swizzle[] = {GL_RED, GL_RED, GL_RED, GL_RED};
    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);

    font->atlas_width = atlas_size;
    font->atlas_height = atlas_size;

    free(atlas_data);
}

Font* load_font(FontPool* pool, const char* filepath, float base_size, bool use_sdf) {
    if (!pool || !filepath)
        return NULL;

    // Check if already loaded
    Font* existing;
    HASH_FIND_STR(pool->font_cache, filepath, existing);
    if (existing) {
        existing->ref_count++;
        return existing;
    }

    // Load TTF file
    FILE* f = fopen(filepath, "rb");
    if (!f) {
        log_error("Failed to open font file: %s", filepath);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    unsigned char* ttf_buffer = malloc(file_size);
    if (!ttf_buffer) {
        fclose(f);
        return NULL;
    }

    if (fread(ttf_buffer, 1, file_size, f) != (size_t)file_size) {
        free(ttf_buffer);
        fclose(f);
        return NULL;
    }
    fclose(f);

    // Initialize stb_truetype
    stbtt_fontinfo* info = malloc(sizeof(stbtt_fontinfo));
    if (!info) {
        free(ttf_buffer);
        return NULL;
    }

    if (!stbtt_InitFont(info, ttf_buffer, stbtt_GetFontOffsetForIndex(ttf_buffer, 0))) {
        log_error("Failed to initialize font: %s", filepath);
        free(info);
        free(ttf_buffer);
        return NULL;
    }

    // Create font
    Font* font = calloc(1, sizeof(Font));
    if (!font) {
        free(info);
        free(ttf_buffer);
        return NULL;
    }

    font->name = safe_strdup(filepath);
    font->filepath = safe_strdup(filepath);
    font->ttf_buffer = ttf_buffer;
    font->stb_font_info = info;
    font->base_size = base_size;
    font->is_sdf = use_sdf;
    font->sdf_spread = use_sdf ? 8.0f : 0.0f;
    font->sdf_scale = 1.0f;
    font->ref_count = 1;

    // Generate atlas
    generate_glyph_atlas(font, DEFAULT_CHARSET);

    // Add to cache
    HASH_ADD_KEYPTR(hh, pool->font_cache, font->name, strlen(font->name), font);

    // Add to array
    if (pool->font_count >= pool->font_capacity) {
        pool->font_capacity *= 2;
        pool->fonts = realloc(pool->fonts, pool->font_capacity * sizeof(Font*));
    }
    pool->fonts[pool->font_count++] = font;

    log_info("Loaded font: %s (size=%.0f, sdf=%d, atlas=%dx%d)", filepath, base_size, use_sdf,
             font->atlas_width, font->atlas_height);

    return font;
}

Font* get_font(FontPool* pool, const char* name) {
    if (!pool || !name)
        return NULL;

    Font* font;
    HASH_FIND_STR(pool->font_cache, name, font);
    return font;
}

void font_retain(Font* font) {
    if (font)
        font->ref_count++;
}

void font_release(Font* font) {
    if (font && font->ref_count > 0)
        font->ref_count--;
}

GlyphInfo* font_get_glyph(Font* font, int codepoint) {
    if (!font)
        return NULL;

    GlyphInfo* glyph;
    HASH_FIND_INT(font->glyph_cache, &codepoint, glyph);
    return glyph;
}

// --- TextMesh ---

TextMesh* create_text_mesh(Font* font, const char* text, float font_size) {
    if (!font)
        return NULL;

    TextMesh* mesh = calloc(1, sizeof(TextMesh));
    if (!mesh)
        return NULL;

    mesh->font = font;
    font_retain(font);

    mesh->font_size = font_size;
    mesh->text = text ? safe_strdup(text) : safe_strdup("");
    mesh->text_length = strlen(mesh->text);

    glm_vec4_one(mesh->color);
    glm_mat4_identity(mesh->transform);

    mesh->alignment = TEXT_ALIGN_LEFT;
    mesh->max_width = 0.0f;
    mesh->is_screen_space = true;
    mesh->needs_rebuild = true;

    // Create GPU buffers
    glGenVertexArrays(1, &mesh->vao);
    glGenBuffers(1, &mesh->vbo);
    glGenBuffers(1, &mesh->ebo);

    return mesh;
}

void free_text_mesh(TextMesh* mesh) {
    if (!mesh)
        return;

    if (mesh->vao)
        glDeleteVertexArrays(1, &mesh->vao);
    if (mesh->vbo)
        glDeleteBuffers(1, &mesh->vbo);
    if (mesh->ebo)
        glDeleteBuffers(1, &mesh->ebo);

    font_release(mesh->font);
    free(mesh->text);
    free(mesh->vertices);
    free(mesh->indices);
    free(mesh->char_colors);
    free(mesh->char_offsets);
    free(mesh);
}

void text_mesh_set_text(TextMesh* mesh, const char* text) {
    if (!mesh)
        return;

    free(mesh->text);
    mesh->text = text ? safe_strdup(text) : safe_strdup("");
    mesh->text_length = strlen(mesh->text);
    mesh->needs_rebuild = true;

    // Reset per-character arrays
    free(mesh->char_colors);
    free(mesh->char_offsets);
    mesh->char_colors = NULL;
    mesh->char_offsets = NULL;
}

void text_mesh_set_color(TextMesh* mesh, vec4 color) {
    if (!mesh)
        return;
    glm_vec4_copy(color, mesh->color);
    mesh->needs_rebuild = true;
}

void text_mesh_set_font_size(TextMesh* mesh, float size) {
    if (!mesh)
        return;
    mesh->font_size = size;
    mesh->needs_rebuild = true;
}

void text_mesh_set_alignment(TextMesh* mesh, TextAlignment alignment) {
    if (!mesh)
        return;
    mesh->alignment = alignment;
    mesh->needs_rebuild = true;
}

void text_mesh_set_max_width(TextMesh* mesh, float width) {
    if (!mesh)
        return;
    mesh->max_width = width;
    mesh->needs_rebuild = true;
}

void text_mesh_set_char_color(TextMesh* mesh, size_t index, vec4 color) {
    if (!mesh || index >= mesh->text_length)
        return;

    // Allocate char_colors if needed
    if (!mesh->char_colors) {
        mesh->char_colors = malloc(mesh->text_length * 4 * sizeof(float));
        if (!mesh->char_colors)
            return;
        // Initialize with default color
        for (size_t i = 0; i < mesh->text_length; i++) {
            memcpy(&mesh->char_colors[i * 4], mesh->color, 4 * sizeof(float));
        }
    }

    memcpy(&mesh->char_colors[index * 4], color, 4 * sizeof(float));
    mesh->needs_rebuild = true;
}

void text_mesh_set_char_offset(TextMesh* mesh, size_t index, vec3 offset) {
    if (!mesh || index >= mesh->text_length)
        return;

    // Allocate char_offsets if needed
    if (!mesh->char_offsets) {
        mesh->char_offsets = calloc(mesh->text_length * 3, sizeof(float));
        if (!mesh->char_offsets)
            return;
    }

    memcpy(&mesh->char_offsets[index * 3], offset, 3 * sizeof(float));
    mesh->needs_rebuild = true;
}

void text_mesh_set_position(TextMesh* mesh, vec3 position) {
    if (!mesh)
        return;
    glm_mat4_identity(mesh->transform);
    glm_translate(mesh->transform, position);
}

void text_mesh_set_transform(TextMesh* mesh, mat4 transform) {
    if (!mesh)
        return;
    glm_mat4_copy(transform, mesh->transform);
}

void text_mesh_rebuild(TextMesh* mesh) {
    if (!mesh || !mesh->font || !mesh->text)
        return;

    Font* font = mesh->font;
    float scale = mesh->font_size / font->base_size;

    size_t char_count = mesh->text_length;
    size_t required_verts = char_count * 4;
    size_t required_indices = char_count * 6;

    // Reallocate if needed
    if (required_verts > mesh->vertex_capacity) {
        mesh->vertices = realloc(mesh->vertices, required_verts * sizeof(TextVertex));
        mesh->indices = realloc(mesh->indices, required_indices * sizeof(unsigned int));
        mesh->vertex_capacity = required_verts;
    }

    if (!mesh->vertices || !mesh->indices)
        return;

    // Start cursor at font ascent so position represents top-left of text
    float cursor_x = 0.0f;
    float cursor_y = font->ascent * scale;
    size_t vi = 0;
    size_t ii = 0;
    size_t char_idx = 0;

    for (size_t i = 0; i < char_count; i++) {
        int codepoint = (unsigned char)mesh->text[i];

        // Handle newlines (Y-down: increase y to go down)
        if (codepoint == '\n') {
            cursor_x = 0.0f;
            cursor_y += font->line_height * scale;
            char_idx++;
            continue;
        }

        GlyphInfo* glyph = font_get_glyph(font, codepoint);
        if (!glyph) {
            char_idx++;
            continue;
        }

        // Calculate quad corners
        float x0 = cursor_x + glyph->x0 * scale;
        float y0 = cursor_y - glyph->y1 * scale;
        float x1 = cursor_x + glyph->x1 * scale;
        float y1 = cursor_y - glyph->y0 * scale;

        // Apply per-character offset
        if (mesh->char_offsets) {
            x0 += mesh->char_offsets[char_idx * 3 + 0];
            y0 += mesh->char_offsets[char_idx * 3 + 1];
            x1 += mesh->char_offsets[char_idx * 3 + 0];
            y1 += mesh->char_offsets[char_idx * 3 + 1];
        }

        // Get color
        float r, g, b, a;
        if (mesh->char_colors) {
            r = mesh->char_colors[char_idx * 4 + 0];
            g = mesh->char_colors[char_idx * 4 + 1];
            b = mesh->char_colors[char_idx * 4 + 2];
            a = mesh->char_colors[char_idx * 4 + 3];
        } else {
            r = mesh->color[0];
            g = mesh->color[1];
            b = mesh->color[2];
            a = mesh->color[3];
        }

        // Create quad (top-left, top-right, bottom-right, bottom-left)
        // Y-down screen coords: y0=top, y1=bottom
        unsigned int base = (unsigned int)vi;

        mesh->vertices[vi++] = (TextVertex){x0, y0, 0.0f, glyph->u0, glyph->v0, r, g, b, a};
        mesh->vertices[vi++] = (TextVertex){x1, y0, 0.0f, glyph->u1, glyph->v0, r, g, b, a};
        mesh->vertices[vi++] = (TextVertex){x1, y1, 0.0f, glyph->u1, glyph->v1, r, g, b, a};
        mesh->vertices[vi++] = (TextVertex){x0, y1, 0.0f, glyph->u0, glyph->v1, r, g, b, a};

        // Indices (two triangles)
        mesh->indices[ii++] = base + 0;
        mesh->indices[ii++] = base + 1;
        mesh->indices[ii++] = base + 2;
        mesh->indices[ii++] = base + 0;
        mesh->indices[ii++] = base + 2;
        mesh->indices[ii++] = base + 3;

        cursor_x += glyph->advance_x * scale;
        char_idx++;
    }

    mesh->vertex_count = vi;
    mesh->index_count = ii;
    mesh->needs_rebuild = false;
}

void text_mesh_upload(TextMesh* mesh) {
    if (!mesh || mesh->vertex_count == 0)
        return;

    glBindVertexArray(mesh->vao);

    // Upload vertex data
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo);
    glBufferData(GL_ARRAY_BUFFER, mesh->vertex_count * sizeof(TextVertex), mesh->vertices,
                 GL_DYNAMIC_DRAW);

    // Position (location 0)
    glVertexAttribPointer(GL_ATTR_POSITION, 3, GL_FLOAT, GL_FALSE, sizeof(TextVertex),
                          (void*)offsetof(TextVertex, x));
    glEnableVertexAttribArray(GL_ATTR_POSITION);

    // TexCoord (location 2)
    glVertexAttribPointer(GL_ATTR_TEXCOORD, 2, GL_FLOAT, GL_FALSE, sizeof(TextVertex),
                          (void*)offsetof(TextVertex, u));
    glEnableVertexAttribArray(GL_ATTR_TEXCOORD);

    // Color (location 5)
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(TextVertex),
                          (void*)offsetof(TextVertex, r));
    glEnableVertexAttribArray(5);

    // Upload index data
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh->index_count * sizeof(unsigned int), mesh->indices,
                 GL_DYNAMIC_DRAW);

    glBindVertexArray(0);
}

// --- Text Measurement ---

float text_measure_width(Font* font, const char* text, float size) {
    if (!font || !text)
        return 0.0f;

    float scale = size / font->base_size;
    float width = 0.0f;
    float max_width = 0.0f;

    for (const char* p = text; *p; p++) {
        if (*p == '\n') {
            if (width > max_width)
                max_width = width;
            width = 0.0f;
            continue;
        }

        const GlyphInfo* glyph = font_get_glyph(font, (unsigned char)*p);
        if (glyph) {
            width += glyph->advance_x * scale;
        }
    }

    return width > max_width ? width : max_width;
}

float text_measure_height(const Font* font, const char* text, float size, float max_width) {
    if (!font || !text)
        return 0.0f;

    (void)max_width; // TODO: implement word wrapping

    float scale = size / font->base_size;
    int line_count = 1;

    for (const char* p = text; *p; p++) {
        if (*p == '\n')
            line_count++;
    }

    return line_count * font->line_height * scale;
}

void text_measure_bounds(Font* font, const char* text, float size, float* out_x0, float* out_y0,
                         float* out_x1, float* out_y1) {
    if (!font || !text) {
        if (out_x0)
            *out_x0 = 0;
        if (out_y0)
            *out_y0 = 0;
        if (out_x1)
            *out_x1 = 0;
        if (out_y1)
            *out_y1 = 0;
        return;
    }

    float scale = size / font->base_size;
    float cursor_x = 0.0f;
    float cursor_y = font->ascent * scale;

    float min_x = 1e9f, min_y = 1e9f;
    float max_x = -1e9f, max_y = -1e9f;
    bool has_glyphs = false;

    for (const char* p = text; *p; p++) {
        int codepoint = (unsigned char)*p;

        if (codepoint == '\n') {
            cursor_x = 0.0f;
            cursor_y += font->line_height * scale;
            continue;
        }

        const GlyphInfo* glyph = font_get_glyph(font, codepoint);
        if (!glyph)
            continue;

        // Match text_mesh_rebuild vertex calculation exactly
        float x0 = cursor_x + glyph->x0 * scale;
        float y0 = cursor_y - glyph->y1 * scale;
        float x1 = cursor_x + glyph->x1 * scale;
        float y1 = cursor_y - glyph->y0 * scale;

        if (x0 < min_x)
            min_x = x0;
        if (y0 < min_y)
            min_y = y0;
        if (x1 > max_x)
            max_x = x1;
        if (y1 > max_y)
            max_y = y1;
        has_glyphs = true;

        cursor_x += glyph->advance_x * scale;
    }

    if (!has_glyphs) {
        min_x = min_y = max_x = max_y = 0;
    }

    if (out_x0)
        *out_x0 = min_x;
    if (out_y0)
        *out_y0 = min_y;
    if (out_x1)
        *out_x1 = max_x;
    if (out_y1)
        *out_y1 = max_y;
}

// --- TextRenderer ---

TextRenderer* create_text_renderer(void) {
    TextRenderer* renderer = calloc(1, sizeof(TextRenderer));
    if (!renderer)
        return NULL;

    renderer->font_pool = create_font_pool();
    if (!renderer->font_pool) {
        free(renderer);
        return NULL;
    }

    return renderer;
}

void free_text_renderer(TextRenderer* renderer) {
    if (!renderer)
        return;

    free_font_pool(renderer->font_pool);
    // Note: text_program is owned by engine's program cache, don't free here
    free(renderer);
}

int init_text_renderer(TextRenderer* renderer, int screen_width, int screen_height) {
    if (!renderer)
        return -1;

    renderer->screen_width = screen_width;
    renderer->screen_height = screen_height;

    // Create orthographic projection (origin at top-left, Y down)
    glm_ortho(0.0f, (float)screen_width, (float)screen_height, 0.0f, -1.0f, 1.0f,
              renderer->ortho_projection);

    return 0;
}

void text_renderer_update_screen_size(TextRenderer* renderer, int width, int height) {
    if (!renderer)
        return;

    renderer->screen_width = width;
    renderer->screen_height = height;

    glm_ortho(0.0f, (float)width, (float)height, 0.0f, -1.0f, 1.0f, renderer->ortho_projection);
}

void render_text_2d_effect(TextRenderer* renderer, TextMesh* mesh, const TextEffectConfig* config) {
    if (!renderer || !mesh || !renderer->text_program || mesh->vertex_count == 0 || !config)
        return;

    if (mesh->needs_rebuild) {
        text_mesh_rebuild(mesh);
        text_mesh_upload(mesh);
    }

    ShaderProgram* program = renderer->text_program;
    glUseProgram(program->id);

    UniformManager* u = program->uniforms;

    // Set uniforms
    uniform_set_mat4(u, "projection", (float*)renderer->ortho_projection);
    uniform_set_mat4(u, "model", (float*)mesh->transform);
    uniform_set_int(u, "isScreenSpace", 1);
    uniform_set_int(u, "useSDF", mesh->font->is_sdf ? 1 : 0);
    uniform_set_float(u, "sdfEdge", 0.5f);
    uniform_set_float(u, "sdfSmoothing", 0.1f);

    // Effect uniforms
    uniform_set_int(u, "effectType", (int)config->type);
    uniform_set_float(u, "time", config->time);

    // Effect-specific uniforms
    if (config->type == TEXT_EFFECT_GLOW) {
        uniform_set_float(u, "glowIntensity", config->glow.intensity);
        uniform_set_vec3(u, "glowColor", (float*)config->glow.color);
    } else if (config->type == TEXT_EFFECT_PLASMA) {
        uniform_set_float(u, "plasmaSpeed", config->plasma.speed);
        uniform_set_float(u, "plasmaIntensity", config->plasma.intensity);
    }

    // Bind font atlas
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mesh->font->atlas_texture_id);
    uniform_set_int(u, "fontAtlas", 0);

    // Enable blending
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    // Draw
    glBindVertexArray(mesh->vao);
    glDrawElements(GL_TRIANGLES, (GLsizei)mesh->index_count, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    // Restore state
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

void render_text_2d(TextRenderer* renderer, TextMesh* mesh) {
    TextEffectConfig config = {.type = TEXT_EFFECT_NONE, .time = 0.0f};
    render_text_2d_effect(renderer, mesh, &config);
}

void render_text_3d(TextRenderer* renderer, TextMesh* mesh, mat4 view, mat4 projection) {
    if (!renderer || !mesh || !renderer->text_program || mesh->vertex_count == 0)
        return;

    if (mesh->needs_rebuild) {
        text_mesh_rebuild(mesh);
        text_mesh_upload(mesh);
    }

    ShaderProgram* program = renderer->text_program;
    glUseProgram(program->id);

    UniformManager* u = program->uniforms;

    // Set uniforms
    uniform_set_mat4(u, "projection", (float*)projection);
    uniform_set_mat4(u, "view", (float*)view);
    uniform_set_mat4(u, "model", (float*)mesh->transform);
    uniform_set_int(u, "isScreenSpace", 0);
    uniform_set_int(u, "useSDF", mesh->font->is_sdf ? 1 : 0);
    uniform_set_float(u, "sdfEdge", 0.5f);
    uniform_set_float(u, "sdfSmoothing", 0.1f);

    // Bind font atlas
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mesh->font->atlas_texture_id);
    uniform_set_int(u, "fontAtlas", 0);

    // Enable blending
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Draw
    glBindVertexArray(mesh->vao);
    glDrawElements(GL_TRIANGLES, (GLsizei)mesh->index_count, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
}

void draw_text_2d(TextRenderer* renderer, Font* font, const char* text, float x, float y,
                  float size, vec4 color) {
    if (!renderer || !font || !text)
        return;

    TextMesh* mesh = create_text_mesh(font, text, size);
    if (!mesh)
        return;

    text_mesh_set_color(mesh, color);
    text_mesh_set_position(mesh, (vec3){x, y, 0.0f});
    text_mesh_rebuild(mesh);
    text_mesh_upload(mesh);
    render_text_2d(renderer, mesh);
    free_text_mesh(mesh);
}
