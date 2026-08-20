// Clustered-forward light data (spec 9.1). std140 blocks, bound from C code
// (GLSL 330 cannot write layout(binding=N)); the byte layout is mirrored by
// the C structs in light_cluster.h and pinned by the UBO_*_BLOCK_SIZE
// constants in ubo.h -- change all three together.
//
// Cluster grid: CLUSTER_X x CLUSTER_Y screen tiles x CLUSTER_Z exponential
// view-Z slices (Doom 2016 slicing).
//   slice(z) = floor(log2(z) * clusterParams.x + clusterParams.y)
//   clusterParams.x = CLUSTER_Z / log2(far/near)
//   clusterParams.y = -CLUSTER_Z * log2(near) / log2(far/near)
// Inverse, for volume passes marching the same slices (froxel fog, B1):
//   z(s) = near * pow(far/near, s / CLUSTER_Z)

const int CLUSTER_X = 16;
const int CLUSTER_Y = 8;
const int CLUSTER_Z = 24;
const int MAX_DIR_LIGHTS = 4;
const int MAX_CLUSTER_LIGHTS = 128;

struct DirLight {
    vec4 dirShadow;      // xyz = direction (world), w = float(CSM slot), -1 = no shadow
    vec4 colorIntensity; // xyz = color * intensity (premultiplied on CPU)
    vec4 sizeMisc;       // xy = emitter size (PCSS penumbra width), zw unused
};

struct PackedLight {
    vec4 posRange;       // xyz = world position, w = cull radius
    vec4 dirType;        // xyz = direction (world, UNIT), w = 1 point / 2 spot / 3 area
    vec4 colorIntensity; // xyz = color * intensity (premultiplied on CPU)
    vec4 attenCutoff;    // x = 1/range^2 (0 = unbounded), y = IES profile index
                         //     (-1 = none), z = reserved, w = cos inner cone
    vec4 shadowMisc;     // x = cos outer cone, y = float(punctual shadow base layer),
                         //     -1 = casts no shadow, zw = emitter/panel size
    vec4 upArea;         // xyz = roll reference, unit and orthonormal to dir: a panel's
                         //     height axis (spec 9.2), an asymmetric IES profile's
                         //     azimuth zero (spec 11.57)
};

layout(std140) uniform LightsBlock {
    ivec4 lightCounts;  // x = dir count, y = clustered count,
                        // z = area count (gates the LTC LUT fetches), w unused
    vec4 clusterParams; // x = sliceScale, y = sliceBias,
                        // z = CLUSTER_X / fbWidth, w = CLUSTER_Y / fbHeight
    DirLight dirLights[MAX_DIR_LIGHTS];
    PackedLight clusterLights[MAX_CLUSTER_LIGHTS];
};

// std140 gives SCALAR arrays a vec4 stride, so the grid and index pool pack
// four words per uvec4 -- a raw uint[3072] would silently 4x the block to 48KB.
layout(std140) uniform ClusterBlock {
    uvec4 clusters[768]; // 3072 words: (index-pool offset << 12) | count
};

layout(std140) uniform ClusterIndexBlock {
    uvec4 lightIndices[768]; // 6144 16-bit light indices, two per uint, low half first
};

// IES photometric profiles (spec 11.57). Its own block rather than room in
// LightsBlock because it is STATIC: the per-frame cluster upload orphans its
// buffer and rewrites only the live light prefix, so a table sharing that block
// would be undefined every frame or re-sent seven times on a capture frame.
//
// Mirrored by GpuIesBlock (ies.h) and sized by IES_MAX_* / IES_POOL_FLOATS
// there; the driver's own reported block size is what asserts they agree.
const int IES_MAX_PROFILES = 8;
const int IES_POOL_VEC4 = 992; // IES_POOL_FLOATS / 4

