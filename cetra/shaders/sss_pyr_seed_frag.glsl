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
// The exact-integer profile test lives here rather than in the gather. Doing it
// once at full resolution is what makes cross-profile separation STRUCTURAL --
// a coarse texel can only ever contain this profile's light, so no filtered tag
// is ever compared, and an averaged tag can never select the wrong profile.
uniform sampler2D srcTex; // resolved attachment 4: skin diffuse, a = profile tag
uniform sampler2D auxTex; // .z = linear view Z, negative in front, 0 = sky
uniform int profileTag;   // this walk's profile index + 1, matching the alpha

void main()
{
    vec4 d = texture(srcTex, TexCoords);
    float z = texture(auxTex, TexCoords).z;
    // z >= 0 is sky or an uncovered pixel; a different tag belongs to another
    // profile's walk. Both contribute nothing rather than black, because
    // coverage 0 removes them from the average entirely.
    bool covered = z < 0.0 && int(d.a + 0.5) == profileTag;
    ColorOut = covered ? vec4(d.rgb, 1.0) : vec4(0.0);
    DepthOut = covered ? -z : 0.0;
}
