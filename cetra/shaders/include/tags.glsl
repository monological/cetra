// Tags (spec 14.39): which object a vertex is of and which class, how the view drawing it says to
// draw them, and whether this pass draws the surface at all.
//
// ONE function every program drawing a scene's meshes calls, the depth-only ones included, so a
// surface one pass drops every pass drops. A drop the prepass made and the shading pass did not
// would leave a depth with nothing shaded on it; the reverse, a surface shaded behind its own depth.
// A program wanting only the drop calls cetraTagPosition; one shading the surface, with
// CETRA_TAG_VARYINGS defined before the include, calls cetraTagShade.
//
// The object is the vertex's own (location 4), else its instance's or its node's; the class is the
// instance's or the node's. A dropped corner is moved outside the clip volume, and a triangle's
// three corners carry one object, so a dropped surface goes whole -- and costs no fragment.

#include "tag_constants.glsl"

layout(location = 4) in uint aTag;

uniform uvec2 uNodeTag;          // object, class: a draw that carries one object
uniform samplerBuffer uTagTable; // TEXUNIT_TAGS: TAG_TABLE_TEXELS RGBA8 texels a tag
uniform int uTagLane;            // TAG_LANE_*

// ubo.h's TagViewBlock: the head -- TAG_VIEW_* flags, the table's count, the fade's bits, clamped
// -- and a row a class: the paint, see-through and flat as unorm16 halves of y, hidden.
layout(std140) uniform TagViewBlock {
    uvec4 uTagHead;
    uvec4 uTagClass[TAG_VIEW_CLASSES];
};

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

// Whether the table holds a row for `object`; past its count every object is drawn as it is.
bool cetraTagInTable(uint object) {
    return object != 0u && object < uTagHead.y;
}

// An object's outline, display sRGB with its strength in alpha; 0 for none or no object.
vec4 cetraTagOutline(uint object) {
    return cetraTagInTable(object)
               ? texelFetch(uTagTable, int(object) * TAG_TABLE_TEXELS + TAG_TEXEL_OUTLINE)
               : vec4(0.0);
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
    if ((uTagHead.x & TAG_VIEW_ON) == 0u)
        return t;
    t.object = aTag != 0u ? aTag : node.x;
    bool hidden = false;
    bool lit = false;
    float seeThrough = 0.0;
    if (node.y != 0u) {
        uvec4 row = uTagClass[node.y];
        t.paint = cetraTagRgba(row.x);
        seeThrough = float(row.y & 0xFFFFu) / 65535.0;
        t.flatShare = float(row.y >> 16) / 65535.0;
        hidden = row.z != 0u;
    }
    // A tag's own colour over its class's, the canvas's rule of an object over its net.
    if (cetraTagInTable(t.object)) {
        int at = int(t.object) * TAG_TABLE_TEXELS;
        vec4 colour = texelFetch(uTagTable, at + TAG_TEXEL_COLOUR);
        vec4 misc = texelFetch(uTagTable, at + TAG_TEXEL_MISC);
        if (any(greaterThan(colour, vec4(0.0))))
            t.paint = colour;
        t.dim = misc.x;
        hidden = hidden || misc.y > 0.5;
        lit = misc.w > 0.5;
    }
    float fade = lit ? 0.0 : uintBitsToFloat(uTagHead.z);
    t.opacity = (1.0 - seeThrough) * (1.0 - fade);
    t.drop = hidden || t.opacity <= 0.0 || (uTagLane == TAG_LANE_SOLID && t.opacity < 1.0) ||
             (uTagLane == TAG_LANE_GHOSTS && t.opacity >= 1.0);
    return t;
}

// `clip`, or CETRA_TAG_DROPPED where this pass draws nothing of the surface. A view that drops
// nothing in any pass is answered without the table, which is the common case for the depth-only
// programs that call this: a view painting or outlining, hiding nothing.
vec4 cetraTagPosition(uvec2 node, vec4 clip) {
    if ((uTagHead.x & TAG_VIEW_DROPS) == 0u)
        return clip;
    return cetraTag(node).drop ? CETRA_TAG_DROPPED : clip;
}

#ifdef CETRA_TAG_VARYINGS
// What the view says of the surface's tag, constant across it: the paint, display sRGB with its
// share in alpha; its opacity, flat share and dim; its object, for the tags render mode.
flat out vec4 TagPaint;
flat out vec3 TagMisc;
flat out uint TagObject;

// The varyings filled, and `clip` dropped as cetraTagPosition drops it.
vec4 cetraTagShade(uvec2 node, vec4 clip) {
    CetraTag tag = cetraTag(node);
    TagPaint = tag.paint;
    TagMisc = vec3(tag.opacity, tag.flatShare, tag.dim);
    TagObject = tag.object;
    return tag.drop ? CETRA_TAG_DROPPED : clip;
}
#endif
