// Directional wind (wind.h) -- global scene field + per-material response.
// One shared copy for every pass that displaces vertices: the shading passes
// (pbr_vert, pbr_skinned_vert) and the shadow depth pass MUST run identical
// math, or a swaying surface casts its shadow from somewhere it isn't.
//
// The uniforms below are exactly the ones windOffset() reads. uDeltaTime is
// NOT among them -- only callers use it, to evaluate the previous frame's
// position for motion vectors, so it is declared by the shaders that do that.
uniform vec3 uWindDir;         // world-space blow direction
uniform float uWindStrength;   // 0 = no wind on this scene
uniform float uWindSpeed;
uniform float uWindGustFreq;
uniform float uWindGustAmount;
uniform float uWindTurbulence;
uniform float uWindResponse;   // 0 = this material is rigid
uniform float uWindMaskMinY;   // local-space AABB Y bounds of the mesh
uniform float uWindMaskMaxY;
uniform int uWindMode;         // 0 = cloth, 1 = vegetation branch, 2 = vegetation leaf

// The amplitude coefficients, shared with the CPU-side bound (wind.c) that lets
// a displaced mesh be frustum-culled.
#include "wind_bounds.glsl"

// World-Position Offset wind. Zero unless the scene has wind (uWindStrength>0)
// and this material opted in (uWindResponse>0). `p` is the object-space vertex
// position, `uv0`/`uv1` its texture coordinates, `t` the time to evaluate at.
//
// Cloth (uWindMode 0, curtains): the mask pins the top (maxY) and lets the hem
// (minY) swing; the gust envelope keeps it mostly calm with occasional swells.
//
// Vegetation (uWindMode 1-2) reads UV1: .x is a per-branch phase in [0,1) that
// de-phases neighbouring branches, .y is a flex weight running 0 at the trunk
// base to 1 at the tips. Both are continuous across branch joints, so the
// displacement cannot tear the mesh where a child branch meets its parent.
vec3 windOffset(vec3 p, vec2 uv0, vec2 uv1, float t) {
    if (uWindStrength <= 0.0 || uWindResponse <= 0.0)
        return vec3(0.0);

    float gust = mix(1.0 - uWindGustAmount, 1.0, pow(0.5 + 0.5 * sin(t * uWindGustFreq), 3.0));
    vec3 dir = normalize(uWindDir);

    if (uWindMode == 0) {
        float denom = max(uWindMaskMaxY - uWindMaskMinY, 1e-4);
        float h = clamp((uWindMaskMaxY - p.y) / denom, 0.0, 1.0);
        float mask = h * h; // more sway toward the hem
        float ph = t * uWindSpeed + p.y * 2.0 + p.x * 1.3;
        float sway = 0.5 + 0.5 * sin(ph); // 0..1 forward billow, never past rest
        float amp = uWindStrength * uWindResponse * mask * gust;
        vec3 flutter =
            vec3(sin(ph * 3.1), 0.0, cos(ph * 2.7)) * (uWindTurbulence * amp * WIND_CLOTH_FLUTTER);
        return dir * (sway * amp) + flutter;
    }

    // The trunk leans as one body, weighted by height squared so the base stays
    // planted, and every branch adds its own de-phased sway scaled by flex --
    // twigs travel far, limbs barely at all.
    float phase = uv1.x * 6.2831853;
    float flex = uv1.y;
    float amp = uWindStrength * uWindResponse * gust;

    float denom = max(uWindMaskMaxY - uWindMaskMinY, 1e-4);
    float h = clamp((p.y - uWindMaskMinY) / denom, 0.0, 1.0);
    vec3 off = dir * ((0.5 + 0.5 * sin(t * uWindSpeed * 0.35)) * amp * h * h * WIND_VEG_LEAN);

    off += dir * (sin(t * uWindSpeed + phase) * amp * flex * WIND_VEG_SWAY);
    off += vec3(sin(t * uWindSpeed * 1.7 + phase * 2.0), 0.0,
                cos(t * uWindSpeed * 1.3 + phase)) *
           (uWindTurbulence * amp * flex * WIND_VEG_TURB);

    if (uWindMode == 2) {
        // Leaf flutter rides on top of that: fast, weighted by UV0.y so the
        // card pivots about its stem (v=0) rather than sliding bodily, and
        // decorrelated by position so no two cards in a cluster beat in unison.
        float f = sin(t * uWindSpeed * 6.0 + phase * 7.0 + p.x * 3.0 + p.z * 2.7);
        off += vec3(f, f * WIND_LEAF_FLUTTER_Y, -f * WIND_LEAF_FLUTTER_Z) *
               (amp * flex * uv0.y * uWindTurbulence);
    }
    return off;
}
