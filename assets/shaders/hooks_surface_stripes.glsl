// A surface hook (spec 13.29's fixture): stripes running up the box, `stripes.x` metres a
// pair, by world X, between the albedos `.y` and `.z`. Everything else the material keeps.

// pbr_frag expanded this chunk at build time; including it again is how the fixture holds the
// splice to one copy.
#include "noise.glsl"

uniform vec4 stripes;

void cetraSurface(inout CetraSurface s)
{
    float light = step(0.5, fract(s.worldPos.x / stripes.x));
    s.albedo = vec3(mix(stripes.y, stripes.z, light));
}
