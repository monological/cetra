// What the picture on the living room's television and the glass under it share (spec 13.30):
// the field they both show, the tube's shape and the hum bar. tv.c puts this into both, so the
// glass's light is the picture's own expected light wherever it falls, and the picture adds only
// its noise about it.

uniform vec4 tvField; // this field: its number, the hum bar's phase (0..1), the noise's spread at
                      // the AGC's gain, and the signal's mean light as a fraction of peak
uniform vec4 tvTube;  // its corners' radius and its edge's softness, in picture heights; how far
                      // its light falls toward the sides; how far the hum bar takes it down

// The tube at `p`, 0..1 across and up a picture `aspect` wide for each unit of height: 1 at the
// middle, falling a little toward the sides, and 0 past its rounded corners.
float tvTubeShape(vec2 p, float aspect)
{
    vec2 extent = vec2(aspect, 1.0) * 0.5;
    vec2 q = (p - 0.5) * vec2(aspect, 1.0);
    float d = length(max(abs(q) - (extent - tvTube.x), 0.0)) - tvTube.x;
    vec2 c = 2.0 * p - 1.0;
    return (1.0 - smoothstep(-tvTube.y, 0.0, d)) * (1.0 - tvTube.z * dot(c, c));
}

// The hum bar at `y` up the picture: mains beating against the field rate, a broad dip in the
// light rolling up the picture.
float tvHum(float y)
{
    return 1.0 - tvTube.w * (0.5 + 0.5 * cos(6.2831853 * (1.0 - y + tvField.y)));
}
