#ifndef COMMON_H
#define COMMON_H

#include <cglm/cglm.h>

#define GL_ATTR_POSITION 0
#define GL_ATTR_NORMAL   1
#define GL_ATTR_TEXCOORD 2
#define GL_ATTR_TANGENT  3 // vec4 - xyz tangent, w bitangent handedness
// Slot 4 is free FOR MESH VAOs. It held a bitangent stream until that turned
// out to be dead weight: the fragment shader reconstructs B = cross(N, T)
// regardless and only ever read the stored one for its sign, which now rides in
// tangent.w. It is not unclaimed, though -- particle_sim_vert.glsl binds
// location 4 as iLife on the GPU-sim VAO, exactly as the billboard renderer
// holds 9-11 below. Separate VAOs, so no runtime conflict either way.
#define GL_ATTR_COLOR        5
#define GL_ATTR_BONE_IDS     6 // ivec4 - bone indices per vertex
#define GL_ATTR_BONE_WEIGHTS 7 // vec4  - bone weights per vertex
#define GL_ATTR_TEXCOORD2 \
    8 // UV1 for lightmaps/AO, or wind data under the
      // vegetation wind modes (see material.h wind_mode)

// Slots 9-11 are NOT free: the particle billboard renderer binds per-instance
// center/params/color there (particle_renderer.c, particle_vert.glsl), as bare
// literals rather than defines. It is a separate VAO from any mesh, so there is
// no runtime conflict -- but reusing 9-11 for mesh geometry would collide with
// that convention. 12-15 are unclaimed; GL 4.1 guarantees at least 16.

// PBR material fragment sampler units (render.c _update_program_material_uniforms).
// The engine-side units (shadow map array, IBL) follow in shadow.h / ibl.h; all
// of them must stay distinct and within GL_MAX_TEXTURE_IMAGE_UNITS, which the
// engine queries at init (get_gl_max_texture_image_units).
//
// The seven scalar masks (roughness/metallic/ao/opacity/microsurface/anisotropy/
// subsurface) collapsed into ONE sampler2DArray (TEXUNIT_MASKS, mask_array.h),
// freeing the mid material units. The relocated shadow and IBL engine units took
// 10-14 (shadow.h / ibl.h). Spec 9.2 claimed 7 + 9 for the two LTC area-light
// tables; 10.7.1 packed them into one array on 7 and gave 9 to the Charlie
// sheen environment (IBL_CHARLIE_TEXTURE_UNIT, ibl.h). Unit 15 is the spot
// shadow map. The full ordered budget is pinned by the _Static_assert chain
// in render.c.
#define TEXUNIT_ALBEDO           0
#define TEXUNIT_NORMAL           1
#define TEXUNIT_MASKS            2 // sampler2DArray: packed scalar masks
#define TEXUNIT_CLEARCOAT_NORMAL 3 // clearcoat normal map (a freed mask unit)
#define TEXUNIT_HEIGHT           4 // POM height map (a freed mask unit, §4.11)
#define TEXUNIT_EMISSIVE         5
#define TEXUNIT_SCENE_COLOR      6 // refraction opaque-scene resolve (engine-bound)
#define TEXUNIT_LTC              7 // LTC tables, 2-layer array (engine-bound, §9.2)
#define TEXUNIT_SHEEN            8 // KHR_materials_sheen color texture (§4.10.1)
#define TEXUNIT_MATERIAL_MAX     TEXUNIT_SHEEN

// (Light data moved off the default uniform block entirely: analytic lights
// live in the clustered-forward std140 UBOs -- see light_cluster.h / ubo.h --
// so the old per-light uniform-component budget accounting is gone.)

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

// Which pass is rasterizing this mesh, and therefore where the uber-shader
// stops. Mirrored as pbr_frag's `passMode`, so the numbering is load-bearing.
//
// ONE enum rather than a mode plus a flag beside it. The depth prepass arrived
// (spec 11.31) as a second boolean, and the four states are mutually exclusive:
// nothing is a moment-generation draw AND a depth-only draw. As two independent
// bits that combination was constructible and its meaning fell out of the order
// the shader's early returns happened to be written in -- which is an answer to
// a question that has none. As one value it cannot be asked.
//
// Whether the accumulate weights by measured moments or by the depth curve stays
// a SEPARATE bit, and that is the test this enum applies: it varies
// independently of which pass is drawing, and the composite carries it under its
// own name too.
typedef enum {
    SUBMIT_PASS_SHADE = 0,          // the full shading pass
    SUBMIT_PASS_OIT_ACCUMULATE = 1, // weighted colour into the OIT FBO
    SUBMIT_PASS_OIT_MOMENTS = 2,    // absorbance moments into the moment FBO
    SUBMIT_PASS_DEPTH_ONLY = 3      // depth prepass: stop at the coverage decision
} SubmitPass;

// Axis vertices: 6 vertices, 2 for each line (origin and end)
extern float xyz_vertices[];
extern const size_t xyz_vertices_size;

#endif // COMMON_H
