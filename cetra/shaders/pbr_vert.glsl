#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
layout(location = 3) in vec4 aTangent; // xyz tangent, w bitangent handedness
layout(location = 5) in vec4 aColor;
layout(location = 8) in vec2 aTexCoords2;

/*
 * CENTROID, and it is not a quality tweak -- it is what stops a partly covered pixel being
 * shaded from attributes that were never on the surface.
 *
 * With MSAA the fragment shader runs once per pixel and interpolates varyings at the pixel
 * CENTRE. On a pixel the primitive only partly covers, that centre can lie outside the
 * triangle, so every value here is EXTRAPOLATED past the vertices that bound it: a colour
 * leaves 0..1, a normal stops being a plausible direction, a UV leaves the chart. Thin
 * geometry is nothing but such pixels -- a grass blade a pixel wide is partly covered along
 * its whole length -- and apps/tree colours its grass entirely per vertex, so the
 * extrapolation goes straight into the shading and prints as bright specks that appear with
 * MSAA and vanish without it.
 *
 * `centroid` moves the sample to the centroid of the COVERED samples, which is inside the
 * primitive by construction, so the values stay within the ones the vertices actually carry.
 * It costs a different interpolation point on partly covered pixels and nothing on full ones.
 *
 * The qualifier is part of the interface and must match in the fragment stage, and in
 * pbr_skinned_vert, or the programs fail to link.
 */
out vec3 Normal;
out vec3 WorldPos;    // World position
out vec3 ViewPos;     // View position
out vec2 TexCoords;
out vec2 TexCoords2;  // UV1 for lightmaps/AO
centroid out vec4 VertexColor; // Vertex color (RGBA)
out mat3 TBN;
flat out float TangentW; // bitangent handedness, per-island constant
out vec4 CurrClip;     // Un-jittered current clip position (motion vectors)
out vec4 PrevClip;     // Previous-frame clip position

// No light data here on purpose: this stage does no lighting. Analytic lights
// live in the clustered-forward UBO blocks (lights_ubo.glsl), consumed by
// pbr_frag alone; the moment a vertex shader needs lighting, include it once.

uniform mat4 model;
// transpose(inverse(model)), uploaded per node (render.c). Normals need it
// rather than `model` under non-uniform scale; computing it here would be a
// full mat4 inverse per vertex for a value constant across the draw.
uniform mat3 uNormalMatrix;
uniform mat4 view;
uniform mat4 projection; // Jittered when TAA is on (rasterization only)

// Motion-vector inputs (un-jittered). Default to zero when unset, yielding zero
// velocity, which the TAA resolve treats as static.
uniform mat4 uCurrViewProjNoJitter;
uniform mat4 uPrevViewProj;
uniform mat4 uPrevModel;

uniform float time;
uniform float uDeltaTime; // render clock advance, for the previous-frame position

#include "wind.glsl"
#include "tbn.glsl"
#include "instancing.glsl"
#include "object_position.glsl"

// The depth prepass rasterizes these same triangles and this pass then tests
// against its depth with GL_LEQUAL, so this value has to be bit-identical
// between two programs compiled from different source files. Without the
// qualifier the driver is free to schedule the same arithmetic differently in
// each, and a fragment landing a bit BEHIND the depth it just wrote fails --
// surfaces disappearing, not shading subtly wrong.
invariant gl_Position;

void main() {

    // The per-object transforms, from the instance block or the uniforms. Read
    // once into locals so everything below is written the same way whichever
    // path supplied them.
    mat4 mModel = cetra_instance_model(model);
    mat4 mPrevModel = cetra_instance_prev_model(uPrevModel);
    mat3 mNormal = cetra_instance_normal_matrix(uNormalMatrix);

    // Wind displaces the object-space position; the previous-frame position uses
    // t - dt so the motion vector stays honest (no TAA/motion-blur smear).
    // No skeleton in this stage: identity bone, and false so the posed branch
    // folds away.
    vec4 local = cetra_local_position(aPos, mat4(1.0), false, aTexCoords, aTexCoords2, time);
    vec3 posPrev = aPos + windOffset(aPos, aTexCoords, aTexCoords2, time - uDeltaTime);

    CetraObjectPos obj = cetra_object_position(mModel, view, projection, local);
    vec4 worldPos = obj.world;
    WorldPos = worldPos.xyz;

    // Motion vectors: current vs previous clip position, both un-jittered.
    CurrClip = uCurrViewProjNoJitter * worldPos;
    PrevClip = uPrevViewProj * mPrevModel * vec4(posPrev, 1.0);

    ViewPos = obj.view.xyz;

    vec4 clipPos = obj.clip;

    Normal = normalize(mNormal * aNormal);
    TexCoords = aTexCoords;
    TexCoords2 = aTexCoords2;
    VertexColor = aColor;

    // TBN carries the SAME normal as the Normal varying: the fragment stage
    // orthogonalizes the basis against that varying, so a second, differently
    // transformed normal here would tilt the basis relative to the shading
    // normal under non-uniform scale.
    TBN = buildTBN(Normal, mat3(mModel) * aTangent.xyz, aTangent.w);
    TangentW = aTangent.w;

    gl_Position = clipPos;
}
