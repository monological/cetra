#version 330 core

in vec2 TexCoord;
in vec4 Color;

out vec4 FragColor;

uniform sampler2D fontAtlas;
uniform int useSDF;
uniform float sdfEdge;
uniform float sdfSmoothing;

// Effect system
uniform int effectType;  // 0=none, 1=glow, 2=plasma
uniform float time;

// Glow effect uniforms
uniform float glowIntensity;
uniform vec3 glowColor;

// Plasma effect uniforms
uniform float plasmaSpeed;
uniform float plasmaIntensity;

// Wizard color palette for plasma
vec3 plasmaPalette(float t) {
    t = fract(t);

    vec3 c0 = vec3(0.106, 0.0, 0.212);    // #1b0036 deep purple
    vec3 c1 = vec3(0.008, 0.278, 0.161);  // #024729 dark green
    vec3 c2 = vec3(0.0, 0.243, 0.2);      // #003e33 dark teal
    vec3 c3 = vec3(0.259, 0.024, 0.259);  // #420642 dark magenta
    vec3 c4 = vec3(0.235, 0.016, 0.125);  // #3c0420 dark crimson

    if (t < 0.25) return mix(c0, c1, t * 4.0);
    if (t < 0.5)  return mix(c1, c2, (t - 0.25) * 4.0);
    if (t < 0.75) return mix(c2, c3, (t - 0.5) * 4.0);
    return mix(c3, c4, (t - 0.75) * 4.0);
}

void main() {
    float dist = texture(fontAtlas, TexCoord).r;

    if (useSDF == 1) {
        float edge = sdfEdge;
        float sw = fwidth(dist) * 0.5 + sdfSmoothing;
        float core = smoothstep(edge - sw, edge + sw, dist);

        vec3 finalColor;
        float finalAlpha;

        if (effectType == 0) {
            // TEXT_EFFECT_NONE - plain text
            finalColor = Color.rgb;
            finalAlpha = core * Color.a;

        } else if (effectType == 1) {
            // TEXT_EFFECT_GLOW - solid color with outer glow
            float glowField = smoothstep(0.0, edge + 0.1, dist);
            glowField = pow(glowField, 0.5);

            vec3 coreColor = Color.rgb;
            vec3 glowCol = glowColor * glowIntensity;

            finalColor = glowCol * glowField * (1.0 - core);
            finalColor += coreColor * core;

            float glowAlpha = glowField * 0.75 * glowIntensity;
            finalAlpha = max(core, glowAlpha) * Color.a;

        } else if (effectType == 2) {
            // TEXT_EFFECT_PLASMA - animated swirl effect
            float glowField = smoothstep(0.0, edge + 0.1, dist);
            glowField = pow(glowField, 0.5);

            float t = time * plasmaSpeed;
            vec2 uv = gl_FragCoord.xy * 0.006;

            // Plasma layers
            float p1 = sin(uv.x * 2.5 + t * 0.8)
                     + sin(uv.y * 2.8 + t * 0.6)
                     + sin((uv.x + uv.y) * 1.8 + t * 1.0)
                     + sin(length(uv - vec2(8.0, 4.5)) * 2.2 - t * 0.9);

            float p2 = sin(uv.x * 3.2 - uv.y * 1.8 + t * 0.7)
                     + sin(uv.y * 2.2 - t * 0.5)
                     + sin(length(uv) * 1.8 + t * 1.1);

            float p3 = sin(uv.x * 1.5 + uv.y * 2.5 - t * 0.6)
                     + sin((uv.x - uv.y) * 2.0 + t * 0.8);

            float plasma = (p1 + p2 * 0.7 + p3 * 0.5) * 0.10 + 0.5;

            // Multiple swirl centers
            float swirl1 = sin(atan(uv.y - 3.0, uv.x - 4.0) * 3.0 + length(uv - vec2(4.0, 3.0)) * 0.6 - t * 0.5);
            float swirl2 = sin(atan(uv.y - 5.0, uv.x - 8.0) * 2.5 + length(uv - vec2(8.0, 5.0)) * 0.5 + t * 0.4);
            float swirl3 = sin(atan(uv.y - 2.0, uv.x - 12.0) * 3.5 + length(uv - vec2(12.0, 2.0)) * 0.7 - t * 0.6);
            float swirl4 = sin(atan(uv.y - 6.0, uv.x - 2.0) * 2.0 + length(uv - vec2(2.0, 6.0)) * 0.4 + t * 0.3);

            float swirlSum = (swirl1 + swirl2 + swirl3 + swirl4) * 0.25;
            swirlSum = swirlSum * 0.5 + 0.5;
            swirlSum = smoothstep(0.2, 0.8, swirlSum);
            plasma += (swirlSum - 0.5) * 0.15;

            vec3 col = plasmaPalette(plasma + t * 0.02);
            col *= 1.8 * plasmaIntensity;

            float depth = smoothstep(edge - 0.05, edge + 0.3, dist);
            col *= 0.85 + 0.25 * depth;

            vec3 glowCol = plasmaPalette(plasma + 0.3 + t * 0.02) * 0.8 * plasmaIntensity;

            finalColor = glowCol * glowField * (1.0 - core);
            finalColor += col * core;

            float glowAlpha = glowField * 0.75 * plasmaIntensity;
            finalAlpha = max(core, glowAlpha) * Color.a;

        } else {
            // Fallback
            finalColor = Color.rgb;
            finalAlpha = core * Color.a;
        }

        if (finalAlpha < 0.01) discard;
        FragColor = vec4(finalColor, finalAlpha);

    } else {
        float alpha = dist;
        if (alpha < 0.01) discard;
        FragColor = vec4(Color.rgb, Color.a * alpha);
    }
}
