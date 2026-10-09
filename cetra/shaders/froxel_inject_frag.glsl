#version 330 core
in vec2 TexCoords;
out vec4 FragColor; // rgb = in-scattered radiance, a = extinction sigma

// Froxel fog, pass 1 of 3 (spec 9.5): evaluate the participating medium once
// per volume cell. The screen-space march this replaces evaluated the same
// lighting once per pixel per step, which made the clustered light list far too
// expensive to consult; a froxel pays for it once per cell.
//
// One draw per slice writes one layer of the volume (sliceIndex says which), so
// this is an ordinary fullscreen pass over the volume's XY grid.
//
// The cascade tap, spot in-scatter and Henyey-Greenstein phase were inherited
// from the retired screen-space fog march (fog_frag, deleted in spec 9.5); the
// height sigma deliberately was NOT (a volume cannot express the march's
// floor-plane ray clamp). A pixel has one view ray, so the march could hoist
// phase and the light-space projection out of its step loop; a froxel has no
// single ray, so both are evaluated per cell.

// Mirrors shadow.h's MAX_SHADOW_LIGHTS / SHADOW_CASCADES under private names;
// they must track it the same way csm.glsl's copies do.
#define MAX_FOG_LIGHTS 3
#define FOG_CASCADES 3
// Mirrors POSTFX_MAX_FOG_VOLUMES the same way.
#define MAX_LOCAL_FOG 16

uniform int sliceIndex;  // Which volume layer this draw is writing
uniform int froxelDepth; // Slice count; mirrors POSTFX_FROXEL_Z
uniform mat4 projection; // read by depth.glsl
uniform mat4 invView;    // view -> world (camera pose)
uniform float fogNear;   // Near end of the volume's exponential depth range
uniform float fogFar;    // Far end of the volume's exponential depth range
uniform float fogDepthDist; // Slice bias exponent; 1 = pure exponential

uniform sampler2DArray shadowMaps;
uniform mat4 lightSpaceMatrix[MAX_FOG_LIGHTS * FOG_CASCADES];
uniform int cascadeCount;
uniform vec3 lightColor[MAX_FOG_LIGHTS]; // color * intensity
uniform vec3 lightDir[MAX_FOG_LIGHTS];   // normalized travel direction
uniform int numLights;
uniform vec3 ambientColor;
uniform float density;       // Extinction at floor height (1/world units)
uniform float heightFalloff; // World units for a 1/e density drop
uniform float floorY;        // World height of max density
// Water as a second medium (spec 11.33). 0 = air only.
uniform int waterMedium;
uniform float waterLevelY;
uniform vec4 waterBounds;     // where the water is: Water.bounds, all zero = everywhere
uniform vec3 waterExtinction; // per-channel; reduced to its mean here, see below
uniform vec3 waterInscatter;  // scene radiance, pre-exposed with everything else
// Cloud transmittance toward the sun (spec 11.39). NOT a fourth medium -- a visibility term
// for one light, which is why it multiplies into fogVisibility below rather than into sigma.
// The deck occludes the sun; it does not scatter.
#include "cloud_shadow.glsl"

/*
 * Local fog volumes (spec 11.39): boxes of denser air, for a smoky room or a dust shaft
 * that the one global medium cannot express. Count 0 = none, and costs one compare.
 *
 * A volume carries a TINT rather than its own radiance, and that is the whole reason it
 * is cheap: the box is lit by the same sun, sky and clustered lights as the air around
 * it, so it re-weights the lighting already computed here instead of asking for a second
 * evaluation of it. Emission -- a volume that glows on its own -- is the thing this shape
 * cannot express, and is not modelled.
 */
uniform int localFogCount;
uniform vec4 localFogCenterDensity[MAX_LOCAL_FOG]; // xyz world centre, w extinction added
uniform vec4 localFogExtentFeather[MAX_LOCAL_FOG]; // xyz half-extent, w inward ramp width
uniform vec3 localFogTint[MAX_LOCAL_FOG];          // scattering colour

uniform float anisotropy;    // Henyey-Greenstein g
uniform float sunBoost;
uniform float shadowBias;
// shadowMaps holds the fog's own ESM cascades when this is 1, and the scene's
// exact depth array when it is 0 (the build can decline: no caster, or an
// allocation that failed).
uniform int esmEnabled;
uniform float esmK;
uniform int spotEsmLayer; // The spot's layer in shadowMaps, after the cascades

