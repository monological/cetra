#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// One bloom-pyramid downsample step (Jimenez, CoD:AW 2014): 13 bilinear
// taps forming five overlapping 2x2 boxes, center-weighted 0.5/0.125. The
// overlap is the point -- a plain 2x2 box chain aliases sub-pixel bright
// features into per-frame pulsing as they cross texel boundaries; the
// overlapping boxes keep every source texel inside several taps so energy
// moves smoothly between destination pixels.
uniform sampler2D srcTex;
uniform vec2 texelSize; // One SOURCE-level texel

void main()
{
    vec3 a = texture(srcTex, TexCoords + texelSize * vec2(-2.0, 2.0)).rgb;
    vec3 b = texture(srcTex, TexCoords + texelSize * vec2(0.0, 2.0)).rgb;
    vec3 c = texture(srcTex, TexCoords + texelSize * vec2(2.0, 2.0)).rgb;
    vec3 d = texture(srcTex, TexCoords + texelSize * vec2(-2.0, 0.0)).rgb;
    vec3 e = texture(srcTex, TexCoords).rgb;
    vec3 f = texture(srcTex, TexCoords + texelSize * vec2(2.0, 0.0)).rgb;
    vec3 g = texture(srcTex, TexCoords + texelSize * vec2(-2.0, -2.0)).rgb;
    vec3 h = texture(srcTex, TexCoords + texelSize * vec2(0.0, -2.0)).rgb;
    vec3 i = texture(srcTex, TexCoords + texelSize * vec2(2.0, -2.0)).rgb;
    vec3 j = texture(srcTex, TexCoords + texelSize * vec2(-1.0, 1.0)).rgb;
    vec3 k = texture(srcTex, TexCoords + texelSize * vec2(1.0, 1.0)).rgb;
    vec3 l = texture(srcTex, TexCoords + texelSize * vec2(-1.0, -1.0)).rgb;
    vec3 m = texture(srcTex, TexCoords + texelSize * vec2(1.0, -1.0)).rgb;

    // Center box 0.5, each overlapping outer box 0.125 (weights sum to 1)
    vec3 color = e * 0.125;
    color += (a + c + g + i) * 0.03125;
    color += (b + d + f + h) * 0.0625;
    color += (j + k + l + m) * 0.125;
    FragColor = vec4(color, 1.0);
}
