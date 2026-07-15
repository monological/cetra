#version 330 core
in vec3 WorldPos;
out vec4 FragColor;

uniform samplerCube environmentMap;
// Mip to integrate from: the ~1.5k hemisphere samples below would
// statistically miss small ultra-bright sources (studio lamps, the sun) at
// the full-res faces; a pre-averaged mip folds their energy into every texel
uniform float sampleMipLevel;

const float PI = 3.14159265359;

void main()
{
    vec3 N = normalize(WorldPos);

    vec3 irradiance = vec3(0.0);

    vec3 up    = vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up         = normalize(cross(N, right));

    float sampleDelta = 0.025;
    float nrSamples = 0.0;

    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {
            vec3 tangentSample = vec3(sin(theta) * cos(phi),
                                      sin(theta) * sin(phi),
                                      cos(theta));

            vec3 sampleVec = tangentSample.x * right +
                             tangentSample.y * up +
                             tangentSample.z * N;

            irradiance += textureLod(environmentMap, sampleVec, sampleMipLevel).rgb *
                          cos(theta) * sin(theta);
            nrSamples++;
        }
    }
    irradiance = PI * irradiance * (1.0 / nrSamples);

    FragColor = vec4(irradiance, 1.0);
}
