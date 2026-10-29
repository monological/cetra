#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "canvas_record.h"
#include "engine.h"
#include "ext/log.h"
#include "ext/uthash.h"
#include "program.h"
#include "text.h"
#include "uniform.h"
#include "util.h"

// The most tags a table holds, three texels each in a buffer texture.
#define CANVAS_TAG_LIMIT  (1u << 22)
#define CANVAS_TAG_TEXELS 3
// The most styles a canvas holds: a record carries its style in 16 bits.
#define CANVAS_STYLE_LIMIT 65536u
// The smallest range a tile is given, so one growing a few shapes at a time is
// not moved each time.
#define CANVAS_TILE_MIN_SLOTS 16u
// The furthest a tile's origin may be from a view's centre and still be drawn,
// so its offset and every record's offset from it add in int32 without
// overflowing. A view wider than this is far past any detail anyway.
#define CANVAS_VIEW_REACH (INT64_C(1) << 30)
// An outline ring's gap from its shape and its width, in window points.
#define CANVAS_RING_GAP_PT   1.0
#define CANVAS_RING_WIDTH_PT 1.5

// The units the canvas program reads its tables from: its own ledger. A font's
// atlas is on a unit of its own, CANVAS_UNIT_FONTS plus its slot, since GL 4.1
// indexes a sampler array only by a value the same across a draw.
enum {
    CANVAS_UNIT_TILES = 0,
    CANVAS_UNIT_STYLES = 1,
    CANVAS_UNIT_TAGS = 2,
    CANVAS_UNIT_GLYPHS = 3,
    CANVAS_UNIT_FONTS = 4,
    CANVAS_UNIT_END = CANVAS_UNIT_FONTS + CANVAS_FONT_MAX,
};

/*
 * Tags.
 */

struct CanvasTags {
    CanvasTagState* states;
    uint32_t count; // one past the largest tag set, at least 1
    size_t cap;
    // Tags whose state draws a ring, and that raise: a layer's pass for either
    // is drawn only while some tag asks for it.
    uint32_t outlined, raised;
    uint64_t generation;
    GLuint buffer, texture;
    uint32_t capacity;             // tags the GPU buffer holds
    uint32_t dirty_from, dirty_to; // the tags written since the last upload, [from, to)
};

CanvasTags* create_canvas_tags(void) {
    CanvasTags* tags = calloc(1, sizeof(CanvasTags));
    if (!tags) {
        log_error("canvas: out of memory making a tag table");
        return NULL;
    }
    tags->count = 1;
    return tags;
}

void free_canvas_tags(CanvasTags* tags) {
    if (!tags)
        return;
    gl_delete_texture(&tags->texture);
    if (tags->buffer)
        glDeleteBuffers(1, &tags->buffer);
    free(tags->states);
    free(tags);
}

bool canvas_tags_set(CanvasTags* tags, uint32_t tag, CanvasTagState state) {
    if (!tags)
        return false;
    if (tag == 0 || tag >= CANVAS_TAG_LIMIT) {
        log_error("canvas: tag %u is refused (1 to %u)", tag, CANVAS_TAG_LIMIT - 1);
        return false;
    }
    const size_t had = tags->cap;
    if (!grow_array((void**)&tags->states, &tags->cap, (size_t)tag + 1, sizeof(CanvasTagState),
                    256))
        return false;
    memset(tags->states + had, 0, (tags->cap - had) * sizeof(CanvasTagState));
    const CanvasTagState was = tags->states[tag];
    tags->outlined += (state.outline != 0) - (was.outline != 0);
    tags->raised += state.raised - was.raised;
    tags->states[tag] = state;
    if (tag + 1 > tags->count)
        tags->count = tag + 1;
    if (tags->dirty_from == tags->dirty_to) {
        tags->dirty_from = tag, tags->dirty_to = tag + 1;
    } else {
        tags->dirty_from = tag < tags->dirty_from ? tag : tags->dirty_from;
        tags->dirty_to = tag + 1 > tags->dirty_to ? tag + 1 : tags->dirty_to;
    }
    tags->generation++;
    return true;
}

CanvasTagState canvas_tags_get(const CanvasTags* tags, uint32_t tag) {
    if (!tags || tag == 0 || tag >= tags->cap)
        return (CanvasTagState){0};
    return tags->states[tag];
}

static void _rgba_bytes(uint32_t rgba, uint8_t out[4]) {
    out[0] = (uint8_t)(rgba >> 24), out[1] = (uint8_t)(rgba >> 16);
    out[2] = (uint8_t)(rgba >> 8), out[3] = (uint8_t)rgba;
}

// A tag's three texels; tag 0, never set, reads as none, as does a table
// with nothing set at all.
static void _tag_texels(const CanvasTags* tags, uint32_t tag, uint8_t out[12]) {
    const CanvasTagState s = tag < tags->cap ? tags->states[tag] : (CanvasTagState){0};
    _rgba_bytes(s.colour, out);
    _rgba_bytes(s.outline, out + 4);
    out[8] = (uint8_t)lroundf(glm_clamp(s.dim, 0.0f, 1.0f) * 255.0f);
    out[9] = s.hidden ? 255 : 0;
    out[10] = s.raised ? 255 : 0;
    out[11] = 0;
}

// Brings the GPU's copy up to date: the written range alone, or, when the
// table has outgrown its buffer, a buffer twice the size and every tag in it.
// The shader reads no tag past the count, so the rest of a new buffer is left.
static void _tags_upload(CanvasTags* tags) {
    if (!tags->buffer) {
        glGenBuffers(1, &tags->buffer);
        glGenTextures(1, &tags->texture);
    }
    glBindBuffer(GL_TEXTURE_BUFFER, tags->buffer);
    if (tags->count > tags->capacity) {
        uint32_t capacity = tags->capacity ? tags->capacity : 256;
        while (capacity < tags->count)
            capacity *= 2;
        glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)capacity * 4 * CANVAS_TAG_TEXELS, NULL,
                     GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_BUFFER, tags->texture);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8, tags->buffer);
        tags->capacity = capacity;
        tags->dirty_from = 0, tags->dirty_to = tags->count;
    }
    if (tags->dirty_from < tags->dirty_to) {
        const uint32_t n = tags->dirty_to - tags->dirty_from;
        uint8_t* texels = malloc((size_t)n * 4 * CANVAS_TAG_TEXELS);
        if (!texels) {
            log_error("canvas: out of memory uploading a tag table");
            return;
        }
        for (uint32_t i = 0; i < n; i++)
            _tag_texels(tags, tags->dirty_from + i, texels + (size_t)i * 4 * CANVAS_TAG_TEXELS);
        glBufferSubData(GL_TEXTURE_BUFFER, (GLintptr)tags->dirty_from * 4 * CANVAS_TAG_TEXELS,
                        (GLsizeiptr)n * 4 * CANVAS_TAG_TEXELS, texels);
        free(texels);
    }
    tags->dirty_from = tags->dirty_to = 0;
}

