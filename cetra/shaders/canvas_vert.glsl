#version 330 core

// The 2D canvas's shapes (spec 14.0): one instance of a unit quad per record, laid out as
// canvas_record.h states. A record's points are int32 offsets from its tile's origin, and the tile
// table adds that origin's offset from the view's integer centre still in int32, so the first
// float anything becomes is a distance from the view.
//
// From there everything is in framebuffer pixels from the view's centre, y up. The quad is the
// shape's extent padded by a pixel, and vLocal is the fragment's place in the shape's own frame,
// interpolated from the corners: the fragment stage measures with small numbers however far the
// shape's other end is.

#include "canvas_constants.glsl"

layout(location = 0) in ivec4 aP;
layout(location = 1) in vec4 aF;
layout(location = 2) in uvec4 aU;

uniform isamplerBuffer uTiles; // per tile: its origin less the view's centre, and whether it shows
uniform samplerBuffer uStyles; // a colour per style
uniform samplerBuffer uTags;   // three texels per tag: colour, outline, (dim, hidden, raised)
uniform int uTagCount;
uniform vec2 uFraction;        // the view's centre past its integer unit
uniform float uPixelsPerUnit;
uniform float uMirror;         // -1 to see the canvas from the other side
uniform vec2 uSize;            // the view in framebuffer pixels
uniform float uShapeMinPx;

flat out uint vKind;
flat out uint vFlags;
flat out vec4 vColor; // premultiplied
flat out vec4 vShape; // the kind's measures, in pixels
flat out vec4 vEnds;  // an arc's sector: from xy counter-clockwise to zw
out vec2 vLocal;

const float PAD = 1.0; // the outer half of the edge ramp
const float TAU = 6.283185307179586;

vec2 toPixels(ivec2 rel, ivec2 tileOffset) {
    return (vec2(rel + tileOffset) - uFraction) * uPixelsPerUnit;
}

float cross2(vec2 a, vec2 b) {
    return a.x * b.y - a.y * b.x;
}

// How far counter-clockwise from s the direction p lies, in [0, TAU).
float turnFrom(vec2 s, vec2 p) {
    float t = atan(cross2(s, p), dot(s, p));
    return t < 0.0 ? t + TAU : t;
}

vec4 tagTexel(uint tag, int i) {
    return texelFetch(uTags, int(tag) * 3 + i);
}

