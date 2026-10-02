#version 330 core

// A fire drawn (spec 13.14): the ray through each pixel marched across the fire's box, from where
// it enters to where it leaves or meets the scene, composited premultiplied onto the HDR canvas
// after the temporal seam (blend ONE, ONE_MINUS_SRC_ALPHA).
//
// Soot absorbs, and glows as a blackbody at its temperature: emission sigma_a B(T) per metre
// (Pegoraro and Parker 2006), plus the reaction zone's blue. Smoke -- the same soot, cooled --
// also scatters a share of what it takes, lit by the scene's ambient and its lights, among them
// the fire's own. The fire is fogged at its own depth, which is the emission-weighted depth along
// the ray, from the volume the frame integrated.

#include "fire_constants.glsl"
#include "fire_grid.glsl"
#include "blackbody.glsl"
#include "view.glsl"

uniform mat4 projection; // read by depth.glsl, through froxel.glsl
uniform mat4 view;
uniform mat4 invViewProj;
#include "froxel.glsl"
#include "lights_ubo.glsl"

uniform vec2 viewport;        // post-resolution pixels
uniform sampler2D sceneDepth; // resolved, at RENDER resolution
uniform sampler3D fogVolume;
uniform int fogSlices;
uniform float fogNear;
uniform float fogFar;
uniform float fogDepthDist;

uniform int fireKind; // 0 GRID, 1 FLAME
uniform vec3 boxMin;
uniform vec3 boxMax;
uniform float stepLength; // metres between samples
uniform int maxSteps;
uniform sampler2D scalarTex; // GRID: (temperature above ambient, Y, soot, blue core weight)
uniform float cell;          // GRID
uniform int floorSolid;      // GRID: 1 = the box's bottom face is a floor, not an open face
uniform vec4 spine[FIRE_SPINE_POINTS]; // FLAME: xyz and radius, base to tip
uniform float flameTemperature;
uniform float flameSoot;

uniform float ambient;
uniform float sootAbsorption;
uniform float smokeAlbedo;
uniform float blueCore;
uniform vec3 blueColor; // fire_blue_color: unclamped, luminance 1
uniform float brightness;
uniform mat3 adaptation;      // fire_adaptation: the eye's adaptation to the fire, on Rec.709
uniform vec3 ambientRadiance; // nits, the scene's flat ambient

out vec4 FragColor;

const float PI = 3.14159265359;

// fire.c's _flame_profile, line for line.
void flameProfile(float u, float q, out float kelvin, out float soot, out float blue) {
    float inside = q < 1.0 ? 1.0 - q * q : 0.0;
    float lit = smoothstep(0.08, 0.32, u) * (1.0 - smoothstep(0.7, 1.0, u));
    soot = flameSoot * inside * lit;
    kelvin = ambient + (flameTemperature - ambient) * (1.0 - 0.35 * q * q) *
                           (1.0 - 0.4 * smoothstep(0.6, 1.0, u));
    blue = inside * q * q * (1.0 - smoothstep(0.05, 0.3, u));
}

// The flame's field at P, through the nearest point on its spine: fire_flame_field.
vec4 flameAt(vec3 P) {
    float best = 1e30, bestU = 0.0, bestR = 0.0;
    for (int i = 0; i + 1 < FIRE_SPINE_POINTS; i++) {
        vec3 a = spine[i].xyz;
        vec3 ab = spine[i + 1].xyz - a;
        float s = clamp(dot(P - a, ab) / max(dot(ab, ab), 1e-12), 0.0, 1.0);
        vec3 d = P - (a + s * ab);
        float d2 = dot(d, d);
        if (d2 < best) {
            best = d2;
            bestU = (float(i) + s) / float(FIRE_SPINE_POINTS - 1);
            bestR = mix(spine[i].w, spine[i + 1].w, s);
        }
    }
    float q = bestR > 1e-6 ? sqrt(best) / bestR : 2.0;
    float kelvin, soot, blue;
    flameProfile(bestU, q, kelvin, soot, blue);
    // In a grid fire's layout: temperature above ambient, Y (unused), soot, blue core weight.
    return vec4(kelvin - ambient, 0.0, soot, blue);
}

// Cells over which a GRID fire thins to nothing before an open face of its box. The box is
// where the simulation ends, not where the gas does, and without this whatever reaches a face
// -- a plume, a wisp of smoke -- is drawn cut off flat there.
const float FIRE_EDGE_FADE_CELLS = 3.0;

vec4 fieldAt(vec3 P) {
    if (fireKind == 1)
        return flameAt(P);
    vec3 p = (P - boxMin) / cell;
    vec4 f = fireSample(scalarTex, p, vec4(0.0));
    vec3 far = vec3(gridSize) - p;
    float edge = min(min(min(p.x, far.x), min(p.z, far.z)), far.y);
    if (floorSolid == 0)
        edge = min(edge, p.y);
    float fade = smoothstep(0.0, FIRE_EDGE_FADE_CELLS, edge);
    return vec4(f.x, f.y, f.z * fade, f.w * fade);
}

