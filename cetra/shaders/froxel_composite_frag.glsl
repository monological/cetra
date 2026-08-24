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
// The MBOIT atlas (spec 11.17): b1..b4 in the lower half, b0 in the upper. Read
// here for what the aux buffer structurally cannot hold -- see the second depth
// below. Point-sampled and addressed in normalized coordinates rather than off
// gl_FragCoord, because this pass runs at POST resolution and the atlas is at
// RENDER resolution; under --render-scale those differ.
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
#include "mboit.glsl" // MBOIT_ATLAS_B0_TAP: the atlas layout, stated once

// Both media in series, resolved at ONE depth.
//
// Factored out because the frame needs it at TWO. The aux buffer holds a single
// linear Z per pixel and the late pass never writes it -- render.c drops the
// draw-buffer count to 1 when the opaque scope closes -- so a translucent
// surface's own depth is simply absent from it, and everything downstream reads
// the surface BEHIND the glass instead. Water escapes this by writing aux like an
// opaque surface; a stack of translucent layers cannot, because one slot cannot
// hold both its depth and the wall's.
vec4 mediumAt(float dist, bool isSky, float camNearZ) {
    vec4 fogLayer =
        froxelSampleMedium(integratedVolume, TexCoords, isSky ? fogFar : dist, fogNear, fogFar,
                           froxelDepth, fogDepthDist);
    // Aerial keeps the pure exponential aerial_lut_frag builds it with, as well
    // as its own near.
    vec4 aerialLayer =
        froxelSampleMedium(aerialVolume, TexCoords, dist, camNearZ, aerialFar,
                           isSky ? 0 : aerialDepth, 1.0);

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
    float camNearZ = projection[3][2] / (projection[2][2] - 1.0);
    float linZ = texture(linDepthTex, TexCoords).z;

    // Sky/background: the aux buffer's sentinel is 0, there is no surface.
    bool sky = (linZ >= -1e-4);
    // PLANAR depth, not the ray length the screen-space march used: volume
    // slices are constant-depth planes (froxelViewPos puts every cell of a slice
    // at the same view z), so feeding it a radial distance would index deeper
    // the further a pixel sits off the optical axis.
    //
    // Sky takes fog's full column -- the far slice already holds it. It takes
    // NO aerial perspective, though: the sky background is drawn from the
    // sky-view LUT, which is the same integral the aerial volume holds, so
    // applying it there too would count the same air twice and wash the sky out.
    vec4 layer = mediumAt(-linZ, sky, camNearZ);

    // The translucent stack, at its own depth (spec 11.78).
    //
    // The moments already measure it: b0 is the stack's total absorbance along
    // this pixel, and b1/b0 is the absorbance-weighted mean of the WARPED depth,
    // so one atlas fetch gives both how much of the pixel is translucent and how
    // far off that part of it is. Nothing else in the frame carries either.
    //
    // The blend is affine in (in-scatter, transmittance), so weighting the two
    // answers by coverage is the same as fogging the two contributions separately
    // -- given that the translucent contribution is `cover` of the pixel. That is
    // exact when the layers carry the background's own radiance and when coverage
    // is 0 or 1, and an approximation between: a bright pane over a dark wall
    // takes a share of the wall's fog in proportion to how much of the pixel the
    // wall still supplies. The exact split wants the un-composited translucent
    // colour, and the accumulation buffer holding it is upstream of the TAA
    // resolve this pass deliberately runs after, so subtracting it here would mix
    // a filtered pixel with an unfiltered one.
    if (momentArmed == 1) {
        vec2 mUV = vec2(TexCoords.x, TexCoords.y * 0.5);
        float b0 = textureLod(momentTex, mUV + MBOIT_ATLAS_B0_TAP, 0.0).r;
        // No stack here: leave the surface answer exactly as it was computed, so
        // every pixel of every fog frame without translucency in front of it is
        // bit-identical to the frame before this feature existed.
        if (b0 > 1e-5) {
            float b1 = textureLod(momentTex, mUV, 0.0).r;
            // Invert mboitWarpDepth. It clamps its own output, so a mean of
            // clamped values is in range and this lands inside [near, far].
            float t = clamp(b1 / b0, -1.0, 1.0) * 0.5 + 0.5;
            float zTrans = oitNearFar.x * pow(oitNearFar.y / oitNearFar.x, t);
            // The stack transmits exp(-b0), so it accounts for the rest.
            float cover = 1.0 - exp(-b0);
            layer = mix(layer, mediumAt(zTrans, false, camNearZ), cover);
        }
    }

    FragColor = layer;
}
