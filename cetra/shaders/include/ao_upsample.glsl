// The AO buffer's magnification, asked to look at depth (spec 11.75).
//
// GTAO runs at half render res and every consumer reads it at full res, so
// something has to invent the pixels in between. That was the sampler's own
// bilinear filter, which knows nothing about what it is averaging: at an
// occluder's contact it blends the dark texels under the occluder against the
// open floor beside it and hands back a value LIGHTER at the contact than a
// hundred pixels away -- occlusion inverted exactly where it should be
// deepest, ringed by a halo at the radius the blend reaches. That is the whole
// defect, and this is the whole fix.
//
// The weight is the bilinear weight TIMES a Gaussian on how far the tap's
// depth sits from this pixel's, so a tap on the far side of a silhouette
// contributes nothing. The tolerance and the falloff are ssao_blur_frag's own
// -- the half-res blur has always been depth-aware, and it was only ever the
// magnification afterwards that was not, so the two now reject on the same
// terms rather than on two spellings of similar-looking ones.
//
// The includer declares nothing: the samplers are PARAMETERS, probe_specular's
// arrangement, which is what lets two programs with different unit ledgers
// share one expression.

// The depth a half-res AO texel was computed from.
//
// GTAO reads the FULL-res aux with NEAREST (postfx.c forces the filter), so an
// AO texel's depth is a specific aux texel, not an average -- and texelFetch is
// how a consumer names it rather than re-deriving it. That matters more than it
// looks: a half-res texel CENTRE maps to full-res coordinate 2i+1.0 exactly,
// the boundary between two texels, where which side NEAREST lands on is the
// hardware's rounding to make. Fetching the index directly is the same texel
// every time, on every driver.
float aoTapDepth(sampler2D auxT, ivec2 aoTexel)
{
    return texelFetch(auxT, aoTexel * 2, 0).z;
}

/*
 * One AO sample at `uv`, reconstructed from the four half-res texels around it.
 *
 * Returns the whole vec4 -- .r visibility, .gba the encoded bent normal -- and
 * the bent normal rides the SAME weights, which it can because the encode is
 * affine: a weighted mean of encoded directions is the encoding of the weighted
 * mean, and one normalize at the consumer recovers a unit vector. Weighting the
 * two channels differently would point the cone term somewhere the visibility
 * it is paired with never looked.
 *
 * `zRef` is this pixel's own linear view-Z, which the caller already has: both
 * consumers run at render res where aux is 1:1, so it is a plain fetch there
 * and passing it in keeps this function from needing the caller's coordinate
 * conventions.
 */
vec4 aoFetchBilateral(sampler2D aoT, sampler2D auxT, vec2 uv, vec2 aoRes, float zRef)
{
    // Sky, or an aux buffer nothing wrote. GTAO's own sky test, so the two
    // agree on where the depth buffer stops meaning anything -- and an unbound
    // auxTex reads 0, which lands here too and leaves the plain filtered answer
    // this function replaces.
    if (zRef >= -1e-4)
        return texture(aoT, uv);

    vec2 st = uv * aoRes - 0.5;
    vec2 base = floor(st);
    vec2 f = st - base;
    ivec2 hi = ivec2(aoRes) - 1;

    // The blur's tolerance, verbatim: proportional to depth because view-Z
    // precision and surface slope both scale with distance, floored so a
    // surface at the near plane still admits its own neighbours.
    float tol = max(0.05 * abs(zRef), 0.02);

    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    // The nearest-depth tap, for when every weight collapses. A pixel whose
    // four neighbours all sit across a silhouette has no good answer, only a
    // least-wrong one, and dividing by ~0 would make it a bright or NaN speck
    // in the one place the eye is already looking.
    vec4 best = vec4(1.0, 0.5, 0.5, 0.5);
    float bestDz = 1e30;

    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            // Clamped by hand: texelFetch does not honour CLAMP_TO_EDGE.
            ivec2 ti = clamp(ivec2(base) + ivec2(dx, dy), ivec2(0), hi);
            vec4 tap = texelFetch(aoT, ti, 0);
            float dz = abs(aoTapDepth(auxT, ti) - zRef);
            if (dz < bestDz) {
                bestDz = dz;
                best = tap;
            }
            float bw = (dx == 0 ? 1.0 - f.x : f.x) * (dy == 0 ? 1.0 - f.y : f.y);
            float t = dz / tol;
            // A sky tap against a lit pixel has dz of the whole view depth, so
            // this is what rejects the background as well as the silhouette.
            sum += tap * (bw * exp(-t * t));
            wsum += bw * exp(-t * t);
        }
    }

    return wsum > 1e-4 ? sum / wsum : best;
}
