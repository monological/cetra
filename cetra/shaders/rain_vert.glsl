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

uniform float time; // seconds, the engine's clock
uniform vec3 rainWind;
uniform float mpLambda; // Marshall-Palmer slope, 1/mm
uniform float fallScale; // on the terminal velocity; 1 = physical
uniform float shutter;
uniform float boxHalf;  // the innermost box's half-width, m
uniform int dropsPerBox;
// How many real drops each streak in the innermost box stands for: the rain's density times
// the box's volume over the streaks drawn in it. Each next box is RAIN_STREAK_BOX_SCALE cubed
// the volume with the same count, so it stands for that many times as many.
uniform float dropsPerStreak;
uniform float streakWidth;
uniform float streakBrightness;
uniform float forwardG;
uniform float glintShare;

// The splashes: instances past the falling drops, RAIN_SPLASH_DROPLETS to a slot.
uniform int splashSide;         // slots along each side of the square round the camera; 0 = none
uniform float splashCell;       // metres across one slot's cell
uniform float splashFire;       // the chance a slot splashes in a given life, 0..1
uniform float splashStandsFor;  // the splashes each drawn one stands for, 1 or more
uniform float splashSize;       // scale on the droplets' diameter
uniform vec3 rainTravel;        // unit direction the rain, and the occlusion map, looks along
uniform float rainWaterLevel;   // world Y of the still water drawn this frame; far below if none
uniform vec4 rainWaterBounds;   // where that water is: Water.bounds, all zero = everywhere

// The drips (spec 13.12): instances past the splashes, RAIN_DRIP_INSTANCES to a slot. Per line:
// its ends, and its schedule -- cycle in seconds, the chance a slot drips in one, the running
// end of its slots, and the world Y its drops land at.
uniform int dripLines;
uniform vec3 dripFrom[RAIN_DRIP_MAX];
uniform vec3 dripTo[RAIN_DRIP_MAX];
uniform vec4 dripSchedule[RAIN_DRIP_MAX];
uniform float dripTerminal;   // m/s, a RAIN_DRIP_MM drop's terminal velocity, look and all
uniform float dripBrightness; // scale on a drip's opacity, where streakBrightness scales a streak's

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
#include "rain_phase.glsl"

// The drop diameters, in mm, over which the glints come in. A small drop is held round by its
// own surface tension and barely rings, and it rings fastest: given the same flash as a big
// one, the Marshall-Palmer majority under a millimetre striped every streak with twenty to
// thirty bands. The visible oscillation belongs to the big drops, which flash two to four
// times an exposure. Where between these two it rises is a judgment, not a measurement.
const float RAIN_GLINT_D_MIN = 1.0;
const float RAIN_GLINT_D_FULL = 2.5;
// How far back up the rain's path from the camera's height a splash's landing is looked for
// from, in metres: above anything near enough to splash on, and well inside the map's reach.
const float RAIN_SPLASH_DROP_FROM = 50.0;

#include "pcg4d.glsl"
#include "water_bounds.glsl"

// The light the lamps and the sun put into a drop and it sends on toward the eye: each
// source's illuminance there times the fraction scattered this way. What it REFRACTS -- the
// scene behind it -- is the fragment stage's, read from the frame itself.
vec3 dropLit(vec3 P, vec3 toCamera, vec2 uv, float viewDepth) {
    vec3 L = vec3(0.0);
    for (int j = 0; j < lightCounts.x; j++) {
        vec3 travel = dirLights[j].dirShadow.xyz;
        L += dirLights[j].colorIntensity.xyz * rainDropPhase(dot(travel, toCamera), forwardG);
    }
    // Points, spots and area panels from the cluster the drop sits in.
    uvec2 list = clusterLightListUv(uv, viewDepth);
    for (uint k = 0u; k < list.y; k++) {
        uint li = lightIndexAt(list.x + k);
        vec3 toL = clusterLights[li].posRange.xyz - P;
        float d2 = dot(toL, toL);
        vec3 dirToL = toL * inversesqrt(max(d2, 1e-8));
        float e;
        if (clusterLights[li].dirType.w == 3.0) {
            // A panel, as the small source it is at a drop's distance: its luminance over its
            // area, foreshortened as its face turns from the drop, falling as the square of
            // the distance -- floored at the area, so a drop against the panel cannot take
            // more than the panel's own luminance. Unshadowed, like every light here, so the
            // panel reaches drops through the walls round it as well as through its window.
            vec2 size = clusterLights[li].shadowMisc.zw;
            float area = size.x * size.y;
            float facing = max(dot(clusterLights[li].dirType.xyz, -dirToL), 0.0);
            e = area * facing *
                getDistanceAtt(max(d2, area), clusterLights[li].attenCutoff.x);
        } else {
            e = getDistanceAtt(d2, clusterLights[li].attenCutoff.x) * punctualAngular(li, dirToL);
        }
        L += clusterLights[li].colorIntensity.xyz *
             (e * rainDropPhase(dot(-dirToL, toCamera), forwardG));
    }
    return L * preExposure;
}

