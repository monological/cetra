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

uniform sampler2D linDepthTex;      // Aux G-buffer; .z = linear view Z (<0), 0 = sky
uniform sampler3D integratedVolume; // Fog: front-to-back (inscatter, transmittance)
uniform sampler3D aerialVolume;     // Atmosphere: same encoding, camera to cell
uniform sampler2D layerTex; // mode 1 only: an already-composited layer to fold
uniform mat4 projection;
uniform float fogFar;
uniform float aerialFar;
// Slice counts, and the medium's on/off state: zero means absent.
uniform int froxelDepth;
uniform int aerialDepth;
// 0 = composite from the volumes, 1 = fold layerTex under the same blend.
uniform int mode;

#include "froxel.glsl"

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

    float nearZ = projection[3][2] / (projection[2][2] - 1.0);
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
    vec4 fogLayer =
        froxelSampleMedium(integratedVolume, TexCoords, sky ? fogFar : -linZ, nearZ, fogFar,
                           froxelDepth);
    vec4 aerialLayer =
        froxelSampleMedium(aerialVolume, TexCoords, -linZ, nearZ, aerialFar,
                           sky ? 0 : aerialDepth);

    // Two media in series. Fog is the nearer one -- a local ground layer, where
    // the atmosphere spans the whole ray -- so the far medium's in-scatter is
    // attenuated by the near one and the transmittances multiply:
    //     S = S_fog + T_fog * S_aerial,   T = T_fog * T_aerial
    // Exact for two media in series given that ordering; the fully physical
    // answer is one medium in one volume, which is a later consolidation.
    FragColor = vec4(fogLayer.rgb + aerialLayer.rgb * fogLayer.a, fogLayer.a * aerialLayer.a);
}
