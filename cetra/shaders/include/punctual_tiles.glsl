// Cached point-light shadows (spec 13.16): the SURFACE lookup for a light whose faces are
// tiles of the punctual array -- a receiver-plane PCF for a light with no body, and the average
// of views spread over the body for one with a body. Where a point falls on the faces is
// tile_lookup.glsl's, which the fog reads too.
//
// Needs punctual_shadow.glsl (the sampler, the plane bias and the grazing fade),
// lights_ubo.glsl, noise.glsl's ign, and pbr_frag's pcssStochastic above it.

#include "tile_lookup.glsl"

// Where a read starts, off the receiver along its face normal, and how far behind a stored
// surface it must be to count as hidden, both in texels at the depth in question: a texel's
// footprint is the most a surface's depth can disagree with itself across one, at any angle,
// so a start a texel and a half up never finds the receiver it left.
#define TILE_START_TEXELS 1.5
#define TILE_BIAS_TEXELS  1.0

uniform int tileViewCount; // the views each cached light with a body was drawn from
uniform int tileViewBlur;  // 1 = each view's edge blurred to meet its neighbours'; 0 = exact

// Metres across one texel of a face, at `d` metres along its axis.
float tileTexelMetres(float d) {
    return d * 2.0 / (TILE_INNER * float(SHADOW_TILE_SIZE));
}

// View m of n over a body: its centre for m = 0, else u stratified along the segment plus a
// point of the ball from R3. shadow.c's tile_body_point placed the views at the same points.
vec3 tileBodyPoint(vec3 centre, vec3 segment, float radius, int m, int n) {
    if (m == 0)
        return centre;
    const vec3 R3 = vec3(SHADOW_TILE_R3_X, SHADOW_TILE_R3_Y, SHADOW_TILE_R3_Z);
    float u = (float(m) - 0.5) / float(n - 1);
    vec3 q = fract(0.5 + float(m - 1) * R3);
    float rho = radius * pow(q.x, 1.0 / 3.0);
    float cosT = 1.0 - 2.0 * q.y;
    float sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
    float phi = 6.2831853 * q.z;
    return centre + segment * (u - 0.5) + rho * vec3(sinT * cos(phi), cosT, sinT * sin(phi));
}

// What the view standing at `origin`, whose six faces start at cell `first`, stored in P's
// direction: x the stored surface's depth and y P's, both along the face's axis. Nearer than
// the near plane or past the far, nothing was drawn, so the stored depth reads as unbounded.
vec2 tileViewRead(int first, vec3 origin, vec3 P, float nearP, float farP, int edge,
                  float span) {
    vec3 rel = P - origin;
    int face = punctualCubeFace(rel);
    float d = dot(rel, TILE_FACE_AXIS[face]);
    if (d <= nearP || d >= farP)
        return vec2(1e30, d);
    vec3 pc = tileFaceProject(face, rel, nearP, farP);
    float stored = tileDepthAt(tileCell(first + face, edge), span, pc.xy);
    return vec2(tileLinearDepth(stored, nearP, farP), d);
}

bool tileReadHidden(vec2 read) {
    return read.x < read.y - TILE_BIAS_TEXELS * tileTexelMetres(read.y);
}

