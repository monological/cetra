#version 330 core

// Falling rain (spec 13.9). One instance per drop, four vertices per streak, and no vertex
// data at all: where a drop is, how big and how fast are all functions of its index and the
// rain's clock. Nothing is simulated because nothing needs to be -- drops do not meet each
// other -- and a stateless drop is the same drop on every run at the same frame.
//
// Drawn after the temporal seam (PostFXLateDraw), in HDR at post resolution, so the streak
// is its own motion blur: its length is how far the drop moved across the camera's exposure,
// and nothing downstream smears it again.

#include "rain_constants.glsl"

uniform mat4 view;
uniform mat4 projection; // UNjittered: this draws after TAA has taken the jitter out
uniform vec3 cameraPos;
uniform vec3 cameraVelocity; // m/s; a streak is the drop's motion RELATIVE to the camera
uniform vec2 viewport;       // post-resolution pixels

uniform float rainTime;
uniform vec3 rainWind;
uniform float mpLambda; // Marshall-Palmer slope, 1/mm
uniform float fallScale; // on the terminal velocity; 1 = physical
uniform float shutter;
uniform float boxHalf;  // the innermost box's half-width, m
uniform int dropsPerBox;
// How many real drops each streak in the innermost box stands for: the rain's density times
// the box's volume over the streaks drawn in it. Each next box is 27 times the volume with
// the same count, so it stands for 27 times as many.
uniform float dropsPerStreak;
uniform float streakWidth;
uniform float streakBrightness;
uniform float forwardG;
uniform float glintShare;

out float vAcrossPx; // signed pixels from the streak's centre line
out float vHalfWidth;
out vec3 vLit;       // what the lights send toward the eye through the drop, pre-exposed
out float vAlpha;    // its opacity, physical, before the depth and edge fades
out float vViewDepth; // planar, positive
out float vAlongPx;  // pixels from the tail toward the head
flat out float vLengthPx;
// The glints: flashes across the streak, their phase, and how much of the lit light they
// carry -- zero where the streak is too short on screen to resolve them.
flat out float vGlintBands;
flat out float vGlintPhase;
flat out float vGlintShare;

const float PI = 3.14159265359;
#include "view.glsl"
#include "phase.glsl"
#include "depth.glsl"
#include "lights_ubo.glsl"
// The array the rain's cover is a layer of, declared here rather than through
// punctual_shadow.glsl, whose lookup is a fragment-stage receiver-plane test.
uniform sampler2DArray punctualShadowMaps;
#include "rain_occlusion.glsl"

// The drop diameters, in mm, over which the glints come in. A small drop is held round by its
// own surface tension and barely rings, and it rings fastest: given the same flash as a big
// one, the Marshall-Palmer majority under a millimetre striped every streak with twenty to
// thirty bands. The visible oscillation belongs to the big drops, which flash two to four
// times an exposure. Where between these two it rises is a judgment, not a measurement.
const float RAIN_GLINT_D_MIN = 1.0;
const float RAIN_GLINT_D_FULL = 2.5;

// A 4D integer hash (Jarzynski and Olano's pcg4d). Integer rather than the sin-fract form
// noise.glsl carries, because here the SEEDS are consecutive integers by the ten thousand,
// which is the regime a sin-fract hash lines up in.
uvec4 pcg4d(uvec4 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.w;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.w += v.y * v.z;
    v ^= v >> 16u;
    v.x += v.y * v.w;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.w += v.y * v.z;
    return v;
}

// The light a drop sends toward the camera from each source: its illuminance there times
// the fraction scattered this way. Refraction throws most of it forward and the rest goes
// everywhere, so rain between the eye and a lamp glitters and rain lit from the side is
// barely there.
float dropPhase(float cosTheta) {
    return mix(1.0 / (4.0 * PI), phaseHG(cosTheta, forwardG), RAIN_REFRACT_SHARE);
}

// The light the lamps and the sun put into a drop and it sends on toward the eye. What it
// REFRACTS -- the scene behind it -- is the fragment stage's, read from the frame itself.
vec3 dropLit(vec3 P, vec3 toCamera, vec2 uv, float viewDepth) {
    vec3 L = vec3(0.0);
    for (int j = 0; j < lightCounts.x; j++) {
        vec3 travel = dirLights[j].dirShadow.xyz;
        L += dirLights[j].colorIntensity.xyz * dropPhase(dot(travel, toCamera));
    }
    // Points and spots from the cluster the drop sits in. Area panels are skipped, like
    // the fog skips them: they light surfaces through an integral the drop does not have.
    uvec2 list = clusterLightListUv(uv, viewDepth);
    for (uint k = 0u; k < list.y; k++) {
        uint li = lightIndexAt(list.x + k);
        if (clusterLights[li].dirType.w == 3.0)
            continue;
        vec3 toL = clusterLights[li].posRange.xyz - P;
        float d2 = dot(toL, toL);
        vec3 dirToL = toL * inversesqrt(max(d2, 1e-8));
        float e = getDistanceAtt(d2, clusterLights[li].attenCutoff.x) * punctualAngular(li, dirToL);
        L += clusterLights[li].colorIntensity.xyz * (e * dropPhase(dot(-dirToL, toCamera)));
    }
    return L * preExposure;
}

