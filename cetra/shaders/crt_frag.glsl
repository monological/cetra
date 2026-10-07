#version 330 core

// A consumer television (spec 13.28), after Lottes' CRTS (2018, public domain): each window pixel
// is lit by the two nearest lines of the signal, each a soft horizontal beam, under a slot mask,
// on a slightly curved tube. Written from CRTS's source and named after its parts; what is not
// CRTS is said where it is.
//
// The signal is LINEAR (crt_resample_frag) and the beam and the mask are light, so all of this is
// in light; the display encode and the output dither come last, this being the pass that writes
// the window.

in vec2 TexCoords;
out vec4 FragColor;

#include "dither.glsl"

uniform sampler2D signalTex; // the signal, linear, signalSize texels
uniform vec2 signalSize;     // the signal's texels across and down
uniform vec2 outputSize;     // the window's pixels across and down
uniform vec2 warp;           // CRTS's warp: how far each axis bows, by the other's square
uniform float corner;        // CRTS's rounding of the tube's corners
uniform float thin;          // CRTS's scanline thinness: 0.5 = fused lines, 1.0 = thin
uniform float blur;          // CRTS's horizontal filter, exp2(blur * d^2) per texel
uniform float chromaBlur;    // the same for the composite colour, wider than blur
uniform float maskDark;      // a masked phosphor's exposure: 1 = no mask
uniform float maskScale;     // window pixels per mask pixel
uniform vec2 tone;           // CRTS's tone.y and tone.z: the exposure match
uniform int ditherEnabled;
uniform float ditherStrength;

// Composite video carries colour as two narrow chroma signals beside a wide luma one (NTSC's I
// and Q beside Y), so a console's colour smeared sideways where its brightness did not. Not CRTS.
const mat3 RGB_TO_YIQ = mat3(0.299, 0.596, 0.211, 0.587, -0.274, -0.523, 0.114, -0.322, 0.312);
const mat3 YIQ_TO_RGB = mat3(1.0, 1.0, 1.0, 0.956, -0.272, -1.106, 0.621, -0.647, 1.703);

vec3 signalAt(int x, int y)
{
    ivec2 last = ivec2(signalSize) - 1;
    return texelFetch(signalTex, clamp(ivec2(x, y), ivec2(0), last), 0).rgb;
}

// One line of the signal at `px` (signal texels across): six texels, the middle four of which
// are CRTS's, filtered as luma at `blur` and as chroma at the wider `chromaBlur`.
vec3 beamLine(float px, int y)
{
    float x0 = floor(px - 2.5) + 0.5; // the first of the six texel centres
    int xi = int(floor(x0));
    vec3 lumaSum = vec3(0.0), chromaSum = vec3(0.0);
    float lumaW = 0.0, chromaW = 0.0;
    for (int k = 0; k < 6; k++) {
        float d = px - (x0 + float(k));
        vec3 yiq = RGB_TO_YIQ * signalAt(xi + k, y);
        float wl = exp2(blur * d * d);
        float wc = exp2(chromaBlur * d * d);
        lumaSum += yiq * wl;
        lumaW += wl;
        chromaSum += yiq * wc;
        chromaW += wc;
    }
    vec3 yiq = vec3(lumaSum.x / lumaW, chromaSum.yz / chromaW);
    return max(YIQ_TO_RGB * yiq, 0.0);
}

// Crt-lottes's "very compressed TV style" mask (Lottes 2014, public domain), a slot mask: RGB
// stripes a pixel each, and a dark row every second line, offset a row in alternate groups of
// three columns. Darkened only, CRTS's way, so a bright pixel is never pushed past white.
vec3 slotMask(vec2 pos, float dark)
{
    float line = 1.0;
    float odd = fract(pos.x * (1.0 / 6.0)) < 0.5 ? 1.0 : 0.0;
    if (fract((pos.y + odd) * 0.5) < 0.5)
        line = dark;
    float x = fract(pos.x * (1.0 / 3.0));
    vec3 m = vec3(dark);
    if (x < 1.0 / 3.0)
        m.r = 1.0;
    else if (x < 2.0 / 3.0)
        m.g = 1.0;
    else
        m.b = 1.0;
    return m * line;
}

void main()
{
    // CRTS's warp, from the window's pixel to the signal's, and its fade at the tube's rim: past
    // the edge of the picture the screen is black, rounded at the corners.
    vec2 pos = gl_FragCoord.xy * (2.0 / outputSize) - 1.0;
    pos *= vec2(1.0 + pos.y * pos.y * warp.x, 1.0 + pos.x * pos.x * warp.y);
    float vin = (1.0 - (1.0 - clamp(pos.x * pos.x, 0.0, 1.0)) * (1.0 - clamp(pos.y * pos.y, 0.0, 1.0))) *
                (0.998 + 0.001 * corner);
    vin = clamp(-vin * signalSize.y + signalSize.y, 0.0, 1.0);
    pos = pos * (0.5 * signalSize) + 0.5 * signalSize;

    // The two lines either side, each a windowed-cosine beam: at thin 0.5 the two sum to 1
    // everywhere and the lines fuse; thinner, a dark gap opens between them.
    float y0 = floor(pos.y - 0.5) + 0.5;
    float off = pos.y - y0;
    const float TAU = 6.28318530717958;
    float scanA = cos(min(0.5, off * thin) * TAU) * 0.5 + 0.5;
    float scanB = cos(min(0.5, (1.0 - off) * thin) * TAU) * 0.5 + 0.5;
    int yi = int(floor(y0));
    vec3 color = (beamLine(pos.x, yi) * scanA + beamLine(pos.x, yi + 1) * scanB) * vin;

    // The mask in window pixels, unwarped, as CRTS has it: its fineness is the display's.
    color *= slotMask(floor(gl_FragCoord.xy / maskScale) + 0.5, maskDark);

    // CRTS's exposure match: the beams and the mask both darken, and this lifts mid-grey back
    // to where it was, compressing highlights rather than clipping them.
    float peak = max(max(color.r, color.g), max(color.b, 1.0 / (256.0 * 65536.0)));
    vec3 ratio = color / peak;
    peak = peak / (peak * tone.x + tone.y);
    color = ratio * peak;

    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));
    if (ditherEnabled == 1)
        color = applyDither(color, gl_FragCoord.xy, ditherStrength);
    FragColor = vec4(color, 1.0);
}
