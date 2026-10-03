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
// fade), lights_ubo.glsl, noise.glsl's ign, and pbr_frag's POISSON16 and pcssStochastic
// above it.

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

// How far a soft edge's disk may reach, in tile uv: the guard band, so a fragment on a face's
// edge still filters this face's own depth.
const float TILE_GUARD_UV = float(SHADOW_TILE_GUARD) / float(SHADOW_TILE_SIZE);
// A blocker must be this much nearer the light than the receiver's plane, as a fraction of
// the receiver's distance -- the cascades' reason (CSM_BLOCKER_SEPARATION): a receiver lies
// within its own filter bias, and counting it as its own blocker collapses the penumbra.
#define TILE_BLOCKER_SEPARATION 0.99

// The soft edge (PCSS) for an emitter `emitter` metres across, at a receiver `dRecv` metres
// along the face: a blocker search over the cone from the receiver to the emitter, then a
// filter as wide as the penumbra the found blockers cast. Both are physical rather than
// scaled -- the penumbra is the emitter's width carried past the blockers by similar
// triangles -- so the edge is as wide as the emitter makes it, where the cascades'
// PCSS_PENUMBRA_SCALE sets it by eye. Both radii stop at the guard band.
float tileSoft(vec3 cell, float span, vec3 pc, vec2 duv_dz, float emitter, float dRecv,
               float nearP, float farP) {
    float halfW = 0.5 * emitter;
    float texel = 1.0 / float(SHADOW_TILE_SIZE);
    // Where a blocker could reach farthest is at the near plane: the cone's cross-section
    // there, in uv at that depth.
    float searchUV =
        min(halfW * (dRecv - nearP) / dRecv * TILE_INNER / (2.0 * nearP), TILE_GUARD_UV);

    // The cascades' rotation, for their reason: identity unless TAA is there to average it.
    mat2 rot = mat2(1.0);
    if (pcssStochastic == 1) {
        vec2 fc = gl_FragCoord.xy + vec2(float(pcssFrameIndex) * 5.588238);
        float ang = 6.2831853 * ign(fc);
        float c = cos(ang);
        float s = sin(ang);
        rot = mat2(c, s, -s, c);
    }

    float blockerSum = 0.0;
    float blockerCount = 0.0;
    for (int i = 0; i < 16; i++) {
        vec2 off = rot * POISSON16[i] * searchUV;
        float zTap = tileLinearDepth(tileDepthAt(cell, span, pc.xy + off), nearP, farP);
        float zPlane = tileLinearDepth(pc.z + receiverPlaneBias(duv_dz, off), nearP, farP);
        if (zTap < zPlane * TILE_BLOCKER_SEPARATION) {
            blockerSum += zTap;
            blockerCount += 1.0;
        }
    }
    if (blockerCount < 0.5)
        return 1.0;
    float zBlocker = blockerSum / blockerCount;

    // Half the penumbra each side of the edge, in uv at the receiver's depth.
    float filterUV = clamp(halfW * (dRecv - zBlocker) / zBlocker * TILE_INNER / (2.0 * dRecv),
                           texel, TILE_GUARD_UV);
    float ref = pc.z - SHADOW_PLANE_BIAS_FLOOR;
    float sum = 0.0;
    for (int i = 0; i < 16; i++) {
        vec2 off = rot * POISSON16[i] * filterUV;
        sum += (ref + receiverPlaneBias(duv_dz, off) > tileDepthAt(cell, span, pc.xy + off))
                   ? 0.0
                   : 1.0;
    }
    return sum / 16.0;
}

// Occlusion for light `li` through its cached faces: 1 = lit, 0 = occluded. The same lookup
// punctualShadow is for a per-frame map -- a 3x3 PCF under the receiver's own plane, faded
// out at grazing -- with the face found and projected here rather than by a matrix, and a
// soft edge in place of the 3x3 wherever the light states an emitter size.
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

    int edge = textureSize(punctualShadowMaps, 0).x;
    vec3 cell = tileCell(int(clusterLights[li].shadowTile.w) + face, edge);
    float span = float(SHADOW_TILE_SIZE) / float(edge);

    float emitter = clusterLights[li].attenCutoff.z;
    if (emitter > 0.0) {
        float soft = tileSoft(cell, span, pc, duv_dz, emitter, dot(rel, axis), nearP, farP);
        return mix(1.0, soft, trust);
    }

    float texel = 1.0 / float(SHADOW_TILE_SIZE);
    float ref = pc.z - SHADOW_PLANE_BIAS_FLOOR;
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 off = vec2(x, y) * texel;
            sum += (ref + receiverPlaneBias(duv_dz, off) > tileDepthAt(cell, span, pc.xy + off))
                       ? 0.0
                       : 1.0;
        }
    }
    return mix(1.0, sum / 9.0, trust);
}
