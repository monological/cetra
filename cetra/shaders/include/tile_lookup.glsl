// Cached shadows (spec 13.16): where a point falls on a cached light's faces,
// which are tiles of the punctual array drawn once by shadow.c and kept. The surface lookup
// (punctual_tiles.glsl) and the fog volume's tap both read through this.
//
// NOTHING PER FACE IS UPLOADED. A face is projected analytically from where the faces were
// drawn from, the near plane and the far, with the basis shadow.c's lookAt gives that face
// (shadow_tile_face_matrix, over compute_perspective_light_space and light_space_up), written
// out below as three tables, which must reproduce it. Matrices would be six a view for every
// cached light, which no uniform array in this shader has room for, where the projection is
// three dot products and a divide.
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

// What a lookup needs of a cached light, decoded from its packed slots here and nowhere else:
// the body its views were drawn over -- its centre, where view 0 stands, its segment end to
// end and its radius -- its planes, its first tile, where it is now, and the array's address.
struct TileLight {
    vec3 centre;
    vec3 segment;
    float radius;
    float nearP;
    float farP;
    int first;
    vec3 current;
    int edge;   // the array's edge in texels
    float span; // a tile's extent in layer uv
};

TileLight tileLightAt(uint li) {
    TileLight t;
    t.centre = clusterLights[li].shadowTile.xyz;
    t.segment = vec3(clusterLights[li].attenCutoff.zw, clusterLights[li].shadowMisc.x);
    t.radius = clusterLights[li].colorIntensity.w;
    t.nearP = clusterLights[li].upArea.w;
    t.farP = clusterLights[li].posRange.w;
    t.first = int(clusterLights[li].shadowTile.w);
    t.current = clusterLights[li].posRange.xyz;
    t.edge = textureSize(punctualShadowMaps, 0).x;
    t.span = float(SHADOW_TILE_SIZE) / float(t.edge);
    return t;
}

// A point on face `face`, `rel` from where the faces were drawn: xy its uv over the tile and
// z the depth the face's map stores for it. The caller keeps `rel` in front of the face.
vec3 tileFaceProject(int face, vec3 rel, float nearP, float farP) {
    float d = dot(rel, TILE_FACE_AXIS[face]);
    vec2 ndc = SHADOW_TILE_INNER *
               vec2(dot(rel, TILE_FACE_RIGHT[face]), dot(rel, TILE_FACE_UP[face])) / d;
    float z = ((farP + nearP) - 2.0 * farP * nearP / d) / (farP - nearP);
    return vec3(ndc, z) * 0.5 + 0.5;
}

// Metres along the face's axis from a stored depth: tileFaceProject's z, inverted.
float tileLinearDepth(float z01, float nearP, float farP) {
    return 2.0 * farP * nearP / ((farP + nearP) - (2.0 * z01 - 1.0) * (farP - nearP));
}

// How much stored depth a metre along the face's axis is worth `d` metres from where the faces
// were drawn: tileFaceProject's z, differentiated.
float tileDepthPerMetre(float d, float nearP, float farP) {
    return farP * nearP / (d * d * (farP - nearP));
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

// What view `view` of a light, standing at `origin`, stored in P's direction: x the stored
// surface's depth and y P's, both along the face's axis. Nearer than the near plane or past the
// far, nothing was drawn, so the stored depth reads as unbounded.
vec2 tileViewRead(TileLight t, int view, vec3 origin, vec3 P) {
    vec3 rel = P - origin;
    int face = punctualCubeFace(rel);
    float d = dot(rel, TILE_FACE_AXIS[face]);
    if (d <= t.nearP || d >= t.farP)
        return vec2(1e30, d);
    vec3 pc = tileFaceProject(face, rel, t.nearP, t.farP);
    float stored = tileDepthAt(tileCell(t.first + 6 * view + face, t.edge), t.span, pc.xy);
    return vec2(tileLinearDepth(stored, t.nearP, t.farP), d);
}

// Whether light `li` reaches a point through its cached faces, in ONE tap from its first view:
// 1 lit, 0 not. For a point that is not a surface -- a cell of air -- so there is no receiver
// plane, and the bias is in METRES along the face rather than in depth, which is what keeps it
// the same size near the light, where the stored depth is fine-grained, and near the range,
// where it is coarse. A point past the range is lit; the light has reached zero there anyway.
float tileVisibility(uint li, vec3 P, float biasMetres) {
    TileLight t = tileLightAt(li);
    vec2 read = tileViewRead(t, 0, t.centre, P);
    return read.x < read.y - biasMetres ? 0.0 : 1.0;
}
