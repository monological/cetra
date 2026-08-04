#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Screen-space reflections: march the reflected ray in screen space
// against the min-depth Hi-Z pyramid and sample the scene color at the
// hit. Projective transforms map lines to lines, so interpolating the
// NDC endpoints linearly walks the exact projected ray — unlike a
// view-space march, whose fixed world steps alias into dotted hits on
// thin geometry. Writes a reflection buffer (full-res by default, half-res
// when ssr_full_res is off) that a separate pass composites into the HDR
// target (GL 4.1 has no texture barrier, so the scene texture cannot be
// read and written in one pass).
uniform sampler2D depthTex;   // Full-res resolved scene depth
uniform sampler2D normalsTex; // View-space normal (xyz) + reflective marker (a)
uniform sampler2D hdrTex;     // Resolved linear HDR scene color
uniform sampler2D hizTex;     // Min-depth pyramid (SSR-res base) for the traversal
uniform int hizWidth;         // Pyramid base dimensions
uniform int hizHeight;
uniform int hizMips;
uniform mat4 projection;
uniform mat4 invProjection;
uniform float maxDistance;   // March length in view-space units
uniform float thicknessMin;  // Acceptance-slab floor behind a surface (view units)
uniform float floorRoughness; // Roughness of the reflective floor
uniform float maxRoughness;   // Reflections fade out toward this roughness
uniform float strength;       // Reflection strength (folded into the weight)

// Stochastic march (SSR denoiser): perturb the reflection ray per pixel + per
// frame so the deterministic Hi-Z traversal's step/cell grid stops producing
// fixed-position stripes and instead scatters them into per-frame noise, which
// the temporal accumulator + a-trous denoise then resolve. Off -> the exact
// deterministic ray, bit-identical.
uniform int ssrStochastic;   // 1 = jitter the reflection ray
uniform int ssrFrameIndex;   // Advances the per-frame random
uniform float ssrJitter;     // Base ray-jitter spread (a floor under roughness)

// Local reflection probe fallback for rays the march cannot answer
uniform mat4 invView;         // view -> world (camera pose)
uniform samplerCube probeTex; // prefiltered local probe capture
uniform int probeEnabled;
uniform vec3 probePos;    // capture origin (world)
uniform vec3 probeBoxMin; // parallax proxy AABB (world)
uniform vec3 probeBoxMax;
uniform float probeMaxLOD;
uniform float probeIntensity;

// hdrTex is already pre-exposed, so a reflection inherits the conversion and
// WS_REFLECT_MAX below is read in the same space it was written in.
#include "view.glsl"

// Acceptance slab behind a surface: floor thicknessMin (absolute view units,
// covering depth quantization and the start bias) plus the ray's own view-z
// travel across the column under test, scaled by THICK_SCALE. Scaling by the
// ray's travel keeps acceptance incidence-invariant -- a grazing ray gets the
// deep slab its geometry needs, a steep ray a shallow one where a fixed
// constant painted silhouette ghosts.
const float THICK_SCALE = 1.5;
// Bounds on marching behind known geometry (the occlusion exit in the loop):
// a single gap this many slabs deep, or this many consecutive behind-columns.
const float K_OCCLUDE = 8.0;
const int M_BEHIND = 12;
// The budget's worst customer is a floor-hugging ray reflecting distant
// content at grazing incidence: its clearance above the receiver is too
// small for coarse hi-z skips, so it crawls fine cells for hundreds of
// screen columns, and exhausting the loop there cuts a near-mirror
// reflection off along a hard mid-weight frontier (no distFade rescue --
// the ray dies mid-segment). The budget is therefore spent in screen
// COLUMNS, so a fixed count reaches half as far every time the resolution
// doubles -- 128 truncated a close-up mirror floor at 1600 wide, 256 the
// same shot at 3456. The C side scales it with the trace width instead
// (floor 256, so low-res output is bit-identical to the fixed era).
uniform int marchBudget;

vec3 viewPosFromDepth(vec2 uv, float depth)
{
    vec4 ndc = vec4(vec3(uv, depth) * 2.0 - 1.0, 1.0);
    vec4 view = invProjection * ndc;
    return view.xyz / view.w;
}

// Analytic view-space Z from an NDC depth (cglm right-handed perspective)
#include "depth.glsl"

