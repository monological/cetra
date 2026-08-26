#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Final post pass: composite bloom onto the linear HDR scene, then apply
// exposure, ACES tone mapping, and gamma. Mode 0 is a raw copy for frames
// that are already display-ready (debug render modes, LDR-authored apps).
// hdrTex arrives pre-exposed; this pass reads it in working space and only
// applies the residual EV bias, so WS_SCENE_MAX below is the same ceiling the
// shading passes wrote under.
#include "view.glsl"
#include "noise.glsl"
// Declares purkinjeAdaptTex on unit 7 -- the metering 1x1, which is why this
// program samples 13 of 16 rather than 12.
#include "purkinje.glsl"

uniform sampler2D hdrTex;
uniform sampler2D bloomTex;
uniform sampler2D aoTex; // Blurred SSAO, upsampled by its linear filter
uniform float bloomStrength;
uniform int bloomEnabled;
// Lens flare (spec 11.21), quarter post-res, composited with bloom below.
uniform sampler2D flareTex;
uniform float flareStrength;
uniform int flareEnabled;
uniform int aoEnabled;
uniform float aoStrength;
uniform sampler2D normalsTex; // Resolved view-space normal .xyz + SSR marker .a
uniform sampler2D ssrTex;     // Half-res reflection buffer
uniform sampler2D albedoTex;  // Resolved albedo G-buffer (SSGI)
uniform sampler2D giTex;      // Half-res gathered GI radiance (SSGI)
// Specular occlusion: GTAO approximates DIFFUSE occlusion, so multiplying it
// onto specular/reflections is wrong -- it darkens (and, at silhouettes,
// shimmers) a bright grazing specular. These recover the reflection: aux .z/.w
// give view-Z + roughness, normalsTex the view normal, for a Lagarde term.
uniform sampler2D auxTex;       // Aux G-buffer: linear view-Z (.z) + roughness (.w)
uniform vec2 invFocal;          // 1/projection focal terms, for view-pos reconstruction
uniform vec2 aoRes;             // aoTex's own size, which is HALF the render res
uniform int specOccMode;        // 0 off, 1 legacy smoothness blend, 2 split (applied in the composite)
uniform int specOccHasMetallic; // albedoTex.a carries metallic this frame (SSGI wrote it)
uniform sampler2D csTex;        // Contact-shadow visibility (spec 9.3), full internal res
uniform sampler2D specOccTex;   // Reflection-lobe sums (.r visible, .g total), half render res
uniform int csEnabled;          // Multiply the direct-light term by contact shadows
uniform float csStrength;       // Contact-shadow darkening weight
// Debug view dispatch (PostFXDebugView): 0=none, 1=AO, 2=normals, 3=SSR,
// 4=albedo, 5=GI, 6=fog, 7=spec-occ AO, 8=contact shadows, 9=bent normal
uniform int debugView;
// 1 = ACES, 2 = PBR Neutral, 3 = AgX (passthrough frames are blitted by
// postfx_run and never reach this pass)
uniform int tonemapMode;
uniform vec2 texelSize; // Display-pixel size, for the sharpen taps

