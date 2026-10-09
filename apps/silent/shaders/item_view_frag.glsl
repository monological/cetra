#version 330 core

// The backpack's turntable (spec 13.40): an item lit as in a display case lit from its floor, by a
// warm key from below on the right and behind it, a cool fill from the left and behind, and a
// light from below in front, over an ambient pale from beneath, and written display-encoded, since
// the UI shows it as it is. Its material's albedo, map and vertex colour (the kit's grime),
// roughness and metalness; no normal map.

#include "fresnel.glsl"
#include "display.glsl"

in vec3 vWorld;
in vec3 vNormal;
in vec2 vUv;
in vec4 vColor;
out vec4 FragColor;

uniform sampler2D albedoTex;
uniform bool hasAlbedoTex;
uniform vec3 albedo;
uniform float roughness;
uniform float metallic;
uniform vec3 eye;

// Toward each light from the item; +y is up the picture and +z toward the eye.
const vec3 KEY_DIR = vec3(0.45, -0.75, -0.5);
const vec3 KEY = vec3(3.3, 3.0, 2.6);
const vec3 FILL_DIR = vec3(-0.8, -0.15, -0.35);
const vec3 FILL = vec3(0.45, 0.55, 0.7);
const vec3 FRONT_DIR = vec3(-0.1, -0.45, 1.0);
const vec3 FRONT = vec3(1.9, 1.9, 2.0);
// The ambient's two sides: pale from beneath and dark from above on what is lit, the other way
// up in what the metal reflects.
const vec3 PALE = vec3(0.32, 0.33, 0.36);
const vec3 DARK = vec3(0.08, 0.075, 0.07);

// Blinn-Phong from a roughness: normalised so a rough surface spreads what a smooth one focuses.
// `seen` is the direction the shine is seen from.
vec3 lightFrom(vec3 dir, vec3 colour, vec3 n, vec3 seen, vec3 diffuse, vec3 f0, float shine)
{
    vec3 l = normalize(dir);
    float ndl = max(dot(n, l), 0.0);
    vec3 h = normalize(l + seen);
    float spec = (shine + 8.0) / 25.13 * pow(max(dot(n, h), 0.0), shine);
    return colour * ndl * (diffuse + fresnelSchlick(max(dot(h, seen), 0.0), f0) * spec);
}

void main()
{
    vec3 base = albedo * vColor.rgb;
    if (hasAlbedoTex)
        base *= texture(albedoTex, vUv).rgb;
    vec3 n = normalize(vNormal);
    if (!gl_FrontFacing)
        n = -n;
    vec3 v = normalize(eye - vWorld);
    vec3 diffuse = base * (1.0 - metallic);
    vec3 f0 = mix(vec3(0.04), base, metallic);
    float r = clamp(roughness, 0.05, 1.0);
    float shine = clamp(2.0 / (r * r * r * r) - 2.0, 1.0, 2048.0);

    // The shine is seen from the eye's mirror image through the item rather than from the eye,
    // which puts the bare metal's highlights where the look chosen has them (spec 13.40).
    vec3 c = lightFrom(KEY_DIR, KEY, n, -v, diffuse, f0, shine);
    c += lightFrom(FILL_DIR, FILL, n, -v, diffuse, f0, shine);
    c += lightFrom(FRONT_DIR, FRONT, n, -v, diffuse, f0, shine);
    // The ambient: diffuse from the side the normal faces, and in what the surface reflects,
    // dulled as it roughens.
    vec3 ambient = mix(DARK, PALE, 0.5 - 0.5 * n.y);
    vec3 reflected = mix(DARK, PALE, 0.5 + 0.5 * reflect(-v, n).y);
    c += diffuse * ambient + f0 * reflected * (1.0 - 0.6 * r);

    // A soft shoulder to white, then the display's curve.
    c = vec3(1.0) - exp(-1.2 * c);
    FragColor = vec4(displayEncode(c), 1.0);
}
