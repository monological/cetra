// Interleaved-gradient noise in [0,1) (Jimenez 2014) -- the cheap per-pixel
// dither every stochastic pass keys its pattern from. Advance a pattern per
// frame by offsetting the input: ign(gl_FragCoord.xy + vec2(frame * 5.588238))
// (the SSR/PCSS idiom -- an irrational-ish stride so no frame repeats).
// ssr_frag and contact_shadow_frag still carry inline copies of this hash;
// new stochastic code includes this file instead of adding another.
float ign(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}