// A drop, as the streak below needs it: where it is, how fast it goes, its diameter in mm, how
// far it has faded, how many real drops it stands for, the scale on its opacity, whether the
// cover decides if it is there, and the hash its glints are keyed on.
struct Drop {
    vec3 P;
    vec3 vel;
    float dMm;
    float fade;
    float standsFor;
    float brightness;
    bool askCover;
    vec4 r;
};

// Marshall-Palmer by inverting its CDF from `dMin`: most drops are the small ones.
float mpDiameter(float u, float dMin) {
    float span = 1.0 - exp(-mpLambda * (RAIN_DROP_MAX_MM - dMin));
    return dMin - log(1.0 - u * span) / mpLambda;
}

float atlasSpeed(float dMm) {
    return max(0.0, RAIN_ATLAS_A - RAIN_ATLAS_B * exp(-RAIN_ATLAS_C * dMm));
}

Drop fallingDrop(int instance) {
    int box = instance / dropsPerBox;
    int idx = instance - box * dropsPerBox;
    Drop d;
    d.r = vec4(pcg4d(uvec4(uint(idx), uint(box), 0x9e3779b9u, 0x85ebca6bu))) / 4294967296.0;
    d.dMm = mpDiameter(d.r.w, RAIN_DROP_MIN_MM);
    d.vel = rainWind - vec3(0.0, atlasSpeed(d.dMm) * fallScale, 0.0);

    // Every drop falls in a straight line through the world and is wrapped into a box that
    // follows the camera, so the rain stays put while the camera moves through it.
    float halfSize = boxHalf * pow(RAIN_STREAK_BOX_SCALE, float(box));
    float size = 2.0 * halfSize;
    vec3 rel = mod(d.r.xyz * size + d.vel * time - cameraPos, size) - halfSize;
    d.P = cameraPos + rel;
    // A drop is fading out as it nears its box's face, where it wraps to the opposite one.
    float edge = max(abs(rel.x), max(abs(rel.y), abs(rel.z))) / halfSize;
    d.fade = 1.0 - smoothstep(0.75, 1.0, edge);
    d.standsFor = dropsPerStreak * pow(RAIN_STREAK_BOX_SCALE * RAIN_STREAK_BOX_SCALE *
                                           RAIN_STREAK_BOX_SCALE, float(box));
    d.brightness = streakBrightness;
    d.askCover = true;
    return d;
}

/*
 * A slot's cycle of `period` seconds, for the splashes and the drips alike. Its clock is offset
 * by a hash of `key` and `salt`, so slots do not keep time with each other. Returns the life it
 * is in; `age` is the seconds into that life and `h` the life's own hash.
 */
uint slotCycle(uvec2 key, uvec2 salt, float period, out float age, out vec4 h) {
    float clock = time / period + float(pcg4d(uvec4(key, salt)).x) / 4294967296.0;
    float life = floor(clock);
    age = (clock - life) * period;
    uint lifeIndex = uint(int(life));
    h = vec4(pcg4d(uvec4(key, lifeIndex, 0x68e31da4u))) / 4294967296.0;
    return lifeIndex;
}

