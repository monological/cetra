// Punctual shadow map interface: the uniforms that shadow.c's
// bind_shadow_maps_to_program fills for the perspective light types, and the
// one lookup that reads them.
//
// Same contract as csm.glsl: these names are shared with ONE C function and
// the binder is location-guarded, so renaming a uniform here silently no-ops
// the upload in whichever shader you forgot, with no error anywhere.
//
// One 2D array serves every perspective type. A point light's six 90-degree
// frusta are six ordinary layers the caller picks between, so no
// samplerCubeArray (GLSL 400 against a shader set that is uniformly 330) and
// no second texture unit -- and there is no second unit to be had, since
// pbr_frag samples all 16.
//
// MAX_PUNCTUAL_SHADOW_LAYERS is mirrored in C at shadow.h; uniform.c runs the
// same drift check on this array as on the cascade ones.

#define MAX_PUNCTUAL_SHADOW_LAYERS 8

uniform sampler2DArray punctualShadowMaps;
// Edge length the array was built at. This used to be a GLSL literal mirroring a
// C one, on the reasoning that the size was fixed where the cascades' was not.
// It is not fixed any more: shadow.c picks it from a VRAM budget against the
// layer count, so one area light gets a finer map than six point-light faces
// would (shadow.h). A literal here would have silently mis-sized the PCF step by
// the same factor.
uniform float punctualShadowMapSize;
uniform mat4 punctualShadowMatrix[MAX_PUNCTUAL_SHADOW_LAYERS];
// One texel's world width per unit of axial distance (shadow.c writes
// 2*tan(fov/2)/size). Per layer, because a point light's 90-degree face and a
// panel's 120-degree cone do not cover the same ground at the same distance.
uniform float punctualTexelScale[MAX_PUNCTUAL_SHADOW_LAYERS];
// Layers rendered this frame. 0 disables every punctual shadow at once, which
// is what the shadow system's master switch and an absent depth pass both
// reduce to; a light carries its own base layer in its UBO entry, so this is
// the only global the lookup needs.
uniform int punctualShadowCount;

// Which of a point light's six layers covers a direction, by dominant axis.
// The order is +X -X +Y -Y +Z -Z, the GL cubemap face order, and it is the ONE
// thing this file and shadow.c must agree on -- everything else about a face
// travels in its matrix. shadow.c renders the faces in this order.
//
// A boundary is exactly a 45-degree plane, so a PCF tap taken near one lands
// past the face's edge and reads the array's border -- "lit", since a 2D array
// has no neighbouring face to sample. That is a real mechanism and it measures
// below the noise floor at this map size; shadow.c records the measurement
// beside the 90-degree fov it decided on.
int punctualCubeFace(vec3 toFrag) {
    vec3 a = abs(toFrag);
    if (a.x >= a.y && a.x >= a.z)
        return toFrag.x > 0.0 ? 0 : 1;
    if (a.y >= a.z)
        return toFrag.y > 0.0 ? 2 : 3;
    return toFrag.z > 0.0 ? 4 : 5;
}

// Constant floor under the receiver-plane bias below, in units of the depth
// buffer's own resolution. The plane bias is exact for a planar receiver, so
// what is left for a constant to cover is what a plane cannot describe:
// quantization in the 24-bit depth buffer, and curvature within one texel.
#define PUNCTUAL_BIAS_FLOOR 4.0
// How far the plane may be extrapolated toward the light, as a fraction of the
// depth range per texel. A silhouette is where the receiver plane stops being
// the surface the kernel is sampling, and an unbounded extrapolation there
// turns the bias into a light leak that widens with the slope.
//
// The clamp is ONE-SIDED: the plane term is never allowed to predict the
// surface DEEPER than the fragment. Its whole job is the nearer direction --
// matching the receiver's own surface up-slope so grazing acne dies. A deeper
// prediction only ever makes the test stricter than a flat compare, and at a
// concave junction that manufactures occlusion: the plane extrapolates INTO
// the adjacent surface, which truncates it and stands epsilon NEARER, so taps
// crossing the junction read "occluded" at coplanar scale. Measured on
// cornell_leak's wall base: every failing tap failed with a sub-1e-4 depth
// gap and none held a genuinely different surface -- the dark wedge was
// entirely this extrapolation, not the geometry.
#define PUNCTUAL_MAX_PLANE_BIAS 0.01
// Below this NdotL the map is not answering the question asked of it. One
// perspective map tests whether the light's CENTRE is visible, and for a
// receiver nearly edge-on to that centre the real answer for an AREA source is
// "half the panel", which a binary test cannot express.
//
// It used to also cover spurious self-occlusion at grazing, which the plane bias
// now handles properly, so the threshold is a third of what it was -- it is back
// to describing the single-sample approximation rather than papering over acne.
//
// A no-op on the point/spot path, whose radiance already carries the same NdotL
// and is zero wherever the fade is not one. Only the LTC path has anything to
// lose here, because its form factor carries orientation instead.
#define PUNCTUAL_GRAZING_FADE 0.07

