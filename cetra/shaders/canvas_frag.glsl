#version 330 core

// The 2D canvas's shapes (spec 14.0): each fragment's signed distance d from the shape's exact
// outline, in pixels, and its coverage clamp(0.5 - d), so a shape's coverage summed over the
// frame is its area. canvas_vert.glsl puts the fragment in the shape's own frame (vLocal).
//
// A cut end is where an edit split a shape that reached too far from its tile. Each piece draws
// on half a pixel past the cut, as though it went on, so the two overlap by a pixel: where
// exactly each places the cut is float arithmetic from its own end points, and pieces that each
// own only their own side leave a line of pixels neither owns wherever the two disagree. An
// overlap of a single colour shows nowhere.

#include "canvas_constants.glsl"
#include "canvas_pass.glsl"

flat in uint vKind;
flat in uint vFlags;
flat in vec4 vColor;
flat in vec4 vOutline;
flat in float vRaised;
flat in vec4 vShape;
flat in vec4 vEnds;
flat in vec4 vDash;
in vec2 vLocal;

out vec4 FragColor;

const float TAU = 6.283185307179586;

uniform vec2 uRing; // an outline ring's inner and outer distance outside a shape, in pixels

float cross2(vec2 a, vec2 b) {
    return a.x * b.y - a.y * b.x;
}

// A flat end: `out` how far past the end, `across` from the line through it.
float flatEnd(float out_, float across, float hw) {
    vec2 q = vec2(out_, across - hw);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
}

const float CUT_OVERLAP = 0.5; // pixels each piece draws past a cut

// An end that is not a corner, `out_` along past it: cut, butt, square or round.
float freeEnd(float out_, float across, float hw, uint cut, uint butt, uint square) {
    if ((vFlags & cut) != 0u) {
        if (out_ > CUT_OVERLAP)
            discard;
        return across - hw;
    }
    if ((vFlags & butt) != 0u)
        return flatEnd(out_, across, hw);
    if ((vFlags & square) != 0u)
        return flatEnd(out_ - hw, across, hw);
    return out_ > 0.0 ? length(vec2(out_, across)) - hw : across - hw;
}

/*
 * A corner of a polyline, which each of its two segments draws its own side of: the segment's
 * body run on past its end and cut at the corner's bisector, where the neighbour's begins, their
 * outer edges meeting in a mitre's point. Each draws on half a pixel past the bisector, as a piece
 * does past a cut, but only within the neighbour's width: the two edges' ramps there would
 * otherwise each give the seam half its coverage, and the layer's max would show a line, while
 * past the neighbour's outer edge the half pixel would stand off the mitre's point as a speck of
 * a third of a pixel's coverage. A bevel cuts the point off along the line
 * through the two outer edges' ends; a round corner is the round end the two share. `q` is the
 * place from the corner, `o` the way out of the segment there and `w` the way the stroke goes on,
 * in the segment's frame.
 */
float joinedEnd(float body, vec2 q, vec2 o, vec2 w, float hw, uint join) {
    if (join == CANVAS_STROKE_ROUND)
        return dot(q, o) > 0.0 ? length(q) - hw : body;
    float past = dot(q, normalize(o + w));
    float d = max(body, min(past, max(past - CUT_OVERLAP, abs(cross2(w, q)) - hw)));
    if (join == CANVAS_STROKE_BEVEL) {
        // The outer side is the one the stroke turns away from: the right of both ways for a
        // turn to the left, and the left for one to the right.
        bool left = cross2(o, w) > 0.0;
        vec2 out_ = left ? vec2(o.y, -o.x) : vec2(-o.y, o.x);
        vec2 next = left ? vec2(w.y, -w.x) : vec2(-w.y, w.x);
        vec2 u = normalize(out_ + next);
        d = max(d, dot(q, u) - hw * dot(out_, u));
    }
    return d;
}

