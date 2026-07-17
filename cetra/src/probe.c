#include <stdlib.h>
#include <string.h>

#include "probe.h"
#include "ext/log.h"

ReflectionProbe* create_reflection_probe(void) {
    ReflectionProbe* probe = malloc(sizeof(ReflectionProbe));
    if (!probe) {
        log_error("Failed to allocate reflection probe");
        return NULL;
    }
    memset(probe, 0, sizeof(ReflectionProbe));

    probe->intensity = 1.0f;
    probe->box_fade = 0.2f;
    probe->enabled = true;

    return probe;
}

void free_reflection_probe(ReflectionProbe* probe) {
    if (!probe)
        return;

    if (probe->cubemap)
        glDeleteTextures(1, &probe->cubemap);
    if (probe->prefiltered)
        glDeleteTextures(1, &probe->prefiltered);

    free(probe);
}
