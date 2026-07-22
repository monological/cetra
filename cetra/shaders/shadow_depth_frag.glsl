#version 330 core

in vec2 TexCoords;

// Alpha-tested depth for foliage (material.h foliage_shadows). Off by default:
// opaque geometry writes depth with no texture fetch at all, exactly as before.
uniform sampler2D albedoTex;
uniform int alphaTested;
uniform float alphaCutoff;

void main()
{
    if (alphaTested == 1 && texture(albedoTex, TexCoords).a < alphaCutoff)
        discard; // leaf cutout: the gaps between leaves must let light through

    // Depth is written automatically to gl_FragDepth
}