// The light smoke in-scatters, as radiance: the ambient, plus every light's illuminance at P
// spread over the sphere. Taken once per pixel, at the middle of the ray's run through the box.
// Unshadowed, like the rain's drops; a light inside the box is held a few cells off.
vec3 smokeLight(vec3 P, vec2 uv, float viewZ) {
    vec3 E = vec3(0.0);
    for (int j = 0; j < lightCounts.x; j++)
        E += dirLights[j].colorIntensity.xyz;
    float floor2 = 16.0 * stepLength * stepLength;
    uvec2 list = clusterLightListUv(uv, viewZ);
    for (uint k = 0u; k < list.y; k++) {
        uint li = lightIndexAt(list.x + k);
        vec3 toL = clusterLights[li].posRange.xyz - P;
        float d2 = max(dot(toL, toL), floor2);
        vec3 dirToL = toL * inversesqrt(d2);
        float e;
        if (clusterLights[li].dirType.w == 3.0) {
            vec2 size = clusterLights[li].shadowMisc.zw;
            float area = size.x * size.y;
            float facing = max(dot(clusterLights[li].dirType.xyz, -dirToL), 0.0);
            e = area * facing * getDistanceAtt(max(d2, area), clusterLights[li].attenCutoff.x);
        } else {
            e = getDistanceAtt(d2, clusterLights[li].attenCutoff.x) * punctualAngular(li, dirToL);
        }
        E += clusterLights[li].colorIntensity.xyz * e;
    }
    return ambientRadiance + E / (4.0 * PI);
}

void main() {
    vec2 uv = gl_FragCoord.xy / viewport;
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 nearH = invViewProj * vec4(ndc, -1.0, 1.0);
    vec4 farH = invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 origin = nearH.xyz / nearH.w;
    vec3 dir = normalize(farH.xyz / farH.w - origin);

    // Where the ray crosses the box.
    vec3 inv = 1.0 / dir;
    vec3 ta = (boxMin - origin) * inv;
    vec3 tb = (boxMax - origin) * inv;
    vec3 tlo = min(ta, tb), thi = max(ta, tb);
    float t0 = max(max(tlo.x, tlo.y), max(tlo.z, 0.0));
    float t1 = min(thi.x, min(thi.y, thi.z));
    // And where it meets the scene.
    float depth = texture(sceneDepth, uv).r;
    vec4 surfH = invViewProj * vec4(ndc, depth * 2.0 - 1.0, 1.0);
    t1 = min(t1, dot(surfH.xyz / surfH.w - origin, dir));
    if (t1 <= t0)
        discard;

    float tmid = 0.5 * (t0 + t1);
    vec3 mid = origin + dir * tmid;
    float midZ = -(view * vec4(mid, 1.0)).z;
    vec3 lightIn = smokeLight(mid, uv, midZ);
    float scatterShare = smokeAlbedo / max(1.0 - smokeAlbedo, 1e-3);

    vec3 color = vec3(0.0);
    float transmittance = 1.0;
    float weight = 0.0, weightedZ = 0.0;
    // No dither: drawn after the temporal seam, nothing would average it, and it reads as grain.
    // The step is finer than the field's own detail instead.
    float t = t0;
    for (int i = 0; i < maxSteps && t < t1; i++) {
        float ds = min(stepLength, t1 - t);
        vec3 P = origin + dir * (t + 0.5 * ds);
        vec4 f = fieldAt(P);
        float soot = max(f.z, 0.0);
        float glow = blueCore * max(f.w, 0.0);
        if (soot > 0.0 || glow > 0.0) {
            float lum;
            vec3 bb = blackbodyNits(ambient + max(f.x, 0.0), lum);
            float sa = soot * sootAbsorption;
            float ss = sa * scatterShare;
            float st = sa + ss;
            // Adapted, then clamped: the spectrum's colour as the adapted eye sees it, brought
            // into the gamut only after.
            vec3 emitted = max(adaptation * (sa * bb + glow * blueColor), vec3(0.0));
            vec3 source = emitted * brightness + ss * lightIn;
            float through = exp(-st * ds);
            // The source integrated exactly across the step at its constant extinction.
            float path = st > 1e-6 ? (1.0 - through) / st : ds;
            vec3 added = transmittance * source * path;
            color += added;
            float w = dot(added, vec3(0.2126, 0.7152, 0.0722));
            weight += w;
            weightedZ += w * -(view * vec4(P, 1.0)).z;
            transmittance *= through;
            if (transmittance < 0.003)
                break;
        }
        t += stepLength;
    }
    float alpha = 1.0 - transmittance;
    if (weight <= 0.0 && alpha <= 0.0)
        discard;
    float z = weight > 0.0 ? weightedZ / weight : midZ;
    vec4 front = froxelSampleMedium(fogVolume, uv, z, fogNear, fogFar, fogSlices, fogDepthDist);
    // The fog in front dims the fire, and its own in-scatter in front of the smoke -- which the
    // blend just took out with the background -- is put back.
    vec3 lit = min(color * preExposure * front.a, vec3(WS_SCENE_MAX));
    FragColor = vec4(lit + front.rgb * alpha, alpha);
}
