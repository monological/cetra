// Cloud-deck sun transmittance, shared by every consumer that shadows a light
// with it (specs 11.39, 11.41). There are four -- the froxel medium and the
// three ground surfaces -- and they have to state the shell geometry
// identically, or the haze is shadowed by a deck the terrain is standing in a
// different place from.

// Declared here unless the consumer has already aliased the name. pbr_frag is at
// the driver's limit of sixteen DECLARED fragment samplers and reaches this map
// through `#define cloudShadowTex sceneColorTex`; everyone else takes a real unit
// and needs to say nothing. Same shape as csm.glsl's tunables: the exception is
// written where the exception lives, not repeated by every ordinary consumer.
#ifndef cloudShadowTex
uniform sampler2D cloudShadowTex;
#endif

uniform float cloudShadowTile;   // world units the map's period covers
uniform float cloudShadowShellY; // world Y the map is indexed at
uniform vec2 cloudShadowShear;   // world XZ travelled per unit of climb toward the sun
// CSM slot of the light the deck occludes, and the single off switch. -1 covers
// every case: no deck, a sun below the horizon, and a host that has lent
// cloudShadowTex to another tenant this pass. A consumer therefore never has to
// know whose unit it is reading.
uniform int cloudShadowLight;

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
    if (cloudShadowLight < 0)
        return 1.0;
    vec2 hit = P.xz + cloudShadowShear * (cloudShadowShellY - P.y);
    return texture(cloudShadowTex, hit / cloudShadowTile).r;
}

// The same, for a consumer that shades one light at a time and has to ask whether
// THIS light is the occluded one. Exactly 1.0 for every other light.
//
// The slot compare has to exclude -1 rather than let it match: -1 is also the
// sentinel above, so a light with no cascade would otherwise satisfy -1 == -1 and
// take a cloud shadow belonging to nothing. Only pbr_frag can currently reach that
// state, and only because its slot comes from the light rather than a loop index --
// which is precisely why the test belongs here and not at three call sites.
float cloudSunForSlot(vec3 P, int slot)
{
    return (slot >= 0 && slot == cloudShadowLight) ? cloudSunAt(P) : 1.0;
}
