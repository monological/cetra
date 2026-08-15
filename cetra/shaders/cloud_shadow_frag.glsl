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
 * And the marched length is capped rather than the sun angle, because the step SCHEDULE is
 * what breaks at grazing incidence, not the geometry.
 *
 * Uncapped, span = thickness/sinEl, so at the floor above dt reaches ~2 km against an 8 km
 * noise period: under four samples per period, which is aliasing rather than sampling. Worse,
 * every tap is weighted by dt, so a single d=0.3 sample contributes tau ~ 15.6 and blacks the
 * texel outright, and the wind offset re-rolls which taps land each frame.
 *
 * The reach is the deck's OWN light cone, and matching it is the point rather than a
 * coincidence. clouds.glsl:177 spends 1.2 km on its sun march and says why: past that, one far
 * tap through a neighbouring tower accounts for kilometres of extinction and blackens
 * everything. That argument is about this noise field at this extinction, so it governs here
 * too -- the deck and the shadow it casts should not disagree about how far light travels.
 *
 * A longer reach was tried and is exactly the failure that comment predicts. The full shell
 * traverse, and then a 4 km cap, both SATURATE: at coverage 0.45 the mean density over a
 * slanted path puts tau near 25 * 0.1 * 4 = 10 at every texel, and the map goes uniformly
 * black -- a flat full shadow with no gaps, which reads as the deck having no holes in it. It
 * survived review because the arms watch the shadow's effect on the fog rather than the map,
 * and a constant map still darkens and still varies downstream. The debug tile is what found
 * it.
 *
 * 24 steps over 1.2 km is dt = 50 m, inside one 62.5 m shape texel, so the field is sampled
 * rather than aliased at every sun angle.
 */
const float CLOUD_SHADOW_SPAN_CAP_KM = 1.2;

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
    vec2 xz = TexCoords * CLOUD_SHAPE_TILE_KM;
    // Start at the shell bottom -- the altitude this map is indexed at.
    vec3 pos = vec3(xz.x, CLOUD_BOTTOM_KM, xz.y);

    float span = min((CLOUD_TOP_KM - CLOUD_BOTTOM_KM) / sunY, CLOUD_SHADOW_SPAN_CAP_KM);
    float dt = span / float(CLOUD_SHADOW_STEPS);

    vec3 centre = vec3(0.0, -Rg, 0.0);
    float tau = 0.0;
    for (int i = 0; i < CLOUD_SHADOW_STEPS; i++) {
        // Cell centres, not edges: a half-step offset makes the sum a midpoint rule, which is
        // exact for the linear ramps the altitude gradient is built from.
        vec3 p = pos + vec3(sunDir.x, sunY, sunDir.z) * (float(i) + 0.5) * dt;
        // detailOn false: the erosion tap is a texture-scale feature, and this map is read
        // through a filter far coarser than the detail it would add.
        float d = cloudDensity(p, cloudHeightFracAt(p), shapeTex, detailTex, coverage, cloudType,
                               false, windOffsetKm, 0.0);
        tau += d * CLOUD_EXTINCTION * densityScale * dt;
    }

    FragColor = mix(1.0, exp(-tau), smoothstep(0.0, CLOUD_SHADOW_MIN_SUN_Y, sunDir.y));
}
