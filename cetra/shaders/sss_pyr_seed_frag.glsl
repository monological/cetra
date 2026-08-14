#version 330 core
in vec2 TexCoords;
layout(location = 0) out vec4 ColorOut; // coverage-premultiplied diffuse, a = coverage
layout(location = 1) out float DepthOut; // coverage-weighted view depth (positive)

// Level 0 of the scatter pyramid, for ONE profile.
//
// Every later level is a plain weighted average of this one, which is what lets
// the whole chain stay linear: carrying coverage alongside premultiplied colour
// means a partially covered coarse texel can be divided back out at the gather
// instead of darkening toward the uncovered black around it.
//
// The profile test lives here rather than in the gather, so that a coarse texel can only
// ever hold one profile's light and no FILTERED tag is compared while walking the pyramid.
//
// What it does NOT survive is the resolve that produces srcTex. A tag is categorical and an
// MSAA colour resolve is a box filter, so a partly covered pixel arrives holding the mean of
// this profile's tag and its neighbours' -- and the mean of tag 2 and the cleared 0 is a
// valid, wrong tag. At one sample there is nothing to average and the test is exact; above
// one it misfiles silhouette pixels, which is most of a thin subject. Fixing that needs the
// tag somewhere per-sample, i.e. stencil (spec 11.37 phase 2).
uniform sampler2D srcTex;   // resolved attachment 4: skin diffuse, a = profile tag
uniform sampler2D auxTex;   // .z = linear view Z, negative in front, 0 = sky
uniform sampler2D depthTex; // resolved scene depth, NDC
uniform mat4 projection;    // viewZFromNdcZ reads this by name
uniform int profileTag;     // this walk's profile index + 1, matching the alpha

#include "depth.glsl"

void main()
{
    vec4 d = texture(srcTex, TexCoords);
    /*
     * Depth from the DEPTH buffer, not from the aux buffer's linear Z.
     *
     * Both hold this surface's view Z, but they resolve differently: a depth blit selects a
     * sample and a colour blit averages. Averaged against the cleared value a silhouette
     * pixel reads far nearer than it is, and the gather divides its blur radius by exactly
     * this number -- so the error arrives as a blur several pyramid levels too wide, moving
     * frame to frame with coverage.
     *
     * aux .z is still the on-surface test. It is a mean here too, but the question it answers
     * is "is there geometry at all", where a soft edge costs a fringe rather than a wrong
     * answer.
     */
    float onSurface = texture(auxTex, TexCoords).z;
    float z = viewZFromNdcZ(texture(depthTex, TexCoords).r * 2.0 - 1.0);
    // onSurface >= 0 is sky or an uncovered pixel; a different tag belongs to another
    // profile's walk. Both contribute nothing rather than black, because
    // coverage 0 removes them from the average entirely.
    bool covered = onSurface < 0.0 && int(d.a + 0.5) == profileTag;
    ColorOut = covered ? vec4(d.rgb, 1.0) : vec4(0.0);
    DepthOut = covered ? -z : 0.0;
}