/*
 * The canvas's own state.
 */

// A tile coordinate, shared by every layer: its entry in the tile table, and
// the bound of everything any layer holds in it, grown and never shrunk.
typedef struct CanvasCoord {
    int64_t key[2];
    uint32_t index;
    int64_t min[2], max[2];
    UT_hash_handle hh;
} CanvasCoord;

// The two kinds of thing a layer keeps, each an instance in a buffer of its
// own: shape records, a quad each, and fill triangles. Each carries its head,
// net, object and tile as `u`, where the second column says.
enum { CANVAS_SHAPES = 0, CANVAS_FILLS = 1, CANVAS_STORES = 2 };
static const size_t CANVAS_ITEM_SIZE[CANVAS_STORES] = {sizeof(CanvasRecord), sizeof(CanvasFill)};
static const size_t CANVAS_ITEM_U[CANVAS_STORES] = {offsetof(CanvasRecord, u),
                                                    offsetof(CanvasFill, u)};

// A tile's items of one kind, and the range of its layer's buffer they upload
// into.
typedef struct CanvasRun {
    void* items;
    size_t count, cap;
    uint32_t slot, slots; // slots 0 = no range yet
    bool dirty;
} CanvasRun;

// A layer's buffer for one kind of item: its tiles' ranges one after another.
typedef struct CanvasStore {
    GLuint vao, vbo;
    uint32_t end;      // slots in use, live and dead; what a draw covers
    uint32_t live;     // items in them, as of the last upload
    uint32_t capacity; // slots the buffer holds
    bool dirty;        // a tile's run wants uploading
} CanvasStore;

typedef struct CanvasTile {
    int64_t key[2];
    CanvasRun runs[CANVAS_STORES];
    CanvasStore* stores; // its layer's, which a run of it changing makes dirty
    UT_hash_handle hh;
} CanvasTile;

typedef struct CanvasLayerData {
    CanvasTile* tiles;
    CanvasStore stores[CANVAS_STORES];
} CanvasLayerData;

// Item i's head, net, object and tile.
static uint32_t* _item_u(int store, void* items, size_t i) {
    return (uint32_t*)((char*)items + i * CANVAS_ITEM_SIZE[store] + CANVAS_ITEM_U[store]);
}

// The tiles an object has records in, which a remove looks through.
typedef struct CanvasObject {
    uint32_t object;
    CanvasTile** tiles;
    size_t count, cap;
    UT_hash_handle hh;
} CanvasObject;

typedef struct CanvasData {
    int tile_shift;
    uint64_t generation; // its own; canvas_generation adds its tags'
    CanvasLayer* layers[CANVAS_LAYER_MAX];
    uint32_t layer_count;

    uint32_t* styles;
    uint32_t style_count;
    size_t style_cap;
    bool styles_dirty;

    CanvasTags* tags;
    bool owns_tags;

    CanvasCoord* coords;
    CanvasCoord** coord_by_index;
    uint32_t coord_count;
    size_t coord_cap;

    CanvasObject* objects;

    // The fonts its strings are drawn with, in the order first applied: a
    // glyph record's slot.
    const Font* fonts[CANVAS_FONT_MAX];
    uint32_t font_count;
    bool glyphs_dirty;

    // GL, made at the first draw.
    bool gl_ready, gl_failed;
    ShaderProgram* program; // shapes
    ShaderProgram* fill_program;
    ShaderProgram* composite;
    GLuint quad_vao, quad_vbo;
    GLuint style_buffer, style_texture;
    GLuint tile_buffer, tile_texture;
    GLuint glyph_buffer, glyph_texture;
    int32_t* tile_table;
    size_t tile_table_cap;
    GLuint target_fbo, target_texture;
    int target_w, target_h;
    void* scratch; // a tile's range, its items then dead slots
    size_t scratch_bytes;
} CanvasData;

int canvas_tile_shift(const Canvas* canvas) {
    return canvas && canvas->data ? canvas->data->tile_shift : CANVAS_TILE_SHIFT;
}

Canvas* create_canvas(Engine* engine, const CanvasDesc* desc) {
    if (!engine) {
        log_error("canvas: a canvas needs its engine");
        return NULL;
    }
    const int shift = desc && desc->tile_shift ? desc->tile_shift : CANVAS_TILE_SHIFT;
    if (shift < 4 || shift > 28) {
        log_error("canvas: a tile shift of %d is refused (4 to 28)", shift);
        return NULL;
    }
    Canvas* canvas = calloc(1, sizeof(Canvas));
    CanvasData* data = calloc(1, sizeof(CanvasData));
    if (!canvas || !data) {
        log_error("canvas: out of memory making a canvas");
        free(canvas);
        free(data);
        return NULL;
    }
    canvas->engine = engine;
    canvas->data = data;
    canvas->text_min_px = 3.0f;
    data->tile_shift = shift;
    data->tags = desc && desc->tags ? desc->tags : create_canvas_tags();
    data->owns_tags = !(desc && desc->tags);
    if (!data->tags) {
        free_canvas(canvas);
        return NULL;
    }
    canvas_add_style(canvas, 0xffffffffu);
    return canvas;
}

static void _free_tiles(CanvasLayerData* ld) {
    CanvasTile *tile, *next;
    HASH_ITER(hh, ld->tiles, tile, next) {
        HASH_DEL(ld->tiles, tile);
        for (int s = 0; s < CANVAS_STORES; s++)
            free(tile->runs[s].items);
        free(tile);
    }
}