// The tallest step, in metres along the rain, between two neighbouring texels of the map that
// is still one surface rather than the drop off its edge.
const float RAIN_SPLASH_MAX_STEP = 0.25;

/*
 * The depth of the SURFACE the rain reaches at lookup uv `uv`, which the map does not hold
 * directly: it was drawn with a slope bias, so a surface the rain meets at an angle is stored
 * deeper than it is by RAIN_MAP_SLOPE_BIAS of its step a texel -- 5 cm on flat ground under
 * a moderate wind, which buries a droplet that rises only a few. Three taps give the plane
 * through the texel, which puts the depth at `uv` itself rather than at the texel's centre and
 * takes the bias back out. False where there is nothing, or where the step is an edge.
 */
bool rainSurfaceDepth(vec2 uv, out float depth) {
    vec2 size = vec2(textureSize(punctualShadowMaps, 0).xy);
    vec2 at = uv * size - 0.5;
    ivec2 texel = ivec2(floor(at));
    vec2 frac = at - vec2(texel);
    float m = texelFetch(punctualShadowMaps, ivec3(texel, rainOcclusionLayer), 0).r;
    float mx = texelFetch(punctualShadowMaps, ivec3(texel + ivec2(1, 0), rainOcclusionLayer), 0).r;
    float my = texelFetch(punctualShadowMaps, ivec3(texel + ivec2(0, 1), rainOcclusionLayer), 0).r;
    if (max(m, max(mx, my)) >= 1.0)
        return false;
    vec2 step = vec2(mx - m, my - m);
    float slope = max(abs(step.x), abs(step.y));
    if (slope * RAIN_MAP_DEPTH_METRES > RAIN_SPLASH_MAX_STEP)
        return false;
    depth = m + dot(frac, step) - RAIN_MAP_SLOPE_BIAS * slope -
            RAIN_MAP_CONSTANT_BIAS / 16777216.0; // one unit of a 24-bit depth
    return true;
}

/*
 * Droplet `k` of the crown a drop `impactMm` across throws on striking at `impactSpeed`,
 * `age` seconds after it struck at `hit`. The droplets leave at a fraction of the impact
 * speed, steeply, round the whole circle: a judgment on the crown's shape rather than a
 * measurement of it, which is what the speed and angle bands below are. `key` names the
 * strike. False once the droplet has landed.
 */
bool crownDroplet(vec3 hit, float impactMm, float impactSpeed, float age, int k, uvec3 key,
                  out Drop d) {
    d.r = vec4(pcg4d(uvec4(key.xy, key.z + uint(k), 0x1b873593u))) / 4294967296.0;
    float az = 6.2831853 * (float(k) + d.r.x) / float(RAIN_SPLASH_DROPLETS);
    float elev = mix(0.8, 1.3, d.r.y);
    vec3 v0 = impactSpeed * mix(0.08, 0.2, d.r.z) *
              vec3(cos(elev) * cos(az), sin(elev), cos(elev) * sin(az));
    d.vel = v0 - vec3(0.0, RAIN_GRAVITY * age, 0.0);
    d.P = hit + v0 * age - vec3(0.0, 0.5 * RAIN_GRAVITY * age * age, 0.0);
    d.dMm = impactMm * mix(0.15, 0.35, d.r.w) * splashSize;
    return d.P.y >= hit.y;
}

/*
 * A droplet a splash throws. The slots are cells of a grid anchored in the WORLD, which the
 * camera carries a whole cell at a time, and each cell is hashed on its world index -- so a
 * splash stays where it landed as the eye walks past it. A cell's clock is offset by its hash,
 * so the cells do not all splash on the same frame.
 *
 * What it lands on is the occlusion map's answer: from well above the cell, down the rain's
 * travel by the depth the map holds there, which is the roof, the car or the road the rain
 * actually reaches. Nothing under the map, or a map with no layer this frame, is no splash.
 * The drop that made it is Marshall-Palmer above the splash floor.
 */
