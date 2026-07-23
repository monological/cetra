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
    glBufferData(GL_UNIFORM_BUFFER, ubo->size, NULL, GL_DYNAMIC_DRAW); // orphan
    glBufferSubData(GL_UNIFORM_BUFFER, 0, size, data);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void ubo_wire_program_block(GLuint program_id, const char* block_name, GLuint binding,
                            GLsizeiptr expected_size) {
    GLuint index = glGetUniformBlockIndex(program_id, block_name);
    if (index == GL_INVALID_INDEX)
        return; // program doesn't reference this block

    glUniformBlockBinding(program_id, index, binding);

    GLint data_size = 0;
    glGetActiveUniformBlockiv(program_id, index, GL_UNIFORM_BLOCK_DATA_SIZE, &data_size);
    if ((GLsizeiptr)data_size != expected_size)
        log_error("UBO block '%s': driver std140 size %d != C-side %ld -- layout mismatch",
                  block_name, data_size, (long)expected_size);
}

void ubo_wire_light_blocks(GLuint program_id) {
    ubo_wire_program_block(program_id, "LightsBlock", UBO_BINDING_LIGHTS, UBO_LIGHTS_BLOCK_SIZE);
    ubo_wire_program_block(program_id, "ClusterBlock", UBO_BINDING_CLUSTERS,
                           UBO_CLUSTERS_BLOCK_SIZE);
    ubo_wire_program_block(program_id, "ClusterIndexBlock", UBO_BINDING_CLUSTER_INDICES,
                           UBO_CLUSTER_INDICES_BLOCK_SIZE);
}
