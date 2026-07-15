#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Final post pass: composite bloom onto the linear HDR scene, then apply
// exposure, ACES tone mapping, and gamma. Mode 0 is a raw copy for frames
// that are already display-ready (debug render modes, LDR-authored apps).
uniform sampler2D hdrTex;
uniform sampler2D bloomTex;
uniform float exposure;
uniform float bloomStrength;
uniform int bloomEnabled;
uniform int tonemapMode; // 0 = passthrough, 1 = ACES + gamma

// ACES filmic fit (Narkowicz 2015)
vec3 acesTonemap(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14),
                 0.0, 1.0);
}

void main()
{
    vec3 color = texture(hdrTex, TexCoords).rgb;
    if (bloomEnabled == 1) {
        color += bloomStrength * texture(bloomTex, TexCoords).rgb;
    }

    if (tonemapMode == 1) {
        color = acesTonemap(color * exposure);
        color = pow(color, vec3(1.0 / 2.2));
    }

    FragColor = vec4(color, 1.0);
}
