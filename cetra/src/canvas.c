#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "canvas_record.h"
#include "engine.h"
#include "ext/log.h"
#include "ext/uthash.h"
#include "program.h"
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

// The units the canvas program reads its tables from: its own ledger.
enum { CANVAS_UNIT_TILES = 0, CANVAS_UNIT_STYLES = 1, CANVAS_UNIT_TAGS = 2 };

// What the canvas registers its two programs as, so every canvas shares them.
#define CANVAS_PROGRAM           "canvas"
#define CANVAS_COMPOSITE_PROGRAM "canvas_composite"

/*
 * Tags.
 */

struct CanvasTags {
    CanvasTagState* states;
    uint32_t count; // one past the largest tag set, at least 1
    uint32_t cap;
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
    if (tag >= tags->cap) {
        uint32_t cap = tags->cap ? tags->cap : 256;
        while (cap <= tag)
            cap *= 2;
        CanvasTagState* grown = realloc(tags->states, cap * sizeof(CanvasTagState));
        if (!grown) {
            log_error("canvas: out of memory growing a tag table");
            return false;
        }
        memset(grown + tags->cap, 0, (cap - tags->cap) * sizeof(CanvasTagState));
        tags->states = grown;
        tags->cap = cap;
    }
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

static void _tag_texels(const CanvasTags* tags, uint32_t tag, uint8_t out[12]) {
    const CanvasTagState s = tag < tags->cap ? tags->states[tag] : (CanvasTagState){0};
    _rgba_bytes(s.colour, out);
    _rgba_bytes(s.outline, out + 4);
    const float dim = s.dim < 0.0f ? 0.0f : s.dim > 1.0f ? 1.0f : s.dim;
    out[8] = (uint8_t)lroundf(dim * 255.0f);
    out[9] = s.hidden ? 255 : 0;
    out[10] = s.raised ? 255 : 0;
    out[11] = 0;
}

// Brings the GPU's copy up to date: the written range alone, or the whole
// table into a buffer twice the size when it has outgrown the one it has.
static void _tags_upload(CanvasTags* tags) {
    if (!tags->buffer) {
        glGenBuffers(1, &tags->buffer);
        glGenTextures(1, &tags->texture);
    }
    if (tags->count > tags->capacity) {
        uint32_t capacity = tags->capacity ? tags->capacity : 256;
        while (capacity < tags->count)
            capacity *= 2;
        uint8_t* texels = calloc(capacity, 4 * CANVAS_TAG_TEXELS);
        if (!texels) {
            log_error("canvas: out of memory uploading a tag table");
            return;
        }
        for (uint32_t t = 1; t < tags->count; t++)
            _tag_texels(tags, t, texels + (size_t)t * 4 * CANVAS_TAG_TEXELS);
        glBindBuffer(GL_TEXTURE_BUFFER, tags->buffer);
        glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)capacity * 4 * CANVAS_TAG_TEXELS, texels,
                     GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_BUFFER, tags->texture);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8, tags->buffer);
        free(texels);
        tags->capacity = capacity;
    } else if (tags->dirty_from < tags->dirty_to) {
        const uint32_t n = tags->dirty_to - tags->dirty_from;
        uint8_t* texels = malloc((size_t)n * 4 * CANVAS_TAG_TEXELS);
        if (!texels) {
            log_error("canvas: out of memory uploading a tag table");
            return;
        }
        for (uint32_t i = 0; i < n; i++)
            _tag_texels(tags, tags->dirty_from + i, texels + (size_t)i * 4 * CANVAS_TAG_TEXELS);
        glBindBuffer(GL_TEXTURE_BUFFER, tags->buffer);
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

// A layer's tile: the records it keeps, and the range of the layer's buffer
// they are uploaded into.
typedef struct CanvasTile {
    int64_t key[2];
    CanvasRecord* records;
    uint32_t count, cap;
    uint32_t slot, slots; // slots 0 = no range yet
    bool dirty;
    UT_hash_handle hh;
} CanvasTile;

typedef struct CanvasLayerData {
    CanvasTile* tiles;
    GLuint vao, vbo;
    uint32_t end;      // slots in use in the buffer, live and dead; what a draw covers
    uint32_t capacity; // slots the buffer holds
    bool dirty;        // a tile wants uploading
} CanvasLayerData;

// The tiles an object has records in, which a remove looks through.
typedef struct CanvasObject {
    uint32_t object;
    CanvasTile** tiles;
    uint32_t count, cap;
    UT_hash_handle hh;
} CanvasObject;

typedef struct CanvasData {
    int tile_shift;
    CanvasLayer* layers[CANVAS_LAYER_MAX];
    uint32_t layer_count;

    uint32_t* styles;
    uint32_t style_count, style_cap;
    bool styles_dirty;

    CanvasTags* tags;
    bool owns_tags;

    CanvasCoord* coords;
    CanvasCoord** coord_by_index;
    uint32_t coord_count, coord_cap;

    CanvasObject* objects;

    // GL, made at the first draw.
    bool gl_ready, gl_failed;
    ShaderProgram* program;
    ShaderProgram* composite;
    GLuint quad_vao, quad_vbo;
    GLuint style_buffer, style_texture;
    GLuint tile_buffer, tile_texture;
    int32_t* tile_table;
    uint32_t tile_table_cap;
    GLuint target_fbo, target_texture;
    int target_w, target_h;
    CanvasRecord* scratch; // a tile's range, its records then dead slots
    uint32_t scratch_cap;
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
        free(tile->records);
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
            if (ld->vbo)
                glDeleteBuffers(1, &ld->vbo);
            if (ld->vao)
                glDeleteVertexArrays(1, &ld->vao);
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
        gl_delete_texture(&d->style_texture);
        gl_delete_texture(&d->tile_texture);
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
    canvas->generation++;
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
    if (d->style_count == d->style_cap) {
        const uint32_t cap = d->style_cap ? d->style_cap * 2 : 16;
        uint32_t* grown = realloc(d->styles, cap * sizeof(uint32_t));
        if (!grown) {
            log_error("canvas: out of memory adding a style");
            return 0;
        }
        d->styles = grown;
        d->style_cap = cap;
    }
    d->styles[d->style_count] = rgba;
    d->styles_dirty = true;
    canvas->generation++;
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
    canvas->generation++;
}

CanvasTags* canvas_tags(const Canvas* canvas) {
    return canvas ? canvas->data->tags : NULL;
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
    if (d->coord_count == d->coord_cap) {
        const uint32_t cap = d->coord_cap ? d->coord_cap * 2 : 256;
        CanvasCoord** grown = realloc(d->coord_by_index, cap * sizeof(CanvasCoord*));
        if (!grown)
            return NULL;
        d->coord_by_index = grown;
        d->coord_cap = cap;
    }
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
    HASH_ADD(hh, ld->tiles, key, sizeof(tile->key), tile);
    return tile;
}

static bool _tile_push(CanvasTile* tile, const CanvasRecord* record) {
    if (tile->count == tile->cap) {
        const uint32_t cap = tile->cap ? tile->cap * 2 : 16;
        CanvasRecord* grown = realloc(tile->records, cap * sizeof(CanvasRecord));
        if (!grown)
            return false;
        tile->records = grown;
        tile->cap = cap;
    }
    tile->records[tile->count++] = *record;
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
    for (uint32_t i = entry->count; i-- > 0;)
        if (entry->tiles[i] == tile)
            return;
    if (entry->count == entry->cap) {
        const uint32_t cap = entry->cap ? entry->cap * 2 : 4;
        CanvasTile** grown = realloc(entry->tiles, cap * sizeof(CanvasTile*));
        if (!grown)
            return;
        entry->tiles = grown;
        entry->cap = cap;
    }
    entry->tiles[entry->count++] = tile;
}

static bool _owns_layer(const CanvasData* d, const CanvasLayer* layer) {
    return layer && layer->index < d->layer_count && d->layers[layer->index] == layer;
}

static void _add(CanvasData* d, const CanvasPiece* piece, uint32_t* refused) {
    const uint16_t style = (uint16_t)(piece->record.u[0] >> 16);
    if (!_owns_layer(d, piece->layer) || style >= d->style_count) {
        (*refused)++;
        return;
    }
    CanvasCoord* coord = _coord(d, piece->tx, piece->ty);
    CanvasLayerData* ld = piece->layer->data;
    CanvasTile* tile = _tile(ld, piece->tx, piece->ty);
    CanvasRecord record = piece->record;
    if (!coord || !tile) {
        (*refused)++;
        return;
    }
    record.u[3] = coord->index;
    if (!_tile_push(tile, &record)) {
        (*refused)++;
        return;
    }
    for (int axis = 0; axis < 2; axis++) {
        coord->min[axis] =
            piece->min[axis] < coord->min[axis] ? piece->min[axis] : coord->min[axis];
        coord->max[axis] =
            piece->max[axis] > coord->max[axis] ? piece->max[axis] : coord->max[axis];
    }
    tile->dirty = true;
    ld->dirty = true;
    _note_object(d, record.u[2], tile);
}

// Takes an object's records out of every tile it was in, keeping the order of
// the rest: where shapes overlap, the order they were added decides.
static void _remove_object(CanvasData* d, uint32_t object) {
    CanvasObject* entry = NULL;
    HASH_FIND(hh, d->objects, &object, sizeof(object), entry);
    if (!entry)
        return;
    for (uint32_t i = 0; i < entry->count; i++) {
        CanvasTile* tile = entry->tiles[i];
        uint32_t kept = 0;
        for (uint32_t r = 0; r < tile->count; r++)
            if (tile->records[r].u[2] != object)
                tile->records[kept++] = tile->records[r];
        if (kept != tile->count) {
            tile->count = kept;
            tile->dirty = true;
        }
    }
    // A removed tile's layer is not known from here, so every layer is asked
    // whether it has a dirty tile at the next upload.
    for (uint32_t l = 0; l < d->layer_count; l++)
        d->layers[l]->data->dirty = true;
    HASH_DEL(d->objects, entry);
    free(entry->tiles);
    free(entry);
}

static void _clear_layer(const CanvasData* d, CanvasLayer* layer) {
    if (!_owns_layer(d, layer))
        return;
    CanvasLayerData* ld = layer->data;
    CanvasTile *tile, *next;
    HASH_ITER(hh, ld->tiles, tile, next) {
        if (tile->count) {
            tile->count = 0;
            tile->dirty = true;
            ld->dirty = true;
        }
    }
}

void canvas_apply(Canvas* canvas, CanvasEdit* edit) {
    if (!canvas || !edit)
        return;
    CanvasData* d = canvas->data;
    if (edit->tile_shift != d->tile_shift) {
        log_error("canvas: an edit made for another canvas's tiles is refused");
    } else {
        uint32_t refused = 0;
        for (size_t s = 0; s < edit->step_count; s++) {
            const CanvasStep* step = &edit->steps[s];
            switch (step->kind) {
                case CANVAS_STEP_ADD:
                    for (size_t i = 0; i < step->count; i++)
                        _add(d, &edit->pieces[step->first + i], &refused);
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
        canvas->generation++;
    }
    edit->piece_count = 0;
    edit->step_count = 0;
}

/*
 * The GPU side.
 */

static bool _gl_ready(Canvas* canvas) {
    CanvasData* d = canvas->data;
    if (d->gl_ready)
        return true;
    if (d->gl_failed)
        return false;
    Engine* engine = canvas->engine;
    d->program = engine_find_program(engine, CANVAS_PROGRAM);
    if (!d->program) {
        d->program = create_canvas_program();
        if (d->program)
            engine_add_program(engine, d->program);
    }
    d->composite = engine_find_program(engine, CANVAS_COMPOSITE_PROGRAM);
    if (!d->composite) {
        d->composite = create_canvas_composite_program();
        if (d->composite)
            engine_add_program(engine, d->composite);
    }
    if (!d->program || !d->composite) {
        log_error("canvas: its programs did not build, so it draws nothing");
        d->gl_failed = true;
        return false;
    }
    create_fullscreen_quad_vao(&d->quad_vao, &d->quad_vbo);
    glGenBuffers(1, &d->style_buffer);
    glGenTextures(1, &d->style_texture);
    glGenBuffers(1, &d->tile_buffer);
    glGenTextures(1, &d->tile_texture);
    d->gl_ready = true;
    return true;
}

static void _layer_gl(CanvasLayerData* ld) {
    if (ld->vao)
        return;
    glGenVertexArrays(1, &ld->vao);
    glGenBuffers(1, &ld->vbo);
    glBindVertexArray(ld->vao);
    glBindBuffer(GL_ARRAY_BUFFER, ld->vbo);
    const GLsizei stride = (GLsizei)sizeof(CanvasRecord);
    glEnableVertexAttribArray(0);
    glVertexAttribIPointer(0, 4, GL_INT, stride, (const void*)offsetof(CanvasRecord, p));
    glVertexAttribDivisor(0, 1);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(CanvasRecord, f));
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 4, GL_UNSIGNED_INT, stride, (const void*)offsetof(CanvasRecord, u));
    glVertexAttribDivisor(2, 1);
    glBindVertexArray(0);
}

static uint32_t _slots_for(uint32_t count) {
    if (count == 0)
        return 0;
    const uint32_t slots = count + count / 2;
    return slots < CANVAS_TILE_MIN_SLOTS ? CANVAS_TILE_MIN_SLOTS : slots;
}

// Writes a tile's range: its records, then dead slots to the end of the range.
static void _upload_range(Canvas* canvas, uint32_t slot, uint32_t slots,
                          const CanvasRecord* records, uint32_t count) {
    CanvasData* d = canvas->data;
    if (slots == 0)
        return;
    if (slots > d->scratch_cap) {
        CanvasRecord* grown = realloc(d->scratch, slots * sizeof(CanvasRecord));
        if (!grown) {
            log_error("canvas: out of memory uploading a tile");
            return;
        }
        d->scratch = grown;
        d->scratch_cap = slots;
    }
    if (count)
        memcpy(d->scratch, records, count * sizeof(CanvasRecord));
    memset(d->scratch + count, 0, (slots - count) * sizeof(CanvasRecord));
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)slot * (GLintptr)sizeof(CanvasRecord),
                    (GLsizeiptr)slots * (GLsizeiptr)sizeof(CanvasRecord), d->scratch);
    canvas->uploads++;
    canvas->bytes_uploaded += (uint64_t)slots * sizeof(CanvasRecord);
}