// The soft shadow of a light with a body: the average over the views spread over it of each
// view's own answer, which is what visibility from a body IS -- each view a real shadow map
// from a real point of the light, so nothing here guesses what lies behind a surface.
//
// A finite count draws the shadow as overlapping copies, a step of one in n at each view's
// hard edge. A view stands for the part of the body round it, about `s` across, and that
// part's own penumbra is a ramp s (dR - dB) / dB wide on the receiver: so a second pass reads
// each view at a point jittered across the receiver's plane by that width, and the jitter
// averages to the ramp. dB comes from the first pass, where each view that finds the receiver
// hidden says how far away what hides it is; a receiver no view finds hidden is lit, and stops
// there. So the first pass reads at jittered points too, or a ramp's outer half -- past the
// last view's edge, where none is hidden -- would be cut off, and where the views' edges
// coincide that half is most of the step: its jitter assumes the occluder a body's length
// from the light, which finds the edges the second pass then blurs at the width they need.
// The blur is taken only where TAA averages it (`pcssStochastic`): a jitter that does not turn
// moves each view's copy bodily, sheared by the spread, which reads worse than the copies
// themselves, so without TAA the average is the views' plain one, as the reference's.
//
// The kept views stay where they were drawn while the body moves -- a flame breathes and leans
// -- and the shadow moves with it: a light moved by D casts its shadow -D (dR - dB) / dB from
// where it was on the receiver, so the views read at the receiver moved +D (dR - dB) / dB give
// the moved body's shadow, to first order, with no face drawn again. The reference is drawn
// from the body as it is, and does not move its reads.
float tileViewsShadow(uint li, vec3 X, vec3 Nf, float nearP, float farP, int edge, float span) {
    vec3 centre = clusterLights[li].shadowTile.xyz;
    int first = int(clusterLights[li].shadowTile.w);
    vec3 segment = vec3(clusterLights[li].attenCutoff.zw, clusterLights[li].shadowMisc.x);
    float radius = clusterLights[li].colorIntensity.w;
    int n = tileViewCount;

    float dRecv = max(length(X - centre), nearP);
    vec3 start = X + Nf * (TILE_START_TEXELS * tileTexelMetres(dRecv));
    bool kept = tileViewBlur == 1;
    bool blur = kept && pcssStochastic == 1;

    // The part of the body each view stands for: the body's volume shared n ways, or its
    // length for a body with no radius.
    float len = length(segment);
    float volume = 3.14159265 * radius * radius * (len + 4.0 / 3.0 * radius);
    float s = radius > 0.0 ? pow(volume / float(n), 1.0 / 3.0) : len / float(n);

    // The jitter lies across the light's direction and is carried back onto the receiver's
    // plane along it, so it moves the read across the edge and never off the surface. R2 over
    // the disc, one point a view: the plastic number's reciprocal powers.
    vec3 w = normalize(start - centre);
    vec3 t1 = normalize(cross(w, abs(w.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 t2 = cross(w, t1);
    float wn = min(dot(w, Nf), -0.25);
    float turn = 6.2831853 * ign(gl_FragCoord.xy + vec2(float(pcssFrameIndex) * 5.588238));
    const vec2 R2 = vec2(0.7548776662, 0.5698402910);

    // How far the body has moved since its views were drawn, across the light's direction and
    // carried onto the receiver's plane, as the jitter is.
    vec3 moved = kept ? clusterLights[li].posRange.xyz - centre : vec3(0.0);
    moved -= w * dot(moved, w);
    moved -= w * (dot(moved, Nf) / wn);

    // The first pass's reads, before any view has said where its occluder is: moved, and
    // jittered by the ramp, as an occluder a body's length from the light would have them.
    float guess = max(dRecv / (len + 2.0 * radius) - 1.0, 0.0);
    float probe = blur ? 0.5 * s * guess : 0.0;
    int hidden = 0;
    float spread = 0.0; // the mean of (dR - dB) / dB over the views that find it hidden
    for (int m = 0; m < n; ++m) {
        vec2 q = fract(0.5 + float(m) * R2);
        float a = 6.2831853 * q.y + turn;
        vec3 delta = probe * sqrt(q.x) * (cos(a) * t1 + sin(a) * t2);
        delta -= w * (dot(delta, Nf) / wn);
        vec2 read = tileViewRead(first + 6 * m, tileBodyPoint(centre, segment, radius, m, n),
                                 start + moved * guess + delta, nearP, farP, edge, span);
        if (tileReadHidden(read)) {
            hidden++;
            spread += (read.y - read.x) / read.x;
        }
    }
    if (hidden == 0)
        return 1.0;
    if (!kept)
        return 1.0 - float(hidden) / float(n);
    spread /= float(hidden);
    float halfWidth = blur ? 0.5 * s * spread : 0.0;

    float lit = 0.0;
    for (int m = 0; m < n; ++m) {
        vec2 q = fract(0.5 + float(m) * R2);
        float a = 6.2831853 * q.y + turn;
        vec3 delta = halfWidth * sqrt(q.x) * (cos(a) * t1 + sin(a) * t2);
        delta -= w * (dot(delta, Nf) / wn);
        vec2 read = tileViewRead(first + 6 * m, tileBodyPoint(centre, segment, radius, m, n),
                                 start + moved * spread + delta, nearP, farP, edge, span);
        lit += tileReadHidden(read) ? 0.0 : 1.0;
    }
    return lit / float(n);
}

// Occlusion for light `li` through its cached faces: 1 = lit, 0 = occluded. The same lookup
// punctualShadow is for a per-frame map -- a 3x3 PCF under the receiver's own plane, faded
// out at grazing -- with the face found and projected here rather than by a matrix, and the
// views' average in place of the 3x3 wherever the light has a body.
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

    int edge = textureSize(punctualShadowMaps, 0).x;
    float span = float(SHADOW_TILE_SIZE) / float(edge);
    // The body its views were drawn over, not the one it has now: that is what they hold.
    vec3 segment = vec3(clusterLights[li].attenCutoff.zw, clusterLights[li].shadowMisc.x);
    if (clusterLights[li].colorIntensity.w > 0.0 || dot(segment, segment) > 0.0) {
        // The reads start off the surface along its true face normal, from the position's own
        // derivatives: the shading normal leans with a normal map and would start them inside.
        vec3 Nf = cross(ddxWorld, ddyWorld);
        Nf = dot(Nf, Nf) > 1e-20 ? normalize(Nf) : N;
        Nf = dot(Nf, N) < 0.0 ? -Nf : Nf;
        float soft = tileViewsShadow(li, worldPos, Nf, nearP, farP, edge, span);
        return mix(1.0, soft, trust);
    }

    vec2 duv_dz = vec2(0.0);
    vec3 axis = TILE_FACE_AXIS[face];
    if (dot(rel + ddxWorld, axis) > 0.0 && dot(rel + ddyWorld, axis) > 0.0) {
        vec3 px = tileFaceProject(face, rel + ddxWorld, nearP, farP) - pc;
        vec3 py = tileFaceProject(face, rel + ddyWorld, nearP, farP) - pc;
        duv_dz = receiverPlaneGradient(px, py);
    }

    vec3 cell = tileCell(int(clusterLights[li].shadowTile.w) + face, edge);
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