bool splashDroplet(int instance, out Drop d) {
    int slot = instance / RAIN_SPLASH_DROPLETS;
    int k = instance - slot * RAIN_SPLASH_DROPLETS;
    int row = slot / splashSide;
    ivec2 cell = ivec2(floor(cameraPos.xz / splashCell)) - splashSide / 2 +
                 ivec2(slot - row * splashSide, row);
    uvec2 key = uvec2(cell);
    float age;
    vec4 h;
    uint life = slotCycle(key, uvec2(0x51ed270bu, 0x2c1b3c6du), RAIN_SPLASH_LIFE, age, h);
    if (h.w >= splashFire || rainOcclusionLayer < 0)
        return false;

    // Back up the rain's own path, not straight up: in any wind a drop falls at an angle, and
    // one followed down from overhead lands tens of metres downwind of the cell.
    vec2 xz = (vec2(cell) + h.xy) * splashCell;
    vec3 above = vec3(xz.x, cameraPos.y, xz.y) - rainTravel * RAIN_SPLASH_DROP_FROM;
    vec3 pc = rainMapCoord(above);
    float map;
    if (!rainSurfaceDepth(pc.xy, map) || map <= pc.z)
        return false;
    vec3 hit = above + rainTravel * ((map - pc.z) * RAIN_MAP_DEPTH_METRES);
    // Water is not in the occlusion map, so over flooded ground the map answers the bed: the
    // drop strikes the surface first, back up its own path. Waves are not followed.
    if (waterUnder(rainWaterLevel, rainWaterBounds, hit))
        hit += rainTravel * ((rainWaterLevel - hit.y) / rainTravel.y);

    float impact = mpDiameter(h.z, RAIN_SPLASH_MIN_MM);
    if (!crownDroplet(hit, impact, atlasSpeed(impact), age, k,
                      uvec3(key, life * uint(RAIN_SPLASH_DROPLETS)), d))
        return false;
    // Fading out toward the square's edge, where the camera's next step moves the grid.
    vec2 rel = abs(xz - cameraPos.xz) / (0.5 * float(splashSide) * splashCell);
    d.fade = 1.0 - smoothstep(0.75, 1.0, max(rel.x, rel.y));
    d.standsFor = splashStandsFor;
    d.brightness = streakBrightness;
    d.askCover = true;
    return true;
}

// A drip's fall under linear drag, the curve rain.c solves on the CPU for the cycle: how fast a
// drop let go from rest is falling after `s` seconds, how far it has fallen, and the time it
// takes to fall `h`.
float dripSpeed(float s) {
    float tau = dripTerminal / RAIN_GRAVITY;
    return dripTerminal * (1.0 - exp(-s / tau));
}

float dripFallen(float s) {
    float tau = dripTerminal / RAIN_GRAVITY;
    return dripTerminal * (s - tau * (1.0 - exp(-s / tau)));
}

float dripFallTime(float h) {
    if (h <= 0.0)
        return 0.0;
    float t = sqrt(2.0 * h / RAIN_GRAVITY);
    for (int i = 0; i < RAIN_DRIP_NEWTON_STEPS; i++)
        t -= (dripFallen(t) - h) / dripSpeed(t);
    return t;
}

/*
 * A drip: slot `instance` / RAIN_DRIP_INSTANCES of the line whose running end first passes it,
 * the drop itself at 0 and its splash's droplets after. Each cycle the slot drips with its
 * line's chance, from a point hashed along the line: the drop hangs from the edge growing as its
 * volume does, lets go, falls, and throws a crown where it lands.
 *
 * Straight down, where the streaks drift: a drip leaves from the lee of what it drips off, where
 * the wind is broken, and carried with the open air a gutter's drops cross the wall under it.
 * And it does not ask the cover, which would cut every drop under the edge it came off.
 */
