// The dome's surface (spec 13.29's fixture): the normal of the height its offset raises, so
// the dome is lit as one -- the grid's own normal is the flat one it was built with. `dome.y` is
// the grid's width in metres, which the slope is measured against.

uniform vec4 dome;

void cetraSurface(inout CetraSurface s)
{
    vec2 d = s.uv * 2.0 - 1.0;
    if (dot(d, d) < 1.0) {
        float k = 4.0 * dome.x / dome.y;
        s.normal = normalize(vec3(k * d.x, 1.0, k * d.y));
    }
}
