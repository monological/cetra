// A 16-tap Poisson disk of unit radius, shared by every soft-shadow filter:
// the cascade PCSS in pbr_frag and the area-panel penumbra in
// punctual_shadow.glsl. One disk so the two paths cannot drift into different
// sampling noise for the same look.
//
// Sampled UNROTATED: a per-pixel rotation decorrelates the pattern between
// neighbours, which turns the 16-tap quantization into per-pixel shadow noise
// -- invisible on diffuse but riding the sharp specular lobe into a field of
// bright speckle on the metal. An unrotated disk gives a spatially coherent,
// smooth penumbra instead; the modest radius caps at each call site keep 16
// taps free of banding.
const vec2 POISSON16[16] = vec2[](
    vec2(-0.9420, -0.3991), vec2(0.9456, -0.7689), vec2(-0.0942, -0.9294),
    vec2(0.3450, 0.2939), vec2(-0.9159, 0.4577), vec2(-0.8154, -0.8791),
    vec2(-0.3828, 0.2768), vec2(0.9748, 0.7565), vec2(0.4432, -0.9751),
    vec2(0.5374, -0.4737), vec2(-0.2650, -0.4189), vec2(0.7920, 0.1909),
    vec2(-0.2419, 0.9971), vec2(-0.8141, 0.9144), vec2(0.1998, 0.7864),
    vec2(0.1438, -0.1410));
