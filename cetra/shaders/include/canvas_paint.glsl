// What both of the 2D canvas's vertex stages share (spec 14.0): the view, the tables, and how a
// shape's colour comes through its style and its two tags.
//
// A position arrives as an int32 offset from its tile's origin, and the tile table adds the
// tile's offset from the view's integer centre, still in int32, so the first float anything
// becomes is a distance from the view.

uniform isamplerBuffer uTiles; // per tile: its origin less the view's centre, and whether it shows
uniform samplerBuffer uStyles; // a colour per style
uniform samplerBuffer uTags;   // three texels per tag: colour, outline, (dim, hidden, raised, lit)
uniform int uTagCount;
uniform int uKeepsColour;      // the layer's: a tag's colour leaves its shapes' style colours
uniform int uPass;             // canvas_pass.glsl's
uniform float uFade;           // how far a shape left out fades; 0 = none, 1 = hidden
uniform float uRoom;           // canvas_pass.glsl's
uniform vec2 uFraction;        // the view's centre past its integer unit
uniform float uPixelsPerUnit;
uniform float uMirror;         // -1 to see the canvas from the other side
uniform vec2 uSize;            // the view in framebuffer pixels

// Framebuffer pixels from the view's centre, y up.
vec2 canvasPixels(ivec2 rel, ivec2 tileOffset) {
    return (vec2(rel + tileOffset) - uFraction) * uPixelsPerUnit;
}

// Pixels from the view's centre into clip space, mirrored when seen from the other side.
vec4 canvasClip(vec2 pos) {
    pos.x *= uMirror;
    return vec4(pos / (uSize * 0.5), 0.0, 1.0);
}

vec4 canvasTagTexel(uint tag, int i) {
    return texelFetch(uTags, int(tag) * 3 + i);
}

// A shape's colour, from u = (head, net, object, tile): the object's colour if it states one,
// else the net's, else the style's -- the style's alone on a layer keeping its colour -- dimmed by
// the larger of the two dims; its outline the object's if it states one, else the net's, alpha 0
// for none; raised when either tag raises it; and how far a fading view fades it, uFade when it is
// tagged and neither tag lights it. False when either tag hides it, or the mask with no room does.
// In the lit pass it has no outline, and a shape no tag lights is false: the pass draws lit shapes
// alone.
bool canvasColour(uvec4 u, out vec4 colour, out vec4 outline, out float raised, out float fade) {
    colour = texelFetch(uStyles, int(u.x >> 16));
    outline = vec4(0.0);
    float dim = 0.0;
    bool hidden = false;
    raised = 0.0;
    float lit = 0.0;
    // Any tag but 0 is a tag; one past the table was never set, its state all zero, and the table
    // is read within its length alone.
    bool tagged = u.y != 0u || u.z != 0u;
    vec4 netColour = vec4(0.0), objectColour = vec4(0.0), objectOutline = vec4(0.0);
    if (u.y != 0u && u.y < uint(uTagCount)) {
        netColour = canvasTagTexel(u.y, 0);
        outline = canvasTagTexel(u.y, 1);
        vec4 m = canvasTagTexel(u.y, 2);
        dim = m.r;
        hidden = m.g > 0.5;
        raised = max(raised, m.b);
        lit = max(lit, m.a);
    }
    if (u.z != 0u && u.z < uint(uTagCount)) {
        objectColour = canvasTagTexel(u.z, 0);
        objectOutline = canvasTagTexel(u.z, 1);
        vec4 m = canvasTagTexel(u.z, 2);
        dim = max(dim, m.r);
        hidden = hidden || m.g > 0.5;
        raised = max(raised, m.b);
        lit = max(lit, m.a);
    }
    fade = tagged && lit == 0.0 ? uFade : 0.0;
    if (uPass == CANVAS_PASS_LIT) {
        outline = vec4(0.0);
        return lit > 0.0 && !hidden;
    }
    if (fade >= 1.0 && uRoom < 1.0)
        return false;
    if (uKeepsColour == 0)
        colour = objectColour.a > 0.0 ? objectColour : netColour.a > 0.0 ? netColour : colour;
    if (objectOutline.a > 0.0)
        outline = objectOutline;
    colour.rgb *= 1.0 - dim;
    return !hidden;
}

// Whether an instance can paint anything in this pass (canvas_pass.glsl), so one that cannot is
// dropped before it is rasterised: the raised pass draws raised shapes alone, the solid pass none
// of them (a raised shape's solid pixels went in the raised pass), and the rings' two passes only
// shapes with a ring.
bool canvasInPass(float raised, float outline) {
    return !(uPass == CANVAS_PASS_RAISED && raised < 0.5) &&
           !(uPass == CANVAS_PASS_SHAPE && raised > 0.5) &&
           !(CANVAS_PASS_RINGS(uPass) && outline == 0.0);
}
