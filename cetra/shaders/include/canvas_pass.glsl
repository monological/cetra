// How both of the 2D canvas's fragment stages write a layer (spec 14.0). A layer resolves in the
// view's target in passes over the same geometry, the target's alpha its scratch, in the order of
// their numbers (CANVAS_PASS_*); a fragment's rank is the pass that paints it:
//
//   COVERAGE  the LARGEST coverage any shape gives a pixel, times the layer's opacity, into alpha
//             alone. The largest and not the union, because a shape cut at a tile edge, and a
//             polygon's outline fringe at a corner, cover one pixel twice with the same coverage:
//             max is idempotent there, and over shapes of one layer it is the coverage of the
//             nearest edge.
//   RAISED    the colour of shapes a tag raises, where they cover half a pixel or more,
//   SHAPE     then of the rest where they cover half a pixel or more,
//   FRINGE    then of every shape where less,
//   NEAREST   then the least of every ringed shape's inner ramp into alpha by min (spec 14.36).
//             COVERAGE left there the largest ring any shape gives the pixel; one ramp rising and
//             the other falling with distance, the least inner one and the most outer one are the
//             nearest shape's, so what is left is the ring of the nearest ringed shape: a
//             segment's ring in the gap just outside the next one at a joint goes, and two ringed
//             shapes nearer than twice the ring's gap share one outline. Its coverage is the
//             nearest's; where two colours of ring meet, RING's order still picks the colour.
//   RING      then outline rings: each paints by the pixel's alpha and zeroes it, so the first
//             shape to reach a pixel spends the whole coverage and overlaps add nothing.
// RAISED is drawn only while a tag raises, and NEAREST and RING only while one draws a ring.
//
// A view that fades (spec 14.25) fades each shape it leaves out as the shape is drawn, so what lies
// over it is never faded with it. Below a fade of 1 its colour goes toward the view's colour, and
// it covers what lies under it as any shape does; at 1, the mask, its coverage goes instead, so
// what lies under it shows. Within uRoom of a lit shape it fades less, down to nothing at the
// light's edge, though never over the light itself: the lit pass, CANVAS_PASS_LIT, drawn first and
// only while there is room, puts every lit shape's coverage into a target of its own by max, which
// the taps below read.
//
// The order is what decides which colour a pixel shared by two shapes takes: a raised one, else
// one that covers it well rather than an edge's fringe, and a ring last of all. A raised shape's
// fringe is a fringe like any other, or its edge would move half a pixel out over a neighbour.
// A ring is not a distance that falls away from its shape, so where it crosses another shape of
// the layer -- the next segment of a picked trace at a joint, a polygon's own inside at a reflex
// corner -- that shape must win, and it has painted and spent the pixel by then.

uniform int uPass;
uniform float uOpacity; // the layer's
uniform vec3 uFadeTo;   // the colour a shape left out fades toward
uniform float uRoom;    // framebuffer pixels round a light the fading eases off over; 0 = none
uniform sampler2D uLit; // the lit pass's: a lit shape's coverage, while there is room

// A tap reads the lit target at a level as wide as its ring's spacing, which averages a light
// thinner than that down; so a quarter of a tap's footprint lit counts as wholly lit.
const float CANVAS_TAP_GAIN = 4.0;

// How much the room spares a shape left out at the fragment's pixel: within uRoom of a light, easing
// to 0 at uRoom, by the largest of CANVAS_FADE_RINGS rings of CANVAS_FADE_SPOKES taps round it. 0
// where a light covers the pixel itself: a shape left out over a lit one is never spared by it,
// which the lit target, holding coverage alone, could not tell from a light beside it. 0 with no
// room.
float canvasLight() {
    if (uRoom < 1.0)
        return 0.0;
    float here = texelFetch(uLit, ivec2(gl_FragCoord.xy), 0).r;
    if (here >= 1.0)
        return 0.0;
    const float tau = 6.283185307179586;
    vec2 size = vec2(textureSize(uLit, 0));
    vec2 uv = gl_FragCoord.xy / size;
    float lit = 0.0;
    for (int r = 1; r <= CANVAS_FADE_RINGS && lit < 1.0; r++) {
        float radius = uRoom * float(r) / float(CANVAS_FADE_RINGS);
        float lod = log2(max(tau * radius / float(CANVAS_FADE_SPOKES), 1.0));
        float inner = uRoom * float(r - 1) / float(CANVAS_FADE_RINGS);
        float falloff = 1.0 - smoothstep(0.0, uRoom, inner);
        for (int s = 0; s < CANVAS_FADE_SPOKES; s++) {
            float a = tau * (float(s) + 0.5 * float(r & 1)) / float(CANVAS_FADE_SPOKES);
            vec2 at = uv + radius * vec2(cos(a), sin(a)) / size;
            float v = clamp(textureLod(uLit, at, lod).r * CANVAS_TAP_GAIN, 0.0, 1.0);
            lit = max(lit, v * falloff);
        }
    }
    return lit * (1.0 - here);
}

// A fragment of a shape in the pass being drawn: its colour and the pass that paints it
// (CANVAS_PASS_RAISED, _SHAPE or _RING), its coverage, and how far a fading view fades it
// (canvasColour's fade). The mask takes coverage, in every pass alike; a lesser fade takes colour,
// only in the pass that paints.
vec4 canvasOutput(vec4 colour, int paints, float coverage, float fade) {
    if (fade >= 1.0)
        coverage *= canvasLight();
    if (coverage <= 0.0)
        discard;
    if (uPass == CANVAS_PASS_COVERAGE)
        return vec4(0.0, 0.0, 0.0, coverage * colour.a * uOpacity);
    if (uPass == CANVAS_PASS_LIT)
        return vec4(coverage, 0.0, 0.0, 0.0);
    if (paints != CANVAS_PASS_RING && coverage < 0.5)
        paints = CANVAS_PASS_FRINGE;
    if (paints != uPass)
        discard;
    if (fade > 0.0 && fade < 1.0)
        colour.rgb = mix(colour.rgb, uFadeTo, fade * (1.0 - canvasLight()));
    return vec4(colour.rgb, 0.0);
}

// A ringed shape's fragment in NEAREST: its ring's inner ramp, scaled as COVERAGE scales the ring,
// by its outline's alpha and the layer's opacity, and where the mask hides the shape taken as far
// as can be, so a shape hidden cuts no other's ring. Never discarded: a 0 is what it gives.
vec4 canvasNearest(float inner, float alpha, float fade) {
    if (fade >= 1.0)
        inner = 1.0 - (1.0 - inner) * canvasLight();
    return vec4(0.0, 0.0, 0.0, inner * alpha * uOpacity);
}
