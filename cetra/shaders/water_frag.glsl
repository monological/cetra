#version 330 core

/*
 * Water surface shading (spec 11.32).
 *
 * A single-layer water interface: Schlick-approximated dielectric Fresnel, with F0
 * taken from the IOR rather than authored, splits the view into
 * a reflected share taken from the environment cubemap and a transmitted share
 * taken from the resolved opaque scene, attenuated through the body by
 * Beer-Lambert over the real path length the depth buffer gives.
 *
 * Output is LINEAR WORKING SPACE, like every other pass that writes the scene
 * HDR buffer. Anything sampled in absolute scene radiance -- the environment
 * cubemap, the authored scatter colour -- is multiplied by preExposure here; the
 * resolved scene colour is NOT, because it was written pre-exposed and would be
 * counted twice. view.glsl is the authority on that contract.
 */

// The full G-buffer. Every location the opaque draw-buffer list may enable has to
// be written: slots are turned on for the FRAME, by whether a consumer wants them,
// not per draw -- so a location enabled and left unwritten keeps whatever the
// previous pass put there.
layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 NormalOut;
layout(location = 2) out vec4 VelocityOut;
layout(location = 3) out vec4 AlbedoOut;
layout(location = 4) out vec4 DiffuseOut;
layout(location = 7) out vec4 SpecOut;

in vec3 WorldPos;
in vec3 ViewPos;
in vec3 Normal;
in vec4 CurrClip;
in vec4 PrevClip;
in float Jacobian;
in float Shoal;
// How much of the displacing bands' slope the vertex stage's footprint filtered away. The
// other half of the geometry-to-BRDF handover: what left the mesh arrives here as
// roughness.
in float Filtered;

uniform mat4 view;
uniform mat4 projection;
uniform vec2 screenSize;

uniform float waterRoughness;
uniform float waterIor;
uniform vec3 waterAbsorption; // extinction per world unit, per channel
uniform vec3 waterScatter;    // in-scattered colour, absolute scene radiance

// Mipped resolve of the opaque scene (colour + skybox), already pre-exposed.
uniform sampler2D sceneColorTex;
uniform int sceneColorAvailable;
// Single-sample resolved scene depth: the water column and the shoreline test.
uniform sampler2D sceneDepthTex;
uniform int sceneDepthAvailable;

uniform vec3 sunDir; // toward the sun, world space
uniform int sunAvailable;
uniform int cameraSubmerged;
uniform int causticsEnabled;
// 1 = the shoreline writes fractional coverage for alpha-to-coverage; 0 = the
// binary cutoff, which is all a single-sample target can express.
uniform int alphaToCoverage;

// Split-sum environment cubemap, engine-bound. The procedural sky bakes into it,
// so a reflection taken from here tracks the sun without asking.
uniform samplerCube prefilteredMap;
uniform sampler2D brdfLUT;
uniform int iblEnabled;
uniform float iblIntensity;
uniform float maxReflectionLOD;

#include "view.glsl"
#include "velocity.glsl"
#include "fresnel.glsl"
#include "depth.glsl"
// The surface definition, for its cascade samplers and its wave-model switch. The
// fragment stage does not re-evaluate the surface -- the vertex stage's position
// and normal are interpolated in -- but it reads the SHORT band, which by design
// never reaches the mesh.
#include "ocean.glsl"

// Coarsest mip the transmission may select. The resolve stops generating there.
const float WATER_TRANSMISSION_MAX_LOD = 6.0;
/*
 * Absorption is evaluated over a clamped path: past a few extinction lengths the
 * exponential is already zero, and an unbounded value from a sky-depth sample
 * would only cost precision.
 *
 * The budget is in EXTINCTION LENGTHS rather than world units, because that is what
 * the sentence above actually says, and a length in units only means it at one world
 * scale. 3.84 is 64 units at a blue extinction of 0.06 per unit -- clear seawater in a
 * metre-scale world, which is what this was tuned against. A world of 22 units to the
 * metre gets the same optical depth instead of a sight line 22x shorter than the water
 * it authored.
 *
 * The WEAKEST channel sets it: the clamp has to sit past the point the LAST channel
 * dies, and in water that is blue. Truncation at the clamp therefore leaves at most
 * exp(-3.84) = 2.15% -- a derivative kink rather than a step, and the bound to claim
 * instead of claiming nothing is truncated.
 *
 * WATER_MAX_PATH survives as a FLOOR, so this can only lengthen a clamp, never shorten
 * one -- no scene loses reach to the rounding of the division.
 */