// Finishing grade — a "look" stack applied after tone mapping. Each stage is
// gated by its own toggle; with all off the output is the plain tonemapped
// frame. Order: sharpen -> grade -> vignette -> gamma -> LUT -> grain -> dither.
// Dither is last and must stay last: it is the quantization stage, so anything
// appended after it reaches the 8-bit target undithered.
//
// THE GAMMA ENCODE IS THIS STACK'S DIVIDING LINE, and it is what decides where
// a new stage goes. There are now two grading stages on opposite sides of it,
// which looks arbitrary and is not:
//
//   Before it, on LDR-linear values, live the operators this engine DEFINES.
//   Nothing external constrains their space, so we pick one and calibrate the
//   defaults to it -- lift/gamma/gain and the vignette are both this kind, and
//   postfx_apply_film_look's hand-tuned values only mean what they mean here.
//
//   After it, on display-encoded values, live stages whose DATA was authored in
//   that space. They are not sited by preference; the file decides.
//
// The LUT is the second kind. A .cube is authored against what a monitor was
// showing, so applying it to the LDR-linear numbers toneSelect returns would
// produce a plausible frame that is not the look the colourist made. It goes
// before the grain because grain is sensor noise laid over a finished look, not
// something a look should be graded through.
//
// Note the two compose rather than replace: --film turns the lift/gamma/gain
// grade ON, so --film with a .cube applies this engine's look underneath the
// colourist's. Defensible, and worth knowing before blaming the table.
//
// AND THERE IS A THIRD RULE, which the gamma line above does not reach: stages
// that model a physical step of IMAGE FORMATION are ordered by the OPTICAL
// CHAIN, not by anyone's data space. Chromatic aberration is sited that way
// below ("a LENS effect: it acts on the light before the sensor"), and grain is
// sited that way here ("sensor noise"). The Purkinje shift (spec 11.83) is the
// sensor's own SPECTRAL RESPONSE, so it lands between them -- after the lens,
// before the response curve. The whole chain now reads:
//
//   lens aberration -> lens scatter (bloom, flare) -> retinal spectral response
//   -> the response curve (toneSelect) -> sharpen -> grade -> vignette -> gamma
//   -> LUT -> sensor noise (grain) -> quantization (dither)
//
// Which is why Purkinje is NOT in this stack: everything listed above is
// downstream of the tonemap, and a retina is not.
uniform int sharpenEnabled;
uniform float sharpenStrength;
uniform int gradeEnabled;
uniform vec3 gradeLift;  // Black point / shadow raise (0 = none)
uniform vec3 gradeGamma; // Per-channel midtone curve (1 = none)
uniform vec3 gradeGain;  // White point / highlight scale (1 = none)
uniform int vignetteEnabled;
uniform float vignetteStrength;
uniform float vignetteRadius;
uniform int caEnabled;    // Chromatic aberration; separates channels radially
uniform float caStrength; // Channel separation at the CORNER, in pixels
uniform int grainEnabled;
uniform float grainStrength;
uniform float grainSeed; // Per-frame, deterministic across equal --frames runs
uniform int ditherEnabled;
uniform float ditherStrength; // Peak dither amplitude in 8-bit LSB (1 = textbook TPDF)
// 3D colour-grading LUT (spec 11.58), display-referred; see the note above.
uniform sampler3D lutTex;
uniform int lutEnabled;
uniform float lutSize;     // LUT_3D_SIZE of the loaded table, read from the file
uniform float lutStrength; // Blend toward the graded result (0 = bit-exact identity)
uniform int lutInterp;     // 0 trilinear, 1 tetrahedral

// ACES filmic fit (Narkowicz 2015). High contrast: crushes shadows,
// desaturates highlights — the cinematic look.
vec3 acesTonemap(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14),
                 0.0, 1.0);
}

// Khronos PBR Neutral (2024). Identity below ~0.76 so shadows, midtones,
// and material colors stay faithful; only highlights are compressed.
// Made for product/model viewers where albedo fidelity matters.
vec3 pbrNeutralTonemap(vec3 color)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
        return color;

    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    // Weight toward white, and the leading 1.0 - is load-bearing. Without it the
    // blend inverts: slight compression (peak just over startCompression, so
    // peak - newPeak near 0) would desaturate ~fully instead of not at all, and
    // the curve stops being a roll-off and becomes a hard clip at 0.76 -- every
    // highlight snapping to a flat white plateau while still looking, in code,
    // like tonemapping.
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, newPeak * vec3(1.0), g);
}

// 6th-order polynomial fit of the AgX base sigmoid
vec3 agxContrastApprox(vec3 x)
{
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 +
           0.1191 * x - 0.00232;
}

// AgX (Sobotka; Blender 4+'s default view transform), Wrensch's fitted
// minification. Desaturates toward white as radiance climbs, so saturated
// HDR highlights roll off without the hue skew ACES produces. The inset/
// outset matrices and sigmoid coefficients are the published constants and
// are inverses of each other through the curve's 2.2 encoding -- transcribe
// exactly or everything takes a global color cast.
vec3 agxTonemap(vec3 c)
{
    const mat3 AGX_INSET = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                0.0784335999999992, 0.878468636469772, 0.0784336,
                                0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 AGX_OUTSET = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                                 -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                                 -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float MIN_EV = -12.47393;
    const float MAX_EV = 4.026069;
    const float INV_EV_RANGE = 1.0 / (MAX_EV - MIN_EV);

    c = AGX_INSET * c;
    // log2 of black is -inf; the floor keeps the clamp finite
    c = clamp(log2(max(c, vec3(1e-10))), MIN_EV, MAX_EV);
    c = (c - MIN_EV) * INV_EV_RANGE;
    c = agxContrastApprox(c); // output is 2.2-encoded by construction
    c = AGX_OUTSET * c;
    // Linearize: toneSelect's contract is LDR-linear, displayEncode does
    // the gamma later
    return pow(max(c, vec3(0.0)), vec3(2.2));
}