// Probe fallback where the SSR ray misses (off-screen, grazing, occluded,
// iteration cap): parallax-correct the world reflection ray against the probe's
// proxy box and return the hit path's premultiplied (color*weight, weight)
// contract. The screen-space fades don't apply — the probe has data in
// every direction. Exact vec4(0) when the probe is off, so the miss sites
// write today's values bit-identically.
vec4 probeSample(vec3 fragPosV, vec3 n, vec3 RV, vec3 viewDir)
{
    if (probeEnabled == 0)
        return vec4(0.0);
    vec3 worldPos = (invView * vec4(fragPosV, 1.0)).xyz;
    vec3 worldR = normalize(mat3(invView) * RV);
    vec3 invR = 1.0 / worldR;
    vec3 tMax3 = max((probeBoxMax - worldPos) * invR, (probeBoxMin - worldPos) * invR);
    float t = min(min(tMax3.x, tMax3.y), tMax3.z);
    // The floor lies ON the box's bottom face, so no inside-the-box fade;
    // a ray starting outside the box keeps the uncorrected direction
    vec3 dir = (t > 0.0) ? normalize((worldPos + worldR * t) - probePos) : worldR;
    // The probe bakes absolute radiance (render.c renders captures at unity), so
    // unlike the hdrTex hit path this one converts. Without it the fallback
    // would sit at scene scale while the hit it substitutes for sits at working
    // scale, and a ray crossing the screen edge would step in brightness.
    vec3 col =
        textureLod(probeTex, dir, floorRoughness * probeMaxLOD).rgb * probeIntensity * preExposure;
    float NdotV = max(dot(n, -viewDir), 0.0);
    float fresnel = 0.1 + 0.9 * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);
    float roughnessFade = 1.0 - smoothstep(0.5 * maxRoughness, maxRoughness, floorRoughness);
    float w = clamp(fresnel * roughnessFade * strength, 0.0, 1.0);
    return vec4(min(col, vec3(WS_REFLECT_MAX)) * w, w);
}

// Ray-vs-cell boundary planes: the segment params at which the ray enters
// (tIn) and leaves (tOut) `cell` at `levelSize`, and which axis the entry
// crossed (0 = x, 1 = y; defaults to y when the entry is the segment start).
// A degenerate axis (no screen motion) bounds nothing: tIn stays 0, tOut
// stays 1.
void cellSpan(vec3 seg0, vec3 dseg, ivec2 cell, ivec2 levelSize, out float tIn, out float tOut,
              out int enteredAxis)
{
    vec2 cellUV0 = vec2(cell) / vec2(levelSize);
    vec2 cellUV1 = vec2(cell + 1) / vec2(levelSize);
    tIn = 0.0;
    tOut = 1.0;
    enteredAxis = 1;
    for (int a = 0; a < 2; a++) {
        if (abs(dseg[a]) > 1e-8) {
            float e = ((dseg[a] >= 0.0 ? cellUV0[a] : cellUV1[a]) - seg0[a]) / dseg[a];
            if (e > tIn) {
                tIn = e;
                enteredAxis = a;
            }
            tOut = min(tOut, ((dseg[a] >= 0.0 ? cellUV1[a] : cellUV0[a]) - seg0[a]) / dseg[a]);
        }
    }
}

