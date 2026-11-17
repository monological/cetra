// Tags (spec 14.39): which object a vertex is of and which class, how the view drawing it says to
// draw them, and whether this pass draws the surface at all.
//
// ONE function every program drawing a scene's meshes calls, the depth-only ones included, so a
// surface one pass drops every pass drops. A drop the prepass made and the shading pass did not
// would leave a depth with nothing shaded on it; the reverse, a surface shaded behind its own depth.
//
// The object is the vertex's own (location 4), else its instance's or its node's; the class is the
// instance's or the node's. A dropped corner is moved outside the clip volume, and a triangle's
// three corners carry one object, so a dropped surface goes whole -- and costs no fragment.

layout(location = 4) in uint aTag;

uniform uvec2 uNodeTag;          // object, class: a draw that carries one object
uniform samplerBuffer uTagTable; // TEXUNIT_TAGS: three RGBA8 texels a tag
// Which of a see-through view's surfaces this draw keeps: 0 = every one, 1 = the solid, 2 = the
// see-through. Hidden ones are dropped by all three.
uniform int uTagLane;

// ubo.h's TagViewBlock: the head -- flags, the table's count, the fade's bits -- and a row a class:
// the paint, see-through and flat as unorm16 halves of y, hidden.
layout(std140) uniform TagViewBlock {
    uvec4 uTagHead;
    uvec4 uTagClass[256];
};

#define CETRA_TAG_VIEW_ON 1u

// Outside every side of the clip volume, and the same point for all three corners: clipped, and
// degenerate besides, so a pass drawing under depth clamp draws nothing of it either.
const vec4 CETRA_TAG_DROPPED = vec4(2.0, 2.0, 2.0, 1.0);

struct CetraTag {
    bool drop;      // this pass draws nothing of the surface
    uint object;    // 0 = none
    vec4 paint;     // display sRGB, alpha its share; 0 = the material's colour
    float opacity;  // 1 = solid
    float flatShare;
    float dim;      // toward black
};

vec4 cetraTagRgba(uint c) {
    return vec4(float((c >> 24) & 255u), float((c >> 16) & 255u), float((c >> 8) & 255u),
                float(c & 255u)) / 255.0;
}

// An object's outline, display sRGB with its strength in alpha; 0 for none or no object.
vec4 cetraTagOutline(uint object) {
    if (object == 0u || object >= uTagHead.y)
        return vec4(0.0);
    return texelFetch(uTagTable, int(object) * 3 + 1);
}

// `node` is the instance's or the node's object and class: cetra_instance_tag(uNodeTag) where the
// program reads the instance block, uNodeTag where it does not.
CetraTag cetraTag(uvec2 node) {
    CetraTag t;
    t.drop = false;
    t.object = 0u;
    t.paint = vec4(0.0);
    t.opacity = 1.0;
    t.flatShare = 0.0;
    t.dim = 0.0;
    if ((uTagHead.x & CETRA_TAG_VIEW_ON) == 0u)
        return t;
    t.object = aTag != 0u ? aTag : node.x;
    bool hidden = false;
    bool lit = false;
    float seeThrough = 0.0;
    if (node.y != 0u && node.y < 256u) {
        uvec4 row = uTagClass[node.y];
        t.paint = cetraTagRgba(row.x);
        seeThrough = float(row.y & 0xFFFFu) / 65535.0;
        t.flatShare = float(row.y >> 16) / 65535.0;
        hidden = row.z != 0u;
    }
    // A tag's own colour over its class's, the canvas's rule of an object over its net.
    if (t.object != 0u && t.object < uTagHead.y) {
        int at = int(t.object) * 3;
        vec4 colour = texelFetch(uTagTable, at);
        vec4 misc = texelFetch(uTagTable, at + 2);
        if (any(greaterThan(colour, vec4(0.0))))
            t.paint = colour;
        t.dim = misc.x;
        hidden = hidden || misc.y > 0.5;
        lit = misc.w > 0.5;
    }
    float fade = lit ? 0.0 : clamp(uintBitsToFloat(uTagHead.z), 0.0, 1.0);
    t.opacity = (1.0 - seeThrough) * (1.0 - fade);
    t.drop = hidden || t.opacity <= 0.0 || (uTagLane == 1 && t.opacity < 1.0) ||
             (uTagLane == 2 && t.opacity >= 1.0);
    return t;
}