// A segment with a corner or a square end: each end by its cap or its join. A corner in a dash's
// gap is drawn by nothing: the dashes there end at their own caps, and a dash starting at the
// corner starts square, as PDF draws a dash beginning at a point of its path.
float strokeDistance(float len, float hw, uint joins) {
    float along = vLocal.x, across = abs(vLocal.y);
    float body = across - hw;
    bool dashed = vDash.y > 0.0;
    bool cornerA = (joins & CANVAS_JOINED_A) != 0u && (!dashed || (joins & CANVAS_DASHED_A) != 0u);
    bool cornerB = (joins & CANVAS_JOINED_B) != 0u && (!dashed || (joins & CANVAS_DASHED_B) != 0u);
    float a = cornerA ? joinedEnd(body, vLocal, vec2(-1.0, 0.0), vEnds.xy, hw,
                                  joins & CANVAS_JOIN_AT_A)
              : (joins & CANVAS_JOINED_A) != 0u
                  ? body
                  : freeEnd(-along, across, hw, CANVAS_FLAG_CUT_A, CANVAS_FLAG_FLAT_A,
                            CANVAS_FLAG_SQUARE_A);
    float b = cornerB ? joinedEnd(body, vLocal - vec2(len, 0.0), vec2(1.0, 0.0), vEnds.zw, hw,
                                  (joins & CANVAS_JOIN_AT_B) >> 2)
              : (joins & CANVAS_JOINED_B) != 0u
                  ? body
                  : freeEnd(along - len, across, hw, CANVAS_FLAG_CUT_B, CANVAS_FLAG_FLAT_B,
                            CANVAS_FLAG_SQUARE_B);
    return max(a, b);
}

float segmentDistance() {
    float len = vShape.x, hw = vShape.y;
    uint joins = uint(vShape.z);
    if (joins != 0u || (vFlags & (CANVAS_FLAG_SQUARE_A | CANVAS_FLAG_SQUARE_B)) != 0u)
        return strokeDistance(len, hw, joins);
    float along = vLocal.x, across = abs(vLocal.y);
    if (along < 0.0) {
        if ((vFlags & CANVAS_FLAG_CUT_A) != 0u) {
            if (along < -CUT_OVERLAP)
                discard;
            return across - hw;
        }
        if ((vFlags & CANVAS_FLAG_FLAT_A) != 0u)
            return flatEnd(-along, across, hw);
        return length(vec2(along, across)) - hw;
    }
    if (along > len) {
        if ((vFlags & CANVAS_FLAG_CUT_B) != 0u) {
            if (along - len > CUT_OVERLAP)
                discard;
            return across - hw;
        }
        if ((vFlags & CANVAS_FLAG_FLAT_B) != 0u)
            return flatEnd(along - len, across, hw);
        return length(vec2(along - len, across)) - hw;
    }
    // Between the ends: the sides, and the inner half of a flat end's ramp.
    float d = across - hw;
    if ((vFlags & CANVAS_FLAG_FLAT_A) != 0u)
        d = max(d, -along);
    if ((vFlags & CANVAS_FLAG_FLAT_B) != 0u)
        d = max(d, along - len);
    return d;
}

