#version 330 core

// A sum along one row of a texture (spec 13.14): what turns a GRID fire's row sums into slice
// sums, and those into the texels the CPU reads back. The fragment at x sums `count` texels of
// row x - outBase, left to right, so its order is fixed and the answer the same every run.

uniform sampler2D sumTex;
uniform int count;
uniform int outBase;

out vec4 outSum;

void main() {
    int row = int(gl_FragCoord.x) - outBase;
    vec4 sum = vec4(0.0);
    for (int k = 0; k < count; k++)
        sum += texelFetch(sumTex, ivec2(k, row), 0);
    outSum = sum;
}
