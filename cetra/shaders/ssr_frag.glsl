#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Screen-space reflections: march the reflected ray in screen space
// against the resolved depth buffer and sample the scene color at the
// hit. Projective transforms map lines to lines, so interpolating the
// NDC endpoints linearly walks the exact projected ray — unlike a
// view-space march, whose fixed world steps alias into dotted hits on
// thin geometry. Writes a half-res reflection buffer that a separate
// pass composites additively into the HDR target (GL 4.1 has no texture
// barrier, so the scene texture cannot be read and written in one pass).
uniform sampler2D depthTex;   // Full-res resolved scene depth
uniform sampler2D normalsTex; // View-space normal (xyz) + reflective marker (a)
uniform sampler2D hdrTex;     // Resolved linear HDR scene color
uniform mat4 projection;
uniform mat4 invProjection;
uniform float maxDistance;  // March length in view-space units
uniform float thickness;    // Accepted depth gap behind a surface
uniform int steps;          // March samples along the screen segment
uniform float floorRoughness; // Roughness of the reflective floor
uniform float maxRoughness;   // Reflections fade out toward this roughness
uniform float strength;       // Reflection strength (folded into the weight)

// Local reflection probe fallback for rays the march cannot answer
uniform mat4 invView;         // view -> world (camera pose)
uniform samplerCube probeTex; // prefiltered local probe capture
uniform int probeEnabled;
uniform vec3 probePos;    // capture origin (world)
uniform vec3 probeBoxMin; // parallax proxy AABB (world)
uniform vec3 probeBoxMax;
uniform float probeMaxLOD;
uniform float probeIntensity;

vec3 viewPosFromDepth(vec2 uv, float depth)
{
    vec4 ndc = vec4(vec3(uv, depth) * 2.0 - 1.0, 1.0);
    vec4 view = invProjection * ndc;
    return view.xyz / view.w;
}

// Analytic view-space Z from an NDC depth (cglm right-handed perspective)
float viewZFromNdcZ(float ndcZ)
{
    return -projection[3][2] / (projection[2][2] + ndcZ);
}

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
    vec3 col = textureLod(probeTex, dir, floorRoughness * probeMaxLOD).rgb * probeIntensity;
    float NdotV = max(dot(n, -viewDir), 0.0);
    float fresnel = 0.1 + 0.9 * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);
    float roughnessFade = 1.0 - smoothstep(0.5 * maxRoughness, maxRoughness, floorRoughness);
    float w = clamp(fresnel * roughnessFade * strength, 0.0, 1.0);
    return vec4(min(col, vec3(2.0)) * w, w);
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

    // Start biased along the normal so the ray does not immediately test
    // against its own surface; clamp rays heading toward the camera so
    // the end point stays safely in front of the near plane.
    vec3 startV = fragPos + n * 0.02;
    if (startV.z > -(nearV + 0.01)) {
        FragColor = probeSample(fragPos, n, R, viewDir);
        return;
    }
    float tMax = maxDistance;
    if (R.z > 0.0) {
        tMax = min(tMax, (-nearV - startV.z) / R.z);
    }
    if (tMax <= 0.0) {
        FragColor = probeSample(fragPos, n, R, viewDir);
        return;
    }
    vec3 endV = startV + R * tMax;

    vec4 clip0 = projection * vec4(startV, 1.0);
    vec4 clip1 = projection * vec4(endV, 1.0);
    // uv in [0,1], z as depth-buffer values in [0,1]
    vec3 seg0 = vec3(clip0.xy / clip0.w, clip0.z / clip0.w) * 0.5 + 0.5;
    vec3 seg1 = vec3(clip1.xy / clip1.w, clip1.z / clip1.w) * 0.5 + 0.5;

    // Deterministic per-pixel jitter (interleaved gradient noise) turns
    // stepping banding into noise the half-res upsample averages away
    float jitter =
        fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x + 0.00583715 * gl_FragCoord.y));

    bool hit = false;
    float sPrev = 0.0;
    float sHit = 0.0;
    vec2 hitUV = vec2(0.0);

    for (int i = 0; i < steps; i++) {
        float s = (float(i) + jitter) / float(steps);
        vec3 p = mix(seg0, seg1, s);
        if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z <= 0.0 || p.z >= 1.0) {
            break; // left the screen or the depth range: no information there
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

    if (!hit) {
        // Off-screen, out of steps, or occluded with no acceptable surface
        FragColor = probeSample(fragPos, n, R, viewDir);
        return;
    }

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

    // Reject hits on surfaces seen from behind (their normal points along
    // the ray): the depth buffer holds their front side, not what the ray
    // would actually see.
    vec3 hitN = texture(normalsTex, hitUV).xyz;
    if (dot(hitN, hitN) > 0.01 && dot(normalize(hitN), R) > 0.2) {
        FragColor = probeSample(fragPos, n, R, viewDir);
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
    vec3 reflection = min(texture(hdrTex, hitUV).rgb, vec3(2.0));
    // Partial fades (screen edge, march distance) blend toward the probe
    // instead of toward nothing — premultiplied "SSR over probe"
    FragColor = vec4(reflection * weight, weight) + probeSample(fragPos, n, R, viewDir)
                                                        * (1.0 - weight);
}