float arcDistance() {
    float r = vShape.x, hw = vShape.y, sweep = vShape.z;
    vec2 s = vEnds.xy, e = vEnds.zw, p = vLocal;
    float t = atan(cross2(s, p), dot(s, p));
    if (t < 0.0)
        t += TAU;
    if (t <= sweep) {
        // The band, and the inner half of a flat end's ramp: -cross(s, p) is how far p lies
        // inside the radial line through s, and cross(e, p) inside the one through e. Each is
        // the distance to its end only within a quarter turn of it: past that the line runs on
        // through the far side of the centre, and on an arc of more than half a turn it would
        // cut the band away there.
        float d = abs(length(p) - r) - hw;
        if ((vFlags & CANVAS_FLAG_FLAT_A) != 0u && t < 0.25 * TAU)
            d = max(d, -cross2(s, p));
        if ((vFlags & CANVAS_FLAG_FLAT_B) != 0u && sweep - t < 0.25 * TAU)
            d = max(d, cross2(e, p));
        return d;
    }
    // Past the sector: the nearer end by angle, and the tangent there pointing away from the arc.
    bool atEnd = t - sweep < TAU - t;
    vec2 dir = atEnd ? e : s;
    vec2 away = atEnd ? vec2(-e.y, e.x) : vec2(s.y, -s.x);
    uint cut = atEnd ? CANVAS_FLAG_CUT_B : CANVAS_FLAG_CUT_A;
    uint flat_ = atEnd ? CANVAS_FLAG_FLAT_B : CANVAS_FLAG_FLAT_A;
    vec2 q = p - dir * r;
    if ((vFlags & cut) != 0u) {
        if (abs(dot(q, away)) > CUT_OVERLAP)
            discard;
        return abs(length(p) - r) - hw;
    }
    if ((vFlags & flat_) != 0u)
        return flatEnd(dot(q, away), abs(dot(q, dir)), hw);
    if ((vFlags & (atEnd ? CANVAS_FLAG_SQUARE_B : CANVAS_FLAG_SQUARE_A)) != 0u)
        return flatEnd(dot(q, away) - hw, abs(dot(q, dir)), hw);
    return length(q) - hw;
}

// The outer half of a fill's edge ramp: a segment with no width, on the side away from the
// polygon, which the fill's own ramp stops at; past an end, the distance to the polygon's corner
// there. It reaches a thirty-second of a pixel inside: a pixel centre on the edge has coverage
// 0.5 whichever side rounding puts it, and cut exactly at the line, which side a pass kept it on
// depended on the quad it was interpolated across. One the fill did not own either was then never
// painted, and its alpha was left for the next layer. The overlap gives the same coverage twice,
// which the layer's max absorbs.
float edgeDistance() {
    if (vLocal.y < -1.0 / 32.0)
        discard;
    return segmentDistance();
}

// A glyph: its font's distance field, whose value is vShape.x on the outline and falls by
// vShape.y a framebuffer pixel outward. The four fonts are four units, chosen by branch, and
// read at the atlas's own level: there is one, and a branch on a flat value has no derivatives.
uniform sampler2D uFont0;
uniform sampler2D uFont1;
uniform sampler2D uFont2;
uniform sampler2D uFont3;

float glyphDistance() {
    int slot = int(vShape.z);
    float value = slot == 0   ? textureLod(uFont0, vLocal, 0.0).r
                  : slot == 1 ? textureLod(uFont1, vLocal, 0.0).r
                  : slot == 2 ? textureLod(uFont2, vLocal, 0.0).r
                              : textureLod(uFont3, vLocal, 0.0).r;
    return (vShape.x - value) / vShape.y;
}

float circleDistance() {
    float d = length(vLocal) - vShape.x;
    return vShape.y > 0.0 ? abs(d) - vShape.y : d;
}

// A stroked square-cornered box's outline, a mitre's corners square and a bevel's cut off along
// the line through its two outer edges' ends.
float boxJoined(uint join) {
    vec2 q = abs(vLocal) - vShape.xy;
    float hs = vShape.w;
    float d = abs(max(q.x, q.y)) - hs;
    return join == CANVAS_STROKE_BEVEL ? max(d, (q.x + q.y - hs) * 0.7071067811865476) : d;
}

float boxDistance() {
    if (vEnds.x != 0.0)
        return boxJoined(uint(vEnds.x));
    vec2 halfSize = vShape.xy;
    float corner = vShape.z, hs = vShape.w;
    vec2 q = abs(vLocal) - halfSize + corner;
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - corner;
    return hs > 0.0 ? abs(d) - hs : d;
}

/*
 * Dashes: the stroke cut into pieces along its length (canvas.h), each drawn with the cap at its
 * ends. The fragment's place along the shape picks the dash nearest it, which is cut at the
 * shape's own ends, run on past a corner it reaches so that the corner takes its join, and run on
 * through a cut, so both pieces give a pixel there the same coverage. A dashed shape's distance is
 * the larger of its own and its nearest dash's.
 */

