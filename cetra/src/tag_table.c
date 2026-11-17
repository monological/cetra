#include "tag_table.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

#define TAG_TABLE_BYTES (4 * TAG_TABLE_TEXELS)

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

bool tag_table_write(TagTable* table, uint32_t tag, const uint8_t texels[TAG_TABLE_BYTES]) {
    const size_t had = table->cap;
    if (!grow_array((void**)&table->texels, &table->cap, (size_t)tag + 1, TAG_TABLE_BYTES, 256))
        return false;
    memset(table->texels + had * TAG_TABLE_BYTES, 0, (table->cap - had) * TAG_TABLE_BYTES);
    memcpy(table->texels + (size_t)tag * TAG_TABLE_BYTES, texels, TAG_TABLE_BYTES);
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