const float WATER_MAX_PATH = 64.0;
const float WATER_MAX_OPTICAL_DEPTH = 3.84;
// Below this column the surface has emerged through the bed; see the discard.
const float WATER_MIN_PATH = 0.01;
// Coverage below this contributes no samples at any supported sample count, so the
// fragment is dropped rather than shaded for nothing.
const float WATER_MIN_COVERAGE = 0.02;
// Depth at or above which the buffer is holding its cleared value rather than geometry. One
// code short of 1.0 at 24 bits, so a real surface sitting exactly on the far plane is the only
// thing this can misread -- and that surface is already at the limit of what depth can express.
const float WATER_DEPTH_EMPTY = 0.99999;
// Ceiling on the refraction bend, in world units. The bend is a screen-space
// approximation, and past a metre or so of offset the sample it reaches has
// little to do with the ray -- the validity check below catches the wrong ones,
// but not reaching for them is cheaper than rejecting them.
const float WATER_MAX_BEND = 1.0;
// The short cascade's resolved slope is faded out between these view distances,
// its energy moving into roughness. Metres: at 12 m tiling, past ~120 m a period
// is a couple of pixels and a literal normal there is aliasing, not detail.
const float WATER_SHORT_NEAR = 42.0;
const float WATER_SHORT_FAR = 118.0;
const float WATER_SHORT_SLOPE_GAIN = 0.42;
// Where interface roughness lands once ALL of the resolved slope has been handed over. The
// low end is the authored waterRoughness, not a constant here: the calm value is a property
// of the water being described, and only the far end is a property of this mechanism.
const float WATER_ROUGH_RIPPLED = 0.115;
// Jacobian compression at which foam starts and saturates.
const float WATER_FOAM_ON = 0.16;
const float WATER_FOAM_FULL = 0.42;
// Whitewater is not white: it is a bright grey with the sky in it.
const vec3 WATER_FOAM_COLOR = vec3(0.72, 0.80, 0.78);
const float WATER_FOAM_MAX = 0.62;
// Shore band strength. Lower than a breaking crest: this is water going shallow,
// not water breaking, and at crest strength every lake edge would read as surf.
const float WATER_SHORE_FOAM = 0.45;
/*
 * Caustics. Light crossing the surface is focused by the surface's own curvature,
 * so the brightness on the bed is a property of the WAVES above the point being
 * lit -- not of a texture pasted onto the floor. Sampling the cascade derivatives
 * where the refracted sun ray CROSSED the surface is what ties the two together;
 * the reference study rejected cellular caustics for exactly this reason and then
 * WATER_CAUSTIC_GAIN sets the strength.
 */
const float WATER_CAUSTIC_ON = 0.060;
const float WATER_CAUSTIC_FULL = 0.27;
const float WATER_CAUSTIC_GAIN = 1.35;
// Caustics need water above them to focus through, and lose coherence with depth.
const float WATER_CAUSTIC_SHALLOW = 0.35;
const float WATER_CAUSTIC_DEEP_ON = 9.0;
const float WATER_CAUSTIC_DEEP_OFF = 20.0;

