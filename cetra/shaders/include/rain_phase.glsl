// The light a raindrop scatters toward the eye (spec 13.9), one statement for a drop drawn as
// a streak and a drop that is part of the air. Refraction throws RAIN_REFRACT_SHARE of it into
// a forward lobe and the rest goes everywhere, so rain between the eye and a lamp glitters and
// rain lit from the side is barely there. c = cos(angle between the light's travel and the
// direction toward the camera), phase.glsl's convention.
//
// Requires phase.glsl, rain_constants.glsl and the includer's PI.
float rainDropPhase(float c, float forwardG) {
    return mix(1.0 / (4.0 * PI), phaseHG(c, forwardG), RAIN_REFRACT_SHARE);
}
