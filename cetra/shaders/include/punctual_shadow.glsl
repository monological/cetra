// Punctual shadow map interface: the uniforms that shadow.c's
// bind_shadow_maps_to_program fills for the perspective light types, and the
// one lookup that reads them.
//
// Same contract as csm.glsl: these names are shared with ONE C function and
// the binder is location-guarded, so renaming a uniform here silently no-ops
// the upload in whichever shader you forgot, with no error anywhere.
//
// One 2D array serves every perspective type. A point light's six 90-degree
// frusta are six ordinary layers the caller picks between, and a panel's five
// in its own frame (spec 13.27), so no
// samplerCubeArray (GLSL 400 against a shader set that is uniformly 330) and
// no second texture unit -- and there is no second unit to be had, since
// pbr_frag samples all 16.
//
// MAX_PUNCTUAL_SHADOW_LAYERS comes from the constants C reads too; program.c still
// checks this array's size against it, as it does the cascade ones.

#include "shadow_tile_constants.glsl"

uniform sampler2DArray punctualShadowMaps;
// Edge length the array was built at. This used to be a GLSL literal mirroring a
// C one, on the reasoning that the size was fixed where the cascades' was not.
// It is not fixed any more: shadow.c picks it from a VRAM budget against the
// layer count, so one spot gets a finer map than six point-light faces would
// (shadow.h). A literal here would have silently mis-sized the PCF step by the
// same factor.
uniform float punctualShadowMapSize;
uniform mat4 punctualShadowMatrix[MAX_PUNCTUAL_SHADOW_LAYERS];
// Layers rendered this frame. 0 disables every punctual shadow at once, which
// is what the shadow system's master switch and an absent depth pass both
// reduce to; a light carries its own base layer in its UBO entry, so this is
// the only global the lookup needs.
uniform int punctualShadowCount;

#include "cube_face.glsl"

// The plane-bias policy (constants, 2x2 solve, one-sided clamp) is shared
// with the cascade lookup; see receiver_plane.glsl for the rationale.
#include "receiver_plane.glsl"
// Below this NdotL the map is not answering the question asked of it. One
// perspective map tests whether the light's CENTRE is visible, and for a
// receiver nearly edge-on to that centre the real answer for an AREA source is
// "half the panel", which a binary test cannot express.
//
// It used to also cover spurious self-occlusion at grazing, which the plane bias
// now handles properly, so the threshold is a third of what it was -- it is back
// to describing the single-sample approximation rather than papering over acne.
//
// A no-op on the point/spot path, whose radiance already carries the same NdotL
// and is zero wherever the fade is not one. Only the LTC path has anything to
// lose here, because its form factor carries orientation instead.
#define PUNCTUAL_GRAZING_FADE 0.07

// The penumbra's taps, in the blocker search and in the filter: a golden-angle spiral, uniform at
// any count, rotated per pixel and per frame where TAA or a still view's average turns them.
#define PUNCTUAL_PENUMBRA_TAPS 32
// How far nearer the light than the receiver's own plane a tap must be to occlude it, in texels
// at the receiver: the stored depth is its texel's centre, which on a receiver tilted to the light
// stands off the plane at the tap by up to half a texel times the slope. A depth-buffer epsilon
// finds the receiver blocking itself, and a share of the distance misses a part a millimetre tall
// on a board.
#define PUNCTUAL_BLOCKER_TEXELS 0.5
// The steepest the receiver's slope to the light is taken as, a tangent: past it the surface is
// edge-on, where the grazing fade already takes the shadow away.
#define PUNCTUAL_SLOPE_MAX 8.0

vec2 punctualDiskTap(int i) {
    float r = sqrt((float(i) + 0.5) / float(PUNCTUAL_PENUMBRA_TAPS));
    float theta = float(i) * 2.3999632;
    return r * vec2(cos(theta), sin(theta));
}

// A layer's depth terms, read back off its light-space matrix: the view in it is rigid, so its w
// row is the view's -z row and its z row that times A, plus B. A stored depth d is then the axial
// distance B / ((2 d - 1) + A), for any layer's near and far.
vec2 punctualDepthTerms(mat4 m) {
    int j = abs(m[0][3]) > abs(m[1][3]) ? 0 : 1;
    if (abs(m[2][3]) > abs(m[j][3]))
        j = 2;
    float a = -m[j][2] / m[j][3];
    return vec2(a, m[3][2] + a * m[3][3]);
}

float punctualDistance(vec2 terms, float depth01) {
    return terms.y / ((2.0 * depth01 - 1.0) + terms.x);
}

