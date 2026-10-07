// A surface hook taking its albedo from a param (spec 13.29's fixture): one hook on two
// materials, which tell it apart by `tint` alone.

uniform vec4 tint;

void cetraSurface(inout CetraSurface s)
{
    s.albedo = tint.rgb;
}
