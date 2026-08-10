#version 330 core

uniform sampler2D absorbance;

// Turn the accumulated absorbance into a transmittance and store THAT.
//
// Which way round this goes is the whole reason the shadow lookup may filter
// these layers at all. Transmittance averages linearly, so a PCF box over the
// stored values is exact; absorbance does not, and averaging it would carry
// Jensen's bias -- spec 11.17 measured that error at 0.368 reconstructed where
// the true average was 0.568, and it grows as the square of the absorbance, so
// it is worst on precisely the dense casters this feature exists for.
//
// Written to gl_FragDepth because the destination is a layer of the depth
// array: DEPTH_COMPONENT24 is a 24-bit UNORM, which is better storage for a
// value in [0,1] than the moment array's fp16, and it costs no sampler because
// the shadow lookup is already bound to that array.
void main()
{
    float b0 = texelFetch(absorbance, ivec2(gl_FragCoord.xy), 0).r;
    gl_FragDepth = exp(-b0);
}
