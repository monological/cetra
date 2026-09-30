// The medium a reflection sees the distant environment through (spec 13.9): the global height
// fog, lit by its ambient. Wet ground in fog does not mirror the clear sky above the fog, it
// mirrors the fog -- which is what the eye sees looking up, and what Unreal's sky light gets by
// capturing its height fog. Applied per prefilter SAMPLE, so a lobe straddling the horizon
// averages fogged sky rather than fogging an average.
//
// envMediumDensity 0 is no medium, and the answer is the radiance handed in, unchanged.

uniform float envMediumDensity;  // extinction at the fog's floor, per world unit
uniform float envMediumFalloff;  // world units per 1/e of density above the floor
uniform float envMediumReach;    // world units along a ray the fog is integrated over
uniform vec3 envMediumInscatter; // radiance the fog scatters toward the eye, scene units

// Along `dir` from the fog's floor, which is where the ground that reflects the sky lies. A ray
// that starts downward leaves the medium at once, since nothing of it exists below the floor.
vec3 envThroughMedium(vec3 radiance, vec3 dir) {
    if (envMediumDensity <= 0.0 || dir.y <= 0.0)
        return radiance;
    // The optical depth of an exponential atmosphere along a straight ray, in closed form:
    // density * reach * (1 - e^-k) / k, where k is how many falloffs the ray climbs over its
    // reach. Near the horizon the closed form cancels itself, and the series takes over.
    float k = dir.y * envMediumReach / envMediumFalloff;
    float climb = k < 1e-3 ? 1.0 - 0.5 * k : (1.0 - exp(-k)) / k;
    float T = exp(-envMediumDensity * envMediumReach * climb);
    // Constant in-scatter integrated against the same extinction: the froxel integrate's
    // segment form, taken over the whole ray at once.
    return radiance * T + envMediumInscatter * (1.0 - T);
}