void free_canvas(Canvas* canvas) {
    if (!canvas)
        return;
    CanvasData* d = canvas->data;
    if (d) {
        for (uint32_t i = 0; i < d->layer_count; i++) {
            CanvasLayer* layer = d->layers[i];
            CanvasLayerData* ld = layer->data;
            _free_tiles(ld);
            for (int s = 0; s < CANVAS_STORES; s++) {
                if (ld->stores[s].vbo)
                    glDeleteBuffers(1, &ld->stores[s].vbo);
                if (ld->stores[s].vao)
                    glDeleteVertexArrays(1, &ld->stores[s].vao);
            }
            free(ld);
            free(layer->name);
            free(layer);
        }
        CanvasCoord *coord, *next_coord;
        HASH_ITER(hh, d->coords, coord, next_coord) {
            HASH_DEL(d->coords, coord);
            free(coord);
        }
        CanvasObject *object, *next_object;
        HASH_ITER(hh, d->objects, object, next_object) {
            HASH_DEL(d->objects, object);
            free(object->tiles);
            free(object);
        }
        if (d->owns_tags)
            free_canvas_tags(d->tags);
        if (d->quad_vbo)
            glDeleteBuffers(1, &d->quad_vbo);
        if (d->quad_vao)
            glDeleteVertexArrays(1, &d->quad_vao);
        if (d->style_buffer)
            glDeleteBuffers(1, &d->style_buffer);
        if (d->tile_buffer)
            glDeleteBuffers(1, &d->tile_buffer);
        if (d->glyph_buffer)
            glDeleteBuffers(1, &d->glyph_buffer);
        gl_delete_texture(&d->style_texture);
        gl_delete_texture(&d->tile_texture);
        gl_delete_texture(&d->glyph_texture);
        gl_delete_texture(&d->target_texture);
        gl_delete_fbo(&d->target_fbo);
        free(d->coord_by_index);
        free(d->styles);
        free(d->tile_table);
        free(d->scratch);
        free(d);
    }
    free(canvas);
}

CanvasLayer* canvas_add_layer(Canvas* canvas, const char* name) {
    if (!canvas)
        return NULL;
    CanvasData* d = canvas->data;
    if (d->layer_count == CANVAS_LAYER_MAX) {
        log_error("canvas: a layer past the %d a canvas holds is refused", CANVAS_LAYER_MAX);
        return NULL;
    }
    CanvasLayer* layer = calloc(1, sizeof(CanvasLayer));
    CanvasLayerData* ld = calloc(1, sizeof(CanvasLayerData));
    char* copy = strdup(name ? name : "");
    if (!layer || !ld || !copy) {
        log_error("canvas: out of memory adding a layer");
        free(layer);
        free(ld);
        free(copy);
        return NULL;
    }
    layer->index = d->layer_count;
    layer->name = copy;
    layer->data = ld;
    layer->opacity = 1.0f;
    layer->order = (int)layer->index;
    d->layers[d->layer_count++] = layer;
    canvas->data->generation++;
    return layer;
}

uint16_t canvas_add_style(Canvas* canvas, uint32_t rgba) {
    if (!canvas)
        return 0;
    CanvasData* d = canvas->data;
    if (d->style_count == CANVAS_STYLE_LIMIT) {
        log_error("canvas: a style past the %u a canvas holds is refused", CANVAS_STYLE_LIMIT);
        return 0;
    }
    if (!grow_array((void**)&d->styles, &d->style_cap, (size_t)d->style_count + 1, sizeof(uint32_t),
                    16))
        return 0;
    d->styles[d->style_count] = rgba;
    d->styles_dirty = true;
    canvas->data->generation++;
    return (uint16_t)d->style_count++;
}

void canvas_set_style(Canvas* canvas, uint16_t style, uint32_t rgba) {
    if (!canvas)
        return;
    CanvasData* d = canvas->data;
    if (style >= d->style_count) {
        log_error("canvas: style %u is not one of the canvas's %u", style, d->style_count);
        return;
    }
    d->styles[style] = rgba;
    d->styles_dirty = true;
    canvas->data->generation++;
}

CanvasTags* canvas_tags(const Canvas* canvas) {
    return canvas ? canvas->data->tags : NULL;
}

uint64_t canvas_generation(const Canvas* canvas) {
    return canvas ? canvas->data->generation + canvas->data->tags->generation : 0;
}

/*
 * Applying an edit.
 */

static CanvasCoord* _coord(CanvasData* d, int64_t tx, int64_t ty) {
    const int64_t key[2] = {tx, ty};
    CanvasCoord* coord = NULL;
    HASH_FIND(hh, d->coords, key, sizeof(key), coord);
    if (coord)
        return coord;
    if (!grow_array((void**)&d->coord_by_index, &d->coord_cap, (size_t)d->coord_count + 1,
                    sizeof(CanvasCoord*), 256))
        return NULL;
    coord = calloc(1, sizeof(CanvasCoord));
    if (!coord)
        return NULL;
    coord->key[0] = tx, coord->key[1] = ty;
    coord->index = d->coord_count;
    coord->min[0] = coord->min[1] = INT64_MAX;
    coord->max[0] = coord->max[1] = INT64_MIN;
    HASH_ADD(hh, d->coords, key, sizeof(coord->key), coord);
    d->coord_by_index[d->coord_count++] = coord;
    return coord;
}

static CanvasTile* _tile(CanvasLayerData* ld, int64_t tx, int64_t ty) {
    const int64_t key[2] = {tx, ty};
    CanvasTile* tile = NULL;
    HASH_FIND(hh, ld->tiles, key, sizeof(key), tile);
    if (tile)
        return tile;
    tile = calloc(1, sizeof(CanvasTile));
    if (!tile)
        return NULL;
    tile->key[0] = tx, tile->key[1] = ty;
    tile->stores = ld->stores;
    HASH_ADD(hh, ld->tiles, key, sizeof(tile->key), tile);
    return tile;
}

static bool _run_push(CanvasRun* run, int store, const void* item) {
    const size_t size = CANVAS_ITEM_SIZE[store];
    if (!grow_array(&run->items, &run->cap, run->count + 1, size, 16))
        return false;
    memcpy((char*)run->items + run->count++ * size, item, size);
    run->dirty = true;
    return true;
}

// Notes that an object has records in a tile, once.
static void _note_object(CanvasData* d, uint32_t object, CanvasTile* tile) {
    if (object == 0)
        return;
    CanvasObject* entry = NULL;
    HASH_FIND(hh, d->objects, &object, sizeof(object), entry);
    if (!entry) {
        entry = calloc(1, sizeof(CanvasObject));
        if (!entry)
            return;
        entry->object = object;
        HASH_ADD(hh, d->objects, object, sizeof(entry->object), entry);
    }
    for (size_t i = entry->count; i-- > 0;)
        if (entry->tiles[i] == tile)
            return;
    if (!grow_array((void**)&entry->tiles, &entry->cap, entry->count + 1, sizeof(CanvasTile*), 4))
        return;
    entry->tiles[entry->count++] = tile;
}

