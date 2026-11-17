#ifndef _TAG_TABLE_H_
#define _TAG_TABLE_H_

#include <GL/glew.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A table of tags in a buffer texture, three RGBA8 texels a tag: the one store the canvas's tags
 * (canvas.h) and a scene's (scene_tags.h) are read through. Each keeps its own typed states and
 * writes their texels here; a draw brings the GPU's copy up to date with tag_table_upload.
 */

#define TAG_TABLE_LIMIT  (1u << 22) // the most tags a table holds
#define TAG_TABLE_TEXELS 3

typedef struct TagTable {
    uint8_t* texels; // TAG_TABLE_TEXELS RGBA8 texels a tag
    size_t cap;      // tags the CPU copy holds
    uint32_t count;  // one past the largest tag written, at least 1
    GLuint buffer, texture;
    uint32_t capacity;             // tags the GPU buffer holds
    uint32_t dirty_from, dirty_to; // the tags written since the last upload, [from, to)
} TagTable;

// An empty table, every tag's texels zero: the count is 1, since tag 0 is never written.
void tag_table_init(TagTable* table);
// Frees the GL objects and the texels, and leaves the table empty.
void tag_table_release(TagTable* table);
// A tag's texels, written; tag 0 and past the limit are the caller's to refuse. False, logged,
// when the table cannot grow to hold it, and nothing changed.
bool tag_table_write(TagTable* table, uint32_t tag, const uint8_t texels[4 * TAG_TABLE_TEXELS]);
// Brings the GPU's copy up to date: the written range alone, or, when the table has outgrown its
// buffer, a buffer twice the size and every tag in it. The shader reads no tag past the count, so
// the rest of a new buffer is left.
void tag_table_upload(TagTable* table);

#endif // _TAG_TABLE_H_
