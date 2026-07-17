#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Edge-aware a-trous (holey wavelet) blur for SSGI. Each iteration convolves a
// 3x3 B3-spline kernel whose taps are spaced stepSize texels apart; successive
// iterations double the spacing (1, 2, 4), so three cheap passes cover a wide
// footprint without ever touching more than 9 taps. Depth and normal weights
// stop the blur at geometric edges: bounce light smooths across a wall but
// does not bleed around a corner or across a silhouette.
uniform sampler2D giTex;       // GI to be smoothed (.rgb)
uniform sampler2D linDepthTex; // Aux G-buffer; .z = linear view-space Z (<0), 0 = sky
uniform sampler2D normalsTex;  // View-space normals (xyz)
uniform int useNormalsTex;     // 0 = normals buffer absent; weight by depth only
uniform vec2 texelSize;        // 1 / GI resolution
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

    vec3 sum = vec3(0.0);
    float wSum = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 uv = TexCoords + vec2(float(x), float(y)) * texelSize * float(stepSize);
            float w = KERNEL[x + 1] * KERNEL[y + 1];

            float z = texture(linDepthTex, uv).z;
            w *= exp(-abs(z - centerZ) / zTol);

            if (haveN) {
                vec3 n = texture(normalsTex, uv).xyz;
                // Zero-normal marker pixels (hair) contribute by depth only
                if (dot(n, n) > 0.01)
                    w *= pow(max(dot(centerN, normalize(n)), 0.0), 32.0);
            }

            sum += texture(giTex, uv).rgb * w;
            wSum += w;
        }
    }

    FragColor = vec4(sum / max(wSum, 1e-6), 1.0);
}