// The receiver's plane, through `X` with normal `n`, as the layer sees it. The matrix's rows are
// P00 s, P11 u, the depth row and f, the light's frame scaled by its projection with its position
// in w, so a tap at NDC (x, y) looks along f + s x / P00 + u y / P11 and meets the plane at the
// axial distance n.(X - C) / n.(that direction): the plane's four terms, for
// punctualPlaneDistance.
vec4 punctualPlane(mat4 m, vec3 X, vec3 n) {
    vec3 r0 = vec3(m[0][0], m[1][0], m[2][0]);
    vec3 r1 = vec3(m[0][1], m[1][1], m[2][1]);
    vec3 f = vec3(m[0][3], m[1][3], m[2][3]);
    float p00 = length(r0), p11 = length(r1);
    vec3 s = r0 / p00, u = r1 / p11;
    vec3 c = -s * (m[3][0] / p00) - u * (m[3][1] / p11) - f * m[3][3];
    return vec4(dot(n, f), dot(n, s) / p00, dot(n, u) / p11, dot(n, X - c));
}

// Where a tap's ray meets the receiver's plane, as an axial distance; far past everything where it
// never does.
float punctualPlaneDistance(vec4 plane, vec2 uv) {
    float den = plane.x + plane.y * (2.0 * uv.x - 1.0) + plane.z * (2.0 * uv.y - 1.0);
    float t = plane.w / den;
    return abs(den) > 1e-6 && t > 0.0 ? t : 1e30;
}

// The penumbra a source `sourceUV` across in the map casts onto the receiver at `pc`, `zR` from
// it: the blockers found over the source's footprint, their mean distance setting the filter's
// width by similar triangles -- a source of half-width r at zR, blocked at zB, throws a penumbra
// r (zR - zB) / zB wide -- and the filter over that width.
//
// A tap occludes when it stands nearer the light than the receiver's own plane does along that
// tap's ray, by `separation`. The plane is met exactly, from the vertex normal, and not
// extrapolated from screen-space derivatives as the 3x3's is: a fragment beside a part's
// silhouette takes its derivatives across the part, which tilts that plane toward the light and
// reads the part as no blocker at all, a lit speck in its shadow.
float punctualPenumbra(int layer, vec3 pc, float zR, float sourceUV, vec2 texel,
                       float separation, vec4 plane) {
    vec2 terms = punctualDepthTerms(punctualShadowMatrix[layer]);
    // Built once, so the search and the filter read the same rotated taps.
    mat2 rot = mat2(1.0);
    if (pcssStochastic == 1) {
        float ang = 6.2831853 * ign(gl_FragCoord.xy + vec2(float(pcssFrameIndex) * 5.588238));
        rot = mat2(cos(ang), sin(ang), -sin(ang), cos(ang));
    }

    float reach = PUNCTUAL_PENUMBRA_MAX_TEXELS * texel.x;
    float searchUV = min(sourceUV, reach);
    float blockers = 0.0;
    float found = 0.0;
    for (int i = 0; i < PUNCTUAL_PENUMBRA_TAPS; ++i) {
        vec2 off = rot * punctualDiskTap(i) * searchUV;
        vec2 uv = clamp(pc.xy + off, texel * 0.5, 1.0 - texel * 0.5);
        float zTap = punctualDistance(terms, texture(punctualShadowMaps, vec3(uv, float(layer))).r);
        if (zTap < punctualPlaneDistance(plane, uv) - separation) {
            blockers += zTap;
            found += 1.0;
        }
    }
    if (found < 0.5)
        return 1.0;
    float zB = blockers / found;
    float filterUV = clamp(sourceUV * max(zR - zB, 0.0) / max(zB, 1e-6 * zR), texel.x, reach);

    float lit = 0.0;
    for (int i = 0; i < PUNCTUAL_PENUMBRA_TAPS; ++i) {
        vec2 off = rot * punctualDiskTap(i) * filterUV;
        vec2 uv = clamp(pc.xy + off, texel * 0.5, 1.0 - texel * 0.5);
        float zTap = punctualDistance(terms, texture(punctualShadowMaps, vec3(uv, float(layer))).r);
        lit += zTap < punctualPlaneDistance(plane, uv) - separation ? 0.0 : 1.0;
    }
    return lit / float(PUNCTUAL_PENUMBRA_TAPS);
}

