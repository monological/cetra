#version 330 core
in vec2 TexCoords;
layout(location = 0) out vec4 AoOut; // visibility (1 = unoccluded)
layout(location = 1) out vec4 GiOut; // gathered one-bounce radiance (SSGI; only drawn when on)
// Reflection-lobe visibility as the estimator's two SUMS, .r visible and .g
// total, not their quotient (split spec-occ; only drawn when on). Sums because
// everything downstream averages this -- the accumulation, the blur, the
// bilateral upsample -- and the denominator varies per pixel, so a mean of
// quotients is not the quotient of the means. Dividing last also gives the
// early-outs a true neutral: (0,0) contributes nothing to a neighbour's sums
// and leaves its ratio where it was, where any ratio would have to invent a
// value and drag the neighbour toward it.
layout(location = 2) out vec4 SpecOccOut;

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
// Split spec-occ routes ambient specular out of the scene color until the
// composite folds it back -- which runs after this pass, so an occluder's
// outgoing radiance is missing that share here. Summing the split buffer
// back in restores it (pre-occlusion: the cone term has not applied yet, a
// slight bounce overestimate in crevices, accepted against the loss).
uniform sampler2D specTex; // Resolved split ambient specular
uniform int gatherSpec;    // 1 = add specTex to the gather's source radiance
uniform int specOcclusion; // 1 = also test the reflection lobe against the masks

// hdrTex is already pre-exposed, so the gather inherits the conversion and
// WS_BOUNCE_MAX below is read in the same space it was written in.
#include "view.glsl"

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
#include "depth.glsl"