void main() {
    uint kind = aU.x & 63u;
    uint flags = (aU.x >> 6) & 1023u;
    uint style = aU.x >> 16;
    vKind = kind;
    vFlags = flags;
    vColor = vec4(0.0);
    vShape = vec4(0.0);
    vEnds = vec4(0.0);
    vLocal = vec2(0.0);
    // Outside every clip plane: a quad of four such corners draws nothing.
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);

    ivec4 tile = texelFetch(uTiles, int(aU.w));
    if (kind == CANVAS_KIND_NONE || tile.z == 0)
        return;

    // The colour, through the shape's two tags: the object's colour if it states one, else the
    // net's, else the style's.
    vec4 colour = texelFetch(uStyles, int(style));
    float dim = 0.0;
    bool hidden = false;
    vec4 netColour = vec4(0.0), objectColour = vec4(0.0);
    if (aU.y != 0u && int(aU.y) < uTagCount) {
        netColour = tagTexel(aU.y, 0);
        vec4 m = tagTexel(aU.y, 2);
        dim = m.r;
        hidden = m.g > 0.5;
    }
    if (aU.z != 0u && int(aU.z) < uTagCount) {
        objectColour = tagTexel(aU.z, 0);
        vec4 m = tagTexel(aU.z, 2);
        dim = max(dim, m.r);
        hidden = hidden || m.g > 0.5;
    }
    if (hidden)
        return;
    if (objectColour.a > 0.0)
        colour = objectColour;
    else if (netColour.a > 0.0)
        colour = netColour;
    colour.rgb *= 1.0 - dim;
    vColor = vec4(colour.rgb * colour.a, colour.a);

    // The quad's corners as a strip: (0, 0), (1, 0), (0, 1), (1, 1).
    vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vec2 pos;
    float span;

    if (kind == CANVAS_KIND_SEGMENT) {
        vec2 a = toPixels(aP.xy, tile.xy);
        vec2 b = toPixels(aP.zw, tile.xy);
        float hw = max(aF.x * uPixelsPerUnit, 0.5); // never narrower than a pixel
        vec2 d = b - a;
        float len = length(d);
        vec2 dir = len > 1e-6 ? d / len : vec2(1.0, 0.0);
        vec2 n = vec2(-dir.y, dir.x);
        bool roundA = (flags & (CANVAS_FLAG_FLAT_A | CANVAS_FLAG_CUT_A)) == 0u;
        bool roundB = (flags & (CANVAS_FLAG_FLAT_B | CANVAS_FLAG_CUT_B)) == 0u;
        float along = c.x == 0.0 ? -(roundA ? hw : 0.0) - PAD : len + (roundB ? hw : 0.0) + PAD;
        float across = (c.y * 2.0 - 1.0) * (hw + PAD);
        pos = a + dir * along + n * across;
        vLocal = vec2(along, across);
        vShape = vec4(len, hw, 0.0, 0.0);
        span = len + 2.0 * hw;
    } else if (kind == CANVAS_KIND_CIRCLE) {
        vec2 centre = toPixels(aP.xy, tile.xy);
        float r = float(aP.z) * uPixelsPerUnit;
        float hs = aP.w > 0 ? max(float(aP.w) * uPixelsPerUnit * 0.5, 0.5) : 0.0;
        vLocal = (c * 2.0 - 1.0) * (r + hs + PAD);
        pos = centre + vLocal;
        vShape = vec4(r, hs, 0.0, 0.0);
        span = 2.0 * (r + hs);
    } else if (kind == CANVAS_KIND_BOX) {
        vec2 centre = toPixels(aP.xy, tile.xy);
        vec2 halfSize = aF.xy * uPixelsPerUnit;
        float corner = min(aF.z * uPixelsPerUnit, min(halfSize.x, halfSize.y));
        float hs = aP.z > 0 ? max(float(aP.z) * uPixelsPerUnit * 0.5, 0.5) : 0.0;
        vec2 u = vec2(cos(aF.w), sin(aF.w));
        vec2 v = vec2(-u.y, u.x);
        vLocal = (c * 2.0 - 1.0) * (halfSize + hs + PAD);
        pos = centre + u * vLocal.x + v * vLocal.y;
        vShape = vec4(halfSize, corner, hs);
        span = 2.0 * (max(halfSize.x, halfSize.y) + hs);
    } else if (kind == CANVAS_KIND_ARC) {
        vec2 a = toPixels(aP.xy, tile.xy);
        vec2 b = toPixels(aP.zw, tile.xy);
        vec2 centre = (a + b) * 0.5 + aF.yz * uPixelsPerUnit;
        float r = aF.w * uPixelsPerUnit;
        float hw = max(aF.x * uPixelsPerUnit, 0.5);
        // The sector runs counter-clockwise from s to e, so a clockwise arc is its ends swapped,
        // and so are their flags: A is the sector's start from here on.
        bool clockwise = (flags & CANVAS_FLAG_CLOCKWISE) != 0u;
        vec2 da = normalize(a - centre), db = normalize(b - centre);
        vec2 s = clockwise ? db : da;
        vec2 e = clockwise ? da : db;
        if (clockwise) {
            uint ends = flags & (CANVAS_FLAG_FLAT_A | CANVAS_FLAG_CUT_A);
            uint other = flags & (CANVAS_FLAG_FLAT_B | CANVAS_FLAG_CUT_B);
            vFlags = (flags & ~(CANVAS_FLAG_FLAT_A | CANVAS_FLAG_FLAT_B | CANVAS_FLAG_CUT_A |
                                CANVAS_FLAG_CUT_B)) |
                     (ends << 1) | (other >> 1);
        }
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
        float grow = hw + PAD;
        vLocal = mix(lo - grow, hi + grow, c);
        pos = centre + vLocal;
        vShape = vec4(r, hw, sweep, 0.0);
        vEnds = vec4(s, e);
        span = max(hi.x - lo.x, hi.y - lo.y) + 2.0 * hw;
    } else {
        return;
    }

    if (span < uShapeMinPx)
        return;
    pos.x *= uMirror;
    gl_Position = vec4(pos / (uSize * 0.5), 0.0, 1.0);
}