// Gamma-encode a linear [0,1] color for display.
vec3 displayEncode(vec3 c)
{
    return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
}

// A 3D LUT's lattice points are at TEXEL CENTRES, so [0,1] has to be remapped
// into [0.5/N, (N-0.5)/N] before sampling. Without this the table is addressed
// by texel EDGE and the whole grade slides half a cell: an identity table stops
// being an identity, by a few 8-bit codes at the ends, which reads as a faint
// wash rather than as anything being wrong.
//
// N comes from the file. Inlining a size here would work on every table that
// happened to share it and silently mis-address the rest.
//
// TRILINEAR ONLY. lutTetrahedral addresses texels by integer index and never
// needs this, so an identity table stays an identity there whatever this
// function does -- which means the default path cannot see a mistake in it, and
// the trilinear leg of the identity arm is what covers it. Measured at 8 codes
// with the remap dropped, against 1 code with it.
vec3 lutCoord(vec3 c)
{
    return (clamp(c, 0.0, 1.0) * (lutSize - 1.0) + 0.5) / lutSize;
}

// Hardware trilinear: one fetch, and the texture unit does the eight-corner
// blend. Kept alongside tetrahedral below rather than replaced by it, because
// it is the only reference a frame contains for what tetrahedral is supposed to
// differ from.
vec3 lutTrilinear(vec3 c)
{
    return texture(lutTex, lutCoord(c)).rgb;
}

// Tetrahedral: the cube cell is cut into six tetrahedra, the ordering of the
// three fractional coordinates picks which one the sample is in, and four
// corners are blended by barycentric weights. What Resolve, Nuke and Baselight
// do, and it differs from trilinear in two ways that matter here.
//
// The NEUTRAL AXIS is the reason it is the default. All six tetrahedra share
// the (0,0,0)-(1,1,1) diagonal as an edge, so a grey sample puts all its weight
// on the two grey corners and none on the six that are not — r == g == b in
// gives r == g == b out, exactly. Trilinear's eight-corner blend always reaches
// off the diagonal and tints it.
//
// And the weights are computed HERE, in the shader, where trilinear's come from
// the texture unit's fixed-point subtexel fraction. That is the precision that
// decides whether an identity table is an identity, and it is not a precision
// this code controls.
vec3 lutTetrahedral(vec3 c)
{
    float d = lutSize - 1.0;
    vec3 p = clamp(c, 0.0, 1.0) * d;
    vec3 base = min(floor(p), vec3(d - 1.0));
    vec3 f = p - base;
    ivec3 i = ivec3(base);

    vec3 c000 = texelFetch(lutTex, i, 0).rgb;
    vec3 c111 = texelFetch(lutTex, i + ivec3(1, 1, 1), 0).rgb;
    vec3 ca, cb, w;
    if (f.r > f.g) {
        if (f.g > f.b) { // r > g > b
            ca = texelFetch(lutTex, i + ivec3(1, 0, 0), 0).rgb;
            cb = texelFetch(lutTex, i + ivec3(1, 1, 0), 0).rgb;
            w = vec3(f.r - f.g, f.g - f.b, f.b);
        } else if (f.r > f.b) { // r > b > g
            ca = texelFetch(lutTex, i + ivec3(1, 0, 0), 0).rgb;
            cb = texelFetch(lutTex, i + ivec3(1, 0, 1), 0).rgb;
            w = vec3(f.r - f.b, f.b - f.g, f.g);
        } else { // b > r > g
            ca = texelFetch(lutTex, i + ivec3(0, 0, 1), 0).rgb;
            cb = texelFetch(lutTex, i + ivec3(1, 0, 1), 0).rgb;
            w = vec3(f.b - f.r, f.r - f.g, f.g);
        }
    } else {
        if (f.b > f.g) { // b > g > r
            ca = texelFetch(lutTex, i + ivec3(0, 0, 1), 0).rgb;
            cb = texelFetch(lutTex, i + ivec3(0, 1, 1), 0).rgb;
            w = vec3(f.b - f.g, f.g - f.r, f.r);
        } else if (f.b > f.r) { // g > b > r
            ca = texelFetch(lutTex, i + ivec3(0, 1, 0), 0).rgb;
            cb = texelFetch(lutTex, i + ivec3(0, 1, 1), 0).rgb;
            w = vec3(f.g - f.b, f.b - f.r, f.r);
        } else { // g > r > b
            ca = texelFetch(lutTex, i + ivec3(0, 1, 0), 0).rgb;
            cb = texelFetch(lutTex, i + ivec3(1, 1, 0), 0).rgb;
            w = vec3(f.g - f.r, f.r - f.b, f.b);
        }
    }
    return (1.0 - (w.x + w.y + w.z)) * c000 + w.x * ca + w.y * cb + w.z * c111;
}

