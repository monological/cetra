#version 330 core
in vec2 TexCoords;
out vec4 FragColor; // .r = exp(k * depth), the filterable shadow term

// Exponential shadow maps for the fog volume (spec 11.12), the representation
// the AC4 paper reaches for and the reason it can afford to filter at all:
//
//   Our regular shadowing cascades were very high resolution ... It was way too
//   much detail for us ... For smooth and approximate volumetric fog we needed
//   something of much smaller resolution to reduce any flickering / aliasing
//   artifacts from moving foliage etc. First implementations using wide kernel
//   PCF had very poor performance and still some flickering and aliasing.
//
// A depth compare is a step function of the stored depth, so averaging depths
// and then comparing is NOT the average of the comparisons -- which is why a
// depth map cannot simply be downsampled, and why widening the PCF kernel is
// the only lever a binary tap has. Storing exp(k*d) moves the comparison into
// a product: exp(k*d_blocker) * exp(-k*d_receiver) approximates the visibility
// directly, and a product of exponentials survives linear filtering. That makes
// the shadow term downsamplable and blurrable, which is the whole point -- the
// medium wants a soft, low-frequency occlusion, not the surface's exact one.
//
// This buffer is the FOG's alone. Surface shading keeps the exact depth array:
// ESM's failure mode is light leaking near a blocker, tolerable in a medium
// (the paper says as much) and not on a contact shadow.

uniform sampler2DArray srcDepth; // mode 0: the scene's depth cascades
uniform sampler2DArray srcEsm;   // mode 1: this buffer, mid-blur
uniform int layer;               // Array layer being written
uniform float esmK;              // Exponent scale; higher is sharper and less filterable
uniform vec2 blurStep;           // mode 1: one texel along the axis being blurred, 0 on the other
uniform int mode;                // 0 = downsample + exponentiate, 1 = separable blur

void main() {
    if (mode == 0) {
        // A 2x2 box in the SOURCE, which is what makes this a downsample rather
        // than a point resample: the target is a fraction of the source size, so
        // a single tap would alias in exactly the frequencies this pass exists
        // to remove. Exponentiating each tap before averaging is required, not
        // an optimisation -- averaging depth first would be the step-function
        // mistake described above.
        vec2 texel = 1.0 / vec2(textureSize(srcDepth, 0).xy);
        float sum = 0.0;
        for (int y = 0; y < 2; y++) {
            for (int x = 0; x < 2; x++) {
                vec2 uv = TexCoords + (vec2(x, y) - 0.5) * texel;
                sum += exp(esmK * texture(srcDepth, vec3(uv, float(layer))).r);
            }
        }
        FragColor = vec4(sum * 0.25, 0.0, 0.0, 1.0);
        return;
    }

    // Separable box, run once per axis. Linear in the stored quantity, which is
    // legal precisely because the quantity is already exponentiated.
    float sum = 0.0;
    for (int i = -2; i <= 2; i++) {
        vec2 uv = TexCoords + blurStep * float(i);
        sum += texture(srcEsm, vec3(uv, float(layer))).r;
    }
    FragColor = vec4(sum * 0.2, 0.0, 0.0, 1.0);
}
