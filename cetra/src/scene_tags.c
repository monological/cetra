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

bool scene_tags_set(SceneTags* tags, uint32_t tag, SceneTagState state) {
    if (!tags || !tag_table_admits(tag, "scene tags"))
        return false;
    const size_t had = tags->cap;
    if (!grow_array((void**)&tags->states, &tags->cap, (size_t)tag + 1, sizeof(SceneTagState), 256))
        return false;
    memset(tags->states + had, 0, (tags->cap - had) * sizeof(SceneTagState));
    // The canvas's layout, nothing raised in 3D.
    uint8_t texels[TAG_TABLE_BYTES];
    tag_table_pack(state.colour, state.outline, state.dim, state.hidden, false, state.lit, texels);
    unsigned changed = 0;
    if (!tag_table_write(&tags->table, tag, texels, &changed))
        return false;
    tags->hidden += (uint32_t)state.hidden - (uint32_t)tags->states[tag].hidden;
    tags->states[tag] = state;
    // Counted as the texels the shader reads change, so a write of what is already there restarts
    // no view, and an outline's apart.
    tags->generation += (changed & TAG_TABLE_DRAWN) != 0;
    tags->outline_generation += (changed & TAG_TABLE_RINGED) != 0;
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
    return tags && tags->table.outlined > 0;
}

bool scene_tags_outlined_within(const SceneTags* tags, uint32_t lo, uint32_t hi) {
    return scene_tags_outlined(tags) && lo <= tags->table.outlined_hi &&
           hi >= tags->table.outlined_lo;
}

bool scene_tags_hides(const SceneTags* tags) {
    return tags && tags->hidden > 0;
}

uint32_t scene_tags_upload(SceneTags* tags) {
    if (!tags)
        return 0;
    tag_table_upload(&tags->table);
    return tags->table.count;
}

void scene_tags_bind(const SceneTags* tags, int unit) {
    if (!tags)
        return;
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    glBindTexture(GL_TEXTURE_BUFFER, tags->table.texture);
    glActiveTexture(GL_TEXTURE0);
}
