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
layout(location = 3) in vec4 aD; // a dashed record's pattern, read only under FLAG_DASHED

uniform float uShapeMinPx;
uniform float uTextMinPx;
uniform float uDashMinPx;
uniform samplerBuffer uGlyphs; // canvas_constants.glsl's glyph table
uniform vec2 uRing;            // an outline ring's inner and outer distance outside a shape

flat out uint vKind;
flat out uint vFlags;
flat out vec4 vColor;
flat out vec4 vOutline; // alpha 0 = no ring
flat out float vRaised;
flat out float vFades; // 1 for a shape a fading view fades
flat out vec4 vShape;  // the kind's measures, in pixels
flat out vec4 vEnds;  // an arc's sector: from xy counter-clockwise to zw; a segment's way on past
                      // a, back toward the segment before, and past b, in its frame; a box's join
flat out vec4 vDash;  // the dash, the period and the place in them at the record's a, in pixels;
                      // a period of 0 is solid
out vec2 vLocal;      // in the shape's own frame, in pixels; for a glyph, its place in the atlas

const float PAD = 1.0; // the outer half of the edge ramp
const float TAU = 6.283185307179586;

float cross2(vec2 a, vec2 b) {
    return a.x * b.y - a.y * b.x;
}

// An end of a segment or an arc, CANVAS_END_*: a cut; the shape's cap at an end of its own; and
// at a corner its join, or open where the corner lies in a dash's gap, the dashes there ending at
// their own caps. Drawn solid, every corner is its join.
uint endCode(uint flags, uint cut, uint joins, uint joined, uint inDash, uint join, bool dashed) {
    if ((flags & cut) != 0u)
        return CANVAS_END_CUT;
    if ((joins & joined) == 0u)
        return flags & CANVAS_FLAG_CAP;
    return dashed && (joins & inDash) == 0u ? CANVAS_END_OPEN : CANVAS_END_JOIN + join;
}

// How far past an end its drawing reaches along the segment, in half widths: nothing past a cut
// or a butt end, a mitre's point at a turn whose cosine is `c`, and a half width past every
// other. The edit has drawn a mitre past its limit as a bevel, so its reach is bounded.
float endReach(uint code, float c) {
    if (code == CANVAS_END_CUT || code == CANVAS_STROKE_BUTT)
        return 0.0;
    if (code != CANVAS_END_JOIN + CANVAS_STROKE_MITRE)
        return 1.0;
    return max(1.0, sqrt(max(1.0 - c, 0.0) / max(1.0 + c, 1e-4)));
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
    vDash = vec4(0.0);
    vLocal = vec2(0.0);
    // Outside every clip plane: a quad of four such corners draws nothing.
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);

    ivec4 tile = texelFetch(uTiles, int(aU.w));
    vRaised = 0.0;
    vFades = 0.0;
    vOutline = vec4(0.0);
    if (kind == CANVAS_KIND_NONE || tile.z == 0 ||
        !canvasColour(aU, vColor, vOutline, vRaised, vFades))
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
    // The pattern in pixels, as every length is. Where a dash or a gap would span fewer than
    // uDashMinPx, pixel centres land inside a dash or not at random: the line is drawn solid at
    // the dash's share of the period, the pattern's average, so it keeps the weight its dashes had.
    vec3 dash = aD.xyz * uPixelsPerUnit;
    bool dashed = (flags & CANVAS_FLAG_DASHED) != 0u && min(dash.x, dash.y) >= uDashMinPx;

    if (kind == CANVAS_KIND_SEGMENT) {
        vec2 a = canvasPixels(aP.xy, tile.xy);
        vec2 b = canvasPixels(aP.zw, tile.xy);
        float hw = max(aF.x * uPixelsPerUnit, 0.5); // never narrower than a pixel
        vec2 d = b - a;
        float len = length(d);
        vec2 dir = len > 1e-6 ? d / len : vec2(1.0, 0.0);
        vec2 n = vec2(-dir.y, dir.x);
        uint joins = uint(aF.w);
        uint endA = endCode(flags, CANVAS_FLAG_CUT_A, joins, CANVAS_JOINED_A, CANVAS_DASHED_A,
                            joins & CANVAS_JOIN_AT_A, dashed);
        uint endB = endCode(flags, CANVAS_FLAG_CUT_B, joins, CANVAS_JOINED_B, CANVAS_DASHED_B,
                            (joins & CANVAS_JOIN_AT_B) >> 2, dashed);
        // The way the stroke goes on past each corner, in the segment's frame: back out of a
        // toward the segment before, on out of b; and the cosine of the turn there.
        vec2 wa = vec2(0.0), wb = vec2(0.0);
        float turnA = 1.0, turnB = 1.0;
        if ((joins & CANVAS_JOINED_A) != 0u) {
            vec2 inA = vec2(cos(aF.y), sin(aF.y));
            wa = vec2(dot(-inA, dir), dot(-inA, n));
            turnA = dot(inA, dir);
        }
        if ((joins & CANVAS_JOINED_B) != 0u) {
            vec2 outB = vec2(cos(aF.z), sin(aF.z));
            wb = vec2(dot(outB, dir), dot(outB, n));
            turnB = dot(dir, outB);
        }
        float along = c.x == 0.0 ? -hw * endReach(endA, turnA) - pad
                                 : len + hw * endReach(endB, turnB) + pad;
        float across = (c.y * 2.0 - 1.0) * (hw + pad);
        pos = a + dir * along + n * across;
        vLocal = vec2(along, across);
        vShape = vec4(len, hw, float(endA | endB << 3), 0.0);
        vEnds = vec4(wa, wb);
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
        // A mitred corner reaches half the stroke past the outline each way, as its sides do.
        vLocal = (c * 2.0 - 1.0) * (halfSize + hs + pad);
        pos = centre + u * vLocal.x + v * vLocal.y;
        vShape = vec4(halfSize, corner, hs);
        vEnds = vec4(float(aP.w), 0.0, 0.0, 0.0);
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
        // A square end's corners lie a half width along the tangent and across from its end.
        bool square = (flags & CANVAS_FLAG_CAP) == CANVAS_STROKE_SQUARE;
        float grow = hw * (square ? 1.4143 : 1.0) + pad;
        vLocal = mix(lo - grow, hi + grow, c);
        pos = centre + vLocal;
        uint endA = endCode(flags, CANVAS_FLAG_CUT_A, 0u, 0u, 0u, 0u, false);
        uint endB = endCode(flags, CANVAS_FLAG_CUT_B, 0u, 0u, 0u, 0u, false);
        vShape = vec4(r, hw, sweep, float(endA | endB << 3));
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
        uint endA = endCode(flags, CANVAS_FLAG_CUT_A, 0u, 0u, 0u, 0u, false);
        uint endB = endCode(flags, CANVAS_FLAG_CUT_B, 0u, 0u, 0u, 0u, false);
        vShape = vec4(len, 0.0, float(endA | endB << 3), 0.0);
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
    // The average as an opacity, not a coverage: its pixels keep the rank their coverage gives.
    if (dashed)
        vDash = vec4(dash.x, dash.x + dash.y, dash.z, 0.0);
    else if ((flags & CANVAS_FLAG_DASHED) != 0u)
        vColor.a *= aD.x / (aD.x + aD.y);
    gl_Position = canvasClip(pos);
}
