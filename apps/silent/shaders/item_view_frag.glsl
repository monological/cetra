#version 330 core

// The backpack's turntable (spec 13.40): an item lit as a photograph of it would be, by a warm
// key over the viewer's left shoulder, a cool fill from the right and a rim from behind, over a
// studio's soft ambient, and written display-encoded, since the UI shows it as it is. Its
// material's albedo, map and vertex colour (the kit's grime), roughness and metalness; no
// normal map.

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

const vec3 KEY_DIR = vec3(-0.45, 0.75, 0.5);
const vec3 KEY = vec3(3.3, 3.0, 2.6);
const vec3 FILL_DIR = vec3(0.8, 0.15, 0.35);
const vec3 FILL = vec3(0.45, 0.55, 0.7);
const vec3 RIM_DIR = vec3(0.1, 0.45, -1.0);
const vec3 RIM = vec3(1.9, 1.9, 2.0);
const vec3 SKY = vec3(0.32, 0.33, 0.36); // the ambient from above, and from below
const vec3 GROUND = vec3(0.08, 0.075, 0.07);

// Blinn-Phong from a roughness: normalised so a rough surface spreads what a smooth one focuses.
vec3 lightFrom(vec3 dir, vec3 colour, vec3 n, vec3 v, vec3 diffuse, vec3 f0, float shine)
{
    vec3 l = normalize(dir);
    float ndl = max(dot(n, l), 0.0);
    vec3 h = normalize(l + v);
    float spec = (shine + 8.0) / 25.13 * pow(max(dot(n, h), 0.0), shine);
    vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - max(dot(h, v), 0.0), 5.0);
    return colour * ndl * (diffuse + fresnel * spec);
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

    vec3 c = lightFrom(KEY_DIR, KEY, n, v, diffuse, f0, shine);
    c += lightFrom(FILL_DIR, FILL, n, v, diffuse, f0, shine);
    c += lightFrom(RIM_DIR, RIM, n, v, diffuse, f0, shine);
    // The ambient: diffuse from the hemisphere the normal faces, and the same sky in what the
    // surface reflects, dulled as it roughens.
    vec3 ambient = mix(GROUND, SKY, 0.5 + 0.5 * n.y);
    vec3 reflected = mix(GROUND, SKY, 0.5 + 0.5 * reflect(-v, n).y);
    c += diffuse * ambient + f0 * reflected * (1.0 - 0.6 * r);

    // A soft shoulder to white, then the display's curve.
    c = vec3(1.0) - exp(-1.2 * c);
    FragColor = vec4(pow(c, vec3(1.0 / 2.2)), 1.0);
}
