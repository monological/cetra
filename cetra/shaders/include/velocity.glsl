// The screen-space motion vector, and the aux attachment it is written into.
//
// One place, because it is a CONTRACT rather than a convenience: attachment 2 carries
// (motion.xy, linear view-Z, effective roughness), and TAA, motion blur, GTAO's position
// reconstruction and the specular-occlusion pass all decode it by that layout. Two
// shaders writing it from two copies of the packing is two chances for the layout to drift
// from its readers.
//
// Requires the including shader to declare `in vec4 CurrClip;` and `in vec4 PrevClip;` --
// both UN-JITTERED, which is the other half of the contract. TAA's jitter is a sub-pixel
// sampling offset, and letting it into the velocity reports it downstream as scene motion.

// Half the clip-space delta, i.e. the offset in UV units: NDC spans 2 and UV spans 1.
vec2 screenVelocity() {
    return (CurrClip.xy / CurrClip.w - PrevClip.xy / PrevClip.w) * 0.5;
}

// The whole attachment, so the channel order exists once.
vec4 packVelocityAux(float viewZ, float perceptualRoughness) {
    return vec4(screenVelocity(), viewZ, perceptualRoughness);
}
