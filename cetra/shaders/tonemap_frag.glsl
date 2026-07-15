#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Final post pass: composite bloom onto the linear HDR scene, then apply
// exposure, ACES tone mapping, and gamma. Mode 0 is a raw copy for frames
// that are already display-ready (debug render modes, LDR-authored apps).
uniform sampler2D hdrTex;
uniform sampler2D bloomTex;
uniform sampler2D aoTex; // Blurred SSAO, upsampled by its linear filter
uniform float exposure;
uniform float bloomStrength;
uniform int bloomEnabled;
uniform int aoEnabled;
uniform float aoStrength;
uniform int aoDebug; // Show the raw AO buffer for verification
// 1 = ACES, 2 = PBR Neutral (passthrough frames are blitted by postfx_run
// and never reach this pass)
uniform int tonemapMode;

// ACES filmic fit (Narkowicz 2015). High contrast: crushes shadows,
// desaturates highlights — the cinematic look.
vec3 acesTonemap(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14),
                 0.0, 1.0);
}

// Khronos PBR Neutral (2024). Identity below ~0.76 so shadows, midtones,
// and material colors stay faithful; only highlights are compressed.
// Made for product/model viewers where albedo fidelity matters.
vec3 pbrNeutralTonemap(vec3 color)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
        return color;

    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    float g = 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, newPeak * vec3(1.0), g);
}

void main()
{
    vec3 color = texture(hdrTex, TexCoords).rgb;

    if (aoDebug == 1) {
        FragColor = vec4(vec3(texture(aoTex, TexCoords).r), 1.0);
        return;
    }
    if (aoEnabled == 1) {
        // Occlude before adding bloom: bloom models lens scatter, which
        // happens after the light already left the scene
        float ao = texture(aoTex, TexCoords).r;
        color *= mix(1.0, ao, aoStrength);
    }

    if (bloomEnabled == 1) {
        color += bloomStrength * texture(bloomTex, TexCoords).rgb;
    }

    color *= exposure;
    color = tonemapMode == 1 ? acesTonemap(color) : pbrNeutralTonemap(color);
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));

    FragColor = vec4(color, 1.0);
}
