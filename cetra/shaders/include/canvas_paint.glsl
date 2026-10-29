// What both of the 2D canvas's vertex stages share (spec 14.0): the view, the tables, and how a
// shape's colour comes through its style and its two tags.
//
// A position arrives as an int32 offset from its tile's origin, and the tile table adds the
// tile's offset from the view's integer centre, still in int32, so the first float anything
// becomes is a distance from the view.

uniform isamplerBuffer uTiles; // per tile: its origin less the view's centre, and whether it shows
uniform samplerBuffer uStyles; // a colour per style
uniform samplerBuffer uTags;   // three texels per tag: colour, outline, (dim, hidden, raised)
uniform int uTagCount;
uniform int uPass;             // canvas_pass.glsl's
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
// else the net's, else the style's, dimmed by the larger of the two dims; its outline the
// object's if it states one, else the net's, alpha 0 for none; raised when either tag raises
// it. False when either tag hides it.
bool canvasColour(uvec4 u, out vec4 colour, out vec4 outline, out float raised) {
    colour = texelFetch(uStyles, int(u.x >> 16));
    outline = vec4(0.0);
    float dim = 0.0;
    bool hidden = false;
    raised = 0.0;
    vec4 netColour = vec4(0.0), objectColour = vec4(0.0), objectOutline = vec4(0.0);
    if (u.y != 0u && u.y < uint(uTagCount)) {
        netColour = canvasTagTexel(u.y, 0);
        outline = canvasTagTexel(u.y, 1);
        vec4 m = canvasTagTexel(u.y, 2);
        dim = m.r;
        hidden = m.g > 0.5;
        raised = max(raised, m.b);
    }
    if (u.z != 0u && u.z < uint(uTagCount)) {
        objectColour = canvasTagTexel(u.z, 0);
        objectOutline = canvasTagTexel(u.z, 1);
        vec4 m = canvasTagTexel(u.z, 2);
        dim = max(dim, m.r);
        hidden = hidden || m.g > 0.5;
        raised = max(raised, m.b);
    }
    if (objectColour.a > 0.0)
        colour = objectColour;
    else if (netColour.a > 0.0)
        colour = netColour;
    if (objectOutline.a > 0.0)
        outline = objectOutline;
    colour.rgb *= 1.0 - dim;
    return !hidden;
}

// Whether an instance can paint anything in this pass (canvas_pass.glsl), so one that cannot is
// dropped before it is rasterised: the raised pass draws raised shapes alone, the solid pass none
// of them (a raised shape's solid pixels went in the raised pass), and the ring pass only shapes
// with a ring.
bool canvasInPass(float raised, float outline) {
    return !(uPass == 1 && raised < 0.5) && !(uPass == 2 && raised > 0.5) &&
           !(uPass == 4 && outline == 0.0);
}
