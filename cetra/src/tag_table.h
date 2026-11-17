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

#include "../shaders/include/tag_constants.glsl"

#define TAG_TABLE_LIMIT (1u << 22) // the most tags a table holds
#define TAG_TABLE_BYTES (4 * TAG_TABLE_TEXELS)

typedef struct TagTable {
    uint8_t* texels; // TAG_TABLE_TEXELS RGBA8 texels a tag
    size_t cap;      // tags the CPU copy holds
    uint32_t count;  // one past the largest tag written, at least 1
    GLuint buffer, texture;
    uint32_t capacity;             // tags the GPU buffer holds
    uint32_t dirty_from, dirty_to; // the tags written since the last upload, [from, to)
    uint32_t outlined;             // tags whose outline is set
    // The least and greatest tag whose outline has been set since none was: every outlined tag
    // lies within, and one cleared inside leaves it as it was.
    uint32_t outlined_lo, outlined_hi;
} TagTable;

// What a write changed: the texels that draw the tag, and its outline's.
#define TAG_TABLE_DRAWN  1u
#define TAG_TABLE_RINGED 2u

// A tag's three texels, as both tables' shaders read them.
void tag_table_pack(uint32_t colour, uint32_t outline, float dim, bool hidden, bool raised,
                    bool lit, uint8_t out[TAG_TABLE_BYTES]);
// An empty table, every tag's texels zero: the count is 1, since tag 0 is never written.
void tag_table_init(TagTable* table);
// Frees the GL objects and the texels, and leaves the table empty.
void tag_table_release(TagTable* table);
// Whether `tag` may be written: 1 up to the limit. Refused by name, under `who`, otherwise.
bool tag_table_admits(uint32_t tag, const char* who);
// A tag's texels written, `changed` (when given) set to what differs from before. False, logged,
// when the table cannot grow to hold it, and nothing changed; the tag is one it admits.
bool tag_table_write(TagTable* table, uint32_t tag, const uint8_t texels[TAG_TABLE_BYTES],
                     unsigned* changed);
// Brings the GPU's copy up to date with every write since the last.
void tag_table_upload(TagTable* table);

#endif // _TAG_TABLE_H_
