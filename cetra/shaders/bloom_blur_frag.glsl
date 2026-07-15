#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// One direction of a separable 9-tap Gaussian; direction is pre-scaled to
// texel units by the caller ((1/w, 0) horizontal, (0, 1/h) vertical)
uniform sampler2D image;
uniform vec2 direction;

void main()
{
    const float weights[5] =
        float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

    vec3 result = texture(image, TexCoords).rgb * weights[0];
    for (int i = 1; i < 5; i++) {
        vec2 offset = direction * float(i);
        result += texture(image, TexCoords + offset).rgb * weights[i];
        result += texture(image, TexCoords - offset).rgb * weights[i];
    }

    FragColor = vec4(result, 1.0);
}
