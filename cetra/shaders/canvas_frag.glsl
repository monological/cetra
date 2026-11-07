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
flat in float vFades;
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

// An end that is not a corner's join, by its code (canvas_constants.glsl's CANVAS_END_*), `out_`
// along past it and `across` from the centreline: a cut; a corner left open, which the dashes
// there end; or a cap, the shape's own or a dash's. Inside a flat end, the inner half of its ramp.
float capEnd(uint code, float out_, float across, float hw) {
    if (code == CANVAS_END_CUT) {
        if (out_ > CUT_OVERLAP)
            discard;
        return across - hw;
    }
    if (code == CANVAS_END_OPEN)
        return across - hw;
    if (code == CANVAS_STROKE_ROUND)
        return out_ > 0.0 ? length(vec2(out_, across)) - hw : across - hw;
    float flat_ = code == CANVAS_STROKE_SQUARE ? out_ - hw : out_;
    return flat_ > 0.0 ? flatEnd(flat_, across, hw) : max(across - hw, flat_);
}

/*
 * A corner of a polyline, which each of its two segments draws its own side of. Inside the turn
 * each draws its own body to its end, the two overlapping. Outside it, the body runs on past the
 * end to the corner's bisector, where the neighbour's begins, their outer edges meeting in a
 * mitre's point: cut at the end there, a dash ending just past the corner on the neighbour would
 * leave the corner of this segment's body that lies past the bisector drawn by neither. Each
 * draws on half a pixel past the bisector, as a piece does past a cut, but only within the
 * neighbour's width: the two edges' ramps there would otherwise each give the seam half its
 * coverage, and the layer's max would show a line, while past the neighbour's outer edge the half
 * pixel would stand off the mitre's point as a speck of a third of a pixel's coverage. A bevel
 * cuts the point off along the line through the two outer edges' ends; a round corner is the
 * round end the two share. `q` is the place from the corner, `o` the way out of the segment there
 * and `w` the way the stroke goes on, in the segment's frame.
 */
float joinedEnd(uint join, float body, vec2 q, vec2 o, vec2 w, float hw) {
    if (join == CANVAS_STROKE_ROUND)
        return dot(q, o) > 0.0 ? length(q) - hw : body;
    // The outer side is the one the stroke turns away from: the right of both ways for a turn to
    // the left, and the left for one to the right.
    bool left = cross2(o, w) > 0.0;
    vec2 outer = left ? vec2(o.y, -o.x) : vec2(-o.y, o.x);
    if (dot(q, outer) <= 0.0)
        return max(body, dot(q, o));
    float past = dot(q, normalize(o + w));
    float d = max(body, min(past, max(past - CUT_OVERLAP, abs(cross2(w, q)) - hw)));
    if (join == CANVAS_STROKE_BEVEL) {
        vec2 next = left ? vec2(w.y, -w.x) : vec2(-w.y, w.x);
        vec2 u = normalize(outer + next);
        d = max(d, dot(q, u) - hw * dot(outer, u));
    }
    return d;
}

// A segment's end by its code, `q` the place from it and `o` the way out of the segment there,
// `w` the way the stroke goes on past a corner.
float segmentEnd(uint code, float body, float across, float hw, vec2 q, vec2 o, vec2 w) {
    if (code >= CANVAS_END_JOIN)
        return joinedEnd(code - CANVAS_END_JOIN, body, q, o, w, hw);
    return capEnd(code, dot(q, o), across, hw);
}

float segmentDistance() {
    float len = vShape.x, hw = vShape.y;
    uint ends = uint(vShape.z);
    float across = abs(vLocal.y), body = across - hw;
    return max(segmentEnd(ends & 7u, body, across, hw, vLocal, vec2(-1.0, 0.0), vEnds.xy),
               segmentEnd(ends >> 3, body, across, hw, vLocal - vec2(len, 0.0), vec2(1.0, 0.0),
                          vEnds.zw));
}

