#version 330 core

// The loading screen's mark (spec 13.34): CETRA over ENGINE as a 1970s station ident. The letters
// are drawn, not set: each is a skeleton of strokes and arcs, and a stroke is a tube round it,
// coloured in bands of the distance to the skeleton -- concentric stripes, cream at the core out
// to brown at the edge, which join cleanly wherever strokes meet because the distance does. Each
// CETRA letter flips in about its own upright axis in perspective; ENGINE's letters then light one
// by one, and a striped rule draws out beneath them with a light sweeping along it while the game
// loads. Linear light out; the tape pass wears it and encodes it.

in vec2 TexCoords;
out vec4 FragColor;

uniform float time;      // seconds since the screen was shown
uniform vec2 resolution; // the picture's pixels
uniform vec3 palette[7]; // linear: the ground, five stripes from the core out, the block shadow

const float PI = 3.14159265;

// The title, in letter units (cap height 1). The view is at least VIEW_H tall and wide enough to
// hold the title at TITLE_FILL of the picture's width.
const float VIEW_H = 4.6;
const float TITLE_FILL = 0.8;
const float TITLE_R = 0.16;  // a stroke's radius
const float TITLE_GAP = 0.55; // between one letter's skeleton and the next
const float TITLE_Y = 0.05;   // the baseline
const vec2 SHADOW_OFF = vec2(0.10, -0.12); // the block shadow's depth, down and to the right
const float EYE_D = 7.0;      // the eye's distance from the plane the mark stands in

// The flip: each letter starts edge on, three quarters of a turn away, and springs to face the
// eye with one small overshoot.
const float TITLE_START = 0.55;
const float TITLE_STAGGER = 0.18;
const float SPRING_K = 6.0;
const float SPRING_W = 6.0;

// ENGINE, lit a letter at a time beneath the title and spaced to its width.
const float ENGINE_START = 2.1;
const float ENGINE_STAGGER = 0.08;
const float ENGINE_SCALE = 0.34;
const float ENGINE_R = 0.08;   // in its own letter units
const float ENGINE_Y = -0.75;
const float ENGINE_FLASH = 2.5; // how far past its level a letter flares as it lights
const float ENGINE_DECAY = 9.0;

// The rule: four stripes, drawn out from the middle, then a light sweeping along them.
const float RULE_START = 2.6;
const float RULE_GROW = 0.4;
const float RULE_TOP = -0.98;
const float RULE_LINE = 0.04;
const float RULE_SPACE = 0.014;
const float SWEEP_PERIOD = 2.4;

// The phosphor's glow round a lit stroke, falling to exactly nothing GLOW_REACH past its edge.
const float GLOW_W = 0.1;
const float GLOW_A = 0.2;
const float GLOW_REACH = 0.4;

const int L_C = 0, L_E = 1, L_T = 2, L_R = 3, L_A = 4, L_N = 5, L_G = 6, L_I = 7;
const float WIDTH[8] = float[8](0.854, 0.7, 0.8, 0.78, 1.0, 0.85, 1.0, 0.0);
const int TITLE[5] = int[5](L_C, L_E, L_T, L_R, L_A);
const int ENGINE[6] = int[6](L_E, L_N, L_G, L_I, L_N, L_E);

float segDist(vec2 p, vec2 a, vec2 b)
{
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h);
}

// The arc of radius `r` about `c` from angle a0 round to a1, counter-clockwise, radians.
float arcDist(vec2 p, vec2 c, float r, float a0, float a1)
{
    vec2 q = p - c;
    float mid = 0.5 * (a0 + a1);
    float off = mod(atan(q.y, q.x) - mid + PI, 2.0 * PI) - PI;
    if (abs(off) <= 0.5 * (a1 - a0))
        return abs(length(q) - r);
    vec2 e0 = c + r * vec2(cos(a0), sin(a0));
    vec2 e1 = c + r * vec2(cos(a1), sin(a1));
    return min(length(p - e0), length(p - e1));
}

