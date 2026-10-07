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

// A surface hook's caster (spec 13.29): its alpha cuts the shadow as it cuts the surface. Only
// the alpha is kept here, so the hook is handed what this stage has -- its coordinate, its
// world position and its vertex colour -- and placeholders for what it does not: no eye, so one
// straight above (a view direction and normal of +Y), and white for the albedo. A material
// carrying no albedo map is cut by the hook alone.
#ifdef CETRA_SURFACE_HOOK
in vec3 HookWorldPos;
uniform float time;
uniform int tsmHasAlbedo;
#include "surface_hook.glsl"
// CETRA_SURFACE_HOOK_CHUNK
#endif

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

#ifdef CETRA_SURFACE_HOOK
        float alpha = tsmHasAlbedo > 0 ? texture(albedoTex, uv).a : 1.0;
#else
        float alpha = texture(albedoTex, uv).a;
#endif
        if (vertexColorExists > 0)
            alpha *= VertexColor.a;
#ifdef CETRA_SURFACE_HOOK
        CetraSurface hooked = CetraSurface(uv, HookWorldPos, vec3(0.0, 1.0, 0.0),
                                           vec3(0.0, 1.0, 0.0),
                                           vertexColorExists > 0 ? VertexColor : vec4(1.0),
                                           vec3(1.0), alpha, vec3(0.0, 1.0, 0.0), 1.0, 0.0, 1.0,
                                           vec3(0.0));
        cetraSurface(hooked);
        alpha = hooked.alpha;
#endif
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
