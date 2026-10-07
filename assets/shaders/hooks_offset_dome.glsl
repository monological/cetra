// An offset hook raising a flat grid into a dome (spec 13.29's fixture): `dome.x` metres at
// the middle of the UV square, falling to nothing at its inscribed circle.

uniform vec4 dome;

vec3 cetraOffset(CetraVertex v)
{
    vec2 d = v.uv * 2.0 - 1.0;
    return vec3(0.0, dome.x * max(1.0 - dot(d, d), 0.0), 0.0);
}
