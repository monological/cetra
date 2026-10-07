// The surface an app's hook decides (spec 13.29): what it is handed, and what it hands back. A
// hooked variant includes this and splices the app's
//
//     void cetraSurface(inout CetraSurface s)
//
// in after it. The OUTPUTS arrive holding what the engine gathered from the material, so a
// hook that leaves a field alone keeps the material's value, and one that scales a field scales
// the material's.
//
// On the lit surface the hook runs twice and must say the same thing both times: once before the
// alpha test, where only its albedo and alpha are kept, and once after the material's normal,
// roughness and the rest are gathered, where those are kept. Both calls start from the same
// gathered values, so a hook that multiplies its albedo multiplies it once. What one call writes
// that is not kept is dead code the compiler removes. A shadow keeps only the alpha.

struct CetraSurface {
    // Read: where and how the surface is seen.
    vec2 uv;          // the material's coordinate, after its texture transform and any parallax
    vec3 worldPos;
    vec3 viewDir;     // surface to eye, unit
    vec3 geomNormal;  // the geometric normal, unit, on the side being drawn
    vec4 vertexColor; // (1, 1, 1, 1) on a mesh without colours

    // Written, each holding the material's own value on entry.
    vec3 albedo;     // linear
    float alpha;     // coverage; a masked material is cut where it falls below its cutoff
    vec3 normal;     // world space, unit; on the first call, the geometric normal
    float roughness; // perceptual, clamped to [0.04, 1] after the hook
    float metallic;
    float ao;
    vec3 emissive;   // linear, in the units emissiveFactor carries
};

// The frame index, uploaded for a hook. `time` is the host's own uniform, the render clock.
uniform int frame;
