// A wet surface, whatever wetted it (specs 11.45 and 13.9): the swash on a beach and the rain
// on a street are two SOURCES of the same water, and this is the one statement of what that
// water does to a surface. Each source decides how wet and how filmed a point is; nothing
// here knows which one is asking.

// Lekner-Dorf internal reflection as a power on the albedo: saturating, so a channel the dry
// surface already absorbs is absorbed more, which is the cool-warm shift a wet surface has.
const float WET_DARKEN_POWER = 0.55;
// What a full film smooths the surface to, and the F0 of the air/water interface it puts in
// front of it. Water is LESS reflective than a dielectric at normal incidence; the shine is
// the roughness, not the Fresnel.
const float WET_ROUGHNESS = 0.12;
const float WET_F0 = 0.02;
// How far a full film flattens the normal toward the geometric one. Not 1: a drained sheet
// still follows the surface it is lying on, and taking the normal all the way to flat reads
// as a decal of water rather than water on a surface.
const float WET_FLATTEN = 0.75;

// `porosity` is the fraction of the diffuse albedo a fully wet surface loses, `wet` how wet
// it is (0..1) and `film` how much free water lies on it (0..1). The caller carries `film`
// on to the Fresnel: F0 is derived from the IOR later, so it cannot be folded in here.
void wetSurface(inout vec3 albedo, inout float roughness, inout vec3 N, vec3 Ngeom,
                float porosity, float wet, float film) {
    /*
     * POROSITY DARKENING (Lagarde). Water filling the pores raises the effective refractive
     * index between the grains, so less light escapes back out: the diffuse albedo drops by
     * the open porosity that got filled.
     *
     * A tint multiply was the obvious alternative and is wrong twice over -- it does not
     * saturate, and it makes a wet surface a colour rather than a darker version of whatever
     * colour it already is.
     */
    albedo *= 1.0 - porosity * wet;

    /*
     * THE HUE SHIFT, and not by Beer-Lambert. Water's absorption is (0.0035, 0.0004, 0) per
     * cm, so a film 0.1-1 mm thick transmits exp(-0.0007) -- 0.9993, which is nothing.
     *
     * What actually reddens a wet surface is Lekner-Dorf internal reflection: light that
     * scatters inside it is reflected back IN at the water surface instead of escaping, so it
     * makes more passes through the surface's own pigment. A power law on the albedo is the
     * saturating form of that, and it composes with the porosity term above rather than
     * fighting it.
     */
    albedo = pow(albedo, vec3(1.0 + WET_DARKEN_POWER * wet));

    /*
     * The film makes the surface SMOOTH, which is why it looks shinier -- not more reflective.
     * An air/water interface is F0 0.02 against a dielectric's 0.04, so the reflectance
     * actually falls; what rises is how tightly it is concentrated.
     */
    roughness = clamp(mix(roughness, WET_ROUGHNESS, film), 0.04, 1.0);
    // A drained sheet reads as a sheet: the ripples the normal map carries are under the
    // water, not on it.
    N = normalize(mix(N, Ngeom, film * WET_FLATTEN));
}
