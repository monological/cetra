#version 330 core

// Water surface vertex stage (specs 11.32, 11.34). The mesh is a fixed lattice over
// [-0.5, 0.5]^2 read as SCREEN space, positions only: where each vertex lands on the water
// plane is a ray/plane solve in oceanProjectedPosition, and a displaced surface's normal is
// the derivative of the displacement, so a stored normal would only be overwritten.
layout(location = 0) in vec2 aGrid;

uniform mat4 view;
uniform mat4 projection; // TAA-jittered, like every other raster in the frame
uniform mat4 uCurrViewProjNoJitter;
uniform mat4 uPrevViewProj;
uniform float time;
uniform float uDeltaTime;
// The projector's origin. waterLevel and waterExtent are declared by ocean.glsl.
uniform vec3 waterCamPos;

out vec3 WorldPos;
out vec3 ViewPos;
out vec3 Normal;
out vec4 CurrClip;
out vec4 PrevClip;
out float Jacobian;
out float Shoal;

// Kept for symmetry with pbr_vert, where it is load-bearing because the depth
// prepass rasterizes the same triangles from a second program. Nothing rasterizes
// this lattice twice today, so here it is harmless rather than required -- and cheap
// insurance if a depth-only water pass ever appears.
invariant gl_Position;

#include "ocean.glsl"

/*
 * The lattice's far rows sit at OCEAN_FAR_DIST, which is past any camera's far plane, and
 * clipping them is exactly the defect 11.34 exists to fix: it leaves a band of sky between
 * the surface's top edge and the horizon. So clip Z is held just inside the far plane,
 * which keeps the vertex's screen position and its ordering against everything that is not
 * the sky.
 *
 * skybox_vert pins its own Z to exactly the far plane and the skybox pass tests LEQUAL, so
 * a surface held one code short of it still wins there. The epsilon has to stay above a
 * 24-bit depth buffer's own resolution (6e-8) or the two round to the same code.
 */
const float WATER_CLIP_Z_EPS = 1.0e-5;

void main() {
    vec2 p = oceanProjectedPosition(aGrid, view, projection, waterCamPos);

    // One shoal fetch per point, shared by the current surface and the previous position.
    // Both are functions of the same p, and asking twice cost a second bed texture fetch
    // plus its smoothstep and gradient on every vertex.
    vec3 sh = oceanShoal(p);
    OceanSurface s = oceanEvaluateAt(p, time, sh);
    float tPrev = time - uDeltaTime;
    vec3 prevWorld = oceanPreviousWorldAt(p, tPrev, sh);

    WorldPos = s.world;
    Normal = s.normal;
    Jacobian = s.jacobian;
    Shoal = s.shoal;

    vec4 viewPos = view * vec4(s.world, 1.0);
    ViewPos = viewPos.xyz;

    // Motion vectors from the UN-JITTERED pair while gl_Position below uses the
    // jittered projection. The jitter is a sub-pixel sampling offset; letting it
    // into the velocity would report it to TAA and motion blur as scene motion.
    //
    // Both wave models report the motion their waves have -- see oceanPreviousWorld
    // for how each one gets there and what the spectral path needs kept alive to do
    // it. Before that, a spectral ocean reported CAMERA motion only and TAA
    // reprojected travelling wave detail as though it were static.
    CurrClip = uCurrViewProjNoJitter * vec4(s.world, 1.0);
    PrevClip = uPrevViewProj * vec4(prevWorld, 1.0);

    vec4 clip = projection * viewPos;
    gl_Position = vec4(clip.xy, min(clip.z, clip.w * (1.0 - WATER_CLIP_Z_EPS)), clip.w);
}