void main()
{
    float depth = texture(depthTex, TexCoords).r;
    if (depth >= 1.0) {
        // Sky: nothing to reflect from
        FragColor = vec4(0.0);
        return;
    }

    vec4 nr = texture(normalsTex, TexCoords);
    vec3 n = nr.xyz;
    // Only surfaces the catcher marked reflective trace: the marker is the
    // SIGN of the G-buffer alpha (negative = reflective floor), and its
    // magnitude is the catcher's edge falloff — the reflectivity fades to
    // zero at the quad boundary exactly like the shadow does. Model
    // surfaces write non-negative alpha and rely on IBL — screen-space rays
    // off curved geometry graze their own silhouettes and sparkle. The
    // floor's roughness is a scalar uniform, not carried per-texel.
    float floorFade = clamp(-nr.a, 0.0, 1.0);
    // This cutoff must stay above catcher_frag's 0.002 marker floor: the
    // catcher stamps that floor on its dead outer ring precisely so it
    // lands below here and never traces.
    if (floorFade < 0.004 || dot(n, n) < 0.01) {
        FragColor = vec4(0.0);
        return;
    }
    n = normalize(n);
    if (floorRoughness > maxRoughness) {
        FragColor = vec4(0.0);
        return;
    }

    // Near plane recovered from the projection (the app's near clip varies),
    // so the camera-facing-ray clamp below tracks the real frustum
    float nearV = projection[3][2] / (projection[2][2] - 1.0);

    vec3 fragPos = viewPosFromDepth(TexCoords, depth);
    vec3 viewDir = normalize(fragPos); // camera at the view-space origin
    vec3 R = normalize(reflect(viewDir, n));

    if (ssrStochastic == 1) {
        // Cosine-ish disk jitter in R's tangent frame, spread by roughness with
        // a small floor so even the near-mirror grid breaks up. The per-frame
        // index rotates the interleaved-gradient noise so each frame draws a
        // different sample; the temporal accumulator averages them.
        vec2 fc = gl_FragCoord.xy + vec2(float(ssrFrameIndex) * 5.588238);
        // Inline IGN, deliberately NOT include/noise.glsl's ign(): the
        // include's dot() form compiles to different float ordering than
        // this explicit multiply-add, and the hash's low bits steer the
        // ray -- migrating was measured at 31,800 px on a no-TAA render.
        float r0 = fract(52.9829189 * fract(0.06711056 * fc.x + 0.00583715 * fc.y));
        float r1 = fract(52.9829189 * fract(0.06711056 * (fc.y + 41.0) + 0.00583715 * fc.x));
        float spread = max(floorRoughness, ssrJitter);
        float ang = 6.2831853 * r0;
        float rad = spread * sqrt(r1);
        vec3 up = abs(R.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 tang = normalize(cross(up, R));
        vec3 bitang = cross(R, tang);
        R = normalize(R + (cos(ang) * tang + sin(ang) * bitang) * rad);
    }

    // The probe answer for this ray, computed once for every exit below:
    // full fallback on a miss, the faded tail's filler on a partial hit.
    // Exact vec4(0) with the probe off, keeping every path bit-identical.
    // The catcher's edge falloff rides along here so every exit fades.
    vec4 probe = probeSample(fragPos, n, R, viewDir) * floorFade;

    // Start biased along the normal so the ray does not immediately test
    // against its own surface. The bias must grow with view distance: a
    // fixed offset is sub-quantization on distant fragments of large scenes,
    // where grazing rays hug their own surface for hundreds of units and
    // depth rounding flips them "behind" it in row-aliased phase (banded
    // false self-hits). Clamp rays heading toward the camera so the end
    // point stays safely in front of the near plane.
    vec3 startV = fragPos + n * max(0.02, 0.002 * length(fragPos));
    if (startV.z > -(nearV + 0.01)) {
        FragColor = probe;
        return;
    }
    float tMax = maxDistance;
    if (R.z > 0.0) {
        tMax = min(tMax, (-nearV - startV.z) / R.z);
    }
    if (tMax <= 0.0) {
        FragColor = probe;
        return;
    }
    vec3 endV = startV + R * tMax;

    vec4 clip0 = projection * vec4(startV, 1.0);
    vec4 clip1 = projection * vec4(endV, 1.0);
    // uv in [0,1], z as depth-buffer values in [0,1]
    vec3 seg0 = vec3(clip0.xy / clip0.w, clip0.z / clip0.w) * 0.5 + 0.5;
    vec3 seg1 = vec3(clip1.xy / clip1.w, clip1.z / clip1.w) * 0.5 + 0.5;

    vec3 dseg = seg1 - seg0;

    bool hit = false;
    float sHit = 0.0;
    vec2 hitUV = vec2(0.0);
    int itersUsed = 0;

    // Hi-Z traversal: walk the min-depth pyramid, skipping whole cells at
    // coarse levels and descending wherever the ray's z-span reaches behind
    // the nearest surface in a cell. It cannot step over sub-pixel geometry
    // (a one-texel cable survives every min level) and its cost grows with
    // the log of the march length. One loop serves both z directions: cell
    // stepping is x/y boundary math (z never picks the step) and the span
    // test compares the span's FAR end against the cell min, which is
    // direction-agnostic; the near-plane clamp above keeps toward-camera
    // segments short.
    int level = 0;
    int behindRun = 0;

    // Quarter-texel nudge past a cell boundary so the next iteration lands
    // in the next cell — at the FINEST level's texel size regardless of the
    // current level: a coarse-level nudge overshoots whole fine cells after
    // every big skip, snapping hit boundaries to the coarse grid
    // (stair-stepped reflections)
    float tEps = 0.25 / (float(max(hizWidth, hizHeight)) * max(length(dseg.xy), 1e-6));

    // Start past the fragment's own texel column so the ray never tests the
    // column it reflected off (the normal bias handles depth separation;
    // this handles the column). A ray that never leaves its own column
    // (near-vertical: reflection of the camera region) exits immediately to
    // the probe — the screen cannot answer it anyway.
    ivec2 sizeFine = ivec2(hizWidth, hizHeight);
    ivec2 cell0 = ivec2(clamp(seg0.xy, vec2(0.0), vec2(0.99999)) * vec2(sizeFine));
    float skipIn; // only the exit is needed here
    float tSkip;
    int skipAxis;
    cellSpan(seg0, dseg, cell0, sizeFine, skipIn, tSkip, skipAxis);
    float t = max(tSkip, 0.0) + tEps;

    for (int i = 0; i < marchBudget; i++) {
        itersUsed = i;
        vec3 p = seg0 + dseg * t;
        if (t >= 1.0 || p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z <= 0.0 ||
            p.z >= 1.0) {
            break; // left the screen or the depth range: no information
        }

        ivec2 levelSize = max(sizeFine >> level, ivec2(1));
        ivec2 cell = ivec2(clamp(p.xy, vec2(0.0), vec2(0.99999)) * vec2(levelSize));
        float cellMin = texelFetch(hizTex, cell, level).r;

        // Exact entry/exit params of this cell from its boundary planes.
        // The entry is recomputed rather than read from the nudged cursor:
        // the quarter-texel nudge would otherwise leave a z-coverage gap at
        // every column, a meaningful slab fraction at grazing incidence
        // (faint residual striping).
        float tIn;
        float tOut;
        int enteredAxis;
        cellSpan(seg0, dseg, cell, levelSize, tIn, tOut, enteredAxis);
        tOut = max(tOut, t); // degenerate-direction guard: always progress
        tIn = clamp(tIn, 0.0, tOut);

        float zInNdc = seg0.z + dseg.z * tIn;
        float zOutNdc = seg0.z + dseg.z * tOut;

        // Does the span reach at-or-behind the nearest surface in the cell?
        // (NDC depth grows away from the camera, so max is the far end.)
        if (cellMin < 1.0 && max(zInNdc, zOutNdc) >= cellMin) {
            if (level > 0) {
                level--; // look closer without advancing
                continue;
            }

            // Finest level. Full-res traces make level 0 a 1:1 copy of the
            // depth buffer (ssr_hiz_frag.glsl copySrc), so cellMin IS the
            // scene depth of this one screen column and the interval test
            // is exact; half-res level 0 is a 2x2 min, conservative by at
            // most one full-res texel.
            float sceneZ = viewZFromNdcZ(cellMin * 2.0 - 1.0);
            float zEnterV = viewZFromNdcZ(zInNdc * 2.0 - 1.0);
            float zExitV = viewZFromNdcZ(zOutNdc * 2.0 - 1.0);
            float zNearV = max(zEnterV, zExitV); // view z: larger = nearer
            float zFarV = min(zEnterV, zExitV);

            float thickV = thicknessMin + THICK_SCALE * (zNearV - zFarV);

            if (zNearV >= sceneZ) {
                // The span enters in front and leaves behind: the ray
                // provably crosses the surface inside this column. Accept
                // unconditionally — no thickness term, no sampling phase.
                hit = true;
            } else if (sceneZ - zNearV <= thickV) {
                // The whole span is behind: the ray stepped over a depth
                // step BETWEEN columns. A continuous receiver (the floor
                // itself, a ramp) has a neighbor at slab-comparable depth;
                // a silhouette (floor -> sphere) does not, and accepting it
                // paints the occluder's edge color onto the receiver (the
                // ghost blob). Compare against the column the ray entered
                // through.
                ivec2 stepDir = ivec2(0);
                stepDir[enteredAxis] = dseg[enteredAxis] >= 0.0 ? 1 : -1;
                ivec2 fromCell = clamp(cell - stepDir, ivec2(0), levelSize - ivec2(1));
                float fromZ = viewZFromNdcZ(texelFetch(hizTex, fromCell, level).r * 2.0 - 1.0);
                if (abs(fromZ - sceneZ) <= thickV)
                    hit = true;
            }

            if (hit) {
                // Crossing param solved on the segment (exact under the
                // projective interpolation — no bisection needed); a
                // step-accept clamps to the column entry. hitUV at the
                // texel center so the color/normal fetches read this
                // column, not a filtered neighbor (exact at full res; a
                // 2x2 blend at half res).
                sHit = (abs(dseg.z) > 1e-9) ? clamp((cellMin - seg0.z) / dseg.z, tIn, tOut)
                                            : tIn;
                hitUV = (vec2(cell) + 0.5) / vec2(levelSize);
                break;
            }

            // Behind by more than the slab: known-occluded territory. Keep
            // crawling only far enough to cross THIN occluders; a gap many
            // slabs deep or a long behind-run means the reflected surface
            // is simply not in the depth buffer — stop and let the probe
            // answer.
            behindRun++;
            if (sceneZ - zNearV > K_OCCLUDE * thickV || behindRun >= M_BEHIND) {
                break; // behind for good (hit stays false: probe)
            }
            t = tOut + tEps;
            // Deliberately stay at level 0: a min pyramid can never clear
            // a cell while the ray is behind its surface, so ascending
            // here descends straight back (two wasted iterations).
        } else {
            behindRun = 0;
            t = tOut + tEps;
            level = min(level + 1, hizMips - 1); // clear: climb and skip more
        }
    }

    if (!hit) {
        // Off-screen, exhausted, or occluded with no acceptable surface
        FragColor = probe;
        return;
    }

    // Reject hits on surfaces seen from behind (their normal points along
    // the ray): the depth buffer holds their front side, not what the ray
    // would actually see.
    vec3 hitN = texture(normalsTex, hitUV).xyz;
    if (dot(hitN, hitN) > 0.01 && dot(normalize(hitN), R) > 0.2) {
        FragColor = probe;
        return;
    }

    // Fades: screen-edge (information runs out), Fresnel (glossy coating,
    // F0 = 0.1), roughness tail, march distance, and iteration budget. The
    // budget fade is what keeps exhaustion invisible: a column whose ray
    // dies unhit composites nothing, so without it the neighbouring column
    // that hit on its LAST iterations keeps mid-strength weight and the
    // frontier between them prints as a hard cut across the reflection.
    // Fading hits by budget consumed makes both sides of that frontier
    // meet at zero.
    vec2 edge = min(hitUV, 1.0 - hitUV);
    float edgeFade = smoothstep(0.0, 0.1, min(edge.x, edge.y));
    float NdotV = max(dot(n, -viewDir), 0.0);
    float fresnel = 0.1 + 0.9 * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);
    float roughnessFade = 1.0 - smoothstep(0.5 * maxRoughness, maxRoughness, floorRoughness);
    float distFade = 1.0 - clamp(sHit, 0.0, 1.0);
    float budgetFade = 1.0 - smoothstep(0.75, 1.0, float(itersUsed) / float(marchBudget));

    // Premultiplied (color * weight, weight) for a lerp composite: a dark
    // reflection must DARKEN the bright floor, which additive blending
    // cannot do. Strength folds into the weight, CLAMPED to [0,1] so the
    // premultiplied pair stays consistent (strength > 1 saturates toward a
    // full mirror instead of decoupling color from coverage). The sampled
    // color is clamped — HDR spikes read as white discs after upsampling.
    float weight =
        clamp(edgeFade * fresnel * roughnessFade * distFade * budgetFade * strength, 0.0, 1.0);
    vec3 reflection = min(texture(hdrTex, hitUV).rgb, vec3(WS_REFLECT_MAX));
    // Partial fades (screen edge, march distance) blend toward the probe
    // instead of toward nothing — premultiplied "SSR over probe". The probe
    // term already carries floorFade, so scaling the SSR term by it makes
    // the whole composite exactly floorFade * (unfaded composite).
    FragColor = vec4(reflection * weight, weight) * floorFade + probe * (1.0 - weight);
}
