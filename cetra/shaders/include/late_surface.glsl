// What a material drawn in the late draw needs from the frame (spec 13.29), so no app shader
// re-derives it: whether the frame's own surfaces hide it, the fog in front of it, and the
// pre-exposure its light is written at. The engine binds every uniform here by name.
//
// The canvas the late draw writes has no depth attachment, so hiding is a compare against the
// frame's resolved depth -- the fire's and the rain's way, and Unreal's in its "After Motion
// Blur" pass, which drops the depth test for the same reason: the depth was drawn under TAA's
// jitter, so it is a fraction of a pixel off where this surface lands.

uniform mat4 projection; // the unjittered projection; depth.glsl reads it
uniform vec2 viewport;   // the canvas's size in pixels (post resolution)
uniform sampler2D sceneDepth; // the frame's depth, resolved, at RENDER resolution
uniform sampler3D fogVolume;
uniform int fogSlices; // 0 = no fog volume this frame, which the fog reads as clear air
uniform float fogNear;
uniform float fogFar;
uniform float fogDepthDist;
uniform float time; // seconds, the render clock
uniform int frame;  // the frame index

#include "view.glsl"
#include "froxel.glsl"

// How much of a fragment `viewDepth` metres in front of the eye the frame leaves in view: 1 in
// front of the nearest surface there, 0 more than `slack` metres behind it, a ramp between. A
// surface laid on another (a screen over its glass) wants a slack of a few millimetres, so the
// depth's precision and the jitter do not hide it in patches.
float lateVisible(float viewDepth, float slack)
{
    vec2 screen = gl_FragCoord.xy / viewport;
    float surface = -viewZFromNdcZ(2.0 * texture(sceneDepth, screen).r - 1.0);
    return clamp((surface + slack - viewDepth) / max(slack, 1e-6), 0.0, 1.0);
}

// Light emitted `viewDepth` metres in front of the eye, in scene units, as it reaches the eye
// through the frame's fog and in the canvas's working space. For light ADDED to what is behind
// it (alpha 0): the fog's own glow in front is already in the canvas, from whatever the light is
// laid over.
vec3 lateEmit(vec3 radiance, float viewDepth)
{
    vec4 front = froxelSampleMedium(fogVolume, gl_FragCoord.xy / viewport, viewDepth, fogNear,
                                    fogFar, fogSlices, fogDepthDist);
    return radiance * preExposure * front.a;
}
