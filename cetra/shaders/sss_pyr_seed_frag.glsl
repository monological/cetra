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
// WHICH profile is not decided here. The caller sets a stencil test, so only this profile's
// pixels reach this shader at all, and a coarse texel can still only ever hold one profile's
// light -- the property this test used to provide by comparing srcTex's alpha.
//
// It stopped being able to. A tag is categorical and an MSAA colour resolve is a box filter,
// so a partly covered pixel arrived holding the MEAN of this profile's tag and its neighbours'
// -- and the mean of tag 2 and the cleared 0 is 1, a valid and wrong tag. At one sample there
// was nothing to average and the compare was exact; above one it misfiled silhouette pixels,
// which is most of a thin subject. Stencil is integer and per sample, so it has no mean to
// take, and it cannot be read here either way: GL 4.1 has no stencil texturing
// (ARB_stencil_texturing is 4.3), which is why the test is fixed-function rather than a fetch.
uniform sampler2D srcTex;   // resolved attachment 4: skin diffuse
uniform sampler2D auxTex;   // .z = linear view Z, negative in front, 0 = sky
uniform sampler2D depthTex; // resolved scene depth, NDC
uniform mat4 projection;    // viewZFromNdcZ reads this by name

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
    // onSurface >= 0 is sky or an uncovered pixel, and it contributes nothing rather than
    // black, because coverage 0 removes it from the average entirely. Pixels belonging to
    // another profile never arrive: the stencil test rejects them before this shader runs.
    /*
     * COVERAGE comes out of the resolve, and declaring it 1.0 is what put light in the frame
     * that nothing emitted.
     *
     * srcTex is an MSAA resolve, so a partly covered pixel arrives with its rgb averaged
     * against the uncovered samples' zeros -- already premultiplied by coverage -- and its
     * alpha averaged the same way. Since the scene pass writes 1 on skin and 0 elsewhere, that
     * alpha IS the covered fraction: k of n samples resolves to k/n.
     *
     * Declaring it 1.0 instead, which this did while alpha still carried the profile tag, says
     * a blade covering half a pixel is fully covered while it carries half a blade's radiance.
     * The composite is hdr + blur - D: D is the dimmed value, blur a neighbourhood at full
     * brightness, and the difference -- about (1 - coverage) of the true radiance -- was ADDED
     * as light. It grows with the scatter radius, because a wider kernel reaches more fully
     * covered neighbours, which is why the radius slider appeared to control it.
     *
     * rgb stays premultiplied: the resolve already did it, and the pyramid is built on
     * premultiplied colour with coverage alongside precisely so a partly covered texel can be
     * divided back out at the gather instead of darkening toward the black around it.
     */
    /*
     * A CEILING on what may enter the pyramid, and it is the fix for the blown-out blocks.
     *
     * Attachment 4 is clamped at WS_SCENE_MAX -- sixteen stops above white -- which is a guard
     * against fp16 overflow, not a statement about plausible diffuse. One fragment with a
     * runaway value therefore puts up to 60000 into a texel, and every coarse level averages it
     * into every pixel whose kernel reaches that far. What comes out is a hard-edged block of
     * this profile's colour, sized by whichever level the scatter radius selected, which is why
     * the radius appears to control it.
     *
     * The bound belongs HERE rather than at the gather. Clamping the gather's result against
     * the local diffuse collapses at a terminator, where the centre is near black and the
     * neighbourhood is legitimately far brighter -- that is the case scatter exists to serve,
     * and clamping it moved 461,815 px of the skin fixture. Clamping the INPUT leaves the
     * gather's arithmetic untouched and only refuses values no diffuse surface can have.
     *
     * 64 is six stops above white: brighter than any lit skin or foliage in a pre-exposed
     * frame, and four orders below the outlier it exists to catch.
     */
    const float SSS_MAX_DIFFUSE = 2.0;

    bool covered = onSurface < 0.0;
    ColorOut = covered ? vec4(min(d.rgb, vec3(SSS_MAX_DIFFUSE)), d.a) : vec4(0.0);
    DepthOut = covered ? -z : 0.0;
}