static bool _owns_layer(const CanvasData* d, const CanvasLayer* layer) {
    return layer && layer->index < d->layer_count && d->layers[layer->index] == layer;
}

// Adds an item of `store` -- a record or a triangle, its tile index filled in
// here -- to its layer's tile, and grows the tile coordinate's bound. False
// for a layer of another canvas or a style it does not have, as for memory.
static bool _add(CanvasData* d, int store, CanvasLayer* layer, int64_t tx, int64_t ty,
                 const int64_t min[2], const int64_t max[2], void* item) {
    uint32_t* u = _item_u(store, item, 0);
    const uint16_t style = (uint16_t)(u[0] >> 16);
    CanvasCoord* coord = style < d->style_count ? _coord(d, tx, ty) : NULL;
    CanvasTile* tile = coord ? _tile(layer->data, tx, ty) : NULL;
    if (!tile)
        return false;
    u[3] = coord->index;
    if (!_run_push(&tile->runs[store], store, item))
        return false;
    for (int axis = 0; axis < 2; axis++) {
        coord->min[axis] = min[axis] < coord->min[axis] ? min[axis] : coord->min[axis];
        coord->max[axis] = max[axis] > coord->max[axis] ? max[axis] : coord->max[axis];
    }
    layer->data->stores[store].dirty = true;
    _note_object(d, u[2], tile);
    return true;
}

// Takes an object's items out of every tile it was in, keeping the order of
// the rest within each tile.
static void _remove_object(CanvasData* d, uint32_t object) {
    CanvasObject* entry = NULL;
    HASH_FIND(hh, d->objects, &object, sizeof(object), entry);
    if (!entry)
        return;
    for (size_t i = 0; i < entry->count; i++) {
        for (int s = 0; s < CANVAS_STORES; s++) {
            CanvasRun* run = &entry->tiles[i]->runs[s];
            const size_t size = CANVAS_ITEM_SIZE[s];
            size_t kept = 0;
            for (size_t r = 0; r < run->count; r++) {
                if (_item_u(s, run->items, r)[2] == object)
                    continue;
                if (kept != r)
                    memcpy((char*)run->items + kept * size, (char*)run->items + r * size, size);
                kept++;
            }
            if (kept != run->count) {
                run->count = kept;
                run->dirty = true;
                entry->tiles[i]->stores[s].dirty = true;
            }
        }
    }
    HASH_DEL(d->objects, entry);
    free(entry->tiles);
    free(entry);
}

// Empties a layer, and takes its tiles out of the object index, deleting an
// object left in no tile: a layer refilled each frame with fresh objects
// would otherwise grow the index for as long as the canvas lives.
static void _clear_layer(CanvasData* d, CanvasLayer* layer) {
    if (!_owns_layer(d, layer))
        return;
    CanvasLayerData* ld = layer->data;
    CanvasTile *tile, *next;
    HASH_ITER(hh, ld->tiles, tile, next) {
        for (int s = 0; s < CANVAS_STORES; s++) {
            if (tile->runs[s].count) {
                tile->runs[s].count = 0;
                tile->runs[s].dirty = true;
                ld->stores[s].dirty = true;
            }
        }
    }
    CanvasObject *entry, *next_entry;
    HASH_ITER(hh, d->objects, entry, next_entry) {
        size_t kept = 0;
        for (size_t i = 0; i < entry->count; i++)
            if (entry->tiles[i]->stores != ld->stores)
                entry->tiles[kept++] = entry->tiles[i];
        entry->count = kept;
        if (kept == 0) {
            HASH_DEL(d->objects, entry);
            free(entry->tiles);
            free(entry);
        }
    }
}

// A font's slot among the canvas's, taking the next free one for a font not
// yet drawn with; -1 when all are taken.
static int _font_slot(CanvasData* d, const Font* font) {
    for (uint32_t i = 0; i < d->font_count; i++)
        if (d->fonts[i] == font)
            return (int)i;
    if (d->font_count == CANVAS_FONT_MAX)
        return -1;
    d->fonts[d->font_count] = font;
    d->glyphs_dirty = true;
    return (int)d->font_count++;
}

void canvas_apply(Canvas* canvas, CanvasEdit* edit) {
    if (!canvas || !edit)
        return;
    CanvasData* d = canvas->data;
    if (edit->tile_shift != d->tile_shift) {
        log_error("canvas: an edit made for another canvas's tiles is refused");
    } else {
        uint32_t refused = 0, fonts_refused = 0;
        for (size_t s = 0; s < edit->step_count; s++) {
            const CanvasStep* step = &edit->steps[s];
            switch (step->kind) {
                case CANVAS_STEP_ADD:
                    for (size_t i = 0; i < step->count; i++) {
                        CanvasPiece* p = &edit->pieces[step->first + i];
                        // A font slot is spent only on a glyph that will be added.
                        const uint16_t style = (uint16_t)(p->record.u[0] >> 16);
                        if (!_owns_layer(d, p->layer) || style >= d->style_count) {
                            refused++;
                            continue;
                        }
                        if (p->font) {
                            const int slot = _font_slot(d, p->font);
                            if (slot < 0) {
                                fonts_refused++;
                                continue;
                            }
                            p->record.p[3] = slot;
                        }
                        refused += !_add(d, CANVAS_SHAPES, p->layer, p->tx, p->ty, p->min, p->max,
                                         &p->record);
                    }
                    break;
                case CANVAS_STEP_ADD_FILLS:
                    for (size_t i = 0; i < step->count; i++) {
                        CanvasFillPiece* p = &edit->fills[step->first + i];
                        refused +=
                            !_owns_layer(d, p->layer) || !_add(d, CANVAS_FILLS, p->layer, p->tx,
                                                               p->ty, p->min, p->max, &p->fill);
                    }
                    break;
                case CANVAS_STEP_REMOVE_OBJECT:
                    _remove_object(d, step->object);
                    break;
                case CANVAS_STEP_CLEAR_LAYER:
                    _clear_layer(d, step->layer);
                    break;
            }
        }
        if (refused)
            log_error("canvas: %u shape(s) refused: a layer of another canvas, a style it does "
                      "not have, or out of memory",
                      refused);
        if (fonts_refused)
            log_error("canvas: %u glyph(s) refused: a canvas draws with at most %d fonts",
                      fonts_refused, CANVAS_FONT_MAX);
        if (edit->step_count)
            canvas->data->generation++;
    }
    edit->piece_count = 0;
    edit->fill_count = 0;
    edit->step_count = 0;
}

