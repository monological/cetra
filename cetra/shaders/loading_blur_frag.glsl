#version 330 core

// The loading screen's bloom (spec 13.34): one direction of a Gaussian blur, run across and then
// down at a quarter of the picture's size. A real blur rather than the mark's coarse mips read
// back: a mip is a grid of blocks, and a glow built from blocks brightens in straight runs that
// meet in creases -- one shared by every letter whose top stands on the same row of blocks, which
// is a line across the title -- and that shifts as a turning letter crosses from block to block.

in vec2 TexCoords;
out vec4 FragColor;

uniform sampler2D srcTex;
uniform float srcLod; // the source's mip level read: the one a quarter of the picture's size
uniform vec2 stepUv;  // one texel of this pass's target along the blur, in uv

const int RADIUS = 12;
const float SIGMA = 4.0; // in texels of the target

void main()
{
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int i = -RADIUS; i <= RADIUS; i++) {
        float w = exp(-0.5 * float(i * i) / (SIGMA * SIGMA));
        sum += w * textureLod(srcTex, TexCoords + float(i) * stepUv, srcLod).rgb;
        total += w;
    }
    FragColor = vec4(sum / total, 1.0);
}
