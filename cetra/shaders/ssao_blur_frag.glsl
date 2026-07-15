#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// 4x4 box blur matched to the SSAO noise tile period: exactly cancels the
// per-pixel rotation noise in one pass
uniform sampler2D aoTex;
uniform vec2 texelSize;

void main()
{
    float result = 0.0;
    for (int x = -2; x < 2; x++) {
        for (int y = -2; y < 2; y++) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            result += texture(aoTex, TexCoords + offset).r;
        }
    }
    FragColor = vec4(vec3(result / 16.0), 1.0);
}
