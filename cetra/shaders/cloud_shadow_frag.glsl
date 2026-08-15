#version 330 core
in vec2 TexCoords;
out float FragColor;

/*
 * Cloud transmittance toward the sun, as a 2D map (spec 11.39).
 *
 * The deck is a horizontal shell, so how much sun reaches a point below it depends only on
 * WHERE that point's sun ray crosses the shell -- not on how far below it sits. That makes a
 * 2D map exact rather than approximate: one texel per crossing point, and any receiver at any
 * altitude beneath the deck reads it by shearing its own position up to the shell. No matrix,
 * no light-space projection, no depth compare.
 *
 * Indexed at the shell BOTTOM, which is also where this march starts, so the map's texel and
 * the receiver's lookup name the same point by construction.
 *
 * WORLD-ANCHORED, and that is a deliberate divergence from the deck overhead, which is
 * camera-anchored (clouds.glsl's header explains why, and records that the physical
 * alternative was built and rejected on sight). A shadow cannot follow the camera: a dapple
 * that slides across the terrain as you walk, so that you can never step out of shade, is
 * unmistakably wrong in a way the sky's own anchoring is not. The cost is that a cloud
 * overhead does not sit above its own shadow, which is only visible to someone checking one
 * against the other. Making both consistent is a one-line change to the march's sample
 * position; it moves the cloud goldens and is filed rather than smuggled in here.
 *
 * Radiance-free: this marches density and nothing else. No phase function, no ambient, no LUT
 * taps -- a shadow needs the extinction integral alone, which is why it calls cloudDensity
 * directly instead of cloud_march.
 */

uniform sampler3D shapeTex;
uniform sampler3D detailTex;
uniform vec3 sunDir; // world-space unit vector TOWARD the sun, matching sky->sun_dir
uniform float coverage;
uniform float cloudType;
uniform float densityScale;
uniform vec3 windOffsetKm;

#include "clouds.glsl"

// Enough to resolve a tower's self-shadow without paying for the screen march's budget: the
// shell is 2.5 km thick and this integral is read through a trilinear volume lookup that
// blurs finer structure away regardless.
const int CLOUD_SHADOW_STEPS = 24;
/*
 * Below this the shadow fades OUT rather than switching off, and the difference matters.
 *
 * A cliff here was the first attempt and it pops: sky_apply_sun_to_light scales the sun's
 * INTENSITY by elevation/3 but only clears cast_shadows at elevation <= 0, so between the
 * horizon and this floor the sun is still a live, shadow-casting, fog-lighting directional at
 * up to 96% strength. Cutting the map to 1.0 under it made clouds stop shadowing in one step,
 * at 2.87 degrees, in a band real scenes sit in -- apps/tree's default sun is 0.8.
 */
const float CLOUD_SHADOW_MIN_SUN_Y = 0.05;
/*
 * How far the march may wander HORIZONTALLY from the texel it is computing (spec 11.41).
 *
 * ONE tile period, and derived rather than spelled, because the period IS the argument: the
 * shape field repeats over it under GL_REPEAT, so a ray that travels further re-reads the same
 * clouds and the texel stops describing its own column. At a 5 degree sun the slant is 28.7 km,
 * three and a half periods, and every texel's ray then passes through essentially every cloud
 * in the field. That decorrelation -- not the magnitude of tau -- is what made the uncapped
 * march come back uniformly black.
 *
 * Past the reach the march keeps climbing the column it stopped at, which states plainly that
 * the deck is taken as horizontally homogeneous beyond the radius this field can resolve. The
 * deck's own sun cone (clouds.glsl) already assumes exactly that.
 *
 * REJECTED: 1.2 km, borrowed from that cone. It binds below 64 degrees, so like the slant cap
 * it replaced it truncates at nearly every sun angle, and it reports the map LIGHTER than the
 * truth: measured mean 0.1611 against 0.1339 at 35 degrees, 0.1308 against 0.0080 at 15. The
 * two values agree exactly at 75 degrees, where neither clamps, which is the check that
 * distinguishes them.
 */
const float CLOUD_SHADOW_REACH_KM = CLOUD_SHAPE_TILE_KM;

void main()
{
    // Marched at the floor even below it; the fade below is what takes the result to clear,
    // so nothing discontinuous happens at the boundary.
    float sunY = max(sunDir.y, CLOUD_SHADOW_MIN_SUN_Y);

    // Exactly ONE tile of the shape field, which is what makes this map cover the whole world
    // from 256 texels. With detail off, cloudDensity is periodic in XZ with the shape texture's
    // own period, so a GL_REPEAT lookup at any world position is not an approximation of a
    // bigger map -- it is the same value that map would hold.
    vec2 xz = TexCoords * CLOUD_SHAPE_TILE_KM;

    // Step the DECK, not a length: the climb is the same at every sun angle, so the integral
    // covers the whole cloud whatever the elevation.
    float dh = (CLOUD_TOP_KM - CLOUD_BOTTOM_KM) / float(CLOUD_SHADOW_STEPS);
    // World XZ per km of climb toward the sun. Must be the same quantity the receivers get as
    // cloudShadowShear, or the map's geometry and the lookup's disagree about which texel a
    // point maps to.
    vec2 shear = vec2(sunDir.x, sunDir.z) / sunY;
    // Slant travelled per step. The TRUE one, unaffected by the reach clamp -- the light really
    // does cross that much air, and it is what makes a grazing sun come out opaque instead of
    // merely dim. Only the density ESTIMATE is clamped, never the path it weights.
    float ds = dh / sunY;
    // The direction is fixed and the climb only grows, so clamping the excursion IS clamping
    // the altitude at which it stops growing. Stated once here rather than rediscovered by a
    // length() and a divide on all 24 steps -- and above ~17 degrees it never binds at all.
    float hReach = CLOUD_SHADOW_REACH_KM / max(length(shear), 1e-6);
    float tauPerDensity = CLOUD_EXTINCTION * densityScale * ds;

    float tau = 0.0;
    for (int i = 0; i < CLOUD_SHADOW_STEPS; i++) {
        // Cell centres, not edges: a half-step offset makes the sum a midpoint rule, which is
        // exact for the linear ramps the altitude gradient is built from.
        float h = (float(i) + 0.5) * dh;
        vec2 off = xz + shear * min(h, hReach);
        vec3 p = vec3(off.x, CLOUD_BOTTOM_KM + h, off.y);
        // Height fraction through cloudHeightFracAt, not the loop's own (i+0.5)/STEPS.
        //
        // The planar form is cheaper and this march is otherwise planar, but the DECK is not:
        // clouds.glsl measures altitude spherically, and the view march shades against that.
        // A shadow map that placed the same cloud at a different height than the deck does is
        // the exact disagreement this file exists to avoid -- worth two decimals of curvature
        // at the widest excursion, which is where the dapple edges visibly move.
        //
        // detailOn false: the erosion tap is a texture-scale feature, and this map is read
        // through a filter far coarser than the detail it would add.
        float d = cloudDensity(p, cloudHeightFracAt(p), shapeTex, detailTex, coverage, cloudType,
                               false, windOffsetKm, 0.0);
        tau += d * tauPerDensity;
    }

    FragColor = mix(1.0, exp(-tau), smoothstep(0.0, CLOUD_SHADOW_MIN_SUN_Y, sunDir.y));
}
