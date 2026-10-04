#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Split spec-occ composite (spec 11.4): the scene pass routed ambient
// specular to its own buffer, so occlusion can finally multiply exactly what
// it models -- the cone term on the specular share, plain AO on everything
// else -- instead of guessing a per-pixel specular fraction. One blended
// pass, the fog fold's idiom: this outputs (spec * SO, aoFactor) and the
// (GL_ONE, GL_SRC_ALPHA) blend forms spec * SO + scene * aoFactor in place.
// The tonemap's own AO share collapses to 1 in split mode; its
// contact-shadow fold stays (direct light, independent of ambient
// occlusion). Runs before the TAA resolve so the reunited frame is
// stabilized as one image.

#include "split_occlusion.glsl"

void main()
{
    FragColor = splitOcclusionAt(TexCoords);
}
