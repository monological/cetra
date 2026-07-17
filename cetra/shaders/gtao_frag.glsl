#version 330 core
in vec2 TexCoords;
layout(location = 0) out vec4 AoOut; // visibility (1 = unoccluded)
layout(location = 1) out vec4 GiOut; // gathered one-bounce radiance (SSGI; only drawn when on)

// Ground-Truth AO via the 2023 "Visibility Bitmask" (Therrien et al.). For each
// pixel we reconstruct its view-space position and normal, then sweep a few
// slices; along each slice we march the neighbourhood on both sides and, for
// every occluder, mark the angular sectors it hides in a 32-bit mask. The
// occluded interval is bounded front-and-back by an assumed THICKNESS, so a
// thin object shadows only a finite slab and geometry behind it stays lit --
// the win over single-horizon GTAO (and the substrate for screen-space GI: the
// unset bits are exactly the directions free to gather bounce light). Sample
// angles are mapped into the hemisphere *around the surface normal*, so a flat
// surface's coplanar neighbours land on the hemisphere boundary and set no bits.
// Occlusion is popcount(mask)/32, averaged over slices.
//
// Positions are reconstructed from a stored LINEAR view-space Z (the aux
// G-buffer's .z channel), not the non-linear DEPTH24 buffer: at grazing angles
// DEPTH24 quantizes into an iso-depth staircase whose cliffs the AO reads as
// occlusion -- static banding no bias or denoise removes. Linear Z has uniform
// precision and reconstructs a smooth surface.
uniform sampler2D linDepthTex; // Aux G-buffer; .z = linear view-space Z (<0), 0 = sky
uniform sampler2D noiseTex;    // 4x4 random rotation vectors, tiled (spatial)
uniform sampler2D normalsTex;  // View-space normals (xyz); .a is the SSR marker
uniform int useNormalsTex;
uniform mat4 projection; // Only [0][0]/[1][1] focal terms are used, for XY reconstruction
uniform vec2 noiseScale; // ao resolution / 4
uniform float radius;    // Occlusion reach in view-space units
uniform int temporal;    // 1 when the AO accumulation pass is active
uniform int frameIndex;  // Drives the per-frame slice rotation when temporal
uniform sampler2D hdrTex; // Resolved lit scene color -- the radiance SSGI gathers from occluders
uniform int gatherGI;     // 1 = also gather one-bounce irradiance into GiOut (SSGI)

const float PI = 3.14159265359;
const float HALF_PI = 1.57079632679;
const int SLICES = 2;                 // Screen-space sweep directions
const int STEPS = 8;                  // Horizon-march samples per side
const uint SECTOR_COUNT = 32u;        // Bits in the visibility mask
const float THICKNESS = 0.25;         // Assumed view-space depth of an occluder
const float MAX_SCREEN_RADIUS = 0.08; // Clamp UV reach so near geometry can't thrash
const float HEIGHT_BIAS = 0.04;       // Min sin(elevation) a sample needs to count as an occluder

// View-space position from screen UV + stored linear view Z (RH, z < 0):
// Xv = ndc.x * (-z) / focalX, Yv likewise; a jitter-constant XY offset cancels
// in the relative sVec the occlusion math uses.
vec3 viewPosFromLinZ(vec2 uv, float linZ, vec2 invFocal)
{
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc * (-linZ) * invFocal, linZ);
}

// 32-bit population count (SWAR). GLSL 3.30 has uint + bit ops but not the
// bitCount() builtin (added in 4.00), so we roll our own to stay on 330.
uint popCount(uint v)
{
    v = v - ((v >> 1u) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2u) & 0x33333333u);
    v = (v + (v >> 4u)) & 0x0F0F0F0Fu;
    return (v * 0x01010101u) >> 24u;
}

