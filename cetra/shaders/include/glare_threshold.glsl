// The light a pixel carries above the diffraction glare's threshold (spec 13.4), in the same
// working-space units as the pixel: its colour scaled by how far its brightest channel is past
// the threshold. The glare's source pass stars exactly this, and the tonemap takes exactly this
// out of the pixel it came from (spec 13.5), so the two must be one rule.
vec3 glareAboveThreshold(vec3 c, float threshold)
{
    float l = max(max(c.r, c.g), c.b);
    return c * (max(l - threshold, 0.0) / max(l, 1.0e-4));
}
