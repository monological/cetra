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
uniform sampler2D specTex;    // Resolved ambient specular (working space)
uniform sampler2D aoTex;      // AO chain: .r visibility, .gba encoded bent normal
uniform sampler2D normalsTex; // View normal .xyz + catcher marker .a
uniform sampler2D auxTex;     // .w = effective roughness
uniform vec2 invFocal;        // 1/projection focal terms (view-ray reconstruction)
uniform int aoActive;         // 0 = no AO this frame: fold spec back untouched
uniform float aoStrength;

#include "spec_occ.glsl"

void main()
{
    vec3 spec = texture(specTex, TexCoords).rgb;
    if (aoActive == 0) {
        // AO off but split on: the scene still owes its ambient specular.
        FragColor = vec4(spec, 1.0);
        return;
    }
    vec4 aoSample = texture(aoTex, TexCoords);
    float ao = aoSample.r;
    vec4 nrm = texture(normalsTex, TexCoords);
    float so;
    if (dot(nrm.xyz, nrm.xyz) < 0.01 || nrm.a < 0.0) {
        // Sky/hair (zero normal) and the shadow-catcher floor (negative
        // marker): no trustworthy reflection direction; both carry zero
        // split specular, so the plain AO answer serves for the factor.
        so = ao;
    } else {
        vec2 ndc = TexCoords * 2.0 - 1.0;
        vec3 V = normalize(vec3(-ndc * invFocal, 1.0));
        vec3 N = normalize(nrm.xyz);
        vec3 bentN = normalize(aoSample.gba * 2.0 - 1.0);
        so = specOcclusionCone(ao, texture(auxTex, TexCoords).w, bentN, reflect(-V, N));
    }
    FragColor = vec4(spec * mix(1.0, so, aoStrength), mix(1.0, ao, aoStrength));
}
