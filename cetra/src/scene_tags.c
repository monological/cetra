#include "scene_tags.h"

#include <GL/glew.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ext/log.h"
#include "tag_table.h"
#include "util.h"

struct SceneTags {
    SceneTagState* states;
    size_t cap;
    uint64_t generation, outline_generation;
    uint32_t outlined; // tags whose outline is set
    TagTable table;
};

SceneTags* create_scene_tags(void) {
    SceneTags* tags = calloc(1, sizeof(SceneTags));
    if (!tags) {
        log_error("scene tags: out of memory making a table");
        return NULL;
    }
    tag_table_init(&tags->table);
    return tags;
}

void free_scene_tags(SceneTags* tags) {
    if (!tags)
        return;
    tag_table_release(&tags->table);
    free(tags->states);
    free(tags);
}

static void _rgba_bytes(uint32_t rgba, uint8_t out[4]) {
    out[0] = (uint8_t)(rgba >> 24), out[1] = (uint8_t)(rgba >> 16);
    out[2] = (uint8_t)(rgba >> 8), out[3] = (uint8_t)rgba;
}

// The canvas's layout, so the two tables read alike: colour, outline, then dim, hidden, a byte
// the canvas raises with and lit.
static void _tag_texels(SceneTagState s, uint8_t out[4 * TAG_TABLE_TEXELS]) {
    _rgba_bytes(s.colour, out);
    _rgba_bytes(s.outline, out + 4);
    const float dim = s.dim < 0.0f ? 0.0f : s.dim > 1.0f ? 1.0f : s.dim;
    out[8] = (uint8_t)lroundf(dim * 255.0f);
    out[9] = s.hidden ? 255 : 0;
    out[10] = 0;
    out[11] = s.lit ? 255 : 0;
}

bool scene_tags_set(SceneTags* tags, uint32_t tag, SceneTagState state) {
    if (!tags)
        return false;
    if (tag == 0 || tag >= TAG_TABLE_LIMIT) {
        log_error("scene tags: tag %u is refused (1 to %u)", tag, TAG_TABLE_LIMIT - 1);
        return false;
    }
    const size_t had = tags->cap;
    if (!grow_array((void**)&tags->states, &tags->cap, (size_t)tag + 1, sizeof(SceneTagState), 256))
        return false;
    memset(tags->states + had, 0, (tags->cap - had) * sizeof(SceneTagState));
    const SceneTagState was = tags->states[tag];
    uint8_t texels[4 * TAG_TABLE_TEXELS], before[4 * TAG_TABLE_TEXELS];
    _tag_texels(state, texels);
    _tag_texels(was, before);
    // Compared as the texels the shader reads, so a write of what is already there changes nothing
    // and restarts no view. The outline's four bytes are counted apart.
    const bool drawn = memcmp(texels, before, 4) != 0 || memcmp(texels + 8, before + 8, 4) != 0;
    const bool ringed = memcmp(texels + 4, before + 4, 4) != 0;
    if ((drawn || ringed) && !tag_table_write(&tags->table, tag, texels))
        return false;
    tags->outlined += (state.outline != 0) - (was.outline != 0);
    tags->states[tag] = state;
    tags->generation += drawn;
    tags->outline_generation += ringed;
    return true;
}

SceneTagState scene_tags_get(const SceneTags* tags, uint32_t tag) {
    if (!tags || tag == 0 || tag >= tags->cap)
        return (SceneTagState){0};
    return tags->states[tag];
}

uint64_t scene_tags_generation(const SceneTags* tags) {
    return tags ? tags->generation : 0;
}

uint64_t scene_tags_outline_generation(const SceneTags* tags) {
    return tags ? tags->outline_generation : 0;
}

bool scene_tags_outlined(const SceneTags* tags) {
    return tags && tags->outlined > 0;
}

uint32_t scene_tags_bind(SceneTags* tags, int unit) {
    if (!tags)
        return 0;
    tag_table_upload(&tags->table);
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    glBindTexture(GL_TEXTURE_BUFFER, tags->table.texture);
    glActiveTexture(GL_TEXTURE0);
    return tags->table.count;
}
