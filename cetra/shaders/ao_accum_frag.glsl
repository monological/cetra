#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Temporal accumulation for GTAO -- a single-channel mini-TAA on the AO buffer.
// GTAO jitters its slice directions per frame; this pass reprojects last frame's
// accumulated AO by the motion vectors and blends it with the current frame, so
// those jittered samples integrate over time into a stable, smooth result
// (temporal supersampling of the occlusion). A 3x3 neighborhood clamp on the
// reprojected history stops it ghosting when geometry disoccludes.
uniform sampler2D currentTex;  // This frame's blurred AO (.r, 1 = unoccluded)
uniform sampler2D velocityTex; // Screen-space motion .xy (UV units)
uniform sampler2D historyTex;  // Last frame's accumulated AO (.r)
uniform vec2 texelSize;        // 1 / AO resolution
uniform int reset;             // 1 on the first frame -> no history yet

const float FEEDBACK = 0.9; // History weight; ~10-frame effective window

void main()
{
    float current = texture(currentTex, TexCoords).r;

    if (reset != 0) {
        FragColor = vec4(vec3(current), 1.0);
        return;
    }

    vec2 velocity = texture(velocityTex, TexCoords).xy;
    vec2 histUv = TexCoords - velocity;
    if (histUv.x < 0.0 || histUv.x > 1.0 || histUv.y < 0.0 || histUv.y > 1.0) {
        // Reprojected off-screen (disocclusion at the frame edge): current only.
        FragColor = vec4(vec3(current), 1.0);
        return;
    }

    // Bound the history to the current 3x3 AO neighborhood so history from a
    // surface that just disoccluded can't bleed a stale value through.
    float lo = current;
    float hi = current;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            if (x == 0 && y == 0)
                continue; // center is already seeded into lo/hi
            float s = texture(currentTex, TexCoords + vec2(x, y) * texelSize).r;
            lo = min(lo, s);
            hi = max(hi, s);
        }
    }

    float history = clamp(texture(historyTex, histUv).r, lo, hi);
    FragColor = vec4(vec3(mix(current, history, FEEDBACK)), 1.0);
}
