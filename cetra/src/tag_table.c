#include "tag_table.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ext/log.h"
#include "util.h"

// A tag's texel `i`, within its bytes.
#define TEXEL(bytes, i) ((bytes) + 4 * (i))

void tag_table_pack(uint32_t colour, uint32_t outline, float dim, bool hidden, bool raised,
                    bool lit, uint8_t out[TAG_TABLE_BYTES]) {
    rgba_bytes(colour, TEXEL(out, TAG_TEXEL_COLOUR));
    rgba_bytes(outline, TEXEL(out, TAG_TEXEL_OUTLINE));
    uint8_t* misc = TEXEL(out, TAG_TEXEL_MISC);
    misc[0] = (uint8_t)lroundf(glm_clamp(dim, 0.0f, 1.0f) * 255.0f);
    misc[1] = hidden ? 255 : 0;
    misc[2] = raised ? 255 : 0;
    misc[3] = lit ? 255 : 0;
}

bool tag_table_admits(uint32_t tag, const char* who) {
    if (tag != 0 && tag < TAG_TABLE_LIMIT)
        return true;
    log_error("%s: tag %u is refused (1 to %u)", who, tag, TAG_TABLE_LIMIT - 1);
    return false;
}

void tag_table_init(TagTable* table) {
    memset(table, 0, sizeof(*table));
    table->count = 1;
}

void tag_table_release(TagTable* table) {
    gl_delete_texture(&table->texture);
    if (table->buffer)
        glDeleteBuffers(1, &table->buffer);
    free(table->texels);
    tag_table_init(table);
}

// Whether a tag's bytes set its outline.
static bool _ringed(const uint8_t bytes[TAG_TABLE_BYTES]) {
    static const uint8_t none[4];
    return memcmp(TEXEL(bytes, TAG_TEXEL_OUTLINE), none, 4) != 0;
}

bool tag_table_write(TagTable* table, uint32_t tag, const uint8_t texels[TAG_TABLE_BYTES],
                     unsigned* changed) {
    const size_t had = table->cap;
    if (!grow_array((void**)&table->texels, &table->cap, (size_t)tag + 1, TAG_TABLE_BYTES, 256))
        return false;
    memset(table->texels + had * TAG_TABLE_BYTES, 0, (table->cap - had) * TAG_TABLE_BYTES);
    uint8_t* at = table->texels + (size_t)tag * TAG_TABLE_BYTES;
    const bool was_ringed = _ringed(at), ringed = _ringed(texels);
    if (changed) {
        const bool drawn =
            memcmp(TEXEL(at, TAG_TEXEL_COLOUR), TEXEL(texels, TAG_TEXEL_COLOUR), 4) != 0 ||
            memcmp(TEXEL(at, TAG_TEXEL_MISC), TEXEL(texels, TAG_TEXEL_MISC), 4) != 0;
        const bool ring =
            memcmp(TEXEL(at, TAG_TEXEL_OUTLINE), TEXEL(texels, TAG_TEXEL_OUTLINE), 4) != 0;
        *changed = (drawn ? TAG_TABLE_DRAWN : 0u) | (ring ? TAG_TABLE_RINGED : 0u);
    }
    table->outlined += (uint32_t)ringed - (uint32_t)was_ringed;
    if (table->outlined == 0) {
        table->outlined_lo = table->outlined_hi = 0;
    } else if (ringed) {
        const bool first = table->outlined_hi == 0;
        table->outlined_lo = first || tag < table->outlined_lo ? tag : table->outlined_lo;
        table->outlined_hi = tag > table->outlined_hi ? tag : table->outlined_hi;
    }
    memcpy(at, texels, TAG_TABLE_BYTES);
    if (tag + 1 > table->count)
        table->count = tag + 1;
    if (table->dirty_from == table->dirty_to) {
        table->dirty_from = tag, table->dirty_to = tag + 1;
    } else {
        table->dirty_from = tag < table->dirty_from ? tag : table->dirty_from;
        table->dirty_to = tag + 1 > table->dirty_to ? tag + 1 : table->dirty_to;
    }
    return true;
}

// The written range alone, or, when the table has outgrown its buffer, a buffer twice the size and
// every tag in it. The shader reads no tag past the count, so the rest of a new buffer is left.
void tag_table_upload(TagTable* table) {
    if (!table->buffer) {
        glGenBuffers(1, &table->buffer);
        glGenTextures(1, &table->texture);
    }
    glBindBuffer(GL_TEXTURE_BUFFER, table->buffer);
    if (table->count > table->capacity) {
        uint32_t capacity = table->capacity ? table->capacity : 256;
        while (capacity < table->count)
            capacity *= 2;
        glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)capacity * TAG_TABLE_BYTES, NULL,
                     GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_BUFFER, table->texture);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8, table->buffer);
        table->capacity = capacity;
        table->dirty_from = 0, table->dirty_to = table->count;
    }
    // A table nothing was written to still sends tag 0's zeros, so no texel the count covers is
    // left undefined.
    const size_t had = table->cap;
    if (table->dirty_from < table->dirty_to &&
        grow_array((void**)&table->texels, &table->cap, table->dirty_to, TAG_TABLE_BYTES, 256)) {
        memset(table->texels + had * TAG_TABLE_BYTES, 0, (table->cap - had) * TAG_TABLE_BYTES);
        glBufferSubData(GL_TEXTURE_BUFFER, (GLintptr)table->dirty_from * TAG_TABLE_BYTES,
                        (GLsizeiptr)(table->dirty_to - table->dirty_from) * TAG_TABLE_BYTES,
                        table->texels + (size_t)table->dirty_from * TAG_TABLE_BYTES);
    }
    table->dirty_from = table->dirty_to = 0;
}
