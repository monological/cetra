// A surface hook cutting round holes (spec 13.29's fixture): `holes.x` by `.y` cells across
// the card's UV, each holed at its middle out to `.z` of the cell. Masked, so the card is cut,
// and cast through the alpha test, so its shadow is cut too. A radius of 0 is the solid card.

uniform vec4 holes;

void cetraSurface(inout CetraSurface s)
{
    vec2 cell = fract(s.uv * holes.xy) - 0.5;
    if (length(cell) < holes.z)
        s.alpha = 0.0;
}