// Colour-grade with the loaded table. Applied to a display-encoded value.
vec3 lutApply(vec3 c)
{
    vec3 graded = lutInterp == 1 ? lutTetrahedral(c) : lutTrilinear(c);
    // mix(a, b, 0.0) is exactly `a`, so strength 0 is a bit-exact identity and
    // not merely a small one.
    return mix(c, graded, lutStrength);
}

// The frame's selected tonemap curve, shared by the scene path and the
// HDR debug views. Returns LDR-linear [0,1]; displayEncode gammas later.
vec3 toneSelect(vec3 c)
{
    if (tonemapMode == 1)
        return acesTonemap(c);
    if (tonemapMode == 3)
        return agxTonemap(c);
    return pbrNeutralTonemap(c);
}

// Screen-space AO visibility with specular occlusion. GTAO measures DIFFUSE
// hemispherical occlusion; a smooth reflection instead sees the environment in
// its mirror direction, past the local occluders GTAO sampled -- so it must not
// be dimmed (nor jittered) by that AO.
//
// Mode 1 (legacy) blends the AO toward unoccluded (1.0) by the pixel's
// specular fraction x smoothness: a smooth grazing specular (high dielectric
// Fresnel or a metal, low roughness) goes fully unoccluded, killing the AO's
// silhouette shimmer off it; diffuse/rough pixels keep the plain AO. It is
// directionless -- a reflection aimed into a wall is unoccluded as readily as
// one aimed at open sky.
//
// Mode 2 (split) normally does not reach this function at all: the composite
// pass applied the specular-occlusion term to the specular share and plain AO
// to the rest, and aoEnabled arrives false. If the composite could not run,
// aoEnabled stays true and mode 2 falls through to the legacy blend below --
// an error path by construction, since every way of reaching it logs first.
//
// A third mode sat here until spec 11.77: `bent`, which asked the directional
// question by intersecting a cone about the AO chain's bent normal with a cone
// about R. That question is now answered in the AO sweep against the sector
// bitmask itself, before anything is collapsed to a single direction, so the
// cone had no accuracy left to offer -- only the banding its own reconstruction
// introduced.
//
// Mode 0 returns raw ao -> byte-identical to the pre-feature path.
#include "ao_upsample.glsl"
#include "spec_occ.glsl"

// The AO buffer at this pixel, reconstructed rather than merely filtered.
//
// This pass runs at OUT res while aux is at render res, so the aux fetch is
// itself a magnification -- but aux is NEAREST, so it returns one whole texel's
// depth rather than a blend of two surfaces, which is the property the weights
// need. Wrapped because three call sites want it: the applied term and the two
// debug views, which show what the frame received for exactly that reason.
vec4 aoSampleAt()
{
    return aoFetchBilateral(aoTex, auxTex, TexCoords, aoRes, texture(auxTex, TexCoords).z);
}

// The reflection lobe's two sums, magnified the same way. Only debug view 7
// wants these -- split mode's applied term lands in the composite pass, well
// before this one -- so this exists to let that view show what the composite
// used rather than a second opinion about it.
vec2 specOccSampleAt()
{
    return aoFetchBilateral(specOccTex, auxTex, TexCoords, aoRes,
                            texture(auxTex, TexCoords).z)
        .rg;
}