/*
 * The GPU side.
 */

// The engine's program of this name, built and registered by the first canvas
// that asks, so every canvas shares one.
static ShaderProgram* _shared_program(Engine* engine, const char* name,
                                      ShaderProgram* (*create)(void)) {
    ShaderProgram* program = engine_find_program(engine, name);
    if (!program) {
        program = create();
        if (program)
            engine_add_program(engine, program);
    }
    return program;
}

static bool _gl_ready(Canvas* canvas) {
    CanvasData* d = canvas->data;
    if (d->gl_ready)
        return true;
    if (d->gl_failed)
        return false;
    Engine* engine = canvas->engine;
    d->program = _shared_program(engine, CETRA_PROGRAM_CANVAS, create_canvas_program);
    d->fill_program =
        _shared_program(engine, CETRA_PROGRAM_CANVAS_FILL, create_canvas_fill_program);
    d->composite =
        _shared_program(engine, CETRA_PROGRAM_CANVAS_COMPOSITE, create_canvas_composite_program);
    if (!d->program || !d->fill_program || !d->composite) {
        log_error("canvas: its programs did not build, so it draws nothing");
        d->gl_failed = true;
        return false;
    }
    // The units the programs read their tables from, which never change.
    static const char* FONTS[CANVAS_FONT_MAX] = {"uFont0", "uFont1", "uFont2", "uFont3"};
    const ShaderProgram* programs[2] = {d->program, d->fill_program};
    for (int i = 0; i < 2; i++) {
        UniformManager* u = programs[i]->uniforms;
        glUseProgram(programs[i]->id);
        uniform_set_int(u, "uTiles", CANVAS_UNIT_TILES);
        uniform_set_int(u, "uStyles", CANVAS_UNIT_STYLES);
        uniform_set_int(u, "uTags", CANVAS_UNIT_TAGS);
        uniform_set_int(u, "uGlyphs", CANVAS_UNIT_GLYPHS);
        for (int slot = 0; slot < CANVAS_FONT_MAX; slot++)
            uniform_set_int(u, FONTS[slot], CANVAS_UNIT_FONTS + slot);
    }
    create_fullscreen_quad_vao(&d->quad_vao, &d->quad_vbo);
    glGenBuffers(1, &d->style_buffer);
    glGenTextures(1, &d->style_texture);
    glGenBuffers(1, &d->tile_buffer);
    glGenTextures(1, &d->tile_texture);
    glGenBuffers(1, &d->glyph_buffer);
    glGenTextures(1, &d->glyph_texture);
    d->glyphs_dirty = true;
    d->gl_ready = true;
    return true;
}

// An integer attribute of every instance, `count` int32s or uint32s at `offset`.
static void _instance_attrib(GLuint index, GLint count, GLenum type, GLsizei stride,
                             size_t offset) {
    glEnableVertexAttribArray(index);
    glVertexAttribIPointer(index, count, type, stride, (const void*)offset);
    glVertexAttribDivisor(index, 1);
}

// A store's vertex array. Each item is an instance: a shape record of a unit
// quad, a fill of a triangle.
static void _store_gl(CanvasStore* store, int kind) {
    if (store->vao)
        return;
    glGenVertexArrays(1, &store->vao);
    glGenBuffers(1, &store->vbo);
    glBindVertexArray(store->vao);
    glBindBuffer(GL_ARRAY_BUFFER, store->vbo);
    if (kind == CANVAS_SHAPES) {
        const GLsizei stride = (GLsizei)sizeof(CanvasRecord);
        _instance_attrib(0, 4, GL_INT, stride, offsetof(CanvasRecord, p));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride,
                              (const void*)offsetof(CanvasRecord, f));
        glVertexAttribDivisor(1, 1);
        _instance_attrib(2, 4, GL_UNSIGNED_INT, stride, offsetof(CanvasRecord, u));
    } else {
        // The corners as an ivec4 and an ivec2, the nine outline points as four
        // ivec4 and an ivec2, then u.
        const GLsizei stride = (GLsizei)sizeof(CanvasFill);
        const size_t p = offsetof(CanvasFill, p), near = offsetof(CanvasFill, near);
        _instance_attrib(0, 4, GL_INT, stride, p);
        _instance_attrib(1, 2, GL_INT, stride, p + 4 * sizeof(int32_t));
        for (GLuint k = 0; k < 4; k++)
            _instance_attrib(2 + k, 4, GL_INT, stride, near + k * 4 * sizeof(int32_t));
        _instance_attrib(6, 2, GL_INT, stride, near + 16 * sizeof(int32_t));
        _instance_attrib(7, 4, GL_UNSIGNED_INT, stride, offsetof(CanvasFill, u));
    }
    glBindVertexArray(0);
}

static uint32_t _slots_for(size_t count) {
    if (count == 0)
        return 0;
    const uint32_t slots = (uint32_t)(count + count / 2);
    return slots < CANVAS_TILE_MIN_SLOTS ? CANVAS_TILE_MIN_SLOTS : slots;
}

// Writes a range of a store's buffer: `count` items, then dead slots to the
// end of the range. A dead slot is all zero: of kind NONE, which draws nothing.
static void _upload_range(Canvas* canvas, int store, uint32_t slot, uint32_t slots,
                          const void* items, size_t count) {
    CanvasData* d = canvas->data;
    const size_t size = CANVAS_ITEM_SIZE[store];
    const size_t bytes = (size_t)slots * size;
    if (slots == 0)
        return;
    if (bytes > d->scratch_bytes) {
        void* grown = realloc(d->scratch, bytes);
        if (!grown) {
            log_error("canvas: out of memory uploading a tile");
            return;
        }
        d->scratch = grown;
        d->scratch_bytes = bytes;
    }
    if (count)
        memcpy(d->scratch, items, count * size);
    memset((char*)d->scratch + count * size, 0, bytes - count * size);
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)slot * (GLintptr)size, (GLsizeiptr)bytes,
                    d->scratch);
    canvas->uploads++;
    canvas->bytes_uploaded += bytes;
}

/*
 * A store's buffer holds its tiles' ranges one after another. A tile that
 * outgrows its range takes a new one at the end and its old one is written
 * dead; when the end passes the buffer, or the dead ranges outweigh the live
 * ones, every tile is laid out again from the start and uploaded whole.
 */