layout(std140) uniform IesBlock {
    ivec4 iesCounts; // x = profile count
    // TWO rows per profile: [2i] offset, vTaps, hTaps, span degrees
    //                       [2i+1] vLo, vHi (the file's measured vertical range)
    vec4 iesDesc[IES_MAX_PROFILES * 2];
    vec4 iesPool[IES_POOL_VEC4]; // normalised candela, FOUR TAPS PER VEC4
};

// The pool decoder, and the only place its packing is known -- the reason
// clusterWord exists in the same shape. A bare float[4000] in std140 would take
// a vec4 stride and cost 64KB.
float iesTap(int i) {
    return iesPool[i >> 2][i & 3];
}

// Fold a horizontal angle into the measured sweep [0, span].
//
// A partial sweep MIRRORS rather than repeats: a bilateral file measured 0..180
// describes 190 degrees as 170, not as 10, which is what a modulo would give and
// which reads the far side of the luminaire as the near side. Quadrant files
// mirror twice; a full 360 sweep passes through untouched. Same arithmetic as
// ies_fold_horizontal (ies.c) and fold_horizontal (tools/gen_ies_table.py); the
// three cannot share a token, since mod(), fmodf and % disagree on negatives.
// This copy is the one that decides pixels, and the only thing that reads it is
// a rendered frame -- which is why a gate arm has to look at one.
float iesFold(float angle, float span) {
    float period = 2.0 * span;
    float f = mod(angle, period);
    return f > span ? period - f : f;
}

/*
 * The profile's value at a direction, in [0,1]. `L` is the frag->light direction
 * and `axis` the light's own emission direction, so the vertical angle is
 * measured from the NADIR the file is written against: L points back at the
 * lamp, so -L is the ray leaving it.
 *
 * `refUp` gives the luminaire its roll. An asymmetric profile is not rotationally
 * invariant, so a wall-washer needs to know which way it is turned -- and the
 * symmetric case never touches it, which is why hTaps == 1 skips the whole
 * horizontal term rather than computing an angle it would discard.
 *
 * Bilinear on a UNIFORM index, because the table was resampled uniform at load
 * precisely so the runtime lookup is two multiplies rather than a search.
 */
float iesProfile(int idx, vec3 L, vec3 axis, vec3 refUp) {
    // "Nobody asked for a profile" -- the neutral for a MULTIPLIER, which is
    // what this is to a caller sampling it on its own. A caller choosing between
    // a profile and a cone must make that choice before arriving, because 1.0 is
    // the wrong answer to "the profile you asked for is not loaded"; that is
    // punctualAngularOf's bounds test, not this one.
    if (idx < 0 || idx >= iesCounts.x)
        return 1.0;
    vec4 d = iesDesc[idx * 2];
    vec4 range = iesDesc[idx * 2 + 1];
    int base = int(d.x);
    int vTaps = int(d.y);
    int hTaps = int(d.z);

    // Vertical: the angle between the ray leaving the lamp and its axis.
    float cosV = clamp(dot(-L, axis), -1.0, 1.0);
    float vDeg = degrees(acos(cosV));
    // OUTSIDE the measured range the luminaire emits nothing. A file measured
    // 0..90 stopped there because there is nothing above it, so clamping to the
    // edge tap would light the ceiling -- and a nonzero edge would then also
    // break pbr_frag's exact-zero early-out. The epsilon absorbs the float error
    // in acos near the endpoints rather than widening the range.
    if (vDeg < range.x - 1e-3 || vDeg > range.y + 1e-3)
        return 0.0;
    float vSpan = max(range.y - range.x, 1e-4);
    float vx = clamp((vDeg - range.x) / vSpan, 0.0, 1.0) * float(vTaps - 1);
    int v0 = int(vx);
    int v1 = min(v0 + 1, vTaps - 1);
    float vf = vx - float(v0);

    if (hTaps == 1) {
        // Rotationally symmetric: one column, and no horizontal angle exists to
        // be wrong about.
        return mix(iesTap(base + v0), iesTap(base + v1), vf);
    }

    // Horizontal: the ray's azimuth about the axis, in the luminaire's own frame.
    // refUp arrives orthonormal to a unit axis, so the cross is already unit and
    // `axis x right` expands to refUp itself -- the frame is one cross, not two
    // and a normalize, and refUp IS the azimuth zero rather than a vector the
    // second cross recovers.
    vec3 right = cross(refUp, axis);
    vec3 r = -L;
    float hDeg = degrees(atan(dot(r, right), dot(r, refUp)));
    if (hDeg < 0.0)
        hDeg += 360.0;
    float hx = iesFold(hDeg, d.w) / d.w * float(hTaps - 1);
    int h0 = int(hx);
    int h1 = min(h0 + 1, hTaps - 1);
    float hf = hx - float(h0);

    // v-major: hTaps entries per vertical tap, which is the order ies.c packs.
    float a = mix(iesTap(base + v0 * hTaps + h0), iesTap(base + v0 * hTaps + h1), hf);
    float b = mix(iesTap(base + v1 * hTaps + h0), iesTap(base + v1 * hTaps + h1), hf);
    return mix(a, b, vf);
}