// The distance from `p` to letter `id`'s skeleton, in letter units, its box [0, WIDTH] by [0, 1].
float glyph(int id, vec2 p)
{
    const vec2 O = vec2(0.5);
    const float Q = 0.25 * PI;
    if (id == L_C)
        return arcDist(p, O, 0.5, Q, 2.0 * PI - Q);
    if (id == L_E) {
        // Square on the right, its left corners turned on arcs as the C beside it is.
        const float K = 0.3;
        float d = segDist(p, vec2(0.0, 0.5), vec2(0.58, 0.5));
        d = min(d, segDist(p, vec2(0.0, K), vec2(0.0, 1.0 - K)));
        d = min(d, arcDist(p, vec2(K, 1.0 - K), K, 0.5 * PI, PI));
        d = min(d, arcDist(p, vec2(K, K), K, PI, 1.5 * PI));
        d = min(d, segDist(p, vec2(K, 1.0), vec2(0.7, 1.0)));
        return min(d, segDist(p, vec2(K, 0.0), vec2(0.7, 0.0)));
    }
    if (id == L_T)
        return min(segDist(p, vec2(0.0, 1.0), vec2(0.8, 1.0)), segDist(p, vec2(0.4, 1.0), vec2(0.4, 0.0)));
    if (id == L_R) {
        float d = segDist(p, vec2(0.0, 0.0), vec2(0.0, 1.0));
        d = min(d, segDist(p, vec2(0.0, 1.0), vec2(0.4, 1.0)));
        d = min(d, arcDist(p, vec2(0.4, 0.725), 0.275, -0.5 * PI, 0.5 * PI));
        d = min(d, segDist(p, vec2(0.0, 0.45), vec2(0.4, 0.45)));
        return min(d, segDist(p, vec2(0.4, 0.45), vec2(0.78, 0.0)));
    }
    if (id == L_A) {
        float d = min(segDist(p, vec2(0.0, 0.0), vec2(0.5, 1.0)), segDist(p, vec2(0.5, 1.0), vec2(1.0, 0.0)));
        return min(d, segDist(p, vec2(0.15, 0.3), vec2(0.85, 0.3)));
    }
    if (id == L_N) {
        float d = min(segDist(p, vec2(0.0, 0.0), vec2(0.0, 1.0)), segDist(p, vec2(0.0, 1.0), vec2(0.85, 0.0)));
        return min(d, segDist(p, vec2(0.85, 0.0), vec2(0.85, 1.0)));
    }
    if (id == L_G)
        return min(arcDist(p, O, 0.5, Q, 2.0 * PI), segDist(p, vec2(0.55, 0.5), vec2(1.0, 0.5)));
    return segDist(p, vec2(0.0, 0.0), vec2(0.0, 1.0));
}

// The stripes across a stroke of radius `r` at distance `d` from its skeleton, and its coverage
// in .a, `aa` the width of a pixel in the same units.
vec4 stripes(float d, float r, float aa)
{
    const float EDGE[4] = float[4](0.30, 0.55, 0.80, 0.93);
    vec3 c = palette[1];
    for (int k = 0; k < 4; k++)
        c = mix(c, palette[k + 2], smoothstep(EDGE[k] * r - aa, EDGE[k] * r + aa, d));
    return vec4(c, 1.0 - smoothstep(r - aa, r + aa, d));
}

float titleWidth()
{
    float w = 4.0 * TITLE_GAP + 2.0 * TITLE_R;
    for (int i = 0; i < 5; i++)
        w += WIDTH[TITLE[i]];
    return w;
}

// The glow at `x` past a stroke's edge, in units of its width: an exponential lowered by its own
// value at the reach, so it ends at zero there rather than at a step.
float glowAt(float x, float width, float reach)
{
    float floorAt = exp(-reach / width);
    return max(exp(-max(x, 0.0) / width) - floorAt, 0.0) / (1.0 - floorAt);
}

// Back to front over `dst`: premultiplied `src`.
vec3 over(vec3 dst, vec4 src)
{
    return dst * (1.0 - src.a) + src.rgb;
}

