// Cached point-light shadows (spec 13.16): the lookup for a light whose six faces are tiles
// of the punctual array, drawn once by shadow.c and kept.
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
// Needs punctual_shadow.glsl (the sampler, punctualCubeFace, the plane bias and the grazing
// fade) and lights_ubo.glsl above it.

#include "shadow_tile_constants.glsl"

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

// Where cell `cell` sits in an array `edge` texels across: xy its corner in layer uv, z its
// layer.
vec3 tileCell(int cell, int edge) {
    int perRow = edge / SHADOW_TILE_SIZE;
    int perLayer = perRow * perRow;
    int within = cell % perLayer;
    vec2 corner = vec2(float(within % perRow), float(within / perRow)) * float(SHADOW_TILE_SIZE);
    return vec3(corner / float(edge), float(cell / perLayer));
}

// Occlusion for light `li` through its cached faces: 1 = lit, 0 = occluded. The same lookup
// punctualShadow is for a per-frame map -- a 3x3 PCF under the receiver's own plane, faded
// out at grazing -- with the face found and projected here rather than by a matrix.
//
// The receiver's plane is projected onto the SAME face as the point, never re-chosen per
// derivative: near a face boundary the two neighbours would otherwise land on different
// faces, whose uv and depth do not share a space.
float tileShadow(uint li, vec3 worldPos, vec3 N, vec3 L, vec3 ddxWorld, vec3 ddyWorld) {
    float ndl = clamp(dot(N, L), 0.0, 1.0);
    float trust = smoothstep(0.0, PUNCTUAL_GRAZING_FADE, ndl);
    if (trust <= 0.0)
        return 1.0;

    float nearP = clusterLights[li].upArea.w;
    float farP = clusterLights[li].posRange.w;
    vec3 rel = worldPos - clusterLights[li].shadowTile.xyz;
    int face = punctualCubeFace(rel);
    vec3 pc = tileFaceProject(face, rel, nearP, farP);
    // Past the range, where the light has already reached zero.
    if (pc.z > 1.0)
        return 1.0;

    vec2 duv_dz = vec2(0.0);
    vec3 axis = TILE_FACE_AXIS[face];
    if (dot(rel + ddxWorld, axis) > 0.0 && dot(rel + ddyWorld, axis) > 0.0) {
        vec3 px = tileFaceProject(face, rel + ddxWorld, nearP, farP) - pc;
        vec3 py = tileFaceProject(face, rel + ddyWorld, nearP, farP) - pc;
        duv_dz = receiverPlaneGradient(px, py);
    }

    // Taps clamped to the tile, never to the face: the guard band past the face's edge is
    // this face's own depth, and past the tile is another light's.
    int edge = textureSize(punctualShadowMaps, 0).x;
    vec3 cell = tileCell(int(clusterLights[li].shadowTile.w) + face, edge);
    float span = float(SHADOW_TILE_SIZE) / float(edge);
    float texel = 1.0 / float(SHADOW_TILE_SIZE);
    float ref = pc.z - SHADOW_PLANE_BIAS_FLOOR;
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 off = vec2(x, y) * texel;
            vec2 uv = clamp(pc.xy + off, 0.5 * texel, 1.0 - 0.5 * texel);
            float d = texture(punctualShadowMaps, vec3(cell.xy + uv * span, cell.z)).r;
            sum += (ref + receiverPlaneBias(duv_dz, off) > d) ? 0.0 : 1.0;
        }
    }
    return mix(1.0, sum / 9.0, trust);
}