// One volumetric spot light (the flashlight).
uniform int spotEnabled;
uniform vec3 spotPos;
uniform vec3 spotDir;
uniform vec3 spotColor;
uniform vec3 spotAtten;
uniform float spotCosInner;
uniform float spotCosOuter;
uniform int spotShadowed;
// This spot's IES profile, -1 for none, and the roll its asymmetry needs. Fed
// through the fog block rather than read from the cluster list because this
// shaft is the one light here that does NOT come from it -- it is published
// standalone so it can carry its shadow map (spec 9.5).
uniform int spotIesProfile;
uniform vec3 spotUp;
// The punctual shadow array and this spot's layer in it. Fed from postfx's own
// fog block rather than by the punctual_shadow.glsl binder, the same separation
// the cascade uniforms above keep.
uniform sampler2DArray punctualShadowMaps;
uniform int spotShadowLayer;
uniform mat4 spotLightSpaceMatrix;

// Falling rain as a medium (spec 13.9), past the streaks: its own extinction and its own
// phase, which is the drops' -- mostly a forward lobe from refraction, the rest everywhere --
// and a third medium in any cell it shares. Absent where the occlusion map says the rain does
// not reach, which is a layer of the array above.
uniform float rainSigma;    // extinction per world unit; 0 = no rain in the air
uniform float rainForwardG; // the drops' refracted lobe
uniform float rainNear;     // world units from the eye where it takes over from the streaks
#include "rain_occlusion.glsl"

// Temporal reprojection against the previous frame's volume. 1 when the frame
// before this one built a volume; 0 on the first frame and after a gap, which
// centres the sample and blends nothing, there being nothing to blend.
uniform int temporal;
uniform float temporalBlend; // History weight; higher averages more frames
uniform int frameIndex;
uniform sampler3D historyVolume;
uniform mat4 prevView;       // World -> the previous frame's view space
uniform mat4 prevProjection; // Its focal terms map that to the previous volume
uniform float historyScale;  // This frame's pre-exposure over the history's (spec 13.20)
uniform int missSamples;     // Samples a cell with no history averages; 1 = its one (spec 13.45)

const float PI = 3.14159265359;

// Clustered light data for the local-light scattering below.
#include "lights_ubo.glsl"
#include "froxel.glsl"
#include "view.glsl"
// A cached point light's faces (spec 13.16), read through punctualShadowMaps above.
#include "tile_lookup.glsl"
// How far nearer the light than a cell of air a stored surface must be to shadow it, in
// metres: a cell has no surface of its own to be acne on, so this need only clear the
// stored depth's quantisation and the faces' polygon offset.
#define FOG_TILE_BIAS 0.02

// Van der Corput radical inverse in the given base: successive frames land at
// low-discrepancy positions, so a fixed number of them average to an evenly
// distributed sample of the cell rather than clumping the way a hash would.
float halton(int index, int base) {
    float f = 1.0;
    float r = 0.0;
    int i = index;
    for (int k = 0; k < 8; k++) {
        if (i <= 0)
            break;
        f /= float(base);
        r += f * float(i - (i / base) * base);
        i /= base;
    }
    return r;
}

#include "phase.glsl"
#include "rain_phase.glsl"
#include "water_bounds.glsl"

// Is the air at world position P lit by the caster whose cascade block starts
// at layer0? Walks cascades in index order and taps the FIRST whose box
// contains the point -- the tightest covering cascade. Outside every cascade
// counts as lit.
float fogVisibility(int layer0, vec3 P) {
    for (int c = 0; c < cascadeCount; c++) {
        int layer = layer0 + c;
        vec3 proj = (lightSpaceMatrix[layer] * vec4(P, 1.0)).xyz * 0.5 + 0.5;
        if (proj.z > 1.0 || proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0) {
            continue;
        }
        // exp(k*d_blocker) * exp(-k*d_receiver) = exp(k*(d_b - d_r)): at or in
        // front of the blocker the exponent is >= 0 and this saturates to lit,
        // behind it the term decays smoothly. The value being filterable is what
        // lets the medium read a blurred, downsampled cascade at all; a depth
        // compare would have to be re-evaluated per tap.
        if (esmEnabled == 1) {
            float occ = texture(shadowMaps, vec3(proj.xy, float(layer))).r;
            return clamp(occ * exp(-esmK * proj.z), 0.0, 1.0);
        }
        float d = texture(shadowMaps, vec3(proj.xy, float(layer))).r;
        return proj.z - shadowBias > d ? 0.0 : 1.0;
    }
    return 1.0;
}

