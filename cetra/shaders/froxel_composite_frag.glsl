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
uniform int froxelDepth;  // Fog slice count; mirrors POSTFX_FROXEL_Z
uniform int aerialDepth;  // Aerial slice count; mirrors SKY_AERIAL_Z
uniform int fogOn;
uniform int aerialOn;
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
    // Both volumes integrate to the FAR FACE of a slice, so layer s carries
    // continuous slice coordinate s+1 and a depth at coordinate `slice` is read
    // from texel centre (slice-0.5)/depth. CLAMP_TO_EDGE on R holds the fully
    // integrated column past the last slice.
    vec4 fogLayer = vec4(0.0, 0.0, 0.0, 1.0);
    if (fogOn != 0) {
        // Sky takes the full column: the far slice already holds it.
        float depth = sky ? fogFar : min(-linZ, fogFar);
        float slice = froxelViewZToSlice(depth, nearZ, fogFar, float(froxelDepth));
        fogLayer = texture(integratedVolume, vec3(TexCoords, (slice - 0.5) / float(froxelDepth)));
    }

    vec4 aerialLayer = vec4(0.0, 0.0, 0.0, 1.0);
    // Scene pixels only. The sky background is drawn from the sky-view LUT,
    // which is the same integral this volume holds -- applying it there too
    // would count the same air twice and wash the sky out.
    if (aerialOn != 0 && !sky) {
        float depth = min(-linZ, aerialFar);
        float slice = froxelViewZToSlice(depth, nearZ, aerialFar, float(aerialDepth));
        aerialLayer = texture(aerialVolume, vec3(TexCoords, (slice - 0.5) / float(aerialDepth)));
    }

    // Two media in series. Fog is the nearer one -- a local ground layer, where
    // the atmosphere spans the whole ray -- so the far medium's in-scatter is
    // attenuated by the near one and the transmittances multiply:
    //     S = S_fog + T_fog * S_aerial,   T = T_fog * T_aerial
    // Exact for two media in series given that ordering; the fully physical
    // answer is one medium in one volume, which is a later consolidation.
    FragColor = vec4(fogLayer.rgb + aerialLayer.rgb * fogLayer.a, fogLayer.a * aerialLayer.a);
}
