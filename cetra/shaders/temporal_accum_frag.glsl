#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Shared plain-RGBA temporal accumulator, driven by run_temporal_accum for
// every effect whose per-frame jitter should integrate over time without
// signal-specific tricks: AO accumulates its occlusion in .r (the R16F
// history target discards the rest), the composited volumetric-fog layer its
// (inscatter.rgb, transmittance.a). Reprojects last frame's accumulation by the motion
// vectors, bounds it per channel to the current 3x3 neighborhood so a
// surface that just disoccluded can't bleed stale history through, and
// blends at fixed feedback. SSGI keeps its own accumulator: its YCoCg
// clamp and inverse-luma firefly blend are real signal differences.
uniform sampler2D currentTex;  // This frame's raw effect output
uniform sampler2D velocityTex; // Screen-space motion .xy (UV units)
uniform sampler2D historyTex;  // Last frame's accumulation
uniform vec2 texelSize;        // 1 / effect resolution
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
