// Cosine of the GGX reflection lobe's half-angle from perceptual roughness:
// a mirror at 0, near-hemispheric at 1.
//
// Its own file for one function because the lobe is now measured in two places
// that cannot share a program -- against the sector bitmask inside the AO sweep,
// and against the bent normal's cone in the tonemap -- and two spellings of one
// lobe would make those two modes differ for a reason that is not the mode.
float specLobeCos(float perceptualRoughness)
{
    return exp2(-3.321928 * perceptualRoughness * perceptualRoughness);
}
