// The television's glass (spec 13.30), a surface hook after tv_picture.glsl: the light it emits,
// the signal's mean, takes the hum bar and the tube's shape, so what reflects the glass before the
// late draw sees the picture's light where the picture is and none past its corners.

uniform vec4 tvPlace;  // the picture's lower corner, world, and its width, metres
uniform vec4 tvAcross; // the way across it, world and unit, and its height, metres

void cetraSurface(inout CetraSurface s)
{
    vec3 d = s.worldPos - tvPlace.xyz;
    vec2 p = vec2(dot(d, tvAcross.xyz) / tvPlace.w, d.y / tvAcross.w);
    s.emissive *= tvHum(p.y) * tvTubeShape(p, tvPlace.w / tvAcross.w);
}