void main() {
    int box = gl_InstanceID / dropsPerBox;
    int idx = gl_InstanceID - box * dropsPerBox;
    vec4 r = vec4(pcg4d(uvec4(uint(idx), uint(box), 0x9e3779b9u, 0x85ebca6bu))) / 4294967296.0;

    // Marshall-Palmer by inverting its CDF over the sizes drawn: most drops are small.
    float span = 1.0 - exp(-mpLambda * (RAIN_DROP_MAX_MM - RAIN_DROP_MIN_MM));
    float dMm = RAIN_DROP_MIN_MM - log(1.0 - r.w * span) / mpLambda;
    float fall = max(0.0, RAIN_ATLAS_A - RAIN_ATLAS_B * exp(-RAIN_ATLAS_C * dMm)) * fallScale;
    vec3 vel = rainWind - vec3(0.0, fall, 0.0);

    // Every drop falls in a straight line through the world and is wrapped into a box that
    // follows the camera, so the rain stays put while the camera moves through it.
    float halfSize = boxHalf * pow(3.0, float(box));
    float size = 2.0 * halfSize;
    vec3 rel = mod(r.xyz * size + vel * rainTime - cameraPos, size) - halfSize;
    vec3 P = cameraPos + rel;
    // A drop is fading out as it nears its box's face, where it wraps to the opposite one.
    float edge = max(abs(rel.x), max(abs(rel.y), abs(rel.z))) / halfSize;
    float fade = 1.0 - smoothstep(0.75, 1.0, edge);

    // The streak: where the drop was when the shutter opened, relative to the camera then.
    vec3 tail = P - (vel - cameraVelocity) * shutter;
    vec4 vHead = view * vec4(P, 1.0);
    vec4 vTail = view * vec4(tail, 1.0);
    float near = nearPlaneDist();
    if (-vHead.z <= near || -vTail.z <= near) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // off every clip plane: nothing rasterizes
        vAlpha = 0.0;
        return;
    }
    vec4 cHead = projection * vHead;
    vec4 cTail = projection * vTail;
    vec2 sHead = (cHead.xy / cHead.w * 0.5 + 0.5) * viewport;
    vec2 sTail = (cTail.xy / cTail.w * 0.5 + 0.5) * viewport;

    // The drop's own diameter on screen. What it contributes is its IMAGE AREA spread along
    // the streak -- a pixel the streak crosses is covered for only the instant the drop is
    // over it -- so the opacity is that area over the area drawn. Drawn wider and longer
    // than a pixel when it is smaller, the opacity falls to keep the product.
    //
    // Then times the drops it stands for, so the rain's total light is the real rain's
    // whatever count is drawn: fewer streaks, each carrying more. Capped at opaque, which
    // is where a far box's stand-in stops conserving and the medium takes the rest.
    float pxPerM = projection[1][1] * 0.5 * viewport.y * (projectionIsOrtho() ? 1.0 : 1.0 / -vHead.z);
    float wPx = dMm * 1e-3 * streakWidth * pxPerM;
    float lPx = length(sHead - sTail);
    float drawW = max(wPx, 1.0);
    float drawL = max(lPx + wPx, 1.0);
    vec2 along = lPx > 1e-3 ? (sHead - sTail) / lPx : vec2(0.0, 1.0);
    vec2 across = vec2(-along.y, along.x);

    int corner = gl_VertexID; // triangle strip: 0 tail-left, 1 tail-right, 2 head-left, 3 head-right
    bool atHead = corner >= 2;
    float side = (corner & 1) == 0 ? -1.0 : 1.0;
    float ext = 0.5 * (drawL - lPx);
    vec2 s = (atHead ? sHead + along * ext : sTail - along * ext) +
             across * side * (0.5 * drawW + 0.5);
    vec4 c = atHead ? cHead : cTail;
    gl_Position = vec4((s / viewport * 2.0 - 1.0) * c.w, c.z, c.w);

    vAcrossPx = side * (0.5 * drawW + 0.5);
    vHalfWidth = 0.5 * drawW;
    vAlongPx = atHead ? lPx + ext : -ext;
    vLengthPx = lPx;

    // Rayleigh's n = 2 mode, flashing twice a cycle, over the exposure. The phase moves with
    // the rain's clock at that frequency, so each frame's exposure catches another part of
    // the ring and the glints flicker the way real ones do.
    float radiusM = 0.5e-3 * dMm;
    float ringHz = sqrt(8.0 * RAIN_SURFACE_TENSION / (RAIN_WATER_DENSITY * radiusM * radiusM *
                                                      radiusM)) / (2.0 * PI);
    vGlintBands = 2.0 * ringHz * shutter;
    vGlintPhase = fract(r.x * 7.13 + r.z * 3.71 + 2.0 * ringHz * rainTime);
    // Three pixels a flash at least, or the pattern aliases into noise between drops.
    vGlintShare = glintShare * smoothstep(RAIN_GLINT_D_MIN, RAIN_GLINT_D_FULL, dMm) *
                  clamp(lPx / (3.0 * max(vGlintBands, 1.0)), 0.0, 1.0);
    vViewDepth = -(atHead ? vHead.z : vTail.z);
    // Cover per END, so a streak crossing an eave is cut along its length rather than
    // dropped whole: the head is under the roof while the tail is still in the open.
    vec3 end = atHead ? P : tail;
    float standsFor = dropsPerStreak * pow(27.0, float(box));
    vAlpha = min(wPx * wPx / (drawW * drawL) * standsFor * streakBrightness, 1.0) * fade *
             rainExposure(end);

    vec3 toCamera = normalize(cameraPos - P);
    vLit = dropLit(P, toCamera, sHead / viewport, -vHead.z);
}
