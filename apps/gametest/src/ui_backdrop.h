#ifndef _GAMETEST_UI_BACKDROP_H_
#define _GAMETEST_UI_BACKDROP_H_

/*
 * A custom fragment stage for one UI element (spec 12.2, escape hatch 2), built over the UI's own
 * vertex stage by create_ui_draw_program.
 *
 * It is written against the contract ui_frag.glsl declares -- uRect,
 * uResolution, uTime, uFocus, uTex -- so an element swaps between this and the
 * default with no C change. The app carries its own GLSL the way apps/network
 * does; this is a plain string rather than a generated one, because the shader
 * header is built from cetra/shaders and an app's shader is not the engine's.
 */

static const char* const UI_BACKDROP_FRAG =
    "#version 330 core\n"
    "in vec2 vPos;\n"
    "in vec4 vColor;\n"
    "out vec4 FragColor;\n"
    "uniform vec4 uRect;\n"
    "uniform vec2 uResolution;\n"
    "uniform float uTime;\n"
    "uniform float uFocus;\n"
    "void main() {\n"
    // Position within the element, 0..1 -- uRect is the element's own
    // rectangle in points, which is what makes this independent of where the
    // layout happened to put it.
    "    vec2 p = (vPos - uRect.xy) / max(uRect.zw, vec2(1.0));\n"
    // A slow diagonal drift, so uTime being frozen would be obvious rather
    // than subtle: this is the uniform that was declared, uploaded and never
    // assigned until the audits found it.
    "    float w = sin((p.x + p.y) * 6.0 + uTime * 0.8) * 0.5 + 0.5;\n"
    "    float vignette = 1.0 - length(p - 0.5) * 0.9;\n"
    "    vec3 top = vec3(0.05, 0.06, 0.11);\n"
    "    vec3 bottom = vec3(0.10, 0.07, 0.16);\n"
    "    vec3 c = mix(top, bottom, p.y) + w * 0.02;\n"
    "    FragColor = vec4(c * clamp(vignette, 0.0, 1.0), 0.88 * vColor.a);\n"
    "}\n";

#endif // _GAMETEST_UI_BACKDROP_H_
