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

float segmentDistance() {
    float len = vShape.x, hw = vShape.y;
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
    return length(q) - hw;
}

// The outer half of a fill's edge ramp: a segment with no width, on the side away from the
// polygon only, which the fill's own ramp stops at; past an end, the distance to the polygon's
// corner there.
float edgeDistance() {
    if (vLocal.y < 0.0)
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

float boxDistance() {
    vec2 halfSize = vShape.xy;
    float corner = vShape.z, hs = vShape.w;
    vec2 q = abs(vLocal) - halfSize + corner;
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - corner;
    return hs > 0.0 ? abs(d) - hs : d;
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
    // An outline is a ring uRing.x to uRing.y pixels outside the shape. Its inner ramp starts at
    // least half a pixel out, where the shape's own ends, so a pixel is the shape's or the ring's
    // and never both.
    if (vOutline.a > 0.0 && d > 0.5) {
        float ring = max(uRing.x - d, d - uRing.y);
        FragColor = canvasOutput(vOutline, CANVAS_RANK_RING, clamp(0.5 - ring, 0.0, 1.0));
        return;
    }
    FragColor = canvasOutput(vColor, vRaised > 0.5 ? CANVAS_RANK_RAISED : CANVAS_RANK_SHAPE,
                             clamp(0.5 - d, 0.0, 1.0));
}
