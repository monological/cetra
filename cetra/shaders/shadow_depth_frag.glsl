#version 330 core

// centroid, because this stage pairs with pbr_vert and the qualifier is part of the interface:
// a mismatch fails the link rather than shading differently. See pbr_vert for what it prevents.
in vec2 TexCoords;
centroid in vec4 VertexColor;

// Alpha-tested depth for foliage (material.h foliage_shadows). Off by default:
// opaque geometry writes depth with no texture fetch at all, exactly as before.
uniform sampler2D albedoTex;
uniform int alphaTested;
uniform float alphaCutoff;
uniform int vertexColorExists;

// KHR_texture_transform, the same three the shading pass applies. This stage
// used to sample raw TexCoords, which made it the fifth hand-written copy of the
// alpha decision and the only one still diverging: a leaf whose material carries
// a texture transform sampled a DIFFERENT texel here than when it was shaded, so
// it cast a shadow of the wrong shape. The transform is cheap and unconditional
// -- an identity transform costs a sin, a cos and a multiply-add.
uniform vec2 uvOffset;
uniform vec2 uvScale;
uniform float uvRotation;

#include "alpha_coverage.glsl"

// Deliberately NOT the whole of pbr_frag's chain, and the two omissions are
// different in kind.
//
// The POM march is skipped because it is meaningless here: it offsets UVs along
// the TANGENT-SPACE VIEW direction, and this pass has no view -- it has a light.
// Marching it against the light would be a different silhouette from the one the
// camera sees, which is worse than not marching it.
//
// The mask-array opacity layer is skipped because pbr_frag does not use it for
// the DISCARD either. It multiplies into coverage after the cutoff test, so it
// shapes alpha-to-coverage and never decides whether a fragment exists.
void main()
{
    if (alphaTested == 1) {
        float s = sin(uvRotation);
        float c = cos(uvRotation);
        vec2 rotated = vec2(TexCoords.x * c - TexCoords.y * s,
                            TexCoords.x * s + TexCoords.y * c);
        vec2 uv = rotated * uvScale + uvOffset;

        float alpha = texture(albedoTex, uv).a;
        if (vertexColorExists > 0)
            alpha *= VertexColor.a;
        // Through the shared rule, with a2c 0: a shadow map is single-sampled,
        // so the binary branch is the only one that applies and this is exactly
        // the `alpha < alphaCutoff` it replaces. Stated once anyway -- the
        // camera pass diverged from this cutoff for two specs, and it is the
        // divergence rather than the value that made a leaf and its own shadow
        // different shapes.
        if (alphaMaskCoverage(alpha, alphaCutoff, 0) < 0.5)
            discard; // leaf cutout: the gaps between leaves must let light through
    }

    // Depth is written automatically to gl_FragDepth
}