// Where the fragment is along a dashed shape: its place in pixels from the record's a; how much
// a length on the centreline spans at the fragment's own distance from a curve's centre, so a
// dash's ends lie along the radius; how far it is across from the centreline, and the stroke's
// half width; the shape's length; and which of its ends are its own, which hold the place and
// cut a dash, and which are corners, which hold the place and let a dash run on.
struct Along {
    float s, scale, across, hw, len;
    bool ownA, ownB, cornerA, cornerB;
};

// The place along a box's outline from the middle of its bottom side, counter-clockwise: each
// side, then each quarter-circle corner, its turn times its radius.
float boxPlace() {
    vec2 p = vLocal;
    float c = vShape.z;
    float a = vShape.x - c, b = vShape.y - c, q = 0.25 * TAU * c;
    vec2 k = abs(p) - vec2(a, b);
    if (k.x > 0.0 && k.y > 0.0) {
        vec2 o = p - sign(p) * vec2(a, b); // from the corner's centre
        float phi = atan(o.y, o.x);
        if (p.y < 0.0)
            return p.x > 0.0 ? a + c * (phi + 0.25 * TAU)
                             : 3.0 * a + 3.0 * q + 4.0 * b + c * (phi + 0.5 * TAU);
        return p.x > 0.0 ? a + q + 2.0 * b + c * phi
                         : 3.0 * a + 2.0 * q + 2.0 * b + c * (phi - 0.25 * TAU);
    }
    if (k.y > k.x) {
        if (p.y > 0.0)
            return 2.0 * a + 2.0 * q + 2.0 * b - p.x;
        return p.x >= 0.0 ? p.x : 4.0 * (a + b + q) + p.x;
    }
    return p.x > 0.0 ? a + q + b + p.y : 3.0 * a + 3.0 * q + 3.0 * b - p.y;
}

Along dashAlong() {
    Along a = Along(0.0, 1.0, 0.0, 0.0, 0.0, false, false, false, false);
    bool cutA = (vFlags & CANVAS_FLAG_CUT_A) != 0u, cutB = (vFlags & CANVAS_FLAG_CUT_B) != 0u;
    if (vKind == CANVAS_KIND_SEGMENT) {
        // A corner the edit found inside a dash runs that dash on round it; any other cuts the
        // dashes there as the shape's own end does.
        uint joins = uint(vShape.z);
        a.cornerA = (joins & CANVAS_DASHED_A) != 0u;
        a.cornerB = (joins & CANVAS_DASHED_B) != 0u;
        a.ownA = !a.cornerA && !cutA;
        a.ownB = !a.cornerB && !cutB;
        a.s = vLocal.x;
        a.across = abs(vLocal.y);
        a.hw = vShape.y;
        a.len = vShape.x;
    } else if (vKind == CANVAS_KIND_ARC) {
        float r = vShape.x, sweep = vShape.z;
        vec2 s = vEnds.xy, e = vEnds.zw, p = vLocal;
        float t = atan(cross2(s, p), dot(s, p));
        t = t < 0.0 ? t + TAU : t;
        a.ownA = !cutA;
        a.ownB = !cutB;
        a.hw = vShape.y;
        a.len = r * sweep;
        a.across = abs(length(p) - r);
        if (t <= sweep) {
            a.s = r * t;
            a.scale = length(p) / r;
        } else if (t - sweep < TAU - t) {
            // Past an end, straight on along its tangent.
            vec2 q = p - e * r;
            a.s = r * sweep + dot(q, vec2(-e.y, e.x));
            a.across = abs(dot(q, e));
        } else {
            vec2 q = p - s * r;
            a.s = -dot(q, vec2(s.y, -s.x));
            a.across = abs(dot(q, s));
        }
    } else if (vKind == CANVAS_KIND_CIRCLE) {
        float t = atan(vLocal.y, vLocal.x);
        a.s = vShape.x * (t < 0.0 ? t + TAU : t);
        a.scale = length(vLocal) / vShape.x;
        a.across = abs(length(vLocal) - vShape.x);
        a.hw = vShape.y;
    } else {
        a.s = boxPlace();
        a.hw = vShape.w;
        a.across = boxDistance() + a.hw; // the outline's distance, the stroke taken off again
    }
    return a;
}

