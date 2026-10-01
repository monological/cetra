// What rain does to a surface (spec 13.9): wets it, and where the ground is flat enough,
// stands on it as puddles the drops ring. The lit-surface shader's rain feature, whole --
// pbr_frag includes this under PBR_FEAT_RAIN and makes one call.
//
// Requires, included first: wet_surface.glsl (the water model the shore shares), noise.glsl
// (hash21, ign), `punctualShadowMaps` (the array the cover is a layer of), `time`, the
// material's `transmission`, and under the relief bit its `heightTex`.

#include "pbr_features.glsl"
#include "rain_occlusion.glsl"
#include "rain_ripples.glsl"

uniform float rainWetness;     // 0..1, how soaked the world is; see Rain.wetness
uniform float rainDarkening;   // scale on the albedo terms; 1 = physical, see wet_darkening
uniform float uPorosity;       // Material.porosity; -1 = derive it from the roughness
uniform float rainPuddleLevel; // 0..1, how much of the ground the puddles have claimed
uniform float rainPuddleScale; // metres across a typical puddle
#if CETRA_HAS(PBR_FEAT_RELIEF)
uniform float rainPuddleRelief; // 0..1, how far the height map places the puddles
// How far a height map's full range moves the puddle line at full relief, in the puddle noise's
// units (0..1): half of it, so the map reshapes the puddles the noise places rather than
// replacing them with a copy of its own tile.
const float RAIN_RELIEF_RANGE = 0.5;
#endif

// What a fully porous, fully wet surface loses of its diffuse albedo: the top of the 25-50%
// band Lagarde gives for natural materials. The shore's sand is 0.38 of the same scale.
const float RAIN_POROSITY_DARKEN = 0.5;
// Where a surface starts to hold a film and where it holds a full one, as the up component of
// its normal: a roof pitched past about 70 degrees sheds, a road holds.
const float RAIN_FILM_UP_MIN = 0.35;
const float RAIN_FILM_UP_FULL = 0.85;
// How much of a full film the open ground holds between puddles. Rain runs off a surface as
// fast as it arrives, so the water fills the texture's lows and the highs stand out of it:
// glossy, and still rough enough that a lamp smears into a streak. A full film is what only
// standing water has, and if the open ground held one too it would be the same mirror as a
// puddle and no puddle would read.
const float RAIN_SURFACE_FILM = 0.6;
// Where ground stops shedding and starts holding a puddle: within about eight degrees of
// level, fully within three. The margin is how far below the level the ground round a puddle
// is soaked to a full film, in the units of the puddle noise; the waterline itself is sharp.
const float RAIN_PUDDLE_FLAT_MIN = 0.99;
const float RAIN_PUDDLE_FLAT_FULL = 0.9986;
const float RAIN_PUDDLE_MARGIN = 0.06;
// Still water: very nearly a mirror.
const float RAIN_PUDDLE_ROUGHNESS = 0.03;
// The film below which a surface is not marked for screen-space reflection: a trace per pixel
// is not worth a two-percent reflection off a damp wall.
const float RAIN_SSR_MIN_FILM = 0.05;

// Where the ground holds water, 0..1: two octaves of value noise over the ground plane. A
// puddle forms where this is BELOW the level, so a rising level grows the existing puddles
// and joins them rather than scattering new ones.
float rainPuddleNoise(vec2 xz) {
    vec2 p = xz / rainPuddleScale;
    float n = 0.0;
    float weight = 0.65;
    for (int octave = 0; octave < 2; octave++) {
        vec2 i = floor(p);
        vec2 f = fract(p);
        vec2 u = f * f * (3.0 - 2.0 * f);
        const vec2 K = vec2(269.5, 183.3);
        float a = hash21(i, K);
        float b = hash21(i + vec2(1.0, 0.0), K);
        float c = hash21(i + vec2(0.0, 1.0), K);
        float d = hash21(i + vec2(1.0, 1.0), K);
        n += weight * mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
        p = p * 2.7 + 13.1;
        weight = 0.35;
    }
    return n;
}

