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
uniform sampler2D normalsTex; // View-space normals (xyz) + roughness (a)
uniform sampler2D hdrTex;     // Resolved linear HDR scene color
uniform mat4 projection;
uniform mat4 invProjection;
uniform float maxDistance;  // March length in view-space units
uniform float thickness;    // Accepted depth gap behind a surface
uniform int steps;          // March samples along the screen segment
uniform float maxRoughness; // Reflections fade out toward this roughness

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
    // Only surfaces marked reflective trace (the floor: alpha encoded as
    // -1 - roughness by the catcher). Model surfaces carry positive
    // roughness and rely on IBL for their reflections — screen-space rays
    // off curved geometry graze their own silhouettes and sparkle.
    if (nr.a > -0.5 || dot(n, n) < 0.01) {
        FragColor = vec4(0.0);
        return;
    }
    n = normalize(n);
    float roughness = clamp(-1.0 - nr.a, 0.0, 1.0);
    if (roughness > maxRoughness) {
        FragColor = vec4(0.0);
        return;
    }

    vec3 fragPos = viewPosFromDepth(TexCoords, depth);
    vec3 viewDir = normalize(fragPos); // camera at the view-space origin
    vec3 R = normalize(reflect(viewDir, n));

    // Start biased along the normal so the ray does not immediately test
    // against its own surface; clamp rays heading toward the camera so
    // the end point stays safely in front of the near plane.
    vec3 startV = fragPos + n * 0.02;
    if (startV.z > -0.11) {
        FragColor = vec4(0.0);
        return;
    }
    float tMax = maxDistance;
    if (R.z > 0.0) {
        tMax = min(tMax, (-0.1 - startV.z) / R.z);
    }
    if (tMax <= 0.0) {
        FragColor = vec4(0.0);
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
        if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) {
            break; // left the screen: no information beyond it
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
        FragColor = vec4(0.0);
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
        FragColor = vec4(0.0);
        return;
    }

    // Fades: screen-edge (information runs out), Fresnel (glossy coating,
    // F0 = 0.1), roughness tail, and march distance.
    vec2 edge = min(hitUV, 1.0 - hitUV);
    float edgeFade = smoothstep(0.0, 0.1, min(edge.x, edge.y));
    float NdotV = max(dot(n, -viewDir), 0.0);
    float fresnel = 0.1 + 0.9 * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);
    float roughnessFade = 1.0 - smoothstep(0.5 * maxRoughness, maxRoughness, roughness);
    float distFade = 1.0 - clamp(sHit, 0.0, 1.0);

    // Premultiplied (color * weight, weight) for a lerp composite: a dark
    // reflection must DARKEN the bright floor, which additive blending
    // cannot do. The sampled color is clamped — reflections carrying raw
    // HDR spikes read as white discs after the half-res upsample.
    float fade = edgeFade * fresnel * roughnessFade * distFade;
    vec3 reflection = min(texture(hdrTex, hitUV).rgb, vec3(2.0));
    FragColor = vec4(reflection * fade, fade);
}
