#include <cglm/cglm.h>

#include "common.h"

// Axis vertices: 6 vertices, 2 for each line (origin and end)
float xyz_vertices[] = {
    // X-axis (Red)
    0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, // Origin to Red
    1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, // Red end
    // Y-axis (Green)
    0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, // Origin to Green
    0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, // Green end
    // Z-axis (Blue)
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, // Origin to Blue
    0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f  // Blue end
};

const size_t xyz_vertices_size = sizeof(xyz_vertices);