// The frame `index`'s offset within a cell: its point of the Halton sequence in each axis.
vec3 froxelJitter(int index) {
    return vec3(halton(index, 2), halton(index, 3), halton(index, 5));
}

// The medium at one sample of this cell, offset within it by `jitter` (0.5 = the centre): the
// in-scattered radiance, pre-exposed, and the extinction. A sample under the water's surface is
// the water's, with `submerged` set.
vec4 froxelMediumAt(vec3 jitter, float nearZ, out bool submerged) {
    submerged = false;
    vec2 cellUv = TexCoords + (jitter.xy - 0.5) / vec2(textureSize(historyVolume, 0).xy);
    vec3 viewPos = froxelViewPos(cellUv, float(sliceIndex), jitter.z, nearZ, fogFar,
                                 float(froxelDepth), fogDepthDist);
    vec3 camPos = invView[3].xyz;
    vec3 P = (invView * vec4(viewPos, 1.0)).xyz;
    // Direction from the camera toward this cell: the phase function's second
    // argument, and the froxel equivalent of the march's per-pixel rayDir.
    vec3 rayDir = worldRayDirFromEye(P - camPos, invView);

    // Exponential height falloff above the floor. Below it the medium does not
    // exist at all: the screen-space march expressed that by clamping downward
    // rays at the floor plane, which a volume cannot do, so the equivalent is a
    // zero extinction there -- otherwise sub-floor cells would sit at the
    // formula's maximum density and fog the ground from underneath.
    float airSigma = P.y < floorY ? 0.0 : density * exp(-(P.y - floorY) / heightFalloff);

    // A cell below the surface is water, not denser air, so the two media do not blend:
    // this REPLACES air's terms rather than adding to them, and everything the air path
    // computes below would be overwritten. So it returns here instead -- the whole light
    // accumulation, its shadow taps, and the temporal reprojection are all dead work for
    // a submerged cell, and a submerged camera puts most of the frustum down here.
    //
    // Coherent by construction: the test is a world-Y half-space, so divergence is
    // confined to the slice band straddling the surface, and to the edges of the water's
    // bounds.
    if (waterMedium == 1 && waterUnder(waterLevelY, waterBounds, P)) {
        // Luminance mean, because a cell carries ONE scalar extinction and the integrate
        // pass multiplies one scalar transmittance. The colour moves into the in-scatter,
        // so distance fades the seabed TOWARD the body colour rather than reddening out
        // of it on the way. The exact per-channel Beer-Lambert still runs in the surface
        // shader for everything seen through the interface, which is the path that
        // carries the look from above; this one only has to make being under the surface
        // read as being under something.
        //
        // And the body's in-scatter is a constant rather than scattered sunlight, so
        // there are no shafts down here. Real, and not modelled -- which is also why
        // there is nothing for the temporal accumulator to average: the value is
        // constant per frame, so blending it against its own history is a no-op.
        float bodySigma = dot(waterExtinction, vec3(0.2126, 0.7152, 0.0722));
        submerged = true;
        return vec4(min(waterInscatter * preExposure, vec3(WS_MEDIA_MAX)), bodySigma);
    }

    /*
     * Every medium in this cell: an extinction, and the colour it scatters, accumulated
     * together. Air seeds both -- its own tint is white, so it enters the sum as
     * airSigma * 1 -- which is what stops air being a special case at the fold below.
     *
     * Each volume contributes extinction scaled by how far inside the box this cell sits,
     * and carries that same weight into the tint sum, so a cell in the feather band is
     * partly the box and partly the air around it.
     *
     * The weight comes from the box's exact signed distance rather than a per-axis
     * product: a product feathers the corners twice and pinches them, which is visible
     * on any volume whose feather is a real fraction of its size -- and a dust shaft's
     * is, because the soft edge is the whole point of it.
     */
    float sigma = airSigma;
    vec3 sigmaTint = vec3(airSigma);
    for (int i = 0; i < localFogCount; i++) {
        vec3 q = abs(P - localFogCenterDensity[i].xyz) - localFogExtentFeather[i].xyz;
        float sd = length(max(q, vec3(0.0))) + min(max(q.x, max(q.y, q.z)), 0.0);
        // The feather arrives clamped away from zero by the publisher, so there is no
        // per-cell max here: it is an invariant of the packing, not of this loop.
        float w = 1.0 - smoothstep(-localFogExtentFeather[i].w, 0.0, sd);
        float s = localFogCenterDensity[i].w * w;
        sigma += s;
        sigmaTint += localFogTint[i] * s;
    }

    // How much of the deck this cell sits under. Hoisted: one lookup serves every light,
    // because only one of them can be the sun.
    float cloudSun = cloudSunAt(P);

    // The rain's extinction here: none under cover, and none inside the outermost streak box,
    // where the streaks already stand for every drop. Its source function, R, sums the same
    // lights as the fog's through the drops' phase in place of the fog's -- the one thing about
    // it that differs -- and without the fog's sunBoost, which is the fog's look and not light.
    // R is only summed where the rain has an extinction to fold it in by.
    float rainS = 0.0;
    if (rainSigma > 0.0) {
        float handover = rainNear > 0.0
                             ? smoothstep(0.75 * rainNear, rainNear, length(P - camPos))
                             : 1.0;
        rainS = rainSigma * handover;
        if (rainS > 0.0)
            rainS *= rainExposure(P);
    }
    vec3 R = ambientColor;

    vec3 S = ambientColor;
    for (int j = 0; j < numLights; j++) {
        float cosLight = dot(lightDir[j], -rayDir);
        float phase = phaseHG(cosLight, anisotropy) * sunBoost;
        // The deck occludes one light and the publisher says which, so this is a slot
        // compare rather than a direction compare -- exact where a dot needed an epsilon,
        // and -1 states "the sun is not in this list", which a direction cannot.
        float cloud = (j == cloudShadowLight) ? cloudSun : 1.0;
        float vis = fogVisibility(j * cascadeCount, P);
        S += lightColor[j] * (phase * cloud * vis);
        if (rainS > 0.0)
            R += lightColor[j] * (rainDropPhase(cosLight, rainForwardG) * cloud * vis);
    }

    // Spot in-scatter at P: inside the cone, falling off with distance, cut by
    // the spot's shadow.
    if (spotEnabled == 1) {
        vec3 toL = spotPos - P;
        float d = length(toL);
        // Through the same decision as the floor pool, on values because this
        // light is not in the cluster list: the beam has to agree with the pool
        // it casts, which is exactly what a second copy of the rule stops
        // guaranteeing.
        vec3 spotL = toL / max(d, 1e-4);
        float cone = punctualAngularOf(spotIesProfile, 2.0, spotDir, spotUp,
                                       spotCosInner, spotCosOuter, spotL);
        if (cone > 0.0) {
            float atten = getDistanceAtt(d * d, spotAtten.x);
            float vis = 1.0;
            if (spotShadowed == 1) {
                vec4 ls = spotLightSpaceMatrix * vec4(P, 1.0);
                if (ls.w > 0.0) { // in front of the light plane
                    vec3 pc = ls.xyz / ls.w * 0.5 + 0.5;
                    if (pc.z <= 1.0 && pc.x >= 0.0 && pc.x <= 1.0 && pc.y >= 0.0 && pc.y <= 1.0) {
                        // Same exponential test the cascades take, from the layer
                        // the build parks the spot in after them -- one array for
                        // the medium whatever is casting into it.
                        if (esmEnabled == 1) {
                            float occ = texture(shadowMaps, vec3(pc.xy, float(spotEsmLayer))).r;
                            vis = clamp(occ * exp(-esmK * pc.z), 0.0, 1.0);
                        } else {
                            vis = (pc.z - shadowBias >
                                   texture(punctualShadowMaps, vec3(pc.xy, float(spotShadowLayer)))
                                       .r)
                                      ? 0.0
                                      : 1.0;
                        }
                    }
                }
            }
            float spotPhase = phaseHG(dot(spotDir, -rayDir), anisotropy) * sunBoost;
            S += spotColor * (cone * atten * spotPhase * vis);
            // The light's own travel through P rather than the cone's axis: a drop's lobe is
            // narrow enough that the difference is the beam's width.
            if (rainS > 0.0)
                R += spotColor *
                     (cone * atten * rainDropPhase(dot(-spotL, -rayDir), rainForwardG) * vis);
        }
    }

    // Clustered point lights (spec 9.1's list), unshadowed unless cached: a per-frame
    // point map is six layers the fog has no budget to read, and a cached light's faces
    // (spec 13.16) take one tap. Attenuation matches pbr_frag so a light's glow in the
    // air agrees with the pool it casts on the floor.
    // No enable flag: the UBOs are zero-initialised and always bound, so a
    // scene without clustered lights reports count 0 and this costs nothing --
    // the degradation to sun+spot coverage is structural, not a toggle.
    uvec2 list = clusterLightListUv(TexCoords, -viewPos.z);
    for (uint k = 0u; k < list.y; k++) {
        uint li = lightIndexAt(list.x + k);
        // Point lights only. Area panels do not scatter in v1, and every spot
        // is skipped because the scene's first spot is already scattered above
        // WITH its shadow map and also appears in this list -- adding it again
        // would double it. Further spots go unscattered, as in the march.
        if (clusterLights[li].dirType.w != 1.0)
            continue;
        vec3 toL = clusterLights[li].posRange.xyz - P;
        float d = length(toL);
        float sqrDist = dot(toL, toL);
        float atten = getDistanceAtt(sqrDist, clusterLights[li].attenCutoff.x);
        // A point light's ANGULAR term used to be nothing -- this loop never read
        // dirType.xyz, because a bare point has no direction that means anything.
        // An IES profile gives it one, and the air has to agree with the floor
        // about it or a shaped downlight scatters a halo its own pool does not
        // have (spec 11.57). punctualAngular answers 1 for an unprofiled point,
        // so the un-profiled fog is unchanged.
        vec3 pointL = toL / max(d, 1e-4);
        float attenAngular = atten * punctualAngular(li, pointL);
        // One tap of a cached light's faces: the volume's own accumulator averages the cell's
        // binary answers, as it does the cascades'.
        if (clusterLights[li].shadowMisc.y >= float(SHADOW_TILE_MARK))
            attenAngular *= tileVisibility(li, P, FOG_TILE_BIAS);
        // pointL points at the light, so its negation is the direction the light
        // travels -- the punctual analogue of the directional phase above.
        float phase = phaseHG(dot(-pointL, -rayDir), anisotropy) * sunBoost;
        S += clusterLights[li].colorIntensity.xyz * (attenAngular * phase);
        if (rainS > 0.0)
            R += clusterLights[li].colorIntensity.xyz *
                 (attenAngular * rainDropPhase(dot(-pointL, -rayDir), rainForwardG));
    }

    /*
     * Fold the media's colour in, SIGMA-WEIGHTED rather than added.
     *
     * The integrate pass reads this cell's rgb as the medium's source function, not as
     * radiance already scaled by how much medium is present -- so two media sharing a
     * cell combine by averaging their sources weighted by the extinction each brings.
     * Adding them would count the same light once per medium and make a box brighten
     * simply for existing.
     *
     * The guard is a NaN guard, not a feature test: with no volume anywhere and a cell
     * below floorY, both terms are exactly zero and this is 0/0. Do not replace it with
     * max(sigma, eps) -- airSigma legitimately falls below any such epsilon far above the
     * floor, and that form darkens thin air instead. Testing localFogCount rather than
     * sigma keeps the whole thing off a uniform branch in the overwhelmingly common case
     * of no volumes at all.
     */
    if (localFogCount > 0 && sigma > 0.0)
        S *= sigmaTint / sigma;
    // The rain is a third medium and folds in the same way, weighted by the extinction it
    // brings. Guarded on its own extinction, which is what keeps a cell with no rain exactly
    // the cell it was.
    if (rainS > 0.0) {
        S = (S * sigma + R * rainS) / (sigma + rainS);
        sigma += rainS;
    }

    // Scene radiance -> working space HERE, upstream of the clamp, for the same
    // reason pbr_frag pre-exposes its per-light radiance rather than its final
    // write: the clamp sits between, and a working-space constant applied to
    // scene radiance flattens whatever it bites. The volume is therefore stored
    // pre-exposed, and froxel_composite must NOT convert again.
    //
    // Keep shafts HDR (they must bloom) but bound hostile parameter combos away
    // from fp16 overflow, as the screen-space march does. 500 now means 500x
    // white rather than 500 nits.
    return vec4(min(S * preExposure, vec3(WS_MEDIA_MAX)), sigma);
}

