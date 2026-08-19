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
    vec4 upArea;     // AREA only: panel height axis, orthonormal to dir (spec 9.2)
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
// ies_fold_horizontal (ies.c) and fold_horizontal (tools/gen_ies_table.py) --
// three copies, held together by the probe reading the angles one at a time.
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
    vec3 right = normalize(cross(refUp, axis));
    vec3 fwd = cross(axis, right);
    vec3 r = -L;
    float hDeg = degrees(atan(dot(r, right), dot(r, fwd)));
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
    float theta = dot(L, normalize(-lightDir));
    float epsilon = max(cutOff - outerCutOff, 1e-4);
    return clamp((theta - outerCutOff) / epsilon, 0.0, 1.0);
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
