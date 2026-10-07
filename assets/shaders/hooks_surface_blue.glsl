// A surface hook painting its material flat blue (spec 13.29's fixture), the twin of
// hooks_surface_red at the same feature mask: two hooks, two programs.

void cetraSurface(inout CetraSurface s)
{
    s.albedo = vec3(0.02, 0.02, 0.8);
}