// The block accessors are deliberately the only places that decode the
// packing -- the single point of repair if a driver mishandles them.
uint clusterWord(uint ci) {
    return clusters[ci >> 2u][ci & 3u];
}

uint lightIndexAt(uint i) {
    uint w = lightIndices[i >> 3u][(i >> 1u) & 3u];
    return (i & 1u) == 0u ? (w & 0xFFFFu) : (w >> 16u);
}

// The cluster light list for an already-resolved screen tile: .x = index-pool
// offset, .y = light count. The exponential-Z slicing and the offset|count word
// layout live HERE and nowhere else -- both entry points below go through this,
// so the slicing formula has exactly one edit site. Only the tile lookup, which
// is where the two consumers genuinely differ, sits outside.
uvec2 clusterLightListTile(int tileX, int tileY, float viewZ) {
    int slice =
        clamp(int(log2(max(viewZ, 1e-4)) * clusterParams.x + clusterParams.y), 0, CLUSTER_Z - 1);
    uint word = clusterWord(uint(tileX + CLUSTER_X * (tileY + CLUSTER_Y * slice)));
    return uvec2(word >> 12u, word & 0xFFFu);
}

// A fragment's cluster light list, from its pixel coordinate in the scene pass's
// framebuffer -- clusterParams.zw carries CLUSTER_X/fbWidth, CLUSTER_Y/fbHeight.
uvec2 clusterLightList(vec2 fragCoord, float viewZ) {
    return clusterLightListTile(min(int(fragCoord.x * clusterParams.z), CLUSTER_X - 1),
                                min(int(fragCoord.y * clusterParams.w), CLUSTER_Y - 1), viewZ);
}

// The same list at a NORMALIZED screen position, for consumers that do not
// render at the scene pass's resolution and so have no pixel coordinate in the
// space clusterParams.zw is expressed in (the froxel fog volume, spec 9.5).
uvec2 clusterLightListUv(vec2 uv, float viewZ) {
    return clusterLightListTile(min(int(uv.x * float(CLUSTER_X)), CLUSTER_X - 1),
                                min(int(uv.y * float(CLUSTER_Y)), CLUSTER_Y - 1), viewZ);
}

// Spot cone from packed fields: 1 inside the inner cone, smooth to 0 at the
// outer cone, 1 for every non-spot light. cutOff/outerCutOff are cosines of
// the inner/outer half-angles; `L` is the frag->light direction, and the spot
// aims along lightDir, so -lightDir points back up the beam axis.
float spotConeFactor(float typeF, vec3 lightDir, float cutOff, float outerCutOff, vec3 L) {
    if (typeF != 2.0)
        return 1.0;
    // lightDir is unit at every call site -- light_cluster.c normalizes what it
    // packs and shadow.c what it publishes, which is the whole point of moving
    // that normalize off the per-fragment path.
    float theta = dot(L, -lightDir);
    float epsilon = max(cutOff - outerCutOff, 1e-4);
    return clamp((theta - outerCutOff) / epsilon, 0.0, 1.0);
}

