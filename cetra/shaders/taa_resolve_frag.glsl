#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

uniform sampler2D currentTex;  // This frame's freshly resolved color (jittered)
uniform sampler2D velocityTex; // Screen-space motion vectors (.xy, UV units)
uniform sampler2D historyTex;  // Accumulated previous frame
uniform vec2 texelSize;        // 1/width, 1/height of the internal resolution
uniform int reset;             // 1 on the very first frame: no valid history yet

// YCoCg separates luma from chroma, so the neighborhood clamp below bounds
// ghosting far better than clamping in RGB (a small luma change no longer drags
// the full colour). The transform is linear, so it is safe on linear HDR.
vec3 rgbToYCoCg(vec3 c) {
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                0.5 * c.r - 0.5 * c.b,
                -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 yCoCgToRgb(vec3 c) {
    float t = c.x - c.z;
    return vec3(t + c.y, c.x + c.z, t - c.y);
}

// Sharp (bicubic) history sampling. Plain bilinear reprojection smears the
// history a little every frame, which compounds into the classic TAA blur;
// Catmull-Rom keeps edges crisp for the cost of a handful of extra taps.
vec3 sampleHistoryCatmullRom(sampler2D tex, vec2 uv, vec2 res) {
    vec2 samplePos = uv * res;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;

    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;

    vec2 texPos0 = (texPos1 - 1.0) / res;
    vec2 texPos3 = (texPos1 + 2.0) / res;
    vec2 texPos12 = (texPos1 + offset12) / res;

    vec3 result = vec3(0.0);
    result += texture(tex, vec2(texPos0.x, texPos0.y)).rgb * w0.x * w0.y;
    result += texture(tex, vec2(texPos12.x, texPos0.y)).rgb * w12.x * w0.y;
    result += texture(tex, vec2(texPos3.x, texPos0.y)).rgb * w3.x * w0.y;
    result += texture(tex, vec2(texPos0.x, texPos12.y)).rgb * w0.x * w12.y;
    result += texture(tex, vec2(texPos12.x, texPos12.y)).rgb * w12.x * w12.y;
    result += texture(tex, vec2(texPos3.x, texPos12.y)).rgb * w3.x * w12.y;
    result += texture(tex, vec2(texPos0.x, texPos3.y)).rgb * w0.x * w3.y;
    result += texture(tex, vec2(texPos12.x, texPos3.y)).rgb * w12.x * w3.y;
    result += texture(tex, vec2(texPos3.x, texPos3.y)).rgb * w3.x * w3.y;
    return result;
}

void main() {
    vec2 uv = TexCoords;
    vec3 current = texture(currentTex, uv).rgb;

    if (reset != 0) {
        FragColor = vec4(current, 1.0);
        return;
    }

    // Reproject: read history from where this pixel was last frame.
    vec2 velocity = texture(velocityTex, uv).xy;
    vec2 histUv = uv - velocity;

    // Reprojected off-screen (disocclusion / camera turned onto new content):
    // there is no valid history, so fall back to the current frame.
    if (histUv.x < 0.0 || histUv.x > 1.0 || histUv.y < 0.0 || histUv.y > 1.0) {
        FragColor = vec4(current, 1.0);
        return;
    }

    vec3 history = sampleHistoryCatmullRom(historyTex, histUv, 1.0 / texelSize);

    // Neighborhood clamp: bound the history to the 3x3 YCoCg min/max of the
    // current frame. Moving edges and disoccluded pixels whose history falls
    // outside this box get clamped to plausible current colour instead of
    // ghosting or trailing.
    vec3 centerYCoCg = rgbToYCoCg(current);
    vec3 nmin = centerYCoCg;
    vec3 nmax = centerYCoCg;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            if (x == 0 && y == 0)
                continue;
            vec3 n = rgbToYCoCg(texture(currentTex, uv + vec2(float(x), float(y)) * texelSize).rgb);
            nmin = min(nmin, n);
            nmax = max(nmax, n);
        }
    }
    history = yCoCgToRgb(clamp(rgbToYCoCg(history), nmin, nmax));

    // Blend: heavy history weight accumulates the sub-pixel-jittered samples
    // into a stable, anti-aliased image.
    FragColor = vec4(mix(current, history, 0.9), 1.0);
}
