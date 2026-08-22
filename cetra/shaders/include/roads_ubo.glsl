#ifndef ROADS_UBO_GLSL
#define ROADS_UBO_GLSL

/*
 * Road segments (spec 11.68). C mirror: GpuRoadsBlock in roads.h; wired to
 * UBO_BINDING_ROADS by ubo_wire_blocks after link, which compares the driver's
 * own std140 block size against the C size and logs an error at link -- so a
 * layout that drifts between the two languages is caught loudly at startup
 * rather than read as garbage. That check is the net under the counts below.
 *
 * Two descriptor rows per road over one shared segment pool, the IES descriptor
 * idiom: a road spends only its own segments, so sizing every road for the
 * ceiling would quadruple the block for nothing. A zero-filled buffer reads
 * roadsInfo.x == 0, which every consumer treats as "no roads".
 */
const int ROADS_MAX = 4;
const int ROAD_SEGS_PER_ROAD = 15;

layout(std140) uniform RoadsBlock {
    ivec4 roadsInfo; // x = road count
    // [2r] firstSeg, segCount, halfWidth, feather; [2r+1] layer, unused x3
    vec4 roadDesc[ROADS_MAX * 2];
    vec4 roadSegs[ROADS_MAX * ROAD_SEGS_PER_ROAD]; // ax, az, bx, bz -- AUTHORED world XZ
};

#endif // ROADS_UBO_GLSL
