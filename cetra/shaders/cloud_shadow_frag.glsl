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
 * This replaces a cap on the total marched LENGTH, which conflated two unrelated limits and got
 * both wrong. Capping the slant path meant the fraction of the deck actually crossed was
 * 1.2 * sinEl / thickness -- 48% straight overhead, 12% at 15 degrees, 4% at 5. So the map was
 * never a transmittance through the deck at any sun angle, and below ~10 degrees the march never
 * left the cloud base at all: no optical depth, and the map saturated to full sun with nothing
 * downstream able to tell. 11.40 fixed this same constant saturating the map to ZERO at the
 * other end; apps/tree's 0.8 degree sun sat in the dead band the whole time.
 *
 * The two limits, separated:
 *
 *   VERTICAL extent must span the deck, or the integral is not the quantity this map is named
 *   after. It is elevation-independent and not negotiable, so the march steps in equal ALTITUDE
 *   increments and always crosses bottom to top.
 *
 *   HORIZONTAL reach must stay bounded, and THAT is what this constant is. The reason is the
 *   one clouds.glsl:177 gives for its own 1.2 km sun cone -- past that, one far tap through a
 *   neighbouring tower accounts for kilometres of extinction and blackens everything -- plus a
 *   sampling reason specific to this map: the shape field tiles every 8 km under GL_REPEAT, so
 *   a 28.7 km slant at 5 degrees wraps it three and a half times and every texel's ray passes
 *   through essentially every cloud in the field. That, and not the magnitude of tau, is why
 *   11.40's longer-reach attempts came back uniformly black: the excursion decorrelates a texel
 *   from its own column, so the gaps stop being gaps. Truncating the whole path did stop it,
 *   and threw away the vertical coverage with it.
 *
 * Past the reach the march keeps climbing the column it stopped at, which states plainly that
 * the deck is taken as horizontally homogeneous beyond the radius this field can be trusted
 * over. The deck's own cone already assumes exactly that.
 */
const float CLOUD_SHADOW_REACH_KM = 8.0;

void main()
{
    // Marched at the floor even below it; the fade below is what takes the result to clear,
    // so nothing discontinuous happens at the boundary.
    float sunY = max(sunDir.y, CLOUD_SHADOW_MIN_SUN_Y);

    // Exactly ONE tile of the shape field, which is what makes this map cover the whole world
    // from 256 texels. With detail off, cloudDensity is periodic in XZ with the shape texture's
    // own period, so a GL_REPEAT lookup at any world position is not an approximation of a
    // bigger map -- it is the same value that map would hold. A finite window was tried first
    // and its edge is plainly visible: past the window the receiver reverts to full sun, which
    // reads as a hard diagonal across the fog wherever the sun shear runs out of texture.
    // The column this texel owns, at the shell bottom -- the altitude the map is indexed at.
    vec2 xz = TexCoords * CLOUD_SHAPE_TILE_KM;

    // Step the DECK, not a length: the climb is the same 2.5 km at every sun angle, so the
    // integral covers the whole cloud whatever the elevation.
    float dh = (CLOUD_TOP_KM - CLOUD_BOTTOM_KM) / float(CLOUD_SHADOW_STEPS);
    // World XZ travelled per km of climb toward the sun. The same quantity sky.c publishes to
    // the receivers as cloudShadowShear, for the same reason: the sun's Y divides out once.
    vec2 shear = vec2(sunDir.x, sunDir.z) / sunY;
    // Slant travelled per step. The TRUE one, unaffected by the reach clamp below -- the light
    // really does cross that much air, and it is what makes a grazing sun come out opaque
    // instead of merely dim. Only the density ESTIMATE is clamped, never the path it weights.
    float ds = dh / sunY;

    float tau = 0.0;
    for (int i = 0; i < CLOUD_SHADOW_STEPS; i++) {
        // Cell centres, not edges: a half-step offset makes the sum a midpoint rule, which is
        // exact for the linear ramps the altitude gradient is built from.
        float h = (float(i) + 0.5) * dh;
        vec2 off = shear * h;
        float r = length(off);
        if (r > CLOUD_SHADOW_REACH_KM)
            off *= CLOUD_SHADOW_REACH_KM / r;
        vec3 p = vec3(xz.x + off.x, CLOUD_BOTTOM_KM + h, xz.y + off.y);
        // detailOn false: the erosion tap is a texture-scale feature, and this map is read
        // through a filter far coarser than the detail it would add.
        float d = cloudDensity(p, cloudHeightFracAt(p), shapeTex, detailTex, coverage, cloudType,
                               false, windOffsetKm, 0.0);
        tau += d * CLOUD_EXTINCTION * densityScale * ds;
    }

    FragColor = mix(1.0, exp(-tau), smoothstep(0.0, CLOUD_SHADOW_MIN_SUN_Y, sunDir.y));
}
