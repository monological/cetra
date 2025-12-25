#version 330 core

in vec2 TexCoord;
in vec4 Color;

out vec4 FragColor;

uniform sampler2D fontAtlas;
uniform int useSDF;
uniform float sdfEdge;
uniform float sdfSmoothing;

void main() {
    float alpha;

    if (useSDF == 1) {
        float distance = texture(fontAtlas, TexCoord).r;
        float smoothWidth = fwidth(distance) * 0.5 + sdfSmoothing;
        alpha = smoothstep(sdfEdge - smoothWidth, sdfEdge + smoothWidth, distance);
    } else {
        alpha = texture(fontAtlas, TexCoord).r;
    }

    if (alpha < 0.01) {
        discard;
    }

    FragColor = vec4(Color.rgb, Color.a * alpha);
}