float arcDistance() {
    float r = vShape.x, hw = vShape.y, sweep = vShape.z;
    uint ends = uint(vShape.w);
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
        if ((ends & 7u) == CANVAS_STROKE_BUTT && t < 0.25 * TAU)
            d = max(d, -cross2(s, p));
        if ((ends >> 3) == CANVAS_STROKE_BUTT && sweep - t < 0.25 * TAU)
            d = max(d, cross2(e, p));
        return d;
    }
    // Past the sector: the nearer end by angle, and the tangent there pointing away from the arc.
    bool atEnd = t - sweep < TAU - t;
    vec2 dir = atEnd ? e : s;
    vec2 away = atEnd ? vec2(-e.y, e.x) : vec2(s.y, -s.x);
    uint end = atEnd ? ends >> 3 : ends & 7u;
    vec2 q = p - dir * r;
    if (end == CANVAS_END_CUT) {
        if (abs(dot(q, away)) > CUT_OVERLAP)
            discard;
        return abs(length(p) - r) - hw;
    }
    // A round end is the distance from the end's own point.
    if (end == CANVAS_STROKE_ROUND)
        return length(q) - hw;
    return capEnd(end, dot(q, away), abs(dot(q, dir)), hw);
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
 * Dashes: the stroke cut into pieces along its length (canvas.h), each drawn with the shape's cap
 * at its ends. The fragment's place along the shape picks the dash nearest it, which is cut at
 * the shape's own ends and at a corner it does not run through, run on past a corner it runs
 * through so that the corner takes its join, and run on through a cut, so both pieces give a
 * pixel there the same coverage. A dashed shape's distance is the larger of its own and its
 * nearest dash's.
 */

// Where the fragment is along a run of a dashed shape from one end or corner to the next: its
// place in pixels from the run's start, and the pattern's place at that start; how much a length
// on the centreline spans at the fragment's own distance from a curve's centre, so a dash's ends
// lie along the radius; how far it is across from the centreline, and the stroke's half width;
// the run's length; and which of its ends cut a dash, its own ends and corners in a gap, and
// which run one on, corners inside a dash. A cut does neither.
struct Along {
    float s, phase, scale, across, hw, len;
    bool ownA, ownB, cornerA, cornerB;
};

// How far counter-clockwise from s the direction p lies, in [0, TAU).
float turnFrom(vec2 s, vec2 p) {
    float t = atan(cross2(s, p), dot(s, p));
    return t < 0.0 ? t + TAU : t;
}

Along segmentAlong() {
    uint ends = uint(vShape.z), endA = ends & 7u, endB = ends >> 3;
    return Along(vLocal.x, vDash.z, 1.0, abs(vLocal.y), vShape.y, vShape.x,
                 endA != CANVAS_END_CUT && endA < CANVAS_END_JOIN,
                 endB != CANVAS_END_CUT && endB < CANVAS_END_JOIN, endA >= CANVAS_END_JOIN,
                 endB >= CANVAS_END_JOIN);
}

Along arcAlong() {
    float r = vShape.x, sweep = vShape.z;
    uint ends = uint(vShape.w);
    vec2 s = vEnds.xy, e = vEnds.zw, p = vLocal;
    float t = turnFrom(s, p);
    Along a = Along(r * t, vDash.z, length(p) / r, abs(length(p) - r), vShape.y, r * sweep,
                    (ends & 7u) != CANVAS_END_CUT, (ends >> 3) != CANVAS_END_CUT, false, false);
    if (t > sweep) {
        // Past an end, straight on along its tangent.
        bool atEnd = t - sweep < TAU - t;
        vec2 q = p - (atEnd ? e : s) * r;
        a.s = atEnd ? r * sweep + dot(q, vec2(-e.y, e.x)) : -dot(q, vec2(s.y, -s.x));
        a.scale = 1.0;
        a.across = abs(dot(q, atEnd ? e : s));
    }
    return a;
}

Along circleAlong() {
    float r = vShape.x, l = length(vLocal);
    return Along(r * turnFrom(vec2(1.0, 0.0), vLocal), vDash.z, l / r, abs(l - r), vShape.y, 0.0,
                 false, false, false, false);
}

// A box with rounded corners, from the middle of its bottom side counter-clockwise: each side,
// then each quarter-circle corner, its turn times its radius.
Along roundBoxAlong() {
    vec2 p = vLocal;
    float c = vShape.z, hs = vShape.w;
    float a = vShape.x - c, b = vShape.y - c, q = 0.25 * TAU * c;
    // The outline's distance, the stroke taken off again.
    Along along = Along(0.0, vDash.z, 1.0, boxDistance() + hs, hs, 0.0, false, false, false, false);
    vec2 k = abs(p) - vec2(a, b);
    if (k.x > 0.0 && k.y > 0.0) {
        vec2 o = p - sign(p) * vec2(a, b); // from the corner's centre
        float phi = atan(o.y, o.x);
        along.scale = length(o) / c;
        if (p.y < 0.0)
            along.s = p.x > 0.0 ? a + c * (phi + 0.25 * TAU)
                                : 3.0 * a + 3.0 * q + 4.0 * b + c * (phi + 0.5 * TAU);
        else
            along.s = p.x > 0.0 ? a + q + 2.0 * b + c * phi
                                : 3.0 * a + 2.0 * q + 2.0 * b + c * (phi - 0.25 * TAU);
    } else if (k.y > k.x) {
        along.s = p.y > 0.0 ? 2.0 * a + 2.0 * q + 2.0 * b - p.x
                  : p.x >= 0.0 ? p.x
                               : 4.0 * (a + b + q) + p.x;
    } else {
        along.s = p.x > 0.0 ? a + q + b + p.y : 3.0 * a + 3.0 * q + 3.0 * b - p.y;
    }
    return along;
}

