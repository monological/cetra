#version 330 core

in vec2 TexCoords;

out float Absorbance;

uniform sampler2D albedoTex;
uniform int tsmHasAlbedo;
uniform float tsmOpacity;

// Absorbance, so the accumulation can be a GL_ONE/GL_ONE blend: transmittance
// MULTIPLIES along a ray and only its logarithm adds. mboit.glsl carries the
// same primitive for the same reason, and this is deliberately its own copy --
// that file's constants belong to a reconstruction this pass does not perform.
//
// The clamp caps a fully opaque fragment rather than letting -log(0) run to
// infinity. It is the only thing bounding the sum, and the accumulation target
// is fp32 because a dense groom stacks many strands into one texel.
float absorbance(float a)
{
    return -log(1.0 - clamp(a, 0.0, 0.9999));
}

void main()
{
    // Coverage as the surface itself computes it: the albedo texture's alpha
    // times the material's opacity. No view-dependent term -- the depth pass
    // has no view vector, and the ray being attenuated here is the light's.
    float coverage = tsmOpacity;
    if (tsmHasAlbedo == 1)
        coverage *= texture(albedoTex, TexCoords).a;

    Absorbance = absorbance(coverage);
}
