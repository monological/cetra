#include "scene_tags_internal.h"

#include <GL/glew.h>
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
    // The least and greatest tag whose outline has been set since none was: every outlined tag
    // lies within, and one cleared inside leaves it as it was.
    uint32_t outlined_lo, outlined_hi;
    uint32_t hidden; // tags hidden
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

// The canvas's layout, nothing raised in 3D.
static void _tag_texels(SceneTagState s, uint8_t out[4 * TAG_TABLE_TEXELS]) {
    tag_table_pack(s.colour, s.outline, s.dim, s.hidden, false, s.lit, out);
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
    if (tags->outlined == 0) {
        tags->outlined_lo = tags->outlined_hi = 0;
    } else if (state.outline != 0) {
        const bool first = tags->outlined_hi == 0;
        tags->outlined_lo = first || tag < tags->outlined_lo ? tag : tags->outlined_lo;
        tags->outlined_hi = tag > tags->outlined_hi ? tag : tags->outlined_hi;
    }
    tags->hidden += (uint32_t)state.hidden - (uint32_t)was.hidden;
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

bool scene_tags_outlined_within(const SceneTags* tags, uint32_t lo, uint32_t hi) {
    return scene_tags_outlined(tags) && lo <= tags->outlined_hi && hi >= tags->outlined_lo;
}

bool scene_tags_hides(const SceneTags* tags) {
    return tags && tags->hidden > 0;
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
