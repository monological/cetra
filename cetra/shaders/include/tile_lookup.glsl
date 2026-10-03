// Cached point-light shadows (spec 13.16): where a point falls on a cached light's faces,
// which are tiles of the punctual array drawn once by shadow.c and kept. The surface lookup
// (punctual_tiles.glsl) and the fog volume's tap both read through this.
//
// NOTHING PER FACE IS UPLOADED. A face is projected analytically from where the faces were
// drawn from, the near plane and the far, with the basis shadow.c's lookAt gives that face
// (shadow_tile_face_matrix, over compute_perspective_light_space and light_space_up), written
// out below as three tables. Matrices would be six per light, and silent alone caches
// fourteen lights -- no uniform array in this shader has room for 84 of them, where the
// projection is three dot products and a divide. The tables are the contract with C, and
// shadow_tile_face_matrix is what they are checked against.
//
// A tile is found from the array's own size: cells run row-major within a layer from layer 0,
// so a light's first cell and the array's edge are its whole address.
//
// Needs lights_ubo.glsl, and the punctual array declared as `punctualShadowMaps` by the
// includer -- this file declares no sampler, since the surface and the fog each declare
// their own.

#include "shadow_tile_constants.glsl"
#include "cube_face.glsl"

// Each face's axis, and glm_lookat's right and up for it, in the +X -X +Y -Y +Z -Z order
// punctualCubeFace answers in. Up is world +Y, or +X for the two faces looking along Y --
// light_space_up's choice, which is what makes right = cross(axis, up) come out as below.
const vec3 TILE_FACE_AXIS[6] = vec3[6](vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0),
                                       vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0),
                                       vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0));
const vec3 TILE_FACE_RIGHT[6] = vec3[6](vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0),
                                        vec3(0.0, 0.0, -1.0), vec3(0.0, 0.0, 1.0),
                                        vec3(-1.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0));
const vec3 TILE_FACE_UP[6] = vec3[6](vec3(0.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0),
                                     vec3(1.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0),
                                     vec3(0.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0));

// The face's own 90 degrees as a fraction of the tile: the field of view is widened so the
// guard band lies outside it, and a filter reaching past the face's edge still reads this
// face's depth. 1 / tan(fov / 2).
const float TILE_INNER =
    float(SHADOW_TILE_SIZE - 2 * SHADOW_TILE_GUARD) / float(SHADOW_TILE_SIZE);

// A point on face `face`, `rel` from where the faces were drawn: xy its uv over the tile and
// z the depth the face's map stores for it. The caller keeps `rel` in front of the face.
vec3 tileFaceProject(int face, vec3 rel, float nearP, float farP) {
    float d = dot(rel, TILE_FACE_AXIS[face]);
    vec2 ndc = TILE_INNER * vec2(dot(rel, TILE_FACE_RIGHT[face]), dot(rel, TILE_FACE_UP[face])) / d;
    float z = ((farP + nearP) - 2.0 * farP * nearP / d) / (farP - nearP);
    return vec3(ndc, z) * 0.5 + 0.5;
}

// Metres along the face's axis from a stored depth: tileFaceProject's z, inverted.
float tileLinearDepth(float z01, float nearP, float farP) {
    return 2.0 * farP * nearP / ((farP + nearP) - (2.0 * z01 - 1.0) * (farP - nearP));
}

// Where cell `cell` sits in an array `edge` texels across: xy its corner in layer uv, z its
// layer.
vec3 tileCell(int cell, int edge) {
    int perRow = edge / SHADOW_TILE_SIZE;
    int perLayer = perRow * perRow;
    int within = cell % perLayer;
    vec2 corner = vec2(float(within % perRow), float(within / perRow)) * float(SHADOW_TILE_SIZE);
    return vec3(corner / float(edge), float(cell / perLayer));
}

// One tile's depth at a uv over the tile, clamped to the tile and never to the face: the
// guard band past the face's edge is this face's own depth, and past the tile is another
// light's. `span` is a tile's extent in layer uv.
float tileDepthAt(vec3 cell, float span, vec2 uv) {
    float texel = 1.0 / float(SHADOW_TILE_SIZE);
    uv = clamp(uv, 0.5 * texel, 1.0 - 0.5 * texel);
    return texture(punctualShadowMaps, vec3(cell.xy + uv * span, cell.z)).r;
}

// Whether light `li` reaches a point through its cached faces, in ONE tap: 1 lit, 0 not.
// For a point that is not a surface -- a cell of air -- so there is no receiver plane, and
// the bias is in METRES along the face rather than in depth, which is what keeps it the same
// size near the light, where the stored depth is fine-grained, and near the range, where it
// is coarse. A point past the range is lit; the light has reached zero there anyway.
float tileVisibility(uint li, vec3 P, float biasMetres) {
    float nearP = clusterLights[li].upArea.w;
    float farP = clusterLights[li].posRange.w;
    vec3 rel = P - clusterLights[li].shadowTile.xyz;
    int face = punctualCubeFace(rel);
    float d = dot(rel, TILE_FACE_AXIS[face]);
    if (d >= farP)
        return 1.0;
    vec3 pc = tileFaceProject(face, rel, nearP, farP);
    int edge = textureSize(punctualShadowMaps, 0).x;
    vec3 cell = tileCell(int(clusterLights[li].shadowTile.w) + face, edge);
    float stored = tileDepthAt(cell, float(SHADOW_TILE_SIZE) / float(edge), pc.xy);
    return tileLinearDepth(stored, nearP, farP) < d - biasMetres ? 0.0 : 1.0;
}