// Bits for every sector spanned by the normalised range [lo, hi] (each in
// [0,1] across the hemisphere), for OR-ing into the running occlusion mask.
uint sectorBits(float lo, float hi)
{
    lo = clamp(lo, 0.0, 1.0);
    hi = clamp(hi, 0.0, 1.0);
    uint startBit = min(uint(lo * float(SECTOR_COUNT)), SECTOR_COUNT - 1u);
    uint count = uint(ceil((hi - lo) * float(SECTOR_COUNT)));
    if (count == 0u)
        return 0u;
    count = min(count, SECTOR_COUNT);
    return (0xFFFFFFFFu >> (SECTOR_COUNT - count)) << startBit;
}

void main()
{
    float linZ = texture(linDepthTex, TexCoords).z;
    if (linZ >= -1e-4) {
        // Sky / background: the aux buffer clears to 0 there (opaque geometry
        // always has linear Z <= -near < 0). Fully unoccluded, no bounce.
        AoOut = vec4(1.0);
        GiOut = vec4(0.0);
        return;
    }

    // Focal reciprocal, hoisted once so view-XY reconstruction is a per-sample
    // multiply instead of a divide.
    vec2 invFocal = 1.0 / vec2(projection[0][0], projection[1][1]);
    vec3 P = viewPosFromLinZ(TexCoords, linZ, invFocal);
    vec3 V = normalize(-P); // View space: eye at origin, so -P points at the camera

    vec3 gbufferN = texture(normalsTex, TexCoords).xyz;
    if (useNormalsTex == 1 && dot(gbufferN, gbufferN) <= 0.01) {
        // A2C draws (hair) stamp a zero-normal marker and leave a chaotic tangle
        // of thin strands in the depth buffer. A normal reconstructed from depth
        // derivatives there is pure noise, and since screen-space AO is resolved
        // after TAA it never gets stabilised -- the jitter reshuffles the strands
        // each frame and the AO flickers. Skip these pixels; hair still receives
        // its baked material AO in the lighting pass.
        AoOut = vec4(1.0);
        GiOut = vec4(0.0);
        return;
    }
    // G-buffer normal when the MRT is on, else a depth-derivative normal (the
    // --no-normals-mrt debug path; facets on curves, but never NaN).
    vec3 N = useNormalsTex == 1 ? normalize(gbufferN) : normalize(cross(dFdx(P), dFdy(P)));

    // Per-pixel spatial rotation of the slice set; the 4x4 box blur cancels the
    // tile. When temporal AO accumulation is on, add a per-frame golden-ratio
    // turn so successive frames sample different directions and the accumulation
    // integrates them (temporal supersampling). Frame-static otherwise -- without
    // accumulation a per-frame turn just flickers with nothing to average.
    vec2 rnd = texture(noiseTex, TexCoords * noiseScale).xy;
    float sliceRot = rnd.x * 0.5;
    float stepJitter = rnd.y;
    if (temporal == 1) {
        float t = float(frameIndex) * 0.61803398875;
        sliceRot = fract(sliceRot + t);
        stepJitter = fract(stepJitter + t * 1.32471795724);
    }

    // View-space radius -> UV radius at this depth (world-constant reach), clamped.
    // Closed form: only the +radius offset in X matters and clip w = -P.z, so the
    // UV delta reduces to 0.5 * focalX * radius / (-linZ).
    float screenRadius = min(0.5 * projection[0][0] * radius / (-linZ), MAX_SCREEN_RADIUS);

    // Below sampling resolution the occlusion is unmeasurable: when the whole
    // reach fits inside one depth texel, every NEAREST tap returns the center's
    // own Z, and on a tilted surface the flat-Z sample vector fabricates a
    // phantom step-up occluder -- distant grounds band with false occlusion
    // (the texel-snap phase cycles with perspective). Declare such pixels
    // unoccluded instead. noiseScale is ao-resolution / 4.
    if (screenRadius * 4.0 * max(noiseScale.x, noiseScale.y) < 1.0) {
        AoOut = vec4(1.0);
        GiOut = vec4(0.0);
        return;
    }

    float occlusion = 0.0;
    vec3 gi = vec3(0.0); // one-bounce irradiance gathered from occluders (SSGI)
    for (int s = 0; s < SLICES; s++) {
        float phi = (float(s) + sliceRot) * (PI / float(SLICES));
        vec2 dir = vec2(cos(phi), sin(phi));

        // Slice plane basis (screen-aligned view-space approximation) and the
        // surface normal projected into it. tangent is the in-plane axis
        // perpendicular to V and sets the sign of angles within the slice.
        vec3 sliceDir = vec3(dir, 0.0);
        vec3 planeNormal = normalize(cross(sliceDir, V));
        vec3 tangent = cross(V, planeNormal);
        vec3 projN = N - planeNormal * dot(N, planeNormal);
        float projNLen = length(projN);
        if (projNLen < 1e-5)
            continue;
        // Signed angle of the projected normal from V: the hemisphere this slice
        // integrates spans [n - HALF_PI, n + HALF_PI].
        float n = sign(dot(projN, tangent)) * acos(clamp(dot(projN, V) / projNLen, -1.0, 1.0));
        float hemiStart = n - HALF_PI;

        uint bitfield = 0u;
        for (int t = 0; t < STEPS; t++) {
            float tt = (float(t) + stepJitter) / float(STEPS);
            vec2 stepUV = dir * tt * screenRadius;
            for (int side = 0; side < 2; side++) {
                float sgn = side == 0 ? -1.0 : 1.0;
                vec2 sUV = TexCoords + sgn * stepUV;
                if (sUV.x < 0.0 || sUV.x > 1.0 || sUV.y < 0.0 || sUV.y > 1.0)
                    continue;
                float sZ = texture(linDepthTex, sUV).z;
                if (sZ >= -1e-4)
                    continue; // sky / background sample

                vec3 sVec = viewPosFromLinZ(sUV, sZ, invFocal) - P;
                float len = length(sVec);
                if (len < 1e-4 || len > radius)
                    continue;

                // Skip samples at/below the tangent plane. On a flat surface seen
                // at a grazing angle, coplanar neighbours toward the camera map to
                // small angles and would self-occlude into stable streaks; the
                // bias keeps only geometry that genuinely rises above the surface.
                if (dot(sVec, N) <= HEIGHT_BIAS * len)
                    continue;

                // Front and back faces of the occluder (back pushed away from the
                // camera by THICKNESS) give the finite angular slab it blocks.
                float aFront = sign(dot(sVec, tangent)) *
                               acos(clamp(dot(sVec, V) / len, -1.0, 1.0));
                vec3 backVec = sVec - V * THICKNESS;
                float aBack = sign(dot(backVec, tangent)) *
                              acos(clamp(dot(normalize(backVec), V), -1.0, 1.0));

                // Map both angles into the normal hemisphere, normalised to [0,1].
                float lo = (min(aFront, aBack) - hemiStart) / PI;
                float hi = (max(aFront, aBack) - hemiStart) / PI;
                uint sampleBits = sectorBits(lo, hi);
                // SSGI: the sectors this occluder covers for the FIRST time are the
                // solid angle it newly blocks; weight its lit radiance by that
                // fraction so the nearest occluder in each direction wins (later,
                // farther occluders in already-covered sectors add nothing).
                if (gatherGI == 1) {
                    uint newBits = sampleBits & ~bitfield;
                    if (newBits != 0u)
                        gi += texture(hdrTex, sUV).rgb * (float(popCount(newBits)) /
                                                          float(SECTOR_COUNT));
                }
                bitfield |= sampleBits;
            }
        }
        occlusion += float(popCount(bitfield)) / float(SECTOR_COUNT);
    }
    occlusion /= float(SLICES);
    gi /= float(SLICES);

    AoOut = vec4(vec3(1.0 - occlusion), 1.0); // visibility (1 = unoccluded)
    GiOut = vec4(gi, 1.0);                    // gathered one-bounce radiance
}
