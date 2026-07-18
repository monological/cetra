#ifndef COMMON_H
#define COMMON_H

#include <cglm/cglm.h>

#define GL_ATTR_POSITION     0
#define GL_ATTR_NORMAL       1
#define GL_ATTR_TEXCOORD     2
#define GL_ATTR_TANGENT      3
#define GL_ATTR_BITANGENT    4
#define GL_ATTR_COLOR        5
#define GL_ATTR_BONE_IDS     6 // ivec4 - bone indices per vertex
#define GL_ATTR_BONE_WEIGHTS 7 // vec4  - bone weights per vertex
#define GL_ATTR_TEXCOORD2    8 // UV1 for lightmaps/AO

// Fixed non-light uniform overhead the max-light math subtracts. The CSM
// arrays dominate: lightSpaceMatrix[9] (144) + cascadeParams[9] (36) +
// cascadeSplits (4) + counts/toggles; audit when the shadow uniform block
// changes shape (shadow.c static-asserts a floor from the shadow constants).
#define USED_UNIFORM_COMPONENTS 215
#define COMPONENTS_PER_LIGHT    21 // Number of components per light
// Mirror of pbr_frag.glsl's MAX_LIGHTS (the shader array is a hardcoded
// literal; shaders see no C defines). get_gl_max_lights() clamps to this so
// numLights can never exceed the shader array and read out of bounds.
#define PBR_MAX_LIGHTS 64

#define CETRA_RED_COLOR     ((vec3){1.0f, 0.0f, 0.0f})
#define CETRA_GREEN_COLOR   ((vec3){0.0f, 1.0f, 0.0f})
#define CETRA_BLUE_COLOR    ((vec3){0.0f, 0.0f, 1.0f})
#define CETRA_YELLOW_COLOR  ((vec3){1.0f, 1.0f, 0.0f})
#define CETRA_CYAN_COLOR    ((vec3){0.0f, 1.0f, 1.0f})
#define CETRA_MAGENTA_COLOR ((vec3){1.0f, 0.0f, 1.0f})
#define CETRA_WHITE_COLOR   ((vec3){1.0f, 1.0f, 1.0f})
#define CETRA_BLACK_COLOR   ((vec3){0.0f, 0.0f, 0.0f})

typedef enum {
    RENDER_MODE_PBR,             // Regular PBR Rendering
    RENDER_MODE_NORMALS,         // Normals Visualization
    RENDER_MODE_WORLD_POS,       // World Position Visualization
    RENDER_MODE_TEX_COORDS,      // Texture Coordinates Visualization
    RENDER_MODE_TANGENT_SPACE,   // Tangent Space Visualization
    RENDER_MODE_FLAT_COLOR,      // Flat Color Visualization
    RENDER_MODE_ALBEDO,          // Albedo Only
    RENDER_MODE_SIMPLE_LIGHTING, // Simple Diffuse Lighting
    RENDER_MODE_METALLIC_ROUGH,  // Metallic and Roughness Visualization
    RENDER_MODE_VELOCITY         // Motion-vector (velocity) visualization
} RenderMode;

// Axis vertices: 6 vertices, 2 for each line (origin and end)
extern float xyz_vertices[];
extern const size_t xyz_vertices_size;

#endif // COMMON_H