void main() {
    vec2 uv = gl_FragCoord.xy / max(screenSize, vec2(1.0));

    vec3 N = normalize(Normal);
    /*
     * The geometry-to-BRDF handover, which arrives from two directions.
     *
     * `Filtered` is what the vertex stage's cell footprint removed from the bands that
     * DISPLACE -- mip levels on the spectral path, dropped octaves on the Gerstner one --
     * and it is what makes the far field a widening specular lobe rather than the glass
     * plane the filtering leaves behind. Below, the short band adds what its own distance
     * fade removed from the bands that only SHADE.
     *
     * Combined with max, not summed: each is already a fraction of the same surface's slope
     * energy, and adding them would push the horizon past rippled on the strength of
     * double-counted overlap.
     */
    float handover = Filtered;
    float foam = 0.0;
    if (waveModel == 1) {
        // The short cascade shades but never displaces. Carried into the mesh it
        // would alias into a ridged texture; carried into the NORMAL alone it
        // shimmers as the waves recede past a pixel. So its slope is faded out
        // with distance and its remaining energy moved into roughness instead --
        // the geometry-to-BRDF transition, which is what keeps the horizon stable
        // without throwing the simulated detail away.
        // Sampled at the DISPLACED position, where the vertex stage samples at the
        // undisplaced grid parameter. The two lattices are offset by the long and medium
        // horizontal displacement, and that is deliberate: this band shades the surface
        // the eye sees, so its detail belongs where that surface ended up.
        // LOD 0 explicitly. oceanCascadeUv wraps with fract, so the implicit screen
        // derivative reads a whole period across every tile seam -- which, the moment this
        // band's texture carries mips, prints as a blurred line through each of them. The
        // short band is not mipped today and this still states the intent rather than
        // relying on it.
        vec2 shortUv = oceanCascadeUv(WorldPos.xz, 2);
        vec4 short0 = textureLod(cascade2_0, shortUv, 0.0);
        vec4 short1 = textureLod(cascade2_1, shortUv, 0.0);
        float fade = 1.0 - smoothstep(WATER_SHORT_NEAR, WATER_SHORT_FAR, length(ViewPos));
        vec2 shortSlope = short1.rg * fade;
        N = normalize(N + vec3(-shortSlope.x, 0.0, -shortSlope.y) * WATER_SHORT_SLOPE_GAIN);
        // Roughness takes the slope energy the fade REMOVED -- the unfaded slope
        // times (1 - fade) -- which is what makes this a handover rather than two
        // channels dimming together. Driving it from the faded slope instead sent
        // distant water toward the calm value in both, so the horizon got smoother
        // as its waves went sub-pixel, which is backwards.
        vec2 handedOver = short1.rg * (1.0 - fade);
        handover = max(handover, smoothstep(0.012, 0.30, length(handedOver)));

        // Foam where the horizontal map COMPRESSES, from the vertex Jacobian plus
        // the short band's own. Deformation, not a height threshold: a tall smooth
        // swell does not break and a compressing one does, and selecting on height
        // puts white on the wrong crests.
        float shortJ = oceanBandJacobian(short0, short1, cascadeChoppiness[2]);
        float compression = max(0.0, 1.0 - Jacobian) + max(0.0, 1.0 - shortJ) * fade * 0.62;
        foam = smoothstep(WATER_FOAM_ON, WATER_FOAM_FULL, compression) * fade;
    }
    // Shore foam, on both wave models. A band where the bed has risen close to the
    // surface but has not broken it -- the whitewater a beach carries even where nothing
    // is breaking. Windowed on both sides: at shoal 0 the surface is about to be discarded
    // anyway, and at 1 there is no shore to foam against.
    //
    // No bedAvailable guard: with no bed the shoal factor is exactly 1 everywhere, so the
    // window's upper edge already zeroes this. The uniform test enforced the same fact a
    // second time.
    float band = smoothstep(0.02, 0.22, Shoal) * (1.0 - smoothstep(0.30, 0.72, Shoal));
    foam = max(foam, band * WATER_SHORE_FOAM);

    float roughness = mix(waterRoughness, WATER_ROUGH_RIPPLED, clamp(handover, 0.0, 1.0));

    vec3 V = normalize(-ViewPos);
    // The interface is shaded in view space, so the normal has to arrive there
    // too -- the surface normal is authored in world space by ocean.glsl.
    vec3 Nv = normalize(mat3(view) * N);
    // Face the eye. Two separate cases need it, and both rasterize because culling
    // is off: the whole surface seen from underneath, and the back sides of steep
    // crests seen from above. Left unflipped, dot(Nv, V) goes negative -- full
    // Fresnel with no transmitted share, and a refract() whose incident ray is on
    // the same side as the normal, which returns a direction on the wrong side of
    // the interface and sends the refraction sample to an arbitrary texel.
    if (cameraSubmerged == 1 || !gl_FrontFacing)
        Nv = -Nv;
    float NdotV = clamp(dot(Nv, V), 0.0, 1.0);

    // Optical path through the body: the view-Z gap between this surface and
    // whatever the depth buffer holds behind it, stretched from planar depth onto
    // the actual view ray. Without the stretch a grazing sight line would be
    // charged the vertical column and read too shallow exactly where water is
    // deepest-looking.
    float surfaceDist = max(-ViewPos.z, 1e-4);
    // Uniform: one value for the whole draw, so nothing below diverges per fragment and the
    // fwidth further down keeps the uniform control flow it documents.
    float maxPath = max(WATER_MAX_PATH,
                        WATER_MAX_OPTICAL_DEPTH /
                            max(min(min(waterAbsorption.r, waterAbsorption.g),
                                    waterAbsorption.b),
                                1e-4));
    float path = maxPath;
    // How much of this pixel still has water in it. 1 everywhere but the shoreline.
    float coverage = 1.0;
    if (cameraSubmerged == 1) {
        // From below, the body is between the EYE and the surface rather than
        // beyond it, so the optical path is the sight line itself. The depth buffer
        // behind the surface describes air and has nothing to say about it.
        path = length(ViewPos);
    } else if (sceneDepthAvailable == 1 && texture(sceneDepthTex, uv).r < WATER_DEPTH_EMPTY) {
        /*
         * There is geometry behind this fragment, so the column can be measured.
         *
         * The guard is load-bearing and its absence was a visible defect (spec 11.35). Where
         * nothing was drawn the depth buffer holds its cleared value, which reads back as the
         * FAR PLANE -- and the surface itself now runs to the horizon, far past any far plane.
         * So `bedDist - surfaceDist` came out hugely negative out there, the shoreline test
         * read it as "the bed has come up through the surface", and the discard removed every
         * fragment past the last geometry in the scene. It printed as a band of empty sky
         * between the sea and the horizon, whose lower edge sat exactly at the outermost mesh:
         * a water bug that looked like a sky bug.
         *
         * No bed behind means OPEN WATER, which is the `path = maxPath` and
         * `coverage = 1` this branch is skipped in favour of.
         */
        float bedNdc = texture(sceneDepthTex, uv).r * 2.0 - 1.0;
        float bedDist = -viewZFromNdcZ(bedNdc);
        float rayScale = length(ViewPos) / surfaceDist;
        // Signed, and kept signed until coverage has been taken from it: clamping
        // to zero first flattens the derivative on the dry side of the edge, which
        // halves the width the transition is measured over.
        float signedPath = (bedDist - surfaceDist) * rayScale;
        // Shoreline. Where the column has closed the surface has come up through
        // the bed, and what is left is a sliver almost coplanar with it: depth
        // rounding decides which wins per pixel and it reads as a dotted edge
        // rather than as a thin film. So the sliver goes -- but the boundary it
        // leaves is a hard step across one pixel, and an edge inside the opaque
        // lane gets no smoothing from anything downstream.
        //
        // Derivative coverage over that step: the distance to the threshold
        // divided by how fast the column changes per pixel is the threshold's
        // offset MEASURED IN PIXELS, so a half-covered pixel reads 0.5 whatever
        // the scene scale or the camera angle. Alpha-to-coverage then spends it as
        // samples. Only pixels the edge actually crosses come out fractional; a
        // silhouette of submerged geometry that happens to sit within one pixel
        // width of the surface is the one place this reads low where the water is
        // whole, and it is a pixel wide, at a shoreline, where the water is
        // already going.
        float edge = signedPath - WATER_MIN_PATH;
        coverage = clamp(edge / max(fwidth(edge), 1e-5) + 0.5, 0.0, 1.0);
        path = max(signedPath, 0.0);
        // One rule and one discard. A single-sample target has nothing to dither into, so
        // there the fraction rounds to the cutoff -- keeping a fractional fragment would
        // write the sliver at full strength. fwidth above still sits in uniform control
        // flow, before any discard.
        if (alphaToCoverage == 0)
            coverage = step(0.0, edge);
        if (coverage < WATER_MIN_COVERAGE)
            discard;
    }
    path = min(path, maxPath);

    // Transmitted share: the scene behind, bent along the refracted ray and then
    // absorbed over the full path. The bend itself is capped at WATER_MAX_BEND,
    // where the absorption is not.
    vec3 refrDir = refract(-V, Nv, 1.0 / waterIor);
    vec3 bed;
    if (sceneColorAvailable == 1) {
        vec3 exitView = ViewPos + refrDir * min(path, WATER_MAX_BEND);
        vec4 refrClip = projection * vec4(exitView, 1.0);
        vec2 refrUV = clamp(refrClip.xy / refrClip.w * 0.5 + 0.5, vec2(0.001), vec2(0.999));
        // Validity: the offset is a screen-space approximation of a world-space
        // bend, so it can walk onto something that is NOT under the water --
        // most cheaply the sky above the far shore, which arrives as bright
        // mottled patches wherever a wave happens to aim the ray there. If the
        // sample it landed on sits in FRONT of this surface, it was never behind
        // the water and the unbent sample is the honest answer.
        if (sceneDepthAvailable == 1) {
            float probeNdc = texture(sceneDepthTex, refrUV).r * 2.0 - 1.0;
            if (-viewZFromNdcZ(probeNdc) < surfaceDist)
                refrUV = uv;
        }
        bed = textureLod(sceneColorTex, refrUV, roughness * WATER_TRANSMISSION_MAX_LOD).rgb;

        // Caustics, on whatever the surface is refracting -- the bed, a rock, a
        // hull. Walk back along the refracted sun ray to where it crossed the
        // surface, and read the compression of the cascades THERE: a converging
        // patch of surface is a lens, and its focus is what brightens the floor.
        //
        // FFT only, and that is principled rather than a gap: caustics come from
        // compression, and the Gerstner path's steepness is clamped precisely so
        // its mapping cannot compress. The same reasoning gates its foam.
        if (waveModel == 1 && sunAvailable == 1 && causticsEnabled == 1) {
            vec2 crossing = WorldPos.xz - sunDir.xz / max(sunDir.y, 0.12) * path * 0.18;
            vec2 uvMed = oceanCascadeUv(crossing, 1);
            vec2 uvShort = oceanCascadeUv(crossing, 2);
            // LOD 0: `crossing` walks with the sun ray and the path length, so its screen
            // derivative describes neither the surface nor a footprint, and the medium band
            // IS mipped. Caustics are a near-field effect anyway -- WATER_CAUSTIC_DEEP_OFF
            // closes them well before a cell covers a period.
            float mj = oceanBandJacobian(textureLod(cascade1_0, uvMed, 0.0),
                                         textureLod(cascade1_1, uvMed, 0.0),
                                         cascadeChoppiness[1]);
            float sj = oceanBandJacobian(textureLod(cascade2_0, uvShort, 0.0),
                                         textureLod(cascade2_1, uvShort, 0.0),
                                         cascadeChoppiness[2]);
            float focus = max(0.0, 1.0 - mj) * 0.48 + max(0.0, 1.0 - sj) * 0.52;
            float window = smoothstep(0.0, WATER_CAUSTIC_SHALLOW, path) *
                           (1.0 - smoothstep(WATER_CAUSTIC_DEEP_ON, WATER_CAUSTIC_DEEP_OFF, path));
            float focused =
                pow(smoothstep(WATER_CAUSTIC_ON, WATER_CAUSTIC_FULL, focus), 2.0) * window;
            bed *= 1.0 + focused * WATER_CAUSTIC_GAIN;
        }
    } else {
        bed = vec3(0.0);
    }
    vec3 T = exp(-waterAbsorption * path);
    vec3 body = bed * T + waterScatter * preExposure * (1.0 - T);

    // Reflected share: the split-sum environment lobe, the same lookup every
    // other material makes. F0 0.020 falls out of IOR 1.333 rather than being
    // authored.
    float f0s = (waterIor - 1.0) / (waterIor + 1.0);
    vec3 F0 = vec3(f0s * f0s);
    vec3 F = fresnelSchlickRoughness(NdotV, F0, roughness);
    vec2 ab = texture(brdfLUT, vec2(NdotV, roughness)).rg;
    vec3 specWeight = F * ab.x + ab.y;
    vec3 reflected = vec3(0.0);
    if (iblEnabled > 0) {
        vec3 R = reflect(-V, Nv);
        vec3 Rw = normalize(mat3(transpose(view)) * R);
        reflected = textureLod(prefilteredMap, Rw, roughness * maxReflectionLOD).rgb *
                    iblIntensity * preExposure;
    }

    // Energy split: what the interface reflects it does not transmit.
    vec3 color = body * (1.0 - specWeight) + reflected * specWeight;

    // Foam sits ON the interface, so it replaces both halves rather than being
    // added to them: whitewater is opaque, and adding it would let the body colour
    // show through a surface that is full of air.
    if (foam > 0.0) {
        vec3 lit = WATER_FOAM_COLOR * (iblEnabled > 0 ? iblIntensity * preExposure : preExposure);
        color = mix(color, lit, clamp(foam, 0.0, 1.0) * WATER_FOAM_MAX);
    }

    // Alpha is the shoreline coverage, in BOTH writes below, because which output
    // the driver derives alpha-to-coverage from is not something a shader gets to
    // choose. These two are the only outputs whose alpha water does not otherwise
    // need: FragColor's is inert in the opaque lane, where nothing blends, and the
    // normals' is a marker read for its SIGN. The three above them each carry a
    // value a consumer reads -- aux roughness, albedo metalness, SSS profile -- so
    // coverage cannot be put there to be safe.
    //
    // Which means this rests on the driver NOT reading the highest-numbered alpha
    // it can find, and that is measured rather than assumed: albedo binds under
    // --ssgi with a 0 in its alpha, above both of these, and the shoreline still
    // moves by the same 4.4k px it moves without it. If the rule were "highest
    // alpha-bearing output", zero coverage from there would have erased the
    // surface entirely.
    FragColor = vec4(min(color, vec3(WS_SCENE_MAX)), coverage);
    // Non-negative alpha, so NOT the catcher's negative reflective marker: SSR
    // only shades the catcher, and giving water the marker would put it in a
    // channel whose magnitude is already the catcher's edge falloff -- the two
    // are not separable there (see the spec's phase 1b note).
    NormalOut = vec4(Nv, coverage);
    // Linear view-Z in .z is what makes the atmosphere composite fog this
    // surface at the water's own depth rather than the bed's.
    VelocityOut = packVelocityAux(ViewPos.z, roughness);
    // Alpha is METALNESS here, not coverage. Water is a dielectric, and 1.0 would
    // tell SSGI to give it no indirect diffuse (kD = 1 - alpha) and tell the
    // spec-occ composite to hand nearly all its energy to the specular answer.
    AlbedoOut = vec4(waterScatter, 0.0);
    DiffuseOut = vec4(0.0);
    // No ambient specular routed out for occlusion: water's reflection stays in
    // FragColor above, so there is nothing here for the spec-occ composite to
    // scale and nothing double-counted by leaving it at zero.
    SpecOut = vec4(0.0);
}