/*
 * A layer's buffer holds its tiles' ranges one after another. A tile that
 * outgrows its range takes a new one at the end and its old one is written
 * dead; when the end passes the buffer, or the dead ranges outweigh the live
 * ones, every tile is laid out again from the start and uploaded whole.
 */
static void _layer_upload(Canvas* canvas, CanvasLayer* layer) {
    CanvasLayerData* ld = layer->data;
    if (!ld->dirty)
        return;
    _layer_gl(ld);
    glBindBuffer(GL_ARRAY_BUFFER, ld->vbo);

    uint32_t live = 0;
    CanvasTile *tile, *next;
    HASH_ITER(hh, ld->tiles, tile, next) {
        if (tile->dirty && tile->count > tile->slots) {
            if (tile->slots)
                _upload_range(canvas, tile->slot, tile->slots, NULL, 0);
            tile->slots = _slots_for(tile->count);
            tile->slot = ld->end;
            ld->end += tile->slots;
        }
        live += tile->slots;
    }

    if (ld->end > ld->capacity || ld->end > 2 * live + 4096) {
        uint32_t end = 0;
        HASH_ITER(hh, ld->tiles, tile, next) {
            tile->slots = _slots_for(tile->count);
            tile->slot = end;
            end += tile->slots;
            tile->dirty = true;
        }
        ld->end = end;
        ld->capacity = end + end / 2 + 1024;
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)ld->capacity * (GLsizeiptr)sizeof(CanvasRecord),
                     NULL, GL_DYNAMIC_DRAW);
    }

    HASH_ITER(hh, ld->tiles, tile, next) {
        if (!tile->dirty)
            continue;
        _upload_range(canvas, tile->slot, tile->slots, tile->records, tile->count);
        tile->dirty = false;
    }
    ld->dirty = false;
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

