#version 330 core

in vec2 vUV;
in vec4 vColor;
in vec2 vPos;
in vec4 vRect;
in vec4 vParams; // x corner radius, y border width, z mode, w unused
in vec4 vBorder;

out vec4 FragColor;

// Mode 2 binds the font atlas here, mode 1 an arbitrary image. Mode 0 never
// samples, but the declaration is unconditional: a sampler's cost is its
// DECLARATION, and this program has one of sixteen rather than a ledger to
// defend.
uniform sampler2D uTex;

// The contract a custom element program is written against, declared here so
// the default and a replacement take the same uniforms and an element can swap
// between them with no C change. uRect is in window points.
uniform vec4 uRect;
uniform vec2 uResolution;
uniform float uTime;
uniform float uFocus;

#define UI_MODE_FILL 0
#define UI_MODE_TEXTURE 1
#define UI_MODE_GLYPH 2

// Signed distance to a rounded box, negative inside. `b` is the half-extent and
// `r` the corner radius; at r = 0 this is the exact square box, so a square
// corner is the same expression rather than a branch around it.
float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main() {
    int mode = int(vParams.z + 0.5);

    if (mode == UI_MODE_GLYPH) {
        // The SDF core of text_frag.glsl, deliberately the same formula: two
        // renderers disagreeing about where a glyph's edge is would show as
        // text that changes weight depending on which one drew it.
        float dist = texture(uTex, vUV).r;
        // fwidth alone, with no constant added. A fixed smoothing term is a
        // fixed blur in FIELD units, so it widens as the glyph shrinks: at menu
        // sizes it is most of the edge, and the text reads soft and uneven.
        float sw = max(fwidth(dist) * 0.5, 1e-4);
        float core = smoothstep(0.5 - sw, 0.5 + sw, dist);
        float alpha = core * vColor.a;
        if (alpha < 0.003)
            discard;
        FragColor = vec4(vColor.rgb, alpha);
        return;
    }

    vec2 half_size = (vRect.zw - vRect.xy) * 0.5;
    vec2 centre = (vRect.zw + vRect.xy) * 0.5;
    float radius = clamp(vParams.x, 0.0, min(half_size.x, half_size.y));
    float d = sdRoundBox(vPos - centre, half_size, radius);

    // One texel of screen-space falloff. fwidth on the distance itself rather
    // than a fixed epsilon, so the edge stays one pixel wide at any DPI and
    // under any future UI scale.
    float aa = max(fwidth(d), 1e-5);
    float inside = 1.0 - smoothstep(-aa, aa, d);

    vec4 fill = vColor;
    if (mode == UI_MODE_TEXTURE)
        fill *= texture(uTex, vUV);

    // The border is the band between the outer edge and the same box inset by
    // its width -- so it rounds with the corner for free, and a zero width
    // leaves `band` identically zero rather than needing a branch.
    float border_w = max(vParams.y, 0.0);
    float band = 0.0;
    if (border_w > 0.0)
        band = smoothstep(-aa, aa, d + border_w) * inside;

    vec3 rgb = mix(fill.rgb, vBorder.rgb, band * vBorder.a);
    float alpha = mix(fill.a, max(fill.a, vBorder.a), band) * inside;
    if (alpha < 0.003)
        discard;
    FragColor = vec4(rgb, alpha);
}
