// A sixteen-bit mask per froxel, two froxels a uint and eight to a uvec4 row: the packing
// light_cluster.c's _mark_sphere_bit writes for the reflection probes and the decals alike. Bit i
// says item i reaches froxel `ci`.
#define FROXEL_MASK_AT(masks, ci) (((masks)[(ci) >> 3u][((ci) >> 1u) & 3u] >> (((ci) & 1u) * 16u)) & 0xFFFFu)
