// Cached point-light shadows (spec 13.16): the SURFACE lookup for a light whose six faces are
// tiles of the punctual array -- a receiver-plane PCF, or a soft edge sized by the emitter.
// Where a point falls on the faces is tile_lookup.glsl's, which the fog reads too.
//
// Needs punctual_shadow.glsl (the sampler, the plane bias and the grazing fade),
// lights_ubo.glsl, noise.glsl's ign, and pbr_frag's POISSON16 and pcssStochastic above it.

#include "tile_lookup.glsl"

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
// soft edge in place of the 3x3 wherever the light has a body.
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

    float emitter = 2.0 * clusterLights[li].shadowMisc.z;
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
