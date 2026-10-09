// A lampshade's cloth (spec 13.40), a surface hook: the light of the bulbs inside it coming
// through. Where a bulb is behind the side being drawn -- the shade's outside -- the cloth glows
// with what it lets through, as a sheet that scatters it evenly: its transmittance times its
// colour times the light reaching it from behind, over pi. So it is brightest at the bulb's
// height, where the cloth is nearest and square to it, dims toward its rims, and its weave shows
// against the light. The inside faces the bulb and glows with none of this: it is the bulb's own
// light reflected, which the engine lights.

uniform vec4 shadeBulb0; // a bulb: where it is, world, and its candela; a candela of 0 is none
uniform vec4 shadeBulb1;
uniform vec4 shadeBulb2;
uniform vec4 shadeTint0; // its colour
uniform vec4 shadeTint1;
uniform vec4 shadeTint2;
uniform vec4 shadeCloth; // x: the share of the light reaching the cloth that passes through it

// The light from `bulb` reaching the cloth at `p` from behind its side `n`, in lux.
vec3 shadeBehind(vec4 bulb, vec3 tint, vec3 p, vec3 n)
{
    vec3 away = p - bulb.xyz;
    float d2 = max(dot(away, away), 1e-4);
    return tint * bulb.w * max(dot(n, away * inversesqrt(d2)), 0.0) / d2;
}

void cetraSurface(inout CetraSurface s)
{
    vec3 lux = shadeBehind(shadeBulb0, shadeTint0.rgb, s.worldPos, s.geomNormal) +
               shadeBehind(shadeBulb1, shadeTint1.rgb, s.worldPos, s.geomNormal) +
               shadeBehind(shadeBulb2, shadeTint2.rgb, s.worldPos, s.geomNormal);
    s.emissive = shadeCloth.x * s.albedo * lux / 3.14159265;
}
