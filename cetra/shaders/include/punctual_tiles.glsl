// Cached shadows (specs 13.16 and 13.27): the SURFACE lookup for a point light or a panel whose
// faces are tiles of the punctual array -- a receiver-plane PCF for a light with no body, a
// panel always among them, and the average
// of views spread over the body for one with a body. Where a point falls on the faces is
// tile_lookup.glsl's, which the fog reads too.
//
// Needs punctual_shadow.glsl (the sampler, the plane bias and the grazing fade),
// lights_ubo.glsl, noise.glsl's ign, and pbr_frag's pcssStochastic and pcssFrameIndex above it.

#include "tile_lookup.glsl"

// Where a read starts, off the receiver along its face normal, and how far behind a stored
// surface it must be to count as hidden, both in texels at the depth in question: a texel's
// footprint is the most a surface's depth can disagree with itself across one, at any angle,
// so a start a texel and a half up never finds the receiver it left.
#define TILE_START_TEXELS 1.5
#define TILE_BIAS_TEXELS  1.0
// How edge-on to the light a receiver may lie before the jitter's carry onto its plane stops
// growing: the cosine the carry divides by, so the carry stretches at most four times.
#define TILE_CARRY_FLOOR 0.25

uniform int tileViewCount; // the views each cached light with a body was drawn from
// 1 = the kept views, read where the body has moved them and blurred to meet; 0 = the
// reference's, drawn from the body as it is and exact by their count.
uniform int tileViewsKept;

// Metres across one texel of a face, at `d` metres along its axis.
float tileTexelMetres(float d) {
    return d * 2.0 / (SHADOW_TILE_INNER * float(SHADOW_TILE_SIZE));
}

// View m of n over a light's drawn body: its centre for m = 0, else u stratified along the
// segment plus a point of the ball from R3. shadow.c's tile_body_point placed the views at the
// same points.
vec3 tileBodyPoint(TileLight t, int m, int n) {
    if (m == 0)
        return t.centre;
    const vec3 R3 = vec3(SHADOW_TILE_R3_X, SHADOW_TILE_R3_Y, SHADOW_TILE_R3_Z);
    float u = (float(m) - 0.5) / float(n - 1);
    vec3 q = fract(0.5 + float(m - 1) * R3);
    float rho = t.radius * pow(q.x, 1.0 / 3.0);
    float cosT = 1.0 - 2.0 * q.y;
    float sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
    float phi = 6.2831853 * q.z;
    return t.centre + t.segment * (u - 0.5) + rho * vec3(sinT * cos(phi), cosT, sinT * sin(phi));
}

bool tileReadHidden(vec2 read) {
    return read.x < read.y - TILE_BIAS_TEXELS * tileTexelMetres(read.y);
}

