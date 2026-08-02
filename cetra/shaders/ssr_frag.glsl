#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Screen-space reflections: march the reflected ray in screen space
// against the resolved depth buffer and sample the scene color at the
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
uniform float maxDistance;  // March length in view-space units
uniform float thickness;    // Accepted depth gap behind a surface
uniform int steps;          // March samples along the screen segment
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

vec3 viewPosFromDepth(vec2 uv, float depth)
{
    vec4 ndc = vec4(vec3(uv, depth) * 2.0 - 1.0, 1.0);
    vec4 view = invProjection * ndc;
    return view.xyz / view.w;
}

// Analytic view-space Z from an NDC depth (cglm right-handed perspective)
#include "depth.glsl"

// Probe fallback where the SSR ray misses (off-screen, grazing, occluded,
// max steps): parallax-correct the world reflection ray against the probe's
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
    // SIGN of the G-buffer alpha (negative = reflective floor). Model
    // surfaces write non-negative alpha and rely on IBL — screen-space rays
    // off curved geometry graze their own silhouettes and sparkle. The
    // floor's roughness is a scalar uniform, not carried per-texel.
    if (nr.a > -0.5 || dot(n, n) < 0.01) {
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
    vec4 probe = probeSample(fragPos, n, R, viewDir);

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

    if (dseg.z >= 0.0) {
        // Hi-Z traversal (rays marching away from the camera — the common
        // case, and the one a fixed-step march breaks on): walk the
        // min-depth pyramid, skipping whole cells at coarse levels and
        // descending wherever the ray dips behind the nearest surface in a
        // cell. It cannot step over sub-pixel geometry (a one-texel cable
        // survives every min level), needs no jitter, and its cost grows
        // with the log of the march length instead of eating sample density.
        int level = 0;
        float t = 1e-4;
        for (int i = 0; i < 256; i++) {
            vec3 p = seg0 + dseg * t;
            if (t >= 1.0 || p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z >= 1.0) {
                break; // left the screen or the depth range: no information
            }

            ivec2 levelSize = max(ivec2(hizWidth, hizHeight) >> level, ivec2(1));
            ivec2 cell = ivec2(clamp(p.xy, vec2(0.0), vec2(0.99999)) * vec2(levelSize));
            float cellMin = texelFetch(hizTex, cell, level).r;

            // Ray parameter at which we leave this cell laterally
            vec2 cellUV0 = vec2(cell) / vec2(levelSize);
            vec2 cellUV1 = vec2(cell + 1) / vec2(levelSize);
            float tExit = 1.0;
            if (abs(dseg.x) > 1e-8)
                tExit = min(tExit, ((dseg.x >= 0.0 ? cellUV1.x : cellUV0.x) - seg0.x) / dseg.x);
            if (abs(dseg.y) > 1e-8)
                tExit = min(tExit, ((dseg.y >= 0.0 ? cellUV1.y : cellUV0.y) - seg0.y) / dseg.y);
            tExit = max(tExit, t);
            // Quarter-texel nudge past the boundary so the next iteration
            // lands in the next cell — at the FINEST level's texel size
            // regardless of the current level: a coarse-level nudge
            // overshoots whole fine cells after every big skip, snapping
            // hit boundaries to the coarse grid (stair-stepped reflections)
            float tEps = 0.25 / (float(max(hizWidth, hizHeight)) * max(length(dseg.xy), 1e-6));

            float zExit = seg0.z + dseg.z * min(tExit, 1.0);
            if (zExit >= cellMin && cellMin < 1.0) {
                // The ray dips behind the nearest surface within this cell
                if (level > 0) {
                    level--; // look closer without advancing
                    continue;
                }
                // Finest level: verify against the FULL-RES depth. The
                // cell's min is conservative (nearest of its 2x2) and
                // piecewise constant — accepting it directly quantizes hit
                // positions to cells (regular content moires into scallops)
                // and inflates silhouettes where the min came from a
                // different sub-texel. The bracket [t, tExit] spans only a
                // couple of full-res texels here, so a short dense scan
                // resolves the true crossing per pixel.
                float sFront = t;
                for (int r = 0; r < 8; r++) {
                    float s = mix(t, tExit, (float(r) + 0.5) / 8.0);
                    vec3 q = seg0 + dseg * s;
                    float dq = texture(depthTex, q.xy).r;
                    if (dq < 1.0 && q.z > dq) {
                        // Behind a surface: bisect between the last in-front
                        // sample and this one so the hit position resolves
                        // below tap granularity, then accept within thickness
                        float lo = sFront;
                        float hi = s;
                        float dHit = dq;
                        vec2 uvHit = q.xy;
                        for (int b = 0; b < 3; b++) {
                            float mid = 0.5 * (lo + hi);
                            vec3 m = seg0 + dseg * mid;
                            float dm = texture(depthTex, m.xy).r;
                            if (dm < 1.0 && m.z > dm) {
                                hi = mid;
                                dHit = dm;
                                uvHit = m.xy;
                            } else {
                                lo = mid;
                            }
                        }
                        float sceneZ = viewZFromNdcZ(dHit * 2.0 - 1.0);
                        float rayZ = viewZFromNdcZ((seg0.z + dseg.z * hi) * 2.0 - 1.0);
                        if (sceneZ - rayZ < thickness) {
                            hit = true;
                            sHit = hi;
                            hitUV = uvHit;
                        }
                        break; // too far behind: tunneling, not a hit
                    }
                    sFront = s;
                }
                if (hit)
                    break;
                t = tExit + tEps; // nothing real here: march on
            } else {
                t = tExit + tEps;
                level = min(level + 1, hizMips - 1);
            }
        }
    } else {
        // Rays marching toward the camera are clamped short by the
        // near-plane bound above; the fixed-step march is dense enough
        // there. Deterministic per-pixel jitter (interleaved gradient
        // noise) turns its stepping banding into noise the half-res
        // upsample averages away.
        // Same inline hash as the stochastic block above, kept inline for
        // the same measured reason (the include's dot() form is not
        // bit-equal and the low bits steer sampling).
        float jitter = fract(52.9829189 *
                             fract(0.06711056 * gl_FragCoord.x + 0.00583715 * gl_FragCoord.y));
        float sPrev = 0.0;
        for (int i = 0; i < steps; i++) {
            float s = (float(i) + jitter) / float(steps);
            vec3 p = mix(seg0, seg1, s);
            if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z <= 0.0 || p.z >= 1.0) {
                break; // left the screen or the depth range: no information
            }

            float d = texture(depthTex, p.xy).r;
            if (d < 1.0 && p.z > d) {
                // Behind a surface; accept if within its assumed thickness
                float sceneZ = viewZFromNdcZ(d * 2.0 - 1.0);
                float rayZ = viewZFromNdcZ(p.z * 2.0 - 1.0);
                if (sceneZ - rayZ < thickness) {
                    hit = true;
                    sHit = s;
                    hitUV = p.xy;
                }
                // Either way stop: everything farther along is occluded
                break;
            }
            sPrev = s;
        }

        if (hit) {
            // Binary refinement between the last miss and the hit
            float lo = sPrev;
            float hi = sHit;
            for (int i = 0; i < 4; i++) {
                float mid = 0.5 * (lo + hi);
                vec3 p = mix(seg0, seg1, mid);
                float d = texture(depthTex, p.xy).r;
                if (d < 1.0 && p.z > d) {
                    hi = mid;
                    hitUV = p.xy;
                } else {
                    lo = mid;
                }
            }
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
    // F0 = 0.1), roughness tail, and march distance.
    vec2 edge = min(hitUV, 1.0 - hitUV);
    float edgeFade = smoothstep(0.0, 0.1, min(edge.x, edge.y));
    float NdotV = max(dot(n, -viewDir), 0.0);
    float fresnel = 0.1 + 0.9 * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);
    float roughnessFade = 1.0 - smoothstep(0.5 * maxRoughness, maxRoughness, floorRoughness);
    float distFade = 1.0 - clamp(sHit, 0.0, 1.0);

    // Premultiplied (color * weight, weight) for a lerp composite: a dark
    // reflection must DARKEN the bright floor, which additive blending
    // cannot do. Strength folds into the weight, CLAMPED to [0,1] so the
    // premultiplied pair stays consistent (strength > 1 saturates toward a
    // full mirror instead of decoupling color from coverage). The sampled
    // color is clamped — HDR spikes read as white discs after upsampling.
    float weight = clamp(edgeFade * fresnel * roughnessFade * distFade * strength, 0.0, 1.0);
    vec3 reflection = min(texture(hdrTex, hitUV).rgb, vec3(WS_REFLECT_MAX));
    // Partial fades (screen edge, march distance) blend toward the probe
    // instead of toward nothing — premultiplied "SSR over probe"
    FragColor = vec4(reflection * weight, weight) + probe * (1.0 - weight);
}