static void _store_upload(Canvas* canvas, CanvasLayerData* ld, int s) {
    CanvasStore* store = &ld->stores[s];
    if (!store->dirty)
        return;
    _store_gl(store, s);
    glBindBuffer(GL_ARRAY_BUFFER, store->vbo);

    // Where the end would be with every outgrown tile moved there, and what is
    // live: decided before anything is written, so a layout from the start
    // does not first write the old ranges dead.
    uint32_t live = 0, end = store->end;
    size_t items = 0;
    CanvasTile *tile, *next;
    HASH_ITER(hh, ld->tiles, tile, next) {
        const CanvasRun* run = &tile->runs[s];
        const bool moves = run->dirty && run->count > run->slots;
        if (moves)
            end += _slots_for(run->count);
        live += moves ? _slots_for(run->count) : run->slots;
        items += run->count;
    }
    store->live = (uint32_t)items;

    if (end <= store->capacity && end <= 2 * live + 4096) {
        HASH_ITER(hh, ld->tiles, tile, next) {
            CanvasRun* run = &tile->runs[s];
            if (run->dirty && run->count > run->slots) {
                if (run->slots)
                    _upload_range(canvas, s, run->slot, run->slots, NULL, 0);
                run->slots = _slots_for(run->count);
                run->slot = store->end;
                store->end += run->slots;
            }
        }
    } else {
        end = 0;
        HASH_ITER(hh, ld->tiles, tile, next) {
            CanvasRun* run = &tile->runs[s];
            run->slots = _slots_for(run->count);
            run->slot = end;
            end += run->slots;
            run->dirty = true;
        }
        store->end = end;
        store->capacity = end + end / 2 + 1024;
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)store->capacity * (GLsizeiptr)CANVAS_ITEM_SIZE[s],
                     NULL, GL_DYNAMIC_DRAW);
    }

    HASH_ITER(hh, ld->tiles, tile, next) {
        CanvasRun* run = &tile->runs[s];
        if (!run->dirty)
            continue;
        _upload_range(canvas, s, run->slot, run->slots, run->items, run->count);
        run->dirty = false;
    }
    store->dirty = false;
}

static void _styles_upload(CanvasData* d) {
    if (!d->styles_dirty)
        return;
    uint8_t* texels = malloc((size_t)d->style_count * 4);
    if (!texels)
        return;
    for (uint32_t s = 0; s < d->style_count; s++)
        _rgba_bytes(d->styles[s], texels + (size_t)s * 4);
    glBindBuffer(GL_TEXTURE_BUFFER, d->style_buffer);
    glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)d->style_count * 4, texels, GL_DYNAMIC_DRAW);
    glBindTexture(GL_TEXTURE_BUFFER, d->style_texture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8, d->style_buffer);
    free(texels);
    d->styles_dirty = false;
}

/*
 * The glyph table, laid out as canvas_constants.glsl states: for each font,
 * first its own numbers -- its em in atlas pixels, the value at a glyph's
 * outline, how fast the value falls a pixel and how far it reaches -- then for
 * each codepoint its box in ems from the pen, y up, and its rectangle of the
 * atlas, bottom row first so both mix the same way across the quad.
 */
static void _glyphs_upload(CanvasData* d) {
    if (!d->glyphs_dirty)
        return;
    const size_t per_font = (size_t)CANVAS_GLYPH_CODEPOINTS * 2;
    float* texels = calloc((size_t)CANVAS_FONT_MAX * per_font * 4, sizeof(float));
    if (!texels)
        return;
    for (uint32_t slot = 0; slot < d->font_count; slot++) {
        const Font* font = d->fonts[slot];
        const float em = font->em_size;
        float* base = texels + slot * per_font * 4;
        const float numbers[4] = {em, font->sdf_edge, font->sdf_falloff, font->sdf_spread};
        memcpy(base, numbers, sizeof(numbers));
        for (int cp = 1; cp < CANVAS_GLYPH_CODEPOINTS; cp++) {
            const GlyphInfo* g = font_get_glyph(font, cp);
            if (!g)
                continue;
            const float glyph[8] = {g->x0 / em, -g->y1 / em, g->x1 / em, -g->y0 / em,
                                    g->u0,      g->v1,       g->u1,      g->v0};
            memcpy(base + (size_t)cp * 8, glyph, sizeof(glyph));
        }
    }
    glBindBuffer(GL_TEXTURE_BUFFER, d->glyph_buffer);
    glBufferData(GL_TEXTURE_BUFFER,
                 (GLsizeiptr)((size_t)CANVAS_FONT_MAX * per_font * 4 * sizeof(float)), texels,
                 GL_STATIC_DRAW);
    glBindTexture(GL_TEXTURE_BUFFER, d->glyph_texture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, d->glyph_buffer);
    free(texels);
    d->glyphs_dirty = false;
}

// Each tile coordinate's offset from the view's integer centre, and whether
// anything in it can reach the view. One table a view, since each view has
// its own centre.
static void _tiles_upload(CanvasData* d, const CanvasView* view, double half_x, double half_y) {
    const uint32_t n = d->coord_count ? d->coord_count : 1;
    if (!grow_array((void**)&d->tile_table, &d->tile_table_cap, n, 4 * sizeof(int32_t), 256))
        return;
    memset(d->tile_table, 0, (size_t)n * 4 * sizeof(int32_t));
    const int64_t side = (int64_t)1 << d->tile_shift;
    for (uint32_t i = 0; i < d->coord_count; i++) {
        const CanvasCoord* coord = d->coord_by_index[i];
        const int64_t dx = coord->key[0] * side - view->centre.x;
        const int64_t dy = coord->key[1] * side - view->centre.y;
        if (dx <= -CANVAS_VIEW_REACH || dx >= CANVAS_VIEW_REACH || dy <= -CANVAS_VIEW_REACH ||
            dy >= CANVAS_VIEW_REACH || coord->min[0] > coord->max[0])
            continue;
        // The bound against the view's rectangle, both relative to the view's
        // integer centre so the doubles stay small.
        const double x0 = (double)(coord->min[0] - view->centre.x) - view->fraction[0];
        const double x1 = (double)(coord->max[0] - view->centre.x) - view->fraction[0];
        const double y0 = (double)(coord->min[1] - view->centre.y) - view->fraction[1];
        const double y1 = (double)(coord->max[1] - view->centre.y) - view->fraction[1];
        if (x1 < -half_x || x0 > half_x || y1 < -half_y || y0 > half_y)
            continue;
        int32_t* entry = d->tile_table + (size_t)i * 4;
        entry[0] = (int32_t)dx;
        entry[1] = (int32_t)dy;
        entry[2] = 1;
    }
    glBindBuffer(GL_TEXTURE_BUFFER, d->tile_buffer);
    glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)n * 4 * (GLsizeiptr)sizeof(int32_t), d->tile_table,
                 GL_STREAM_DRAW);
    glBindTexture(GL_TEXTURE_BUFFER, d->tile_texture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32I, d->tile_buffer);
}

