#version 330 core

// The outline mask (spec 14.39): an outlined surface's colour, and in alpha its outline's strength
// where this pass draws it -- the whole outline where it is seen, the view's hidden share where
// something solid stands in front. Never quite zero, which is what says no surface.

flat in vec4 OutlineColour;

uniform float uStrength; // 1 for the seen pass, outline_hidden for the hidden one

out vec4 Mask;

void main()
{
    Mask = vec4(OutlineColour.rgb, max(OutlineColour.a * uStrength, 1.0 / 255.0));
}
