#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Motion blur (4.15) -- McGuire (2012) depth-aware reconstruction. Gathers the
// linear-HDR scene along the tile's dominant velocity (neighbor-max) and
// weights each tap by a soft depth comparison + velocity cones, so a fast
// FOREGROUND object blurs OVER a static background while a static foreground is
// not smeared by the background's motion. Tap positions are jittered by
// interleaved-gradient noise, trading banding for fine noise. Blitted back over
// the HDR scene by the C side.
uniform sampler2D sceneTex;       // Resolved linear HDR
uniform sampler2D neighborMaxTex; // Per-tile dominant velocity (UV units, un-jittered)
uniform sampler2D velocityTex;    // aux G-buffer: .xy per-pixel velocity (UV), .z view-space Z
uniform vec2 texelSize;           // 1 / render resolution
uniform float scale;              // Shutter: velocity multiplier (motion_blur_scale)

const int SAMPLES = 16;
const float MAX_PIXELS = 20.0; // clamp blur to the tile size (neighbor-max reach)
const float SOFT_Z = 1.0;      // soft depth-compare extent (view-space units)

// Interleaved-gradient noise in [0,1) (Jimenez) -- cheap per-pixel dither.
float ign(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// 1 when a is (softly) in front of b, 0 behind. Distances are POSITIVE.
float zCompare(float a, float b) {
    return clamp(1.0 - (a - b) / SOFT_Z, 0.0, 1.0);
}

// Full weight at the tap, falling to 0 as the offset reaches the blur length.
float cone(float dist, float vlen) {
    return clamp(1.0 - dist / vlen, 0.0, 1.0);
}

// Full weight within the blur length, soft edge just past it.
float cylinder(float dist, float vlen) {
    return 1.0 - smoothstep(0.95 * vlen, 1.05 * vlen, dist);
}

void main()
{
    vec3 centerColor = texture(sceneTex, TexCoords).rgb;

    // Dominant velocity for this tile (the extent blur may reach), in UV + px.
    vec2 vN = texture(neighborMaxTex, TexCoords).xy * scale;
    float vNlenPx = length(vN / texelSize);

    // No meaningful motion in this neighborhood: return the exact centre texel
    // so a static scene renders byte-identical to the motion-blur-off path.
    if (vNlenPx < 0.5) {
        FragColor = vec4(centerColor, 1.0);
        return;
    }
    vN *= min(vNlenPx, MAX_PIXELS) / vNlenPx; // clamp to the tile reach

    // This pixel's own velocity + depth.
    vec4 cx = texture(velocityTex, TexCoords);
    float vClenPx = max(length(cx.xy * scale / texelSize), 0.5);
    float depthX = -cx.z; // view Z is negative in front -> positive distance

    float jitter = ign(gl_FragCoord.xy) - 0.5;

    // Centre weighted by its own inverse speed (a slow pixel keeps more of
    // itself; a fast one spreads across its neighbours).
    float weight = 1.0 / vClenPx;
    vec3 sum = centerColor * weight;

    for (int i = 0; i < SAMPLES; i++) {
        float t = (float(i) + 0.5 + jitter) / float(SAMPLES) - 0.5; // ~(-0.5, 0.5)
        vec2 offUv = vN * t;
        vec2 sampUv = TexCoords + offUv;
        float distPx = length(offUv / texelSize);

        vec4 sy = texture(velocityTex, sampUv);
        float depthY = -sy.z;
        float vYlenPx = max(length(sy.xy * scale / texelSize), 0.5);

        // Soft depth ordering: does Y sit in front of X (Y's blur covers X), or
        // X in front of Y (X's blur covers Y)?
        float f = zCompare(depthY, depthX);
        float b = zCompare(depthX, depthY);

        float w = f * cone(distPx, vYlenPx) +               // Y (foreground) blurs over X
                  b * cone(distPx, vClenPx) +               // X (foreground) blurs over Y
                  cylinder(distPx, vYlenPx) * cylinder(distPx, vClenPx) * 2.0; // coherent motion

        sum += texture(sceneTex, sampUv).rgb * w;
        weight += w;
    }

    FragColor = vec4(sum / weight, 1.0);
}