// Whether the place `at` round a shape lies inside a dash, a thousandth of a pixel in.
bool inDash(float at) {
    float into = mod(vDash.z + at, vDash.y);
    return into > 1e-3 && into < vDash.x - 1e-3;
}

// A side of a square-cornered box as a run from corner to corner, the corners `fromAt` and `toAt`
// round the outline: `s` along it from the first, `across` from it.
Along boxSide(float fromAt, float toAt, float len, float s, float across) {
    bool inA = inDash(fromAt), inB = inDash(toAt);
    return Along(s, vDash.z + fromAt, 1.0, across, vShape.w, len, !inA, !inB, inA, inB);
}

// The nearer of a dash running [x, y] along the run and `best`, as a distance outside it along
// the path: a dash cut to nothing by the run's ends is none.
float dashNearer(float x, float y, Along a, float best) {
    // The dash a corner was found in: within half a pixel of it, as float places it.
    if (a.cornerA && x <= CUT_OVERLAP)
        x = -1e30;
    if (a.cornerB && y >= a.len - CUT_OVERLAP)
        y = 1e30;
    x = a.ownA ? max(x, 0.0) : x;
    y = a.ownB ? min(y, a.len) : y;
    return x > y ? best : min(best, max(x - a.s, a.s - y));
}

// The fragment's distance from the dash nearest it along a run, its ends drawn with the cap.
float runDistance(Along a) {
    // The dash at or before the place held to the run's ends, so the dash it picks is one of the
    // run's, and the one after; the distance from them is from the place itself.
    float held = a.s;
    held = a.ownA || a.cornerA ? max(held, 0.0) : held;
    held = a.ownB || a.cornerB ? min(held, a.len) : held;
    float start = held - mod(a.phase + held, vDash.y);
    float e = 1e30;
    for (int k = 0; k < 2; k++) {
        float t0 = start + float(k) * vDash.y;
        e = dashNearer(t0, t0 + vDash.x, a, e);
    }
    return capEnd(vFlags & CANVAS_FLAG_CAP, e * a.scale, a.across, a.hw);
}

// A square-cornered box's dashes: the two sides meeting at the fragment's corner of it, each a
// run of its own as a polyline's segments are, drawn together. A corner square is the place of
// neither side alone: cut on the diagonal, a dash ending just past the corner left the half of
// the other side's body there drawn by nothing, and the outer square squeezed to one place gave
// a dash ending at the corner a grey block.
float squareBoxDistance() {
    vec2 p = vLocal;
    float a = vShape.x, b = vShape.y;
    Along along = p.y < 0.0 ? boxSide(-a, a, 2.0 * a, p.x + a, abs(p.y + b))
                            : boxSide(a + 2.0 * b, 3.0 * a + 2.0 * b, 2.0 * a, a - p.x,
                                      abs(p.y - b));
    Along up = p.x > 0.0 ? boxSide(a, a + 2.0 * b, 2.0 * b, p.y + b, abs(p.x - a))
                         : boxSide(3.0 * a + 2.0 * b, -a, 2.0 * b, b - p.y, abs(p.x + a));
    return min(runDistance(along), runDistance(up));
}

float dashDistance() {
    if (vKind == CANVAS_KIND_SEGMENT)
        return runDistance(segmentAlong());
    if (vKind == CANVAS_KIND_ARC)
        return runDistance(arcAlong());
    if (vKind == CANVAS_KIND_CIRCLE)
        return runDistance(circleAlong());
    return vShape.z > 0.0 ? runDistance(roundBoxAlong()) : squareBoxDistance();
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
        FragColor =
            canvasOutput(vOutline, CANVAS_RANK_RING, clamp(0.5 - ring, 0.0, 1.0), vFades);
        return;
    }
    FragColor = canvasOutput(vColor, vRaised > 0.5 ? CANVAS_RANK_RAISED : CANVAS_RANK_SHAPE,
                             clamp(0.5 - d, 0.0, 1.0), vFades);
}
