// What the picture on the living room's television and the glass under it share (spec 13.30):
// the field they both show, the tube's shape and the hum bar. tv.c puts this into both, so the
// glass's light is the picture's own expected light wherever it falls, and the picture adds only
// its noise about it.

uniform vec4 tvField; // this field: its number, the hum bar's phase (0..1), the noise's spread at
                      // the AGC's gain, and the signal's mean light as a fraction of peak

const float TV_CORNER = 0.08;    // the corners' radius, in picture heights
const float TV_EDGE = 0.01;      // how soft the tube's edge is, in picture heights
const float TV_FALLOFF = 0.15;   // how far the light falls toward the sides and corners
const float TV_HUM_DEPTH = 0.18; // how far the hum bar takes the light down

// The tube at `p`, 0..1 across and up a picture `aspect` wide for each unit of height: 1 at the
// middle, falling a little toward the sides, and 0 past its rounded corners.
float tvTubeShape(vec2 p, float aspect)
{
    vec2 extent = vec2(aspect, 1.0) * 0.5;
    vec2 q = (p - 0.5) * vec2(aspect, 1.0);
    float d = length(max(abs(q) - (extent - TV_CORNER), 0.0)) - TV_CORNER;
    vec2 c = 2.0 * p - 1.0;
    return (1.0 - smoothstep(-TV_EDGE, 0.0, d)) * (1.0 - TV_FALLOFF * dot(c, c));
}

// The hum bar at `y` up the picture: mains beating against the field rate, a broad dip in the
// light rolling up the picture.
float tvHum(float y)
{
    return 1.0 - TV_HUM_DEPTH * (0.5 + 0.5 * cos(6.2831853 * (1.0 - y + tvField.y)));
}
