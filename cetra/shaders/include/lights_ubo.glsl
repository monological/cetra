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
    vec4 dirType;        // xyz = direction (world), w = 1 point / 2 spot / 3 area
    vec4 colorIntensity; // xyz = color * intensity (premultiplied on CPU)
    vec4 attenCutoff;    // x = constant, y = linear, z = quadratic, w = cos inner cone
    vec4 spotShadowSize; // x = cos outer cone, y = float(shadow slot) (spare -- the
                         //     spot map is global today), zw = emitter size
    vec4 upReserved;     // RESERVED: LTC area-light `up` frame (spec 9.0 item A2)
};

layout(std140) uniform LightsBlock {
    ivec4 lightCounts;  // x = dir count, y = clustered count, zw unused
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

// The two block accessors are deliberately the only places that decode the
// packing -- the single point of repair if a driver mishandles them.
uint clusterWord(uint ci) {
    return clusters[ci >> 2u][ci & 3u];
}

uint lightIndexAt(uint i) {
    uint w = lightIndices[i >> 3u][(i >> 1u) & 3u];
    return (i & 1u) == 0u ? (w & 0xFFFFu) : (w >> 16u);
}

// Packed-field spot cone, mirroring spotConeFactor (pbr_frag) exactly: 1 inside
// the inner cone, smooth to 0 at the outer cone, 1 for every non-spot light.
float spotConeFactorP(float typeF, vec3 lightDir, float cutOff, float outerCutOff, vec3 L) {
    if (typeF != 2.0)
        return 1.0;
    float theta = dot(L, normalize(-lightDir));
    float epsilon = max(cutOff - outerCutOff, 1e-4);
    return clamp((theta - outerCutOff) / epsilon, 0.0, 1.0);
}
