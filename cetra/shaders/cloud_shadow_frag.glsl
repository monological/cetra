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
// A sun below this is doing nothing but grazing the shell for thousands of kilometres, and
// the path length explodes as 1/sinEl. The light itself is faded out down there
// (sky_apply_sun_to_light clears cast_shadows under ~3 degrees), so the map goes clear.
const float CLOUD_SHADOW_MIN_SUN_Y = 0.05;

void main()
{
    if (sunDir.y < CLOUD_SHADOW_MIN_SUN_Y) {
        FragColor = 1.0;
        return;
    }

    // Exactly ONE tile of the shape field, which is what makes this map cover the whole world
    // from 256 texels. With detail off, cloudDensity is periodic in XZ with the shape texture's
    // own period, so a GL_REPEAT lookup at any world position is not an approximation of a
    // bigger map -- it is the same value that map would hold. A finite window was tried first
    // and its edge is plainly visible: past the window the receiver reverts to full sun, which
    // reads as a hard diagonal across the fog wherever the sun shear runs out of texture.
    vec2 xz = TexCoords * CLOUD_SHAPE_TILE_KM;
    // Start at the shell bottom -- the altitude this map is indexed at.
    vec3 pos = vec3(xz.x, CLOUD_BOTTOM_KM, xz.y);

    // March to the top along the sun. The vertical span is fixed, so the PATH grows as the sun
    // drops and the step count does not: the integral stays right because dt grows with it.
    float span = (CLOUD_TOP_KM - CLOUD_BOTTOM_KM) / sunDir.y;
    float dt = span / float(CLOUD_SHADOW_STEPS);

    vec3 centre = vec3(0.0, -Rg, 0.0);
    float tau = 0.0;
    for (int i = 0; i < CLOUD_SHADOW_STEPS; i++) {
        // Cell centres, not edges: a half-step offset makes the sum a midpoint rule, which is
        // exact for the linear ramps the altitude gradient is built from.
        vec3 p = pos + sunDir * (float(i) + 0.5) * dt;
        float alt = length(p - centre) - Rg;
        // detailOn false: the erosion tap is a texture-scale feature, and this map is read
        // through a filter far coarser than the detail it would add.
        float d = cloudDensity(p, cloudHeightFrac(alt), shapeTex, detailTex, coverage, cloudType,
                               false, windOffsetKm, 0.0);
        tau += d * CLOUD_EXTINCTION * densityScale * dt;
    }

    FragColor = exp(-tau);
}
