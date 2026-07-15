#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Extracts pixels brighter than threshold from the linear HDR scene into the
// half-res bloom buffer; rendering at half resolution with linear sampling
// doubles as the downsample. The soft knee (Karis) fades the cutoff in over
// [threshold - knee, threshold + knee] so highlights don't pop as they cross.
uniform sampler2D hdrTex;
uniform float threshold;
uniform float knee;

void main()
{
    vec3 color = max(texture(hdrTex, TexCoords).rgb, vec3(0.0));
    float brightness = max(color.r, max(color.g, color.b));

    float soft = clamp(brightness - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-5);
    float contribution = max(soft, brightness - threshold) / max(brightness, 1e-5);

    FragColor = vec4(color * contribution, 1.0);
}
