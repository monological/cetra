// Cloud-deck sun transmittance, shared by every consumer that shadows a light
// with it (specs 11.39, 11.41). There are four -- the froxel medium and the
// three ground surfaces -- and they have to state the shell geometry
// identically, or the haze is shadowed by a deck the terrain is standing in a
// different place from.
//
// The SAMPLER is deliberately not declared here, and that is the whole reason
// this file can be shared at all. pbr_frag already declares sixteen fragment
// samplers, which is every one the driver allows, and reaches this map through
// `#define cloudShadowTex sceneColorTex` -- so a declaration in here would be a
// seventeenth and would fail to link. Each consumer supplies the name before
// including: a real uniform where it has a unit to spare, the alias where it
// does not.

uniform float cloudShadowTile;   // world units the map's period covers; 0 = no deck
uniform float cloudShadowShellY; // world Y the map is indexed at
uniform vec2 cloudShadowShear;   // world XZ travelled per unit of climb toward the sun
uniform int cloudShadowLight;    // CSM slot of the light the deck occludes; -1 = none

// Sun transmittance at a world position below the deck. 1 = full sun.
//
// Exact for a horizontal shell rather than an approximation of one: the sun ray
// from P crosses the deck at a single point, so shearing P up to the shell and
// reading there IS the answer, at any altitude below it. No matrix, no cascade,
// no depth compare.
//
// No bounds test. The map holds one period of a field that is periodic over
// cloudShadowTile and the sampler wraps, so every world position lands on a real
// value. A finite window was tried first and its edge reads as a hard diagonal
// wherever the shear runs past the last texel.
float cloudSunAt(vec3 P)
{
    if (cloudShadowTile <= 0.0)
        return 1.0;
    vec2 hit = P.xz + cloudShadowShear * (cloudShadowShellY - P.y);
    return texture(cloudShadowTex, hit / cloudShadowTile).r;
}
