#version 330 core

// The mark the HUD shows as the game saves (spec 13.49): a three-branched candlestick in iron over
// its rect, its candles lit one after another, left to right, as uFocus goes from 0 to 3, and
// flickering once lit. The iron is the quad's colour, and the whole mark takes the quad's alpha.
in vec2 vPos;
in vec4 vColor;

out vec4 FragColor;

uniform vec4 uRect; // x, y, w, h in window points
uniform float uTime;
uniform float uFocus;

const vec3 WAX = vec3(0.80, 0.76, 0.66);
const vec3 FLAME_CORE = vec3(1.0, 0.93, 0.72);
const vec3 FLAME_EDGE = vec3(1.0, 0.52, 0.16);
const vec3 GLOW = vec3(1.0, 0.62, 0.25);

// Each candle's x and the top of its wax; the middle one stands higher.
const vec3 CANDLE_X = vec3(0.20, 0.50, 0.80);
const vec3 CANDLE_TOP = vec3(0.74, 0.80, 0.74);

float box(vec2 p, vec2 c, vec2 b, float r) {
    vec2 q = abs(p - c) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// The two arms: the lower half of a ring, with round ends where it turns up into the cups.
float arms(vec2 p, vec2 c, float radius, float t) {
    vec2 q = p - c;
    if (q.y < 0.0)
        return abs(length(q) - radius) - t;
    return min(length(q - vec2(-radius, 0.0)), length(q - vec2(radius, 0.0))) - t;
}

float iron(vec2 p) {
    float d = box(p, vec2(0.5, 0.045), vec2(0.17, 0.025), 0.025); // the foot
    d = min(d, box(p, vec2(0.5, 0.09), vec2(0.07, 0.03), 0.02));  // the rise over it
    d = min(d, box(p, vec2(0.5, 0.36), vec2(0.022, 0.27), 0.01)); // the stem
    d = min(d, length(p - vec2(0.5, 0.26)) - 0.045);              // the knop the arms spring from
    d = min(d, arms(p, vec2(0.5, 0.56), 0.30, 0.024));
    for (int i = 0; i < 3; i++) {
        float base = i == 1 ? 0.635 : 0.585;
        d = min(d, box(p, vec2(CANDLE_X[i], base), vec2(0.065, 0.014), 0.012)); // its drip pan
    }
    return d;
}

float wax(vec2 p) {
    float d = 1e3;
    for (int i = 0; i < 3; i++) {
        float y0 = i == 1 ? 0.65 : 0.60;
        float y1 = CANDLE_TOP[i];
        d = min(d, box(p, vec2(CANDLE_X[i], 0.5 * (y0 + y1)), vec2(0.028, 0.5 * (y1 - y0)), 0.006));
    }
    return d;
}

// A flame over candle `i` grown to `lit` of its size: an ellipse narrowed toward its tip, the tip
// swaying. Negative inside, in the flame's own units.
float flame(vec2 p, int i, float lit) {
    float fi = float(i);
    float h = 0.06 * lit * (1.0 + 0.08 * sin(uTime * 13.0 + fi * 1.7));
    float w = 0.03 * lit;
    vec2 c = vec2(CANDLE_X[i], CANDLE_TOP[i] + 0.01 + h);
    vec2 q = p - c;
    float up = clamp(q.y / max(h, 1e-4), 0.0, 1.0);
    q.x -= 0.012 * lit * sin(uTime * 9.0 + fi * 2.1) * up;
    q.x /= 1.0 - 0.55 * up;
    return (length(q / vec2(max(w, 1e-4), max(h, 1e-4))) - 1.0) * max(w, 1e-4);
}

float cover(float d) {
    float aa = max(fwidth(d), 1e-5);
    return 1.0 - smoothstep(-aa, aa, d);
}

// `rgb` at `a` over what is there, both straight alpha.
vec4 over(vec4 dst, vec3 rgb, float a) {
    float out_a = a + dst.a * (1.0 - a);
    if (out_a <= 0.0)
        return vec4(0.0);
    return vec4((rgb * a + dst.rgb * dst.a * (1.0 - a)) / out_a, out_a);
}

void main() {
    // The rect's own square, y up.
    vec2 p = (vPos - uRect.xy) / uRect.zw;
    p.y = 1.0 - p.y;

    vec4 c = vec4(0.0);
    for (int i = 0; i < 3; i++) {
        float lit = smoothstep(0.0, 1.0, clamp(uFocus - float(i), 0.0, 1.0));
        vec2 at = vec2(CANDLE_X[i], CANDLE_TOP[i] + 0.06);
        c = over(c, GLOW, 0.35 * lit * exp(-length(p - at) * 16.0));
    }
    c = over(c, vColor.rgb, cover(iron(p)));
    c = over(c, WAX, cover(wax(p)));
    for (int i = 0; i < 3; i++) {
        float lit = smoothstep(0.0, 1.0, clamp(uFocus - float(i), 0.0, 1.0));
        if (lit <= 0.0)
            continue;
        float d = flame(p, i, lit);
        float core = clamp(-d / (0.02 * lit), 0.0, 1.0);
        c = over(c, mix(FLAME_EDGE, FLAME_CORE, core), cover(d));
    }

    float alpha = c.a * vColor.a;
    if (alpha < 0.003)
        discard;
    FragColor = vec4(c.rgb, alpha);
}
