#include "ubo.h"

#include <stdlib.h>

#include "ext/log.h"

Ubo* create_ubo(GLsizeiptr size, GLuint binding) {
    Ubo* ubo = calloc(1, sizeof(Ubo));
    if (!ubo) {
        log_error("Failed to allocate memory for UBO");
        return NULL;
    }
    ubo->size = size;
    ubo->binding = binding;

    glGenBuffers(1, &ubo->id);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo->id);
    void* zeros = calloc(1, (size_t)size);
    glBufferData(GL_UNIFORM_BUFFER, size, zeros, GL_DYNAMIC_DRAW);
    free(zeros);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    // Indexed binding persists for the context; programs reach the buffer
    // through the binding point alone, so this never needs re-binding.
    glBindBufferBase(GL_UNIFORM_BUFFER, binding, ubo->id);
    return ubo;
}

void free_ubo(Ubo* ubo) {
    if (!ubo)
        return;
    if (ubo->id)
        glDeleteBuffers(1, &ubo->id);
    free(ubo);
}

void ubo_upload(Ubo* ubo, const void* data, GLsizeiptr size) {
    if (!ubo || !data)
        return;
    if (size > ubo->size) {
        log_error("UBO upload of %ld bytes exceeds buffer size %ld", (long)size, (long)ubo->size);
        return;
    }
    glBindBuffer(GL_UNIFORM_BUFFER, ubo->id);
    if (size == ubo->size) {
        // Orphan AND fill in one call. glBufferData with a non-NULL pointer
        // replaces the allocation exactly as the NULL form does, so this keeps
        // the property the split form was chosen for and drops the second
        // traversal of the same 12 KB. Measured on forest at -11.0 ms of opaque
        // GPU against a 1.7% floor (spec 11.91).
        glBufferData(GL_UNIFORM_BUFFER, ubo->size, data, GL_DYNAMIC_DRAW);
    } else {
        // A short write cannot take that form: glBufferData sizes the buffer to
        // its argument, so passing `size` here would SHRINK the allocation the
        // binding point describes. Orphan the full size, then fill the prefix.
        glBufferData(GL_UNIFORM_BUFFER, ubo->size, NULL, GL_DYNAMIC_DRAW);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, size, data);
    }
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

bool ubo_wire_program_block(GLuint program_id, const char* block_name, GLuint binding,
                            GLsizeiptr expected_size) {
    GLuint index = glGetUniformBlockIndex(program_id, block_name);
    if (index == GL_INVALID_INDEX)
        return false; // program doesn't reference this block

    glUniformBlockBinding(program_id, index, binding);

    GLint data_size = 0;
    glGetActiveUniformBlockiv(program_id, index, GL_UNIFORM_BLOCK_DATA_SIZE, &data_size);
    if ((GLsizeiptr)data_size != expected_size) {
        log_error("UBO block '%s': driver std140 size %d != C-side %ld -- layout mismatch",
                  block_name, data_size, (long)expected_size);
        return false;
    }
    return true;
}

bool ubo_wire_blocks(GLuint program_id) {
    ubo_wire_program_block(program_id, "LightsBlock", UBO_BINDING_LIGHTS, UBO_LIGHTS_BLOCK_SIZE);
    ubo_wire_program_block(program_id, "ClusterBlock", UBO_BINDING_CLUSTERS,
                           UBO_CLUSTERS_BLOCK_SIZE);
    ubo_wire_program_block(program_id, "ClusterIndexBlock", UBO_BINDING_CLUSTER_INDICES,
                           UBO_CLUSTER_INDICES_BLOCK_SIZE);
    ubo_wire_program_block(program_id, "ViewParams", UBO_BINDING_VIEW, UBO_VIEW_BLOCK_SIZE);
    // The swash film's tips (spec 11.45). Declared by whatever includes shore.glsl -- the
    // water surface and any lit program that asked for shore wetness -- and stripped as
    // unreferenced everywhere else, which this call already treats as a no-op.
    ubo_wire_program_block(program_id, "ShoreFilmBlock", UBO_BINDING_SHORE_FILM,
                           UBO_SHORE_FILM_BLOCK_SIZE);
    // IES profiles (spec 11.57). Declared by anything including lights_ubo.glsl
    // and stripped as unreferenced where nothing calls iesProfile, which this
    // already treats as a no-op.
    ubo_wire_program_block(program_id, "IesBlock", UBO_BINDING_IES, UBO_IES_BLOCK_SIZE);
    // The composite cache's page table (spec 11.67). Declared by anything
    // including vt_pages.glsl; stripped where nothing pages.
    ubo_wire_program_block(program_id, "VtPageBlock", UBO_BINDING_VT_PAGES,
                           UBO_VT_PAGES_BLOCK_SIZE);
    // Roads (spec 11.68). Declared by anything including roads_ubo.glsl -- the
    // lit surface and the composite bake both, through layers.glsl -- which is
    // what keeps the two reading one set of segments.
    ubo_wire_program_block(program_id, "RoadsBlock", UBO_BINDING_ROADS, UBO_ROADS_BLOCK_SIZE);
    // Clustered specular probes (spec 11.70). Declared by anything including
    // probe_specular.glsl -- the lit surface and SSR -- so the two blend the
    // same probes through one implementation.
    ubo_wire_program_block(program_id, "ProbeBlock", UBO_BINDING_PROBES, UBO_PROBES_BLOCK_SIZE);
    // Clustered decals (spec 11.73). Declared by anything including
    // decals_ubo.glsl, which today is the lit surface alone.
    ubo_wire_program_block(program_id, "DecalBlock", UBO_BINDING_DECALS, UBO_DECALS_BLOCK_SIZE);
    return ubo_wire_program_block(program_id, "InstanceBlock", UBO_BINDING_INSTANCES,
                                  UBO_INSTANCES_BLOCK_SIZE);
}
