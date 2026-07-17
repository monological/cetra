#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Auto-exposure step 2: eye adaptation. Blend the measured mean log-luminance
// (top mip of the measure target) toward the previous adapted value with a
// fixed per-frame rate -- frame-count based, not wall-clock, so equal --frames
// headless runs stay byte-deterministic.
uniform sampler2D measureTex; // 64x64 log2-luminance, mipmapped
uniform sampler2D historyTex; // 1x1 previous adapted log2-luminance
uniform int reset;            // 1 = first frame: snap instead of blending

const float MEASURE_TOP_MIP = 6.0; // log2(64): the 1x1 average
const float ADAPT_RATE = 0.04;     // Per-frame blend toward the measurement

void main()
{
    float measured = textureLod(measureTex, vec2(0.5), MEASURE_TOP_MIP).r;
    if (reset == 1) {
        FragColor = vec4(measured, 0.0, 0.0, 1.0);
        return;
    }
    float prev = texture(historyTex, vec2(0.5)).r;
    float adapted = mix(prev, measured, ADAPT_RATE);
    // Deadband: once within ~1% of the measurement, snap to it exactly. An
    // exponential blend never quite converges in fp16, so the resting value
    // would depend on the approach path (async texture-load timing) -- the
    // snap makes steady state a pure function of the scene, keeping equal
    // --frames headless runs byte-deterministic.
    if (abs(adapted - measured) < 0.01)
        adapted = measured;
    FragColor = vec4(adapted, 0.0, 0.0, 1.0);
}