// Each tile coordinate's offset from the view's integer centre, and whether
// anything in it can reach the view. One table a view, since each view has
// its own centre.
static void _tiles_upload(CanvasData* d, const CanvasView* view, double half_x, double half_y) {
    const uint32_t n = d->coord_count ? d->coord_count : 1;
    if (n > d->tile_table_cap) {
        int32_t* grown = realloc(d->tile_table, (size_t)n * 4 * sizeof(int32_t));
        if (!grown)
            return;
        d->tile_table = grown;
        d->tile_table_cap = n;
    }
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

static bool _target(CanvasData* d, int w, int h) {
    if (d->target_fbo && w <= d->target_w && h <= d->target_h)
        return true;
    const int tw = w > d->target_w ? w : d->target_w;
    const int th = h > d->target_h ? h : d->target_h;
    gl_delete_texture(&d->target_texture);
    gl_delete_fbo(&d->target_fbo);
    glGenTextures(1, &d->target_texture);
    glBindTexture(GL_TEXTURE_2D, d->target_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &d->target_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, d->target_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->target_texture,
                           0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
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
// order added, the active one last.
static uint32_t _draw_order(const CanvasData* d, const CanvasView* view, const CanvasLayer** out) {
    const bool every = canvas_layer_set_empty(&view->shown);
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->layer_count; i++) {
        const CanvasLayer* layer = d->layers[i];
        if (layer == view->active || !(every || canvas_layer_set_has(&view->shown, layer)))
            continue;
        uint32_t at = n++;
        while (at > 0 && out[at - 1]->order > layer->order) {
            out[at] = out[at - 1];
            at--;
        }
        out[at] = layer;
    }
    if (view->active && _owns_layer(d, view->active))
        out[n++] = view->active;
    return n;
}

static void _clear_colour(uint32_t rgba) {
    glClearColor((float)((rgba >> 24) & 255) / 255.0f, (float)((rgba >> 16) & 255) / 255.0f,
                 (float)((rgba >> 8) & 255) / 255.0f, 1.0f);
}

void canvas_draw(Canvas* canvas, const CanvasView* view) {
    if (!canvas || !view || view->points_per_unit <= 0.0)
        return;
    const Engine* engine = canvas->engine;
    CanvasData* d = canvas->data;
    if (engine->win_width <= 0 || engine->win_height <= 0 || !_gl_ready(canvas))
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

    GLPassState pass = gl_pass_begin();
    GLint program = 0, vao = 0, equation_rgb = GL_FUNC_ADD, equation_alpha = GL_FUNC_ADD;
    GLboolean mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &equation_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &equation_alpha);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    const GLboolean multisample = glIsEnabled(GL_MULTISAMPLE);

    if (_target(d, w, h)) {
        for (uint32_t i = 0; i < d->layer_count; i++)
            _layer_upload(canvas, d->layers[i]);
        _styles_upload(d);
        _tags_upload(d->tags);
        _tiles_upload(d, view, (w * 0.5 + 2.0) / ppu, (h * 0.5 + 2.0) / ppu);

        glBindFramebuffer(GL_FRAMEBUFFER, d->target_fbo);
        glViewport(0, 0, w, h);
        glDisable(GL_MULTISAMPLE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        _clear_colour(view->background);
        glClear(GL_COLOR_BUFFER_BIT);

        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied
        glUseProgram(d->program->id);
        UniformManager* u = d->program->uniforms;
        const float fraction[2] = {(float)view->fraction[0], (float)view->fraction[1]};
        const float size[2] = {(float)w, (float)h};
        uniform_set_vec2(u, "uFraction", fraction);
        uniform_set_vec2(u, "uSize", size);
        uniform_set_float(u, "uPixelsPerUnit", (float)ppu);
        uniform_set_float(u, "uMirror", view->mirror_x ? -1.0f : 1.0f);
        uniform_set_float(u, "uShapeMinPx", canvas->shape_min_px);
        uniform_set_int(u, "uTagCount", (int)d->tags->count);
        uniform_set_int(u, "uTiles", CANVAS_UNIT_TILES);
        uniform_set_int(u, "uStyles", CANVAS_UNIT_STYLES);
        uniform_set_int(u, "uTags", CANVAS_UNIT_TAGS);
        glActiveTexture(GL_TEXTURE0 + CANVAS_UNIT_TILES);
        glBindTexture(GL_TEXTURE_BUFFER, d->tile_texture);
        glActiveTexture(GL_TEXTURE0 + CANVAS_UNIT_STYLES);
        glBindTexture(GL_TEXTURE_BUFFER, d->style_texture);
        glActiveTexture(GL_TEXTURE0 + CANVAS_UNIT_TAGS);
        glBindTexture(GL_TEXTURE_BUFFER, d->tags->texture);

        const CanvasLayer* order[CANVAS_LAYER_MAX] = {NULL};
        const uint32_t n = _draw_order(d, view, order);
        for (uint32_t i = 0; i < n; i++) {
            const CanvasLayerData* ld = order[i]->data;
            if (ld->end == 0)
                continue;
            glBindVertexArray(ld->vao);
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)ld->end);
        }
        for (int unit = CANVAS_UNIT_TAGS; unit >= CANVAS_UNIT_TILES; unit--) {
            glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
            glBindTexture(GL_TEXTURE_BUFFER, 0);
        }

        // The view's rectangle into the frame, opaque: what the canvas drew
        // replaces what was there.
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)pass.fbo);
        glViewport((GLint)x0, gl_y, w, h);
        glDisable(GL_BLEND);
        glUseProgram(d->composite->id);
        const int origin[4] = {(int)x0, gl_y, 0, 0};
        uniform_set_ivec4(d->composite->uniforms, "uOrigin", origin);
        uniform_set_int(d->composite->uniforms, "uView", 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, d->target_texture);
        draw_fullscreen_quad(d->quad_vao);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glUseProgram((GLuint)program);
    glBindVertexArray((GLuint)vao);
    glBlendEquationSeparate((GLenum)equation_rgb, (GLenum)equation_alpha);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (multisample)
        glEnable(GL_MULTISAMPLE);
    else
        glDisable(GL_MULTISAMPLE);
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
