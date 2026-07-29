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