// The view's target, grown to hold the largest view drawn and never shrunk.
static bool _target(CanvasData* d, int w, int h) {
    if (d->target_fbo && w <= d->target_w && h <= d->target_h)
        return true;
    const int tw = w > d->target_w ? w : d->target_w;
    const int th = h > d->target_h ? h : d->target_h;
    gl_delete_texture(&d->target_texture);
    gl_delete_fbo(&d->target_fbo);
    if (!gl_color_fbo_create(tw, th, GL_RGBA8, &d->target_fbo, &d->target_texture)) {
        log_error("canvas: its %dx%d target is incomplete, so it draws nothing", tw, th);
        gl_delete_texture(&d->target_texture);
        gl_delete_fbo(&d->target_fbo);
        d->target_w = d->target_h = 0;
        return false;
    }
    d->target_w = tw, d->target_h = th;
    return true;
}

// The layers a view draws, in order: shown, lowest order first, ties in the
// order added; the active one after them; then any kept above it.
static uint32_t _draw_order(const CanvasData* d, const CanvasView* view, const CanvasLayer** out) {
    const bool every = canvas_layer_set_empty(&view->shown);
    const bool active = view->active && _owns_layer(d, view->active);
    uint32_t n = 0;
    for (int above = 0; above < 2; above++) {
        const uint32_t first = n;
        for (uint32_t i = 0; i < d->layer_count; i++) {
            const CanvasLayer* layer = d->layers[i];
            if (layer == view->active || layer->above_active != (above == 1) ||
                !(every || canvas_layer_set_has(&view->shown, layer)))
                continue;
            uint32_t at = n++;
            while (at > first && out[at - 1]->order > layer->order) {
                out[at] = out[at - 1];
                at--;
            }
            out[at] = layer;
        }
        if (above == 0 && active)
            out[n++] = view->active;
    }
    return n;
}

/*
 * A layer, resolved in the view's target with its alpha as scratch, in the
 * passes canvas_pass.glsl describes: the largest coverage into alpha (max is
 * idempotent where a cut shape or a polygon's corner covers a pixel twice),
 * then the colour passes, each painting by alpha and zeroing it, so the first
 * shape to reach a pixel spends its whole coverage. Every pixel a layer covers
 * is painted by one of its passes, so each layer leaves alpha at zero for the
 * next and none needs clearing. Blend and mask changes do not end a render
 * pass on a tiled GPU, where a framebuffer switch per layer would store and
 * reload the whole target.
 */
static void _draw_layer(const CanvasData* d, const CanvasLayer* layer) {
    const CanvasStore* fills = &layer->data->stores[CANVAS_FILLS];
    const CanvasStore* shapes = &layer->data->stores[CANVAS_SHAPES];
    if (!fills->live && !shapes->live)
        return;
    const float opacity = glm_clamp(layer->opacity, 0.0f, 1.0f);
    for (int pass = 0; pass < 5; pass++) {
        // The raised pass only while some tag raises, and the ring pass only
        // while one draws a ring.
        if ((pass == 1 && !d->tags->raised) || (pass == 4 && !d->tags->outlined))
            continue;
        if (pass == 0) {
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
            glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ONE);
        } else if (pass == 1 || (pass == 2 && !d->tags->raised)) {
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_ZERO, GL_ZERO);
        }
        // Fills first, then shapes, among them the outer half of each fill's
        // edge ramp and every ring, a polygon's included.
        if (fills->live && pass < 4) {
            glUseProgram(d->fill_program->id);
            uniform_set_int(d->fill_program->uniforms, "uPass", pass);
            uniform_set_float(d->fill_program->uniforms, "uOpacity", opacity);
            glBindVertexArray(fills->vao);
            glDrawArraysInstanced(GL_TRIANGLES, 0, 3, (GLsizei)fills->end);
        }
        if (shapes->live) {
            glUseProgram(d->program->id);
            uniform_set_int(d->program->uniforms, "uPass", pass);
            uniform_set_float(d->program->uniforms, "uOpacity", opacity);
            glBindVertexArray(shapes->vao);
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)shapes->end);
        }
    }
}

// What both canvas programs read of the view.
static void _view_uniforms(const ShaderProgram* program, const Canvas* canvas,
                           const CanvasView* view, const float size[2], float ppu,
                           const float ring[2]) {
    UniformManager* u = program->uniforms;
    glUseProgram(program->id);
    const float fraction[2] = {(float)view->fraction[0], (float)view->fraction[1]};
    uniform_set_vec2(u, "uFraction", fraction);
    uniform_set_vec2(u, "uSize", size);
    uniform_set_vec2(u, "uRing", ring);
    uniform_set_float(u, "uPixelsPerUnit", ppu);
    uniform_set_float(u, "uMirror", view->mirror_x ? -1.0f : 1.0f);
    uniform_set_float(u, "uShapeMinPx", canvas->shape_min_px);
    uniform_set_float(u, "uTextMinPx", canvas->text_min_px);
    uniform_set_int(u, "uTagCount", (int)canvas->data->tags->count);
}

