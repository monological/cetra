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

#include "display.glsl"
#include "dither.glsl"

uniform sampler2D signalTex; // the signal, linear, signalSize texels
uniform vec2 signalSize;     // the signal's texels across and down
uniform vec2 outputSize;     // the window's pixels across and down
uniform float curvature;     // 0 = a flat tube, 1 = WARP_MAX
uniform float bleed;         // 0 = colour as sharp as brightness, 1 = CHROMA_BLUR_MAX
uniform float thin;          // CRTS's scanline thinness: 0.5 = fused lines, 1.0 = thin
uniform float maskDark;      // a masked phosphor's exposure: 1 = no mask
uniform vec2 tone;           // CRTS's tone.y and tone.z: the exposure match
uniform int ditherEnabled;
uniform float ditherStrength;

// CRTS's default horizontal filter, exp2(LUMA_BLUR * d^2) per texel.
const float LUMA_BLUR = -2.5;
// The composite colour's filter at bleed 1. Composite video carries colour in a narrower band
// than brightness (NTSC's I and Q beside Y), so a console's colour smeared sideways where its
// brightness did not -- not CRTS. The value is taste bounded by the six taps: it leaves 15% weight
// at three texels, where NTSC's I bandwidth would ask about -0.24 and reach past the window.
const float CHROMA_BLUR_MAX = -0.3;
// The warp at curvature 1, CRTS's "more warping" doubled.
const float WARP_MAX = 0.0625;
// The rounding of the tube's corners, the libretro port's CORNER parameter.
const float CORNER = 3.0;
// The mask's pixel is a window pixel up to this many lines, and a whole multiple of one past it,
// so a phosphor stays a size the eye can resolve on a high-density display.
const float MASK_LINES = 1080.0;

const vec3 LUMA = vec3(0.299, 0.587, 0.114);

vec3 signalAt(int x, int y)
{
    ivec2 last = ivec2(signalSize) - 1;
    return texelFetch(signalTex, clamp(ivec2(x, y), ivec2(0), last), 0).rgb;
}

// One line of the signal at `px` (signal texels across): six texels, the middle four of which
// are CRTS's. Brightness takes the narrow filter and colour the wide one: the wide filter's
// colour with its luma replaced by the narrow filter's.
vec3 beamLine(float px, int y, float chromaBlur)
{
    float x0 = floor(px - 2.5) + 0.5; // the first of the six texel centres
    int xi = int(floor(x0));
    vec3 narrow = vec3(0.0), wide = vec3(0.0);
    float narrowW = 0.0, wideW = 0.0;
    for (int k = 0; k < 6; k++) {
        float d = px - (x0 + float(k));
        vec3 rgb = signalAt(xi + k, y);
        float a = exp2(LUMA_BLUR * d * d);
        float b = exp2(chromaBlur * d * d);
        narrow += rgb * a;
        narrowW += a;
        wide += rgb * b;
        wideW += b;
    }
    narrow /= narrowW;
    wide /= wideW;
    return max(wide + dot(narrow - wide, LUMA), 0.0);
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
    // the edge of the picture the screen is black, rounded at the corners. The vertical bow is
    // scaled by the window's shape so the tube curves alike along both edges.
    vec2 warp = curvature * WARP_MAX * vec2(1.0, outputSize.y / outputSize.x);
    vec2 pos = gl_FragCoord.xy * (2.0 / outputSize) - 1.0;
    pos *= vec2(1.0 + pos.y * pos.y * warp.x, 1.0 + pos.x * pos.x * warp.y);
    float vin = (1.0 - (1.0 - clamp(pos.x * pos.x, 0.0, 1.0)) * (1.0 - clamp(pos.y * pos.y, 0.0, 1.0))) *
                (0.998 + 0.001 * CORNER);
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
    float chromaBlur = mix(LUMA_BLUR, CHROMA_BLUR_MAX, bleed);
    vec3 color = (beamLine(pos.x, yi, chromaBlur) * scanA + beamLine(pos.x, yi + 1, chromaBlur) * scanB) * vin;

    // The mask in window pixels, unwarped, as CRTS has it: its fineness is the display's.
    float maskScale = max(1.0, floor(outputSize.y / MASK_LINES + 0.5));
    color *= slotMask(floor(gl_FragCoord.xy / maskScale) + 0.5, maskDark);

    // CRTS's exposure match: the beams and the mask both darken, and this lifts mid-grey back
    // to where it was, compressing highlights rather than clipping them.
    float peak = max(max(color.r, color.g), max(color.b, 1.0 / (256.0 * 65536.0)));
    vec3 ratio = color / peak;
    peak = peak / (peak * tone.x + tone.y);
    color = ratio * peak;

    color = displayEncode(color);
    if (ditherEnabled == 1)
        color = applyDither(color, gl_FragCoord.xy, ditherStrength);
    FragColor = vec4(color, 1.0);
}