void main()
{
    float wide = titleWidth();
    float aspect = resolution.x / resolution.y;
    float viewH = max(VIEW_H, wide / (aspect * TITLE_FILL));
    float px = viewH / resolution.y; // a picture pixel on the mark's plane
    vec2 s = (TexCoords - 0.5) * vec2(aspect, 1.0) * viewH;

    // The ground: the palette's darkest, a little lifted toward the middle, as a lit backdrop.
    float lift = 1.0 - smoothstep(0.0, 1.0, length(s / vec2(aspect * viewH * 0.5, viewH * 0.6)));
    vec3 colour = palette[0] * (0.6 + 0.8 * lift);
    vec3 glow = vec3(0.0);

    // ENGINE and its rule, flat on the plane.
    float left = -0.5 * wide + TITLE_R;
    float right = 0.5 * wide - TITLE_R;
    float engineInk = 0.0;
    for (int j = 0; j < 6; j++)
        engineInk += WIDTH[ENGINE[j]];
    float engineGap = ((right - left) / ENGINE_SCALE - engineInk) / 5.0;
    float x = left;
    for (int j = 0; j < 6; j++) {
        int id = ENGINE[j];
        float w = WIDTH[id];
        float since = time - (ENGINE_START + float(j) * ENGINE_STAGGER);
        if (since > 0.0) {
            vec2 p = (s - vec2(x, ENGINE_Y)) / ENGINE_SCALE;
            float d = glyph(id, p);
            float flare = 1.0 + ENGINE_FLASH * exp(-since * ENGINE_DECAY);
            float cover = 1.0 - smoothstep(ENGINE_R - px / ENGINE_SCALE, ENGINE_R + px / ENGINE_SCALE, d);
            colour = mix(colour, palette[1] * flare, cover);
            glow += palette[2] * GLOW_A * flare * glowAt((d - ENGINE_R) * ENGINE_SCALE, 0.5 * GLOW_W, 0.5 * GLOW_REACH);
        }
        x += (w + engineGap) * ENGINE_SCALE;
    }
    float grow = smoothstep(0.0, 1.0, (time - RULE_START) / RULE_GROW);
    if (grow > 0.0) {
        float reach = 0.5 * (right - left) * grow;
        float sweep = time > RULE_START + RULE_GROW
                          ? sin((time - RULE_START - RULE_GROW) * 2.0 * PI / SWEEP_PERIOD) * 0.5 * (right - left)
                          : 0.0;
        float shine = time > RULE_START + RULE_GROW ? exp(-pow((s.x - sweep) / 0.35, 2.0)) : 0.0;
        for (int k = 0; k < 4; k++) {
            float top = RULE_TOP - float(k) * (RULE_LINE + RULE_SPACE);
            float inside = (1.0 - smoothstep(reach - px, reach + px, abs(s.x))) *
                           smoothstep(top - RULE_LINE - px, top - RULE_LINE + px, s.y) *
                           (1.0 - smoothstep(top - px, top + px, s.y));
            colour = mix(colour, palette[k + 2] * (1.0 + 1.6 * shine), inside);
        }
        // The light's own glow, hugging the rule.
        float band = RULE_TOP - 1.5 * (RULE_LINE + RULE_SPACE) - 0.5 * RULE_LINE;
        glow += palette[2] * shine * 0.5 * exp(-pow((s.y - band) / 0.12, 2.0)) *
                (1.0 - smoothstep(reach, reach + 0.1, abs(s.x)));
    }

    // CETRA: each letter's plane turned about its upright axis, met by this pixel's ray from the
    // eye. Gathered, then drawn back to front.
    vec3 eye = vec3(0.0, 0.0, EYE_D);
    vec3 ray = normalize(vec3(s, 0.0) - eye);
    float far[5];
    vec4 ink[5];
    x = -0.5 * wide + TITLE_R;
    for (int i = 0; i < 5; i++) {
        int id = TITLE[i];
        float w = WIDTH[id];
        float cx = x + 0.5 * w;
        x += w + TITLE_GAP;
        far[i] = -1.0;
        ink[i] = vec4(0.0);

        float since = time - (TITLE_START + float(i) * TITLE_STAGGER);
        float turn = since <= 0.0 ? 1.5 * PI : 1.5 * PI * exp(-SPRING_K * since) * cos(SPRING_W * since);
        vec3 n = vec3(sin(turn), 0.0, cos(turn));
        float facing = dot(ray, n);
        if (abs(facing) < 1e-3)
            continue;
        vec3 centre = vec3(cx, TITLE_Y, 0.0);
        float t = dot(centre - eye, n) / facing;
        if (t <= 0.0)
            continue;
        vec3 hit = eye + t * ray;
        vec2 p = vec2(dot(hit - centre, vec3(cos(turn), 0.0, -sin(turn))) + 0.5 * w, hit.y - TITLE_Y);
        float margin = TITLE_R + GLOW_REACH;
        if (p.x < -margin || p.x > w + margin + SHADOW_OFF.x || p.y < -margin + SHADOW_OFF.y || p.y > 1.0 + margin)
            continue;

        // A pixel, on this plane: the eye's footprint grows with distance and stretches as the
        // plane turns from it.
        float aa = px * t / length(vec3(s, -EYE_D)) / max(abs(facing), 0.15);
        float d = glyph(id, p);
        far[i] = t;
        if (facing < 0.0) {
            vec4 face = stripes(d, TITLE_R, aa);
            // Turning, it catches the light once on its way round.
            float glint = exp(-pow((turn - 0.45) / 0.12, 2.0));
            face.rgb *= 0.55 + 0.45 * abs(cos(turn)) + 0.9 * glint;
            float shadow = 0.0;
            for (int k = 1; k <= 6; k++) {
                float ds = glyph(id, p - SHADOW_OFF * (float(k) / 6.0));
                shadow = max(shadow, 1.0 - smoothstep(TITLE_R - aa, TITLE_R + aa, ds));
            }
            float under = shadow * (1.0 - face.a);
            ink[i] = vec4(face.rgb * face.a + palette[6] * under, face.a + under);
            glow += palette[3] * GLOW_A * abs(cos(turn)) * glowAt(d - TITLE_R, GLOW_W, GLOW_REACH);
        } else {
            // The card's back: dark, with the outer stripe round its edge.
            vec4 face = stripes(d, TITLE_R, aa);
            vec3 back = mix(palette[6], palette[5], smoothstep(0.7 * TITLE_R, 0.85 * TITLE_R, d));
            ink[i] = vec4(back * face.a, face.a);
        }
    }
    // Back to front: the largest distance first.
    for (int pass = 0; pass < 5; pass++) {
        int pick = -1;
        float best = -1.0;
        for (int i = 0; i < 5; i++)
            if (far[i] > best) {
                best = far[i];
                pick = i;
            }
        if (pick < 0)
            break;
        colour = over(colour, ink[pick]);
        far[pick] = -1.0;
    }

    FragColor = vec4(colour + glow, 1.0);
}
