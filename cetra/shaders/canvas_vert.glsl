#version 330 core

// The 2D canvas's shapes (spec 14.0): one instance of a unit quad per record, laid out as
// canvas_record.h states, its points in framebuffer pixels from the view's centre, y up
// (canvas_paint.glsl). The quad is the shape's extent padded by a pixel, and vLocal is the
// fragment's place in the shape's own frame, interpolated from the corners: the fragment stage
// measures with small numbers however far the shape's other end is.

#include "canvas_constants.glsl"
#include "canvas_paint.glsl"

layout(location = 0) in ivec4 aP;
layout(location = 1) in vec4 aF;
layout(location = 2) in uvec4 aU;

uniform float uShapeMinPx;
uniform float uTextMinPx;
uniform samplerBuffer uGlyphs; // canvas_constants.glsl's glyph table
uniform vec2 uRing;            // an outline ring's inner and outer distance outside a shape

flat out uint vKind;
flat out uint vFlags;
flat out vec4 vColor;
flat out vec4 vOutline; // alpha 0 = no ring
flat out float vRaised;
flat out vec4 vShape; // the kind's measures, in pixels
flat out vec4 vEnds;  // an arc's sector: from xy counter-clockwise to zw
out vec2 vLocal;      // in the shape's own frame, in pixels; for a glyph, its place in the atlas

const float PAD = 1.0; // the outer half of the edge ramp
const float TAU = 6.283185307179586;

float cross2(vec2 a, vec2 b) {
    return a.x * b.y - a.y * b.x;
}

// How far counter-clockwise from s the direction p lies, in [0, TAU).
float turnFrom(vec2 s, vec2 p) {
    float t = atan(cross2(s, p), dot(s, p));
    return t < 0.0 ? t + TAU : t;
}