// The nearer of a dash running [x, y] along the shape and `best`, as a distance outside it along
// the path: a dash cut to nothing by the shape's own ends is none.
float dashNearer(float x, float y, Along a, float best) {
    // The dash the edit found the corner in: within half a pixel of it, as float places it.
    if (a.cornerA && x <= CUT_OVERLAP)
        x = -1e30;
    if (a.cornerB && y >= a.len - CUT_OVERLAP)
        y = 1e30;
    x = a.ownA ? max(x, 0.0) : x;
    y = a.ownB ? min(y, a.len) : y;
    return x > y ? best : min(best, max(x - a.s, a.s - y));
}

// The fragment's distance from the dash nearest it, its ends drawn with the cap.
float dashDistance() {
    Along a = dashAlong();
    // The place held to the shape's own ends and its corners, so the dash it picks is one of the
    // shape's, and a corner is drawn by whether it falls inside a dash.
    float held = a.s;
    held = a.ownA || a.cornerA ? max(held, 0.0) : held;
    held = a.ownB || a.cornerB ? min(held, a.len) : held;
    // The dash at or before the held place in the pattern's own direction, and the one after.
    bool back = (vFlags & CANVAS_FLAG_DASH_BACK) != 0u;
    float t = back ? -held : held;
    float start = t - mod(vDash.z + t, vDash.y);
    float e = 1e30;
    for (int k = 0; k < 2; k++) {
        float t0 = start + float(k) * vDash.y, t1 = t0 + vDash.x;
        e = back ? dashNearer(-t1, -t0, a, e) : dashNearer(t0, t1, a, e);
    }
    e *= a.scale;
    uint cap = uint(vDash.w + 0.5);
    if (cap == CANVAS_STROKE_BUTT)
        return max(e, a.across - a.hw);
    if (cap == CANVAS_STROKE_SQUARE)
        return max(e - a.hw, a.across - a.hw);
    return e > 0.0 ? length(vec2(e, a.across)) - a.hw : a.across - a.hw;
}

void main() {
    float d;
    if (vKind == CANVAS_KIND_SEGMENT)
        d = segmentDistance();
    else if (vKind == CANVAS_KIND_ARC)
        d = arcDistance();
    else if (vKind == CANVAS_KIND_CIRCLE)
        d = circleDistance();
    else if (vKind == CANVAS_KIND_EDGE)
        d = edgeDistance();
    else if (vKind == CANVAS_KIND_GLYPH)
        d = glyphDistance();
    else
        d = boxDistance();
    if (vDash.y > 0.0)
        d = max(d, dashDistance());
    // An outline is a ring uRing.x to uRing.y pixels outside the shape. Its inner ramp starts at
    // least half a pixel out, where the shape's own ends, so a pixel is the shape's or the ring's
    // and never both.
    if (vOutline.a > 0.0 && d > 0.5) {
        float ring = max(uRing.x - d, d - uRing.y);
        FragColor = canvasOutput(vOutline, CANVAS_RANK_RING, clamp(0.5 - ring, 0.0, 1.0));
        return;
    }
    // A dashed shape too fine to draw as dashes is its pattern's average (canvas_vert.glsl).
    float strength = vDash.y == 0.0 && vDash.x > 0.0 ? vDash.x : 1.0;
    FragColor = canvasOutput(vColor, vRaised > 0.5 ? CANVAS_RANK_RAISED : CANVAS_RANK_SHAPE,
                             clamp(0.5 - d, 0.0, 1.0) * strength);
}
