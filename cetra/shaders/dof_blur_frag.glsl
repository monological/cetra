#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Depth-of-field, pass 2 of 3: gather blur at half resolution. Each pixel
// spreads a Vogel disk sized by its own circle-of-confusion; a neighbour
// contributes only if its CoC reaches back to this pixel (scatter-as-gather),
// which keeps a sharp foreground from bleeding onto a blurred background.
// Unrotated on purpose — a per-pixel rotation would turn the tap count into
// grainy bokeh noise.
uniform sampler2D cocColorTex; // Half-res scene colour (rgb) + signed CoC (a)
uniform vec2 texelSize;        // Half-res texel size

const int TAPS = 16;
const float GOLDEN_ANGLE = 2.399963230;

vec2 vogel(int i)
{
    float r = sqrt((float(i) + 0.5) / float(TAPS));
    float theta = float(i) * GOLDEN_ANGLE;
    return r * vec2(cos(theta), sin(theta));
}

void main()
{
    vec4 center = texture(cocColorTex, TexCoords);
    float radius = abs(center.a); // blur radius in half-res texels

    vec3 sum = center.rgb;
    float total = 1.0;
    for (int i = 0; i < TAPS; i++) {
        vec2 v = vogel(i);
        vec4 tap = texture(cocColorTex, TexCoords + v * radius * texelSize);
        float sampleDist = length(v) * radius; // texels from centre
        // The tap reaches this pixel only if its own blur is at least that wide
        float weight = clamp(abs(tap.a) - sampleDist + 1.0, 0.0, 1.0);
        sum += tap.rgb * weight;
        total += weight;
    }

    FragColor = vec4(sum / total, center.a);
}
