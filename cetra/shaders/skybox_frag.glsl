#version 330 core
in vec3 TexCoords;
out vec4 FragColor;

uniform samplerCube skyboxTex;
uniform float exposure;

void main()
{
    vec3 envColor = texture(skyboxTex, TexCoords).rgb;

    // Apply exposure
    envColor *= exposure;

    // Reinhard tone mapping
    envColor = envColor / (envColor + vec3(1.0));

    // Gamma correction
    envColor = pow(envColor, vec3(1.0 / 2.2));

    FragColor = vec4(envColor, 1.0);
}
