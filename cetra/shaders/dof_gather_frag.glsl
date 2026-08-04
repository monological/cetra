#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Depth-of-field gather at half resolution. Each pixel spreads the CPU-built
// aperture kernel sized by its own circle-of-confusion; a neighbour
// contributes only if its CoC reaches back to this pixel (scatter-as-gather),
// which keeps a sharp foreground from bleeding onto a blurred background.
// The kernel arrives pre-warped to the aperture's N-gon and pre-rotated --
// deliberately NOT rotated per pixel, which would grind the polygon shape
// into grainy noise.
uniform sampler2D cocColorTex; // Half-res scene colour (rgb) + signed CoC (a)
uniform sampler2D tileTex;     // Dilated per-tile (maxFarCoC, maxNearCoC)
uniform vec2 texelSize;        // Half-res texel size
uniform vec2 kernel[64];       // Unit-radius aperture points (disk or N-gon)
uniform int tileSize;          // DOF_TILE (must match postfx.c)

const int TAPS = 64;

void main()
{
    vec4 center = texture(cocColorTex, TexCoords);

    // Kernel radius from the DILATED tile, not this pixel's own CoC: a sharp
    // pixel beside a defocused neighbour must still reach out far enough to
    // collect that neighbour's spill -- its own CoC says zero exactly where
    // the spill matters most. texelFetch, never bilinear: interpolating two
    // maxima can dip below either plateau at a seam and cut a needed reach.
    vec2 tile = texelFetch(tileTex, ivec2(gl_FragCoord.xy) / tileSize, 0).rg;
    float radius = max(tile.r, tile.g);

    // Whole tile in focus: nothing here or nearby spreads anywhere.
    if (radius < 0.5) {
        FragColor = vec4(center.rgb, center.a);
        return;
    }

    vec3 sum = center.rgb;
    float total = 1.0;
    for (int i = 0; i < TAPS; i++) {
        vec2 v = kernel[i];
        vec4 tap = texture(cocColorTex, TexCoords + v * radius * texelSize);
        float sampleDist = length(v) * radius; // texels from centre
        // The tap reaches this pixel only if its own blur is at least that wide
        float weight = clamp(abs(tap.a) - sampleDist + 1.0, 0.0, 1.0);
        sum += tap.rgb * weight;
        total += weight;
    }

    FragColor = vec4(sum / total, center.a);
}