// One pass over the n views: each read at `at`, jittered over a disc `radius` across whose
// axes p1 and p2 already lie on the receiver's plane, its points from R2 -- the plastic
// number's reciprocal powers -- turned by `turn`. Returns how many find the point hidden, and
// adds each one's (dR - dB) / dB to `spread`.
int tileViewsHidden(TileLight t, int n, vec3 at, float radius, vec3 p1, vec3 p2, float turn,
                    inout float spread) {
    const vec2 R2 = vec2(0.7548776662, 0.5698402910);
    int hidden = 0;
    for (int m = 0; m < n; ++m) {
        vec2 q = fract(0.5 + float(m) * R2);
        float a = 6.2831853 * q.y + turn;
        vec3 jitter = radius * sqrt(q.x) * (cos(a) * p1 + sin(a) * p2);
        vec2 read = tileViewRead(t, m, tileBodyPoint(t, m, n), at + jitter);
        if (tileReadHidden(read)) {
            hidden++;
            spread += (read.y - read.x) / read.x;
        }
    }
    return hidden;
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
float tileViewsShadow(TileLight t, vec3 X, vec3 Nf) {
    int n = tileViewCount;
    float dRecv = max(length(X - t.centre), t.nearP);
    vec3 start = X + Nf * (TILE_START_TEXELS * tileTexelMetres(dRecv));
    bool kept = tileViewsKept == 1;
    bool blur = kept && pcssStochastic == 1;

    // The part of the body each view stands for: the body's volume shared n ways, or its
    // length for a body with no radius.
    float len = length(t.segment);
    float volume = 3.14159265 * t.radius * t.radius * (len + 4.0 / 3.0 * t.radius);
    float s = t.radius > 0.0 ? pow(volume / float(n), 1.0 / 3.0) : len / float(n);

    // The jitter lies across the light's direction and is carried back onto the receiver's
    // plane along it, so it moves the read across the edge and never off the surface. The
    // carry is linear, so it is taken once, on the disc's axes.
    vec3 w = normalize(start - t.centre);
    vec3 t1 = normalize(cross(w, abs(w.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 t2 = cross(w, t1);
    float wn = min(dot(w, Nf), -TILE_CARRY_FLOOR);
    vec3 p1 = t1 - w * (dot(t1, Nf) / wn);
    vec3 p2 = t2 - w * (dot(t2, Nf) / wn);
    float turn =
        blur ? 6.2831853 * ign(gl_FragCoord.xy + vec2(float(pcssFrameIndex) * 5.588238)) : 0.0;

    // How far the body has moved since its views were drawn, across the light's direction and
    // carried onto the receiver's plane, as the jitter is.
    vec3 moved = kept ? t.current - t.centre : vec3(0.0);
    moved -= w * dot(moved, w);
    moved -= w * (dot(moved, Nf) / wn);

    // The first pass's reads, before any view has said where its occluder is: moved, and
    // jittered by the ramp, as an occluder a body's length from the light would have them.
    float guess = max(dRecv / (len + 2.0 * t.radius) - 1.0, 0.0);
    float spread = 0.0; // the sum of (dR - dB) / dB over the views that find it hidden
    int hidden = tileViewsHidden(t, n, start + moved * guess, blur ? 0.5 * s * guess : 0.0, p1,
                                 p2, turn, spread);
    if (hidden == 0)
        return 1.0;
    // Nothing blurred and nothing moved: a second pass would read the points the first did.
    if (!blur && moved == vec3(0.0))
        return float(n - hidden) / float(n);
    spread /= float(hidden);
    float unused = 0.0;
    hidden = tileViewsHidden(t, n, start + moved * spread, blur ? 0.5 * s * spread : 0.0, p1, p2,
                             turn, unused);
    return float(n - hidden) / float(n);
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

    TileLight t = tileLightAt(li);
    vec3 rel = worldPos - t.centre;
    int face = punctualCubeFace(rel);
    vec3 pc = tileFaceProject(face, rel, t.nearP, t.farP);
    // Past the range, where the light has already reached zero.
    if (pc.z > 1.0)
        return 1.0;

    // The body its views were drawn over, not the one it has now: that is what they hold.
    if (t.radius > 0.0 || dot(t.segment, t.segment) > 0.0) {
        // The reads start off the surface along its true face normal, from the position's own
        // derivatives: the shading normal leans with a normal map and would start them inside.
        vec3 Nf = cross(ddxWorld, ddyWorld);
        Nf = dot(Nf, Nf) > 1e-20 ? normalize(Nf) : N;
        Nf = dot(Nf, N) < 0.0 ? -Nf : Nf;
        return mix(1.0, tileViewsShadow(t, worldPos, Nf), trust);
    }

    vec2 duv_dz = vec2(0.0);
    vec3 axis = TILE_FACE_AXIS[face];
    if (dot(rel + ddxWorld, axis) > 0.0 && dot(rel + ddyWorld, axis) > 0.0) {
        vec3 px = tileFaceProject(face, rel + ddxWorld, t.nearP, t.farP) - pc;
        vec3 py = tileFaceProject(face, rel + ddyWorld, t.nearP, t.farP) - pc;
        duv_dz = receiverPlaneGradient(px, py);
    }

    vec3 cell = tileCell(t.first + face, t.edge);
    float texel = 1.0 / float(SHADOW_TILE_SIZE);
    float ref = pc.z - SHADOW_PLANE_BIAS_FLOOR;
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 off = vec2(x, y) * texel;
            sum += (ref + receiverPlaneBias(duv_dz, off) > tileDepthAt(cell, t.span, pc.xy + off))
                       ? 0.0
                       : 1.0;
        }
    }
    return mix(1.0, sum / 9.0, trust);
}