float aoVisibility()
{
    vec4 aoSample = aoSampleAt(); // .r visibility, .gba encoded bent normal
    float ao = aoSample.r;
    if (specOccMode == 0)
        return ao;
    vec4 nrm = texture(normalsTex, TexCoords);
    // Real model surfaces only: a zero normal excludes sky/hair, and a
    // NEGATIVE marker excludes the shadow-catcher floor. The test is the
    // marker's sign, matching what the catcher writes and the SSR march
    // reads -- its magnitude is the catcher's edge falloff, so a threshold
    // at -0.5 would hand the plane's whole outer ring to the paths below,
    // which is where a mirror-roughness catcher meets the cone term.
    if (dot(nrm.xyz, nrm.xyz) < 0.01 || nrm.a < 0.0)
        return ao;
    vec4 aux = texture(auxTex, TexCoords); // .w = effective roughness
    // View direction from screen UV. normalize(-viewPos) is independent of depth
    // (same direction everywhere along a camera ray), so linZ cancels -- no need
    // to reconstruct the view position, just the ray direction from NDC + focal.
    vec2 ndc = TexCoords * 2.0 - 1.0;
    vec3 V = normalize(vec3(-ndc * invFocal, 1.0));
    vec3 N = normalize(nrm.xyz);
    float NdotV = clamp(dot(N, V), 0.0, 1.0);
    float metallic = specOccHasMetallic == 1 ? texture(albedoTex, TexCoords).a : 0.0;
    float fresnel = 0.04 + 0.96 * pow(1.0 - NdotV, 5.0); // dielectric specular fraction
    // How much of this pixel's energy is the specular lobe. The buffer being
    // multiplied holds diffuse and specular summed under one factor, so this
    // is what decides how far from the plain (diffuse) AO the result may move.
    // The smoothness factor is not decoration: Fresnel alone climbs to ~1 at
    // grazing, where a ROUGH dielectric's actual specular share is small, and
    // without it every grazing pixel hands its whole -- mostly diffuse --
    // energy to the specular answer.
    float specWeight = mix(fresnel, 1.0, metallic) * (1.0 - aux.w);

    return mix(ao, 1.0, specWeight);
}

// Scene HDR sample -> tonemapped LDR-linear [0,1], applying the shared AO
// factor and bloom addition (the same order the composite uses). Sharpen
// neighbour taps reuse the centre's aoFactor/bloomAdd so the mask measures
// scene edges, not AO/bloom gradients.
//
// No exposure here. The buffer arrives fully exposed -- camera AND adaptation
// both, applied at the scene passes (view.glsl) -- so this pass only maps
// working space to display.
// The one place this pass reads the scene, so aberration cannot apply to some
// samples and not others.
//
// Chromatic aberration is a LENS effect: it acts on the light before the
// sensor, which puts it ahead of the tonemap entirely and far ahead of grain,
// which is sensor noise that must not be resampled. Applying it here rather
// than to the composited result also keeps it in linear HDR, where a blown
// highlight is still hundreds of times white -- the fringe stays saturated
// instead of being clipped to a grey smear by the tone curve.
//
// Only the scene is offset. Bloom and flare are wide blurs where a two-pixel
// shift is invisible.
vec3 sceneTap(vec2 uv)
{
    if (caEnabled == 0)
        return texture(hdrTex, uv).rgb;
    // dir * r^2 collapses: normalising cancels one power of the radius, so
    // there is no division and no degenerate case at the optical centre.
    // caStrength is in PIXELS at the corner -- denominated that way because the
    // useful band is one to a few of them, and as a raw UV offset the sensible
    // values sat three decimal places down, where the shipped default and
    // slider range were both wrong by two orders of magnitude.
    vec2 toCentre = uv - 0.5;
    vec2 shift = toCentre * (length(toCentre) * caStrength / (0.7071 * 0.7071)) * texelSize;
    return vec3(texture(hdrTex, uv + shift).r, texture(hdrTex, uv).g,
                texture(hdrTex, uv - shift).b);
}

// Triangular-PDF dither in (-1, 1) LSB, one independent sample per channel.
//
// The subtrahend swaps and SCALES the coordinates rather than offsetting them.
// ign() is a sawtooth riding a linear ramp in p, so a constant offset shifts
// only that sawtooth's phase: ign(p) - ign(p + c) is two sawtooths at one
// frequency and collapses to four distinct values -- a staircase, not a dither.
// Changing the scale changes the frequency, which is what makes the taps
// independent.
//
// Each channel varies in both the swap and the scale for the same reason. Three
// channels sharing a frequency would dither only along the grey axis, leaving a
// colour gradient's contours standing in whichever channel banded elsewhere.
float ditherTap(vec2 p, float sa, vec2 oa, float sb, vec2 ob)
{
    return ign(p * sa + oa) - ign(p.yx * sb + ob);
}

// The scale pairs read down the column -- 1.00/1.37, 1.61/0.83, 0.71/2.13 --
// so a channel that lost its frequency split is visible without re-deriving it.
vec3 ditherPattern(vec2 p)
{
    return vec3(ditherTap(p, 1.00, vec2(0.0, 0.0), 1.37, vec2(19.0, 7.0)),
                ditherTap(p, 1.61, vec2(7.0, 31.0), 0.83, vec2(53.0, 11.0)),
                ditherTap(p, 0.71, vec2(31.0, 3.0), 2.13, vec2(97.0, 61.0)));
}

