#ifndef COMMON_H
#define COMMON_H

#include <cglm/cglm.h>

#define GL_ATTR_POSITION 0
#define GL_ATTR_NORMAL   1
#define GL_ATTR_TEXCOORD 2
#define GL_ATTR_TANGENT 3
#define GL_ATTR_BITANGENT 4

#define MAX_VERTEX_BUFFER 512 * 1024
#define MAX_ELEMENT_BUFFER 128 * 1024

#define USED_UNIFORM_COMPONENTS 77  // Number of components used by non-light uniforms
#define COMPONENTS_PER_LIGHT 21      // Number of components per light

typedef enum {
    RENDER_MODE_PBR,              // Regular PBR Rendering
    RENDER_MODE_NORMALS,          // Normals Visualization
    RENDER_MODE_WORLD_POS,        // World Position Visualization
    RENDER_MODE_TEX_COORDS,       // Texture Coordinates Visualization
    RENDER_MODE_TANGENT_SPACE,    // Tangent Space Visualization
    RENDER_MODE_FLAT_COLOR        // Flat Color Visualization
} RenderMode;

typedef struct {
    vec3 position;
    vec3 rotation;
    vec3 scale;
} Transform;

// Axis vertices: 6 vertices, 2 for each line (origin and end)
extern float xyz_vertices[];
extern const size_t xyz_vertices_size;

#endif // COMMON_H