/*
 * Wet the surface for the rain and return the film on it, which the caller carries on to the
 * Fresnel -- F0 is derived from the IOR later, so it cannot be folded in here.
 *
 * How wet is the world's integrated state times this point's cover, read off the occlusion map
 * a little way out along the geometric normal `Ng`. Everything the rain reaches wets, walls
 * included; only what faces UP holds a film, because a wall sheds its water as fast as it
 * arrives. A metal's albedo is its reflectance, not a diffuse colour water can darken, so the
 * porosity and hue terms fade out with the metalness and a wet metal only takes the film --
 * and a transmissive surface's albedo is the tint of what shows through it, which water on
 * glass does not darken either.
 *
 * `uv` is where the material's height map is read under the relief bit. `exposure` is the
 * cover the point read, 0 to 1, for a caller that asks about the same spot.
 * Call it from control flow uniform over the draw: it takes a screen derivative.
 */
float rainWetSurface(inout vec3 albedo, inout float roughness, inout vec3 N, vec3 Ng,
                     vec3 worldPos, float metallic, vec2 fragCoord, vec2 uv,
                     out float exposure) {
    exposure = 0.0;
    if (rainWetness <= 0.0)
        return 0.0;
    // Both here, above the puddle test, because that test is not uniform over the draw.
    float footprint = length(fwidth(worldPos.xz));
    // How far below the puddle level this point lies: positive is under standing water.
    float depth = rainPuddleLevel - rainPuddleNoise(worldPos.xz);
#if CETRA_HAS(PBR_FEAT_RELIEF)
    // Where the material's own height map is low its puddles stand first (spec 13.12), measured
    // from the map's mean -- its coarsest mip -- so the map lowers as much ground as it raises:
    // it gathers the puddles into the lows rather than flooding or draining the whole surface.
    depth -= rainPuddleRelief * RAIN_RELIEF_RANGE *
             (texture(heightTex, uv).r - textureLod(heightTex, uv, 16.0).r);
#endif
    // The waterline is one pixel wide wherever it falls, so it is sharp underfoot and does not
    // alias in the distance.
    float waterline = max(fwidth(depth), 1e-4);

    exposure =
        rainExposureSoft(worldPos + Ng * RAIN_NORMAL_OFFSET, 6.2831853 * ign(fragCoord));
    float wet = rainWetness * exposure;
    // Rough is porous, the Lagarde mapping: gloss 0.5 and above is sealed, 0.1 fully open.
    float porosity = uPorosity >= 0.0 ? uPorosity : clamp((roughness - 0.5) / 0.4, 0.0, 1.0);
    float flatness = smoothstep(RAIN_PUDDLE_FLAT_MIN, RAIN_PUDDLE_FLAT_FULL, Ng.y);
    // The open ground's partial film, rising to a full one over the soaked margin of a puddle.
    float soaked = flatness * smoothstep(-RAIN_PUDDLE_MARGIN, 0.0, depth);
    float film = wet * smoothstep(RAIN_FILM_UP_MIN, RAIN_FILM_UP_FULL, Ng.y) *
                 mix(RAIN_SURFACE_FILM, 1.0, soaked);
    wetSurface(albedo, roughness, N, Ng, porosity * RAIN_POROSITY_DARKEN,
               wet * (1.0 - metallic) * (1.0 - transmission) * rainDarkening, film);

    /*
     * PUDDLES: where flat ground holds standing water. Water deep enough to cover the
     * surface's own relief is a mirror lying flat, whatever the surface under it was doing --
     * so a puddle takes the geometric normal whole and a still water's roughness, and the rain
     * rings it. Only on ground within a few degrees of level: a pitched roof or a kerb sheds.
     */
    float puddle = wet * flatness * smoothstep(-waterline, waterline, depth);
    if (puddle > 0.0) {
        roughness = mix(roughness, RAIN_PUDDLE_ROUGHNESS, puddle);
        N = normalize(mix(N, Ng, puddle));
        N = rainRippleTilt(N, rainRippleSlope(worldPos.xz, footprint), puddle);
        film = max(film, puddle);
    }
    return film;
}