// Cosine of the GGX reflection lobe's half-angle from perceptual roughness: a
// mirror at 0, near-hemispheric at 1.
float specLobeCos(float perceptualRoughness)
{
    return exp2(-3.321928 * perceptualRoughness * perceptualRoughness);
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

// Bits for every sector the normalised range [lo, hi] (each in [0,1] across the
// hemisphere) covers by at least HALF -- the paper's ROUND criterion. The paper
// and the authors' own published reference shader disagree about this: the text
// and Figure 5 both say round, the reference code takes floor/ceil. What settles
// it is the bias below, not which source is quoted.
//
// The rounded EDGES are how the paper realises "half covered" in O(1): a sector
// is set when the slab reaches its midpoint. That is an approximation of true
// per-sector coverage and is meant to be -- a narrow slab straddling one
// midpoint still sets that sector -- but it is unbiased, where the form this
// replaces was not.
//
// What it replaces: floor(lo*N) with ceil((hi-lo)*N), which set every sector the
// slab TOUCHED. A sample grazing one per cent of a sector occluded it whole, so
// every occluder carried about half a sector of phantom width, on every slab, in
// every slice. That is a systematic darkening the denoise cannot remove because
// it is not noise, and it is largest exactly where slabs are thinnest -- grazing
// samples and distant geometry.
//
// A grazing slab can now legitimately round to zero sectors, which is the point
// of the criterion rather than a case to defend against; the count == 0 early-out
// below already says so.
uint sectorBits(float lo, float hi)
{
    lo = clamp(lo, 0.0, 1.0);
    hi = clamp(hi, 0.0, 1.0);
    uint startBit = min(uint(round(lo * float(SECTOR_COUNT))), SECTOR_COUNT - 1u);
    uint endBit = min(uint(round(hi * float(SECTOR_COUNT))), SECTOR_COUNT);
    if (endBit <= startBit)
        return 0u;
    uint count = min(endBit - startBit, SECTOR_COUNT);
    return (0xFFFFFFFFu >> (SECTOR_COUNT - count)) << startBit;
}

// Bits for every sector the reflection lobe REACHES -- TOUCH semantics, not
// sectorBits' round, and the asymmetry is the point rather than an oversight.
// An occluder is asked "do you cover this sector", so a grazing slab that
// covers almost none of one should set nothing. A lobe is asked "do you reach
// it", and a mirror lobe is narrower than a single sector: rounded, it would
// round away to no bits at all, the total below would be zero, and the surface
// would report itself unoccluded no matter what stood in front of it. So the
// range is widened to whole sectors and is never empty once it meets the
// hemisphere.
//
// The [0,1] clamp is the HORIZON REFERENCE, and it is why this needs no second
// term. A reflection lobe always hangs partly below its own horizon and the
// BRDF has already zeroed that half, so the cone form this replaces had to
// measure itself against a second cone overlap against an open hemisphere;
// clamping the sector range does the same thing exactly, for nothing. It was
// that reference being an approximation -- not the idea of having one -- that
// the cone needed a smoothstep to hide, and which rang wherever it failed.
uint lobeSectors(float lo, float hi)
{
    // Entirely below the horizon: this slice sees no part of the lobe. Tested
    // BEFORE the clamp, which would otherwise collapse both ends onto the same
    // edge and hand back a spurious sector there.
    if (hi <= 0.0 || lo >= 1.0)
        return 0u;
    lo = clamp(lo, 0.0, 1.0);
    hi = clamp(hi, 0.0, 1.0);
    uint startBit = min(uint(floor(lo * float(SECTOR_COUNT))), SECTOR_COUNT - 1u);
    uint endBit = clamp(uint(ceil(hi * float(SECTOR_COUNT))), startBit + 1u, SECTOR_COUNT);
    return (0xFFFFFFFFu >> (SECTOR_COUNT - (endBit - startBit))) << startBit;
}

void main()
{
    float linZ = texture(linDepthTex, TexCoords).z;
    // Local depth-slope of the surface, one AO-res texel wide: the NEAREST
    // linZ fetch snaps samples to the texel grid, so on a sloped surface a
    // reconstructed neighbor carries up to ~a texel of depth-slope as phantom
    // "height". Computed before any divergent branch (derivative rules).
    float zGrain = fwidth(linZ);
    if (linZ >= -1e-4) {
        // Sky / background: the aux buffer clears to 0 there (opaque geometry
        // always has linear Z <= -near < 0). Fully unoccluded, no bounce. The
        // bent normal is view-facing: the consumer's zero-normal guard returns
        // before reading it, but the blur averages these texels at silhouettes
        // and an unwritten channel there would be undefined.
        AoOut = vec4(1.0, 0.5, 0.5, 1.0);
        GiOut = vec4(0.0);
        SpecOccOut = vec4(0.0, 0.0, 0.0, 1.0);
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
        AoOut = vec4(1.0, 0.5, 0.5, 1.0);
        GiOut = vec4(0.0);
        SpecOccOut = vec4(0.0, 0.0, 0.0, 1.0);
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
        // Declared unoccluded, and an unoccluded surface's average visible
        // direction is its own normal.
        AoOut = vec4(1.0, N * 0.5 + 0.5);
        GiOut = vec4(0.0);
        SpecOccOut = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float occlusion = 0.0;
    vec3 gi = vec3(0.0);   // one-bounce irradiance gathered from occluders (SSGI)
    vec3 bent = vec3(0.0); // sum of the still-visible directions (bent normal)
    // The specular lobe this pixel reflects along, and how wide it is. Both are
    // constant across slices, so they are taken once; the roughness is the aux
    // buffer's own .w, the channel this shader has always had bound and never
    // read. Half res, which is the one thing the term gives up by moving here.
    vec3 R = reflect(-V, N);
    float lobeHalf = specOcclusion == 1
                         ? acos(clamp(specLobeCos(texture(linDepthTex, TexCoords).w), -1.0, 1.0))
                         : 0.0;
    float specVis = 0.0; // sector-weighted lobe still visible, summed over slices
    float specTot = 0.0; // ... and the lobe it is a fraction of
    // Per-sector rotation step for the bent-normal recurrence below; constant
    // across slices.
    float dAng = PI / float(SECTOR_COUNT);
    float cd = cos(dAng);
    float sd = sin(dAng);
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
                // zGrain additionally rejects heights within the NEAREST-snap
                // reconstruction noise (one texel of depth-slope): without it,
                // sloped surfaces at large view distances band with phantom
                // occlusion rows as the snap error crosses the fixed bias.
                if (dot(sVec, N) <= HEIGHT_BIAS * len + zGrain)
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
                    if (newBits != 0u) {
                        vec3 rad = texture(hdrTex, sUV).rgb;
                        if (gatherSpec == 1)
                            rad += texture(specTex, sUV).rgb;
                        // Clamp the source radiance -- the SUM, once, so the
                        // bound the exposure meter sees does not double when
                        // the split share joins: one specular-hot texel
                        // gathered here becomes a firefly that the a-trous
                        // blur smears into a bright splat downstream.
                        rad = min(rad, vec3(WS_BOUNCE_MAX));
                        gi += rad * (float(popCount(newBits)) / float(SECTOR_COUNT));
                    }
                }
                bitfield |= sampleBits;
            }
        }
        occlusion += float(popCount(bitfield)) / float(SECTOR_COUNT);

        // Specular occlusion, against the same mask and while it still exists.
        // The lobe is projected into this slice's plane and read on the very
        // scale the occluders were marked on, so the two are the same
        // measurement rather than two approximations meeting downstream.
        //
        // projRLen weights the slice the way projNLen already gates one: a
        // reflection direction perpendicular to this plane projects to nothing,
        // and a slice that cannot see the lobe should not get a vote on it.
        if (specOcclusion == 1) {
            vec3 projR = R - planeNormal * dot(R, planeNormal);
            float projRLen = length(projR);
            if (projRLen > 1e-5) {
                float aR = sign(dot(projR, tangent)) *
                           acos(clamp(dot(projR, V) / projRLen, -1.0, 1.0));
                uint lobe = lobeSectors((aR - lobeHalf - hemiStart) / PI,
                                        (aR + lobeHalf - hemiStart) / PI);
                specVis += projRLen * float(popCount(lobe & ~bitfield));
                specTot += projRLen * float(popCount(lobe));
            }
        }

        // Bent normal: the average of the directions still VISIBLE, i.e. the
        // sectors this slice left unset. Sector k spans the hemisphere angles
        // [hemiStart + k*dAng, +(k+1)*dAng], so its mid-direction is
        // cos(t)*V + sin(t)*tangent -- and since V and tangent are constant
        // across the slice, the whole sum collapses to two scalar sums.
        //
        // The (cos, sin) pair is stepped by a rotation recurrence rather than
        // called per sector: a sincos in a 32-iteration loop, twice per pixel,
        // is the one place this shader would pay real ALU for the bent normal,
        // and fp32 drift over 32 steps of a fixed angle is ~1e-6.
        float ca = cos(hemiStart + 0.5 * dAng);
        float sa = sin(hemiStart + 0.5 * dAng);
        float sumC = 0.0;
        float sumS = 0.0;
        for (uint k = 0u; k < SECTOR_COUNT; k++) {
            if ((bitfield & (1u << k)) == 0u) {
                sumC += ca;
                sumS += sa;
            }
            float rc = ca * cd - sa * sd;
            sa = sa * cd + ca * sd;
            ca = rc;
        }
        bent += sumC * V + sumS * tangent;
    }
    occlusion /= float(SLICES);
    gi /= float(SLICES);

    // Fully occluded (or every slice degenerate) leaves no visible direction
    // to average; the surface normal is the honest answer there.
    vec3 bentN = dot(bent, bent) > 1e-8 ? normalize(bent) : N;

    AoOut = vec4(1.0 - occlusion, bentN * 0.5 + 0.5); // visibility (1 = unoccluded) + bent normal
    GiOut = vec4(gi, 1.0);                            // gathered one-bounce radiance
    // Normalised so both sums land in [0,1] and the ratio is unchanged: the
    // most a slice can contribute is every sector at full weight.
    const float SPEC_NORM = 1.0 / (float(SECTOR_COUNT) * float(SLICES));
    SpecOccOut = vec4(specVis * SPEC_NORM, specTot * SPEC_NORM, 0.0, 1.0);
}
