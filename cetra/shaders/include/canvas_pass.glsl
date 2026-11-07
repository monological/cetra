// How both of the 2D canvas's fragment stages write a layer (spec 14.0). A layer resolves in the
// view's target in passes over the same geometry, the target's alpha its scratch:
//
//   0  coverage, the LARGEST any shape gives a pixel, times the layer's opacity, into alpha
//      alone. The largest and not the union, because a shape cut at a tile edge, and a polygon's
//      outline fringe at a corner, cover one pixel twice with the same coverage: max is
//      idempotent there, and over shapes of one layer it is the coverage of the nearest edge.
//   1  the colour of shapes a tag raises, where they cover half a pixel or more,
//   2  then of the rest where they cover half a pixel or more,
//   3  then of every shape where less,
//   4  then outline rings: each paints by the pixel's alpha and zeroes it, so the first shape to
//      reach a pixel spends the whole coverage and overlaps add nothing.
// Pass 1 is drawn only while a tag raises and pass 4 only while one draws a ring.
//
// A view that fades (spec 14.25) fades each shape it leaves out as the shape is drawn, so what lies
// over it is never faded with it. Below a fade of 1 its colour goes toward the view's colour, and
// it covers what lies under it as any shape does; at 1, the mask, its coverage goes instead, so
// what lies under it shows. Within uRoom of a lit shape it fades less, down to nothing at the
// light: the lit pass, CANVAS_PASS_LIT, drawn first and only while there is room, puts every lit
// shape's coverage into a target of its own by max, which the taps below read.
//
// The order is what decides which colour a pixel shared by two shapes takes: a raised one, else
// one that covers it well rather than an edge's fringe, and a ring last of all. A raised shape's
// fringe is a fringe like any other, or its edge would move half a pixel out over a neighbour.
// A ring is not a distance that falls away from its shape, so where it crosses another shape of
// the layer -- the next segment of a picked trace at a joint, a polygon's own inside at a reflex
// corner -- that shape must win, and it has painted and spent the pixel by then.

uniform int uPass;
uniform float uOpacity; // the layer's
uniform float uFade;    // how far a shape left out fades away from a light; 0 = none
uniform vec3 uFadeTo;   // toward this colour
uniform float uRoom;    // framebuffer pixels round a light the fading eases off over; 0 = none
uniform sampler2D uLit; // the lit pass's: a lit shape's coverage, while there is room

const int CANVAS_RANK_RAISED = 1;
const int CANVAS_RANK_SHAPE = 2;
const int CANVAS_RANK_FRINGE = 3; // any shape where it covers less than half the pixel
const int CANVAS_RANK_RING = 4;

// A tap reads the lit target at a level as wide as its ring's spacing, which averages a light
// thinner than that down; so a quarter of a tap's footprint lit counts as wholly lit.
const float CANVAS_TAP_GAIN = 4.0;

// How lit the fragment's pixel is, from the light on and round it: 1 on a light; within uRoom of
// one, easing to 0 at uRoom, by the largest of CANVAS_FADE_RINGS rings of CANVAS_FADE_SPOKES taps
// round it; 0 with no room.
float canvasLight() {
    if (uRoom < 1.0)
        return 0.0;
    const float tau = 6.283185307179586;
    vec2 size = vec2(textureSize(uLit, 0));
    vec2 uv = gl_FragCoord.xy / size;
    float lit = texelFetch(uLit, ivec2(gl_FragCoord.xy), 0).r;
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
    return lit;
}

// A fragment of a shape in the pass being drawn: its colour and its rank (CANVAS_RANK_*), its
// coverage, and whether a fading view fades it (canvasColour's fades).
vec4 canvasOutput(vec4 colour, int rank, float coverage, float fades) {
    if (uPass != CANVAS_PASS_LIT && fades > 0.0 && uFade > 0.0) {
        float fade = uFade * (1.0 - canvasLight());
        if (uFade >= 1.0)
            coverage *= 1.0 - fade;
        else
            colour.rgb = mix(colour.rgb, uFadeTo, fade);
    }
    if (coverage <= 0.0)
        discard;
    if (uPass == 0)
        return vec4(0.0, 0.0, 0.0, coverage * colour.a * uOpacity);
    if (uPass == CANVAS_PASS_LIT)
        return vec4(coverage, 0.0, 0.0, 0.0);
    if (rank != CANVAS_RANK_RING && coverage < 0.5)
        rank = CANVAS_RANK_FRINGE;
    if (rank != uPass)
        discard;
    return vec4(colour.rgb, 0.0);
}