// Occlusion for one perspective map: 1 = lit, 0 = fully occluded. A layer
// outside the live range, a fragment behind the light, a fragment off the map,
// and a fragment edge-on to the light all read as lit -- the third via the
// array's white border, the last via the fade above.
//
// `ddxWorld`/`ddyWorld` are the screen-space derivatives of the world position,
// taken by the CALLER. They cannot be taken here: this runs inside the clustered
// light loop, which is non-uniform control flow, where derivatives are
// undefined. Passing them in is what lets the receiver's own plane be
// reconstructed per light without leaving that constraint.
//
// `sourceRadius` is the emitter's world half-extent: a panel's, which then casts the penumbra its
// size implies wherever that is wider than a texel, against the receiver's plane through
// `worldPos` with normal `planeN`, the vertex normal, which no normal map tilts. 0, a point's or
// a spot's, is the 3x3.
float punctualShadow(int layer, vec3 worldPos, vec3 N, vec3 L, vec3 ddxWorld, vec3 ddyWorld,
                     float sourceRadius, vec3 planeN) {
    if (layer < 0 || layer >= punctualShadowCount)
        return 1.0;

    float ndl = clamp(dot(N, L), 0.0, 1.0);
    float trust = smoothstep(0.0, PUNCTUAL_GRAZING_FADE, ndl);
    if (trust <= 0.0)
        return 1.0;

    vec4 ls = punctualShadowMatrix[layer] * vec4(worldPos, 1.0);
    if (ls.w <= 0.0)
        return 1.0;

    vec3 pc = ls.xyz / ls.w * 0.5 + 0.5;
    if (pc.z > 1.0 || pc.x < 0.0 || pc.x > 1.0 || pc.y < 0.0 || pc.y > 1.0)
        return 1.0;

    // Receiver-plane depth bias (Isidoro, GDC 2006). The old bias displaced the
    // sample toward the light by a slope-scaled world distance, which is a guess
    // at how much depth a tilted surface covers across the kernel. This computes
    // it instead: differentiate the receiver's own shadow-space position along
    // both screen axes, then solve the 2x2 for how its depth changes per unit of
    // shadow UV. Each tap is then compared against where the receiver's PLANE
    // would be at that tap, which is exact at any slope -- so a surface edge-on
    // to the light no longer needs a bias large enough to detach its own contact.
    vec4 lx = punctualShadowMatrix[layer] * vec4(worldPos + ddxWorld, 1.0);
    vec4 ly = punctualShadowMatrix[layer] * vec4(worldPos + ddyWorld, 1.0);
    vec2 duv_dz = vec2(0.0);
    if (lx.w > 0.0 && ly.w > 0.0) {
        vec3 px = lx.xyz / lx.w * 0.5 + 0.5 - pc;
        vec3 py = ly.xyz / ly.w * 0.5 + 0.5 - pc;
        duv_dz = receiverPlaneGradient(px, py);
    }

    // 3x3 PCF, which the directional cascades have always had and this never
    // did: a single tap quantizes the edge to the texel grid, and reads as a
    // staircase on any silhouette not aligned to it.
    //
    // Taps are clamped to the face. A point light's six faces meet on 45-degree
    // planes, and a fragment sitting near one has taps that fall OFF this face
    // -- where a 2D array has no neighbouring face to sample, only its border,
    // which reads lit. Unclamped that draws a straight lit seam across whatever
    // the boundary crosses, and it is immune to every depth remedy because no
    // depth comparison is involved: the tap simply misses the data. Clamping
    // re-reads this face's edge texel, which is the nearest depth that actually
    // exists; the true neighbour is a face away and unreachable without a
    // samplerCubeArray (GLSL 400, above this shader set's 330).
    vec2 texel = vec2(1.0 / punctualShadowMapSize);

    // The map's world width at this receiver: the layer's x scale is 1 / tan(fov / 2) over a
    // rigid view, so this is 2 zR tan(fov / 2).
    if (sourceRadius > 0.0) {
        mat4 m = punctualShadowMatrix[layer];
        float mapWorld = 2.0 * ls.w / length(vec3(m[0][0], m[1][0], m[2][0]));
        float sourceUV = sourceRadius / mapWorld;
        float slope = min(sqrt(max(1.0 - ndl * ndl, 0.0)) / max(ndl, 1e-3), PUNCTUAL_SLOPE_MAX);
        float separation = PUNCTUAL_BLOCKER_TEXELS * texel.x * mapWorld * max(slope, 1.0);
        if (sourceUV > texel.x)
            return mix(1.0,
                       punctualPenumbra(layer, pc, ls.w, sourceUV, texel, separation,
                                        punctualPlane(m, worldPos, planeN)),
                       trust);
    }

    float ref = pc.z - SHADOW_PLANE_BIAS_FLOOR;
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 off = vec2(x, y) * texel;
            vec2 uv = clamp(pc.xy + off, texel * 0.5, 1.0 - texel * 0.5);
            float d = texture(punctualShadowMaps, vec3(uv, float(layer))).r;
            sum += (ref + receiverPlaneBias(duv_dz, off) > d) ? 0.0 : 1.0;
        }
    }
    return mix(1.0, sum / 9.0, trust);
}