void main() {
    uint kind = aU.x & 63u;
    uint flags = (aU.x >> 6) & 1023u;
    vKind = kind;
    vFlags = flags;
    vColor = vec4(0.0);
    vShape = vec4(0.0);
    vEnds = vec4(0.0);
    vLocal = vec2(0.0);
    // Outside every clip plane: a quad of four such corners draws nothing.
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);

    ivec4 tile = texelFetch(uTiles, int(aU.w));
    vRaised = 0.0;
    vOutline = vec4(0.0);
    if (kind == CANVAS_KIND_NONE || tile.z == 0 || !canvasColour(aU, vColor, vOutline, vRaised))
        return;
    // A glyph has no ring, its quad being its atlas cell.
    if (kind == CANVAS_KIND_GLYPH)
        vOutline = vec4(0.0);
    if (!canvasInPass(vRaised, vOutline.a))
        return;

    // The quad's corners as a strip: (0, 0), (1, 0), (0, 1), (1, 1). A shape with an outline
    // reaches out to its ring in the passes that draw it.
    vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    bool ringed = vOutline.a > 0.0 && (uPass == 0 || uPass == 4);
    float pad = PAD + (ringed ? uRing.y : 0.0);
    vec2 pos;
    float span;

    if (kind == CANVAS_KIND_SEGMENT) {
        vec2 a = canvasPixels(aP.xy, tile.xy);
        vec2 b = canvasPixels(aP.zw, tile.xy);
        float hw = max(aF.x * uPixelsPerUnit, 0.5); // never narrower than a pixel
        vec2 d = b - a;
        float len = length(d);
        vec2 dir = len > 1e-6 ? d / len : vec2(1.0, 0.0);
        vec2 n = vec2(-dir.y, dir.x);
        bool roundA = (flags & (CANVAS_FLAG_FLAT_A | CANVAS_FLAG_CUT_A)) == 0u;
        bool roundB = (flags & (CANVAS_FLAG_FLAT_B | CANVAS_FLAG_CUT_B)) == 0u;
        float along = c.x == 0.0 ? -(roundA ? hw : 0.0) - pad : len + (roundB ? hw : 0.0) + pad;
        float across = (c.y * 2.0 - 1.0) * (hw + pad);
        pos = a + dir * along + n * across;
        vLocal = vec2(along, across);
        vShape = vec4(len, hw, 0.0, 0.0);
        span = len + 2.0 * hw;
    } else if (kind == CANVAS_KIND_CIRCLE) {
        vec2 centre = canvasPixels(aP.xy, tile.xy);
        float r = float(aP.z) * uPixelsPerUnit;
        float hs = aP.w > 0 ? max(float(aP.w) * uPixelsPerUnit * 0.5, 0.5) : 0.0;
        vLocal = (c * 2.0 - 1.0) * (r + hs + pad);
        pos = centre + vLocal;
        vShape = vec4(r, hs, 0.0, 0.0);
        span = 2.0 * (r + hs);
    } else if (kind == CANVAS_KIND_BOX) {
        vec2 centre = canvasPixels(aP.xy, tile.xy);
        vec2 halfSize = aF.xy * uPixelsPerUnit;
        float corner = min(aF.z * uPixelsPerUnit, min(halfSize.x, halfSize.y));
        float hs = aP.z > 0 ? max(float(aP.z) * uPixelsPerUnit * 0.5, 0.5) : 0.0;
        vec2 u = vec2(cos(aF.w), sin(aF.w));
        vec2 v = vec2(-u.y, u.x);
        vLocal = (c * 2.0 - 1.0) * (halfSize + hs + pad);
        pos = centre + u * vLocal.x + v * vLocal.y;
        vShape = vec4(halfSize, corner, hs);
        span = 2.0 * (max(halfSize.x, halfSize.y) + hs);
    } else if (kind == CANVAS_KIND_ARC) {
        // Counter-clockwise from a to b, an edit having swapped a clockwise arc's ends.
        vec2 a = canvasPixels(aP.xy, tile.xy);
        vec2 b = canvasPixels(aP.zw, tile.xy);
        vec2 centre = (a + b) * 0.5 + aF.yz * uPixelsPerUnit;
        float r = aF.w * uPixelsPerUnit;
        float hw = max(aF.x * uPixelsPerUnit, 0.5);
        vec2 s = normalize(a - centre), e = normalize(b - centre);
        float sweep = turnFrom(s, e);
        if (sweep <= 0.0)
            sweep = TAU;
        // Its bound: the two ends and each quarter point the sector passes.
        vec2 lo = min(s, e) * r, hi = max(s, e) * r;
        vec2 axes[4] = vec2[4](vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(-1.0, 0.0), vec2(0.0, -1.0));
        for (int q = 0; q < 4; q++) {
            if (turnFrom(s, axes[q]) <= sweep) {
                lo = min(lo, axes[q] * r);
                hi = max(hi, axes[q] * r);
            }
        }
        float grow = hw + pad;
        vLocal = mix(lo - grow, hi + grow, c);
        pos = centre + vLocal;
        vShape = vec4(r, hw, sweep, 0.0);
        vEnds = vec4(s, e);
        span = max(hi.x - lo.x, hi.y - lo.y) + 2.0 * hw;
    } else if (kind == CANVAS_KIND_EDGE) {
        // The outer half of a fill's edge ramp: a band from a pixel inside the edge to a pixel
        // and a half outside, the polygon on the left of a to b, past the ends to its corners.
        vec2 a = canvasPixels(aP.xy, tile.xy);
        vec2 b = canvasPixels(aP.zw, tile.xy);
        vec2 d = b - a;
        float len = length(d);
        vec2 dir = len > 1e-6 ? d / len : vec2(1.0, 0.0);
        vec2 out_ = vec2(dir.y, -dir.x); // the right of a to b
        float extA = (flags & CANVAS_FLAG_CUT_A) != 0u ? PAD : 0.5 + pad;
        float extB = (flags & CANVAS_FLAG_CUT_B) != 0u ? PAD : 0.5 + pad;
        float along = c.x == 0.0 ? -extA : len + extB;
        float across = c.y == 0.0 ? -PAD : 0.5 + pad;
        pos = a + dir * along + out_ * across;
        vLocal = vec2(along, across);
        vShape = vec4(len, 0.0, 0.0, 0.0);
        span = len;
    } else if (kind == CANVAS_KIND_GLYPH) {
        // The glyph's box about its pen, in ems, mirrored and turned about the string's anchor.
        // The box is the atlas's whole cell, so it already holds the outer half of the ramp
        // wherever the field reaches half a pixel; past that the string is too small to draw.
        float emPx = aF.z * uPixelsPerUnit;
        if (emPx < uTextMinPx)
            return;
        int base = aP.w * CANVAS_GLYPH_CODEPOINTS * 2;
        vec4 font = texelFetch(uGlyphs, base); // em, edge, falloff, spread
        float atlasPx = emPx / font.x;         // framebuffer pixels an atlas pixel spans
        if (font.w * atlasPx < 0.5)
            return;
        vec4 box = texelFetch(uGlyphs, base + aP.z * 2); // x0, y0, x1, y1
        vec4 cell = texelFetch(uGlyphs, base + aP.z * 2 + 1);
        vec2 local = (aF.xy + mix(box.xy, box.zw, c)) * emPx;
        if ((flags & CANVAS_FLAG_MIRROR) != 0u)
            local.x = -local.x;
        vec2 u = vec2(cos(aF.w), sin(aF.w));
        pos = canvasPixels(aP.xy, tile.xy) + u * local.x + vec2(-u.y, u.x) * local.y;
        vLocal = mix(cell.xy, cell.zw, c);
        vShape = vec4(font.y, font.z / atlasPx, float(aP.w), 0.0);
        span = emPx;
    } else {
        return;
    }

    // A polygon's edge pieces are not shapes of their own: dropping one by its length would
    // harden the outline wherever a piece is short or cut short at the tile grid.
    if (kind != CANVAS_KIND_EDGE && span < uShapeMinPx)
        return;
    gl_Position = canvasClip(pos);
}
