#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Depth-of-field, pass 3 of 3: composite at full resolution. Recompute the
// circle-of-confusion from depth (cheap, sharp) and lerp the sharp scene
// toward the upsampled half-res blur by how defocused the pixel is. In-focus
// pixels stay bit-for-bit sharp; defocused ones cross-fade to the blur.
uniform sampler2D sceneTex; // Full-res sharp HDR scene
uniform sampler2D blurTex;  // Half-res blurred scene (linear upsample)
uniform sampler2D depthTex; // Full-res resolved depth
uniform mat4 projection;
uniform float focusDistance;
uniform float focusRange;
uniform float maxCoC;

float viewZFromNdcZ(float ndcZ)
{
    return -projection[3][2] / (projection[2][2] + ndcZ);
}

void main()
{
    vec3 sharp = texture(sceneTex, TexCoords).rgb;

    float depth = texture(depthTex, TexCoords).r;
    float coc;
    if (depth >= 1.0) {
        coc = maxCoC;
    } else {
        float dist = -viewZFromNdcZ(depth * 2.0 - 1.0);
        coc = clamp((dist - focusDistance) / focusRange, -1.0, 1.0) * maxCoC;
    }

    float blend = clamp(abs(coc) / max(maxCoC, 1e-4), 0.0, 1.0);
    if (blend <= 0.0) {
        FragColor = vec4(sharp, 1.0); // in focus: untouched
        return;
    }
    vec3 blurred = texture(blurTex, TexCoords).rgb;
    FragColor = vec4(mix(sharp, blurred, blend), 1.0);
}
