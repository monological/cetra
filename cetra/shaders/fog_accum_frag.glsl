#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Temporal accumulation for volumetric fog -- the ssgi_accum idiom on the
// half-res (inscatter.rgb, transmittance.a) march output. Under TAA the
// march's dither rotates per frame (golden-ratio IGN offset); this pass
// reprojects last frame's accumulation by the motion vectors and blends,
// integrating the rotating jitter into a stable volume. Unlike the GI
// accumulator this stays in plain RGBA: the fog signal is low-frequency
// (no chroma-vs-luma disocclusion tension worth a YCoCg trip, no gathered
// fireflies worth an inverse-luma blend), and the transmittance in .a must
// ride through the same neighborhood clamp as the inscatter it belongs to.
uniform sampler2D currentTex;  // This frame's raw march (.rgb inscatter, .a T)
uniform sampler2D velocityTex; // Screen-space motion .xy (UV units)
uniform sampler2D historyTex;  // Last frame's accumulated fog
uniform vec2 texelSize;        // 1 / fog resolution
uniform int reset;             // 1 on the first frame -> no history yet

const float FEEDBACK = 0.9; // History weight; ~10-frame effective window

void main()
{
    vec4 current = texture(currentTex, TexCoords);

    if (reset != 0) {
        FragColor = current;
        return;
    }

    vec2 velocity = texture(velocityTex, TexCoords).xy;
    vec2 histUv = TexCoords - velocity;
    if (histUv.x < 0.0 || histUv.x > 1.0 || histUv.y < 0.0 || histUv.y > 1.0) {
        // Reprojected off-screen (disocclusion at the frame edge): current only.
        FragColor = current;
        return;
    }

    // Bound the history to the current 3x3 neighborhood so a surface that
    // just disoccluded can't bleed a stale shaft (or stale transmittance)
    // through the edge.
    vec4 nmin = current;
    vec4 nmax = current;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            if (x == 0 && y == 0)
                continue;
            vec4 n = texture(currentTex, TexCoords + vec2(float(x), float(y)) * texelSize);
            nmin = min(nmin, n);
            nmax = max(nmax, n);
        }
    }
    vec4 history = clamp(texture(historyTex, histUv), nmin, nmax);

    FragColor = mix(current, history, FEEDBACK);
}