// Temporal reprojection: where this cell sat in the previous frame's volume, and whether it sat
// inside it at all. Unlike the screen-space passes there is no velocity buffer to reproject by --
// a froxel is a volume of air, not a surface -- so the previous camera does the mapping.
//
// From the cell's UNJITTERED centre. Reprojecting the jittered sample instead would land the
// history lookup at a different sub-cell offset every frame, so each frame reads a different
// trilinear mix of neighbours and the accumulation never settles -- it flickers rather than
// converging. Reprojecting the centre makes a static camera read exactly this cell's own
// history, which is what turns the blend into a running average of the jittered samples.
bool froxelHistoryAt(float nearZ, out vec3 prevUvw) {
    prevUvw = vec3(0.0);
    vec3 centreView = froxelViewPos(TexCoords, float(sliceIndex), 0.5, nearZ, fogFar,
                                    float(froxelDepth), fogDepthDist);
    vec3 centreP = (invView * vec4(centreView, 1.0)).xyz;
    vec4 prevViewPos = prevView * vec4(centreP, 1.0);
    float prevZ = -prevViewPos.z;
    if (prevZ <= nearZ)
        return false;
    vec2 prevUv = uvFromViewXY(prevViewPos.xy, prevZ, prevProjection);
    float prevSlice = froxelViewZToSlice(prevZ, nearZ, fogFar, float(froxelDepth), fogDepthDist);
    // Scatter cells sit at their slice centre (slice s spans continuous s..s+1), so continuous
    // coordinate c reads texel c-0.5, i.e. the normalized coordinate is just c/depth.
    prevUvw = vec3(prevUv, prevSlice / float(froxelDepth));
    // Off-volume reprojection has no history to blend: the standard disocclusion fallback.
    return all(greaterThanEqual(prevUvw, vec3(0.0))) && all(lessThanEqual(prevUvw, vec3(1.0)));
}