/*
 * The ANGULAR half of a punctual light's falloff: its IES profile when it has
 * one, the analytic cone when it does not.
 *
 * One decision because the falloff is evaluated in five places -- the shading
 * loop, its hand-duplicated debug twin, the fog's clustered point walk, the fog's
 * spot shaft, and the contact-shadow fold weight -- and a profile applied in some
 * but not all of them means the beam disagrees with the pool it casts. That is
 * precisely the failure getDistanceAtt and spotConeFactor were centralised here
 * to prevent, restated one layer up now that there are two ways to be angular.
 *
 * A profile REPLACES the cone rather than multiplying it: the profile is the
 * whole distribution, cutoff included, so applying both renders a falloff
 * matching neither the file nor the authored cone (spec 11.57).
 *
 * Split in two along the axis the fifth site differs on. The shaft is published
 * standalone so it can carry its shadow map, so it has the VALUES and not a
 * cluster index -- taking those directly is what lets it share the decision
 * rather than restate it.
 */
// An index past the loaded set is not a profile. Falling through to the cone is
// the only answer that leaves a spot a spot: iesProfile answers 1.0 there, which
// is the neutral for something MULTIPLIED and wrong for something that replaces,
// and it would also strand pbr_frag's exact-zero early-out on a light that can
// no longer reach zero.
float punctualAngularOf(int iesIdx, float typeF, vec3 axis, vec3 refUp,
                        float cosInner, float cosOuter, vec3 L) {
    if (iesIdx >= 0 && iesIdx < iesCounts.x)
        return iesProfile(iesIdx, L, axis, refUp);
    return spotConeFactor(typeF, axis, cosInner, cosOuter, L);
}

// The general form takes the light's frame explicitly, because a consumer
// working in VIEW space (the contact-shadow pass) has to transform the axis and
// the roll reference itself -- view is rigid, so every angle here survives that
// unchanged, but the vectors must be in the same space as `L`.
float punctualAngular(uint li, vec3 L, vec3 axis, vec3 refUp) {
    return punctualAngularOf(int(clusterLights[li].attenCutoff.y), clusterLights[li].dirType.w,
                             axis, refUp, clusterLights[li].attenCutoff.w,
                             clusterLights[li].shadowMisc.x, L);
}

// ...and the world-space one, which is every consumer but that.
float punctualAngular(uint li, vec3 L) {
    return punctualAngular(li, L, clusterLights[li].dirType.xyz, clusterLights[li].upArea.xyz);
}

// Whether a light needs its frame at all. A point light with no profile reads
// neither the axis nor the roll, so a consumer that has to transform them can
// skip the work -- which is what 11.56 measured as a mat4 multiply per point
// light per pixel, thrown away.
bool punctualNeedsFrame(uint li) {
    return int(clusterLights[li].attenCutoff.y) >= 0 || clusterLights[li].dirType.w == 2.0;
}

// The other half of punctual attenuation, beside the cone for the same reason:
// a surface and the fog in front of it have to agree about how a lamp dies off,
// or the beam separates from the pool it casts.
//
// Inverse-square is the falloff a real emitter has, which is what lets intensity
// be candela rather than a multiplier that needs retuning per scene scale. The
// window takes it to exactly zero AT the range so the culler is not truncating a
// live curve -- squared, so it lands C1 and the cutoff has no visible edge.
//
// invSqrAttRadius is 1/range^2, precomputed on the CPU into attenCutoff.x; 0
// means unbounded. The near floor is a 1 cm sphere, since 1/d^2 is singular at
// the light's own position and one INF pixel survives every clamp downstream.
float getDistanceAtt(float sqrDist, float invSqrAttRadius) {
    float atten = 1.0 / max(sqrDist, 1e-4);
    float factor = sqrDist * invSqrAttRadius;
    float smoothFactor = clamp(1.0 - factor * factor, 0.0, 1.0);
    return atten * smoothFactor * smoothFactor;
}
