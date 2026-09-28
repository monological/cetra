// Normalized Henyey-Greenstein for a caller-chosen asymmetry; c = cos(angle between light travel
// and the direction toward the camera). The plain HG the fog, the cloud media and the water's
// in-scatter all mix, where the atmosphere's own aerosols take Cornette-Shanks (miePhase).
//
// One copy for every shader that wants it; the froxel pass and the water each carried their own
// until spec 13.4. Uses the includer's PI, which every one of them already declares.
float phaseHG(float c, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * c, 1.5));
}
