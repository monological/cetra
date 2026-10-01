// Rings from rain landing on standing water (spec 13.9): puddles in pbr_frag, the sea and the
// lakes in water_frag. Procedural, so it costs no sampler and nothing tiles.
//
// The plane is cut into cells `size` metres across, a few layers of them offset from each
// other so no grid shows. Every live cell drops a ring on its own cycle -- a random place in
// the cell, a random moment in the cycle -- and the ring spreads, fading, until the next.
// How many cells are live is the rain's rate, so a drizzle rings here and there and a
// downpour rings everywhere.
//
// Requires `time`, the engine's clock in seconds, declared first.

#include "pcg4d.glsl"

uniform float rainRippleActivity; // 0..1, the fraction of ripple cells live; 0 = no rain
uniform float rainRippleSize;     // metres across a ripple cell
uniform float rainRippleStrength; // scale on the rings' tilt

// A ring's life, in seconds, and how far it spreads in that time, in cells: capillary rings
// run at a few tenths of a metre a second, so 0.45 of a 0.35 m cell in 0.6 s is 0.26 m/s.
const float RAIN_RIPPLE_LIFE = 0.6;
const float RAIN_RIPPLE_REACH = 0.45;
// The ring's own shape in cells: its crest spacing and how wide the wave packet runs.
const float RAIN_RIPPLE_WAVELENGTH = 0.12;
const float RAIN_RIPPLE_WIDTH = 0.06;
// The steepest a ring tilts the surface at strength 1.
const float RAIN_RIPPLE_SLOPE = 0.35;
const int RAIN_RIPPLE_LAYERS = 2;

// A cell and two seeds, to [0,1)^4.
vec4 rainCellHash(vec2 cell, uint a, uint b) {
    return vec4(pcg4d(uvec4(uvec2(ivec2(cell)), a, b))) / 4294967296.0;
}

// A ripple cell and its layer.
vec4 rainRippleHash(vec2 cell, int layer) {
    return rainCellHash(cell, uint(layer), 0x2545f491u);
}

// The slope (dh/dx, dh/dz) the rings put on a horizontal surface at world `xz` now.
// `footprint` is the metres a pixel spans, which fades the rings out where they would alias
// into noise.
vec2 rainRippleSlope(vec2 xz, float footprint) {
    float resolve = 1.0 - smoothstep(0.5, 1.5, footprint / (RAIN_RIPPLE_WIDTH * rainRippleSize));
    if (rainRippleActivity <= 0.0 || resolve <= 0.0)
        return vec2(0.0);
    const float K = 6.2831853 / RAIN_RIPPLE_WAVELENGTH;
    const float INV_W2 = 1.0 / (RAIN_RIPPLE_WIDTH * RAIN_RIPPLE_WIDTH);
    vec2 slope = vec2(0.0);
    for (int layer = 0; layer < RAIN_RIPPLE_LAYERS; layer++) {
        vec2 p = xz / rainRippleSize + vec2(0.37, 0.61) * float(layer);
        vec2 base = floor(p);
        for (int j = -1; j <= 1; j++) {
            for (int i = -1; i <= 1; i++) {
                vec2 cell = base + vec2(i, j);
                vec4 h = rainRippleHash(cell, layer);
                if (h.w >= rainRippleActivity)
                    continue;
                float phase = fract(time / RAIN_RIPPLE_LIFE + h.z);
                vec2 d = p - (cell + 0.2 + 0.6 * h.xy);
                float dist = length(d);
                float x = dist - phase * RAIN_RIPPLE_REACH;
                // A wave packet riding the ring: sin(K x) under a Gaussian, dying as it
                // spreads. Its radial derivative, which is what tilts the normal.
                float fade = (1.0 - phase) * (1.0 - phase);
                float env = exp(-x * x * INV_W2) * fade;
                float dh = (K * cos(K * x) - 2.0 * x * INV_W2 * sin(K * x)) * env;
                slope += dh * d / max(dist, 1e-4);
            }
        }
    }
    // Normalised so that strength 1 tilts the steepest crest by RAIN_RIPPLE_SLOPE.
    return slope * (RAIN_RIPPLE_SLOPE / K) * resolve;
}

// `N` with the rings' slope `ring` tilted into it, at `weight` of their strength: how much of
// the surface is water the rain reaches.
vec3 rainRippleTilt(vec3 N, vec2 ring, float weight) {
    return normalize(N + vec3(-ring.x, 0.0, -ring.y) * (rainRippleStrength * weight));
}
