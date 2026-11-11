#version 330 core

in vec2 vUV;
in vec4 vColor;
in vec2 vPos;
in vec4 vRect;
// x corner radius or a line's half width, y border width, z mode, w a glyph's edge or the corners a
// fill or an image rounds
in vec4 vParams;
// The border's colour; a line's place along and across it and its length; a convex shape's distance
// to each edge, positive inside.
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
#define UI_MODE_LINE 3
#define UI_MODE_CONVEX 4

// Signed distance to a rounded box, negative inside. `b` is the half-extent and
// `r` the corner radius; at r = 0 this is the exact square box, so a square
// corner is the same expression rather than a branch around it.
float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main() {
    int mode = int(vParams.z + 0.5);

    // A one-pixel ramp centred on the edge, a box filter's coverage of it, so a
    // shape on the pixel grid fills its pixels whole and two meeting there leave
    // nothing between them. A fill is drawn a pixel past its rect (_push_quad),
    // so the ramp's outer half is drawn. `aa` is a pixel's size in vPos's
    // units, at any DPI or UI scale, so it is one pixel along an edge at any
    // angle. Not d's own derivatives: across a box's corner, where d has a
    // kink, they read 1.4 pixels, and the corner pixel of a box on the grid is
    // left part covered. Taken before any branch, where every fragment of the
    // pixel quad reaches it.
    float aa = max(length(dFdx(vPos)), 1e-5);

    if (mode == UI_MODE_LINE || mode == UI_MODE_CONVEX) {
        float cover;
        if (mode == UI_MODE_LINE) {
            // The distance to the segment, less half the width: the round
            // ends are where the place along it is clamped to its length.
            float along = vBorder.x - clamp(vBorder.x, 0.0, vBorder.z);
            float d = length(vec2(along, vBorder.y)) - vParams.x;
            cover = clamp(0.5 - d / aa, 0.0, 1.0);
        } else {
            // The product of the edges' ramps: the box filter's coverage of a
            // corner between edges at right angles, so a shape on whole pixels
            // draws as a fill of the same rect. Their minimum would cover a
            // corner pixel by the lesser edge's share where the box covers the
            // product.
            vec4 edge = clamp(0.5 + vBorder / aa, 0.0, 1.0);
            cover = edge.x * edge.y * edge.z * edge.w;
        }
        float alpha = cover * vColor.a;
        if (alpha < 0.003)
            discard;
        FragColor = vec4(vColor.rgb, alpha);
        return;
    }

    if (mode == UI_MODE_GLYPH) {
        // The SDF core of text_frag.glsl, deliberately the same formula: two
        // renderers disagreeing about where a glyph's edge is would show as
        // text that changes weight depending on which one drew it. The edge is
        // the font's (Font.sdf_edge), where its field has the outline; cut at
        // 0.5, text was drawn about two atlas pixels bolder than the face.
        float dist = texture(uTex, vUV).r;
        float edge = vParams.w;
        // fwidth alone, with no constant added. A fixed smoothing term is a
        // fixed blur in FIELD units, so it widens as the glyph shrinks: at menu
        // sizes it is most of the edge, and the text reads soft and uneven.
        float sw = max(fwidth(dist) * 0.5, 1e-4);
        float core = smoothstep(edge - sw, edge + sw, dist);
        float alpha = core * vColor.a;
        if (alpha < 0.003)
            discard;
        FragColor = vec4(vColor.rgb, alpha);
        return;
    }

    vec2 half_size = (vRect.zw - vRect.xy) * 0.5;
    vec2 centre = (vRect.zw + vRect.xy) * 0.5;
    float radius = clamp(vParams.x, 0.0, min(half_size.x, half_size.y));
    // The corners rounded, UIStyle.corners riding a fill's last parameter: top-left 1, top-right
    // 2, bottom-right 4, bottom-left 8, down the window, 0 for all four. Each quadrant of the box
    // takes the radius or none; a square corner is the same expression at r = 0.
    vec2 p = vPos - centre;
    int corners = int(vParams.w + 0.5);
    int corner = p.y < 0.0 ? (p.x < 0.0 ? 1 : 2) : (p.x < 0.0 ? 8 : 4);
    float d = sdRoundBox(p, half_size, corners == 0 || (corners & corner) != 0 ? radius : 0.0);
    float inside = clamp(0.5 - d / aa, 0.0, 1.0);

    vec4 fill = vColor;
    if (mode == UI_MODE_TEXTURE)
        fill *= texture(uTex, vUV);

    // The border is the band between the outer edge and the same box inset by
    // its width -- so it rounds with the corner for free. `ring` is its inner
    // edge; the outer edge is `inside`, applied once, to the whole box.
    float border_w = max(vParams.y, 0.0);
    float ring = border_w > 0.0 ? clamp(0.5 + (d + border_w) / aa, 0.0, 1.0) : 0.0;

    // The border laid over the fill by Porter-Duff over, in straight colour for
    // the blend: each weighted by its own alpha, so a clear fill's colour counts
    // for nothing. With no border this is the fill exactly; over an opaque fill
    // it is the border mixed into the fill by its alpha. A pixel written has
    // `a` of at least the discard's bound, so `ba / a` is defined.
    float ba = ring * vBorder.a;
    float a = ba + fill.a * (1.0 - ba);
    float alpha = a * inside;
    if (alpha < 0.003)
        discard;
    FragColor = vec4(mix(fill.rgb, vBorder.rgb, ba / a), alpha);
}
