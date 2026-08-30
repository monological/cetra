#include <stdlib.h>
#include <string.h>

#include "occlusion.h"

#include "ext/log.h"

OcclusionContext* create_occlusion_context(void) {
    OcclusionContext* context = calloc(1, sizeof(OcclusionContext));
    if (!context) {
        log_error("Failed to allocate occlusion context");
        return NULL;
    }
    return context;
}

void free_occlusion_context(OcclusionContext* context) {
    if (!context)
        return;
    free(context);
}