/*
 * The scene as this pass sees it, before any response curve: sanitize, occlude,
 * add lens scatter. ONE statement of that order, because three places need the
 * same one -- the composite below, the Purkinje pooling beside it, and debug
 * view 10. The sanitize is against a +INF texel from a half-float overflow
 * upstream, which both tonemap curves turn into NaN and a black pixel.
 */
vec3 sceneComposite(vec2 uv, float aoFactor, vec3 bloomAdd)
{
    return min(sceneTap(uv), vec3(WS_SCENE_MAX)) * aoFactor + bloomAdd;
}

/*
 * Rod spatial pooling (spec 11.83). Scotopic vision is INDISTINCT, not merely
 * grey -- many rods share a ganglion cell, buying sensitivity by spending
 * resolution -- and that is the half that reads as "I cannot quite see" rather
 * than as a blue filter.
 *
 * It lives HERE and not in purkinje.glsl because it resamples the SCENE, which
 * is this file's business: sceneTap and texelSize are already names in scope,
 * so the retina's include stays a file about spectral response and needs no
 * sampler, no texel size and no composite terms threaded into it.
 *
 * Four taps composited the same way the centre was, which is what stops the
 * pooling blurring the AO and bloom gradients -- those belong to the geometry
 * and the lens, not to the retina.
 */
vec3 purkinjePooled(vec2 uv, float aoFactor, vec3 bloomAdd, float w)
{
    float r = purkinjePoolRadius(w);
    return 0.25 * (sceneComposite(uv + vec2(texelSize.x, 0.0) * r, aoFactor, bloomAdd) +
                   sceneComposite(uv - vec2(texelSize.x, 0.0) * r, aoFactor, bloomAdd) +
                   sceneComposite(uv + vec2(0.0, texelSize.y) * r, aoFactor, bloomAdd) +
                   sceneComposite(uv - vec2(0.0, texelSize.y) * r, aoFactor, bloomAdd));
}

// Takes the tap's COORDINATE, not its sample: the two must agree, and passing
// both let a caller hand over a uv that did not produce the hdr -- a wrong
// unsharp mask that still looks exactly like a sharpen. Under --sharpen this
// runs at five different taps, and each must pool its OWN neighbourhood or the
// mask measures nothing.
vec3 sceneToToned(vec2 uv, float aoFactor, vec3 bloomAdd)
{
    vec3 c = sceneComposite(uv, aoFactor, bloomAdd);
    /*
     * The retina, before the response curve. Inside this function rather than
     * at the call site so all five sharpen taps see it: shifting the centre
     * while the neighbours stay un-shifted would make the unsharp mask
     * high-pass the Purkinje term itself, ringing every luminance edge. And
     * AFTER the sanitize above, which is what keeps the identity exact -- on a
     * +INF texel mix(c, INF * tint, 0.0) is NaN, not c.
     */
    if (purkinjeEnabled == 1 && purkinjeStrength > 0.0) {
        // The frame-uniform gate FIRST. In daylight it is exactly 0 while the
        // per-pixel gate is ~0.997, so a whole daylight frame exits here
        // without paying a log2 and a smoothstep per fragment -- five times
        // over under --sharpen. Its branch is wave-coherent; the per-pixel
        // one below is not.
        float wGlobal = purkinjeGlobalWeight();
        if (wGlobal > 0.0) {
            // Grouped left to right exactly as purkinjeWeight groups it, so the
            // shading path and the debug view cannot disagree by a rounding.
            float w = purkinjeStrength * purkinjeLocalWeight(c) * wGlobal;
            if (w > 0.0) {
                vec3 pooled = purkinjeAcuity > 0.0
                                  ? purkinjePooled(uv, aoFactor, bloomAdd, w)
                                  : c;
                c = purkinjeShift(pooled, w, uv, grainSeed);
            }
        }
    }
    return toneSelect(c);
}