void canvas_draw(Canvas* canvas, const CanvasView* view) {
    if (!canvas || !view || view->points_per_unit <= 0.0)
        return;
    const Engine* engine = canvas->engine;
    CanvasData* d = canvas->data;
    if (engine->win_width <= 0 || engine->win_height <= 0)
        return;

    // The rectangle in framebuffer pixels, from the bottom as GL counts.
    const double sx = (double)engine->fb_width / (double)engine->win_width;
    const double sy = (double)engine->fb_height / (double)engine->win_height;
    const long x0 = lround(view->x * sx), x1 = lround((view->x + view->width) * sx);
    const long y0 = lround(view->y * sy), y1 = lround((view->y + view->height) * sy);
    const int w = (int)(x1 - x0), h = (int)(y1 - y0);
    if (w <= 0 || h <= 0)
        return;
    const int gl_y = engine->fb_height - (int)y1;
    const double ppu = view->points_per_unit * sx;
    // A ring from one point outside a shape to two and a half: at least a
    // pixel out, where the shape's own ramp has ended.
    const float gap = (float)fmax(1.0, CANVAS_RING_GAP_PT * sx);
    const float ring[2] = {gap, gap + (float)(CANVAS_RING_WIDTH_PT * sx)};

    GLPassState pass = gl_pass_begin();
    if (_gl_ready(canvas) && _target(d, w, h)) {
        for (uint32_t i = 0; i < d->layer_count; i++)
            for (int s = 0; s < CANVAS_STORES; s++)
                _store_upload(canvas, d->layers[i]->data, s);
        _styles_upload(d);
        _tags_upload(d->tags);
        _glyphs_upload(d);
        // A tile is drawn when anything in it reaches the view, a ring and
        // its ramp included.
        const double margin = 2.0 + ring[1] + 0.5;
        _tiles_upload(d, view, (w * 0.5 + margin) / ppu, (h * 0.5 + margin) / ppu);

        // The background, and an alpha of zero for the first layer's scratch.
        uint8_t bg[4];
        _rgba_bytes(view->background, bg);
        glBindFramebuffer(GL_FRAMEBUFFER, d->target_fbo);
        glViewport(0, 0, w, h);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(bg[0] / 255.0f, bg[1] / 255.0f, bg[2] / 255.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glEnable(GL_BLEND);
        const float size[2] = {(float)w, (float)h};
        _view_uniforms(d->program, canvas, view, size, (float)ppu, ring);
        _view_uniforms(d->fill_program, canvas, view, size, (float)ppu, ring);
        glActiveTexture(GL_TEXTURE0 + CANVAS_UNIT_TILES);
        glBindTexture(GL_TEXTURE_BUFFER, d->tile_texture);
        glActiveTexture(GL_TEXTURE0 + CANVAS_UNIT_STYLES);
        glBindTexture(GL_TEXTURE_BUFFER, d->style_texture);
        glActiveTexture(GL_TEXTURE0 + CANVAS_UNIT_TAGS);
        glBindTexture(GL_TEXTURE_BUFFER, d->tags->texture);
        glActiveTexture(GL_TEXTURE0 + CANVAS_UNIT_GLYPHS);
        glBindTexture(GL_TEXTURE_BUFFER, d->glyph_texture);
        for (uint32_t slot = 0; slot < d->font_count; slot++) {
            glActiveTexture(GL_TEXTURE0 + CANVAS_UNIT_FONTS + slot);
            glBindTexture(GL_TEXTURE_2D, d->fonts[slot]->atlas_texture_id);
        }

        const CanvasLayer* order[CANVAS_LAYER_MAX] = {NULL};
        const uint32_t n = _draw_order(d, view, order);
        for (uint32_t i = 0; i < n; i++)
            _draw_layer(d, order[i]);
        for (int unit = CANVAS_UNIT_END - 1; unit >= CANVAS_UNIT_TILES; unit--) {
            glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
            glBindTexture(unit >= CANVAS_UNIT_FONTS ? GL_TEXTURE_2D : GL_TEXTURE_BUFFER, 0);
        }

        // The view's rectangle into the frame, opaque: what the canvas drew
        // replaces what was there.
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)pass.fbo);
        glViewport((GLint)x0, gl_y, w, h);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glUseProgram(d->composite->id);
        const int origin[4] = {(int)x0, gl_y, 0, 0};
        uniform_set_ivec4(d->composite->uniforms, "uOrigin", origin);
        uniform_set_int(d->composite->uniforms, "uView", 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, d->target_texture);
        draw_fullscreen_quad(d->quad_vao);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);
    gl_pass_end(&pass);
}

/*
 * Views. A view's centre is an integer and a fraction in [0, 1), so a pan at
 * any zoom moves it by exactly what was asked.
 */

static void _view_move(CanvasView* view, double dx, double dy) {
    const double fx = view->fraction[0] + dx, fy = view->fraction[1] + dy;
    const double ix = floor(fx), iy = floor(fy);
    view->centre.x += (int64_t)ix;
    view->centre.y += (int64_t)iy;
    view->fraction[0] = fx - ix;
    view->fraction[1] = fy - iy;
}

// A window point as units from the view's centre, y up.
static void _view_offset(const CanvasView* view, float x, float y, double* ux, double* uy) {
    double dx = ((double)x - ((double)view->x + (double)view->width * 0.5)) / view->points_per_unit;
    const double dy =
        (((double)view->y + (double)view->height * 0.5) - (double)y) / view->points_per_unit;
    if (view->mirror_x)
        dx = -dx;
    *ux = dx, *uy = dy;
}

CanvasPoint canvas_view_unit_at(const CanvasView* view, float x, float y) {
    if (!view || view->points_per_unit <= 0.0)
        return (CanvasPoint){0, 0};
    double ux, uy;
    _view_offset(view, x, y, &ux, &uy);
    return (CanvasPoint){view->centre.x + llround(view->fraction[0] + ux),
                         view->centre.y + llround(view->fraction[1] + uy)};
}

void canvas_view_point_of(const CanvasView* view, CanvasPoint p, float* x, float* y) {
    if (!view || !x || !y)
        return;
    double dx = ((double)(p.x - view->centre.x) - view->fraction[0]) * view->points_per_unit;
    const double dy = ((double)(p.y - view->centre.y) - view->fraction[1]) * view->points_per_unit;
    if (view->mirror_x)
        dx = -dx;
    *x = (float)((double)view->x + (double)view->width * 0.5 + dx);
    *y = (float)((double)view->y + (double)view->height * 0.5 - dy);
}

void canvas_view_zoom_at(CanvasView* view, float x, float y, double factor) {
    if (!view || view->points_per_unit <= 0.0 || !(factor > 0.0))
        return;
    double before_x, before_y;
    _view_offset(view, x, y, &before_x, &before_y);
    view->points_per_unit *= factor;
    // The unit under the point is the centre plus the offset; keep it there.
    _view_move(view, before_x - before_x / factor, before_y - before_y / factor);
}

void canvas_view_pan(CanvasView* view, float dx, float dy) {
    if (!view || view->points_per_unit <= 0.0)
        return;
    double ux = -(double)dx / view->points_per_unit;
    const double uy = (double)dy / view->points_per_unit;
    if (view->mirror_x)
        ux = -ux;
    _view_move(view, ux, uy);
}
