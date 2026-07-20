#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Edge-aware a-trous (holey wavelet) blur for the SSR reflection buffer. The
// stochastic march scatters the Hi-Z traversal's fixed grid stripes into
// per-pixel noise; this pass resolves that noise into a clean reflection in a
// single frame (no temporal convergence wait). Same B3-spline kernel as the
// SSGI denoiser -- three passes at tap spacing 1, 2, 4 cover a wide footprint
// from nine taps -- but it smooths the full PREMULTIPLIED (color*weight,
// weight) vec4: filtering premultiplied RGBA is the correct way to blur a
// buffer whose alpha is coverage, and smoothing the weight is what softens the
// reflection's hard hit/miss coverage edge (the serration). Depth and normal
// weights come from the REFLECTING surface's G-buffer, so the blur stays on the
// floor and stops at its silhouette instead of bleeding reflection off the edge.
uniform sampler2D reflTex;     // Premultiplied reflection (color*weight, weight)
uniform sampler2D linDepthTex; // Aux G-buffer; .z = linear view-space Z (<0), 0 = sky
uniform sampler2D normalsTex;  // View-space normals (xyz) + reflective marker (a)
uniform int useNormalsTex;     // 0 = normals buffer absent; weight by depth only
uniform vec2 texelSize;        // 1 / reflection resolution
uniform int stepSize;          // Tap spacing in texels (1, 2, 4, ...)

// 1D B3-spline [1/4, 1/2, 1/4]; the 2D kernel is the outer product.
const float KERNEL[3] = float[3](0.25, 0.5, 0.25);

void main()
{
    float centerZ = texture(linDepthTex, TexCoords).z;
    vec3 centerN = texture(normalsTex, TexCoords).xyz;
    bool haveN = useNormalsTex == 1 && dot(centerN, centerN) > 0.01;
    if (haveN)
        centerN = normalize(centerN);

    // Depth tolerance scales with the depth itself so the edge test is
    // scene-scale invariant (2% of view distance).
    float zTol = 0.02 * abs(centerZ) + 1e-4;

    vec4 sum = vec4(0.0);
    float wSum = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 uv = TexCoords + vec2(float(x), float(y)) * texelSize * float(stepSize);
            float w = KERNEL[x + 1] * KERNEL[y + 1];

            float z = texture(linDepthTex, uv).z;
            w *= exp(-abs(z - centerZ) / zTol);

            if (haveN) {
                vec3 n = texture(normalsTex, uv).xyz;
                // Off-floor marker pixels contribute by depth only
                if (dot(n, n) > 0.01)
                    w *= pow(max(dot(centerN, normalize(n)), 0.0), 32.0);
            }

            // Premultiplied filter: blur (color*weight) and weight together.
            sum += texture(reflTex, uv) * w;
            wSum += w;
        }
    }

    FragColor = sum / max(wSum, 1e-6);
}