bool dripInstance(int instance, out Drop d) {
    int slot = instance / RAIN_DRIP_INSTANCES;
    int k = instance - slot * RAIN_DRIP_INSTANCES;
    int line = 0;
    while (line < dripLines - 1 && float(slot) >= dripSchedule[line].z)
        line++;
    vec4 schedule = dripSchedule[line];
    float period = schedule.x;
    uvec2 key = uvec2(uint(slot), 0x7feb352du);
    float t;
    vec4 h;
    uint life = slotCycle(key, uvec2(0x2c1b3c6du, 0x297a2d39u), period, t, h);
    if (h.w >= schedule.y)
        return false;

    vec3 at = mix(dripFrom[line], dripTo[line], h.x);
    float fall = dripFallTime(at.y - schedule.w);
    float letGo = period - RAIN_SPLASH_LIFE - fall;
    bool alive;
    if (k > 0) {
        float age = t - letGo - fall;
        alive = age >= 0.0 &&
                crownDroplet(vec3(at.x, schedule.w, at.z), RAIN_DRIP_MM, dripSpeed(fall), age,
                             k - 1, uvec3(key.x, life, 0x165667b1u), d);
    } else if (t < letGo) {
        // Hanging, fed at a steady rate, so its diameter goes as the cube root of the time.
        d.r = h;
        d.dMm = RAIN_DRIP_MM * pow(t / letGo, 1.0 / 3.0);
        d.vel = vec3(0.0);
        d.P = at - vec3(0.0, 0.5e-3 * d.dMm, 0.0);
        alive = true;
    } else {
        float s = t - letGo;
        d.r = h;
        d.dMm = RAIN_DRIP_MM;
        d.vel = vec3(0.0, -dripSpeed(s), 0.0);
        d.P = at - vec3(0.0, dripFallen(s), 0.0);
        alive = s < fall;
    }
    d.fade = 1.0;
    d.standsFor = 1.0;
    d.brightness = dripBrightness;
    d.askCover = false;
    return alive;
}

void main() {
    Drop d;
    int falling = RAIN_STREAK_BOXES * dropsPerBox;
    int splashes = splashSide * splashSide * RAIN_SPLASH_DROPLETS;
    bool alive = true;
    if (gl_InstanceID < falling)
        d = fallingDrop(gl_InstanceID);
    else if (gl_InstanceID < falling + splashes)
        alive = splashDroplet(gl_InstanceID - falling, d);
    else
        alive = dripInstance(gl_InstanceID - falling - splashes, d);

    // The streak: where the drop was when the shutter opened, relative to the camera then.
    vec3 tail = d.P - (d.vel - cameraVelocity) * shutter;
    vec4 vHead = view * vec4(d.P, 1.0);
    vec4 vTail = view * vec4(tail, 1.0);
    float near = nearPlaneDist();
    if (!alive || -vHead.z <= near || -vTail.z <= near) {
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
    float pxPerM = projection[1][1] * 0.5 * viewport.y / clipWAt(vHead.z);
    float wPx = d.dMm * 1e-3 * streakWidth * pxPerM;
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
    float radiusM = 0.5e-3 * d.dMm;
    float ringHz = sqrt(8.0 * RAIN_SURFACE_TENSION / (RAIN_WATER_DENSITY * radiusM * radiusM *
                                                      radiusM)) / (2.0 * PI);
    vGlintBands = 2.0 * ringHz * shutter;
    vGlintPhase = fract(d.r.x * 7.13 + d.r.z * 3.71 + 2.0 * ringHz * time);
    // Three pixels a flash at least, or the pattern aliases into noise between drops.
    vGlintShare = glintShare * smoothstep(RAIN_GLINT_D_MIN, RAIN_GLINT_D_FULL, d.dMm) *
                  clamp(lPx / (3.0 * max(vGlintBands, 1.0)), 0.0, 1.0);
    vViewDepth = -(atHead ? vHead.z : vTail.z);
    // Cover per END, so a streak crossing an eave is cut along its length rather than
    // dropped whole: the head is under the roof while the tail is still in the open.
    vec3 end = atHead ? d.P : tail;
    vAlpha = min(wPx * wPx / (drawW * drawL) * d.standsFor * d.brightness, 1.0) * d.fade *
             (d.askCover ? rainExposure(end) : 1.0);

    vec3 toCamera = normalize(cameraPos - d.P);
    vLit = dropLit(d.P, toCamera, sHead / viewport, -vHead.z);
}
