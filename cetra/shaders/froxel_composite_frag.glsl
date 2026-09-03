#version 330 core
in vec2 TexCoords;
out vec4 FragColor; // rgb = in-scatter to add, a = transmittance to multiply by

// Atmospheric media composite (specs 9.5, 9.6): fold the fog volume and the
// aerial-perspective volume into the HDR scene as a single layer. Full res, one
// trilinear tap each -- the reason both are real 3D textures and not the
// codebase's usual 2D array, which cannot filter across slices.
//
// Output is NOT premultiplied: the caller blends with
// glBlendFunc(GL_ONE, GL_SRC_ALPHA), giving scene*T + inscatter, exactly the
// contract the screen-space fog composite used. That blend applies ONE factor
// to the scene, which is why the two media are combined here analytically
// rather than drawn as two passes -- and combining them also lets the single
// temporal accumulator downstream stabilise both.

#include "view.glsl"

uniform sampler2D linDepthTex;      // Aux G-buffer; .z = linear view Z (<0), 0 = sky
uniform sampler3D integratedVolume; // Fog: front-to-back (inscatter, transmittance)
uniform sampler3D aerialVolume;     // Atmosphere: same encoding, camera to cell
uniform sampler2D layerTex; // mode 1 only: an already-composited layer to fold
// The MBOIT atlas (spec 11.17), read here for what the aux buffer structurally
// cannot hold -- see the second depth below. Its layout, and the point sampling
// it inherits, belong to mboit.glsl; this pass supplies a uv because it draws at
// two different sizes in its two modes and gl_FragCoord suits neither.
uniform sampler2D momentTex;
uniform vec2 oitNearFar; // the interval the moments' depth warp is stated over
uniform int momentArmed; // 1 = the atlas holds this frame's translucent stack
uniform mat4 projection;
uniform float fogNear;
uniform float fogFar;
uniform float fogDepthDist;
uniform float aerialFar;
// Slice counts, and the medium's on/off state: zero means absent.
uniform int froxelDepth;
uniform int aerialDepth;
// 0 = composite from the volumes, 1 = fold layerTex under the same blend.
uniform int mode;

#include "froxel.glsl"
#include "mboit.glsl" // the moment atlas, its fold and the warp's inverse

// Both media in series, resolved at ONE depth. `aerialSlices` is the caller's
// aerial slice count, which is zero for a ray that takes none.
vec4 mediumAt(float dist, int aerialSlices, float camNearZ) {
    vec4 fogLayer =
        froxelSampleMedium(integratedVolume, TexCoords, dist, fogNear, fogFar,
                           froxelDepth, fogDepthDist);
    // Aerial keeps the pure exponential aerial_lut_frag builds it with, as well
    // as its own near.
    vec4 aerialLayer =
        froxelSampleMedium(aerialVolume, TexCoords, dist, camNearZ, aerialFar,
                           aerialSlices, 1.0);

    // Two media in series. Fog is the nearer one -- a local ground layer, where
    // the atmosphere spans the whole ray -- so the far medium's in-scatter is
    // attenuated by the near one and the transmittances multiply:
    //     S = S_fog + T_fog * S_aerial,   T = T_fog * T_aerial
    // Exact for two media in series given that ordering; the fully physical
    // answer is one medium in one volume, which is a later consolidation.
    // The two media arrive in DIFFERENT spaces, which is the whole subtlety here.
    // Fog is already working space -- froxel_inject pre-exposes before its own
    // clamp, so converting it again would double-apply. Aerial comes from a LUT
    // baked upstream of the HDR buffer (aerial_lut_frag) and is still absolute
    // radiance, so it converts here. Getting this backwards puts the two media on
    // different scales and reads as fog or haze at the wrong strength rather than
    // as an error.
    //
    // Transmittance (.a) is a ratio in both and is never scaled.
    return vec4(fogLayer.rgb + aerialLayer.rgb * preExposure * fogLayer.a,
                fogLayer.a * aerialLayer.a);
}

void main() {
    // Fold pass: under TAA the layer produced by mode 0 is temporally
    // accumulated before it reaches the scene, and this copies the stabilized
    // result out under the blend the caller already has set. Keeping it in this
    // shader rather than borrowing a filter program is the sss_blur_frag mode-2
    // idiom, and it keeps the (inscatter, transmittance) contract in one file.
    if (mode == 1) {
        FragColor = texture(layerTex, TexCoords);
        return;
    }

    // The two media do NOT share a near. Fog's volume is built from fogNear
    // (postfx.c owns it); the aerial LUT is built by aerial_lut_frag from the
    // camera's, so sampling it against fog's would index the wrong slices --
    // silently, as haze at the wrong distance rather than as an error.
    float camNearZ = nearPlaneDist();
    float linZ = texture(linDepthTex, TexCoords).z;

    // Sky/background: the aux buffer's sentinel is 0, there is no surface.
    //
    // Sky takes fog's full column -- the far slice already holds it -- and NO
    // aerial perspective: the sky background is drawn from the sky-view LUT,
    // which is the same integral the aerial volume holds, so applying it there
    // too would count the same air twice and wash the sky out. A slice count of
    // zero is how a medium says it is absent.
    bool sky = (linZ >= -1e-4);
    // PLANAR depth, not the ray length the screen-space march used: volume
    // slices are constant-depth planes (froxelViewPos puts every cell of a slice
    // at the same view z), so feeding it a radial distance would index deeper
    // the further a pixel sits off the optical axis.
    vec4 layer = mediumAt(sky ? fogFar : -linZ, sky ? 0 : aerialDepth, camNearZ);

    // The translucent stack, at its own depth (spec 11.78).
    //
    // A translucent surface's own depth is not in the aux buffer, and one slot
    // cannot hold both it and the wall's. The moments can: b0 is the stack's
    // total absorbance along this pixel and b1/b0 the absorbance-weighted mean
    // of its warped depth, so one fetch gives both how much of the pixel the
    // stack supplies and how far off that part of it is.
    //
    // The blend is affine in (in-scatter, transmittance), so weighting the two
    // answers by opacity is the same as fogging the two contributions
    // separately. Exact when the layers carry the background's own radiance and
    // when opacity is 0 or 1; between, a bright pane over a dark wall takes a
    // share of the wall's fog in proportion to how much of the pixel the wall
    // still supplies. The exact split needs the un-composited translucent
    // colour, which is a SIGNED correction from an unfiltered buffer applied
    // against a canvas the TAA resolve has already filtered -- on a moving
    // silhouette that overshoots and can ring where TAA exists to stabilise.
    // A bounded approximation is the better trade.
    if (momentArmed == 1) {
        vec2 stack = mboitStackAtlas(momentTex, TexCoords);
        // Skips the second medium evaluation -- two volume taps and the series
        // fold -- on every pixel with nothing translucent in front of it, which
        // is most of a frame. It also keeps b1/b0 away from 0/0.
        if (stack.x > 1e-5) {
            float zTrans = mboitUnwarpDepth(stack.y / stack.x, oitNearFar);
            layer = mix(layer, mediumAt(zTrans, aerialDepth, camNearZ),
                        mboitStackOpacity(stack.x));
        }
    }

    FragColor = layer;
}
