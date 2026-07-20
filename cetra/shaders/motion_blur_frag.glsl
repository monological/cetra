#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Motion blur (4.15) -- M1 per-pixel reconstruction. Gathers the linear-HDR
// scene along this pixel's own screen-space velocity (from the aux G-buffer,
// UV units, un-jittered), approximating the radiance integrated over the
// camera shutter. M2 replaces the per-pixel velocity with a tile/neighbour-max
// dilation so fast objects blur past their silhouette; M3 adds depth-aware tap
// weighting. Output is blitted back over the HDR scene by the C side.
uniform sampler2D sceneTex; // Resolved linear HDR
uniform sampler2D auxTex;   // .xy = velocity (UV units, un-jittered), .z = view-Z
uniform vec2 texelSize;     // 1 / render resolution
uniform float scale;        // Shutter: velocity multiplier (motion_blur_scale)

const int SAMPLES = 16;
const float MAX_PIXELS = 20.0; // clamp blur to the (future M2) tile size

void main()
{
    vec3 center = texture(sceneTex, TexCoords).rgb;

    // Velocity in pixels for this fragment (UV delta / texel size), scaled by the shutter.
    vec2 velUv = texture(auxTex, TexCoords).xy * scale;
    float lenPx = length(velUv / texelSize);

    // Sub-pixel motion: nothing to integrate. Return the exact centre texel so a
    // static scene renders byte-identical to the motion-blur-off path.
    if (lenPx < 0.5) {
        FragColor = vec4(center, 1.0);
        return;
    }

    // Clamp the blur extent so a tap never leaves the (future) neighbour-max reach.
    vec2 stepUv = velUv * (min(lenPx, MAX_PIXELS) / lenPx);

    // Symmetric gather across [-0.5, +0.5] of the velocity, centred on this pixel.
    vec3 sum = vec3(0.0);
    for (int i = 0; i < SAMPLES; i++) {
        float t = (float(i) + 0.5) / float(SAMPLES) - 0.5; // -0.5 .. +0.5
        sum += texture(sceneTex, TexCoords + stepUv * t).rgb;
    }
    FragColor = vec4(sum / float(SAMPLES), 1.0);
}
