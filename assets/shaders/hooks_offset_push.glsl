// An offset hook moving its surface `push.x` metres down -z (spec 13.29's fixture): the
// quad it is on faces +z, toward the eye, so this is straight away from it.

uniform vec4 push;

vec3 cetraOffset(CetraVertex v)
{
    return vec3(0.0, 0.0, -push.x);
}