// Occlusion for one perspective map: 1 = lit, 0 = fully occluded. A layer
// outside the live range, a fragment behind the light, a fragment off the map,
// and a fragment edge-on to the light all read as lit -- the third via the
// array's white border, the last via the fade above.
//
// `ddxWorld`/`ddyWorld` are the screen-space derivatives of the world position,
// taken by the CALLER. They cannot be taken here: this runs inside the clustered
// light loop, which is non-uniform control flow, where derivatives are
// undefined. Passing them in is what lets the receiver's own plane be
// reconstructed per light without leaving that constraint.
float punctualShadow(int layer, vec3 worldPos, vec3 N, vec3 L, vec3 ddxWorld, vec3 ddyWorld) {
    if (layer < 0 || layer >= punctualShadowCount)
        return 1.0;

    float ndl = clamp(dot(N, L), 0.0, 1.0);
    float trust = smoothstep(0.0, PUNCTUAL_GRAZING_FADE, ndl);
    if (trust <= 0.0)
        return 1.0;

    vec4 ls = punctualShadowMatrix[layer] * vec4(worldPos, 1.0);
    if (ls.w <= 0.0)
        return 1.0;

    vec3 pc = ls.xyz / ls.w * 0.5 + 0.5;
    if (pc.z > 1.0 || pc.x < 0.0 || pc.x > 1.0 || pc.y < 0.0 || pc.y > 1.0)
        return 1.0;

    // Receiver-plane depth bias (Isidoro, GDC 2006). The old bias displaced the
    // sample toward the light by a slope-scaled world distance, which is a guess
    // at how much depth a tilted surface covers across the kernel. This computes
    // it instead: differentiate the receiver's own shadow-space position along
    // both screen axes, then solve the 2x2 for how its depth changes per unit of
    // shadow UV. Each tap is then compared against where the receiver's PLANE
    // would be at that tap, which is exact at any slope -- so a surface edge-on
    // to the light no longer needs a bias large enough to detach its own contact.
    vec4 lx = punctualShadowMatrix[layer] * vec4(worldPos + ddxWorld, 1.0);
    vec4 ly = punctualShadowMatrix[layer] * vec4(worldPos + ddyWorld, 1.0);
    vec2 duv_dz = vec2(0.0);
    if (lx.w > 0.0 && ly.w > 0.0) {
        vec3 px = lx.xyz / lx.w * 0.5 + 0.5 - pc;
        vec3 py = ly.xyz / ly.w * 0.5 + 0.5 - pc;
        float det = px.x * py.y - px.y * py.x;
        // A degenerate 2x2 means the receiver projects to a line in the map --
        // exactly edge-on -- where there is no plane to extrapolate along.
        if (abs(det) > 1e-12)
            duv_dz = vec2(py.y * px.z - px.y * py.z, px.x * py.z - py.x * px.z) / det;
    }

    // 3x3 PCF, which the directional cascades have always had and this never
    // did: a single tap quantizes the edge to the texel grid, and reads as a
    // staircase on any silhouette not aligned to it.
    //
    // Taps are clamped to the face. A point light's six faces meet on 45-degree
    // planes, and a fragment sitting near one has taps that fall OFF this face
    // -- where a 2D array has no neighbouring face to sample, only its border,
    // which reads lit. Unclamped that draws a straight lit seam across whatever
    // the boundary crosses, and it is immune to every depth remedy because no
    // depth comparison is involved: the tap simply misses the data. Clamping
    // re-reads this face's edge texel, which is the nearest depth that actually
    // exists; the true neighbour is a face away and unreachable without a
    // samplerCubeArray (GLSL 400, above this shader set's 330).
    vec2 texel = vec2(1.0 / punctualShadowMapSize);
    // The constant floor, in depth-buffer units at this map's precision.
    float floorBias = PUNCTUAL_BIAS_FLOOR / 16777216.0; // DEPTH_COMPONENT24
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 off = vec2(x, y) * texel;
            vec2 uv = clamp(pc.xy + off, texel * 0.5, 1.0 - texel * 0.5);
            // Where the receiver's own plane sits at this tap, rather than at
            // the fragment -- clamped one-sided; see PUNCTUAL_MAX_PLANE_BIAS.
            float plane = clamp(dot(duv_dz, off), -PUNCTUAL_MAX_PLANE_BIAS, 0.0);
            float d = texture(punctualShadowMaps, vec3(uv, float(layer))).r;
            sum += (pc.z + plane - floorBias > d) ? 0.0 : 1.0;
        }
    }
    return mix(1.0, sum / 9.0, trust);
}