#ifndef FROXEL_HISTORY_MISS
void main() {
    // The VOLUME's near, not the camera's: see fogNear's owner in postfx.c.
    float nearZ = fogNear;

    // Sub-cell sample offset. The offset is the SAME for every cell -- a
    // low-discrepancy shift of the whole grid -- so averaging successive frames
    // supersamples the volume rather than adding per-cell noise. It has to move
    // laterally, not just in depth: the cascade tap is binary, so a shadow
    // boundary lands on a cell edge and stair-steps at the grid's 160x90, and
    // only an X/Y shift walks that edge across the cell. This is what replaces
    // the 24 taps per ray the screen-space march averaged.
    // Centred (no offset) when there is no history to average into, which keeps
    // the first frame from sampling off-centre with nothing to blend against.
    vec3 jitter = vec3(0.5);
    if (temporal == 1)
        jitter = froxelJitter(frameIndex + 1);
    bool submerged;
    vec4 result = froxelMediumAt(jitter, nearZ, submerged);
    if (submerged) {
        FragColor = result;
        return;
    }

    if (temporal == 1) {
        vec3 prevUvw;
        if (froxelHistoryAt(nearZ, prevUvw)) {
            // The history was stored at its own frame's pre-exposure; brought to this
            // frame's, a change of exposure lands whole rather than at the blend's pace.
            // Only the in-scatter: extinction knows nothing of the exposure. Under the
            // same ceiling as this frame's value, which a step up could otherwise pass.
            vec4 history = texture(historyVolume, prevUvw);
            history.rgb = min(history.rgb * historyScale, vec3(WS_MEDIA_MAX));
            result = mix(result, history, temporalBlend);
        } else if (missSamples > 1) {
            // A cell with no history (spec 13.45) -- one entering the volume as the camera
            // turns or moves -- would show its one jittered sample beside neighbours averaged
            // over many frames, and a turning camera drew the difference as a hard-edged band
            // along the grid's columns. It takes the points before this one in the sequence
            // too, the ones those averages weigh most, wrapping at the sequence's start.
            for (int k = 1; k < missSamples; k++) {
                int index = frameIndex + 1 - k;
                if (index < 1)
                    index += missSamples;
                bool under;
                result += froxelMediumAt(froxelJitter(index), nearZ, under);
            }
            result /= float(missSamples);
        }
    }

    FragColor = result;
}
#else
/*
 * The miss probe's count (spec 13.45), drawn with colour writes off under an occlusion query, on
 * a frame whose cells may take several samples: what passes is every cell the main above gave
 * them, found by the same two tests in the same order.
 */
void main() {
    float nearZ = fogNear;
    bool submerged;
    froxelMediumAt(froxelJitter(frameIndex + 1), nearZ, submerged);
    vec3 prevUvw;
    if (submerged || froxelHistoryAt(nearZ, prevUvw))
        discard;
    FragColor = vec4(0.0);
}
#endif
