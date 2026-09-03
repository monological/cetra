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
#define MAX_LOCAL_FOG 8

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

// Temporal reprojection against the previous frame's volume. 0 freezes the
// jitter and skips the blend, so headless renders stay byte-deterministic --
// the same contract every other accumulator in this stack honours.
uniform int temporal;
uniform float temporalBlend; // History weight; higher averages more frames
uniform int frameIndex;
uniform sampler3D historyVolume;
uniform mat4 prevView;       // World -> the previous frame's view space
uniform mat4 prevProjection; // Its focal terms map that to the previous volume

const float PI = 3.14159265359;

// Clustered light data for the local-light scattering below.
#include "lights_ubo.glsl"
#include "froxel.glsl"
#include "view.glsl"

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

// Normalized Henyey-Greenstein; c = cos(angle between light travel and the
// direction toward the camera). Duplicates include/atmosphere.glsl's phaseHG
// (keep in step) -- this shader deliberately does not ingest the atmosphere
// toolkit for one four-line function.
float phaseHG(float c, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * c, 1.5));
}

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
    if (temporal == 1) {
        jitter = vec3(halton(frameIndex + 1, 2), halton(frameIndex + 1, 3),
                      halton(frameIndex + 1, 5));
    }
    vec2 cellUv = TexCoords + (jitter.xy - 0.5) / vec2(textureSize(historyVolume, 0).xy);
    vec3 viewPos = froxelViewPos(cellUv, float(sliceIndex), jitter.z, nearZ, fogFar,
                                 float(froxelDepth), fogDepthDist);
    vec3 camPos = invView[3].xyz;
    vec3 P = (invView * vec4(viewPos, 1.0)).xyz;
    // The cell's UNJITTERED centre, kept for reprojection only. Reprojecting the
    // jittered sample instead would land the history lookup at a different
    // sub-cell offset every frame, so each frame reads a different trilinear mix
    // of neighbours and the accumulation never settles -- it flickers rather
    // than converging. Reprojecting the centre makes a static camera read
    // exactly this cell's own history, which is what turns the blend into a
    // running average of the jittered samples.
    vec3 centreView = froxelViewPos(TexCoords, float(sliceIndex), 0.5, nearZ, fogFar,
                                    float(froxelDepth), fogDepthDist);
    vec3 centreP = (invView * vec4(centreView, 1.0)).xyz;
    // Direction from the camera toward this cell: the phase function's second
    // argument, and the froxel equivalent of the march's per-pixel rayDir.
    vec3 rayDir = normalize(P - camPos);

    // Exponential height falloff above the floor. Below it the medium does not
    // exist at all: the screen-space march expressed that by clamping downward
    // rays at the floor plane, which a volume cannot do, so the equivalent is a
    // zero extinction there -- otherwise sub-floor cells would sit at the
    // formula's maximum density and fog the ground from underneath.
    float airSigma = P.y < floorY ? 0.0 : density * exp(-(P.y - floorY) / heightFalloff);

    // A cell below the surface is water, not denser air, so the two media do not blend:
    // this REPLACES air's terms rather than adding to them, and everything the air path
    // computes below would be overwritten. So it exits here instead -- the whole light
    // accumulation, its shadow taps, and the temporal reprojection are all dead work for
    // a submerged cell, and a submerged camera puts most of the frustum down here.
    //
    // Coherent by construction: the test is a world-Y half-space, so divergence is
    // confined to the slice band straddling the surface.
    if (waterMedium == 1 && P.y < waterLevelY) {
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
        FragColor = vec4(min(waterInscatter * preExposure, vec3(WS_MEDIA_MAX)), bodySigma);
        return;
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

    vec3 S = ambientColor;
    for (int j = 0; j < numLights; j++) {
        float phase = phaseHG(dot(lightDir[j], -rayDir), anisotropy) * sunBoost;
        // The deck occludes one light and the publisher says which, so this is a slot
        // compare rather than a direction compare -- exact where a dot needed an epsilon,
        // and -1 states "the sun is not in this list", which a direction cannot.
        float cloud = (j == cloudShadowLight) ? cloudSun : 1.0;
        S += lightColor[j] * (phase * cloud * fogVisibility(j * cascadeCount, P));
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
        }
    }

    // Clustered point lights (spec 9.1's list), unshadowed -- the engine has no
    // shadow map for point lights. Attenuation matches pbr_frag so a light's
    // glow in the air agrees with the pool it casts on the floor.
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
        // pointL points at the light, so its negation is the direction the light
        // travels -- the punctual analogue of the directional phase above.
        float phase = phaseHG(dot(-pointL, -rayDir), anisotropy) * sunBoost;
        S += clusterLights[li].colorIntensity.xyz * (attenAngular * phase);
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

    // Scene radiance -> working space HERE, upstream of the clamp, for the same
    // reason pbr_frag pre-exposes its per-light radiance rather than its final
    // write: the clamp sits between, and a working-space constant applied to
    // scene radiance flattens whatever it bites. The volume is therefore stored
    // pre-exposed, and froxel_composite must NOT convert again.
    //
    // Keep shafts HDR (they must bloom) but bound hostile parameter combos away
    // from fp16 overflow, as the screen-space march does. 500 now means 500x
    // white rather than 500 nits.
    vec4 result = vec4(min(S * preExposure, vec3(WS_MEDIA_MAX)), sigma);

    // Temporal reprojection: find where this cell's world position sat in the
    // previous frame's volume and blend against it. Unlike the screen-space
    // passes there is no velocity buffer to reproject by -- a froxel is a
    // volume of air, not a surface -- so the previous camera does the mapping.
    if (temporal == 1) {
        vec4 prevViewPos = prevView * vec4(centreP, 1.0);
        float prevZ = -prevViewPos.z;
        if (prevZ > nearZ) {
            vec2 prevFocal = vec2(prevProjection[0][0], prevProjection[1][1]);
            vec2 prevUv = (prevViewPos.xy * prevFocal / prevZ) * 0.5 + 0.5;
            float prevSlice =
                froxelViewZToSlice(prevZ, nearZ, fogFar, float(froxelDepth), fogDepthDist);
            // Scatter cells sit at their slice centre (slice s spans continuous
            // s..s+1), so continuous coordinate c reads texel c-0.5, i.e. the
            // normalized coordinate is just c/depth.
            vec3 prevUvw = vec3(prevUv, prevSlice / float(froxelDepth));
            // Off-volume reprojection has no history to blend, so those cells
            // keep the current frame -- the standard disocclusion fallback.
            if (all(greaterThanEqual(prevUvw, vec3(0.0))) &&
                all(lessThanEqual(prevUvw, vec3(1.0)))) {
                vec4 history = texture(historyVolume, prevUvw);
                result = mix(result, history, temporalBlend);
            }
        }
    }

    FragColor = result;
}