void main()
{
    if (debugView == 1) {
        // The reconstructed value, not the stored one: a debug view answers
        // "what did this pixel get", and the half-res buffer is not what any
        // pixel gets. Reading it raw here would show a frame nothing renders.
        FragColor = vec4(vec3(aoSampleAt().r), 1.0);
        return;
    }
    if (debugView == 2) {
        // Remap to display range; unwritten texels (sky, hair) stay black
        vec3 n = texture(normalsTex, TexCoords).xyz;
        FragColor = vec4(dot(n, n) > 0.001 ? normalize(n) * 0.5 + 0.5 : vec3(0.0), 1.0);
        return;
    }
    if (debugView == 3) {
        // Raw reflection buffer, gamma-corrected so dim hits are visible
        FragColor = vec4(displayEncode(texture(ssrTex, TexCoords).rgb), 1.0);
        return;
    }
    if (debugView == 4) {
        // Albedo G-buffer (stored linear); gamma-encode for display
        FragColor = vec4(displayEncode(texture(albedoTex, TexCoords).rgb), 1.0);
        return;
    }
    if (debugView == 5) {
        // Gathered GI radiance (linear HDR); tone map so bright bounces
        // don't clip to white, gamma-encode for display
        vec3 gi = toneSelect(texture(giTex, TexCoords).rgb);
        FragColor = vec4(displayEncode(gi), 1.0);
        return;
    }
    // 6 was fog in-scatter, retired with the screen-space march (spec 9.5):
    // the froxel volume composites directly and has no 2D buffer to show.
    if (debugView == 7) {
        // Spec-occ AO visibility (what actually multiplies the scene) -- the
        // reflection relief vs the raw AO of debug view 1. Split mode shows
        // the term the composite applied to the specular share, through the
        // same shared function so this view cannot drift from it.
        // 2 is POSTFX_SPEC_OCC_SPLIT. The uniform carries the C enum's value
        // raw, so this literal and postfx.h are coupled by nothing but agreeing
        // -- and getting it wrong is quiet, since the wrong arm still returns a
        // plausible picture. ao-ring renders through this view for that reason.
        FragColor = vec4(vec3(specOccMode == 2
                                  ? specOccSplitAt(TexCoords, aoSampleAt(), specOccSampleAt())
                                  : aoVisibility()),
                         1.0);
        return;
    }
    if (debugView == 8) {
        // Contact-shadow visibility term before compositing (1 = lit)
        FragColor = vec4(vec3(texture(csTex, TexCoords).r), 1.0);
        return;
    }
    if (debugView == 9) {
        // Bent normal from the AO chain, re-encoded for display. Flat lit
        // surfaces read as their own normal; a crevice tilts away from the
        // occluder. Sky/hair (zero G-buffer normal) stay black.
        vec3 nrm = texture(normalsTex, TexCoords).xyz;
        vec3 bent = normalize(aoSampleAt().gba * 2.0 - 1.0);
        FragColor = vec4(dot(nrm, nrm) > 0.001 ? bent * 0.5 + 0.5 : vec3(0.0), 1.0);
        return;
    }

    // Occlude before adding bloom: bloom models lens scatter, which happens
    // after the light already left the scene
    float aoFactor = 1.0;
    // aoEnabled arrives false when the split composite already applied AO --
    // one C-side owner decides who multiplies, this pass never re-derives it.
    if (aoEnabled == 1)
        aoFactor = mix(1.0, aoVisibility(), aoStrength);
    // Contact shadows fold into the same factor (so the sharpen taps inherit
    // them like AO) but stay independent of AO/spec-occ: they occlude direct
    // light, not ambient. Exact identity at cs == 1, so a lit frame matches the
    // feature off bit for bit.
    if (csEnabled == 1)
        aoFactor *= 1.0 - csStrength * (1.0 - texture(csTex, TexCoords).r);
    vec3 bloomAdd = vec3(0.0);
    if (bloomEnabled == 1)
        bloomAdd = bloomStrength * texture(bloomTex, TexCoords).rgb;
    // Lens flare joins bloom's additive term rather than getting a composite
    // pass of its own: both are pre-tonemap light added to the scene, and
    // folding it in here means the sharpen taps below see it exactly as they
    // see bloom instead of sampling a different image.
    if (flareEnabled == 1)
        bloomAdd += flareStrength * texture(flareTex, TexCoords).rgb;

    // Chromatic aberration: a lens focuses wavelengths at slightly different
    // scales, so the channels separate along the radius and the effect vanishes
    // at the optical centre.
    //
    // Applied to the SCENE sample, not the composited result. It is a lens
    // effect, so it belongs on the light before the sensor -- ahead of the
    // tonemap, and well ahead of grain, which is sensor noise that must not be
    // resampled. Only the scene tap is offset: bloom and flare are wide blurs
    // where a two-pixel shift is invisible, and offsetting them would cost taps
    // for nothing.
    //
    // r^2, not r. Lateral chromatic aberration grows faster than linearly
    // toward the corners; a linear ramp reads as a uniform colour cast over the
    // whole frame instead of a corner artifact.
    /*
     * The rod weight (spec 11.83), and it is the ONE debug view that is not an
     * early return at the top of main(): its input is the composited scene
     * value, which does not exist until this line. Packed (w, wLocal, wGlobal)
     * so the two gates are separable -- "which one limited this frame" is the
     * first question a calibration session asks, and a single grey channel
     * cannot answer it.
     *
     * NOT display-encoded: these are weights, not colour. Read the raw bytes,
     * as --cs-debug's own note says. And deliberately NOT suppressed when the
     * feature is off, unlike the views gated on their source buffer -- "what
     * would this stage do here" is exactly the question you want answered while
     * deciding whether to switch it on.
     */
    if (debugView == 10) {
        FragColor = vec4(purkinjeWeight(sceneComposite(TexCoords, aoFactor, bloomAdd)), 1.0);
        return;
    }

    vec3 color = sceneToToned(TexCoords, aoFactor, bloomAdd);

    // Sharpen: unsharp mask on the tonemapped result (4-tap cross)
    if (sharpenEnabled == 1) {
        vec3 blur = sceneToToned(TexCoords + vec2(texelSize.x, 0.0), aoFactor, bloomAdd) +
                    sceneToToned(TexCoords - vec2(texelSize.x, 0.0), aoFactor, bloomAdd) +
                    sceneToToned(TexCoords + vec2(0.0, texelSize.y), aoFactor, bloomAdd) +
                    sceneToToned(TexCoords - vec2(0.0, texelSize.y), aoFactor, bloomAdd);
        color = clamp(color + sharpenStrength * (color - blur * 0.25), 0.0, 1.0);
    }

    // Colour grade: lift/gamma/gain (identity at the defaults)
    if (gradeEnabled == 1) {
        color = gradeLift + color * (gradeGain - gradeLift);
        color = pow(max(color, 0.0), 1.0 / gradeGamma);
    }

    // Vignette: radial edge darkening. 0.7071 is the centre-to-corner UV
    // distance, so radius is the fraction of that kept fully bright.
    if (vignetteEnabled == 1) {
        float d = length(TexCoords - 0.5);
        float falloff = smoothstep(vignetteRadius * 0.7071, 0.7071, d);
        color *= 1.0 - vignetteStrength * falloff;
    }

    // Gamma-encode to display space
    color = displayEncode(color);

    // Colour-grading LUT: display-referred, so it lands here and not earlier
    if (lutEnabled == 1)
        color = lutApply(color);

    // Film grain: display-space, weighted toward midtones (invisible in flat
    // black/white), animated by a deterministic per-frame seed
    if (grainEnabled == 1) {
        float n = hash21(gl_FragCoord.xy + grainSeed, vec2(12.9898, 78.233)) - 0.5;
        float luma = dot(color, vec3(0.299, 0.587, 0.114));
        float w = 1.0 - abs(2.0 * luma - 1.0);
        color = clamp(color + n * grainStrength * w, 0.0, 1.0);
    }

    // Dither the 8-bit write: a shallow gradient otherwise quantizes to contour
    // bands (sky, fog, bloom falloff). Last stage of this pass — anything added
    // after it would itself be quantized undithered. (The GUI and the SKY LUT
    // debug tiles draw to the same target afterwards; they are flat UI fills,
    // not image-forming, so they are deliberately left alone.)
    //
    // Static, with no frame term. An animated pattern would put roughly half the
    // frame's pixels 1 LSB apart between any two consecutive frames, and that
    // lands in every temporal-churn measurement taken off the final framebuffer
    // — including the no-feature floor arms those measurements are scaled
    // against, so both sides would inflate together and the comparison would
    // stop discriminating.
    // Faded out where the signal has no room for the full swing. A value
    // already at 0 or 1 carries no quantization error to decorrelate, and the
    // clamp below would keep only one half of the TPDF there -- flat white
    // stipples to 254 and flat black lifts off zero, which is a DC bias, not
    // noise. `room` is the distance to the nearer endpoint measured in units of
    // the dither's own amplitude, so full swing resumes as soon as the
    // distribution fits, and the fade widens correctly as the strength rises.
    if (ditherEnabled == 1) {
        float amp = ditherStrength * (1.0 / 255.0);
        vec3 room = clamp(min(color, 1.0 - color) / max(amp, 1e-6), 0.0, 1.0);
        color = clamp(color + ditherPattern(gl_FragCoord.xy) * amp * room, 0.0, 1.0);
    }

    FragColor = vec4(color, 1.0);
}
